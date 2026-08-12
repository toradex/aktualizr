/**
 * \file
 * Tests for Aktualizr's Consent operation that are independent of D-Bus
 */

#include <gtest/gtest.h>
#include <json/json.h>
#include <libaktualizr/config.h>
#include <libaktualizr/events.h>
#include <libaktualizr/types.h>
#include <logging/logging.h>
#include <primary/consent.h>
#include <utilities/utils.h>

#include "httpfake.h"
#include "libaktualizr/aktualizr.h"
#include "metafake.h"
#include "uptane_repo.h"
#include "uptane_test_common.h"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = boost::filesystem;

/**
 * HttpFake subclass that tracks whether director targets requests are made
 * in peek mode (with x-trx-mark-seen header) or commit mode (without the header).
 */
class HttpFakePeek : public HttpFake {
 public:
  using HttpFake::HttpFake;

  HttpResponse get(const std::string &url, int64_t maxsize, const api::FlowControlToken *flow_control,
                   const Headers *extra_headers) override {
    if (url.find("/director/targets.json") != std::string::npos) {
      const bool is_peek = extra_headers != nullptr &&
                           std::any_of(extra_headers->begin(), extra_headers->end(), [](const std::string &header) {
                             return header == "x-trx-mark-seen: false";
                           });
      if (is_peek) {
        peek_requests_++;
      } else {
        commit_requests_++;
      }
    }
    return HttpFake::get(url, maxsize, flow_control, extra_headers);
  }

  int peek_requests() const { return peek_requests_; }
  int commit_requests() const { return commit_requests_; }

 private:
  int peek_requests_{0};
  int commit_requests_{0};
};

/**
 * Testing base class that sets up Uptane metadata with custom fields.
 */
class AktualizrConsent : public testing::Test {
 public:
  AktualizrConsent(AktualizrConsent &&) = delete;
  AktualizrConsent(const AktualizrConsent &) = delete;
  AktualizrConsent &operator=(const AktualizrConsent &) = delete;
  AktualizrConsent &operator=(AktualizrConsent &&) = delete;
  ~AktualizrConsent() override = default;

 protected:
  AktualizrConsent()
      : uptane_metadata_dir_{temp_dir_.Path() / "uptane"},
        aktualizr_dir_{temp_dir_ / "aktualizr"},
        repo_{uptane_metadata_dir_, "2029-07-04T16:33:27Z", "id0"} {
    // Build a simple uptane repo
    repo_.generateRepo(KeyType::kED25519);
    const std::string hwid = "primary_hw";
    auto firmware_path = uptane_metadata_dir_ / "targets/primary_firmware.txt";
    Utils::writeFile(firmware_path, std::string("test firmware"));
    Json::Value image_custom;
    image_custom["foo"] = "bar";
    repo_.addImage(firmware_path, "primary_firmware.txt", hwid, "", 0, {}, image_custom);
    repo_.addTarget("primary_firmware.txt", hwid, "CA:FE:A6:D2:84:9D", "http://customurl/primary.txt");
    repo_.signTargets();
  }

  TemporaryDirectory temp_dir_;
  fs::path uptane_metadata_dir_;
  fs::path aktualizr_dir_;
  UptaneRepo repo_;
};

/**
 * Mock Consent implementation that records the list of targets and rejects everything.
 */
class MockConsent : public Consent {
 public:
  explicit MockConsent(Json::Value *targets) : targets_{targets} { assert(targets_); }

  std::future<Outcome> GetConsent(const std::vector<Uptane::Target> &targets) override {
    *targets_ = TargetsToJson(targets);
    std::promise<Outcome> p;
    p.set_value({false, false, "Rejected in MockConsent"});
    return p.get_future();
  }

  void PendingUpdateCancelled() override {}

 private:
  Json::Value *targets_;
};

/**
 * Configurable mock consent for two-phase tests.
 */
class ConfigurableConsent : public Consent {
 public:
  enum class Action { kGrant, kRefuse, kCancel };

  explicit ConfigurableConsent(Action action) : action_(action) {}

  std::future<Outcome> GetConsent(const std::vector<Uptane::Target> & /* targets */) override {
    get_consent_called_ = true;
    std::promise<Outcome> p;
    switch (action_) {
      case Action::kGrant:
        p.set_value({true, false, "Granted in test"});
        break;
      case Action::kRefuse:
        p.set_value({false, false, "Refused in test"});
        break;
      case Action::kCancel:
        p.set_value({false, true, "Cancelled in test"});
        break;
      default:
        p.set_value({false, true, "Unknown action in test"});
        break;
    }
    return p.get_future();
  }

  void PendingUpdateCancelled() override {}

  bool getConsentCalled() const { return get_consent_called_; }

 private:
  Action action_;
  bool get_consent_called_{false};
};

/**
 * ConsentRequired should carry Director targets with Image-repo custom fields
 * merged in (director wins on conflicts; IMAGE_REPO_MERGE_IGNORE excluded).
 */
