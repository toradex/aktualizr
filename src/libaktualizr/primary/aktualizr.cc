#include <boost/filesystem.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <memory>

#include <sodium.h>

#include "json/json.h"
#include "libaktualizr/aktualizr.h"
#include "libaktualizr/events.h"
#include "libaktualizr/results.h"
#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "primary/consent.h"
#include "primary/sotauptaneclient.h"
#include "primary/update_lock_file.h"
#include "utilities/apiqueue.h"

#ifdef BUILD_DBUS
#include "primary/dbus.h"
#endif

namespace fs = boost::filesystem;  // NOLINT Used when building for offline updates

Aktualizr::Aktualizr(const Config &config)
    : Aktualizr(config, INvStorage::newStorage(config.storage), std::make_shared<HttpClient>()) {}

Aktualizr::Aktualizr(Config config, std::shared_ptr<INvStorage> storage_in,
                     const std::shared_ptr<HttpInterface> &http_in)
    : config_{std::move(config)},
      sig_{new event::Channel()},
      api_queue_{new api::CommandQueue()},
      update_lock_file_{config_.uptane.update_lock_file} {
  if (sodium_init() == -1) {  // Note that sodium_init doesn't require a matching 'sodium_deinit'
    throw std::runtime_error("Unable to initialize libsodium");
  }

  storage_ = std::move(storage_in);
  storage_->importData(config_.import);

  uptane_client_ = std::make_shared<SotaUptaneClient>(config_, storage_, http_in, sig_, api_queue_->FlowControlToken());
}

Aktualizr::~Aktualizr() { api_queue_.reset(nullptr); }

void Aktualizr::Initialize() {
  bool update_was_in_progress = storage_->hasPendingInstall();
  // bool was_offine = storage_->OfflineLogsPath()
  // Check path exists
  // Grab journal cursor

  uptane_client_->initialize();
  if (update_was_in_progress) {
    // Read if there was a offline update
    LOG_INFO << "Writing logs out..";
    // Write out everything from the cursor above to now
    // Write out manifest from SotaUptaneClient
    //
  }
  api_queue_->run();
}

bool Aktualizr::UptaneCycle() {
  {
    std::lock_guard<std::mutex> const lock{exit_cond_.m};
    if (exit_cond_.run_mode != RunMode::kStop) {
      LOG_WARNING
          << "UptaneCycle() was called in parallel with either UptaneCycle() or RunForever(). This is not supported";
    }
    exit_cond_.run_mode = RunMode::kOnce;
  }
  auto res = RunUpdateLoop();
  // false -> reboot required
  return res != ExitReason::kRebootRequired;
}

std::future<void> Aktualizr::RunForever() {
  {
    std::lock_guard<std::mutex> const lock{exit_cond_.m};
    if (exit_cond_.run_mode != RunMode::kStop) {
      LOG_WARNING
          << "RunForever() was called parallel with either UptaneCycle() or RunForever(). This is not supported";
    }
    exit_cond_.run_mode = RunMode::kUntilRebootNeeded;
  }
  std::future<void> future = std::async(std::launch::async, [this]() { RunUpdateLoop(); });
  return future;
}

std::ostream &operator<<(std::ostream &os, Aktualizr::UpdateCycleState state) {
  switch (state) {
    case Aktualizr::UpdateCycleState::kUnprovisioned:
      os << "Unprovisioned";
      break;
    case Aktualizr::UpdateCycleState::kSendingDeviceData:
      os << "SendingDeviceData";
      break;
    case Aktualizr::UpdateCycleState::kIdle:
      os << "Idle";
      break;
    case Aktualizr::UpdateCycleState::kSendingManifest:
      os << "SendingManifest";
      break;
    case Aktualizr::UpdateCycleState::kCheckingForUpdates:
      os << "CheckingForUpdates";
      break;
    case Aktualizr::UpdateCycleState::kGetConsent:
      os << "GetConsent";
      break;
    case Aktualizr::UpdateCycleState::kConfirmingUpdate:
      os << "ConfirmingUpdate";
      break;
    case Aktualizr::UpdateCycleState::kDownloading:
      os << "Downloading";
      break;
    case Aktualizr::UpdateCycleState::kInstalling:
      os << "Installing";
      break;
#ifdef BUILD_OFFLINE_UPDATES
    case Aktualizr::UpdateCycleState::kCheckingForUpdatesOffline:
      os << "CheckingForUpdatesOffline";
      break;
    case Aktualizr::UpdateCycleState::kFetchingImagesOffline:
      os << "FetchingImagesOffline";
      break;
    case Aktualizr::UpdateCycleState::kInstallingOffline:
      os << "InstallingOffline";
      break;
#endif  // BUILD_OFFLINE_UPDATES
    case Aktualizr::UpdateCycleState::kAwaitReboot:
      os << "AwaitReboot";
      break;
    default:
      os << "Unknown(" << static_cast<int>(state) << ")";
      break;
  }
  return os;
}

