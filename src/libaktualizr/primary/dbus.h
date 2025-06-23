#ifndef PRIMARY_DBUS_H_
#define PRIMARY_DBUS_H_

// Note that this header file must not depend on sd-bus.h because it is
// included when BUILD_DBUS is not set.

#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <thread>

#include "primary/consent.h"

class Aktualizr;
class INvStorage;
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

/**
 * Expose Aktualizr over D-Bus.
 *
 * Thread Safety: All methods on this class are/must be safe to call from
 * multiple threads.
 */
class Dbus : public Consent {
 public:
  static const char *const Path;
  static const char *const Interface;
  static const char *const WellKnown;
  static const char *const InstallUpdatesAutomatically;
  static const char *const CheckForUpdates;
  static const char *const Consent;
  static const char *const ConsentRequired;

  Dbus(SdBus &&bus, std::shared_ptr<INvStorage> storage);
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
  void SetCheckForUpdatesCallback(std::function<void()> callback);

 private:
  // Launch and stop the Thread to handle D-Bus traffic
  void Run();
  void Stop() noexcept;

  [[nodiscard]] static constexpr int DiffTime(struct timespec *now, uint64_t systemd_abs_timeout);

  [[nodiscard]] std::function<void()> check_for_updates_callback() {
    std::lock_guard<std::mutex> guard{lock_};
    return check_for_updates_callback_;
  }

  friend class DbusCb;
  const SdBus bus_;
  const std::shared_ptr<INvStorage> storage_;
  sd_bus_slot *vtable_slot_{nullptr};
  // stop_fds_[0] is the read end, stop_fds_[1] is the write end
  std::array<int, 2> stop_fds_{-1, -1};
  std::thread dbus_thread_;
  std::mutex lock_;  // Hold this while modifying anything below
  std::function<void()> check_for_updates_callback_{};
  /** The currently in-flight request. Empty => Nothing in flight */
  std::string current_consent_request_;
  /** If there is an in-flight request, then this is valid */
  std::promise<Consent::Outcome> current_consent_promise_;
};

#endif  // PRIMARY_DBUS_H_
