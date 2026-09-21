#include <gtest/gtest.h>

#include "primary/sync_plan.h"
#include "primary/sync_supervisor.h"

#include "logging/logging.h"

namespace {

SyncPlan makePlan() {
  return SyncPlan::Create("corr-1", {{"os", "hw-os", SyncPlan::Phase::kStaged, false},
                                     {"compose", "hw-compose", SyncPlan::Phase::kStaged, false}});
}

SyncPlan makePlanWithOsInstalled() {
  SyncPlan plan = makePlan();
  plan.noteInstallStarted("os");
  plan.noteInstallSucceeded("os");
  return plan;
}

}  // namespace

TEST(SyncSupervisor, WaitForRebootWhileOsDeployPending) {
  SyncPlan plan = makePlan();
  plan.noteInstallStarted("os");
  const SupervisorStep step = NextStep(plan, true, BootObservation::kRebootNotDetected);
  EXPECT_EQ(step.action, SupervisorAction::kWaitForReboot);
}

TEST(SyncSupervisor, FailAndReportOnRollbackWithoutInstallCalled) {
  SyncPlan plan = makePlan();
  const SupervisorStep step = NextStep(plan, true, BootObservation::kRolledBack);
  EXPECT_EQ(step.action, SupervisorAction::kFailAndReport);
}

TEST(SyncSupervisor, ApplyNextInstallAfterNewOsBooted) {
  SyncPlan plan = makePlanWithOsInstalled();
  const SupervisorStep step = NextStep(plan, true, BootObservation::kNewOsBooted);
  EXPECT_EQ(step.action, SupervisorAction::kApplyNextInstall);
  EXPECT_EQ(step.serial, "compose");
}

TEST(SyncSupervisor, CommitAndReportWhenAllMembersInstalled) {
  SyncPlan plan = makePlanWithOsInstalled();
  plan.noteInstallStarted("compose");
  plan.noteInstallSucceeded("compose");
  const SupervisorStep step = NextStep(plan, true, BootObservation::kNewOsBooted);
  EXPECT_EQ(step.action, SupervisorAction::kCommitAndReport);
}

TEST(SyncSupervisor, RollbackInstalledMemberAfterFailedInstall) {
  SyncPlan plan = makePlanWithOsInstalled();
  plan.noteInstallStarted("compose");
  const SupervisorStep step = NextStep(plan, true, BootObservation::kNewOsBooted);
  EXPECT_EQ(step.action, SupervisorAction::kRollbackInstalled);
  EXPECT_EQ(step.serial, "compose");
}

TEST(SyncSupervisor, RetryManifestAfterFailedPlan) {
  SyncPlan plan = makePlan();
  plan.markFailed();
  const SupervisorStep step = NextStep(plan, true, BootObservation::kNewOsBooted);
  EXPECT_EQ(step.action, SupervisorAction::kRetryManifest);
}

TEST(SyncSupervisor, RequiresOstreeInGroup) {
  SyncPlan plan = makePlan();
  EXPECT_THROW(NextStep(plan, false, BootObservation::kRebootNotDetected), std::logic_error);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);
  return RUN_ALL_TESTS();
}
