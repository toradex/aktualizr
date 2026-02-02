#include "journal_copier.h"

// Include our logging first to define our LOG_* macros
#include "logging/logging.h"

// Save our logging macros before systemd headers redefine them
#pragma push_macro("LOG_DEBUG")
#pragma push_macro("LOG_WARNING")
#pragma push_macro("LOG_INFO")
#pragma push_macro("LOG_ERROR")

// Undefine them so systemd's syslog.h doesn't conflict
#undef LOG_DEBUG
#undef LOG_WARNING
#undef LOG_INFO
#undef LOG_ERROR

#include <systemd/sd-journal.h>

// Restore our logging macros
#pragma pop_macro("LOG_ERROR")
#pragma pop_macro("LOG_INFO")
#pragma pop_macro("LOG_WARNING")
#pragma pop_macro("LOG_DEBUG")

#include <algorithm>
#include <cstring>
#include <memory>

namespace {

// RAII wrapper for sd_journal*
class JournalHandle {
 public:
  JournalHandle() = default;
  ~JournalHandle() {
    if (journal_ != nullptr) {
      sd_journal_close(journal_);
    }
  }

  // Non-copyable, non-movable
  JournalHandle(const JournalHandle&) = delete;
  JournalHandle& operator=(const JournalHandle&) = delete;
  JournalHandle(JournalHandle&&) = delete;
  JournalHandle& operator=(JournalHandle&&) = delete;

  sd_journal** ptr() { return &journal_; }
  sd_journal* get() { return journal_; }
  explicit operator bool() const { return journal_ != nullptr; }

 private:
  sd_journal* journal_{nullptr};
};

// Ensure service name has .service suffix
std::string NormalizeServiceName(const std::string& service) {
  if (service.size() >= 8 && service.substr(service.size() - 8) == ".service") {
    return service;
  }
  return service + ".service";
}

// Extract a string field from current journal entry
// Returns empty string if field not found
std::string GetJournalField(sd_journal* journal, const char* field) {
  const void* data = nullptr;
  size_t length = 0;

  int ret = sd_journal_get_data(journal, field, &data, &length);
  if (ret < 0) {
    return "";
  }

  // Data format is "FIELD=value"
  const char* str = static_cast<const char*>(data);
  const char* eq = static_cast<const char*>(std::memchr(data, '=', length));
  if (eq == nullptr) {
    return "";
  }

  size_t value_offset = static_cast<size_t>(eq - str) + 1;
  if (value_offset >= length) {
    return "";
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return std::string(str + value_offset, length - value_offset);
}

// Get timestamp in microseconds from current journal entry
int64_t GetJournalTimestamp(sd_journal* journal) {
  uint64_t usec = 0;
  int ret = sd_journal_get_realtime_usec(journal, &usec);
  if (ret < 0) {
    return 0;
  }
  return static_cast<int64_t>(usec);
}

}  // namespace

JournalCopier::Cursor JournalCopier::GetCurrentCursor() {
  JournalHandle journal;

  // Open journal for reading (SD_JOURNAL_LOCAL_ONLY for local system journal)
  int ret = sd_journal_open(journal.ptr(), SD_JOURNAL_LOCAL_ONLY);
  if (ret < 0) {
    LOG_WARNING << "Failed to open journal: " << strerror(-ret);
    return Cursor();
  }

  // Seek to the end of the journal
  ret = sd_journal_seek_tail(journal.get());
  if (ret < 0) {
    LOG_WARNING << "Failed to seek to journal tail: " << strerror(-ret);
    return Cursor();
  }

  // Move back one entry so we're at the last entry (seek_tail positions after the last entry)
  ret = sd_journal_previous(journal.get());
  if (ret < 0) {
    LOG_WARNING << "Failed to move to previous journal entry: " << strerror(-ret);
    return Cursor();
  }
  if (ret == 0) {
    // Journal is empty - return invalid cursor
    LOG_DEBUG << "Journal is empty";
    return Cursor();
  }

  // Get cursor for current position
  char* cursor_str = nullptr;
  ret = sd_journal_get_cursor(journal.get(), &cursor_str);
  if (ret < 0) {
    LOG_WARNING << "Failed to get journal cursor: " << strerror(-ret);
    return Cursor();
  }

  Cursor cursor(cursor_str);
  free(cursor_str);  // NOLINT(cppcoreguidelines-no-malloc, hicpp-no-malloc)

  LOG_DEBUG << "Got journal cursor: " << cursor.ToString();
  return cursor;
}

size_t JournalCopier::CopyFromCursor(const Cursor& cursor, const std::vector<std::string>& services, OfflineLogsDb& db,
                                     InstallId install_id) {
  if (!cursor.IsValid()) {
    LOG_WARNING << "Invalid cursor provided to CopyFromCursor";
    return 0;
  }

  if (!install_id.IsValid()) {
    LOG_WARNING << "Invalid install_id provided to CopyFromCursor";
    return 0;
  }

  if (!db.Ok()) {
    LOG_WARNING << "Database not ready in CopyFromCursor";
    return 0;
  }

  JournalHandle journal;

  // Open journal for reading
  int ret = sd_journal_open(journal.ptr(), SD_JOURNAL_LOCAL_ONLY);
  if (ret < 0) {
    LOG_WARNING << "Failed to open journal: " << strerror(-ret);
    return 0;
  }

  // Seek to the cursor position
  ret = sd_journal_seek_cursor(journal.get(), cursor.ToString().c_str());
  if (ret < 0) {
    LOG_WARNING << "Failed to seek to cursor: " << strerror(-ret);
    return 0;
  }

  // Verify we're at the cursor position
  ret = sd_journal_test_cursor(journal.get(), cursor.ToString().c_str());
  if (ret < 0) {
    LOG_WARNING << "Cursor is no longer valid (journal may have been rotated): " << strerror(-ret);
    // Continue anyway - we'll just start from wherever we are
  }

  // Build set of normalized service names for efficient lookup
  std::vector<std::string> normalized_services;
  normalized_services.reserve(services.size());
  for (const auto& svc : services) {
    normalized_services.push_back(NormalizeServiceName(svc));
  }

  // Move past the cursor position to start reading new entries
  ret = sd_journal_next(journal.get());
  if (ret < 0) {
    LOG_WARNING << "Failed to advance past cursor: " << strerror(-ret);
    return 0;
  }

  size_t entries_copied = 0;

  // Iterate through journal entries
  while (ret > 0) {
    // Get the systemd unit for this entry
    std::string unit = GetJournalField(journal.get(), "_SYSTEMD_UNIT");

    // Check if this unit is in our list of services to capture
    bool should_capture = false;
    if (!unit.empty()) {
      for (const auto& svc : normalized_services) {
        if (unit == svc) {
          should_capture = true;
          break;
        }
      }
    }

    if (should_capture) {
      // Get message and timestamp
      std::string message = GetJournalField(journal.get(), "MESSAGE");
      int64_t timestamp_us = GetJournalTimestamp(journal.get());

      if (!message.empty() && timestamp_us > 0) {
        // Remove .service suffix for cleaner display
        std::string service_name = unit;
        if (service_name.size() >= 8 && service_name.substr(service_name.size() - 8) == ".service") {
          service_name = service_name.substr(0, service_name.size() - 8);
        }

        db.AddLogEntry(install_id, timestamp_us, service_name, message);
        ++entries_copied;
      }
    }

    // Move to next entry
    ret = sd_journal_next(journal.get());
    if (ret < 0) {
      LOG_WARNING << "Failed to read next journal entry: " << strerror(-ret);
      break;
    }
  }

  LOG_DEBUG << "Copied " << entries_copied << " journal entries";
  return entries_copied;
}
