#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <memory>

#include <boost/filesystem.hpp>
#include <boost/polymorphic_pointer_cast.hpp>

#include "libaktualizr/packagemanagerfactory.h"

#include "crypto/crypto.h"
#include "http/httpclient.h"
#include "httpfake.h"
#include "logging/logging.h"
#include "package_manager/ostreemanager.h"
#include "primary/reportqueue.h"
#include "primary/sotauptaneclient.h"
#include "storage/invstorage.h"
#include "uptane/uptanerepository.h"
#include "uptane_test_common.h"
#include "uptane_repo.h"
#include "utilities/utils.h"

namespace {
/**
 * A Mock Secondary that fails on demand
 */
class FailingSecondary : public SecondaryInterface {
 public:
  explicit FailingSecondary(Primary::VirtualSecondaryConfig &sconfig_in) : sconfig(std::move(sconfig_in)) {
    std::string public_key_str;
    if (!Crypto::generateKeyPair(sconfig.key_type, &public_key_str, &private_key)) {
      throw std::runtime_error("Key generation failure");
    }
    public_key = PublicKey(public_key_str, sconfig.key_type);
    Json::Value manifest_unsigned;
    manifest_unsigned["key"] = "value";

    std::string const b64sig = Utils::toBase64(
        Crypto::Sign(sconfig.key_type, nullptr, private_key, Utils::jsonToCanonicalStr(manifest_unsigned)));
    Json::Value signature;
    signature["method"] = "rsassa-pss";
    signature["sig"] = b64sig;
    signature["keyid"] = public_key.KeyId();
    manifest_["signed"] = manifest_unsigned;
    manifest_["signatures"].append(signature);
  }
  void init(std::shared_ptr<SecondaryProvider> secondary_provider_in) override {
    secondary_provider = std::move(secondary_provider_in);
  }
  std::string Type() const override { return "docker-compose"; }
  PublicKey getPublicKey() const override { return public_key; }

  Uptane::HardwareIdentifier getHwId() const override { return Uptane::HardwareIdentifier(sconfig.ecu_hardware_id); }
  Uptane::EcuSerial getSerial() const override {
    if (!sconfig.ecu_serial.empty()) {
      return Uptane::EcuSerial(sconfig.ecu_serial);
    }
    return Uptane::EcuSerial(public_key.KeyId());
  }
  Uptane::Manifest getManifest() const override {
    Json::Value manifest = Uptane::ManifestIssuer::assembleManifest(firmware_info, getSerial());
    manifest["attacks_detected"] = "";
    Uptane::Manifest signed_ecu_version;
    auto const b64sig = Utils::toBase64(Crypto::RSAPSSSign(nullptr, private_key, Utils::jsonToCanonicalStr(manifest)));
    Json::Value signature;
    signature["method"] = "rsassa-pss";
    signature["sig"] = b64sig;

    signature["keyid"] = public_key.KeyId();
    signed_ecu_version["signed"] = manifest;
    signed_ecu_version["signatures"] = Json::Value(Json::arrayValue);
    signed_ecu_version["signatures"].append(signature);

    return signed_ecu_version;
  }
  bool ping() const override { return true; }

  bool needsImageFileOnPrimary() const override { return needs_image_file_on_primary_; }

  data::InstallationResult putMetadata(const Uptane::Target & /*target*/) override {
    return {data::ResultCode::Numeric::kOk, ""};
  }
  int32_t getRootVersion(bool /*director*/) const override { return 1; }

  data::InstallationResult putRoot(const std::string & /*root*/, bool /*director*/) override {
    return {data::ResultCode::Numeric::kOk, ""};
  }
  data::InstallationResult sendFirmware(const Uptane::Target & /*target*/, const InstallInfo & /*install_info*/,
                                        const api::FlowControlToken *flow_control) override {
    send_firmware_calls++;
    if (abort_during_send_firmware) {
      assert(flow_control != nullptr);
      // Simulate a user on a separate thread cancelling the ongoing operation
      auto *fct = const_cast<api::FlowControlToken *>(flow_control);
      fct->setAbort();
      assert(flow_control->hasAborted());
      return {data::ResultCode::Numeric::kOperationCancelled, ""};
    }
    return {send_firmware_result, ""};
  }
  data::InstallationResult install(const Uptane::Target &target, const InstallInfo & /*info*/,
                                   const api::FlowControlToken * /*flow_control*/) override {
    install_calls++;
    if (observe_plan_storage) {
      std::string plan;
      sync_plan_seen_during_install = observe_plan_storage->loadSyncPlan(&plan);
    }
    if (install_order != nullptr) {
      install_order->push_back(getSerial().ToString());
    }
    return installCommon(target);
  }

  boost::optional<data::InstallationResult> completePendingInstall(const Uptane::Target &target) override {
    complete_pending_install_calls++;
    return installCommon(target);
  }

  data::InstallationResult installCommon(const Uptane::Target &target) {
    if (install_result == data::ResultCode::Numeric::kOk) {
      // Record the fact we did an update so it appears in getManifest()
      firmware_info.hash = target.sha256Hash();
      firmware_info.len = target.length();
      firmware_info.name = target.filename();
    }
    return {install_result, ""};
  }

