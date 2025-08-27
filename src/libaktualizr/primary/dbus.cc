#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>
#if !(defined(BUILD_DBUS) || defined(CLANG_TIDY))
#error "BUILD_DBUS or CLANG_TIDY must be defined"
#endif

#include "primary/dbus.h"

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "primary/consent.h"
#include "storage/invstorage.h"
#include "utilities/utils.h"

#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <memory>
#include <mutex>
#include "json/json.h"

// Constants for the D-Bus interface
// These form the external D-Bus interface for \aktualizr. Don't change them.

const char *const Dbus::Path = "/org/uptane/aktualizr";
const char *const Dbus::Interface = "org.uptane.Aktualizr";
const char *const Dbus::WellKnown = Dbus::Interface;
const char *const Dbus::InstallUpdatesAutomatically = "InstallUpdatesAutomatically";
const char *const Dbus::Cancel = "Cancel";
const char *const Dbus::CheckForUpdates = "CheckForUpdates";
const char *const Dbus::Consent = "Consent";
const char *const Dbus::ConsentRequired = "ConsentRequired";
const char *const Dbus::OfflineUpdate = "OfflineUpdate";

SdBus::SdBus(SdBus &&other) noexcept : ptr{other.ptr} { other.ptr = nullptr; }

SdBus::~SdBus() {
  if (ptr != nullptr) {
    sd_bus_unref(ptr);
    ptr = nullptr;
  }
}

class DbusCb {
 public:
  static int Cancel(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    auto *dbus = static_cast<Dbus *>(userdata);
    auto callback = dbus->cancel_callback();
    if (callback) {
      callback();
    } else {
      LOG_ERROR << "Cancel received but no callback set";
      return sd_bus_error_set(ret_error, SD_BUS_ERROR_NOT_SUPPORTED, nullptr);
    }
    return sd_bus_reply_method_return(m, "");
  }
  static int CheckForUpdates(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    auto *dbus = static_cast<Dbus *>(userdata);
    auto callback = dbus->check_for_updates_callback();
    if (callback) {
      callback();
    } else {
      LOG_ERROR << "CheckForUpdates received but no callback set";
      return sd_bus_error_set(ret_error, SD_BUS_ERROR_NOT_SUPPORTED, nullptr);
    }
    return sd_bus_reply_method_return(m, "");
  }
  static int Consent(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    auto *dbus = static_cast<Dbus *>(userdata);
    int granted;
    const char *reason = nullptr;  // Owned by msg, see man sd_bus_message_read_basic
    sd_bus_message_read_basic(m, 'b', &granted);
    sd_bus_message_read_basic(m, 's', &reason);
    {
      std::lock_guard guard{dbus->lock_};
      if (!dbus->current_consent_request_.empty()) {
        Consent::Outcome outcome;
        outcome.granted = granted != 0;
        outcome.was_cancelled = false;
        outcome.reason = reason;
        dbus->current_consent_promise_.set_value(std::move(outcome));
        dbus->current_consent_request_.clear();
      } else {
        LOG_WARNING << "Consent was granted over D-Bus when no request pending. Ignoring";
        return sd_bus_error_set(ret_error, SD_BUS_ERROR_FAILED, "No consent request is currently outstanding");
      }
    }

    return sd_bus_reply_method_return(m, "");
  }
  static int ConsentRequired(sd_bus * /* bus */, const char * /*path */, const char * /* interface */,
                             const char * /*property */, sd_bus_message *reply, void *userdata,
                             sd_bus_error *ret_error) {
    auto *dbus = static_cast<Dbus *>(userdata);
    (void)ret_error;
    std::lock_guard guard{dbus->lock_};
    return sd_bus_message_append_basic(reply, SD_BUS_TYPE_STRING, dbus->current_consent_request_.c_str());
  }

  static int GetInstallUpdatesAutomatically(sd_bus * /* bus */, const char * /*path */, const char * /* interface */,
                                            const char * /*property */, sd_bus_message *reply, void *userdata,
                                            sd_bus_error *ret_error) {
    auto *dbus = static_cast<Dbus *>(userdata);
    (void)ret_error;
    auto current = InstallUpdatesAutomatically::kProceed;
    dbus->storage_->loadInstallUpdatesAutomatically(&current);
    auto current_int = static_cast<int32_t>(current);
    return sd_bus_message_append_basic(reply, SD_BUS_TYPE_INT32, &current_int);
  }

