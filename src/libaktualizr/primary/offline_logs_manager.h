#ifndef PRIMARY_OFFLINE_LOGS_MANAGER_H_
#define PRIMARY_OFFLINE_LOGS_MANAGER_H_

#include <boost/filesystem/path.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "libaktualizr/config.h"
#include "storage/journal_copier.h"
#include "storage/offline_logs_db.h"

class INvStorage;

namespace Uptane {
class Manifest;
}

#ifdef BUILD_OFFLINE_UPDATES

/**
 * High-level manager that coordinates all offline logging operations.
 *
 * This class is owned by SotaUptaneClient because most integration points
 * (finalizeAfterReboot, uptaneInstall, putManifest) are in that class.
 */
class OfflineLogsManager {
 public:
  explicit OfflineLogsManager(const Config& config);

  // Non-copyable, non-movable
  OfflineLogsManager(const OfflineLogsManager&) = delete;
  OfflineLogsManager& operator=(const OfflineLogsManager&) = delete;
  OfflineLogsManager(OfflineLogsManager&&) = delete;
  OfflineLogsManager& operator=(OfflineLogsManager&&) = delete;

  ~OfflineLogsManager() = default;

  /**
   * Called at start of offline update (from fetchMetaOffUpd).
   * Opens the offline logs database and creates an install record.
   *
   * @param offline_update_path Root path of the offline update media
   * @param device_id The device ID for this install
   * @param update_name Name/description of the update
   * @param update_version Version number of the update
   * @return Valid InstallId if db opened successfully, invalid otherwise
   */
  InstallId BeginInstall(const boost::filesystem::path& offline_update_path, std::string_view device_id,
                         std::string_view update_name, int update_version);

  /**
   * Called after reboot to find in-progress install (from finalizeAfterReboot).
   * Reopens the offline logs database and finds any incomplete install.
   *
   * @param offline_update_path Root path of the offline update media
   * @param device_id The device ID to search for
   * @return Valid InstallId if found, invalid otherwise
   */
  InstallId FindAndResumeInstall(const boost::filesystem::path& offline_update_path, std::string_view device_id);

  /**
   * Capture logs from journal since last capture.
   * Copies journal entries from configured services to the offline logs database.
   * Updates the internal cursor position after each capture.
   */
  void CaptureLogs();

  /**
   * Capture unsent report events from main storage.
   * Currently a stub that logs a message (Phase 2 will implement ReportCopier).
   *
   * @param storage The main Aktualizr storage to read reports from
   */
  void CaptureReports(INvStorage& storage);

  /**
   * Called when install completes (from putManifestSimple).
   * Records the final manifest and marks the install as complete.
   *
   * @param report_counter The report counter value at completion
   * @param manifest The signed manifest that was sent
   */
  void CompleteInstall(int64_t report_counter, const Uptane::Manifest& manifest);

  /**
   * Check if we have an active install in progress.
   */
  bool HasActiveInstall() const { return current_install_id_.IsValid(); }

  /**
   * Check if offline logging is enabled via configuration.
   */
  bool IsEnabled() const { return enabled_; }

 private:
  /**
   * Resolve the logs database path from config and update path.
   * If logs_filename_ is absolute, use it directly.
   * Otherwise, resolve relative to offline_update_path.
   */
  boost::filesystem::path ResolveLogsDbPath(const boost::filesystem::path& offline_update_path) const;

  bool enabled_;
  std::string logs_filename_;
  std::vector<std::string> capture_services_;
  std::unique_ptr<OfflineLogsDb> db_;
  JournalCopier::Cursor journal_cursor_;
  InstallId current_install_id_;
};

#else  // !BUILD_OFFLINE_UPDATES

/**
 * Stub implementation of OfflineLogsManager when BUILD_OFFLINE_UPDATES is disabled.
 * All methods are no-ops.
 */
class OfflineLogsManager {
 public:
  explicit OfflineLogsManager(const Config& /* config */) {}

  OfflineLogsManager(const OfflineLogsManager&) = delete;
  OfflineLogsManager& operator=(const OfflineLogsManager&) = delete;
  OfflineLogsManager(OfflineLogsManager&&) = delete;
  OfflineLogsManager& operator=(OfflineLogsManager&&) = delete;

  ~OfflineLogsManager() = default;

  // All methods are no-ops in stub implementation
  // Note: Return type doesn't exist when BUILD_OFFLINE_UPDATES is off, so we use int as a placeholder
  // These methods won't actually be called when offline updates are disabled
  static InstallId BeginInstall(const boost::filesystem::path& /* offline_update_path */,
                                std::string_view /* device_id */, std::string_view /* update_name */,
                                int /* update_version */) {
    return {};
  }

  static InstallId FindAndResumeInstall(const boost::filesystem::path& /* offline_update_path */,
                                        std::string_view /* device_id */) {
    return {};
  }

  void CaptureLogs() {}
  void CaptureReports(INvStorage& /* storage */) {}
  void CompleteInstall(int64_t /* report_counter */, const Uptane::Manifest& /* manifest */) {}

  static bool HasActiveInstall() { return false; }
  static bool IsEnabled() { return false; }
};

#endif  // BUILD_OFFLINE_UPDATES

#endif  // PRIMARY_OFFLINE_LOGS_MANAGER_H_
