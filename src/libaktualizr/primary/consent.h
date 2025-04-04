#ifndef CONSENT_H_
#define CONSENT_H_

#include "libaktualizr/types.h"

#include <future>
#include <string>
#include <vector>

class Consent {
 public:
  struct Outcome {
    bool granted;
    std::string reason;
  };
  Consent() = default;
  Consent(const Consent&) = delete;
  Consent(Consent&&) = delete;
  Consent& operator=(const Consent&) = delete;
  Consent& operator=(Consent&&) = delete;

  virtual ~Consent() = default;

  virtual std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& targets) = 0;

  virtual void PendingUpdateCancelled() = 0;
};

class TrivialConsent : public Consent {
 public:
  TrivialConsent() = default;
  std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& /* targets */) override {
    std::promise<Outcome> p;
    p.set_value({true, "Granted Trivially"});
    return p.get_future();
  }

  void PendingUpdateCancelled() override {
    // No-op since our implementation of GetConsent() will return a future that is already resolved
  }
};

#endif  // CONSENT_H_
