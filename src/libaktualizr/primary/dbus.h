#ifndef PRIMARY_DBUS_H_
#define PRIMARY_DBUS_H_

// Note that this header file must not depend on sd-bus.h because it is
// included when BUILD_DBUS is not set.

#include <array>
#include <atomic>
#include <functional>
#include <thread>

#include "primary/consent.h"

class Aktualizr;
struct sd_bus;
struct sd_bus_slot;
// struct sd_bus_message;
// struct sd_bus_error;

// RAII sd_bus wrapper
// This object owns one reference to sd_bus object in ptr.
class SdBus {
 public:
  SdBus() = default;
  SdBus(SdBus &&) noexcept;
  // These could be implemented, but they are not needed at the moment
  SdBus(const SdBus &) = delete;
  SdBus &operator=(const SdBus &) = delete;
  SdBus &operator=(SdBus &&) = delete;
  ~SdBus();

  sd_bus *ptr{nullptr};
};

class Dbus : public Consent {
 public:
  static const char *const Path;
  static const char *const Interface;
  static const char *const WellKnown;
  explicit Dbus(SdBus &&bus);
  ~Dbus() override;

  // Non-copyable, non-movable
  Dbus(const Dbus &) = delete;
  Dbus(Dbus &&) = delete;
  Dbus &operator=(const Dbus &) = delete;
  Dbus &operator=(Dbus &&) = delete;

  // Consent implementation
  std::future<Outcome> GetConsent(const std::vector<Uptane::Target> &targets) override;

  void PendingUpdateCancelled() override;

  // Register callback for Aktualizr
  void SetShoulderTapCallback(std::function<void()> callback);

 private:
  // Launch and stop the Thread to handle D-Bus traffic
  void Run();
  void Stop() noexcept;

  static int DiffTime(struct timespec *now, uint64_t systemd_abs_timeout);
  friend class DbusCb;
  SdBus bus_;
  sd_bus_slot *vtable_slot_{nullptr};
  std::atomic<bool> stop_{false};
  // stop_fds_[0] is the read end, stop_fds_[1] is the write end
  std::array<int, 2> stop_fds_{-1, -1};
  std::function<void()> shoulder_tap_callback_{};
  std::thread dbus_thread_;
};

#endif  // PRIMARY_DBUS_H_
