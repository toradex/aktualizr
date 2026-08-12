/**
 * \file
 * Tests for D-Bus functionality.
 *
 * When debugging these tests, running 'dbus-monitor' in a separate terminal is useful.
 * Also 'd-feet' is a useful visual inspector
 */

#include <gtest/gtest.h>
#include <libaktualizr/config.h>
#include <libaktualizr/events.h>
#include <libaktualizr/types.h>
#include <logging/logging.h>
#include <primary/consent.h>
#include <primary/dbus.h>
#include <utilities/utils.h>

#include "httpfake.h"
#include "libaktualizr/aktualizr.h"
#include "metafake.h"
#include "uptane_test_common.h"

#include <systemd/sd-bus.h>
#include <boost/filesystem.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

using std::future_status;
using namespace std::chrono_literals;

boost::filesystem::path uptane_repos_dir;  // NOLINT
boost::filesystem::path fake_meta_dir;     // NOLINT

/**
 * Base class to implement tests that require communication over D-Bus.
 * It provides 2 connections to the bus:
 *  dut_bus_ that should be passed to the component under test
 *  client_bus_ that can be used to send commands to the DUT
 *
 * It uses the default bus (which will be the desktop user bus during interactive
 * development). This means that the DUT must not take a well-known name on the
 * bus, instead the client should find and connect to it using bus_name_. This
 * is critical to allow tests to run in parallel. This approach was chosen over
 * others (such as spawning a D-Bus daemon for each test) to keep the system
 * simple and fast.
 */
class AktualizrDbus : public testing::Test {
 public:
  AktualizrDbus(AktualizrDbus&&) = delete;
  AktualizrDbus(const AktualizrDbus&) = delete;
  AktualizrDbus& operator=(const AktualizrDbus&) = delete;
  AktualizrDbus& operator=(AktualizrDbus&&) = delete;
  ~AktualizrDbus() override { sd_bus_unref(client_bus_); }

 protected:
  AktualizrDbus() {
    int err = sd_bus_open(&dut_bus_.ptr);
    if (err < 0) {
      throw std::system_error(-err, std::system_category(), "Failed to open dut bus");
    }
    sd_bus_get_unique_name(dut_bus_.ptr, &bus_name_);

    err = sd_bus_open(&client_bus_);  // Must be 'open' not 'default' to get a
                                      // separate connection
    if (err < 0) {
      throw std::system_error(-err, std::system_category(), "Failed to open client bus");
    }
  }

  SdBus dut_bus_;
  sd_bus* client_bus_{nullptr};
  const char* bus_name_{nullptr};
  TemporaryDirectory temp_dir_;
};

/**
 * Trigger a check for updates from a D-Bus client and check it causes
 * \aktualizr to skip the polling interval. Correct operation is checked by
 * watching the Events that are emitted through Aktualizr::SetSignalHandler().
 */
TEST_F(AktualizrDbus, CheckForUpdates) {
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "noupdates", fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 600;

  auto storage = INvStorage::newStorage(conf.storage);
  UptaneTestCommon::TestAktualizr aktualizr(conf, storage, http);

  aktualizr.SetDbusInterface(std::move(dut_bus_));

  struct CheckEvents {
    std::mutex m;
    std::condition_variable cv;
    int update_checks{0};

    void HandleEvent(const std::shared_ptr<event::BaseEvent>& event) {
      if (event->variant == "UpdateCheckComplete") {
        std::lock_guard<std::mutex> guard{m};
        update_checks++;
        cv.notify_all();
      }
    }
  };

  CheckEvents check_events;
  auto conn = aktualizr.SetSignalHandler(std::bind(&CheckEvents::HandleEvent, &check_events, std::placeholders::_1));

  aktualizr.Initialize();
  auto ak_future = aktualizr.RunForever();

  {
    std::unique_lock guard{check_events.m};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (check_events.update_checks < 1) {
      if (check_events.cv.wait_until(guard, deadline) == std::cv_status::timeout) {
        FAIL() << "Timed out waiting for update_check";
      }
    }
  }
  LOG_INFO << "First update check successful. Triggering a 2nd via D-Bus";

  // Give time to get back the the Idle state
  std::this_thread::sleep_for(std::chrono::seconds(2));

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::CheckForUpdates, &ret_error,
                               &reply, "");
  ASSERT_GE(res, 0) << "Call failed";

  {
    std::unique_lock guard{check_events.m};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (check_events.update_checks < 2) {
      if (check_events.cv.wait_until(guard, deadline) == std::cv_status::timeout) {
        aktualizr.Shutdown();
        ASSERT_TRUE(false) << "Timed out waiting for second update_check";
        return;
      }
    }
  }

  aktualizr.Shutdown();
  ak_future.wait();

  EXPECT_EQ(http->put_urls().back(), "/director/manifest?reason=dbus-wake");
}

