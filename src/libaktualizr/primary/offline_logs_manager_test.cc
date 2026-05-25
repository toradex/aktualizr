#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

#include "primary/offline_logs_manager.h"
#include "primary/test_journal.h"
#include "storage/offline_logs_db.h"
#include "utilities/utils.h"

#ifdef BUILD_OFFLINE_UPDATES

// ============================================================================
// OfflineLogsManager Symlink Canonicalization Tests
// ============================================================================

// Test fixture that creates a minimal config for OfflineLogsManager
class OfflineLogsManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create minimal config with offline logging enabled
    config_.logger.offline_logs_enabled = true;
    config_.logger.offline_logs_file = "update-logs.db";
  }

  Config config_;
};

TEST_F(OfflineLogsManagerTest, BeginInstallWorksWithSymlinkedParentDirectory) {
  TemporaryDirectory temp_dir;

  // Create the actual update directory
  auto real_update_dir = temp_dir.Path() / "actual_media";
  boost::filesystem::create_directories(real_update_dir);

  // Create a symlink that points to the actual directory (simulates /mnt -> /var/mnt/...)
  auto symlink_dir = temp_dir.Path() / "symlink_mount";
  boost::filesystem::create_directory_symlink(real_update_dir, symlink_dir);
  ASSERT_TRUE(boost::filesystem::is_symlink(symlink_dir));

  // OfflineLogsManager should resolve the symlink and successfully create the database
  OfflineLogsManager manager(config_, std::make_shared<TestJournal>());
  InstallId id = manager.BeginInstall(symlink_dir, "device-123", "test-update", 1);

  // Should succeed because ResolveLogsDbPath canonicalizes the path
  EXPECT_TRUE(id.IsValid()) << "BeginInstall should succeed when parent directory is a symlink";

  // Verify the database was created in the real directory (not via symlink)
  auto expected_db_path = real_update_dir / "update-logs.db";
  EXPECT_TRUE(boost::filesystem::exists(expected_db_path))
      << "Database should be created in the real directory: " << expected_db_path;
}

TEST_F(OfflineLogsManagerTest, BeginInstallWorksWithDeeplyNestedSymlinks) {
  TemporaryDirectory temp_dir;

  // Create a chain: symlink1 -> symlink2 -> real_dir
  auto real_dir = temp_dir.Path() / "real";
  boost::filesystem::create_directories(real_dir);

  auto symlink2 = temp_dir.Path() / "link2";
  boost::filesystem::create_directory_symlink(real_dir, symlink2);

  auto symlink1 = temp_dir.Path() / "link1";
  boost::filesystem::create_directory_symlink(symlink2, symlink1);

  ASSERT_TRUE(boost::filesystem::is_symlink(symlink1));
  ASSERT_TRUE(boost::filesystem::is_symlink(symlink2));

  OfflineLogsManager manager(config_, std::make_shared<TestJournal>());
  InstallId id = manager.BeginInstall(symlink1, "device-123", "test-update", 1);

  EXPECT_TRUE(id.IsValid()) << "BeginInstall should handle nested symlinks";
}

TEST_F(OfflineLogsManagerTest, BeginInstallFailsWhenDatabaseFileIsSymlink) {
  TemporaryDirectory temp_dir;

  // Create the update directory
  auto update_dir = temp_dir.Path() / "update";
  boost::filesystem::create_directories(update_dir);

  // Create a real database file somewhere else
  auto real_db = temp_dir.Path() / "elsewhere" / "real.db";
  boost::filesystem::create_directories(real_db.parent_path());
  {
    OfflineLogsDb real_db_obj(real_db);
    ASSERT_TRUE(real_db_obj.Ok());
  }

  // Create a symlink in the update directory pointing to the real database
  auto symlink_db = update_dir / "update-logs.db";
  boost::filesystem::create_symlink(real_db, symlink_db);
  ASSERT_TRUE(boost::filesystem::is_symlink(symlink_db));

  // OfflineLogsManager should fail because the final db file is a symlink
  // (SQLITE_OPEN_NOFOLLOW rejects this)
  OfflineLogsManager manager(config_, std::make_shared<TestJournal>());
  InstallId id = manager.BeginInstall(update_dir, "device-123", "test-update", 1);

  EXPECT_FALSE(id.IsValid())
      << "BeginInstall should fail when the database file itself is a symlink (security protection)";
}

TEST_F(OfflineLogsManagerTest, FindAndResumeWorksWithSymlinkedParentDirectory) {
  TemporaryDirectory temp_dir;

  // Create the actual update directory
  auto real_update_dir = temp_dir.Path() / "actual_media";
  boost::filesystem::create_directories(real_update_dir);

  // Create a symlink that points to the actual directory
  auto symlink_dir = temp_dir.Path() / "symlink_mount";
  boost::filesystem::create_directory_symlink(real_update_dir, symlink_dir);

  // First create an install using the real path
  {
    OfflineLogsManager manager(config_, std::make_shared<TestJournal>());
    InstallId id = manager.BeginInstall(real_update_dir, "device-123", "test-update", 1);
    ASSERT_TRUE(id.IsValid());
  }

  // Now try to resume using the symlinked path (simulates reboot with different mount)
  OfflineLogsManager manager(config_, std::make_shared<TestJournal>());
  InstallId id = manager.FindAndResumeInstall(symlink_dir, "device-123");

  EXPECT_TRUE(id.IsValid()) << "FindAndResumeInstall should work when accessing via symlinked parent";
}

#endif  // BUILD_OFFLINE_UPDATES

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
#endif