Aktualizr::ExitReason Aktualizr::RunUpdateLoop() {
  assert(config_.uptane.polling_sec > 0);  // enforced by Config::postUpdateValues()

  next_online_poll_ = next_offline_poll_ = Clock::now();
  if (!config_.uptane.enable_offline_updates) {
    // We'd like to use Clock::time_point::max() here, but it triggers a bug.
    next_offline_poll_ = Clock::now() + std::chrono::hours(24 * 365 * 10);
  }

  int64_t loops = 0;
  Clock::time_point marker_time;

  while (exit_cond_.get() != RunMode::kStop) {
    auto now = Clock::now();

    // This is to protect against programming errors in the logic below. There should never be a set of states that
    // execute in a hard loop, but if we made a mistake this will limit the damage.
    if (loops++ % 100 == 0) {
      if (now < marker_time + std::chrono::seconds(10)) {
        // We should only go around this loop once / second
        LOG_WARNING << "Aktualizr::RunUpdateLoop is spinning in state " << state_ << " sleeping...";
        std::this_thread::sleep_for(std::chrono::seconds(10));
      }
      marker_time = now;
    }

    if (next_offline_poll_ <= now) {
      next_offline_poll_ = now + std::chrono::seconds(1);
#ifdef BUILD_OFFLINE_UPDATES
      // Poll the magic filesystem directory for offline updates once per second.
      switch (state_) {
        case UpdateCycleState::kUnprovisioned:
        case UpdateCycleState::kSendingDeviceData:
        case UpdateCycleState::kIdle:
        case UpdateCycleState::kSendingManifest:
        case UpdateCycleState::kCheckingForUpdates:
        case UpdateCycleState::kGetConsent:
        case UpdateCycleState::kConfirmingUpdate:
        case UpdateCycleState::kDownloading:
        case UpdateCycleState::kInstalling:
          // In these cases we need to poll for Offline updates
          if (OfflineUpdateAvailable()) {
            if (state_ == UpdateCycleState::kGetConsent) {
              consent_->PendingUpdateCancelled();
            }
            // Asynchronously cancel the current operation
            api_queue_->Cancel();
            // TODO: How can we send an 'update failed' the next time we have idle network

            // Queue an offline update check for after the cancel finishes
            op_update_check_ = CheckUpdatesOffline(config_.uptane.offline_updates_source);
            state_ = UpdateCycleState::kCheckingForUpdatesOffline;
          }
          break;
        case UpdateCycleState::kCheckingForUpdatesOffline:
        case UpdateCycleState::kFetchingImagesOffline:
        case UpdateCycleState::kInstallingOffline:
        case UpdateCycleState::kAwaitReboot:
          // We cannot start an offline update in these states
          break;
        default:
          LOG_ERROR << "Unknown state:" << state_;
          state_ = UpdateCycleState::kIdle;
      }
#endif  // BUILD_OFFLINE_UPDATES
    }
    // Drive the main event loop
    // LOG_TRACE << "State is:" << state_ << " run mode:" << static_cast<int>(exit_cond_.get());
    switch (state_) {
      case UpdateCycleState::kUnprovisioned:
        update_lock_file_.UpdateComplete();
        if (next_online_poll_ <= now && !op_provision_.valid()) {
          op_provision_ = AttemptProvision();
        } else if (op_provision_.valid() && op_provision_.wait_until(next_offline_poll_) == std::future_status::ready) {
          if (op_provision_.get()) {
            // Provisioned OK, send device data
            op_void_ = SendDeviceData();
            state_ = UpdateCycleState::kSendingDeviceData;
          } else {
            // If we didn't provision, then stay in this state. We'll wait until next_online_poll_ before trying again
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          }
          op_provision_ = {};  // Clear future
        } else {
          // Idle but unprovisioned
          std::unique_lock<std::mutex> guard{exit_cond_.m};
          if (exit_cond_.run_mode == RunMode::kOnce) {
            // We tried to provision but it didn't succeed, exit
            exit_cond_.run_mode = RunMode::kStop;
            return ExitReason::kNoUpdates;
          }
          auto next_wake_up = std::min(next_offline_poll_, next_online_poll_);
          exit_cond_.cv.wait_until(guard, next_wake_up);
        }
        break;
      case UpdateCycleState::kSendingDeviceData:
        if (op_void_.wait_until(next_offline_poll_) == std::future_status::ready) {
          state_ = UpdateCycleState::kIdle;
        }
        break;
      case UpdateCycleState::kIdle:
        update_lock_file_.UpdateComplete();
        if (next_online_poll_ <= now) {
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          if (!config_.uptane.enable_online_updates) {
            state_ = UpdateCycleState::kIdle;
            break;
          }
          op_update_check_ = CheckUpdatesPeek(next_check_reason_);
          next_check_reason_ = CheckReason::kPoll;
          state_ = UpdateCycleState::kCheckingForUpdates;
        } else {
          // Idle
          std::unique_lock<std::mutex> guard{exit_cond_.m};
          if (exit_cond_.run_mode == RunMode::kOnce) {
            // We've performed one round of checks, exit from 'once' runmode.
            exit_cond_.run_mode = RunMode::kStop;
            return ExitReason::kNoUpdates;
          }
          auto next_wake_up = std::min(next_offline_poll_, next_online_poll_);
          exit_cond_.cv.wait_until(guard, next_wake_up);
          if (exit_cond_.check_for_updates_now) {
            LOG_INFO << "CheckForUpdates woke Aktualizr thread";
            exit_cond_.check_for_updates_now = false;
            next_check_reason_ = CheckReason::kDbusWake;
            next_online_poll_ = now;
          } else if (!exit_cond_.check_for_offline_updates.empty()) {
            LOG_INFO << "Offline Update woke Aktualizr thread";
#ifdef BUILD_OFFLINE_UPDATES
            op_update_check_ = CheckUpdatesOffline(exit_cond_.check_for_offline_updates);
            state_ = UpdateCycleState::kCheckingForUpdatesOffline;
#else
            LOG_WARNING << "Offline updates are disabled at compile time";
#endif
            exit_cond_.check_for_offline_updates.clear();
          }
        }
        break;
      case UpdateCycleState::kSendingManifest:
        if (op_put_manifest_.wait_until(next_offline_poll_) == std::future_status::ready) {
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          auto put_manifest_result = op_put_manifest_.get();

          // TODO: Phase 6 will integrate OfflineLogsManager here via SotaUptaneClient

          if (put_manifest_result.status == result::PutManifestStatus::kUnprovisioned) {
            LOG_INFO << "Didn't put manifest to server because the device was not able to provision";
            // We can get to this state when doing an offline update
            state_ = UpdateCycleState::kUnprovisioned;
          } else {
            state_ = UpdateCycleState::kIdle;
          }
        }
        break;
      case UpdateCycleState::kCheckingForUpdates:
        if (op_update_check_.wait_until(next_offline_poll_) == std::future_status::ready) {
          update_result_ = op_update_check_.get();
          if (update_lock_file_.ShouldUpdate() == UpdateLockFile::kNoUpdate) {
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
            state_ = UpdateCycleState::kIdle;
            break;
          }
          if (update_result_.updates.empty()) {
            if (update_result_.status == result::UpdateStatus::kError) {
              op_put_manifest_ = SendManifest();
              state_ = UpdateCycleState::kSendingManifest;
              break;
            }
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
            state_ = UpdateCycleState::kIdle;
            break;
          }

          peek_correlation_id_ = update_result_.correlation_id;
          op_consent_ = consent_->GetConsent(update_result_.updates);
          uptane_client_->reportAwaitingConsent();
          state_ = UpdateCycleState::kGetConsent;
        }
        break;
      case UpdateCycleState::kGetConsent:
        if (op_consent_.wait_until(next_offline_poll_) == std::future_status::ready) {
          last_consent_outcome_ = op_consent_.get();
          uptane_client_->reportConsentOutcome(last_consent_outcome_);
          if (last_consent_outcome_.was_cancelled) {
            LOG_INFO << "Install cancelled while waiting for consent";
            state_ = UpdateCycleState::kIdle;
          } else {
            op_update_check_ = CommitUpdate(peek_correlation_id_);
            state_ = UpdateCycleState::kConfirmingUpdate;
          }
        }
        break;
      case UpdateCycleState::kConfirmingUpdate:
        if (op_update_check_.wait_until(next_offline_poll_) == std::future_status::ready) {
          auto confirm_result = op_update_check_.get();

          if (!last_consent_outcome_.granted) {
            LOG_WARNING << "User refused consent of update: " << last_consent_outcome_.reason;
            StoreInstallationFailure(
                data::InstallationResult(data::ResultCode::Numeric::kConsentRefused, last_consent_outcome_.reason));
            op_put_manifest_ = SendManifest();
            state_ = UpdateCycleState::kSendingManifest;
          } else if (confirm_result.status == result::UpdateStatus::kUpdatesAvailable) {
            update_result_ = confirm_result;
            op_download_ = Download(update_result_.updates);
            state_ = UpdateCycleState::kDownloading;
          } else {
            LOG_WARNING << "Update changed or was cancelled after consent: " << confirm_result.message;
            if (confirm_result.status == result::UpdateStatus::kError) {
              StoreInstallationFailure(
                  data::InstallationResult(data::ResultCode::Numeric::kInternalError, confirm_result.message));
              op_put_manifest_ = SendManifest();
              state_ = UpdateCycleState::kSendingManifest;
            } else {
              state_ = UpdateCycleState::kIdle;
            }
          }
        }
        break;
      case UpdateCycleState::kDownloading:
        if (op_download_.wait_until(next_offline_poll_) == std::future_status::ready) {
          result::Download const download_result = op_download_.get();
          if (download_result.status != result::DownloadStatus::kSuccess || download_result.updates.empty()) {
            if (download_result.status != result::DownloadStatus::kNothingToDownload) {
              // If the download failed, inform the backend immediately.
              op_put_manifest_ = SendManifest();
              state_ = UpdateCycleState::kSendingManifest;
              break;
            }
            next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
            state_ = UpdateCycleState::kIdle;
            break;
          }
          op_install_ = Install(download_result.updates);
          state_ = UpdateCycleState::kInstalling;
        }
        break;
      case UpdateCycleState::kInstalling:
        if (op_install_.wait_until(next_offline_poll_) == std::future_status::ready) {
          if (uptane_client_->isInstallCompletionRequired()) {
            state_ = UpdateCycleState::kAwaitReboot;
            break;
          }
          if (!uptane_client_->hasPendingUpdates()) {
            // If updates were applied and no any reboot/finalization is required then send/put manifest
            // as soon as possible, don't wait for config_.uptane.polling_sec
            op_put_manifest_ = SendManifest();
            state_ = UpdateCycleState::kSendingManifest;
            break;
          }
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          state_ = UpdateCycleState::kIdle;
        }
        break;
#ifdef BUILD_OFFLINE_UPDATES
      case UpdateCycleState::kCheckingForUpdatesOffline: {
        update_result_ = op_update_check_.get();  // No need to timeout
        if (update_result_.updates.empty() || update_lock_file_.ShouldUpdate() == UpdateLockFile::kNoUpdate) {
          next_online_poll_ = now + std::chrono::seconds(config_.uptane.polling_sec);
          state_ = UpdateCycleState::kIdle;
          break;
        }
        if (update_result_.status == result::UpdateStatus::kError) {
          op_put_manifest_ = SendManifest();
          state_ = UpdateCycleState::kSendingManifest;
          break;
        }
        op_download_ = Download(update_result_.updates, UpdateType::kOffline);
        state_ = UpdateCycleState::kFetchingImagesOffline;
        break;
      }
      case UpdateCycleState::kFetchingImagesOffline: {
        result::Download const download_result = op_download_.get();
        if (download_result.status != result::DownloadStatus::kSuccess || download_result.updates.empty()) {
          if (download_result.status != result::DownloadStatus::kNothingToDownload) {
            op_put_manifest_ = SendManifest();
            state_ = UpdateCycleState::kSendingManifest;
            break;
          }
          state_ = UpdateCycleState::kIdle;
          break;
        }
        // Record the current journal cursor
        // Record the location of the offline logs file
        // storage_->storageOfflineLogsLocation(...);
        op_install_ = Install(download_result.updates, UpdateType::kOffline);
        state_ = UpdateCycleState::kInstallingOffline;
        break;
      }
      case UpdateCycleState::kInstallingOffline: {
        result::Install const install_result = op_install_.get();
        // TODO: Write the Journal logs out here
        if (uptane_client_->isInstallCompletionRequired()) {
          state_ = UpdateCycleState::kAwaitReboot;
          // In this case the manifest will be sent by SotaUptaneClient::finalizeAfterReboot()
          // after the restart
          break;
        }
        // Even though this is an offline update, tell the server about it. After
        // an online update no manifest is sent if hasPendingUpdates() is set.
        // However:
        //  - There is no operation to update the pending status of secondaries
        //    without a reboot
        //  - Even though sending is skipped now, the manifest will be sent anyway
        //    after the next polling interval.
        //  - isInstallCompletionRequired implies hasPendingUpdates but not vice-versa
        //    because the former requires the primary to be pending whereas hasPendingUpdates
        //    is true if any ecu is pending. I'm not aware of any system that can get into this
        //    state.
        // Kick off a manifest send now anyway
        op_put_manifest_ = SendManifest();
        state_ = UpdateCycleState::kSendingManifest;
        break;
      }
#endif  // BUILD_OFFLINE_UPDATES
      case UpdateCycleState::kAwaitReboot: {
        uptane_client_->completeInstall();
        std::lock_guard<std::mutex> const lock{exit_cond_.m};
        exit_cond_.run_mode = RunMode::kStop;
        exit_cond_.cv.notify_all();
        return ExitReason::kRebootRequired;
      }
      default:
        LOG_ERROR << "Unknown state:" << state_;
        state_ = UpdateCycleState::kIdle;
    }
  };
  LOG_INFO << "RunForever thread exiting";
  return ExitReason::kStopRequested;
}