namespace {
int bus_signal_callback(sd_bus_message* /* *m */, void* userdata, sd_bus_error* /* ret error*/) {
  int* counter = static_cast<int*>(userdata);
  LOG_DEBUG << "Got property change signal";
  (*counter)++;
  return 0;
}
}  // namespace

TEST_F(AktualizrDbus, ConsentRejected) {
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "hasupdates", fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 600;

  // Require consent
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);

  UptaneTestCommon::TestAktualizr aktualizr(conf, storage, http);

  aktualizr.SetDbusInterface(std::move(dut_bus_));

  aktualizr.Initialize();
  auto ak_future = aktualizr.RunForever();

  // Wait up to 20s for the consent property to change
  int counter = 0;
  int res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;

  // pump the system bus to wait for the Consent Property to changed
  for (int i = 0; (counter == 0) && (i < 200); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(100'000);
  }
  EXPECT_EQ(counter, 1) << "Should have got a notification that the signal has changed";

  // Read the pending request to learn its correlationId
  sd_bus_error err = SD_BUS_ERROR_NULL;
  char* property_value = nullptr;
  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";
  Json::Value consent_json = Utils::parseJSON(property_value);
  std::string correlation_id = consent_json["correlationId"].asString();
  EXPECT_FALSE(correlation_id.empty()) << "ConsentRequired should carry a correlationId";

  // Reject the install
  http->last_manifest.clear();
  sd_bus_message* reply = nullptr;
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 0,
                           "I refuse.", correlation_id.c_str());
  ASSERT_GE(res, 0) << "Call failed";
  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);

  // Wait for the manifest to come back
  for (int i = 0; i < 200 && http->last_manifest.empty(); i++) {
    usleep(100'000);
  }

  EXPECT_FALSE(http->last_manifest.empty()) << "No Manifest reported after install rejected";

  LOG_INFO << "Manifest was:" << http->last_manifest;

  // Check the returned manifest
  Json::Value installation_report = http->last_manifest["signed"]["installation_report"]["report"];
  EXPECT_EQ(installation_report["result"]["code"].asString(), "CONSENT_REFUSED")
      << "The manifest should contain a failure";
  EXPECT_FALSE(installation_report["result"]["success"].asBool())
      << "The overall success of the installation should be a failure";

  int found = 0;
  for (const auto& event : http->report_events()) {
    if (event == "ConsentOutcome") {
      found++;
    }
  }

  EXPECT_EQ(found, 1) << "Should have got a ConsentOutcomeEvent";

  aktualizr.Shutdown();
  ak_future.wait();
}

TEST_F(AktualizrDbus, CancelAtConsentPoint) {
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "hasupdates", fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 600;

  // Require consent
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);

  UptaneTestCommon::TestAktualizr aktualizr(conf, storage, http);

  aktualizr.SetDbusInterface(std::move(dut_bus_));

  aktualizr.Initialize();
  auto ak_future = aktualizr.RunForever();

  // Wait up to 20s for the consent property to change
  int counter = 0;
  int res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;

  // pump the system bus to wait for the Consent Property to changed
  for (int i = 0; (counter == 0) && (i < 200); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(100'000);
  }
  EXPECT_EQ(counter, 1) << "Should have got a notification that the signal has changed";

  // Cancel the update here
  http->last_manifest.clear();
  sd_bus_message* reply = nullptr;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Cancel, &err, &reply, "");
  ASSERT_GE(res, 0) << "Call failed";
  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);

  // Note: It isn't clear whether the 'right' behaviour here is to return a
  // manifest or not. Right now we do send one if we cancel here, so check for
  // it.

  // Wait for the manifest to come back
  for (int i = 0; i < 200 && http->last_manifest.empty(); i++) {
    usleep(100'000);
  }

  EXPECT_FALSE(http->last_manifest.empty()) << "No Manifest reported after install cancelled";

  // Check the returned manifest
  Json::Value installation_report = http->last_manifest["signed"]["installation_report"]["report"];
  EXPECT_EQ(installation_report["result"]["code"].asString(), "OPERATION_CANCELLED")
      << "The manifest should contain a failure";
  EXPECT_FALSE(installation_report["result"]["success"].asBool())
      << "The overall success of the installation should be a failure";

  aktualizr.Shutdown();
  EXPECT_EQ(ak_future.wait_for(100s), future_status::ready) << "Shutdown failed";
}

