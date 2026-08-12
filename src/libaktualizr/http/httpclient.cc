#include "httpclient.h"

#include <cassert>
#include <sstream>
#include <utility>

#include "logging/logging.h"
#include "utilities/utils.h"

struct WriteStringArg {
  std::string out;
  int64_t limit{0};
};

/*****************************************************************************/
/**
 * \par Description:
 *    A writeback handler for the curl library. It handles writing response
 *    data from curl into a string.
 *    https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
 *
 */
static size_t writeString(void* contents, size_t size, size_t nmemb, void* userp) {
  assert(contents);
  assert(userp);
  // append the writeback data to the provided string
  auto* arg = static_cast<WriteStringArg*>(userp);
  if (arg->limit > 0) {
    if (arg->out.length() + size * nmemb > static_cast<uint64_t>(arg->limit)) {
      return 0;
    }
  }
  (static_cast<WriteStringArg*>(userp))->out.append(static_cast<char*>(contents), size * nmemb);

  // return size of written data
  return size * nmemb;
}

static int ProgressHandler(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  (void)dltotal;
  (void)dlnow;
  (void)ultotal;
  (void)ulnow;
  if (clientp == nullptr) {
    LOG_ERROR << "ProgressHandler given null user pointer";
    return 0;  // Let the download continue
  }

  auto* token = static_cast<api::FlowControlToken*>(clientp);

  if (!token->IsValid()) {
    LOG_ERROR << "ProgressHandler given a thing that isn't a FlowControlToken";
    return 0;
  }

  if (token->hasAborted()) {
    // Abort download
    return 1;
  }
  return 0;
}

HttpClient::HttpClient(const std::vector<std::string>* extra_headers) : template_handle_{curl_easy_init(), nullptr} {
  if (template_handle_.get() == nullptr) {
    throw std::runtime_error("Could not initialize curl");
  }

  template_handle_.setopt(CURLOPT_NOSIGNAL, 1L);
  template_handle_.setopt(CURLOPT_TIMEOUT, 60L);
  template_handle_.setopt(CURLOPT_CONNECTTIMEOUT, 60L);
  template_handle_.setopt(CURLOPT_CAPATH, Utils::getCaPath());

  template_handle_.setopt(CURLOPT_FOLLOWLOCATION, 1L);
  template_handle_.setopt(CURLOPT_MAXREDIRS, 10L);
  template_handle_.setopt(CURLOPT_POSTREDIR, CURL_REDIR_POST_301);

  // let curl use our write function
  template_handle_.setopt(CURLOPT_WRITEFUNCTION, writeString);
  template_handle_.setopt(CURLOPT_WRITEDATA, NULL);

  template_handle_.setopt(CURLOPT_VERBOSE, get_curlopt_verbose());

  template_handle_.appendHeader("Accept: */*");

  if (extra_headers != nullptr) {
    for (const auto& header : *extra_headers) {
      template_handle_.appendHeader(header);
    }
  }
  template_handle_.setopt(CURLOPT_USERAGENT, Utils::getUserAgent());
}

HttpClient::HttpClient(const std::string& socket) : HttpClient() {
  template_handle_.setopt(CURLOPT_UNIX_SOCKET_PATH, socket.c_str());
}

HttpClient::HttpClient(const HttpClient& curl_in)
    : HttpInterface(curl_in),
      template_handle_(curl_easy_duphandle(curl_in.template_handle_.get()),
                       curl_slist_dup(curl_in.template_handle_.headers())),
      pkcs11_key(curl_in.pkcs11_key),
      pkcs11_cert(curl_in.pkcs11_key) {}

const CurlGlobalInitWrapper HttpClient::manageCurlGlobalInit_{};

HttpClient::CurlHandle HttpClient::dupCurl() {
  std::lock_guard<std::mutex> lock(curl_mutex_);
  CurlHandle res(Utils::curlDupHandleWrapper(template_handle_.get(), pkcs11_key),
                 curl_slist_dup(template_handle_.headers()));
  res.setopt(CURLOPT_HTTPHEADER, res.headers());
  if (pkcs11_cert) {
    res.setopt(CURLOPT_SSLCERTTYPE, "ENG");
  }
  res.setopt(CURLOPT_LOW_SPEED_TIME, speed_limit_time_interval_);
  res.setopt(CURLOPT_LOW_SPEED_LIMIT, speed_limit_bytes_per_sec_);
  return res;
}