  void rollbackPendingInstall() override { rollback_pending_install_calls++; }

  void cleanStartup() override { nothing_pending_calls++; }

#ifdef BUILD_OFFLINE_UPDATES
  data::InstallationResult putMetadataOffUpd(const Uptane::Target & /*target*/,
                                             const Uptane::OfflineUpdateFetcher & /*fetcher*/) override {
    return {data::ResultCode::Numeric::kInternalError, "SecondaryInterfaceMock::putMetadataOffUpd not implemented"};
  }
#endif

  std::shared_ptr<SecondaryProvider> secondary_provider;
  PublicKey public_key;
  std::string private_key;
  Json::Value manifest_;

  Primary::VirtualSecondaryConfig sconfig;
  int send_firmware_calls{0};
  data::ResultCode::Numeric send_firmware_result{data::ResultCode::Numeric::kOk};
  int install_calls{0};
  int complete_pending_install_calls{0};
  int rollback_pending_install_calls{0};
  int nothing_pending_calls{0};
  // This result is used for both install and completePendingInstall
  data::ResultCode::Numeric install_result{data::ResultCode::Numeric::kOk};
  Uptane::InstalledImageInfo firmware_info;
  // Simulate a user abort during sendFirmware
  bool abort_during_send_firmware{false};
  // When false, primary will not fetch/store image for this secondary (handler-download mode).
  bool needs_image_file_on_primary_{true};
  std::vector<std::string> *install_order{nullptr};
  std::shared_ptr<INvStorage> observe_plan_storage;
  bool sync_plan_seen_during_install{false};
};

class ExplicitSecondary : public FailingSecondary {
 public:
  ExplicitSecondary(Primary::VirtualSecondaryConfig &sconfig_in, bool supports_rollback,
                    std::vector<std::string> *install_order = nullptr)
      : FailingSecondary(sconfig_in), supports_rollback_(supports_rollback) {
    this->install_order = install_order;
  }

  std::string Type() const override { return "torizon-generic"; }
  bool supportsRollback() const { return supports_rollback_; }

