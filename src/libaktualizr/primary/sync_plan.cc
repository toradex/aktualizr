#include "primary/sync_plan.h"

#include <stdexcept>

SyncPlan SyncPlan::Create(const std::string& correlation_id, std::vector<Member> members) {
  if (members.size() < 2) {
    throw std::runtime_error("sync plan requires at least two members");
  }
  return SyncPlan(correlation_id, std::move(members));
}

SyncPlan::SyncPlan(std::string correlation_id, std::vector<Member> members)
    : correlation_id_(std::move(correlation_id)), members_(std::move(members)) {}

bool SyncPlan::isTerminal() const { return outcome_ != Outcome::kInProgress; }

SyncPlan::Member* SyncPlan::findMember(const std::string& serial) {
  for (auto& member : members_) {
    if (member.serial == serial) {
      return &member;
    }
  }
  return nullptr;
}

const SyncPlan::Member* SyncPlan::findMember(const std::string& serial) const {
  for (const auto& member : members_) {
    if (member.serial == serial) {
      return &member;
    }
  }
  return nullptr;
}

void SyncPlan::noteInstallStarted(const std::string& serial) {
  if (isTerminal()) {
    throw std::runtime_error("cannot start install on terminal sync plan");
  }
  Member* member = findMember(serial);
  if (member == nullptr) {
    throw std::runtime_error("unknown sync plan member");
  }
  if (member->phase == Phase::kInstalled) {
    throw std::runtime_error("member already installed");
  }
  member->install_called = true;
}

void SyncPlan::noteInstallSucceeded(const std::string& serial) {
  Member* member = findMember(serial);
  if (member == nullptr) {
    throw std::runtime_error("unknown sync plan member");
  }
  if (!member->install_called) {
    throw std::runtime_error("install was not started");
  }
  member->phase = Phase::kInstalled;
}

void SyncPlan::markFailed() {
  if (outcome_ == Outcome::kCommitted) {
    throw std::runtime_error("cannot fail committed sync plan");
  }
  outcome_ = Outcome::kFailed;
  manifest_sent_ = false;
}

void SyncPlan::markCommitted() {
  if (outcome_ == Outcome::kFailed) {
    throw std::runtime_error("cannot commit failed sync plan");
  }
  for (const auto& member : members_) {
    if (member.phase != Phase::kInstalled) {
      throw std::runtime_error("not all members installed");
    }
  }
  outcome_ = Outcome::kCommitted;
  manifest_sent_ = false;
}

void SyncPlan::noteManifestSent() {
  if (!isTerminal()) {
    throw std::runtime_error("manifest can only be sent for terminal sync plan");
  }
  manifest_sent_ = true;
}

std::vector<std::string> SyncPlan::membersToInstall() const {
  if (outcome_ != Outcome::kInProgress) {
    return {};
  }
  std::vector<std::string> serials;
  for (const auto& member : members_) {
    if (member.phase == Phase::kStaged && !member.install_called) {
      serials.push_back(member.serial);
    }
  }
  return serials;
}

std::vector<std::string> SyncPlan::membersToRollback() const {
  if (isTerminal()) {
    return {};
  }
  std::vector<std::string> serials;
  for (const auto& member : members_) {
    if (member.install_called) {
      serials.push_back(member.serial);
    }
  }
  return serials;
}

bool SyncPlan::manifestPending() const { return isTerminal() && !manifest_sent_; }

Json::Value SyncPlan::toJson() const {
  Json::Value json;
  json["correlation_id"] = correlation_id_;
  json["outcome"] = static_cast<int>(outcome_);
  json["manifest_sent"] = manifest_sent_;
  Json::Value members_json(Json::arrayValue);
  for (const auto& member : members_) {
    Json::Value member_json;
    member_json["serial"] = member.serial;
    member_json["hardware_id"] = member.hardware_id;
    member_json["phase"] = static_cast<int>(member.phase);
    member_json["install_called"] = member.install_called;
    members_json.append(member_json);
  }
  json["members"] = members_json;
  return json;
}

SyncPlan SyncPlan::fromJson(const Json::Value& json) {
  std::vector<Member> members;
  for (const auto& member_json : json["members"]) {
    Member member;
    member.serial = member_json["serial"].asString();
    member.hardware_id = member_json["hardware_id"].asString();
    member.phase = static_cast<Phase>(member_json["phase"].asInt());
    member.install_called = member_json["install_called"].asBool();
    members.push_back(std::move(member));
  }
  SyncPlan plan(json["correlation_id"].asString(), std::move(members));
  plan.outcome_ = static_cast<Outcome>(json["outcome"].asInt());
  plan.manifest_sent_ = json["manifest_sent"].asBool();
  return plan;
}
