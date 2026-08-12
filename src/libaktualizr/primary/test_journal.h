#ifndef PRIMARY_TEST_JOURNAL_H_
#define PRIMARY_TEST_JOURNAL_H_

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "primary/journal.h"

/**
 * A single fake journal entry used by TestJournal.
 */
struct TestJournalEntry {
  std::string unit;     ///< value of the _SYSTEMD_UNIT field, e.g. "aktualizr.service"
  std::string message;  ///< value of the MESSAGE field
  int64_t timestamp;    ///< microseconds since epoch
  std::string cursor;   ///< opaque cursor string for this entry
};

/**
 * In-memory JournalHandle implementation used to drive tests of
 * OnlineLogsUploader / OfflineLogsManager without a real systemd journal.
 *
 * Entries are held in a shared store so that clones produced by Clone()
 * observe entries added to the prototype, even after Clone() has been called.
 * This mirrors the production behaviour where the uploader thread reads
 * journal entries that arrive after streaming starts.
 *
 * All access is mutex-protected because the uploader reads from a background
 * thread while the test adds entries from the main thread.
 */
class TestJournal : public JournalHandle {
 public:
  /// Shared, thread-safe backing store shared between a prototype and its clones.
  struct Shared {
    std::mutex mutex;
    std::vector<TestJournalEntry> entries;
    bool is_open{true};
  };

  TestJournal() : shared_(std::make_shared<Shared>()) {}
  explicit TestJournal(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

  std::unique_ptr<JournalHandle> Clone() const override {
    return std::unique_ptr<JournalHandle>(new TestJournal(shared_));
  }

  bool IsOpen() const override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->is_open;
  }

  int Next() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    // Non-blocking, matching sd_journal_next(): returns 0 immediately when
    // there is no further entry, leaving any "wait for new entries" to the
    // caller (the uploader thread's outer loop sleeps for kBatchInterval
    // between non-full batches).
    if (!shared_->is_open) {
      return 0;
    }
    if (position_ + 1 < static_cast<int>(shared_->entries.size())) {
      ++position_;
      return 1;
    }
    return 0;
  }

  std::string GetField(const char* field) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (position_ < 0 || position_ >= static_cast<int>(shared_->entries.size())) {
      return "";
    }
    const auto& entry = shared_->entries[static_cast<size_t>(position_)];
    if (std::strcmp(field, "_SYSTEMD_UNIT") == 0) {
      return entry.unit;
    }
    if (std::strcmp(field, "MESSAGE") == 0) {
      return entry.message;
    }
    return "";
  }

  int64_t GetTimestamp() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (position_ < 0 || position_ >= static_cast<int>(shared_->entries.size())) {
      return 0;
    }
    return shared_->entries[static_cast<size_t>(position_)].timestamp;
  }

  bool MoveTail() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    // Position this handle just before the first entry that may be added in
    // the future, so the subsequent Next() call returns any entries that
    // arrive after this point — mirroring sd_journal_seek_tail()'s contract.
    position_ = static_cast<int>(shared_->entries.size()) - 1;
    return true;
  }

  std::string GetCurrentCursor() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (position_ < 0 || position_ >= static_cast<int>(shared_->entries.size())) {
      return "";
    }
    return shared_->entries[static_cast<size_t>(position_)].cursor;
  }

  // -- test helpers ----------------------------------------------------------

  /// Append an entry that subsequently-issued Next() calls will return.
  void AddEntry(const std::string& unit, const std::string& message, int64_t timestamp, const std::string& cursor) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->entries.push_back(TestJournalEntry{unit, message, timestamp, cursor});
  }

  /// Simulate a journal that cannot be opened.
  void SetOpen(bool is_open) {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->is_open = is_open;
  }

  /// Access the shared backing store (e.g. to add entries after Clone()).
  const std::shared_ptr<Shared>& shared() const { return shared_; }

 private:
  std::shared_ptr<Shared> shared_;
  int position_{-1};
};

#endif  // PRIMARY_TEST_JOURNAL_H_