 private:
  bool supports_rollback_;
};

struct TestOptions {
  bool fail_primary_install{false};
  bool primary_installs_on_reboot{true};
  bool explicit_secondary{false};
  bool secondary_supports_rollback{false};
  int explicit_members{0};
};

std::shared_ptr<HttpFake> makeHttp(const boost::filesystem::path &test_dir, const TestOptions &options) {
  if (options.explicit_members == 0) {
    return std::make_shared<HttpFake>(test_dir, "hasupdates");
  }

  class ExplicitHttpFake final : public HttpFake {
   public:
    ExplicitHttpFake(const boost::filesystem::path &dir, int members) : HttpFake(dir), members_(members) {
      meta_dir = generated_.Path() / "repo";
    }

    HttpResponse get(const std::string &url, int64_t maxsize, const api::FlowControlToken *flow_control,
                     const Headers *extra_headers) override {
      (void)maxsize;
      (void)extra_headers;
      prepare();
      if (flow_control != nullptr && flow_control->hasAborted()) {
        return HttpResponse("", 0, CURLE_ABORTED_BY_CALLBACK, "Cancelled by FlowControlToken");
      }
      const auto file = files_.find(url.substr(tls_server.size()));
      if (file == files_.end()) {
        return HttpResponse({}, 404, CURLE_OK, "");
      }
      return HttpResponse(file->second, 200, CURLE_OK, "");
    }

    std::future<HttpResponse> downloadAsync(const std::string &url, curl_write_callback write_cb,
                                            curl_xferinfo_callback progress_cb, void *userp, curl_off_t from) override {
      (void)from;
      prepare();
      std::promise<HttpResponse> promise;
      auto future = promise.get_future();
      const auto file = files_.find(url.substr(tls_server.size()));
      if (file == files_.end()) {
        promise.set_value(HttpResponse("", 404, CURLE_OK, ""));
        return future;
      }
      const std::string content = file->second;
      for (char byte : content) {
        write_cb(&byte, 1, 1, userp);
        progress_cb(userp, 0, 0, 0, 0);
      }
      promise.set_value(HttpResponse(content, 200, CURLE_OK, ""));
      return future;
    }

   private:
    void prepare() {
      if (prepared_) {
        return;
      }
      const boost::filesystem::path payload_dir = generated_.Path() / "payloads";
      boost::filesystem::create_directories(payload_dir);
      repo_ = std_::make_unique<UptaneRepo>(generated_.Path(), "2029-07-04T16:33:27Z", "id0");
      repo_->generateRepo(KeyType::kED25519);

      const auto add_target = [this, &payload_dir](const std::string &filename, const std::string &hardware_id,
                                                  const std::string &serial) {
        const boost::filesystem::path payload = payload_dir / filename;
        Utils::writeFile(payload, "explicit sync group payload for " + serial);
        repo_->addImage(payload, filename, hardware_id);
        repo_->addTarget(filename, hardware_id, serial);
      };

      if (members_ == 3) {
        add_target("primary_firmware.txt", "primary_hw", "CA:FE:A6:D2:84:9D");
      }
      add_target("secondary_firmware.txt", "secondary_hw", "secondary_ecu_serial");
      add_target("generic_firmware.txt", "generic_hw", "generic_ecu_serial");
      repo_->signTargets();
      const std::vector<std::string> metadata_files{
          "repo/1.root.json",       "repo/root.json",      "repo/timestamp.json",
          "repo/snapshot.json",     "repo/targets.json",   "director/1.root.json",
          "director/root.json",     "director/targets.json"};
      const boost::filesystem::path repository_root = generated_.Path() / "repo";
      for (const auto &file : metadata_files) {
        files_["/" + file] = Utils::readFile(repository_root / file);
      }
      const std::vector<std::string> target_files{
          "secondary_firmware.txt", "generic_firmware.txt", "primary_firmware.txt"};
      for (const auto &file : target_files) {
        const boost::filesystem::path target = repository_root / "repo" / "targets" / file;
        if (boost::filesystem::exists(target)) {
          files_["/repo/targets/" + file] = Utils::readFile(target);
        }
      }
      prepared_ = true;
    }

    TemporaryDirectory generated_;
    std::unique_ptr<UptaneRepo> repo_;
    std::map<std::string, std::string> files_;
    int members_;
    bool prepared_{false};
  };

  return std::make_shared<ExplicitHttpFake>(test_dir, options.explicit_members);
}

struct TestScaffolding {
  explicit TestScaffolding(TestOptions test_options = TestOptions())
      : conf{"tests/config/basic.toml"},
        http{makeHttp(temp_dir.Path(), test_options)},
        events_channel{std::make_shared<event::Channel>()} {
    conf.provision.primary_ecu_serial = "CA:FE:A6:D2:84:9D";
    conf.provision.primary_ecu_hardware_id = "primary_hw";
    conf.uptane.director_server = http->tls_server + "/director";
    conf.uptane.repo_server = http->tls_server + "/repo";
    conf.uptane.force_install_completion = true;
    conf.pacman.images_path = temp_dir.Path() / "images";
    conf.bootloader.reboot_sentinel_dir = temp_dir.Path();
    // A sync group rollback runs the real Bootloader::reboot(), which would
    // otherwise try to run /sbin/reboot on the machine running the tests.
    conf.bootloader.reboot_command = "/bin/true";
    // The CI image has no fw_setenv. A failing command must not mark the plan
    // failed, so the tests use a command that succeeds.
    conf.bootloader.rollback_command = "/bin/true";
    conf.pacman.fake_need_reboot = test_options.primary_installs_on_reboot;
    conf.pacman.fake_fail_install = test_options.fail_primary_install;

    conf.storage.path = temp_dir.Path();
    conf.tls.server = http->tls_server;

    storage = INvStorage::newStorage(conf.storage);

    ecu_config.partial_verifying = false;
    ecu_config.full_client_dir = temp_dir.Path();
    ecu_config.ecu_serial = "secondary_ecu_serial";
    ecu_config.ecu_hardware_id = "secondary_hw";
    ecu_config.ecu_private_key = "secondary.priv";
    ecu_config.ecu_public_key = "secondary.pub";
    ecu_config.firmware_path = temp_dir / "firmware.txt";
    ecu_config.target_name_path = temp_dir / "firmware_name.txt";
    ecu_config.metadata_path = temp_dir / "secondary_metadata";
    if (test_options.explicit_secondary) {
      secondary = std::make_shared<ExplicitSecondary>(ecu_config, test_options.secondary_supports_rollback);
    } else {
      secondary = std::make_shared<FailingSecondary>(ecu_config);
    }

    events_channel->connect([this](const std::shared_ptr<event::BaseEvent> &event) {
      events[event->variant]++;
      if (event->variant == "AllInstallsComplete") {
        auto concrete_event = std::static_pointer_cast<event::AllInstallsComplete>(event);
        EXPECT_EQ(expected_install_report, concrete_event->result.dev_report.result_code.num_code);
      }
    });

    dut = std_::make_unique<UptaneTestCommon::TestUptaneClient>(conf, storage, http, events_channel);
    dut->addSecondary(secondary);
  }

  void Reboot() {
    boost::filesystem::remove(conf.bootloader.reboot_sentinel_dir / conf.bootloader.reboot_sentinel_name);
    dut = std_::make_unique<UptaneTestCommon::TestUptaneClient>(conf, storage, http);
    dut->addSecondary(secondary);
  }

