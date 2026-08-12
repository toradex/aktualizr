#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

#include "http/httpinterface.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"
#include "primary/online_logs_uploader.h"
#include "primary/test_journal.h"

// ============================================================================
// Minimal HttpInterface fake that tracks PUTs to /install_logs.
// ============================================================================

class HttpFakeOnlineLogs : public HttpInterface {
 public:
  HttpResponse get(const std::string& /*url*/, int64_t /*maxsize*/, const api::FlowControlToken* /*flow_control*/,
                   const Headers* /*extra_headers*/) override {
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse post(const std::string& /*url*/, const std::string& /*content_type*/,
                    const std::string& /*data*/) override {
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse put(const std::string& url, const Json::Value& data) override {
    if (url.find("/install_logs") != std::string::npos) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++install_logs_put_count_;
      install_logs_puts_.push_back(data);
    }
    install_logs_cv_.notify_all();
    return install_logs_response_;
  }

  HttpResponse post(const std::string& /*url*/, const Json::Value& /*data*/) override {
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse put(const std::string& /*url*/, const std::string& /*content_type*/,
                   const std::string& /*data*/) override {
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse download(const std::string& /*url*/, curl_write_callback /*write_cb*/,
                        curl_xferinfo_callback /*progress_cb*/, void* /*userp*/, curl_off_t /*from*/) override {
    return HttpResponse("", 200, CURLE_OK, "");
  }

  std::future<HttpResponse> downloadAsync(const std::string& /*url*/, curl_write_callback /*write_cb*/,
                                          curl_xferinfo_callback /*progress_cb*/, void* /*userp*/,
                                          curl_off_t /*from*/) override {
    std::promise<HttpResponse> p;
    p.set_value(HttpResponse("", 200, CURLE_OK, ""));
    return p.get_future();
  }

  void setCerts(const std::string& /*ca*/, CryptoSource /*ca_source*/, const std::string& /*cert*/,
                CryptoSource /*cert_source*/, const std::string& /*pkey*/, CryptoSource /*pkey_source*/) override {}

  // -- helpers ---------------------------------------------------------------
  int PutCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return install_logs_put_count_;
  }

  std::vector<Json::Value> Puts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return install_logs_puts_;
  }

  void SetResponse(const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    install_logs_response_ = response;
  }

  // Wait until at least one /install_logs PUT has been observed, or `timeout`
  // elapses. Returns the put count seen (0 on timeout, >0 on success).
  int WaitForInstallLog(std::chrono::steady_clock::duration timeout = std::chrono::seconds(5));

  // Wait for the PUT count to reach `target` (or `target - 1` if you want to
  // assert "no further PUT" within the timeout — i.e. the test passes if the
  // wait times out before the target is hit). Blocks on the same cv that
  // put() notifies, so it returns as soon as the worker thread issues a PUT
  // rather than racing with a fixed-duration timer. The `timeout` only acts
  // as a hang detector.
  bool WaitForPutCount(int target, std::chrono::steady_clock::duration timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return install_logs_cv_.wait_for(lock, timeout, [this, target] { return install_logs_put_count_ >= target; });
  }

  // Convenience wrapper that returns the put count once at least one PUT has
  // happened, or 0 on timeout.
  int WaitForInstallLogWithTimeout(std::chrono::steady_clock::duration timeout) {
    return WaitForPutCount(1, timeout) ? PutCount() : 0;
  }

  // -- state tracking --------------------------------------------------------
  std::mutex mutex_;
  std::condition_variable install_logs_cv_;
  std::vector<Json::Value> install_logs_puts_;
  int install_logs_put_count_{0};
  HttpResponse install_logs_response_{"", 200, CURLE_OK, ""};
};

int HttpFakeOnlineLogs::WaitForInstallLog(std::chrono::steady_clock::duration timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  install_logs_cv_.wait_for(lock, timeout, [this] { return install_logs_put_count_ > 0; });
  return install_logs_put_count_;
}

// ============================================================================
// Tests — guarded by BUILD_OFFLINE_UPDATES because the real class depends on
// libsystemd (JournalHandle).  The stub is trivially correct.
// ============================================================================

#ifdef BUILD_OFFLINE_UPDATES

TEST(OnlineLogsUploader, DisabledViaConfig) {
  Config config;
  config.logger.online_logs_enabled = false;
  auto http = std::make_shared<HttpFakeOnlineLogs>();
  OnlineLogsUploader uploader(config, http, std::make_shared<TestJournal>());

  EXPECT_FALSE(uploader.IsEnabled());
  EXPECT_FALSE(uploader.IsActive());

  uploader.Begin("test-correlation-id");
  EXPECT_FALSE(uploader.IsActive());

  uploader.End();
  EXPECT_FALSE(uploader.IsActive());

  // No HTTP calls should have been made.
  EXPECT_EQ(http->PutCount(), 0);
}

TEST(OnlineLogsUploader, BeginEndLifecycle) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.tls.server = "https://example.com";
  auto http = std::make_shared<HttpFakeOnlineLogs>();
  OnlineLogsUploader uploader(config, http, std::make_shared<TestJournal>());

