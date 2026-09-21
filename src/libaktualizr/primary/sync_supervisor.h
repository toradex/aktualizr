#ifndef SYNC_SUPERVISOR_H_
#define SYNC_SUPERVISOR_H_

#include <string>

#include "primary/sync_plan.h"

enum class BootObservation { kRebootNotDetected, kRolledBack, kNewOsBooted };

enum class SupervisorAction {
  kWaitForReboot,
  kApplyNextInstall,
  kRollbackInstalled,
  kFailAndReport,
  kCommitAndReport,
  kRetryManifest
};

struct SupervisorStep {
  SupervisorAction action;
  std::string serial;  // set for kApplyNextInstall and kRollbackInstalled
};

SupervisorStep NextStep(const SyncPlan& plan, bool ostree_in_group, BootObservation boot);

#endif  // SYNC_SUPERVISOR_H_