/**
 * Validate the D-Bus interface (\ref Dbus) can receive a CheckForUpdates call
 */
TEST_F(AktualizrDbus, DbusCheckForUpdates) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  Dbus dut(std::move(dut_bus_), storage);

  std::atomic<int> taps = 0;

  dut.SetCheckForUpdatesCallback([&] { taps++; });

  EXPECT_EQ(taps, 0);

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::CheckForUpdates, &ret_error,
                               &reply, "");
  ASSERT_GE(res, 0) << "Call failed";

  // int64_t c;
  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);
  ASSERT_EQ(res, 0);
  EXPECT_EQ(taps, 1);
}

/**
 * Validate the D-Bus interface can receive a Cancel call
 */
TEST_F(AktualizrDbus, DbusCancel) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  Dbus dut(std::move(dut_bus_), storage);

  std::atomic<int> cancels = 0;

  dut.SetCancelCallback([&] { cancels++; });

  EXPECT_EQ(cancels, 0);

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int res =
      sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Cancel, &ret_error, &reply, "");
  ASSERT_GE(res, 0) << "Call failed";

  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);
  ASSERT_EQ(res, 0);
  EXPECT_EQ(cancels, 1);
}

/**
 * Validate the D-Bus interface can receive an offline update call
 */
TEST_F(AktualizrDbus, DbusOfflineUpdate) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  Dbus dut(std::move(dut_bus_), storage);

  std::atomic<int> calls = 0;

  dut.SetOfflineUpdateCallback([&](const boost::filesystem::path& /*path*/) {
    // LOG_INFO << "OfflineUpdate path is " << path;
    calls++;
  });

  EXPECT_EQ(calls, 0);

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;

  // Try with an invalid path
  int res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::OfflineUpdate, &ret_error,
                               &reply, "s", "");
  EXPECT_EQ(res, -22) << "Empty path should fail";
  sd_bus_error_free(&ret_error);

  // try with a relative path
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::OfflineUpdate, &ret_error, &reply,
                           "s", "asdf");
  EXPECT_EQ(res, -22) << "non-absolute path should fail";
  sd_bus_error_free(&ret_error);

  // try with an unreadable path
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::OfflineUpdate, &ret_error, &reply,
                           "s", "/root/.ssh");
  // We expect this to not throw.
  // EXPECT_EQ(res, -22) << "should fail";
  sd_bus_error_free(&ret_error);

  TemporaryDirectory tmp_dir;  // Only to get a valid directory path
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::OfflineUpdate, &ret_error, &reply,
                           "s", tmp_dir.Path().c_str());
  ASSERT_GE(res, 0) << "Call failed " << ret_error.message;
  sd_bus_error_free(&ret_error);

  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);

  ASSERT_EQ(res, 0);
  EXPECT_EQ(calls, 1);
}

/**
 * By default, user consent is not requested for updates
 */
TEST_F(AktualizrDbus, DefaultIsNoConsent) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  Dbus dut(std::move(dut_bus_), storage);

  std::vector<Uptane::Target> install_targets;
  install_targets.push_back(Uptane::Target::Unknown());
  auto consent_response = dut.GetConsent(install_targets, "corr-id-default");

  using namespace std::chrono_literals;
  ASSERT_EQ(consent_response.wait_for(1ms), std::future_status::ready) << "Consent result should be immediate";

  EXPECT_TRUE(consent_response.get().granted()) << "Consent should be granted by default";
}

