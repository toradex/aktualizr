#ifndef BUILD_DBUS
#error "BUILD_DBUS not defined"
#endif

#include "primary/dbus.h"
#include "logging/logging.h"

#include <fcntl.h>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <iostream>

const char *const Dbus::Path = "/org/uptane/aktualizr";
const char *const Dbus::Interface = "org.uptane.aktualizr";
const char *const Dbus::WellKnown = Dbus::Interface;

SdBus::SdBus(SdBus &&other) noexcept : ptr{other.ptr} { other.ptr = nullptr; }

SdBus::~SdBus() {
  if (ptr != nullptr) {
    sd_bus_unref(ptr);
    ptr = nullptr;
  }
}

class DbusCb {
 public:
  static int ShoulderTap(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    (void)ret_error;
    auto *dbus = static_cast<Dbus *>(userdata);
    if (dbus->shoulder_tap_callback_) {
      dbus->shoulder_tap_callback_();
    } else {
      LOG_ERROR << "Shoulder tap received but shoulder_tap_callback_ is not set";
    }
    return sd_bus_reply_method_return(m, "");
  }
};

// clang-format off
// NOLINTNEXTLINE Easier to use a C array here
static const sd_bus_vtable dbus_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ShoulderTap", "", "", DbusCb::ShoulderTap, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
// clang-format on

Dbus::Dbus(SdBus &&bus) : bus_{std::move(bus)} {
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
  while (!stop_) {
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
    // convert an absolute timeout relative to CLOCK_MONOTONIC to a relative
    // number of ms
    struct timespec now {};
    res = clock_gettime(CLOCK_MONOTONIC, &now);
    if (res != 0) {
      throw std::system_error(errno, std::system_category(), "clock_gettime failed");
    }

    res = poll(wait_fds.data(), 2, DiffTime(&now, timeout_usec));
  }
}

int Dbus::DiffTime(struct timespec *now, uint64_t systemd_abs_timeout) {
  if (systemd_abs_timeout >= std::numeric_limits<uint64_t>::max() - 999) {
    return std::numeric_limits<int>::max();
  }
  // Round the systemd time up and and now down. We might sleep 2ms longer
  // than desired, but that is OK for our use cases
  int64_t abs_timeout_ms = (systemd_abs_timeout + 999U) / 1000U;

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
  stop_ = true;
  // Wake up polling loop
  ssize_t res = write(stop_fds_[1], "x", 1);
  if (res < 0) {
    // TODO: Figure out what to do here
    LOG_ERROR << "Failed to stop sd_event_loop:" << errno;
    // throw std::system_error(errno, std::system_category(),
    //                         "Failed to stop sd_event_loop");
  }
}

void Dbus::SetShoulderTapCallback(std::function<void()> shoulder_tap_callback) {
  shoulder_tap_callback_ = std::move(shoulder_tap_callback);
}
std::future<Consent::Outcome> Dbus::GetConsent(const std::vector<Uptane::Target> &targets) {
  // TODO
  (void)targets;
  std::promise<Consent::Outcome> p;
  p.set_value({true, "Granted Trivially"});
  return p.get_future();
}
void Dbus::PendingUpdateCancelled() {
  // TODO
}
