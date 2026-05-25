#include "primary/offline_logs_manager.h"

#ifdef BUILD_OFFLINE_UPDATES

#include <string_view>

#include <boost/filesystem.hpp>

#include "logging/logging.h"
#include "storage/invstorage.h"

namespace fs = boost::filesystem;

OfflineLogsManager::OfflineLogsManager(const Config& config, std::shared_ptr<JournalHandle> journal_prototype)
    : enabled_(config.logger.offline_logs_enabled),
      logs_filename_(config.logger.offline_logs_file),
      capture_services_(config.logger.capture_services),
      journal_prototype_(std::move(journal_prototype)) {
  if (!enabled_) {
    LOG_DEBUG << "OfflineLogsManager: disabled via configuration";
  }
}

fs::path OfflineLogsManager::ResolveLogsDbPath(const fs::path& offline_update_path) const {
  fs::path logs_path(logs_filename_);
  if (logs_path.is_absolute()) {
    return logs_path;
  }

  // Canonicalize the offline_update_path to resolve any symlinks in the path hierarchy.
  // This allows system-level symlinks (e.g., /mnt -> /var/mnt/automount) while still
  // protecting against symlink attacks on the removable media itself
  // (via SQLITE_OPEN_NOFOLLOW on the final db file in OfflineLogsDb).
  // The offline_update_path is trusted input, as is the logs_filename_ from configuration.
  boost::system::error_code ec;
  fs::path canonical_update_path = fs::canonical(offline_update_path, ec);
  if (ec) {
    LOG_WARNING << "Failed to canonicalize offline update path: " << ec.message();
    // Fall back to original path if canonicalization fails
    return offline_update_path / logs_filename_;
  }
  return canonical_update_path / logs_filename_;
}

InstallId OfflineLogsManager::BeginInstall(const fs::path& offline_update_path, std::string_view device_id,
                                           std::string_view update_name, int update_version) {
  if (!enabled_) {
    LOG_DEBUG << "OfflineLogsManager::BeginInstall: disabled, returning invalid InstallId";
    return InstallId();
  }

  fs::path db_path = ResolveLogsDbPath(offline_update_path);
  LOG_INFO << "OfflineLogsManager::BeginInstall: opening database at " << db_path;

  db_.emplace(db_path);
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

  // Open a journal handle for the lifetime of this install and seek to tail.
  journal_handle_ = journal_prototype_ ? journal_prototype_->Clone() : nullptr;
  if (!journal_handle_ || !*journal_handle_ || !journal_handle_->MoveTail()) {
    LOG_WARNING
        << "OfflineLogsManager::BeginInstall: failed to open journal or seek to tail (journal logging disabled)";
    journal_handle_.reset();
  }

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

  db_.emplace(db_path);
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

  // Open a journal handle for the lifetime of this install and seek to tail.
  // Note: We capture from now, not from the pre-reboot cursor, as we don't
  // persist cursors across reboots.
  journal_handle_ = journal_prototype_ ? journal_prototype_->Clone() : nullptr;
  if (!journal_handle_ || !*journal_handle_ || !journal_handle_->MoveTail()) {
    LOG_WARNING << "OfflineLogsManager::FindAndResumeInstall: failed to open journal or seek to tail (journal logging "
                   "disabled)";
    journal_handle_.reset();
  }

  return current_install_id_;
}

void OfflineLogsManager::CaptureLogs() {
  if (!enabled_ || !current_install_id_.IsValid() || !db_) {
    return;
  }

  if (!journal_handle_ || !*journal_handle_) {
    LOG_DEBUG << "OfflineLogsManager::CaptureLogs: no valid journal handle, skipping journal capture";
    return;
  }

  int ret = 0;

  size_t entries_copied = 0;
  // Note: <0 means 'error'  0 means ' end' and >0 means 'made progress'
  do {
    ret = journal_handle_->Next();
    if (ret < 0) {
      LOG_WARNING << "OfflineLogsManager::CaptureLogs: failed to advance past cursor: " << strerror(-ret);
    } else if (ret > 0 && capture_services_.ShouldCapture(*journal_handle_)) {
      // Get message, timestamp, and unit
      std::string message = journal_handle_->GetField("MESSAGE");
      int64_t timestamp_us = journal_handle_->GetTimestamp();

      if (!message.empty() && timestamp_us > 0) {
        // Remove .service suffix for cleaner display
        std::string service_name = journal_handle_->GetField("_SYSTEMD_UNIT");
        if (service_name.size() >= 8 && service_name.substr(service_name.size() - 8) == ".service") {
          service_name = service_name.substr(0, service_name.size() - 8);
        }

        db_->AddLogEntry(current_install_id_, timestamp_us, service_name, message);
        ++entries_copied;
      }
    }
  } while (ret > 0);

  LOG_INFO << "OfflineLogsManager::CaptureLogs: captured " << entries_copied << " journal entries for install "
           << current_install_id_.Value();
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
  journal_handle_.reset();
  db_.reset();

  LOG_INFO << "OfflineLogsManager::CompleteInstall: install completed and database closed";
}

#endif  // BUILD_OFFLINE_UPDATES