TEST_F(AktualizrDbus, ControlConsentOverDbus) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  Dbus dut(std::move(dut_bus_), storage);

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  int res = sd_bus_set_property(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::InstallUpdatesAutomatically,
                                &ret_error, "i", static_cast<int>(InstallUpdatesAutomatically::kAsk));
  ASSERT_GE(res, 0) << "Set property failed";

  int required = 0;
  res = sd_bus_get_property_trivial(client_bus_, bus_name_, Dbus::Path, Dbus::Interface,
                                    Dbus::InstallUpdatesAutomatically, &ret_error, 'i', &required);
  ASSERT_GE(res, 0) << "Get property failed";

  EXPECT_EQ(required, 1);
  InstallUpdatesAutomatically in_db;
  bool ok = storage->loadInstallUpdatesAutomatically(&in_db);
  EXPECT_TRUE(ok) << "Should now be in the database";
  EXPECT_EQ(in_db, InstallUpdatesAutomatically::kAsk);
}

TEST_F(AktualizrDbus, DbusRequestConsent) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  Dbus dut(std::move(dut_bus_), storage);

  sd_bus_error err = SD_BUS_ERROR_NULL;
  char* property_value;

  //
  // Read the Consent property and check it is empty
  //
  int res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                       &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";

  ASSERT_STREQ(property_value, "") << "By default, we are not waiting for consent";

  int counter = 0;
  res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                            "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;

  std::vector<Uptane::Target> install_targets;
  install_targets.push_back(Uptane::Target::Unknown());

  //
  // Request Consent and get a notification callback
  //
  auto consent_response = dut.GetConsent(install_targets, "corr-id-123");

  using namespace std::chrono_literals;
  ASSERT_EQ(consent_response.wait_for(1ms), std::future_status::timeout)
      << "Consent result should not be immediately available";

  // pump the system bus to wait for the change notification
  for (int i = 0; (counter == 0) && (i < 100); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(10'000);
  }
  EXPECT_EQ(counter, 1) << "Should have got a notification that the signal has changed";

  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";

  ASSERT_STRNE(property_value, "") << "Should now be asking for consent";

  // The property should carry the correlationId that GetConsent was given
  Json::Value consent_json = Utils::parseJSON(property_value);
  EXPECT_EQ(consent_json["correlationId"].asString(), "corr-id-123");

  //
  // Replying with the wrong correlationId should be rejected and leave the
  // request pending
  //
  sd_bus_message* reply = nullptr;
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 1,
                           "stale response", "corr-id-STALE");
  ASSERT_LT(res, 0) << "Call with mismatched correlationId should fail";
  ASSERT_STREQ(err.name, "org.freedesktop.DBus.Error.InvalidArgs");
  sd_bus_error_free(&err);
  ASSERT_EQ(consent_response.wait_for(1ms), std::future_status::timeout)
      << "A rejected response should leave the request pending";

  //
  // Reply on D-Bus
  //
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 1,
                           "test grant message", "corr-id-123");
  ASSERT_GE(res, 0) << "Call failed";
  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);

  // ..reply twice...
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 1,
                           "repeat", "corr-id-123");
  ASSERT_EQ(res, -13) << "Call should return a failure";
  ASSERT_STREQ(err.name, "org.freedesktop.DBus.Error.Failed");
  sd_bus_error_free(&err);

  //
  // Note the future resolves the right way
  //
  ASSERT_EQ(consent_response.wait_for(1s), std::future_status::ready)
      << "Consent result should become available after responding over D-Bus";

  auto outcome = consent_response.get();
  EXPECT_TRUE(outcome.granted());
  EXPECT_EQ(outcome.reason, "test grant message");
  EXPECT_EQ(outcome.correlation_id, "corr-id-123");

  //
  // Finally, the Consent Property should be empty
  //
  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";

  ASSERT_STREQ(property_value, "") << "Once a response has been returned, no consent request should be pending";
}

TEST_F(AktualizrDbus, PendingUpdateCancelled) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  Dbus dut(std::move(dut_bus_), storage);

  sd_bus_error err = SD_BUS_ERROR_NULL;

  std::vector<Uptane::Target> install_targets;
  install_targets.push_back(Uptane::Target::Unknown());

  // Watch for notifications
  int counter = 0;

  int res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;

  // Ask for consent
  auto consent_response = dut.GetConsent(install_targets, "corr-id-cancel");

  // pump the system bus to wait for the change notification
  for (int i = 0; (counter == 0) && (i < 100); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(10'000);
  }
  EXPECT_EQ(counter, 1) << "Should have got a notification that the signal has changed";

  counter = 0;
  // Cancel the in-flight request
  dut.PendingUpdateCancelled();

  EXPECT_EQ(consent_response.wait_for(100ms), future_status::ready) << "GetConsent promise should complete";
  EXPECT_NO_THROW(consent_response.get()) << "Shouldn't throw";  // NOLINT

  // pump the system bus to wait for the change notification
  for (int i = 0; (counter == 0) && (i < 100); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(10'000);
  }
  EXPECT_EQ(counter, 1) << "Should have got a notification that the signal has changed";

  // We should not be asking any more
  char* property_value;
  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";

  ASSERT_STREQ(property_value, "") << "Should not be asking for consent after a pending update is cancelled";

  //
  // Reply on D-Bus. This should be ignored but not crash
  //
  sd_bus_message* reply = nullptr;
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 1,
                           "test grant message", "corr-id-cancel");
  ASSERT_EQ(res, -13) << "Call should fail";
  sd_bus_error_free(&err);
}

