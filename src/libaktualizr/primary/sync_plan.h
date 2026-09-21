#ifndef SYNC_PLAN_H_
#define SYNC_PLAN_H_

#include <json/json.h>
#include <string>
#include <vector>

class SyncPlan {
 public:
  enum class Phase { kStaged = 0, kInstalled = 1 };
  enum class Outcome { kInProgress = 0, kFailed = 1, kCommitted = 2 };

  struct Member {
    std::string serial;
    std::string hardware_id;
    Phase phase{Phase::kStaged};
    bool install_called{false};
  };

  static SyncPlan Create(const std::string& correlation_id, std::vector<Member> members);

  void noteInstallStarted(const std::string& serial);
  void noteInstallSucceeded(const std::string& serial);
  void markFailed();
  void markCommitted();
  void noteManifestSent();

  std::vector<std::string> membersToInstall() const;
  std::vector<std::string> membersToRollback() const;
  bool manifestPending() const;

  Json::Value toJson() const;
  static SyncPlan fromJson(const Json::Value& json);

  const std::string& correlationId() const { return correlation_id_; }
  Outcome outcome() const { return outcome_; }
  const std::vector<Member>& members() const { return members_; }

 private:
  SyncPlan(std::string correlation_id, std::vector<Member> members);

  bool isTerminal() const;
  Member* findMember(const std::string& serial);
  const Member* findMember(const std::string& serial) const;

  std::string correlation_id_;
  Outcome outcome_{Outcome::kInProgress};
  bool manifest_sent_{false};
  std::vector<Member> members_;
};

#endif  // SYNC_PLAN_H_