void Aktualizr::Shutdown() {
  std::lock_guard<std::mutex> const guard{exit_cond_.m};
  exit_cond_.run_mode = RunMode::kStop;
  exit_cond_.cv.notify_all();
}

void Aktualizr::AddSecondary(const std::shared_ptr<SecondaryInterface> &secondary) {
  uptane_client_->addSecondary(secondary);
}

void Aktualizr::SetSecondaryData(const Uptane::EcuSerial &ecu, const std::string &data) {
  storage_->saveSecondaryData(ecu, data);
}

std::vector<SecondaryInfo> Aktualizr::GetSecondaries() const {
  std::vector<SecondaryInfo> info;
  storage_->loadSecondariesInfo(&info);

  return info;
}

std::future<bool> Aktualizr::AttemptProvision() {
  std::function<bool()> task([this] { return uptane_client_->attemptProvision(); });
  return api_queue_->enqueue(std::move(task), false);
}

std::future<result::CampaignCheck> Aktualizr::CampaignCheck() {
  std::function<result::CampaignCheck()> task([this] { return uptane_client_->campaignCheck(); });
  return api_queue_->enqueue(std::move(task), result::CampaignCheck({}));
}

std::future<void> Aktualizr::CampaignControl(const std::string &campaign_id, campaign::Cmd cmd) {
  std::function<void()> task([this, campaign_id, cmd] {
    switch (cmd) {
      case campaign::Cmd::Accept:
        uptane_client_->campaignAccept(campaign_id);
        break;
      case campaign::Cmd::Decline:
        uptane_client_->campaignDecline(campaign_id);
        break;
      case campaign::Cmd::Postpone:
        uptane_client_->campaignPostpone(campaign_id);
        break;
      default:
        break;
    }
  });
  return api_queue_->enqueue(std::move(task));
}