  EXPECT_TRUE(uploader.IsEnabled());
  EXPECT_FALSE(uploader.IsActive());

  uploader.Begin("correlation-123");
  EXPECT_TRUE(uploader.IsActive());

  uploader.End();
  EXPECT_FALSE(uploader.IsActive());
}

TEST(OnlineLogsUploader, EndWithoutBeginIsNoop) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.tls.server = "https://example.com";
  auto http = std::make_shared<HttpFakeOnlineLogs>();
  OnlineLogsUploader uploader(config, http, std::make_shared<TestJournal>());

  // Should not crash.
  uploader.End();
  EXPECT_FALSE(uploader.IsActive());
}

TEST(OnlineLogsUploader, BeginWhileActiveEndsFirst) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.tls.server = "https://example.com";
  auto http = std::make_shared<HttpFakeOnlineLogs>();
  OnlineLogsUploader uploader(config, http, std::make_shared<TestJournal>());

  uploader.Begin("first");
  EXPECT_TRUE(uploader.IsActive());

  // Second Begin should end the first session gracefully.
  uploader.Begin("second");
  EXPECT_TRUE(uploader.IsActive());

  uploader.End();
  EXPECT_FALSE(uploader.IsActive());
}

TEST(OnlineLogsUploader, DestructorCleansUpActiveThread) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.tls.server = "https://example.com";
  auto http = std::make_shared<HttpFakeOnlineLogs>();

  {
    OnlineLogsUploader uploader(config, http, std::make_shared<TestJournal>());
    uploader.Begin("correlation-456");
    EXPECT_TRUE(uploader.IsActive());
    // Destructor should cleanly shut down the thread.
  }
  // If we get here without hanging, the test passes.
}

// When a journal entry arrives for a captured service after streaming starts,
// it should be PUT to /install_logs with the expected JSON shape.
TEST(OnlineLogsUploader, CapturesAndUploadsMatchingEntries) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.logger.capture_services = {"aktualizr"};
  config.tls.server = "https://example.com";

  auto http = std::make_shared<HttpFakeOnlineLogs>();
  auto journal = std::make_shared<TestJournal>();

  OnlineLogsUploader uploader(config, http, journal);

  uploader.Begin("correlation-abc");
  ASSERT_TRUE(uploader.IsActive());

  // Entries are appended to the shared store; the uploader's clone observes
  // them via Next() and uploads the ones that match the capture filter.
  journal->AddEntry("aktualizr.service", "Checking for updates...", 1731142800000000LL, "cursor-1");
  journal->AddEntry("aktualizr.service", "Downloading target", 1731142801000000LL, "cursor-2");

  ASSERT_GT(http->WaitForInstallLog(), 0);

  uploader.End();
  EXPECT_FALSE(uploader.IsActive());

  // Collect every uploaded message across all batches.
  std::vector<std::string> messages;
  std::string correlation_id;
  for (const auto& body : http->Puts()) {
    correlation_id = body["correlationId"].asString();
    EXPECT_TRUE(body.isMember("startCursor"));
    for (const auto& entry : body["logs"]) {
      messages.push_back(entry["message"].asString());
      EXPECT_EQ(entry["service"].asString(), "aktualizr.service");
      // Timestamp formatted as ISO 8601 UTC.
      EXPECT_NE(entry["timestamp"].asString().find('T'), std::string::npos);
    }
  }

  EXPECT_EQ(correlation_id, "correlation-abc");
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages[0], "Checking for updates...");
  EXPECT_EQ(messages[1], "Downloading target");
}

