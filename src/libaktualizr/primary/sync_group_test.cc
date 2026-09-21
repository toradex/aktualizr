#include <gtest/gtest.h>

#include "primary/sync_group.h"

#include "logging/logging.h"

namespace {

SyncCandidate candidate(const std::string& serial, const std::string& hardware_id) {
  SyncCandidate result;
  result.serial = serial;
  result.hardware_id = hardware_id;
  return result;
}

}  // namespace

TEST(SyncGroup, AutoGroupsOstreeAndCompose) {
  SyncCandidate ostree = candidate("ostree", "hw-ostree");
  ostree.is_ostree = true;
  SyncCandidate compose = candidate("compose", "hw-compose");
  compose.is_docker_compose = true;

  const ResolvedSyncGroup result = ResolveSyncGroup({compose, ostree});

  EXPECT_EQ(result.kind, SyncResolveKind::kGroup);
  EXPECT_EQ(result.serials, (std::vector<std::string>{"ostree", "compose"}));
  EXPECT_TRUE(result.ostree_in_group);
}

TEST(SyncGroup, DoesNotAutoGroupThreeCandidates) {
  SyncCandidate ostree = candidate("ostree", "hw-ostree");
  ostree.is_ostree = true;
  SyncCandidate compose = candidate("compose", "hw-compose");
  compose.is_docker_compose = true;

  EXPECT_EQ(ResolveSyncGroup({ostree, compose, candidate("extra", "hw-extra")}).kind,
            SyncResolveKind::kNotAGroup);
}

TEST(SyncGroup, DoesNotAutoGroupOstreeAlone) {
  SyncCandidate ostree = candidate("ostree", "hw-ostree");
  ostree.is_ostree = true;

  EXPECT_EQ(ResolveSyncGroup({ostree}).kind, SyncResolveKind::kNotAGroup);
}

TEST(SyncGroup, RejectsMoreThanOneGroupId) {
  SyncCandidate first = candidate("first", "hw-first");
  first.sync_group_id = "group-1";
  SyncCandidate second = candidate("second", "hw-second");
  second.sync_group_id = "group-2";

  const ResolvedSyncGroup result = ResolveSyncGroup({first, second});

  EXPECT_EQ(result.kind, SyncResolveKind::kReject);
  EXPECT_EQ(result.reject_reason, "more than one sync_group_id");
}

TEST(SyncGroup, SingleTaggedCandidateIsNotAGroup) {
  SyncCandidate tagged = candidate("tagged", "hw-tagged");
  tagged.sync_group_id = "group";

  EXPECT_EQ(ResolveSyncGroup({tagged, candidate("untagged", "hw-untagged")}).kind,
            SyncResolveKind::kNotAGroup);
}

TEST(SyncGroup, PutsOstreeFirstAndReportsOrderOverride) {
  SyncCandidate ostree = candidate("ostree", "hw-ostree");
  ostree.is_ostree = true;
  ostree.sync_group_id = "group";
  ostree.sync_order = 5;
  SyncCandidate compose = candidate("compose", "hw-compose");
  compose.is_docker_compose = true;
  compose.sync_group_id = "group";
  compose.sync_order = 1;

  const ResolvedSyncGroup result = ResolveSyncGroup({compose, ostree});

  EXPECT_EQ(result.kind, SyncResolveKind::kGroup);
  EXPECT_EQ(result.serials, (std::vector<std::string>{"ostree", "compose"}));
  EXPECT_TRUE(result.ostree_order_overridden);
}

TEST(SyncGroup, BreaksEqualOrdersByHardwareId) {
  SyncCandidate b = candidate("b", "b");
  b.is_torizon_generic = true;
  b.supports_rollback = true;
  b.sync_group_id = "group";
  b.sync_order = 2;
  SyncCandidate a = candidate("a", "a");
  a.is_torizon_generic = true;
  a.supports_rollback = true;
  a.sync_group_id = "group";
  a.sync_order = 2;

  const ResolvedSyncGroup result = ResolveSyncGroup({b, a});

  EXPECT_EQ(result.kind, SyncResolveKind::kGroup);
  EXPECT_EQ(result.serials, (std::vector<std::string>{"a", "b"}));
  EXPECT_FALSE(result.ostree_in_group);
}

TEST(SyncGroup, RejectsNonOstreeMemberWithoutOrder) {
  SyncCandidate first = candidate("first", "hw-first");
  first.is_docker_compose = true;
  first.sync_group_id = "group";
  first.sync_order = 1;
  SyncCandidate second = candidate("second", "hw-second");
  second.is_docker_compose = true;
  second.sync_group_id = "group";

  const ResolvedSyncGroup result = ResolveSyncGroup({first, second});

  EXPECT_EQ(result.kind, SyncResolveKind::kReject);
  EXPECT_EQ(result.reject_reason, "sync member is missing sync_order");
}

TEST(SyncGroup, RejectsGenericWithoutRollback) {
  SyncCandidate generic = candidate("generic", "hw-generic");
  generic.is_torizon_generic = true;
  generic.sync_group_id = "group";
  generic.sync_order = 1;
  SyncCandidate compose = candidate("compose", "hw-compose");
  compose.is_docker_compose = true;
  compose.sync_group_id = "group";
  compose.sync_order = 2;

  const ResolvedSyncGroup result = ResolveSyncGroup({generic, compose});

  EXPECT_EQ(result.kind, SyncResolveKind::kReject);
  EXPECT_EQ(result.reject_reason, "torizon-generic sync member does not support rollback");
}

TEST(SyncGroup, RejectsMemberThatIsNotAParticipant) {
  SyncCandidate unknown = candidate("unknown", "hw-unknown");
  unknown.sync_group_id = "group";
  unknown.sync_order = 1;
  SyncCandidate compose = candidate("compose", "hw-compose");
  compose.is_docker_compose = true;
  compose.sync_group_id = "group";
  compose.sync_order = 2;

  const ResolvedSyncGroup result = ResolveSyncGroup({unknown, compose});

  EXPECT_EQ(result.kind, SyncResolveKind::kReject);
  EXPECT_EQ(result.reject_reason, "sync member is not a sync participant");
}

TEST(SyncGroup, RejectsTargetsOutsideGroup) {
  SyncCandidate first = candidate("first", "hw-first");
  first.is_docker_compose = true;
  first.sync_group_id = "group";
  first.sync_order = 1;
  SyncCandidate second = candidate("second", "hw-second");
  second.is_docker_compose = true;
  second.sync_group_id = "group";
  second.sync_order = 2;

  const ResolvedSyncGroup result = ResolveSyncGroup({first, second, candidate("outside", "hw-outside")});

  EXPECT_EQ(result.kind, SyncResolveKind::kReject);
  EXPECT_EQ(result.reject_reason, "update contains targets outside the sync group");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);
  return RUN_ALL_TESTS();
}
