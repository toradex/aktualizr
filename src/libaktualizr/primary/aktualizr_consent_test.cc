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
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = boost::filesystem;

/**
 * Testing base class that sets up Uptane metadata with custom fields.
 */
class AktualizrConsent : public testing::Test {
 public:
  AktualizrConsent(AktualizrConsent&&) = delete;
  AktualizrConsent(const AktualizrConsent&) = delete;
  AktualizrConsent& operator=(const AktualizrConsent&) = delete;
  AktualizrConsent& operator=(AktualizrConsent&&) = delete;
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
  explicit MockConsent(Json::Value* targets) : targets_{targets} { assert(targets_); }

  std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& targets) override {
    *targets_ = TargetsToJson(targets);
    std::promise<Outcome> p;
    p.set_value({false, false, "Rejected in MockConsent"});
    return p.get_future();
  }

  void PendingUpdateCancelled() override {}

 private:
  Json::Value* targets_;
};

/**
 * ConsentRequired property should (mostly) match the director targets.json
 */
TEST_F(AktualizrConsent, CorrectConsentInformation) {  // NOLINT
  // Setup
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
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
  // auto image = Utils::parseJSONFile(uptane_metadata_dir_ / "repo/repo/targets.json");
  // std::cout << "image target\n" << image << "\n";
  auto director = Utils::parseJSONFile(uptane_metadata_dir_ / "repo/director/targets.json");
  // std::cout << "director target\n" << director << "\n";

  auto expected_content = director["signed"];
  expected_content.removeMember("expires");
  expected_content.removeMember("version");
  expected_content.removeMember("custom");

  EXPECT_EQ(consent_blob, expected_content) << "The consent blob should (mostly) match the director metadata";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // NOLINTNEXTLINE
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