void HttpClient::setCerts(const std::string& ca, CryptoSource ca_source, const std::string& cert,
                          CryptoSource cert_source, const std::string& pkey, CryptoSource pkey_source) {
  std::lock_guard<std::mutex> lock(curl_mutex_);
  template_handle_.setopt(CURLOPT_SSL_VERIFYPEER, 1);
  template_handle_.setopt(CURLOPT_SSL_VERIFYHOST, 2);
  template_handle_.setopt(CURLOPT_USE_SSL, CURLUSESSL_ALL);

  if (ca_source == CryptoSource::kPkcs11) {
    throw std::runtime_error("Accessing CA certificate on PKCS11 devices isn't currently supported");
  }
  std::unique_ptr<TemporaryFile> tmp_ca_file = std_::make_unique<TemporaryFile>("tls-ca");
  tmp_ca_file->PutContents(ca);
  template_handle_.setopt(CURLOPT_CAINFO, tmp_ca_file->Path().c_str());
  tls_ca_file = std::move_if_noexcept(tmp_ca_file);

  if (cert_source == CryptoSource::kPkcs11) {
    template_handle_.setopt(CURLOPT_SSLCERT, cert.c_str());
    template_handle_.setopt(CURLOPT_SSLCERTTYPE, "ENG");
  } else {  // cert_source == CryptoSource::kFile
    std::unique_ptr<TemporaryFile> tmp_cert_file = std_::make_unique<TemporaryFile>("tls-cert");
    tmp_cert_file->PutContents(cert);
    template_handle_.setopt(CURLOPT_SSLCERT, tmp_cert_file->Path().c_str());
    template_handle_.setopt(CURLOPT_SSLCERTTYPE, "PEM");
    tls_cert_file = std::move_if_noexcept(tmp_cert_file);
  }
  pkcs11_cert = (cert_source == CryptoSource::kPkcs11);

  if (pkey_source == CryptoSource::kPkcs11) {
    template_handle_.setopt(CURLOPT_SSLENGINE, "pkcs11");
    template_handle_.setopt(CURLOPT_SSLENGINE_DEFAULT, 1L);
    template_handle_.setopt(CURLOPT_SSLKEY, pkey.c_str());
    template_handle_.setopt(CURLOPT_SSLKEYTYPE, "ENG");
  } else {  // pkey_source == CryptoSource::kFile
    std::unique_ptr<TemporaryFile> tmp_pkey_file = std_::make_unique<TemporaryFile>("tls-pkey");
    tmp_pkey_file->PutContents(pkey);
    template_handle_.setopt(CURLOPT_SSLKEY, tmp_pkey_file->Path().c_str());
    template_handle_.setopt(CURLOPT_SSLKEYTYPE, "PEM");
    tls_pkey_file = std::move_if_noexcept(tmp_pkey_file);
  }
  pkcs11_key = (pkey_source == CryptoSource::kPkcs11);
}

HttpResponse HttpClient::get(const std::string& url, int64_t maxsize, const api::FlowControlToken* flow_control,
                             const Headers* extra_headers) {
  auto curl_get = dupCurl();
  if (extra_headers != nullptr) {
    for (const auto& header : *extra_headers) {
      curl_get.appendHeader(header);
    }
    // appendHeader may update the list head; keep CURLOPT_HTTPHEADER in sync.
    curl_get.setopt(CURLOPT_HTTPHEADER, curl_get.headers());
  }

  // Clear POSTFIELDS to remove any lingering references to strings that have
  // probably since been deallocated.
  curl_get.setopt(CURLOPT_POSTFIELDS, "");
  curl_get.setopt(CURLOPT_URL, url.c_str());
  curl_get.setopt(CURLOPT_HTTPGET, 1L);
  if (flow_control != nullptr) {
    // Handle cancellation
    curl_get.setopt(CURLOPT_NOPROGRESS, 0);
    curl_get.setopt(CURLOPT_XFERINFOFUNCTION, ProgressHandler);
    curl_get.setopt(CURLOPT_XFERINFODATA, flow_control);
  }

  LOG_DEBUG << "GET " << url;
  return perform(curl_get.get(), RETRY_TIMES, maxsize);
}