void Aktualizr::SetCustomHardwareInfo(Json::Value hwinfo) { uptane_client_->setCustomHardwareInfo(std::move(hwinfo)); }
std::future<void> Aktualizr::SendDeviceData() {
  std::function<void()> task([this] { uptane_client_->sendDeviceData(); });
  return api_queue_->enqueue(std::move(task));
}

// FIXME: [TDX] This solution must be reviewed (we should probably have a method to be used just for the data proxy).
std::future<void> Aktualizr::SendDeviceData(const Json::Value &hwinfo) {
  std::function<void()> task([this, hwinfo] {
    uptane_client_->setCustomHardwareInfo(hwinfo);
    uptane_client_->sendDeviceData();
  });
  return api_queue_->enqueue(std::move(task));
}

std::future<result::UpdateCheck> Aktualizr::CheckUpdates(CheckReason check_reason) {
  std::function<result::UpdateCheck()> task(
      [this, check_reason] { return uptane_client_->fetchMeta(/*peek=*/false, "", check_reason); });
  return api_queue_->enqueue(std::move(task), result::UpdateCheck());
}

std::future<result::UpdateCheck> Aktualizr::CheckUpdatesPeek(CheckReason check_reason) {
  std::function<result::UpdateCheck()> task(
      [this, check_reason] { return uptane_client_->fetchMeta(/*peek=*/true, "", check_reason); });
  return api_queue_->enqueue(std::move(task), result::UpdateCheck());
}

