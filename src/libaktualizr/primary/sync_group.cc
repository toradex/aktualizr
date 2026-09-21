#include "primary/sync_group.h"

#include <algorithm>
#include <set>

namespace {

ResolvedSyncGroup reject(const std::string& reason) {
  ResolvedSyncGroup result;
  result.kind = SyncResolveKind::kReject;
  result.reject_reason = reason;
  return result;
}

}  // namespace

ResolvedSyncGroup ResolveSyncGroup(const std::vector<SyncCandidate>& candidates) {
  std::set<std::string> group_ids;
  for (const auto& candidate : candidates) {
    if (candidate.sync_group_id && !candidate.sync_group_id->empty()) {
      group_ids.insert(*candidate.sync_group_id);
    }
  }

  if (group_ids.size() > 1) {
    return reject("more than one sync_group_id");
  }

  if (group_ids.empty()) {
    if (candidates.size() != 2) {
      return {};
    }

    const SyncCandidate* ostree = nullptr;
    const SyncCandidate* compose = nullptr;
    for (const auto& candidate : candidates) {
      if (candidate.is_ostree) {
        if (ostree != nullptr) {
          return {};
        }
        ostree = &candidate;
      }
      if (candidate.is_docker_compose) {
        if (compose != nullptr) {
          return {};
        }
        compose = &candidate;
      }
    }
    if (ostree == nullptr || compose == nullptr || ostree == compose) {
      return {};
    }

    ResolvedSyncGroup result;
    result.kind = SyncResolveKind::kGroup;
    result.ostree_in_group = true;
    result.serials = {ostree->serial, compose->serial};
    return result;
  }

  const std::string& group_id = *group_ids.begin();
  std::vector<const SyncCandidate*> members;
  for (const auto& candidate : candidates) {
    if (candidate.sync_group_id && *candidate.sync_group_id == group_id) {
      members.push_back(&candidate);
    }
  }

  if (members.size() < 2) {
    return {};
  }
  if (members.size() != candidates.size()) {
    return reject("update contains targets outside the sync group");
  }

  for (const auto* member : members) {
    if (member->is_torizon_generic && !member->supports_rollback) {
      return reject("torizon-generic sync member does not support rollback");
    }
    if (!member->is_ostree && !member->is_docker_compose && !member->is_torizon_generic) {
      return reject("sync member is not a sync participant");
    }
    if (!member->is_ostree && !member->sync_order) {
      return reject("sync member is missing sync_order");
    }
  }

  std::vector<const SyncCandidate*> ostree_members;
  std::vector<const SyncCandidate*> non_ostree_members;
  for (const auto* member : members) {
    if (member->is_ostree) {
      ostree_members.push_back(member);
    } else {
      non_ostree_members.push_back(member);
    }
  }
  std::sort(non_ostree_members.begin(), non_ostree_members.end(),
            [](const SyncCandidate* lhs, const SyncCandidate* rhs) {
              if (*lhs->sync_order != *rhs->sync_order) {
                return *lhs->sync_order < *rhs->sync_order;
              }
              return lhs->hardware_id < rhs->hardware_id;
            });

  ResolvedSyncGroup result;
  result.kind = SyncResolveKind::kGroup;
  result.ostree_in_group = !ostree_members.empty();
  for (const auto* member : ostree_members) {
    result.serials.push_back(member->serial);
    result.ostree_order_overridden = result.ostree_order_overridden || static_cast<bool>(member->sync_order);
  }
  for (const auto* member : non_ostree_members) {
    result.serials.push_back(member->serial);
  }
  return result;
}
