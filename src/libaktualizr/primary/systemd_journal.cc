#include "primary/systemd_journal.h"

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

#include <cstring>

// -- SystemdJournal implementation --

SystemdJournal::SystemdJournal() {
  int ret = sd_journal_open(&journal_, SD_JOURNAL_LOCAL_ONLY);
  if (ret < 0) {
    LOG_WARNING << "Failed to open journal: " << strerror(-ret);
    journal_ = nullptr;
  }
}

SystemdJournal::~SystemdJournal() {
  if (journal_ != nullptr) {
    sd_journal_close(journal_);
  }
}

int SystemdJournal::Next() { return sd_journal_next(journal_); }

std::string SystemdJournal::GetField(const char* field) {
  const void* data = nullptr;
  size_t length = 0;

  int ret = sd_journal_get_data(journal_, field, &data, &length);
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

int64_t SystemdJournal::GetTimestamp() {
  uint64_t usec = 0;
  int ret = sd_journal_get_realtime_usec(journal_, &usec);
  if (ret < 0) {
    return 0;
  }
  return static_cast<int64_t>(usec);
}

bool SystemdJournal::MoveTail() {
  if (journal_ == nullptr) {
    return false;
  }

  int ret = sd_journal_seek_tail(journal_);
  if (ret < 0) {
    LOG_WARNING << "Failed to seek to journal tail: " << strerror(-ret);
    return false;
  }

  // seek_tail positions *after* the last entry; move back one to be on it
  ret = sd_journal_previous(journal_);
  if (ret < 0) {
    LOG_WARNING << "Failed to move to previous journal entry: " << strerror(-ret);
    return false;
  }
  if (ret == 0) {
    LOG_DEBUG << "Journal is empty";
    return false;
  }
  return true;
}

std::string SystemdJournal::GetCurrentCursor() {
  if (journal_ == nullptr) {
    return {};
  }

  char* cursor_str = nullptr;
  int ret = sd_journal_get_cursor(journal_, &cursor_str);
  if (ret < 0) {
    return {};
  }

  std::string cursor(cursor_str);
  free(cursor_str);  // NOLINT(cppcoreguidelines-no-malloc, hicpp-no-malloc)
  return cursor;
}
