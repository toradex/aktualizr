/**
 * \file
 * Integration tests for online installation-log capture.
 *
 * Where online_logs_uploader_test.cc exercises OnlineLogsUploader in isolation,
 * this test drives the feature through a real SotaUptaneClient (via
 * UptaneTestCommon::TestUptaneClient) to verify the end-to-end wiring:
 *
 *   - the journal prototype is dependency-injected into SotaUptaneClient,
 *   - streaming starts at the beginning of downloadImages() for online updates,
 *   - journal entries that arrive during the install are PUT to
 *     <gateway>/install_logs, and
 *   - streaming stops when uptaneInstall() completes.
 *
 * See docs/markdown/online-logs.md for the design.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "httpfake.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"
#include "metafake.h"
#include "primary/sotauptaneclient.h"
#include "primary/test_journal.h"
#include "storage/invstorage.h"
#include "uptane_test_common.h"
#include "utilities/utils.h"

boost::filesystem::path fake_meta_dir;

#ifdef BUILD_OFFLINE_UPDATES

namespace {

// Wait until predicate() is true or the timeout elapses. Returns predicate().
template <class Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

}  // namespace

/**
 * HttpFake that also serves the standard "hasupdates" repository but records
 * any PUTs to /install_logs so the test can assert on the streamed logs.
 */
class HttpFakeInstallLogs : public HttpFake {
 public:
  HttpFakeInstallLogs(const boost::filesystem::path& test_dir_in, const boost::filesystem::path& meta_dir_in)
      : HttpFake(test_dir_in, "hasupdates", meta_dir_in) {}

  HttpResponse put(const std::string& url, const Json::Value& data) override {
    if (url.find("/install_logs") != std::string::npos) {
      std::lock_guard<std::mutex> lock(mutex_);
      install_logs_puts_.push_back(data);
      return HttpResponse("", 200, CURLE_OK, "");
    }
    return HttpFake::put(url, data);
  }

  std::vector<Json::Value> InstallLogsPuts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return install_logs_puts_;
  }

  size_t InstallLogsPutCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return install_logs_puts_.size();
  }

 private:
  std::mutex mutex_;
  std::vector<Json::Value> install_logs_puts_;
};

/*
 * Drive a full online update through SotaUptaneClient and verify that journal
 * entries captured during the install are streamed to /install_logs.
 */
TEST(OnlineLogs, CapturedDuringOnlineUpdate) {
  TemporaryDirectory temp_dir;
  auto http = std::make_shared<HttpFakeInstallLogs>(temp_dir.Path(), fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir, http->tls_server);
  conf.logger.online_logs_enabled = true;
  conf.logger.capture_services = {"aktualizr"};

  auto storage = INvStorage::newStorage(conf.storage);

  // Dependency-inject a TestJournal as the journal prototype; SotaUptaneClient
  // clones it when streaming begins.
  auto journal = std::make_shared<TestJournal>();
  UptaneTestCommon::TestUptaneClient sota_client(conf, storage, http, journal);
  sota_client.initialize();

  // Discover the two pending updates from the "hasupdates" repository.
  result::UpdateCheck update_result = sota_client.fetchMeta();
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  ASSERT_EQ(update_result.updates.size(), 2U);

  // downloadImages(kOnline) starts the log streaming thread.
  result::Download download_result = sota_client.downloadImages(update_result.updates, UpdateType::kOnline);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  // Entries arriving after streaming began should be uploaded; entries from
  // services not in capture_services should be ignored.
  journal->AddEntry("aktualizr.service", "Installing target", 1731142800000000LL, "cursor-1");
  journal->AddEntry("sshd.service", "should be ignored", 1731142800500000LL, "cursor-2");
  journal->AddEntry("aktualizr.service", "Install complete", 1731142801000000LL, "cursor-3");

  ASSERT_TRUE(WaitFor([&]() { return http->InstallLogsPutCount() >= 1; }))
      << "Expected at least one PUT to /install_logs";

  // uptaneInstall(kOnline) stops streaming (flushing any remaining logs).
  sota_client.uptaneInstall(update_result.updates, UpdateType::kOnline);

  // Gather every captured message across all batches.
  std::vector<std::string> messages;
  std::string correlation_id;
  for (const auto& body : http->InstallLogsPuts()) {
    correlation_id = body["correlationId"].asString();
    EXPECT_TRUE(body.isMember("startCursor"));
    for (const auto& entry : body["logs"]) {
      messages.push_back(entry["message"].asString());
      EXPECT_EQ(entry["service"].asString(), "aktualizr.service");
      // Timestamps are ISO 8601 UTC.
      EXPECT_NE(entry["timestamp"].asString().find('T'), std::string::npos);
    }
  }

  EXPECT_FALSE(correlation_id.empty()) << "Logs should carry the update correlation id";
  ASSERT_EQ(messages.size(), 2U) << "Only the captured-service entries should be uploaded";
  EXPECT_EQ(messages[0], "Installing target");
  EXPECT_EQ(messages[1], "Install complete");
}

/*
 * When online log capture is disabled in config, no /install_logs PUTs should
 * be made even though an online update runs to completion.
 */
TEST(OnlineLogs, DisabledViaConfig) {
  TemporaryDirectory temp_dir;
  auto http = std::make_shared<HttpFakeInstallLogs>(temp_dir.Path(), fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir, http->tls_server);
  conf.logger.online_logs_enabled = false;
  conf.logger.capture_services = {"aktualizr"};

  auto storage = INvStorage::newStorage(conf.storage);

  auto journal = std::make_shared<TestJournal>();
  UptaneTestCommon::TestUptaneClient sota_client(conf, storage, http, journal);
  sota_client.initialize();

  result::UpdateCheck update_result = sota_client.fetchMeta();
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);

  result::Download download_result = sota_client.downloadImages(update_result.updates, UpdateType::kOnline);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess);

  journal->AddEntry("aktualizr.service", "Installing target", 1731142800000000LL, "cursor-1");

  sota_client.uptaneInstall(update_result.updates, UpdateType::kOnline);

  // Give any (erroneously started) background thread a chance to PUT.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(http->InstallLogsPutCount(), 0U);
}

#endif  // BUILD_OFFLINE_UPDATES

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  TemporaryDirectory tmp_dir;
  fake_meta_dir = tmp_dir.Path();
  CreateFakeRepoMetaData(fake_meta_dir);

  return RUN_ALL_TESTS();
}
#endif

// vim: set tabstop=2 shiftwidth=2 expandtab:
