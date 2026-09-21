#include "primary/sync_supervisor.h"

#include <stdexcept>

SupervisorStep NextStep(const SyncPlan& plan, bool ostree_in_group, BootObservation boot) {
  if (plan.outcome() != SyncPlan::Outcome::kInProgress) {
    if (plan.manifestPending()) {
      return {SupervisorAction::kRetryManifest, {}};
    }
    throw std::logic_error("sync plan finished and manifest already sent");
  }

  switch (boot) {
    case BootObservation::kRebootNotDetected:
      if (ostree_in_group) {
        return {SupervisorAction::kWaitForReboot, {}};
      }
      break;
    case BootObservation::kRolledBack:
      if (ostree_in_group) {
        return {SupervisorAction::kFailAndReport, {}};
      }
      break;
    case BootObservation::kNewOsBooted:
      break;
    default:
      throw std::logic_error("unknown boot observation");
  }

  const auto& members = plan.members();
  for (auto it = members.rbegin(); it != members.rend(); ++it) {
    if (it->install_called && it->phase == SyncPlan::Phase::kStaged) {
      return {SupervisorAction::kRollbackInstalled, it->serial};
    }
  }

  const std::vector<std::string> to_install = plan.membersToInstall();
  if (!to_install.empty()) {
    return {SupervisorAction::kApplyNextInstall, to_install.front()};
  }

  return {SupervisorAction::kCommitAndReport, {}};
}