  static int SetInstallUpdatesAutomatically(sd_bus * /* bus */, const char * /*path */, const char * /* interface */,
                                            const char * /*property */, sd_bus_message *value, void *userdata,
                                            sd_bus_error *ret_error) {
    auto *dbus = static_cast<Dbus *>(userdata);
    int new_value = 0;
    int res = sd_bus_message_read_basic(value, SD_BUS_TYPE_INT32, &new_value);
    if (res <= 0) {
      LOG_ERROR << "Could not read set request for InstallUpdatesAutomatically";
      return res;
    }
    if (new_value < 0 || static_cast<int32_t>(InstallUpdatesAutomatically::kLast) < new_value) {
      LOG_ERROR << "Trying to set a value for InstallUpdatesAutomatically this is out of range";
      return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS,
                               "Value for InstallUpdatesAutomatically is out of range '%d'", new_value);
    }
    dbus->storage_->storeInstallUpdatesAutomatically(static_cast<InstallUpdatesAutomatically>(new_value));
    return 0;
  }

  static int OfflineUpdate(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    const char *path_string = nullptr;
    //  path_string is owned by the message and does not need to be freed
    int res = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &path_string);
    if (res <= 0) {
      LOG_ERROR << "Could not read path parameter for OfflineUpdate D-Bus call";
      return res;
    }
    boost::filesystem::path path{path_string};
    if (!path.is_absolute()) {
      return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "OfflineUpdate path must be absolute");
    }

    boost::system::error_code ec;

    bool is_dir = boost::filesystem::is_directory(path, ec);
    if (ec.failed()) {
      return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "OfflineUpdate unable to access path");
    }
    if (!is_dir) {
      return sd_bus_error_setf(ret_error, SD_BUS_ERROR_INVALID_ARGS, "OfflineUpdate path must be a directory");
    }

    auto *dbus = static_cast<Dbus *>(userdata);
    auto callback = dbus->offline_update_callback();
    callback(path);
    return sd_bus_reply_method_return(m, "");
  }
};

