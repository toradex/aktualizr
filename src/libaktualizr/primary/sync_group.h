#ifndef SYNC_GROUP_H_
#define SYNC_GROUP_H_

#include <boost/optional.hpp>
#include <string>
#include <vector>

enum class SyncResolveKind { kNotAGroup, kGroup, kReject };

struct SyncCandidate {
  std::string serial;
  std::string hardware_id;
  bool is_ostree{false};
  bool is_docker_compose{false};
  bool is_torizon_generic{false};
  bool supports_rollback{false};
  boost::optional<std::string> sync_group_id;
  boost::optional<int> sync_order;
};

struct ResolvedSyncGroup {
  SyncResolveKind kind{SyncResolveKind::kNotAGroup};
  std::string reject_reason;
  bool ostree_in_group{false};
  bool ostree_order_overridden{false};
  std::vector<std::string> serials;  // install order
};

ResolvedSyncGroup ResolveSyncGroup(const std::vector<SyncCandidate>& candidates);

#endif  // SYNC_GROUP_H_
