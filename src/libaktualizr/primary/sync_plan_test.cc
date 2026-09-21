#include <gtest/gtest.h>

#include "primary/sync_plan.h"

#include "logging/logging.h"

namespace {

SyncPlan makePlan() {
  return SyncPlan::Create("corr-1", {{"os", "hw-os", SyncPlan::Phase::kStaged, false},
                                     {"compose", "hw-compose", SyncPlan::Phase::kStaged, false}},
                          true);
}

}  // namespace

TEST(SyncPlan, RollbackOnlyAfterInstallAndManifestAfterTerminal) {
  SyncPlan plan = makePlan();
  EXPECT_EQ(plan.membersToInstall(), (std::vector<std::string>{"os", "compose"}));
  EXPECT_TRUE(plan.membersToRollback().empty());
  EXPECT_FALSE(plan.manifestPending());
  EXPECT_THROW(plan.noteManifestSent(), std::runtime_error);

  plan.noteInstallStarted("compose");
  plan.noteInstallSucceeded("compose");
  EXPECT_EQ(plan.membersToRollback(), (std::vector<std::string>{"compose"}));

  plan.markFailed();
  EXPECT_TRUE(plan.membersToInstall().empty());
  EXPECT_TRUE(plan.membersToRollback().empty());
  EXPECT_TRUE(plan.manifestPending());
  plan.noteManifestSent();
  EXPECT_FALSE(plan.manifestPending());
}

TEST(SyncPlan, CommitRequiresEveryMemberInstalled) {
  SyncPlan plan = makePlan();
  plan.noteInstallStarted("compose");
  plan.noteInstallSucceeded("compose");
  EXPECT_THROW(plan.markCommitted(), std::runtime_error);

  plan.noteInstallStarted("os");
  plan.noteInstallSucceeded("os");
  plan.markCommitted();
  EXPECT_TRUE(plan.manifestPending());
}

TEST(SyncPlan, JsonRoundTrip) {
  SyncPlan plan = makePlan();
  plan.noteInstallStarted("compose");
  plan.noteInstallSucceeded("compose");
  plan.markFailed();
  const SyncPlan loaded = SyncPlan::fromJson(plan.toJson());
  EXPECT_EQ(loaded.correlationId(), "corr-1");
  EXPECT_EQ(loaded.outcome(), SyncPlan::Outcome::kFailed);
  EXPECT_TRUE(loaded.manifestPending());
  EXPECT_TRUE(loaded.members().at(1).install_called);
  EXPECT_EQ(loaded.members().at(1).phase, SyncPlan::Phase::kInstalled);
}

TEST(SyncPlan, JsonRoundTripPreservesNoOstree) {
  const SyncPlan plan =
      SyncPlan::Create("corr-1", {{"a", "hw-a", SyncPlan::Phase::kStaged, false},
                                  {"b", "hw-b", SyncPlan::Phase::kStaged, false}},
                       false);
  const SyncPlan loaded = SyncPlan::fromJson(plan.toJson());
  EXPECT_FALSE(loaded.ostreeInGroup());
}

TEST(SyncPlan, FromJsonDefaultsMissingOstreeInGroupToTrue) {
  Json::Value json = makePlan().toJson();
  json.removeMember("ostree_in_group");
  const SyncPlan loaded = SyncPlan::fromJson(json);
  EXPECT_TRUE(loaded.ostreeInGroup());
}

TEST(SyncPlan, FromJsonRejectsOneMemberPlan) {
  Json::Value json;
  json["correlation_id"] = "corr-1";
  json["outcome"] = 0;
  json["manifest_sent"] = false;
  Json::Value members(Json::arrayValue);
  Json::Value member;
  member["serial"] = "os";
  member["hardware_id"] = "hw-os";
  member["phase"] = 0;
  member["install_called"] = false;
  members.append(member);
  json["members"] = members;
  EXPECT_THROW(SyncPlan::fromJson(json), std::runtime_error);
}

TEST(SyncPlan, FromJsonRejectsOutOfRangePhase) {
  Json::Value json = makePlan().toJson();
  json["members"][0]["phase"] = 99;
  EXPECT_THROW(SyncPlan::fromJson(json), std::runtime_error);
}

TEST(SyncPlan, NoteInstallSucceededRejectsTerminalPlan) {
  SyncPlan plan = makePlan();
  plan.noteInstallStarted("compose");
  plan.noteInstallSucceeded("compose");
  plan.markFailed();
  EXPECT_THROW(plan.noteInstallSucceeded("compose"), std::runtime_error);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);
  return RUN_ALL_TESTS();
}