/**
 * A second GetConsent call supersedes the first: the old future resolves as
 * kSuperseded, the property changes (with a signal), stale correlationIds are
 * rejected, and the new offer can be consented to.
 */
TEST_F(AktualizrDbus, SupersededConsentRequest) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  Dbus dut(std::move(dut_bus_), storage);

  sd_bus_error err = SD_BUS_ERROR_NULL;

  std::vector<Uptane::Target> install_targets;
  install_targets.push_back(Uptane::Target::Unknown());

  int counter = 0;
  int res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;

  auto first_response = dut.GetConsent(install_targets, "corr-id-A");
  // Supersede it before anyone answers
  auto second_response = dut.GetConsent(install_targets, "corr-id-B");

  ASSERT_EQ(first_response.wait_for(100ms), future_status::ready)
      << "Superseded consent future should resolve immediately";
  auto first_outcome = first_response.get();
  EXPECT_EQ(first_outcome.result, Consent::Outcome::Result::kSuperseded);
  EXPECT_FALSE(first_outcome.granted());
  EXPECT_EQ(first_outcome.correlation_id, "corr-id-A");

  // Both transitions must emit PropertiesChanged
  for (int i = 0; (counter < 2) && (i < 100); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(10'000);
  }
  EXPECT_EQ(counter, 2) << "Each GetConsent call should signal a property change";

  // The property must now advertise the new offer
  char* property_value = nullptr;
  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";
  Json::Value consent_json = Utils::parseJSON(property_value);
  EXPECT_EQ(consent_json["correlationId"].asString(), "corr-id-B");

  // Answering the stale offer must be rejected and leave the new one pending
  sd_bus_message* reply = nullptr;
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 1,
                           "stale", "corr-id-A");
  ASSERT_LT(res, 0) << "Stale correlationId should be rejected";
  ASSERT_STREQ(err.name, "org.freedesktop.DBus.Error.InvalidArgs");
  sd_bus_error_free(&err);
  ASSERT_EQ(second_response.wait_for(1ms), std::future_status::timeout)
      << "The new offer should still be pending after a stale response";

  // Answering the current offer works
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::Consent, &err, &reply, "bss", 1,
                           "ok", "corr-id-B");
  ASSERT_GE(res, 0) << "Call failed";
  sd_bus_message_unref(reply);
  ASSERT_EQ(second_response.wait_for(1s), std::future_status::ready);
  auto second_outcome = second_response.get();
  EXPECT_TRUE(second_outcome.granted());
  EXPECT_EQ(second_outcome.correlation_id, "corr-id-B");
}

/**
 * If InstallUpdatesAutomatically flips to kProceed while an offer is pending,
 * a subsequent GetConsent (e.g. supersession) must clear the stale pending
 * request (with a property-change signal) instead of leaving it dangling.
 */
TEST_F(AktualizrDbus, ProceedFlipClearsPendingConsent) {
  StorageConfig config_storage;
  config_storage.path = temp_dir_.Path();
  auto storage = INvStorage::newStorage(config_storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);
  Dbus dut(std::move(dut_bus_), storage);

  sd_bus_error err = SD_BUS_ERROR_NULL;

  std::vector<Uptane::Target> install_targets;
  install_targets.push_back(Uptane::Target::Unknown());

  auto first_response = dut.GetConsent(install_targets, "corr-id-A");
  ASSERT_EQ(first_response.wait_for(1ms), std::future_status::timeout) << "Request should be pending";

  // The user enables automatic updates while the prompt is showing
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kProceed);

  int counter = 0;
  int res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;

  // e.g. a superseding update arrives; consent is now automatic
  auto second_response = dut.GetConsent(install_targets, "corr-id-B");
  ASSERT_EQ(second_response.wait_for(100ms), future_status::ready) << "kProceed consent should be immediate";
  EXPECT_TRUE(second_response.get().granted());

  // The stale request must have been resolved and cleared, with a signal
  ASSERT_EQ(first_response.wait_for(100ms), future_status::ready);
  EXPECT_EQ(first_response.get().result, Consent::Outcome::Result::kSuperseded);

  for (int i = 0; (counter == 0) && (i < 100); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(10'000);
  }
  EXPECT_EQ(counter, 1) << "Clearing the stale offer should signal a property change";

  char* property_value = nullptr;
  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";
  ASSERT_STREQ(property_value, "") << "No stale offer should be advertised after the kProceed flip";
}