std::future<result::UpdateCheck> Aktualizr::CommitUpdate(const std::string &correlation_id) {
  std::function<result::UpdateCheck()> task(
      [this, correlation_id] { return uptane_client_->fetchMeta(/*peek=*/false, correlation_id); });
  return api_queue_->enqueue(std::move(task), result::UpdateCheck());
}

std::future<result::Download> Aktualizr::Download(const std::vector<Uptane::Target> &updates, UpdateType update_type) {
  std::function<result::Download()> task(
      [this, updates, update_type]() { return uptane_client_->downloadImages(updates, update_type); });
  return api_queue_->enqueue(std::move(task), result::Download({}, result::DownloadStatus::kError, "Cancelled"));
}

std::future<result::Install> Aktualizr::Install(const std::vector<Uptane::Target> &updates, UpdateType update_type) {
  std::function<result::Install()> task(
      [this, updates, update_type] { return uptane_client_->uptaneInstall(updates, update_type); });
  return api_queue_->enqueue(
      std::move(task),
      result::Install(
          data::InstallationResult(false, data::ResultCode::Numeric::kOperationCancelled, "Operation Cancelled"), {}));
}

void Aktualizr::StoreInstallationFailure(const data::InstallationResult &result) {
  std::function<void()> task([this, result] { uptane_client_->storeInstallationFailure(result); });
  api_queue_->enqueue(std::move(task));
}

