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

#include <algorithm>
#include <atomic>
#include <boost/filesystem.hpp>
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
                           std::any_of(extra_headers->begin(), extra_headers->end(),
                                       [](const std::string &header) { return header == "x-trx-mark-seen: false"; });
      if (is_peek) {
        peek_requests_++;
      } else {
        commit_requests_++;
        if (fail_next_commits_ > 0) {
          fail_next_commits_--;
          return HttpResponse({}, 503, CURLE_OK, "");
        }
      }
    }
    return HttpFake::get(url, maxsize, flow_control, extra_headers);
  }

  int peek_requests() const { return peek_requests_; }
  int commit_requests() const { return commit_requests_; }

  /** Next N non-peek director targets fetches return 503 (transient). */
  void FailNextCommits(int n) { fail_next_commits_ = n; }

 private:
  std::atomic<int> peek_requests_{0};
  std::atomic<int> commit_requests_{0};
  std::atomic<int> fail_next_commits_{0};
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

  /**
   * Re-sign the director targets with a new correlation ID and a different
   * target, simulating the server superseding the offered update. Reuses the
   * keys already on disk, so the root of trust is unchanged.
   */
  void SupersedeUpdate(const std::string &new_correlation_id) {
    UptaneRepo repo_b{uptane_metadata_dir_, "2029-07-04T16:33:27Z", new_correlation_id};
    const std::string hwid = "primary_hw";
    auto firmware_path = uptane_metadata_dir_ / "targets/superseding_firmware.txt";
    Utils::writeFile(firmware_path, std::string("newer firmware"));
    repo_b.addImage(firmware_path, "superseding_firmware.txt", hwid);
    repo_b.emptyTargets();
    repo_b.addTarget("superseding_firmware.txt", hwid, "CA:FE:A6:D2:84:9D");
    repo_b.signTargets();
  }

  /**
   * Empty the director targets, simulating the server withdrawing the update.
   */
  void WithdrawUpdate() {
    UptaneRepo repo_w{uptane_metadata_dir_, "2029-07-04T16:33:27Z", "withdrawn"};
    repo_w.emptyTargets();
    repo_w.signTargets();
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

  std::future<Outcome> GetConsent(const std::vector<Uptane::Target> &targets,
                                  const std::string &correlation_id) override {
    *targets_ = TargetsToJson(targets);
    std::promise<Outcome> p;
    p.set_value({Outcome::Result::kRefused, "Rejected in MockConsent", correlation_id});
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

  std::future<Outcome> GetConsent(const std::vector<Uptane::Target> & /* targets */,
                                  const std::string &correlation_id) override {
    get_consent_called_ = true;
    std::promise<Outcome> p;
    switch (action_) {
      case Action::kGrant:
        p.set_value({Outcome::Result::kGranted, "Granted in test", correlation_id});
        break;
      case Action::kRefuse:
        p.set_value({Outcome::Result::kRefused, "Refused in test", correlation_id});
        break;
      case Action::kCancel:
        p.set_value({Outcome::Result::kCancelled, "Cancelled in test", correlation_id});
        break;
      default:
        p.set_value({Outcome::Result::kCancelled, "Unknown action in test", correlation_id});
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
 * Consent implementation whose future stays pending until the test resolves
 * it, mimicking a real user thinking about the prompt. Mirrors the promise
 * lifecycle contract of Dbus::GetConsent.
 */
class PendingConsent : public Consent {
 public:
  std::future<Outcome> GetConsent(const std::vector<Uptane::Target> & /* targets */,
                                  const std::string &correlation_id) override {
    std::lock_guard<std::mutex> guard{m_};
    if (pending_) {
      // Same contract as Dbus::GetConsent: the old request is superseded
      promise_.set_value({Outcome::Result::kSuperseded, "Superseded by newer update", correlation_id_});
    }
    promise_ = std::promise<Outcome>{};
    pending_ = true;
    correlation_id_ = correlation_id;
    get_consent_calls_++;
    cv_.notify_all();
    return promise_.get_future();
  }

  void PendingUpdateCancelled() override {
    std::lock_guard<std::mutex> guard{m_};
    if (pending_) {
      promise_.set_value({Outcome::Result::kCancelled, "Cancelled", correlation_id_});
      pending_ = false;
      cancel_calls_++;
      cv_.notify_all();
    }
  }

  /** Resolve the pending request, as if the user answered the prompt. */
  void Respond(bool granted, const std::string &reason) {
    std::lock_guard<std::mutex> guard{m_};
    if (!pending_) {
      throw std::runtime_error("PendingConsent::Respond called with no pending request");
    }
    promise_.set_value({granted ? Outcome::Result::kGranted : Outcome::Result::kRefused, reason, correlation_id_});
    pending_ = false;
  }

  /** Wait until GetConsent has been called at least n times. */
  bool WaitForGetConsentCalls(int n, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock{m_};
    return cv_.wait_for(lock, timeout, [&] { return get_consent_calls_ >= n; });
  }

  /** Wait until the pending request has been cancelled (offer withdrawn). */
  bool WaitForCancel(std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock{m_};
    return cv_.wait_for(lock, timeout, [&] { return cancel_calls_ >= 1; });
  }

  int GetConsentCalls() {
    std::lock_guard<std::mutex> guard{m_};
    return get_consent_calls_;
  }

  std::string CurrentCorrelationId() {
    std::lock_guard<std::mutex> guard{m_};
    return correlation_id_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::promise<Outcome> promise_;
  bool pending_{false};
  std::string correlation_id_;
  int get_consent_calls_{0};
  int cancel_calls_{0};
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
  EXPECT_GE(http->commit_requests(), 1) << "Commit request should have been made even though consent was refused";

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
  EXPECT_EQ(http->commit_requests(), 0) << "No commit fetch should be made when consent is cancelled";
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

namespace {

/** Poll pred() every 100ms until it returns true or timeout expires. */
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::seconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return true;
}

int CountEvents(const std::vector<std::string> &events, const std::string &name) {
  return static_cast<int>(std::count(events.begin(), events.end(), name));
}

}  // namespace

/**
 * While waiting for consent, the device keeps peek-polling. If the offer is
 * unchanged (same correlation ID), it must not re-prompt or re-send
 * AwaitingConsent.
 */
TEST_F(AktualizrConsent, UnchangedOfferDoesNotReprompt) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 1;
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  auto consent = std::make_unique<PendingConsent>();
  auto *consent_ptr = consent.get();
  dut.SetConsent(std::move(consent));
  dut.Initialize();
  auto fut = dut.RunForever();

  ASSERT_TRUE(consent_ptr->WaitForGetConsentCalls(1, std::chrono::seconds(30))) << "Should prompt for consent";
  EXPECT_EQ(consent_ptr->CurrentCorrelationId(), "id0");

  // The device must keep peek-polling while the prompt is pending
  int const peeks_at_prompt = http->peek_requests();
  ASSERT_TRUE(WaitFor([&] { return http->peek_requests() >= peeks_at_prompt + 2; }, std::chrono::seconds(30)))
      << "Should keep peek-polling while waiting for consent";

  EXPECT_EQ(consent_ptr->GetConsentCalls(), 1) << "An unchanged offer must not re-prompt";
  ASSERT_TRUE(
      WaitFor([&] { return CountEvents(http->report_events(), "AwaitingConsent") >= 1; }, std::chrono::seconds(10)));
  EXPECT_EQ(CountEvents(http->report_events(), "AwaitingConsent"), 1)
      << "An unchanged offer must not re-send AwaitingConsent";
  EXPECT_EQ(http->commit_requests(), 0) << "No commit fetch before the user decides";

  // Granting still works after several peeks
  consent_ptr->Respond(true, "ok");
  ASSERT_TRUE(WaitFor([&] { return http->commit_requests() >= 1; }, std::chrono::seconds(30)))
      << "Grant after peeks should lead to a commit fetch";

  dut.Shutdown();
  fut.wait();
}

/**
 * If a peek during consent finds a different correlation ID, the pending
 * offer is superseded: GetConsent is called again with the new targets and a
 * fresh AwaitingConsent is sent. Granting the new offer installs it.
 */
TEST_F(AktualizrConsent, SupersededOfferRepromptsAndInstalls) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 1;
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  auto consent = std::make_unique<PendingConsent>();
  auto *consent_ptr = consent.get();
  dut.SetConsent(std::move(consent));
  dut.Initialize();
  auto fut = dut.RunForever();

  ASSERT_TRUE(consent_ptr->WaitForGetConsentCalls(1, std::chrono::seconds(30))) << "Should prompt for consent";
  EXPECT_EQ(consent_ptr->CurrentCorrelationId(), "id0");

  // Deploy a newer update on the server while the prompt is pending
  SupersedeUpdate("id1");

  ASSERT_TRUE(consent_ptr->WaitForGetConsentCalls(2, std::chrono::seconds(30))) << "A changed offer should re-prompt";
  EXPECT_EQ(consent_ptr->CurrentCorrelationId(), "id1");
  ASSERT_TRUE(
      WaitFor([&] { return CountEvents(http->report_events(), "AwaitingConsent") >= 2; }, std::chrono::seconds(10)))
      << "Supersession should re-send AwaitingConsent";
  EXPECT_EQ(http->commit_requests(), 0) << "No commit fetch before the user decides";
  EXPECT_EQ(CountEvents(http->report_events(), "ConsentOutcome"), 0) << "No ConsentOutcome for the superseded offer";

  // Grant the new offer; the update should proceed to download
  consent_ptr->Respond(true, "yes to the new one");
  ASSERT_TRUE(WaitFor([&] { return http->commit_requests() >= 1; }, std::chrono::seconds(30)));
  ASSERT_TRUE(
      WaitFor([&] { return CountEvents(http->report_events(), "EcuDownloadStarted") >= 1; }, std::chrono::seconds(30)))
      << "Granting the superseding offer should download it";
  EXPECT_EQ(CountEvents(http->report_events(), "ConsentOutcome"), 1)
      << "Exactly one ConsentOutcome, for the offer the user actually answered";

  dut.Shutdown();
  fut.wait();
}

/**
 * If a peek during consent finds no updates at all, the offer was withdrawn
 * on the server: the pending consent request is cancelled, no ConsentOutcome
 * is sent, and the device returns to idle (and keeps polling).
 */
TEST_F(AktualizrConsent, WithdrawnOfferCancelsConsent) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 1;
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  auto consent = std::make_unique<PendingConsent>();
  auto *consent_ptr = consent.get();
  dut.SetConsent(std::move(consent));
  dut.Initialize();
  auto fut = dut.RunForever();

  ASSERT_TRUE(consent_ptr->WaitForGetConsentCalls(1, std::chrono::seconds(30))) << "Should prompt for consent";

  // Retract the update on the server
  WithdrawUpdate();

  ASSERT_TRUE(consent_ptr->WaitForCancel(std::chrono::seconds(30)))
      << "A withdrawn offer should cancel the pending consent request";

  // The device should be idle again and keep polling
  int const peeks_after_withdraw = http->peek_requests();
  ASSERT_TRUE(WaitFor([&] { return http->peek_requests() >= peeks_after_withdraw + 2; }, std::chrono::seconds(30)))
      << "Should return to idle polling after the offer is withdrawn";

  EXPECT_EQ(http->commit_requests(), 0) << "A withdrawn offer must not be committed";
  EXPECT_EQ(CountEvents(http->report_events(), "ConsentOutcome"), 0)
      << "No ConsentOutcome for an offer the user never decided on";
  EXPECT_EQ(consent_ptr->GetConsentCalls(), 1) << "No re-prompt after withdrawal";

  dut.Shutdown();
  fut.wait();
}

/**
 * After consent is granted, a transient network error on the commit fetch must
 * not burn the campaign: the device retries and proceeds once connectivity
 * recovers. This mirrors a user answering Consent while the device is offline.
 */
TEST_F(AktualizrConsent, TransientCommitFailureRetriesAndProceeds) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 1;
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  auto consent = std::make_unique<PendingConsent>();
  auto *consent_ptr = consent.get();
  dut.SetConsent(std::move(consent));
  dut.Initialize();
  auto fut = dut.RunForever();

  ASSERT_TRUE(consent_ptr->WaitForGetConsentCalls(1, std::chrono::seconds(30)));
  http->FailNextCommits(2);
  consent_ptr->Respond(true, "granted while offline");

  ASSERT_TRUE(WaitFor([&] { return http->commit_requests() >= 3; }, std::chrono::seconds(30)))
      << "Should retry the commit fetch after transient failures";
  ASSERT_TRUE(
      WaitFor([&] { return CountEvents(http->report_events(), "EcuDownloadStarted") >= 1; }, std::chrono::seconds(30)))
      << "Should proceed to download once a commit fetch succeeds";

  data::InstallationResult ir;
  std::string report;
  std::string correlation_id;
  EXPECT_FALSE(storage->loadDeviceInstallationResult(&ir, &report, &correlation_id))
      << "Transient commit failures must not store an installation failure";

  dut.Shutdown();
  fut.wait();
}

/**
 * If the server's offer changes between the consented peek and the commit
 * fetch (without a superseding peek having updated the prompt), that is a
 * permanent failure for the consented campaign — not a transient retry.
 */
TEST_F(AktualizrConsent, CorrelationMismatchAfterConsentFailsPermanently) {  // NOLINT
  auto http = std::make_shared<HttpFakePeek>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  // Long poll so no peek runs between superseding on disk and the grant.
  conf.uptane.polling_sec = 600;
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  UptaneTestCommon::TestAktualizr dut(conf, storage, http);

  auto consent = std::make_unique<PendingConsent>();
  auto *consent_ptr = consent.get();
  dut.SetConsent(std::move(consent));
  dut.Initialize();
  auto fut = dut.RunForever();

  ASSERT_TRUE(consent_ptr->WaitForGetConsentCalls(1, std::chrono::seconds(30)));
  EXPECT_EQ(consent_ptr->CurrentCorrelationId(), "id0");

  SupersedeUpdate("id1");
  consent_ptr->Respond(true, "granted stale offer");

  ASSERT_TRUE(WaitFor([&] { return http->commit_requests() >= 1; }, std::chrono::seconds(30)))
      << "Commit fetch should run after consent";
  // SendManifest clears the stored failure after uploading it; observe the
  // failure via the manifest PUT rather than racing the DB clear.
  ASSERT_TRUE(WaitFor(
      [&] {
        return http->last_manifest.isMember("signed") &&
               http->last_manifest["signed"].isMember("installation_report") &&
               http->last_manifest["signed"]["installation_report"]["report"]["correlation_id"].asString() == "id0";
      },
      std::chrono::seconds(30)))
      << "Correlation mismatch should send a failure manifest for the consented ID";

  // Give any mistaken retry path a moment; a permanent failure must not keep committing.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  EXPECT_EQ(http->commit_requests(), 1) << "Permanent mismatch must not retry the commit fetch";
  EXPECT_EQ(CountEvents(http->report_events(), "EcuDownloadStarted"), 0);

  dut.Shutdown();
  fut.wait();
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // NOLINTNEXTLINE
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