/**
 * CheckForUpdates over D-Bus triggers an immediate peek while the device is
 * waiting for consent (previously it only worked from idle).
 */
TEST_F(AktualizrDbus, CheckForUpdatesDuringConsent) {
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "hasupdates", fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.uptane.polling_sec = 600;

  // Require consent
  auto storage = INvStorage::newStorage(conf.storage);
  storage->storeInstallUpdatesAutomatically(InstallUpdatesAutomatically::kAsk);

  UptaneTestCommon::TestAktualizr aktualizr(conf, storage, http);
  aktualizr.SetDbusInterface(std::move(dut_bus_));

  struct CheckEvents {
    std::mutex m;
    std::condition_variable cv;
    int update_checks{0};

    void HandleEvent(const std::shared_ptr<event::BaseEvent>& event) {
      if (event->variant == "UpdateCheckComplete") {
        std::lock_guard<std::mutex> guard{m};
        update_checks++;
        cv.notify_all();
      }
    }
  };
  CheckEvents check_events;
  auto conn = aktualizr.SetSignalHandler(std::bind(&CheckEvents::HandleEvent, &check_events, std::placeholders::_1));

  aktualizr.Initialize();
  auto ak_future = aktualizr.RunForever();

  // Wait until we are waiting for consent
  int counter = 0;
  int res = sd_bus_match_signal(client_bus_, nullptr, nullptr, Dbus::Path, "org.freedesktop.DBus.Properties",
                                "PropertiesChanged", bus_signal_callback, &counter);
  ASSERT_GE(res, 0) << "Adding match signal failed:" << res;
  for (int i = 0; (counter == 0) && (i < 200); i++) {
    int messages = sd_bus_process(client_bus_, nullptr);
    ASSERT_GE(messages, 0) << "sd_bus_process got error" << -messages;
    usleep(100'000);
  }
  ASSERT_EQ(counter, 1) << "Should be waiting for consent";

  int checks_at_prompt;
  {
    std::lock_guard<std::mutex> guard{check_events.m};
    checks_at_prompt = check_events.update_checks;
  }

  // Nudge while waiting for consent; polling_sec is 600 so only the nudge can
  // cause another check
  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  res = sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::CheckForUpdates, &ret_error,
                           &reply, "");
  ASSERT_GE(res, 0) << "Call failed";
  sd_bus_message_unref(reply);

  {
    std::unique_lock guard{check_events.m};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (check_events.update_checks < checks_at_prompt + 1) {
      if (check_events.cv.wait_until(guard, deadline) == std::cv_status::timeout) {
        aktualizr.Shutdown();
        FAIL() << "Timed out waiting for the nudged update check during consent";
      }
    }
  }

  // Still waiting for consent (same offer): the property should be non-empty
  sd_bus_error err = SD_BUS_ERROR_NULL;
  char* property_value = nullptr;
  res = sd_bus_get_property_string(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, &err,
                                   &property_value);
  ASSERT_GE(res, 0) << "Get property call failed";
  EXPECT_STRNE(property_value, "") << "Consent should still be pending after a same-offer peek";

  aktualizr.Shutdown();
  ak_future.wait();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc != 2) {
    // NOLINTNEXTLINE
    std::cerr << "Error: " << argv[0] << " requires the path to the base directory of Uptane repos.\n";
    return EXIT_FAILURE;
  }
  // NOLINTNEXTLINE
  uptane_repos_dir = argv[1];

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  TemporaryDirectory tmp_dir;
  fake_meta_dir = tmp_dir.Path();
  CreateFakeRepoMetaData(fake_meta_dir);

  return RUN_ALL_TESTS();
}
