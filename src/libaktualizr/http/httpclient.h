#ifndef HTTPCLIENT_H_
#define HTTPCLIENT_H_

#include <future>
#include <memory>
#include <mutex>

#include <curl/curl.h>
#include "gtest/gtest_prod.h"
#include "json/json.h"

#include "httpinterface.h"

/**
 * Helper class to manage curl_global_init/curl_global_cleanup calls
 */
class CurlGlobalInitWrapper {
 public:
  CurlGlobalInitWrapper() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobalInitWrapper() { curl_global_cleanup(); }
  CurlGlobalInitWrapper(const CurlGlobalInitWrapper &) = delete;
  CurlGlobalInitWrapper(CurlGlobalInitWrapper &&) = delete;
  CurlGlobalInitWrapper &operator=(const CurlGlobalInitWrapper &) = delete;
  CurlGlobalInitWrapper &operator=(CurlGlobalInitWrapper &&) = delete;
};

class HttpClient : public HttpInterface {
 public:
  explicit HttpClient(const std::vector<std::string> *extra_headers = nullptr);
  explicit HttpClient(const std::string &socket);
  HttpClient(const HttpClient &curl_in);  // non-default!
  ~HttpClient() override = default;
  HttpClient(HttpClient &&) = delete;
  HttpClient &operator=(const HttpClient &) = delete;
  HttpClient &operator=(HttpClient &&) = delete;
  using HttpInterface::get;
  HttpResponse get(const std::string &url, int64_t maxsize, const api::FlowControlToken *flow_control,
                   const Headers *extra_headers) override;
  std::string getEffectiveUrl(const std::string &url) override;
  HttpResponse post(const std::string &url, const std::string &content_type, const std::string &data) override;
  HttpResponse post(const std::string &url, const Json::Value &data) override;
  HttpResponse put(const std::string &url, const std::string &content_type, const std::string &data) override;
  HttpResponse put(const std::string &url, const Json::Value &data) override;

  HttpResponse download(const std::string &url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void *userp, curl_off_t from) override;

  /**
   * Note it is the responsibility of the caller to keep url alive and
   * unmodified until the returned future resolves.
   */
  std::future<HttpResponse> downloadAsync(const std::string &url, curl_write_callback write_cb,
                                          curl_xferinfo_callback progress_cb, void *userp, curl_off_t from) override;
  void setCerts(const std::string &ca, CryptoSource ca_source, const std::string &cert, CryptoSource cert_source,
                const std::string &pkey, CryptoSource pkey_source) override;
  /**
   * Update the value of an existing HTTP header.
   *
   * @warning This method is NOT thread-safe. It mutates the shared headers
   * list without synchronisation. Callers must ensure no concurrent HTTP
   * requests are in flight when calling this method. In practice, this
   * means it should only be called from the main thread before or between
   * update cycles, not during concurrent operations.
   *
   * @return true if the header was found and updated, false otherwise.
   */
  bool updateHeader(const std::string &name, const std::string &value);

 private:
  FRIEND_TEST(HttpClient, DownloadSpeedLimit);
  friend bool doTestInit(const std::string &, const std::string &);

  void timeout(int64_t ms);

  /**
   * RAII wrapper around a CURL* handle and its owned header list.
   * Movable, non-copyable. The destructor calls curl_easy_cleanup and
   * curl_slist_free_all on the owned resources (if non-null).
   */
  class CurlHandle {
   public:
    CurlHandle() = default;
    CurlHandle(CURL *curl, curl_slist *headers) : curl_(curl), headers_(headers) {}
    ~CurlHandle() {
      curl_slist_free_all(headers_);
      curl_easy_cleanup(curl_);
    }

    CurlHandle(CurlHandle &&other) noexcept : curl_(other.curl_), headers_(other.headers_) {
      other.curl_ = nullptr;
      other.headers_ = nullptr;
    }
    CurlHandle &operator=(CurlHandle &&other) noexcept {
      if (this != &other) {
        curl_slist_free_all(headers_);
        curl_easy_cleanup(curl_);
        curl_ = other.curl_;
        headers_ = other.headers_;
        other.curl_ = nullptr;
        other.headers_ = nullptr;
      }
      return *this;
    }
    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;

    CURL *get() const { return curl_; }
    curl_slist *headers() const { return headers_; }

    template <typename... T>
    void setopt(CURLoption option, T &&...args) {
      curlEasySetoptWrapper(curl_, option, std::forward<T>(args)...);
    }

    void appendHeader(const std::string &header) { headers_ = curl_slist_append(headers_, header.c_str()); }

   private:
    CURL *curl_{nullptr};
    curl_slist *headers_{nullptr};
  };

  /**
   * Duplicate the template curl handle and headers under the mutex.
   * Sets CURLOPT_HTTPHEADER, CURLOPT_SSLCERTTYPE (if pkcs11), and
   * the speed-limit options on the new handle.
   */
  CurlHandle dupCurl();

  mutable std::mutex curl_mutex_;
  static const CurlGlobalInitWrapper manageCurlGlobalInit_;
  CurlHandle template_handle_;
  HttpResponse perform(CURL *curl_handler, int retry_times, int64_t size_limit);
  static curl_slist *curl_slist_dup(curl_slist *sl);

  std::unique_ptr<TemporaryFile> tls_ca_file;
  std::unique_ptr<TemporaryFile> tls_cert_file;
  std::unique_ptr<TemporaryFile> tls_pkey_file;
  static const int RETRY_TIMES = 2;
  static const long kSpeedLimitTimeInterval = 60L;   // NOLINT(google-runtime-int)
  static const long kSpeedLimitBytesPerSec = 5000L;  // NOLINT(google-runtime-int)

  long speed_limit_time_interval_{kSpeedLimitTimeInterval};                // NOLINT(google-runtime-int)
  long speed_limit_bytes_per_sec_{kSpeedLimitBytesPerSec};                 // NOLINT(google-runtime-int)
  void overrideSpeedLimitParams(long time_interval, long bytes_per_sec) {  // NOLINT(google-runtime-int)
    speed_limit_time_interval_ = time_interval;
    speed_limit_bytes_per_sec_ = bytes_per_sec;
  }
  bool pkcs11_key{false};
  bool pkcs11_cert{false};
};
#endif