std::string HttpClient::getEffectiveUrl(const std::string& url) {
  auto curl_resolve = dupCurl();
  if (curl_resolve.get() == nullptr) {
    return "";
  }
  curl_resolve.setopt(CURLOPT_POSTFIELDS, "");
  curl_resolve.setopt(CURLOPT_URL, url.c_str());
  curl_resolve.setopt(CURLOPT_HTTPGET, 1L);
  // Use a range request to minimise body transfer while still following
  // redirects. Unlike CURLOPT_NOBODY (HEAD), range GETs follow 3xx chains.
  // If the server ignores Range and sends the full body, cap transfer size so
  // we don't pull a full image. We perform directly (not via perform()) so
  // CURLE_FILESIZE_EXCEEDED is only logged at DEBUG and we still return the
  // effective URL.
  curl_resolve.setopt(CURLOPT_RANGE, "0-0");
  curl_resolve.setopt(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(1));

  // Perform directly: we don't need perform()'s retry/error-logging since we
  // only care about CURLINFO_EFFECTIVE_URL, not the response body or status.
  WriteStringArg discard;
  curl_resolve.setopt(CURLOPT_WRITEDATA, static_cast<void*>(&discard));
  CURLcode res = curl_easy_perform(curl_resolve.get());

  char* effective_url = nullptr;
  curl_easy_getinfo(curl_resolve.get(), CURLINFO_EFFECTIVE_URL, &effective_url);

  if (res != CURLE_OK) {
    LOG_DEBUG << "getEffectiveUrl: curl error " << res << " (" << curl_easy_strerror(res) << ") for " << url;
  }

  return (effective_url != nullptr) ? effective_url : "";
}

HttpResponse HttpClient::post(const std::string& url, const std::string& content_type, const std::string& data) {
  auto guard = dupCurl();
  guard.appendHeader("Content-Type: " + content_type);
  guard.setopt(CURLOPT_URL, url.c_str());
  guard.setopt(CURLOPT_POST, 1);
  guard.setopt(CURLOPT_POSTFIELDS, data.c_str());
  return perform(guard.get(), RETRY_TIMES, HttpInterface::kPostRespLimit);
}

HttpResponse HttpClient::post(const std::string& url, const Json::Value& data) {
  std::string data_str = Utils::jsonToCanonicalStr(data);
  LOG_TRACE << "post request body:" << data;
  return post(url, "application/json", data_str);
}

HttpResponse HttpClient::put(const std::string& url, const std::string& content_type, const std::string& data) {
  auto guard = dupCurl();
  guard.appendHeader("Content-Type: " + content_type);
  guard.setopt(CURLOPT_URL, url.c_str());
  guard.setopt(CURLOPT_POSTFIELDS, data.c_str());
  guard.setopt(CURLOPT_CUSTOMREQUEST, "PUT");
  return perform(guard.get(), RETRY_TIMES, HttpInterface::kPutRespLimit);
}

HttpResponse HttpClient::put(const std::string& url, const Json::Value& data) {
  std::string data_str = Utils::jsonToCanonicalStr(data);
  LOG_TRACE << "put request body:" << data;
  return put(url, "application/json", data_str);
}