  Config conf;
  TemporaryDirectory temp_dir;
  std::shared_ptr<HttpFake> http;
  Primary::VirtualSecondaryConfig ecu_config;
  std::shared_ptr<FailingSecondary> secondary;
  std::shared_ptr<INvStorage> storage;
  std::unique_ptr<UptaneTestCommon::TestUptaneClient> dut;
  std::shared_ptr<event::Channel> events_channel;
  std::map<std::string, int> events;
  data::ResultCode::Numeric expected_install_report{data::ResultCode::Numeric::kUnknown};
};

Uptane::Target explicitTarget(Uptane::Target target, const std::string &serial, const std::string &hardware_id,
                              const std::string &group_id, const boost::optional<int> &order) {
  Json::Value custom = target.custom_data();
  custom["ecuIdentifiers"] = Json::Value(Json::objectValue);
  custom["ecuIdentifiers"][serial]["hardwareId"] = hardware_id;
  custom["sync_group_id"] = group_id;
  if (order) {
    custom["sync_order"] = *order;
  } else {
    custom.removeMember("sync_order");
  }
  target.updateCustom(custom);
  return target;
}

Uptane::Target targetFor(const std::vector<Uptane::Target> &updates, const std::string &serial) {
  const Uptane::EcuSerial ecu_serial(serial);
  const auto target = std::find_if(updates.cbegin(), updates.cend(),
                                   [&ecu_serial](const Uptane::Target &candidate) {
                                     return candidate.IsForEcu(ecu_serial);
                                   });
  if (target == updates.cend()) {
    throw std::runtime_error("No target for ECU " + serial);
  }
  return *target;
}

}  // anonymous namespace

/*
 * Send metadata to Secondary ECUs
 * Send EcuInstallationStartedReport to server for Secondaries
 */
TEST(UptaneUpdateFailure, SynchronousSecondaryUpdatesSuccess) {
  TestScaffolding s;  // NOLINT

  EXPECT_NO_THROW(s.dut->initialize());
  EXPECT_EQ(s.secondary->nothing_pending_calls, 1);

  result::UpdateCheck const update_result = s.dut->fetchMeta();
  EXPECT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  // Preparatory work
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);
  EXPECT_EQ(s.secondary->install_calls, 0);
  EXPECT_EQ(s.secondary->send_firmware_calls, 0);

  s.expected_install_report = data::ResultCode::Numeric::kNeedCompletion;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kNeedCompletion);
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
  EXPECT_EQ(s.secondary->install_calls, 0) << "The sync plan applies the Secondary after the reboot";
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_EQ(s.events["AllInstallsComplete"], 1);

  boost::optional<Uptane::Target> pending_primary;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, nullptr, &pending_primary, nullptr);
  ASSERT_TRUE(!!pending_primary);

  // Simulate a reboot
  s.Reboot();
  s.expected_install_report = data::ResultCode::Numeric::kOk;
  EXPECT_NO_THROW(s.dut->initialize());
  EXPECT_EQ(s.secondary->nothing_pending_calls, 1) << "Shouldn't be called when there is a pending update";

  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
  EXPECT_EQ(s.secondary->install_calls, 1) << "The apply happens on the boot into the new OS";
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_EQ(s.events["AllInstallsComplete"], 1);

  boost::optional<Uptane::Target> current_primary;
  boost::optional<Uptane::Target> pending_after;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, &current_primary, &pending_after, nullptr);
  ASSERT_TRUE(!!current_primary);
  EXPECT_EQ(current_primary->sha256Hash(), pending_primary->sha256Hash());
  EXPECT_FALSE(!!pending_after) << "The Primary is promoted only when the group commits";
}

/**
 * Recovery if the Primary was stored as current before the Secondary applied.
 * The normal path keeps the Primary pending until the group commits.
 */
TEST(UptaneUpdateFailure, SynchronousSecondaryUpdatesResumeAfterPrimaryPromotion) {
  TestScaffolding s;  // NOLINT

  EXPECT_NO_THROW(s.dut->initialize());
  result::UpdateCheck const update_result = s.dut->fetchMeta();
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  s.expected_install_report = data::ResultCode::Numeric::kNeedCompletion;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kNeedCompletion);
  EXPECT_EQ(s.secondary->install_calls, 0);

  // Store the Primary as current, as an interrupted finalize used to, then
  // lose power before the Secondary apply.
  boost::optional<Uptane::Target> pending_primary;
  Uptane::CorrelationId correlation_id;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, nullptr, &pending_primary, &correlation_id);
  ASSERT_TRUE(!!pending_primary);
  s.storage->saveInstalledVersion(s.conf.provision.primary_ecu_serial, *pending_primary,
                                  InstalledVersionUpdateMode::kCurrent, correlation_id);

  s.Reboot();
  s.expected_install_report = data::ResultCode::Numeric::kOk;
  EXPECT_NO_THROW(s.dut->initialize());

  EXPECT_EQ(s.secondary->install_calls, 1) << "The plan must still apply the Secondary";
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_EQ(s.secondary->rollback_pending_install_calls, 0);
  EXPECT_FALSE(s.dut->hasPendingUpdates()) << "The group must reach a terminal state";
}

/**
 * Exercise a couple of failure cases during a synchronous install
 * 1) Download failure
 * 2) Secondary Installation Failure
 * 3) Success
 */
TEST(UptaneUpdateFailure, SynchronousSecondaryUpdatesFailure) {
  TestScaffolding s;  // NOLINT

  EXPECT_NO_THROW(s.dut->initialize());
  result::UpdateCheck update_result = s.dut->fetchMeta();
  EXPECT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  // Preparatory work
  result::Download download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);
  EXPECT_EQ(s.secondary->install_calls, 0);
  EXPECT_EQ(s.secondary->send_firmware_calls, 0);

  // Case 1: Sending the firmware fails
  s.secondary->send_firmware_result = data::ResultCode::Numeric::kDownloadFailed;
  s.expected_install_report = data::ResultCode::Numeric::kDownloadFailed;
  result::Install install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kDownloadFailed);
  EXPECT_EQ(s.secondary->install_calls, 0);
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);

  // Case 2: Installing the secondary firmware fails
  s.secondary->send_firmware_result = data::ResultCode::Numeric::kOk;
  s.secondary->install_result = data::ResultCode::Numeric::kDownloadFailed;
  s.secondary->install_calls = 0;
  s.secondary->send_firmware_calls = 0;
  // First time through it needs a reboot
  s.expected_install_report = data::ResultCode::Numeric::kNeedCompletion;
  install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kNeedCompletion);
  EXPECT_EQ(s.secondary->install_calls, 0);
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_TRUE(s.dut->isInstallCompletionRequired());

  boost::optional<Uptane::Target> pending_primary;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, nullptr, &pending_primary, nullptr);
  ASSERT_TRUE(!!pending_primary);

  // Simulate a reboot
  s.Reboot();
  EXPECT_NO_THROW(s.dut->initialize());

  EXPECT_EQ(s.secondary->install_calls, 1) << "The sync plan applies the Secondary after the reboot";
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_EQ(s.secondary->rollback_pending_install_calls, 1) << "The failed apply is rolled back";

  boost::optional<Uptane::Target> current_primary;
  boost::optional<Uptane::Target> pending_after;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, &current_primary, &pending_after, nullptr);
  EXPECT_FALSE(!!pending_after);
  if (current_primary) {
    EXPECT_NE(current_primary->sha256Hash(), pending_primary->sha256Hash())
        << "A failed group must not leave the new OS stored as current";
  }

  // The rollback boot sends the failure manifest. This process did not reboot,
  // so run startup once more to deliver it and clear the plan.
  EXPECT_NO_THROW(s.dut->initialize());

  // Case 3: Happy path. The Primary target is already installed, so this is
  // not a sync group and the Secondary installs in this call.
  s.storage->saveInstalledVersion(s.conf.provision.primary_ecu_serial, *pending_primary,
                                  InstalledVersionUpdateMode::kCurrent, "id0");

  // Case 3: Happy path
  s.secondary->send_firmware_result = data::ResultCode::Numeric::kOk;
  s.secondary->install_result = data::ResultCode::Numeric::kOk;
  s.secondary->install_calls = 0;
  s.secondary->send_firmware_calls = 0;
  s.secondary->complete_pending_install_calls = 0;

  update_result = s.dut->fetchMeta();
  EXPECT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  download_result = s.dut->downloadImages(update_result.updates);
  s.expected_install_report = data::ResultCode::Numeric::kOk;
  install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_TRUE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kOk);
  EXPECT_EQ(s.secondary->install_calls, 1);
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_FALSE(s.dut->isInstallCompletionRequired());
}

/**
 * A compose install that asks for a later completion fails the group. The
 * stored result is a failure, and completePendingInstall is not used.
 */
TEST(UptaneUpdateFailure, SynchronousSecondaryNeedCompletionFailsThePlan) {
  TestScaffolding s;  // NOLINT

  EXPECT_NO_THROW(s.dut->initialize());
  result::UpdateCheck const update_result = s.dut->fetchMeta();
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  s.secondary->install_result = data::ResultCode::Numeric::kNeedCompletion;
  s.expected_install_report = data::ResultCode::Numeric::kNeedCompletion;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kNeedCompletion);
  EXPECT_EQ(s.secondary->install_calls, 0);

  boost::optional<Uptane::Target> pending_primary;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, nullptr, &pending_primary, nullptr);
  ASSERT_TRUE(!!pending_primary);

  s.Reboot();
  EXPECT_NO_THROW(s.dut->initialize());

  EXPECT_EQ(s.secondary->install_calls, 1);
  EXPECT_EQ(s.secondary->complete_pending_install_calls, 0);
  EXPECT_EQ(s.secondary->rollback_pending_install_calls, 1);

  boost::optional<Uptane::Target> current_primary;
  boost::optional<Uptane::Target> pending_after;
  s.storage->loadInstalledVersions(s.conf.provision.primary_ecu_serial, &current_primary, &pending_after, nullptr);
  EXPECT_FALSE(!!pending_after);
  if (current_primary) {
    EXPECT_NE(current_primary->sha256Hash(), pending_primary->sha256Hash());
  }

  std::vector<std::pair<Uptane::EcuSerial, data::InstallationResult>> ecu_results;
  ASSERT_TRUE(s.storage->loadEcuInstallationResults(&ecu_results));
  const auto secondary_result = std::find_if(
      ecu_results.cbegin(), ecu_results.cend(),
      [](const std::pair<Uptane::EcuSerial, data::InstallationResult> &r) {
        return r.first.ToString() == "secondary_ecu_serial";
      });
  ASSERT_NE(secondary_result, ecu_results.cend());
  EXPECT_EQ(secondary_result->second.result_code.num_code, data::ResultCode::Numeric::kInstallFailed);
  EXPECT_EQ(secondary_result->second.description, "A sync group install must not return need-completion");

  EXPECT_NO_THROW(s.dut->initialize());
  const auto report = s.http->last_manifest["signed"]["installation_report"];
  EXPECT_EQ(report["report"]["items"][1]["result"]["code"].asString(), "INSTALL_FAILED");
  EXPECT_EQ(report["report"]["result"]["code"].asString().find("NEED_COMPLETION"), std::string::npos)
      << report["report"]["result"]["code"].asString();
}

