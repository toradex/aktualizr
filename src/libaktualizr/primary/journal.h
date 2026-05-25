#ifndef PRIMARY_JOURNAL_H_
#define PRIMARY_JOURNAL_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

/**
 * Abstract interface over a systemd journal cursor.
 *
 * This exposes the stateful journal cursor so callers can iterate entries
 * across multiple calls (e.g. batched uploads).
 *
 * The concrete production implementation is SystemdJournal (which wraps
 * sd_journal*). Tests use TestJournal to drive the log-capture logic without
 * a real systemd journal.
 *
 * Instances are created by cloning a prototype via Clone(); see the
 * dependency-injection wiring in SotaUptaneClient. This replaces direct use
 * of a concrete constructor so that the journal source can be substituted in
 * tests.
 */
class JournalHandle {
 public:
  JournalHandle() = default;
  virtual ~JournalHandle() = default;

  // Non-copyable, non-movable through the interface; use Clone() to obtain a
  // fresh handle from a prototype.
  JournalHandle(const JournalHandle&) = delete;
  JournalHandle& operator=(const JournalHandle&) = delete;
  JournalHandle(JournalHandle&&) = delete;
  JournalHandle& operator=(JournalHandle&&) = delete;

  /**
   * Create a fresh handle from this prototype.
   *
   * The clone starts a new, independent journal session positioned at the
   * start of the journal. This replaces use of a concrete constructor.
   */
  virtual std::unique_ptr<JournalHandle> Clone() const = 0;

  /** True if the journal was opened successfully. */
  virtual bool IsOpen() const = 0;
  explicit operator bool() const { return IsOpen(); }

  /**
   * Advance to the next journal entry.
   * @return >0 if positioned on a valid entry, 0 at end, <0 on error.
   */
  virtual int Next() = 0;

  /**
   * Extract a string field from the current journal entry.
   * @return The field value, or empty string if not found.
   */
  virtual std::string GetField(const char* field) = 0;

  /**
   * Get the realtime timestamp of the current journal entry.
   * @return Microseconds since epoch, or 0 on error.
   */
  virtual int64_t GetTimestamp() = 0;

  /**
   * Seek to the tail of the journal, positioned on the last entry. This
   * succeeds even when the journal is currently empty; the handle is then
   * placed before any future entries, so the next Next() call will return
   * entries that arrive after this point.
   * @return true on success, false on error.
   */
  virtual bool MoveTail() = 0;

  /**
   * Get a representation of this handle's current journal position.
   * @return A valid cursor on success, empty string on failure.
   */
  virtual std::string GetCurrentCursor() = 0;
};

/**
 * Filters journal entries by systemd unit name.
 *
 * Constructed from a list of service names (with or without ".service"
 * suffix) and provides a ShouldCapture() predicate for use during
 * journal iteration.
 */
class JournalFilter {
 public:
  explicit JournalFilter(const std::vector<std::string>& services) {
    for (const auto& svc : services) {
      // Ensure service name has .service suffix
      if (svc.size() >= 8 && svc.substr(svc.size() - 8) == ".service") {
        services_.insert(svc);
      } else {
        services_.insert(svc + ".service");
      }
    }
  }

  /** Returns true if the current journal entry's _SYSTEMD_UNIT is in the filter set. */
  bool ShouldCapture(JournalHandle& journal) const {
    std::string unit = journal.GetField("_SYSTEMD_UNIT");
    return !unit.empty() && services_.count(unit) > 0;
  }

 private:
  std::unordered_set<std::string> services_;
};

#endif  // PRIMARY_JOURNAL_H_