// Entries from services not in the capture list should be ignored.
TEST(OnlineLogsUploader, IgnoresUnmatchedServices) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.logger.capture_services = {"aktualizr"};
  config.tls.server = "https://example.com";

  auto http = std::make_shared<HttpFakeOnlineLogs>();
  auto journal = std::make_shared<TestJournal>();

  OnlineLogsUploader uploader(config, http, journal);

  uploader.Begin("correlation-filter");
  ASSERT_TRUE(uploader.IsActive());

  journal->AddEntry("sshd.service", "Accepted password", 1731142800000000LL, "cursor-1");
  journal->AddEntry("cron.service", "Job started", 1731142801000000LL, "cursor-2");
  journal->AddEntry("aktualizr.service", "Installing", 1731142802000000LL, "cursor-3");

  ASSERT_GT(http->WaitForInstallLog(), 0);

  uploader.End();

  std::vector<std::string> messages;
  for (const auto& body : http->Puts()) {
    for (const auto& entry : body["logs"]) {
      messages.push_back(entry["message"].asString());
    }
  }

  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages[0], "Installing");
}

// Multiple distinct batches of log entries should each be PUT to the server
// independently. The uploader's outer loop must continue streaming after the
// first batch is acknowledged (rather than only delivering the first chunk).
TEST(OnlineLogsUploader, StreamsMultipleBatches) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.logger.capture_services = {"aktualizr"};
  config.tls.server = "https://example.com";

  auto http = std::make_shared<HttpFakeOnlineLogs>();
  auto journal = std::make_shared<TestJournal>();

  OnlineLogsUploader uploader(config, http, journal);

  uploader.Begin("correlation-multibatch");
  ASSERT_TRUE(uploader.IsActive());

  // -- batch 1: a couple of entries that should arrive as the first PUT ---
  journal->AddEntry("aktualizr.service", "checking for updates", 1731142800000000LL, "cursor-1");
  journal->AddEntry("aktualizr.service", "downloading target", 1731142801000000LL, "cursor-2");

  ASSERT_GT(http->WaitForInstallLog(std::chrono::seconds(15)), 0);

  // -- batch 2: more entries added after the first batch has been PUT -------
  // The worker sleeps for kBatchInterval (5s) between non-full batches. Give
  // it room to wake, drain the new entries, and emit a second PUT.
  journal->AddEntry("aktualizr.service", "applying update", 1731142802000000LL, "cursor-3");
  journal->AddEntry("aktualizr.service", "update applied", 1731142803000000LL, "cursor-4");

  // Wait until we have observed at least two PUTs.
  {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      if (http->PutCount() >= 2) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  // -- batch 3: yet another round to be sure streaming keeps going ----------
  journal->AddEntry("aktualizr.service", "rebooting", 1731142804000000LL, "cursor-5");
  journal->AddEntry("aktualizr.service", "post-boot", 1731142805000000LL, "cursor-6");

  // Wait for the third PUT.
  {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      if (http->PutCount() >= 3) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  uploader.End();
  EXPECT_FALSE(uploader.IsActive());

  // -- assertions: every entry should appear in some PUT, and each PUT ------
  // should carry its own startCursor.
  const auto puts = http->Puts();
  ASSERT_GE(puts.size(), 3U) << "expected at least three PUTs to /install_logs, got " << puts.size();

  std::vector<std::string> messages;
  std::set<std::string> start_cursors;
  for (const auto& body : puts) {
    EXPECT_EQ(body["correlationId"].asString(), "correlation-multibatch");
    ASSERT_TRUE(body.isMember("startCursor"));
    start_cursors.insert(body["startCursor"].asString());
    for (const auto& entry : body["logs"]) {
      messages.push_back(entry["message"].asString());
    }
  }

  // Six distinct entries were added; all six must have been uploaded.
  EXPECT_EQ(messages.size(), 6U);
  EXPECT_NE(std::find(messages.begin(), messages.end(), "checking for updates"), messages.end());
  EXPECT_NE(std::find(messages.begin(), messages.end(), "downloading target"), messages.end());
  EXPECT_NE(std::find(messages.begin(), messages.end(), "applying update"), messages.end());
  EXPECT_NE(std::find(messages.begin(), messages.end(), "update applied"), messages.end());
  EXPECT_NE(std::find(messages.begin(), messages.end(), "rebooting"), messages.end());
  EXPECT_NE(std::find(messages.begin(), messages.end(), "post-boot"), messages.end());

  // Each PUT should have its own startCursor matching the first entry of that
  // batch — if the body were being reused without resetting startCursor, this
  // would collapse to a single cursor across all PUTs.
  EXPECT_EQ(start_cursors.size(), puts.size())
      << "each PUT must carry the cursor of its first entry, not a reused cursor from batch 1";
  EXPECT_TRUE(start_cursors.count("cursor-1") > 0);
  EXPECT_TRUE(start_cursors.count("cursor-3") > 0);
  EXPECT_TRUE(start_cursors.count("cursor-5") > 0);
}

// A 404 response means the server doesn't support the endpoint; the uploader
// should stop trying for this update and not PUT further batches.
TEST(OnlineLogsUploader, StopsUploadingAfter404) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.logger.capture_services = {"aktualizr"};
  config.tls.server = "https://example.com";

  auto http = std::make_shared<HttpFakeOnlineLogs>();
  http->SetResponse(HttpResponse("", 404, CURLE_OK, "Not Found"));
  auto journal = std::make_shared<TestJournal>();

  OnlineLogsUploader uploader(config, http, journal);

  uploader.Begin("correlation-404");
  ASSERT_TRUE(uploader.IsActive());

  journal->AddEntry("aktualizr.service", "first", 1731142800000000LL, "cursor-1");

  // Event-driven wait: blocks until the worker has emitted its first PUT, or
  // a generous safety timeout (covers a genuine hang in production code). This
  // is the fix for the previous flake where both this wait and the worker's
  // internal kBatchInterval poll used the same 5 s, causing the test to time
  // out at exactly the moment the worker finally woke up.
  ASSERT_TRUE(http->WaitForPutCount(1, std::chrono::seconds(10)))
      << "first PUT never arrived (worker may be stuck or polling slower than expected)";
  int after_first = http->PutCount();

  // Add another entry. The uploader must not PUT it: it should have disabled
  // itself for this update after observing the 404 on the first PUT. After a
  // 404, the worker exits at the top of its outer loop on the next iteration
  // (it first sleeps for the remaining kBatchInterval slice). One full poll
  // interval is enough headroom for the worker to either re-PUT (which would
  // be a regression) or fully exit its run loop.
  journal->AddEntry("aktualizr.service", "second", 1731142801000000LL, "cursor-2");
  EXPECT_FALSE(http->WaitForPutCount(after_first + 1, std::chrono::seconds(10)))
      << "uploader emitted a second PUT after a 404 response";

  uploader.End();

  EXPECT_EQ(http->PutCount(), after_first);
}

// If the journal can't be opened, Begin() should not start streaming.
TEST(OnlineLogsUploader, NoStreamingWhenJournalUnavailable) {
  Config config;
  config.logger.online_logs_enabled = true;
  config.logger.capture_services = {"aktualizr"};
  config.tls.server = "https://example.com";

  auto http = std::make_shared<HttpFakeOnlineLogs>();
  auto journal = std::make_shared<TestJournal>();
  journal->SetOpen(false);

  OnlineLogsUploader uploader(config, http, journal);

  uploader.Begin("correlation-closed");
  // The uploader thread object is created but immediately bails because the
  // cloned journal reports !IsOpen(); no logs are ever uploaded.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uploader.End();

  EXPECT_EQ(http->PutCount(), 0);
}

TEST(OnlineLogsUploader, TimestampFormatEpoch) {
  // 0 µs since epoch → 1970-01-01T00:00:00.000000Z
  EXPECT_EQ(OnlineLogsUploader::FormatTimestamp(0), "1970-01-01T00:00:00.000000Z");
}

TEST(OnlineLogsUploader, TimestampFormatKnownValue) {
  // 2024-11-09T09:00:00.123456Z in µs since epoch
  // 2024-11-09T09:00:00 UTC = 1731142800 seconds
  int64_t ts = (1731142800LL * 1000000) + 123456;
  EXPECT_EQ(OnlineLogsUploader::FormatTimestamp(ts), "2024-11-09T09:00:00.123456Z");
}

TEST(OnlineLogsUploader, TimestampFormatWholeSecond) {
  // 2025-01-01T00:00:00.000000Z = 1735689600 seconds
  int64_t ts = 1735689600LL * 1000000;
  EXPECT_EQ(OnlineLogsUploader::FormatTimestamp(ts), "2025-01-01T00:00:00.000000Z");
}

#else  // !BUILD_OFFLINE_UPDATES

TEST(OnlineLogsUploader, StubIsNoop) {
  Config config;
  auto http = std::make_shared<HttpFakeOnlineLogs>();
  OnlineLogsUploader uploader(config, http, nullptr);

  EXPECT_FALSE(OnlineLogsUploader::IsEnabled());
  EXPECT_FALSE(OnlineLogsUploader::IsActive());

  uploader.Begin("test");
  uploader.End();
}

#endif  // BUILD_OFFLINE_UPDATES

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);
  return RUN_ALL_TESTS();
}
#endif