TEST(UptaneUpdateFailure, ExplicitGroupRejectsGenericWithoutRollback) {
  TestOptions options;
  options.explicit_secondary = true;
  TestScaffolding s{options};  // NOLINT
  ASSERT_NO_THROW(s.dut->initialize());

  const result::UpdateCheck update_result = s.dut->fetchMeta();
  const result::Download download_result = s.dut->downloadImages(update_result.updates);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  const std::vector<Uptane::Target> updates{
      explicitTarget(targetFor(download_result.updates, s.conf.provision.primary_ecu_serial),
                     s.conf.provision.primary_ecu_serial, s.conf.provision.primary_ecu_hardware_id, "g", 1),
      explicitTarget(targetFor(download_result.updates, "secondary_ecu_serial"), "secondary_ecu_serial",
                     "secondary_hw", "g", 1)};

  s.expected_install_report = data::ResultCode::Numeric::kInternalError;
  const result::Install install_result = s.dut->uptaneInstall(updates);

  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kInternalError);
  EXPECT_EQ(install_result.dev_report.description, "torizon-generic sync member does not support rollback");
  std::string plan;
  EXPECT_FALSE(s.storage->loadSyncPlan(&plan));
}

TEST(UptaneUpdateFailure, ExplicitNoPendingNoOstreePlanFailsClosedOnInitialize) {
  TestOptions options;
  options.explicit_secondary = true;
  options.secondary_supports_rollback = true;
  TestScaffolding s{options};  // NOLINT

  SyncPlan plan = SyncPlan::Create(
      "id0",
      {{"secondary_ecu_serial", "secondary_hw", SyncPlan::Phase::kStaged, false},
       {"generic_ecu_serial", "generic_hw", SyncPlan::Phase::kStaged, false}},
      false);
  s.storage->saveSyncPlan(Utils::jsonToCanonicalStr(plan.toJson()));

  EXPECT_NO_THROW(s.dut->initialize());

  std::string stored_plan;
  EXPECT_FALSE(s.storage->loadSyncPlan(&stored_plan));
}

TEST(UptaneUpdateFailure, ExplicitGroupInstallsRollbackCapableGenericSameBoot) {
  TestOptions options;
  options.primary_installs_on_reboot = false;
  options.explicit_secondary = true;
  options.secondary_supports_rollback = true;
  TestScaffolding s{options};  // NOLINT
  ASSERT_NO_THROW(s.dut->initialize());

  const result::UpdateCheck update_result = s.dut->fetchMeta();
  const result::Download download_result = s.dut->downloadImages(update_result.updates);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  const std::vector<Uptane::Target> updates{
      explicitTarget(targetFor(download_result.updates, s.conf.provision.primary_ecu_serial),
                     s.conf.provision.primary_ecu_serial, s.conf.provision.primary_ecu_hardware_id, "g",
                     boost::none),
      explicitTarget(targetFor(download_result.updates, "secondary_ecu_serial"), "secondary_ecu_serial",
                     "secondary_hw", "g", 1)};

  s.secondary->observe_plan_storage = s.storage;
  s.expected_install_report = data::ResultCode::Numeric::kOk;
  const result::Install install_result = s.dut->uptaneInstall(updates);

  EXPECT_TRUE(install_result.dev_report.isSuccess());
  EXPECT_EQ(s.secondary->install_calls, 1);
  EXPECT_TRUE(s.secondary->sync_plan_seen_during_install);
  EXPECT_EQ(s.secondary->rollback_pending_install_calls, 0);
  std::string plan;
  EXPECT_FALSE(s.storage->loadSyncPlan(&plan));
}

TEST(UptaneUpdateFailure, ExplicitThreeWayGroupInstallsSecondariesInOrder) {
  TestOptions options;
  options.primary_installs_on_reboot = false;
  options.explicit_members = 3;
  TestScaffolding s{options};  // NOLINT
  ASSERT_NO_THROW(s.dut->initialize());

  std::vector<std::string> install_order;
  s.secondary->install_order = &install_order;

  Primary::VirtualSecondaryConfig generic_config;
  generic_config.partial_verifying = false;
  generic_config.full_client_dir = s.temp_dir.Path();
  generic_config.ecu_serial = "generic_ecu_serial";
  generic_config.ecu_hardware_id = "generic_hw";
  generic_config.ecu_private_key = "generic.priv";
  generic_config.ecu_public_key = "generic.pub";
  generic_config.firmware_path = s.temp_dir / "generic-firmware.txt";
  generic_config.target_name_path = s.temp_dir / "generic-firmware-name.txt";
  generic_config.metadata_path = s.temp_dir / "generic_metadata";
  auto generic = std::make_shared<ExplicitSecondary>(generic_config, true, &install_order);
  s.dut->addSecondary(generic);

  const result::UpdateCheck update_result = s.dut->fetchMeta();
  const result::Download download_result = s.dut->downloadImages(update_result.updates);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  std::vector<Uptane::Target> updates{
      explicitTarget(targetFor(download_result.updates, s.conf.provision.primary_ecu_serial),
                     s.conf.provision.primary_ecu_serial, s.conf.provision.primary_ecu_hardware_id, "g", 3),
      explicitTarget(targetFor(download_result.updates, "secondary_ecu_serial"), "secondary_ecu_serial",
                     "secondary_hw", "g", 1),
      explicitTarget(targetFor(download_result.updates, "generic_ecu_serial"), "generic_ecu_serial", "generic_hw",
                     "g", 2)};

  s.expected_install_report = data::ResultCode::Numeric::kOk;
  const result::Install install_result = s.dut->uptaneInstall(updates);

  EXPECT_TRUE(install_result.dev_report.isSuccess());
  EXPECT_EQ(s.secondary->install_calls, 1);
  EXPECT_EQ(generic->install_calls, 1);
  EXPECT_EQ(install_order, (std::vector<std::string>{"secondary_ecu_serial", "generic_ecu_serial"}));
}