// clang-format off
// NOLINTNEXTLINE Easier to use a C array here
static const sd_bus_vtable dbus_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD(Dbus::Cancel, "", "", DbusCb::Cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(Dbus::CheckForUpdates, "", "", DbusCb::CheckForUpdates, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(Dbus::Consent, "bs", "", DbusCb::Consent, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY(Dbus::ConsentRequired, "s", DbusCb::ConsentRequired, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY(Dbus::InstallUpdatesAutomatically, "i", DbusCb::GetInstallUpdatesAutomatically, DbusCb::SetInstallUpdatesAutomatically, 0, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(Dbus::OfflineUpdate, "s", "", DbusCb::OfflineUpdate, SD_BUS_VTABLE_UNPRIVILEGED),
    // NOLINTNEXTLINE(clang-diagnostic-missing-field-initializers)
    SD_BUS_VTABLE_END
};
// clang-format on

Dbus::Dbus(SdBus &&bus, std::shared_ptr<INvStorage> storage) : bus_{std::move(bus)}, storage_{std::move(storage)} {
  int r = sd_bus_add_object_vtable(bus_.ptr, &vtable_slot_, Dbus::Path, Dbus::Interface, dbus_vtable, this);
  if (r < 0) {
    throw std::system_error(-r, std::system_category(), "Failed to add sd_bus object");
  }
  r = pipe2(stop_fds_.data(), O_CLOEXEC);
  if (r != 0) {
    throw std::system_error(errno, std::system_category(), "Failed to create stop self-pipe");
  }
  dbus_thread_ = std::thread(&Dbus::Run, this);
}

Dbus::~Dbus() {
  Stop();
  dbus_thread_.join();
}

void Dbus::Run() {
  bool stop = false;
  while (!stop) {
    int messages_processed;
    do {
      messages_processed = sd_bus_process(bus_.ptr, nullptr);
    } while (messages_processed > 0);

    // Now wait for either a stop signal or a new message
    int res = sd_bus_get_fd(bus_.ptr);
    if (res < 0) {
      throw std::system_error(-res, std::system_category(), "sd_bus_get_fd failed");
    }
    int events = sd_bus_get_events(bus_.ptr);
    if (events < 0) {
      throw std::system_error(-res, std::system_category(), "sd_bus_get_events failed");
    }

    // NOLINTNEXTLINE(google-runtime-int)
    std::array<struct pollfd, 2> wait_fds{{{res, static_cast<short>(events), 0}, {stop_fds_[0], POLLIN, 0}}};

    uint64_t timeout_usec;  // absolute time
    res = sd_bus_get_timeout(bus_.ptr, &timeout_usec);
    if (res < 0) {
      throw std::system_error(-res, std::system_category(), "sd_bus_get_timeout");
    }
    // convert an absolute timeout relative to CLOCK_MONOTONIC to a relative
    // number of ms
    struct timespec now {};
    res = clock_gettime(CLOCK_MONOTONIC, &now);
    if (res != 0) {
      throw std::system_error(errno, std::system_category(), "clock_gettime failed");
    }

    res = poll(wait_fds.data(), 2, DiffTime(&now, timeout_usec));
    if (res < 0) {
      if (errno != EINTR) {
        LOG_WARNING << "Poll failed:" << errno;
      } else {
        LOG_DEBUG << "poll() interrupted by signal";
      }
      // In either case just loop round again
    }

    if ((wait_fds[1].revents & POLLIN) != 0) {
      LOG_DEBUG << "D-Bus Thread woken on wait_fds[1]";
      char op;
      ssize_t bytes_read = read(stop_fds_[0], &op, 1);
      if (bytes_read != 1) {
        LOG_WARNING << "Failed to read from stop_fds:" << errno;
      }
      if (op == 'x') {
        stop = true;
        LOG_TRACE << "DBus tending thread exiting...";
      }
      if (op == 'c') {
        LOG_TRACE << "Emiting signal for changed ConsentRequired property";
        sd_bus_emit_properties_changed(bus_.ptr, Dbus::Path, Dbus::Interface, Dbus::ConsentRequired, NULL);
      }
    }
  }
}

constexpr int Dbus::DiffTime(struct timespec *now, uint64_t systemd_abs_timeout) {
  if (systemd_abs_timeout >= std::numeric_limits<uint64_t>::max() - 999) {
    return std::numeric_limits<int>::max();
  }
  // Round the systemd time up and and now down. We might sleep 2ms longer
  // than desired, but that is OK for our use cases
  auto abs_timeout_ms = static_cast<int64_t>((systemd_abs_timeout + 999U) / 1000U);

  int64_t now_ms = now->tv_sec * 1000 + now->tv_nsec / 1000000;

  int64_t delta = abs_timeout_ms - now_ms;

  if (delta < 0) {
    return 0;
  }

  if (delta > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(delta);
}

void Dbus::Stop() noexcept {
  // Wake up polling loop
  ssize_t res = write(stop_fds_[1], "x", 1);
  if (res < 0) {
    // TODO: Figure out what to do here
    LOG_ERROR << "Failed to stop sd_event_loop:" << errno;
    // throw std::system_error(errno, std::system_category(),
    //                         "Failed to stop sd_event_loop");
  }
}

void Dbus::SetCheckForUpdatesCallback(std::function<void()> check_for_updates_callback) {
  std::lock_guard<std::mutex> guard{lock_};
  check_for_updates_callback_ = std::move(check_for_updates_callback);
}

void Dbus::SetCancelCallback(std::function<void()> cancel_callback) {
  std::lock_guard<std::mutex> guard{lock_};
  cancel_callback_ = std::move(cancel_callback);
}

void Dbus::SetOfflineUpdateCallback(std::function<void(const boost::filesystem::path &)> offline_update_callback) {
  std::lock_guard<std::mutex> guard{lock_};
  offline_update_callback_ = std::move(offline_update_callback);
}

std::future<Consent::Outcome> Dbus::GetConsent(const std::vector<Uptane::Target> &targets) {
  auto install_automatically = InstallUpdatesAutomatically::kProceed;

  storage_->loadInstallUpdatesAutomatically(&install_automatically);

  if (install_automatically == InstallUpdatesAutomatically::kProceed) {
    // No need for approval
    std::promise<Outcome> p;
    p.set_value({true, false, "User has not requested to approve updates"});
    return p.get_future();
  }

  std::future<Consent::Outcome> result;
  {
    // Build the new value of the 'Consent' property
    auto formated_targets = TargetsToJson(targets);

    // Now lock..
    std::lock_guard<std::mutex> guard{lock_};

    // Create a new promise. If there was already a consent request in flight,
    // then resolve the old one with a 'cancelled' outcome.
    std::promise<Consent::Outcome> promise;
    std::swap(promise, current_consent_promise_);
    if (!current_consent_request_.empty()) {  // Is the old promise alive?
      promise.set_value({false, true, "Replaced by new request"});
    }
    current_consent_request_ = Utils::jsonToStr(formated_targets);
    result = current_consent_promise_.get_future();
  }
  // Drop lock and wake the D-Bus thread
  ssize_t res = write(stop_fds_[1], "c", 1);
  if (res < 0) {
    LOG_ERROR << "Failed to wake up sd_bus thread:" << errno;
  }
  return result;
}

void Dbus::PendingUpdateCancelled() {
  {
    std::lock_guard<std::mutex> guard{lock_};

    if (current_consent_request_.empty()) {
      // Nothing in progress, no-op
      return;
    }
    // Clear out the property
    current_consent_request_.clear();

    // Resolve old promise as cancelled
    std::promise<Consent::Outcome> promise;
    std::swap(promise, current_consent_promise_);
    promise.set_value({false, true, "Cancelled"});
  }
  // Drop lock and wake the D-Bus thread
  ssize_t res = write(stop_fds_[1], "c", 1);
  if (res < 0) {
    LOG_ERROR << "Failed to wake up sd_bus thread:" << errno;
  }
}
