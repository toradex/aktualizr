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
  if (isTerminal()) {
    throw std::runtime_error("cannot succeed install on terminal sync plan");
  }
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

namespace {

bool isValidPhase(int phase) { return phase >= 0 && phase <= static_cast<int>(SyncPlan::Phase::kInstalled); }

bool isValidOutcome(int outcome) {
  return outcome >= 0 && outcome <= static_cast<int>(SyncPlan::Outcome::kCommitted);
}

}  // namespace

SyncPlan SyncPlan::fromJson(const Json::Value& json) {
  if (!json.isObject()) {
    throw std::runtime_error("invalid sync plan json");
  }
  if (!json.isMember("correlation_id") || !json["correlation_id"].isString()) {
    throw std::runtime_error("missing correlation_id");
  }
  if (!json.isMember("outcome") || !json["outcome"].isInt()) {
    throw std::runtime_error("missing outcome");
  }
  if (!json.isMember("manifest_sent") || !json["manifest_sent"].isBool()) {
    throw std::runtime_error("missing manifest_sent");
  }
  if (!json.isMember("members") || !json["members"].isArray()) {
    throw std::runtime_error("missing members");
  }
  const int outcome_int = json["outcome"].asInt();
  if (!isValidOutcome(outcome_int)) {
    throw std::runtime_error("invalid outcome");
  }

  std::vector<Member> members;
  for (const auto& member_json : json["members"]) {
    if (!member_json.isObject()) {
      throw std::runtime_error("invalid member");
    }
    if (!member_json.isMember("serial") || !member_json["serial"].isString()) {
      throw std::runtime_error("missing member serial");
    }
    if (!member_json.isMember("hardware_id") || !member_json["hardware_id"].isString()) {
      throw std::runtime_error("missing member hardware_id");
    }
    if (!member_json.isMember("phase") || !member_json["phase"].isInt()) {
      throw std::runtime_error("missing member phase");
    }
    if (!member_json.isMember("install_called") || !member_json["install_called"].isBool()) {
      throw std::runtime_error("missing member install_called");
    }
    const int phase_int = member_json["phase"].asInt();
    if (!isValidPhase(phase_int)) {
      throw std::runtime_error("invalid phase");
    }
    Member member;
    member.serial = member_json["serial"].asString();
    member.hardware_id = member_json["hardware_id"].asString();
    member.phase = static_cast<Phase>(phase_int);
    member.install_called = member_json["install_called"].asBool();
    members.push_back(std::move(member));
  }

  SyncPlan plan = Create(json["correlation_id"].asString(), std::move(members));
  plan.outcome_ = static_cast<Outcome>(outcome_int);
  plan.manifest_sent_ = json["manifest_sent"].asBool();
  return plan;
}