TEST(UptaneUpdateFailure, ExplicitNoOstreeTwoGenericRollsBackFirstAfterSecondFails) {
  TestOptions options;
  options.explicit_secondary = true;
  options.secondary_supports_rollback = true;
  options.explicit_members = 2;
  TestScaffolding s{options};  // NOLINT
  ASSERT_NO_THROW(s.dut->initialize());

  Primary::VirtualSecondaryConfig second_config;
  second_config.partial_verifying = false;
  second_config.full_client_dir = s.temp_dir.Path();
  second_config.ecu_serial = "generic_ecu_serial";
  second_config.ecu_hardware_id = "generic_hw";
  second_config.ecu_private_key = "generic.priv";
  second_config.ecu_public_key = "generic.pub";
  second_config.firmware_path = s.temp_dir / "generic-firmware.txt";
  second_config.target_name_path = s.temp_dir / "generic-firmware-name.txt";
  second_config.metadata_path = s.temp_dir / "generic_metadata";
  auto second = std::make_shared<ExplicitSecondary>(second_config, true);
  second->install_result = data::ResultCode::Numeric::kInstallFailed;
  s.dut->addSecondary(second);

  const result::UpdateCheck update_result = s.dut->fetchMeta();
  const result::Download download_result = s.dut->downloadImages(update_result.updates);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  const std::vector<Uptane::Target> updates{
      explicitTarget(targetFor(download_result.updates, "secondary_ecu_serial"), "secondary_ecu_serial",
                     "secondary_hw", "g", 1),
      explicitTarget(targetFor(download_result.updates, "generic_ecu_serial"), "generic_ecu_serial", "generic_hw",
                     "g", 2)};

  s.expected_install_report = data::ResultCode::Numeric::kInstallFailed;
  const result::Install install_result = s.dut->uptaneInstall(updates);

  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(s.secondary->install_calls, 1);
  EXPECT_EQ(second->install_calls, 1);
  EXPECT_EQ(s.secondary->rollback_pending_install_calls, 1);
  EXPECT_EQ(second->rollback_pending_install_calls, 0);
  std::string plan;
  EXPECT_FALSE(s.storage->loadSyncPlan(&plan));
}

/**
 * The user cancels during an installation
 */
TEST(UptaneUpdateFailure, Cancellation) {
  TestScaffolding s;  // NOLINT
  s.dut->initialize();

  result::UpdateCheck const update_result = s.dut->fetchMeta();
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  s.secondary->abort_during_send_firmware = true;
  s.expected_install_report = data::ResultCode::Numeric::kOperationCancelled;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);
  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kOperationCancelled);
  EXPECT_EQ(s.secondary->install_calls, 0);
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
}

/**
 * A sync update where both primary and secondary install without reboot
 */
TEST(UptaneUpdateFailure, SuccessNoReboot) {
  TestOptions test_options;
  test_options.primary_installs_on_reboot = false;
  TestScaffolding s{test_options};  // NOLINT
  s.dut->initialize();

  result::UpdateCheck const update_result = s.dut->fetchMeta();
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  s.expected_install_report = data::ResultCode::Numeric::kOk;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);

  EXPECT_TRUE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kOk);
  EXPECT_EQ(s.secondary->install_calls, 1) << "Without a reboot the Secondary applies in the same call";
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
}

/**
 * The primary can install without a reboot, and the installation on it fails.
 */