bool Aktualizr::SetInstallationRawReport(const std::string &custom_raw_report) {
  return storage_->storeDeviceInstallationRawReport(custom_raw_report);
}

std::future<result::PutManifestResult> Aktualizr::SendManifest(const Json::Value &custom) {
  std::function<result::PutManifestResult()> task([this, custom]() { return uptane_client_->putManifest(custom); });
  return api_queue_->enqueue(std::move(task), {Uptane::Manifest(), result::PutManifestStatus::kCancelled});
}

result::Pause Aktualizr::Pause() {
  if (api_queue_->pause(true)) {
    uptane_client_->reportPause();
    return result::PauseStatus::kSuccess;
  } else {
    return result::PauseStatus::kAlreadyPaused;
  }
}

result::Pause Aktualizr::Resume() {
  if (api_queue_->pause(false)) {
    uptane_client_->reportResume();
    return result::PauseStatus::kSuccess;
  } else {
    return result::PauseStatus::kAlreadyRunning;
  }
}

void Aktualizr::Abort() { api_queue_->Cancel().wait(); }

std::shared_future<void> Aktualizr::Cancel() {
  consent_->PendingUpdateCancelled();
  return api_queue_->Cancel();
}

boost::signals2::connection Aktualizr::SetSignalHandler(
    const std::function<void(std::shared_ptr<event::BaseEvent>)> &handler) {
  return sig_->connect(handler);
}

