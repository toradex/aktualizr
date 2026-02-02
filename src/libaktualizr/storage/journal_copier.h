#ifndef JOURNAL_COPIER_H_
#define JOURNAL_COPIER_H_

#include <string>
#include <vector>

#include "offline_logs_db.h"

/**
 * Copies entries from systemd journal directly to OfflineLogsDb,
 * filtering by configured services.
 *
 * This class provides cursor-based journal access for capturing
 * logs during offline updates. The cursor allows resuming log
 * capture after system reboots.
 */
class JournalCopier {
 public:
  /**
   * Opaque cursor type - represents a position in the journal.
   * Default-constructed Cursor is invalid.
   */
  class Cursor {
   public:
    Cursor() = default;

    /**
     * Check if the cursor is valid (points to a real journal position).
     */
    bool IsValid() const { return !cursor_.empty(); }

    /**
     * Allow use in boolean contexts.
     */
    explicit operator bool() const { return IsValid(); }

    /**
     * Get the raw cursor string (for serialization/debugging).
     */
    const std::string& ToString() const { return cursor_; }

    /**
     * Create a cursor from a serialized string.
     */
    static Cursor FromString(const std::string& cursor_str) { return Cursor(cursor_str); }

   private:
    friend class JournalCopier;
    explicit Cursor(std::string cursor) : cursor_(std::move(cursor)) {}
    std::string cursor_;
  };

  /**
   * Get the current journal position as a cursor.
   * Call this before starting the update to mark the starting point.
   *
   * @return A valid cursor on success, invalid cursor on failure
   */
  static Cursor GetCurrentCursor();

  /**
   * Copy all journal entries from cursor position to now.
   * Writes directly to the OfflineLogsDb.
   *
   * @param cursor The starting position (from GetCurrentCursor)
   * @param services List of systemd service names to filter by
   *                 (e.g., {"aktualizr", "docker-compose"})
   *                 Note: ".service" suffix is added automatically if not present
   * @param db The offline logs database to write entries to
   * @param install_id The install ID to associate log entries with
   * @return The number of log entries copied
   */
  static size_t CopyFromCursor(const Cursor& cursor, const std::vector<std::string>& services, OfflineLogsDb& db,
                               InstallId install_id);
};

#endif  // JOURNAL_COPIER_H_
