#ifndef CONSENT_H_
#define CONSENT_H_

#include "libaktualizr/types.h"

#include <future>
#include <string>
#include <vector>

class Consent {
 public:
  struct Outcome {
    bool granted{false};
    bool was_cancelled{false};
    std::string reason;
  };
  Consent() = default;
  Consent(const Consent&) = delete;
  Consent(Consent&&) = delete;
  Consent& operator=(const Consent&) = delete;
  Consent& operator=(Consent&&) = delete;

  virtual ~Consent() = default;

  /**
   * Check if it is OK to install \p targets.
   * When the user responds, the future will resolve.
   * On the next call to either GetConsent or PendingUpdateCancelled, the future
   * will be abandoned and .get() will throw std::future_errc::broken_promise,
   */
  virtual std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& targets) = 0;

  /**
   * Stop asking the user for Consent to install an update, perhaps to install
   * an offline update instead.
   */
  virtual void PendingUpdateCancelled() = 0;
};

class TrivialConsent : public Consent {
 public:
  TrivialConsent() = default;
  std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& /* targets */) override {
    std::promise<Outcome> p;
    p.set_value({true, false, "Granted Trivially"});
    return p.get_future();
  }

  void PendingUpdateCancelled() override {
    // No-op since our implementation of GetConsent() will return a future that is already resolved
  }
};

#endif  // CONSENT_H_