TEST_F(AktualizrConsent, CorrectConsentInformation) {  // NOLINT
  // Setup
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  Json::Value consent_blob;  // MockConsent will drop the json here
  dut.SetConsent(std::make_unique<MockConsent>(&consent_blob));

  // Perform an update cycle
  dut.Initialize();
  dut.UptaneCycle();

  EXPECT_FALSE(consent_blob.empty()) << "Should have got a set of targets to consent to";
  auto director = Utils::parseJSONFile(uptane_metadata_dir_ / "repo/director/targets.json");
  auto image = Utils::parseJSONFile(uptane_metadata_dir_ / "repo/repo/targets.json");

  auto expected_content = director["signed"];
  expected_content.removeMember("expires");
  expected_content.removeMember("version");
  expected_content.removeMember("custom");

  // Mirror checkUpdates()'s MergeJson(..., IMAGE_REPO_MERGE_IGNORE).
  static const std::vector<std::string> image_repo_merge_ignore{"hardwareIds", "targetFormat", "uri"};
  for (const auto &name : expected_content["targets"].getMemberNames()) {
    ASSERT_TRUE(image["signed"]["targets"].isMember(name)) << "Image repo missing " << name;
    expected_content["targets"][name]["custom"] =
        utils::MergeJson(expected_content["targets"][name]["custom"], image["signed"]["targets"][name]["custom"],
                         &image_repo_merge_ignore);
  }

  EXPECT_EQ(consent_blob, expected_content)
      << "The consent blob should match Director targets with Image-repo custom merged in";
  EXPECT_EQ(consent_blob["targets"]["primary_firmware.txt"]["custom"]["foo"], "bar")
      << "Image-repo custom fields must appear in the consent prompt";
}

/**
 * The initial update check should use peek mode and the commit fetch after
 * consent should be a normal Director targets fetch.
 */
TEST_F(AktualizrConsent, PeekParameterSentWhenConsentRequired) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  dut.SetConsent(std::make_unique<ConfigurableConsent>(ConfigurableConsent::Action::kGrant));
  dut.Initialize();
  dut.UptaneCycle();

  EXPECT_GE(http->peek_requests(), 1) << "Should have at least one peek request";
  EXPECT_GE(http->commit_requests(), 1) << "Should have at least one commit request";
}

/**
 * When consent is granted, the commit fetch should succeed and the update
 * should proceed to download. Both peek and commit fetches should occur.
 */
TEST_F(AktualizrConsent, CommitFetchSucceedsOnConsentGranted) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  dut.SetConsent(std::make_unique<ConfigurableConsent>(ConfigurableConsent::Action::kGrant));
  dut.Initialize();
  dut.UptaneCycle();

  EXPECT_GE(http->peek_requests(), 1);
  EXPECT_GE(http->commit_requests(), 1);

  auto events = http->report_events();
  bool found_download = false;
  for (const auto &e : events) {
    if (e == "EcuDownloadStarted") {
      found_download = true;
    }
  }
  EXPECT_TRUE(found_download) << "Update should have proceeded to download after consent granted";
}

/**
 * When consent is refused, the commit fetch should still happen (to mark
 * the update in progress on the server), then a kConsentRefused failure
 * should be stored and manifest sent.
 */
TEST_F(AktualizrConsent, ConsentRefusedTriggersCommitAndFailure) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  dut.SetConsent(std::make_unique<ConfigurableConsent>(ConfigurableConsent::Action::kRefuse));
  dut.Initialize();
  dut.UptaneCycle();

  EXPECT_GE(http->peek_requests(), 1) << "Peek request should have been made";
  EXPECT_GE(http->commit_requests(), 1)
      << "Commit request should have been made even though consent was refused";

  // Check that no download was started (consent was refused)
  auto events = http->report_events();
  bool found_download = false;
  for (const auto &e : events) {
    if (e == "EcuDownloadStarted") {
      found_download = true;
    }
  }
  EXPECT_FALSE(found_download) << "Should NOT have downloaded after consent was refused";
}

/**
 * When consent is cancelled (e.g., preempted by offline update), no commit
 * fetch should be made, leaving the update cancellable on the server.
 */
TEST_F(AktualizrConsent, ConsentCancelledNoCommitFetch) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  dut.SetConsent(std::make_unique<ConfigurableConsent>(ConfigurableConsent::Action::kCancel));
  dut.Initialize();
  dut.UptaneCycle();

  EXPECT_GE(http->peek_requests(), 1) << "Peek request should have been made";
  // In cancelled case, the state machine goes directly to kIdle without commit
  // The commit_requests count should be 0 since no commit fetch is made.
  // (There may be a root rotation fetch that hits /director/root.json, but not
  // /director/targets.json without the query parameter.)
  EXPECT_EQ(http->commit_requests(), 0)
      << "No commit fetch should be made when consent is cancelled";
}

/**
 * When InstallUpdatesAutomatically is kProceed (TrivialConsent), we still use
 * the unified peek -> consent -> commit flow. Consent is auto-granted.
 */
TEST_F(AktualizrConsent, AutoInstallUsesPeekAndCommit) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  auto storage = INvStorage::newStorage(conf.storage);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  // Default TrivialConsent auto-grants consent immediately.
  dut.Initialize();
  dut.UptaneCycle();

  EXPECT_GE(http->peek_requests(), 1) << "Peek requests should be made in auto-install mode";
  EXPECT_GE(http->commit_requests(), 1) << "Commit requests should also be made in auto-install mode";
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // NOLINTNEXTLINE
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
