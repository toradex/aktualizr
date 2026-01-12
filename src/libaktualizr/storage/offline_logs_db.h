#ifndef OFFLINE_LOGS_DB_H_
#define OFFLINE_LOGS_DB_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boost/filesystem/path.hpp>

#include "sql_utils.h"

/**
 * Type-safe wrapper for install IDs that prevents accidental misuse of raw integers.
 */
class InstallId {
 public:
  // Default constructor creates an invalid InstallId
  InstallId() : value_(-1) {}

  // Factory method for valid IDs (from database)
  static InstallId FromDb(int64_t id) { return InstallId(id); }

  // Check validity
  bool IsValid() const { return value_ >= 0; }

  // Access raw value (for database operations)
  int64_t Value() const { return value_; }

  // Comparison operators
  bool operator==(const InstallId& other) const { return value_ == other.value_; }
  bool operator!=(const InstallId& other) const { return value_ != other.value_; }

  // Allow use in boolean contexts
  explicit operator bool() const { return IsValid(); }

 private:
  explicit InstallId(int64_t value) : value_(value) {}
  int64_t value_;
};

/**
 * SQLite database wrapper for offline update logs.
 *
 * This database is stored on the offline update media (e.g., USB drive)
 * and captures logs, reports, and manifests during offline updates.
 *
 * The database is opened in the constructor. Use Ok() to check if the
 * database opened correctly and hasn't encountered any errors.
 */
class OfflineLogsDb {
 public:
  /**
   * Constructor - opens or creates the database at the given path.
   * Check Ok() to verify the database opened correctly.
   */
  explicit OfflineLogsDb(const boost::filesystem::path& db_path);

  ~OfflineLogsDb() = default;

  // Non-copyable, non-movable (holds database connection)
  OfflineLogsDb(const OfflineLogsDb&) = delete;
  OfflineLogsDb& operator=(const OfflineLogsDb&) = delete;
  OfflineLogsDb(OfflineLogsDb&&) = delete;
  OfflineLogsDb& operator=(OfflineLogsDb&&) = delete;

  /**
   * Check if the database opened correctly and hasn't encountered errors.
   * @return true if the database is usable, false otherwise
   */
  bool Ok() const { return db_.has_value(); }

  /**
   * Create a new install record.
   * @param device_id The device ID performing the installation
   * @param name The name of the offline update
   * @param version The version number from the update metadata
   * @return A valid InstallId on success, invalid on failure
   */
  InstallId CreateInstall(std::string_view device_id, std::string_view name, int version);

  /**
   * Find an in-progress install for a device (one with null manifest).
   * @param device_id The device ID to search for
   * @return A valid InstallId if found, invalid otherwise
   */
  InstallId FindInProgressInstall(std::string_view device_id);

  /**
   * Mark an install as complete with the final manifest.
   * @param install_id The install to complete
   * @param report_counter The report counter value from the device
   * @param manifest The signed manifest JSON
   */
  void CompleteInstall(InstallId install_id, int64_t report_counter, std::string_view manifest);

  /**
   * Add a log entry for an install.
   * @param install_id The install this log belongs to
   * @param timestamp_us Timestamp in microseconds since Unix epoch
   * @param service The systemd service name
   * @param message The log message
   */
  void AddLogEntry(InstallId install_id, int64_t timestamp_us, std::string_view service, std::string_view message);

  /**
   * Add a report event for an install.
   * Reports are deduplicated by report_id - adding a duplicate is a no-op.
   * @param install_id The install this report belongs to
   * @param report_id The unique report ID (UUID)
   * @param timestamp_us Timestamp in microseconds since Unix epoch
   * @param type The event type (e.g., "EcuDownloadStarted")
   * @param version The event version number
   * @param event_json The JSON serialization of the event
   */
  void AddReport(InstallId install_id, std::string_view report_id, int64_t timestamp_us, std::string_view type,
                 int version, std::string_view event_json);

  /**
   * Get the path to the database file.
   */
  boost::filesystem::path dbPath() const { return db_path_; }

 private:
  /**
   * Initialize the database schema.
   * @return true on success, false on failure
   */
  bool InitializeSchema();

  boost::filesystem::path db_path_;
  std::optional<SQLite3Guard> db_;
};

#endif  // OFFLINE_LOGS_DB_H_