// NOLINTNEXTLINE(misc-no-recursion)
HttpResponse HttpClient::perform(CURL* curl_handler, int retry_times, int64_t size_limit) {
  if (size_limit >= 0) {
    // it will only take effect if the server declares the size in advance,
    //    writeString callback takes care of the other case
    curlEasySetoptWrapper(curl_handler, CURLOPT_MAXFILESIZE_LARGE, size_limit);
  }
  WriteStringArg response_arg;
  response_arg.limit = size_limit;
  curlEasySetoptWrapper(curl_handler, CURLOPT_WRITEDATA, static_cast<void*>(&response_arg));
  CURLcode result = curl_easy_perform(curl_handler);
  long http_code;  // NOLINT(google-runtime-int)
  curl_easy_getinfo(curl_handler, CURLINFO_RESPONSE_CODE, &http_code);
  HttpResponse response(response_arg.out, http_code, result, (result != CURLE_OK) ? curl_easy_strerror(result) : "");
  if (response.curl_code != CURLE_OK || response.http_status_code >= 500) {
    std::ostringstream error_message;
    error_message << "curl error " << response.curl_code << " (http code " << response.http_status_code
                  << "): " << response.error_message;
    LOG_ERROR << error_message.str();
    if (retry_times != 0) {
      sleep(1);
      // NOLINTNEXTLINE(misc-no-recursion)
      response = perform(curl_handler, --retry_times, size_limit);
    }
  }
  LOG_TRACE << "response http code: " << response.http_status_code;
  LOG_TRACE << "response: " << response.body;
  return response;
}

HttpResponse HttpClient::download(const std::string& url, curl_write_callback write_cb,
                                  curl_xferinfo_callback progress_cb, void* userp, curl_off_t from) {
  return downloadAsync(url, write_cb, progress_cb, userp, from).get();
}

std::future<HttpResponse> HttpClient::downloadAsync(const std::string& url, curl_write_callback write_cb,
                                                    curl_xferinfo_callback progress_cb, void* userp, curl_off_t from) {
  auto curl_download = dupCurl();

  curl_download.setopt(CURLOPT_URL, url.c_str());
  curl_download.setopt(CURLOPT_HTTPGET, 1L);
  curl_download.setopt(CURLOPT_WRITEFUNCTION, write_cb);
  curl_download.setopt(CURLOPT_WRITEDATA, userp);
  if (progress_cb != nullptr) {
    curl_download.setopt(CURLOPT_NOPROGRESS, 0);
    curl_download.setopt(CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_download.setopt(CURLOPT_XFERINFODATA, userp);
  }
  curl_download.setopt(CURLOPT_TIMEOUT, 0);
  curl_download.setopt(CURLOPT_RESUME_FROM_LARGE, from);

  std::promise<HttpResponse> resp_promise;
  auto resp_future = resp_promise.get_future();
  std::thread(
      [handle = std::move(curl_download)](std::promise<HttpResponse> promise) {
        CURLcode result = curl_easy_perform(handle.get());
        long http_code;  // NOLINT(google-runtime-int)
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_code);
        HttpResponse response("", http_code, result, (result != CURLE_OK) ? curl_easy_strerror(result) : "");
        promise.set_value(response);
      },
      std::move(resp_promise))
      .detach();
  return resp_future;
}

bool HttpClient::updateHeader(const std::string& name, const std::string& value) {
  curl_slist* item = template_handle_.headers();
  std::string lookfor(name + ": ");

  while (item != nullptr) {
    if (strncmp(lookfor.c_str(), item->data, lookfor.length()) == 0) {
      free(item->data);  // NOLINT(cppcoreguidelines-no-malloc, hicpp-no-malloc)
      lookfor += value;
      item->data = strdup(lookfor.c_str());
      return true;
    }
    item = item->next;
  }
  return false;
}

void HttpClient::timeout(int64_t ms) {
  std::lock_guard<std::mutex> lock(curl_mutex_);
  // curl_easy_setopt() takes a 'long' be very sure that we are passing
  // whatever the platform ABI thinks is a long, while keeping the external
  // interface a clang-tidy preferred int64
  auto ms_long = static_cast<long>(ms);  // NOLINT(google-runtime-int)
  template_handle_.setopt(CURLOPT_TIMEOUT_MS, ms_long);
  template_handle_.setopt(CURLOPT_CONNECTTIMEOUT_MS, ms_long);
}

curl_slist* HttpClient::curl_slist_dup(curl_slist* sl) {
  curl_slist* new_list = nullptr;

  for (curl_slist* item = sl; item != nullptr; item = item->next) {
    new_list = curl_slist_append(new_list, item->data);
  }

  return new_list;
}

// vim: set tabstop=2 shiftwidth=2 expandtab:
