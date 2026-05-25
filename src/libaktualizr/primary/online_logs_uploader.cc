#include "primary/online_logs_uploader.h"

#ifdef BUILD_OFFLINE_UPDATES

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include <json/json.h>

#include "http/httpinterface.h"
#include "logging/logging.h"
#include "primary/journal.h"

// ============================================================================
// UploaderThread — lives only between Begin() and End()
// ============================================================================

class OnlineLogsUploader::UploaderThread {
 public:
  UploaderThread(std::string correlation_id, const std::vector<std::string>& capture_services, std::string server_url,
                 std::shared_ptr<HttpInterface> http, std::unique_ptr<JournalHandle> journal)
      : correlation_id_(std::move(correlation_id)),
        server_url_(std::move(server_url)),
        http_(std::move(http)),
        journal_(std::move(journal)),
        filter_(capture_services) {
    if (!journal_ || !*journal_) {
      LOG_WARNING << "OnlineLogsUploader: failed to open journal, log streaming disabled for this update";
      return;
    }

    // Position at the tail — the worker thread will read entries that
    // arrive *after* this point.
    if (!journal_->MoveTail()) {
      LOG_DEBUG << "OnlineLogsUploader: journal is empty, will stream from start";
    }

    thread_ = std::thread(&UploaderThread::Run, this);
  }

  ~UploaderThread() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  UploaderThread(const UploaderThread&) = delete;
  UploaderThread& operator=(const UploaderThread&) = delete;
  UploaderThread(UploaderThread&&) = delete;
  UploaderThread& operator=(UploaderThread&&) = delete;

 private:
  // -- background thread entry point -----------------------------------------

  void Run() {
    // Each iteration reads one batch and POSTs it.  If the batch was
    // full (hit kMaxBatchSize) we loop immediately; otherwise we wait
    // for the batch interval or a shutdown notification.  When shutdown_
    // is set the thread wakes, does one final batch, and exits.
    bool running = (journal_ && *journal_);
    Json::Value body;
    body["correlationId"] = correlation_id_;

    while (running) {
      Json::Value logs(Json::arrayValue);
      // read up to kMaxBatchSize matching entries
      while (logs.size() < kMaxBatchSize) {
        int ret = journal_->Next();
        if (ret < 0) {
          LOG_WARNING << "OnlineLogsUploader: error reading journal: " << strerror(-ret);
          break;
        }
        if (ret == 0) {
          break;  // no more entries
        }

        if (!filter_.ShouldCapture(*journal_)) {
          continue;
        }

        std::string unit = journal_->GetField("_SYSTEMD_UNIT");
        std::string message = journal_->GetField("MESSAGE");
        int64_t timestamp_us = journal_->GetTimestamp();
        if (message.empty() || timestamp_us <= 0) {
          continue;
        }

        // Cursor of the first entry is the batch idempotency key.
        if (logs.empty()) {
          body["startCursor"] = journal_->GetCurrentCursor();
        }

        Json::Value entry;
        entry["timestamp"] = OnlineLogsUploader::FormatTimestamp(timestamp_us);
        entry["service"] = unit;
        entry["message"] = message;
        logs.append(std::move(entry));
      }

      // PUT the batch
      if (!logs.empty()) {
        body["logs"] = logs;

        try {
          auto response = http_->put(server_url_, body);

          if (response.http_status_code == 404) {
            LOG_INFO << "OnlineLogsUploader: server does not support /install_logs (404), "
                        "disabling for this update";
            running = false;
          } else if (response.http_status_code == 429) {
            LOG_WARNING << "OnlineLogsUploader: hit rate limit disabling for this update";
            running = false;
          } else if (!response.isOk()) {
            LOG_WARNING << "OnlineLogsUploader: PUT failed: " << response.getStatusStr();
          } else {
            LOG_DEBUG << "OnlineLogsUploader: sent " << logs.size() << " entries";
          }
        } catch (const std::exception& e) {
          LOG_WARNING << "OnlineLogsUploader: exception during PUT: " << e.what();
        }
      }

      // Shutdown if we're done
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_) {
          running = false;
        } else if (logs.size() >= kMaxBatchSize) {
          // We got to the maximum batch size rather than the end of the journal
          // continue on the next batch 'soon', but wait a few hundred ms as a safety
          // net. Note that if a shutdown is triggered while we are behind on the logs
          // (i.e. hitting kMaxBatchSize) then we'll send one more batch then exit.
          // This protects against a run-away systemd journal delaying the rest of the system.
          cv_.wait_for(lock, kBatchMinInterval);
        } else {
          // Most of the time the loop is waiting here. Note that shutdown will trigger
          // one last loop, which is what we want to flush the tail of the logs.
          cv_.wait_for(lock, kBatchInterval);
        }
      }
    }
  }

  std::string correlation_id_;
  std::string server_url_;
  std::shared_ptr<HttpInterface> http_;

  std::unique_ptr<JournalHandle> journal_;
  JournalFilter filter_;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool shutdown_{false};

  static constexpr size_t kMaxBatchSize = 1000;
  static constexpr std::chrono::seconds kBatchInterval{5};
  static constexpr std::chrono::milliseconds kBatchMinInterval{100};
};

// ============================================================================
// OnlineLogsUploader — the "always present" part
// ============================================================================

OnlineLogsUploader::OnlineLogsUploader(const Config& config, std::shared_ptr<HttpInterface> http,
                                       std::shared_ptr<JournalHandle> journal_prototype)
    : enabled_(config.logger.online_logs_enabled),
      capture_services_(config.logger.capture_services),
      server_url_(
          config.tls.server +
          "/install_logs"),  // should be dgw.torizon.io/install_logs. NOT dgw.torizon.io/director/install_logs !
      http_(std::move(http)),
      journal_prototype_(std::move(journal_prototype)) {
  if (!enabled_) {
    LOG_DEBUG << "OnlineLogsUploader: disabled via configuration";
  }
}

OnlineLogsUploader::~OnlineLogsUploader() { End(); }

void OnlineLogsUploader::Begin(const std::string& correlation_id) {
  if (!enabled_) {
    return;
  }

  if (uploader_thread_) {
    LOG_WARNING << "OnlineLogsUploader::Begin called while already active, ending previous session";
    End();
  }

  if (!journal_prototype_) {
    LOG_WARNING << "OnlineLogsUploader: no journal source available, log streaming disabled for this update";
    return;
  }

  LOG_INFO << "OnlineLogsUploader: starting log streaming for correlation_id=" << correlation_id;
  uploader_thread_ = std::make_unique<UploaderThread>(correlation_id, capture_services_, server_url_, http_,
                                                      journal_prototype_->Clone());
}

void OnlineLogsUploader::End() {
  if (!uploader_thread_) {
    return;
  }

  LOG_INFO << "OnlineLogsUploader: stopping log streaming";

  // Destructor sets shutdown_, notifies the thread, and joins it.
  // The thread does one final drain before exiting.
  uploader_thread_.reset();
}

// -- FormatTimestamp ---------------------------------------------------------

std::string OnlineLogsUploader::FormatTimestamp(int64_t timestamp_us) {
  auto seconds = static_cast<time_t>(timestamp_us / 1000000);
  auto micros = static_cast<int>(timestamp_us % 1000000);

  struct tm tm{};
  gmtime_r(&seconds, &tm);

  std::array<char, 64> buf{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg, hicpp-vararg)
  snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, micros);
  return {buf.data()};
}

#endif  // BUILD_OFFLINE_UPDATES
