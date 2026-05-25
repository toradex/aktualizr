#ifndef PRIMARY_ONLINE_LOGS_UPLOADER_H_
#define PRIMARY_ONLINE_LOGS_UPLOADER_H_

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "libaktualizr/config.h"

class HttpInterface;
class JournalHandle;

#ifdef BUILD_OFFLINE_UPDATES

/**
 * Streams systemd journal logs to the server during online updates.
 *
 * This is the "always present" part — owned by SotaUptaneClient as a plain
 * member. It holds configuration and the shared HttpInterface pointer.
 *
 * The actual background upload work happens in the nested UploaderThread
 * class, which is created by Begin() and destroyed by End(). Between those
 * calls, uploader_thread_ holds the journal handle, worker thread, and all
 * synchronisation state.
 */
class OnlineLogsUploader {
 public:
  /**
   * @param journal_prototype Prototype journal handle, cloned via Clone() to
   *        obtain a fresh handle each time streaming begins. Dependency
   *        injected so tests can supply a TestJournal.
   */
  OnlineLogsUploader(const Config& config, std::shared_ptr<HttpInterface> http,
                     std::shared_ptr<JournalHandle> journal_prototype);
  ~OnlineLogsUploader();

  OnlineLogsUploader(const OnlineLogsUploader&) = delete;
  OnlineLogsUploader& operator=(const OnlineLogsUploader&) = delete;
  OnlineLogsUploader(OnlineLogsUploader&&) = delete;
  OnlineLogsUploader& operator=(OnlineLogsUploader&&) = delete;

  /**
   * Start streaming journal logs for this update correlation.
   * Opens a journal handle, seeks to the tail, and launches the
   * background upload thread.
   */
  void Begin(const std::string& correlation_id);

  /**
   * Stop the background thread, draining remaining logs before returning.
   * Called before reboot or at install completion.
   */
  void End();

  bool IsEnabled() const { return enabled_; }
  bool IsActive() const { return uploader_thread_ != nullptr; }

  /**
   * Format a microsecond-since-epoch timestamp as ISO 8601 UTC.
   * Example: 1731142800123456 → "2024-11-09T10:00:00.123456Z"
   */
  static std::string FormatTimestamp(int64_t timestamp_us);

 private:
  // Defined in online_logs_uploader.cc — holds the journal handle,
  // worker thread, and all state that only exists while streaming.
  class UploaderThread;

  bool enabled_;
  std::vector<std::string> capture_services_;
  std::string server_url_;
  std::shared_ptr<HttpInterface> http_;
  std::shared_ptr<JournalHandle> journal_prototype_;

  std::unique_ptr<UploaderThread> uploader_thread_;
};

#else  // !BUILD_OFFLINE_UPDATES

/**
 * Stub: all methods are no-ops when BUILD_OFFLINE_UPDATES is off.
 */
class OnlineLogsUploader {
 public:
  OnlineLogsUploader(const Config& /*config*/, std::shared_ptr<HttpInterface> /*http*/,
                     std::shared_ptr<JournalHandle> /*journal_prototype*/) {}
  ~OnlineLogsUploader() = default;

  OnlineLogsUploader(const OnlineLogsUploader&) = delete;
  OnlineLogsUploader& operator=(const OnlineLogsUploader&) = delete;
  OnlineLogsUploader(OnlineLogsUploader&&) = delete;
  OnlineLogsUploader& operator=(OnlineLogsUploader&&) = delete;

  void Begin(const std::string& /*correlation_id*/) {}
  void End() {}
  static bool IsEnabled() { return false; }
  static bool IsActive() { return false; }
  static std::string FormatTimestamp(int64_t /*timestamp_us*/) { return ""; }
};

#endif  // BUILD_OFFLINE_UPDATES

#endif  // PRIMARY_ONLINE_LOGS_UPLOADER_H_