Aktualizr::InstallationLog Aktualizr::GetInstallationLog() {
  std::vector<Aktualizr::InstallationLogEntry> ilog;

  EcuSerials serials;
  if (!uptane_client_->getEcuSerials(&serials)) {
    throw std::runtime_error("Could not load ECU serials");
  }

  ilog.reserve(serials.size());
  for (const auto &s : serials) {
    Uptane::EcuSerial serial = s.first;
    std::vector<Uptane::Target> installs;

    std::vector<Uptane::Target> log;
    storage_->loadInstallationLog(serial.ToString(), &log, true);

    ilog.emplace_back(Aktualizr::InstallationLogEntry{serial, std::move(log)});
  }

  return ilog;
}

#ifdef BUILD_DBUS

void Aktualizr::SetDbusInterface(SdBus &&bus) {
  auto dbus_adaptor = std::make_unique<Dbus>(std::move(bus), storage_);

  dbus_adaptor->SetCheckForUpdatesCallback([this] {
    std::lock_guard<std::mutex> lock{exit_cond_.m};
    exit_cond_.check_for_updates_now = true;
    exit_cond_.cv.notify_all();
  });

  dbus_adaptor->SetCancelCallback([this] { Cancel(); });
  dbus_adaptor->SetOfflineUpdateCallback([this](const boost::filesystem::path &path) {
    std::lock_guard<std::mutex> lock{exit_cond_.m};
    exit_cond_.check_for_offline_updates = path;
    exit_cond_.cv.notify_all();
  });

  consent_ = std::move(dbus_adaptor);
}

#endif

std::vector<Uptane::Target> Aktualizr::GetStoredTargets() { return uptane_client_->getStoredTargets(); }

void Aktualizr::DeleteStoredTarget(const Uptane::Target &target) { uptane_client_->deleteStoredTarget(target); }

std::ifstream Aktualizr::OpenStoredTarget(const Uptane::Target &target) {
  return uptane_client_->openStoredTarget(target);
}

#ifdef BUILD_OFFLINE_UPDATES
bool Aktualizr::OfflineUpdateAvailable() {
  static const std::string update_subdir{"metadata"};

  OffUpdSourceState old_state = offupd_source_state_;
  OffUpdSourceState cur_state = OffUpdSourceState::Unknown;

  boost::system::error_code ec;
  if (fs::exists(config_.uptane.offline_updates_source, ec)) {
    if (fs::is_directory(config_.uptane.offline_updates_source / update_subdir, ec)) {
      cur_state = OffUpdSourceState::SourceExists;
    } else {
      cur_state = OffUpdSourceState::SourceExistsNoContent;
    }
  } else {
    cur_state = OffUpdSourceState::SourceDoesNotExist;
  }

  offupd_source_state_ = cur_state;
  // LOG_INFO << "OfflineUpdateAvailable: " << int(old_state) << " -> " << int(cur_state);

  return (old_state == OffUpdSourceState::SourceDoesNotExist && cur_state == OffUpdSourceState::SourceExists);
}

std::future<result::UpdateCheck> Aktualizr::CheckUpdatesOffline(const fs::path &source_path) {
  std::function<result::UpdateCheck()> task(
      [this, source_path] { return uptane_client_->fetchMetaOffUpd(source_path); });
  return api_queue_->enqueue(std::move(task), result::UpdateCheck({}, 0, result::UpdateStatus::kError, ""));
}

#endif