TEST(UptaneUpdateFailure, PrimaryInstallFailureNoReboot) {
  TestOptions test_options;
  test_options.primary_installs_on_reboot = false;
  test_options.fail_primary_install = true;
  TestScaffolding s{test_options};  // NOLINT
  s.dut->initialize();

  result::UpdateCheck const update_result = s.dut->fetchMeta();
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  s.expected_install_report = data::ResultCode::Numeric::kInstallFailed;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);

  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code,
            data::ResultCode(data::ResultCode::Numeric::kInstallFailed, "primary_hw:INSTALL_FAILED"));
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);
  EXPECT_EQ(s.secondary->install_calls, 0);

  // Check the manifest that was reported to the backend
  auto manifest_result = s.dut->putManifest();
  auto manifest = s.http->last_manifest["signed"];
  auto report = manifest["installation_report"];

  auto expected_report = Utils::parseJSON(R"(
  {
          "content_type" : "application/vnd.com.here.otac.installationReport.v1",
          "report" :
          {
                  "correlation_id" : "id0",
                  "items" :
                  [
                          {
                                  "ecu" : "CA:FE:A6:D2:84:9D",
                                  "result" :
                                  {
                                          "code" : "INSTALL_FAILED",
                                          "description" : "PackageManagerFake install failed because of fake_fail_install",
                                          "success" : false
                                  }
                          }
                  ],
                  "raw_report" : "Installation failed on one or more ECUs",
                  "result" :
                  {
                          "code" : "primary_hw:INSTALL_FAILED",
                          "description" : "Installation failed on one or more ECUs",
                          "success" : false
                  }
          }
  })");
  EXPECT_EQ(expected_report, report);

  // Also check what was sent to server matches what we got back
  EXPECT_EQ(manifest_result.manifest, manifest);
}

/**
 * The primary needs a reboot to install, and on the reboot the installation fails.
 * The Secondary is part of the sync group, so it is never applied and is
 * reported as failed along with the primary.
 */
TEST(UptaneUpdateFailure, PrimaryInstallFailure) {
  TestOptions test_options;
  test_options.fail_primary_install = true;
  TestScaffolding s{test_options};  // NOLINT
  s.dut->initialize();

  result::UpdateCheck const update_result = s.dut->fetchMeta();
  result::Download const download_result = s.dut->downloadImages(update_result.updates);
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  s.expected_install_report = data::ResultCode::Numeric::kNeedCompletion;
  result::Install const install_result = s.dut->uptaneInstall(download_result.updates);

  EXPECT_FALSE(install_result.dev_report.isSuccess());
  EXPECT_EQ(install_result.dev_report.result_code, data::ResultCode::Numeric::kNeedCompletion);
  EXPECT_EQ(s.secondary->install_calls, 0);
  EXPECT_EQ(s.secondary->send_firmware_calls, 1);

  // Simulate a reboot
  s.Reboot();
  // The AllInstallsComplete event isn't sent
  // s.expected_install_report = data::ResultCode::Numeric::kVerificationFailed;
  EXPECT_NO_THROW(s.dut->initialize());

  // Check the manifest that was reported to the backend
  auto manifest = s.http->last_manifest["signed"];
  auto report = manifest["installation_report"];
  // std::cout << "Actual installation report is:" << report;

  auto expected_report = Utils::parseJSON(R"(
  {
    "content_type" : "application/vnd.com.here.otac.installationReport.v1",
    "report" :
    {
      "correlation_id" : "id0",
      "items" :
      [
        {
          "ecu" : "CA:FE:A6:D2:84:9D",
          "result" :
          {
            "code" : "INSTALL_FAILED",
            "description" : "PackageManagerFake install failed after reboot because of fake_fail_install",
            "success" : false
          }
        },
        {
          "ecu" : "secondary_ecu_serial",
          "result" :
          {
            "code" : "INSTALL_FAILED",
            "description" : "The synchronous update this ECU belonged to failed",
            "success" : false
          }
        }
      ],
      "raw_report" : "Installation failed on one or more ECUs",
      "result" :
      {
        "code" : "primary_hw:INSTALL_FAILED|secondary_hw:INSTALL_FAILED",
        "description" : "Installation failed on one or more ECUs",
        "success" : false
      }
    }
  })");
  EXPECT_EQ(expected_report, report);

  EXPECT_EQ(s.secondary->install_calls, 0);
}

/**
 * When a target is only for a secondary that has needsImageFileOnPrimary() false
 * (e.g. handler-download generic secondary), the primary should not fetch the
 * image and downloadImages should still return success.
 */
TEST(UptaneUpdateFailure, NeedTargetFileOnPrimarySkipsFetchForHandlerDownloadSecondary) {
  TestScaffolding s;  // NOLINT

  EXPECT_NO_THROW(s.dut->initialize());
  result::UpdateCheck const update_result = s.dut->fetchMeta();
  EXPECT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  ASSERT_FALSE(update_result.updates.empty());

  // Use the secondary target from Director metadata (hasupdates has secondary_firmware.txt).
  Uptane::EcuSerial const secondary_serial("secondary_ecu_serial");
  auto const it = std::find_if(update_result.updates.cbegin(), update_result.updates.cend(),
                               [&secondary_serial](const Uptane::Target &t) { return t.IsForEcu(secondary_serial); });
  ASSERT_NE(it, update_result.updates.cend()) << "No target for secondary in Director metadata";
  Uptane::Target const secondary_target = *it;

  s.secondary->needs_image_file_on_primary_ = false;

  result::Download download_result = s.dut->downloadImages({secondary_target});
  EXPECT_EQ(download_result.status, result::DownloadStatus::kSuccess);
  ASSERT_EQ(download_result.updates.size(), 1u);
  EXPECT_EQ(download_result.updates[0].filename(), secondary_target.filename());
}

#ifndef __NO_MAIN__
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  return RUN_ALL_TESTS();
}
#endif
