#ifndef CONSENT_H_
#define CONSENT_H_

#include <json/json.h>
#include "libaktualizr/types.h"

#include <future>
#include <string>
#include <vector>

class Consent {
 public:
  struct Outcome {
    /**
     * How the consent request was resolved.
     * kSuperseded is only used when a consent request is replaced by a newer
     * one (via a second GetConsent() call); the state machine swaps to the new
     * future without reading the old one, so this value should never be acted
     * upon. If it is ever observed, treat it as "do not install" and stay in
     * the consent state (fail safe).
     */
    enum class Result {
      kGranted,
      kRefused,
      kCancelled,
      kSuperseded,
    };
    Result result{Result::kRefused};
    std::string reason;
    /** Correlation ID of the update offer this outcome answers. */
    std::string correlation_id;

    [[nodiscard]] bool granted() const { return result == Result::kGranted; }
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
   * \p correlation_id identifies the update offer; it is exposed to the user
   * (e.g. over D-Bus) and responses must reference it.
   * If GetConsent() is called again while a request is outstanding, the old
   * future resolves with Result::kSuperseded. If PendingUpdateCancelled() is
   * called, it resolves with Result::kCancelled.
   */
  virtual std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& targets,
                                          const std::string& correlation_id) = 0;

  /**
   * Stop asking the user for Consent to install an update, perhaps to install
   * an offline update instead.
   */
  virtual void PendingUpdateCancelled() = 0;

  /**
   * Convert a list of targets into a JSON format suitable to expose via the
   * D-Bus Consent API.
   */
  static Json::Value TargetsToJson(const std::vector<Uptane::Target>& targets);
};

class TrivialConsent : public Consent {
 public:
  TrivialConsent() = default;
  std::future<Outcome> GetConsent(const std::vector<Uptane::Target>& /* targets */,
                                  const std::string& correlation_id) override {
    std::promise<Outcome> p;
    p.set_value({Outcome::Result::kGranted, "Granted Trivially", correlation_id});
    return p.get_future();
  }

  void PendingUpdateCancelled() override {
    // No-op since our implementation of GetConsent() will return a future that is already resolved
  }
};

#endif  // CONSENT_H_
