#include <gtest/gtest.h>

#include "http/httpclient.h"

#include <atomic>
#include <boost/process.hpp>
#include <chrono>
#include <string>
#include <thread>

#include "json/json.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "utilities/flow_control.h"
#include "utilities/utils.h"

// NOLINTNEXTLINE(*non-const*)
static std::string server = "http://127.0.0.1:";

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, CopyConstructorTest) {
  HttpClient http;
  HttpClient http_copy(http);
  std::string path = "/path/1/2/3";
  Json::Value resp = http_copy.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(resp["path"].asString(), path);
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Get) {
  HttpClient http;
  std::string path = "/path/1/2/3";
  Json::Value response = http.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(response["path"].asString(), path);
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, GetWithHeaders) {
  std::vector<std::string> headers = {"Authorization: Bearer token"};
  HttpClient http(&headers);
  std::string path = "/auth_call";
  Json::Value response = http.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(response["status"].asString(), "good");
}

/* Reject http GET responses that exceed size limit. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, DownloadSizeLimit) {
  HttpClient http;
  std::string path = "/large_file";
  HttpResponse resp = http.get(server + path, 1024, nullptr);
  std::cout << "RESP SIZE " << resp.body.length() << std::endl;
  EXPECT_EQ(resp.curl_code, CURLE_FILESIZE_EXCEEDED);
}

/* Reject http GET responses that do not meet speed limit. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, DownloadSpeedLimit) {
  HttpClient http;
  std::string path = "/slow_file";

  http.overrideSpeedLimitParams(3, 5000);
  HttpResponse resp = http.get(server + path, HttpInterface::kNoLimit, nullptr);
  EXPECT_EQ(resp.curl_code, CURLE_OPERATION_TIMEDOUT);
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Cancellation) {
  HttpClient http;
  std::string path = "/slow_file";
  api::FlowControlToken token;
  std::atomic<bool> did_abort{false};
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::thread abort_thread([&token, end, &did_abort] {
    std::this_thread::sleep_until(end);
    token.setAbort();
    did_abort = true;
  });
  HttpResponse resp = http.get(server + path, HttpInterface::kNoLimit, &token);
  auto actual_end = std::chrono::steady_clock::now();
  EXPECT_TRUE(did_abort);
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(actual_end - end).count();

  LOG_INFO << "Took:" << diff << "ms to abort";
  // Curl takes ~2 seconds to call the progress meter and abort
  EXPECT_LE(0, diff);
  EXPECT_LE(diff, 3000);
  EXPECT_EQ(resp.curl_code, CURLE_ABORTED_BY_CALLBACK);
  abort_thread.join();
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Post) {
  HttpClient http;
  std::string path = "/path/1/2/3";
  Json::Value data;
  data["key"] = "val";

  Json::Value response = http.post(server + path, data).getJson();
  EXPECT_EQ(response["path"].asString(), path);
  EXPECT_EQ(response["data"]["key"].asString(), "val");
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, Put) {
  HttpClient http;
  std::string path = "/path/1/2/3";
  Json::Value data;
  data["key"] = "val";

  Json::Value json = http.put(server + path, data).getJson();

  EXPECT_EQ(json["path"].asString(), path);
  EXPECT_EQ(json["data"]["key"].asString(), "val");
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, UserAgent) {
  {
    // test the default, when setUserAgent hasn't been called yet
    HttpClient http;

    const auto resp = http.get(server + "/user_agent", HttpInterface::kNoLimit, nullptr);
    const auto app = resp.body.substr(0, resp.body.find('/'));
    EXPECT_EQ(app, "Aktualizr");
  }

  Utils::setUserAgent("blah");

  {
    HttpClient http;

    auto resp = http.get(server + "/user_agent", HttpInterface::kNoLimit, nullptr);
    EXPECT_EQ(resp.body, "blah");
  }
}

// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, UpdateHeader) {
  std::vector<std::string> headers = {"Authorization: Bearer bad"};
  HttpClient http(&headers);

  ASSERT_FALSE(http.updateHeader("NOSUCHHEADER", "foo"));

  std::string path = "/auth_call";
  std::string body = http.get(server + path, HttpInterface::kNoLimit, nullptr).body;
  EXPECT_EQ(body, "{}");

  ASSERT_TRUE(http.updateHeader("Authorization", "Bearer token"));
  Json::Value response = http.get(server + path, HttpInterface::kNoLimit, nullptr).getJson();
  EXPECT_EQ(response["status"].asString(), "good");
}

/* Verify that concurrent post() calls from multiple threads do not crash or
 * corrupt data.  The fake server is started with -f (intermittent 503s), so
 * we only assert that the curl call itself succeeded (no transport error) and
 * that every thread ran to completion. */
// NOLINTNEXTLINE(*non-const*)
TEST(HttpClient, ConcurrentPosts) {
  HttpClient http;

  std::vector<std::thread> threads;
  threads.reserve(4);
  std::vector<bool> results(4, false);
  for (int i = 0; i < 4; i++) {
    threads.emplace_back([&http, &results, i]() {
      for (int j = 0; j < 10; j++) {
        Json::Value data;
        data["thread"] = i;
        data["iteration"] = j;
        auto resp = http.post(server + "/events", data);
        EXPECT_EQ(resp.curl_code, CURLE_OK);
      }
      results[static_cast<size_t>(i)] = true;
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  for (int i = 0; i < 4; i++) {
    EXPECT_TRUE(results[static_cast<size_t>(i)]);
  }
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  std::string port = TestUtils::getFreePort();
  server += port;
  boost::process::child server_process("tests/fake_http_server/fake_test_server.py", port, "-f");
  TestUtils::waitForServer(server + "/");

  return RUN_ALL_TESTS();
}
#endif
