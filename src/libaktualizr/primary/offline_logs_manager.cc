#include "primary/offline_logs_manager.h"

#ifdef BUILD_OFFLINE_UPDATES

#include <string_view>

#include <boost/filesystem.hpp>

#include "logging/logging.h"
#include "storage/invstorage.h"

namespace fs = boost::filesystem;

OfflineLogsManager::OfflineLogsManager(const Config& config)
    : enabled_(config.logger.offline_logs_enabled),
      logs_filename_(config.logger.offline_logs_file),
      capture_services_(config.logger.offline_capture_services) {
  if (!enabled_) {
    LOG_DEBUG << "OfflineLogsManager: disabled via configuration";
  }
}

fs::path OfflineLogsManager::ResolveLogsDbPath(const fs::path& offline_update_path) const {
  fs::path logs_path(logs_filename_);
  if (logs_path.is_absolute()) {
    return logs_path;
  }
  return offline_update_path / logs_filename_;
}

InstallId OfflineLogsManager::BeginInstall(const fs::path& offline_update_path, std::string_view device_id,
                                           std::string_view update_name, int update_version) {
  if (!enabled_) {
    LOG_DEBUG << "OfflineLogsManager::BeginInstall: disabled, returning invalid InstallId";
    return InstallId();
  }

  fs::path db_path = ResolveLogsDbPath(offline_update_path);
  LOG_INFO << "OfflineLogsManager::BeginInstall: opening database at " << db_path;

  db_ = std::make_unique<OfflineLogsDb>(db_path);
  if (!db_->Ok()) {
    LOG_WARNING << "OfflineLogsManager::BeginInstall: failed to open database at " << db_path;
    db_.reset();
    return InstallId();
  }

  current_install_id_ = db_->CreateInstall(device_id, update_name, update_version);
  if (!current_install_id_.IsValid()) {
    LOG_WARNING << "OfflineLogsManager::BeginInstall: failed to create install record";
    db_.reset();
    return InstallId();
  }

  LOG_INFO << "OfflineLogsManager::BeginInstall: created install " << current_install_id_.Value() << " for device "
           << device_id << ", update '" << update_name << "' v" << update_version;

  // TODO: Phase 2 - capture initial journal cursor here
  // journal_cursor_ = JournalCopier::GetCurrentCursor();

  return current_install_id_;
}

InstallId OfflineLogsManager::FindAndResumeInstall(const fs::path& offline_update_path, std::string_view device_id) {
  if (!enabled_) {
    LOG_DEBUG << "OfflineLogsManager::FindAndResumeInstall: disabled, returning invalid InstallId";
    return InstallId();
  }

  fs::path db_path = ResolveLogsDbPath(offline_update_path);
  LOG_INFO << "OfflineLogsManager::FindAndResumeInstall: checking for database at " << db_path;

  if (!fs::exists(db_path)) {
    LOG_DEBUG << "OfflineLogsManager::FindAndResumeInstall: no database found at " << db_path;
    return InstallId();
  }

  db_ = std::make_unique<OfflineLogsDb>(db_path);
  if (!db_->Ok()) {
    LOG_WARNING << "OfflineLogsManager::FindAndResumeInstall: failed to open database at " << db_path;
    db_.reset();
    return InstallId();
  }

  current_install_id_ = db_->FindInProgressInstall(device_id);
  if (!current_install_id_.IsValid()) {
    LOG_DEBUG << "OfflineLogsManager::FindAndResumeInstall: no in-progress install found for device " << device_id;
    db_.reset();
    return InstallId();
  }

  LOG_INFO << "OfflineLogsManager::FindAndResumeInstall: resumed install " << current_install_id_.Value()
           << " for device " << device_id;

  return current_install_id_;
}

void OfflineLogsManager::CaptureLogs() {
  if (!enabled_ || !current_install_id_.IsValid() || !db_) {
    return;
  }

  // TODO: Phase 2 - implement JournalCopier integration
  LOG_INFO << "OfflineLogsManager::CaptureLogs: would capture journal logs for install " << current_install_id_.Value()
           << " (stub - Phase 2 not implemented)";
}

void OfflineLogsManager::CaptureReports(INvStorage& storage) {
  if (!enabled_ || !current_install_id_.IsValid() || !db_) {
    return;
  }

  // TODO: Phase 2 - implement ReportCopier integration
  (void)storage;  // Suppress unused parameter warning
  LOG_INFO << "OfflineLogsManager::CaptureReports: would capture report events for install "
           << current_install_id_.Value() << " (stub - Phase 2 not implemented)";
}

void OfflineLogsManager::CompleteInstall(int64_t report_counter, const Uptane::Manifest& manifest) {
  if (!enabled_ || !current_install_id_.IsValid() || !db_) {
    LOG_DEBUG << "OfflineLogsManager::CompleteInstall: no active install to complete";
    return;
  }

  // Serialize manifest to JSON string
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  std::string manifest_str = Json::writeString(writer, manifest);

  LOG_INFO << "OfflineLogsManager::CompleteInstall: completing install " << current_install_id_.Value()
           << " with report_counter=" << report_counter;

  db_->CompleteInstall(current_install_id_, report_counter, manifest_str);

  // Final log capture before closing
  CaptureLogs();

  // Reset state
  current_install_id_ = InstallId();
  db_.reset();

  LOG_INFO << "OfflineLogsManager::CompleteInstall: install completed and database closed";
}

#endif  // BUILD_OFFLINE_UPDATES
