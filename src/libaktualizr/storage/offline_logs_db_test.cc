#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

#include "storage/offline_logs_db.h"
#include "utilities/utils.h"

// ============================================================================
// InstallId Tests
// ============================================================================

TEST(InstallId, DefaultConstructedIsInvalid) {
  InstallId id;
  EXPECT_FALSE(id.IsValid());
  EXPECT_FALSE(static_cast<bool>(id));
}

TEST(InstallId, FromDbZeroIsValid) {
  InstallId id = InstallId::FromDb(0);
  EXPECT_TRUE(id.IsValid());
  EXPECT_TRUE(static_cast<bool>(id));
  EXPECT_EQ(id.Value(), 0);
}

TEST(InstallId, FromDbPositiveIsValid) {
  InstallId id = InstallId::FromDb(1);
  EXPECT_TRUE(id.IsValid());
  EXPECT_EQ(id.Value(), 1);

  InstallId id2 = InstallId::FromDb(12345);
  EXPECT_TRUE(id2.IsValid());
  EXPECT_EQ(id2.Value(), 12345);
}

TEST(InstallId, ComparisonOperators) {
  InstallId id1 = InstallId::FromDb(1);
  InstallId id2 = InstallId::FromDb(1);
  InstallId id3 = InstallId::FromDb(2);
  InstallId invalid1;
  InstallId invalid2;

  EXPECT_EQ(id1, id2);
  EXPECT_NE(id1, id3);
  EXPECT_EQ(invalid1, invalid2);
  EXPECT_NE(id1, invalid1);
}

TEST(InstallId, CanBeUsedInIfStatements) {
  InstallId valid = InstallId::FromDb(1);
  InstallId invalid;

  bool valid_branch_taken = false;
  if (valid) {
    valid_branch_taken = true;
  }
  EXPECT_TRUE(valid_branch_taken);

  bool invalid_branch_taken = false;
  if (invalid) {
    invalid_branch_taken = true;
  }
  EXPECT_FALSE(invalid_branch_taken);
}

// ============================================================================
// OfflineLogsDb Tests
// ============================================================================

TEST(OfflineLogsDb, OpenCreatesDbInTemporaryDirectory) {
  TemporaryDirectory temp_dir;
  auto db_path = temp_dir.Path() / "update-logs.db";

  OfflineLogsDb db(db_path);
  ASSERT_TRUE(db.Ok());
  EXPECT_TRUE(boost::filesystem::exists(db_path));
}

TEST(OfflineLogsDb, OpenFailsIfParentDirectoryDoesNotExist) {
  OfflineLogsDb db("/nonexistent/path/update-logs.db");
  EXPECT_FALSE(db.Ok());
}

TEST(OfflineLogsDb, CreateInstallReturnsValidId) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.CreateInstall("device-123", "test-update", 1);
  EXPECT_TRUE(id.IsValid());
  EXPECT_GE(id.Value(), 1);  // SQLite AUTOINCREMENT starts at 1
}

TEST(OfflineLogsDb, CreateMultipleInstallsReturnsUniqueIds) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id1 = db.CreateInstall("device-123", "update-1", 1);
  InstallId id2 = db.CreateInstall("device-123", "update-2", 2);
  InstallId id3 = db.CreateInstall("device-456", "update-1", 1);

  EXPECT_TRUE(id1.IsValid());
  EXPECT_TRUE(id2.IsValid());
  EXPECT_TRUE(id3.IsValid());
  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
  EXPECT_NE(id1, id3);
}

TEST(OfflineLogsDb, FindInProgressInstallReturnsInvalidForNoInstalls) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.FindInProgressInstall("device-123");
  EXPECT_FALSE(id.IsValid());
}

TEST(OfflineLogsDb, FindInProgressInstallFindsNewInstall) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId created = db.CreateInstall("device-123", "test-update", 1);
  ASSERT_TRUE(created.IsValid());

  InstallId found = db.FindInProgressInstall("device-123");
  EXPECT_TRUE(found.IsValid());
  EXPECT_EQ(created, found);
}

TEST(OfflineLogsDb, FindInProgressInstallReturnsInvalidAfterComplete) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId created = db.CreateInstall("device-123", "test-update", 1);
  db.CompleteInstall(created, 42, R"({"manifest": "data"})");

  InstallId found = db.FindInProgressInstall("device-123");
  EXPECT_FALSE(found.IsValid());
}

TEST(OfflineLogsDb, FindInProgressInstallReturnsLatest) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id1 = db.CreateInstall("device-123", "update-1", 1);
  db.CompleteInstall(id1, 1, R"({"manifest": 1})");

  InstallId id2 = db.CreateInstall("device-123", "update-2", 2);
  // id2 is not completed

  InstallId found = db.FindInProgressInstall("device-123");
  EXPECT_TRUE(found.IsValid());
  EXPECT_EQ(found, id2);
}

TEST(OfflineLogsDb, FindInProgressInstallOnlyMatchesDevice) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  db.CreateInstall("device-123", "test-update", 1);

  InstallId found = db.FindInProgressInstall("device-456");
  EXPECT_FALSE(found.IsValid());
}

TEST(OfflineLogsDb, CompleteInstallStoresManifest) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.CreateInstall("device-123", "test-update", 1);
  const std::string manifest = R"({"signed": {"version": 1}})";
  db.CompleteInstall(id, 42, manifest);

  // After completion, FindInProgressInstall should return invalid
  InstallId found = db.FindInProgressInstall("device-123");
  EXPECT_FALSE(found.IsValid());

  // Verify using direct SQL query
  SQLite3Guard sql_db(temp_dir.Path() / "update-logs.db", true);
  auto statement = sql_db.prepareStatement("SELECT report_counter, manifest FROM installs WHERE id = ?;", id.Value());
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_int(0), 42);
  auto stored_manifest = statement.get_result_col_str(1);
  ASSERT_TRUE(stored_manifest.has_value());
  EXPECT_EQ(stored_manifest.value(), manifest);
}

TEST(OfflineLogsDb, AddLogEntryWritesToDb) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.CreateInstall("device-123", "test-update", 1);
  db.AddLogEntry(id, 1234567890123456LL, "aktualizr.service", "Test log message");
  db.AddLogEntry(id, 1234567890123457LL, "docker.service", "Another message");

  // Verify using direct SQL query
  SQLite3Guard sql_db(temp_dir.Path() / "update-logs.db", true);
  auto statement = sql_db.prepareStatement("SELECT COUNT(*) FROM logs WHERE install_id = ?;", id.Value());
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_int(0), 2);
}

TEST(OfflineLogsDb, AddLogEntryPreservesOrder) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.CreateInstall("device-123", "test-update", 1);
  db.AddLogEntry(id, 100, "svc", "First");
  db.AddLogEntry(id, 200, "svc", "Second");
  db.AddLogEntry(id, 300, "svc", "Third");

  // Verify order using direct SQL query
  SQLite3Guard sql_db(temp_dir.Path() / "update-logs.db", true);
  auto statement =
      sql_db.prepareStatement("SELECT message FROM logs WHERE install_id = ? ORDER BY id ASC;", id.Value());

  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_str(0).value(), "First");
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_str(0).value(), "Second");
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_str(0).value(), "Third");
  EXPECT_EQ(statement.step(), SQLITE_DONE);
}

TEST(OfflineLogsDb, AddLogEntryWithInvalidIdDoesNotCrash) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId invalid;
  // Should not crash, just log a warning
  db.AddLogEntry(invalid, 100, "svc", "message");
}

TEST(OfflineLogsDb, AddReportWritesToDb) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.CreateInstall("device-123", "test-update", 1);
  db.AddReport(id, "report-uuid-1", 1234567890123456LL, "EcuDownloadStarted", 0, R"({"ecu": "primary"})");

  // Verify using direct SQL query
  SQLite3Guard sql_db(temp_dir.Path() / "update-logs.db", true);
  auto statement =
      sql_db.prepareStatement("SELECT report_id, type, event FROM reports WHERE install_id = ?;", id.Value());
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_str(0).value(), "report-uuid-1");
  EXPECT_EQ(statement.get_result_col_str(1).value(), "EcuDownloadStarted");
  EXPECT_EQ(statement.get_result_col_str(2).value(), R"({"ecu": "primary"})");
}

TEST(OfflineLogsDb, AddReportDeduplicatesByReportId) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId id = db.CreateInstall("device-123", "test-update", 1);
  db.AddReport(id, "duplicate-uuid", 100, "Type1", 0, "event1");
  db.AddReport(id, "duplicate-uuid", 200, "Type2", 1, "event2");  // Same report_id, should be ignored

  // Verify only one entry exists
  SQLite3Guard sql_db(temp_dir.Path() / "update-logs.db", true);
  auto statement = sql_db.prepareStatement("SELECT COUNT(*) FROM reports WHERE install_id = ?;", id.Value());
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_int(0), 1);

  // The first entry should be kept
  auto detail_stmt = sql_db.prepareStatement("SELECT type FROM reports WHERE report_id = ?;", "duplicate-uuid");
  ASSERT_EQ(detail_stmt.step(), SQLITE_ROW);
  EXPECT_EQ(detail_stmt.get_result_col_str(0).value(), "Type1");
}

TEST(OfflineLogsDb, AddReportWithInvalidIdDoesNotCrash) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId invalid;
  // Should not crash, just log a warning
  db.AddReport(invalid, "uuid", 100, "type", 0, "event");
}

TEST(OfflineLogsDb, DatabasePersistsAfterReopen) {
  TemporaryDirectory temp_dir;
  auto db_path = temp_dir.Path() / "update-logs.db";
  InstallId original_id;

  {
    OfflineLogsDb db(db_path);
    ASSERT_TRUE(db.Ok());
    original_id = db.CreateInstall("device-123", "test-update", 1);
    db.AddLogEntry(original_id, 100, "svc", "log entry");
    // db goes out of scope here
  }

  // Reopen database
  OfflineLogsDb db(db_path);
  ASSERT_TRUE(db.Ok());

  // Should find the same in-progress install
  InstallId found = db.FindInProgressInstall("device-123");
  EXPECT_TRUE(found.IsValid());
  EXPECT_EQ(found, original_id);

  // Verify log entry is still there
  SQLite3Guard sql_db(db_path, true);
  auto statement = sql_db.prepareStatement("SELECT COUNT(*) FROM logs WHERE install_id = ?;", original_id.Value());
  ASSERT_EQ(statement.step(), SQLITE_ROW);
  EXPECT_EQ(statement.get_result_col_int(0), 1);
}

TEST(OfflineLogsDb, HandlesCompleteInstallWithInvalidId) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId invalid;
  // Should not crash, just log a warning
  db.CompleteInstall(invalid, 42, "manifest");
}

TEST(OfflineLogsDb, HandlesCompleteInstallWithNonexistentId) {
  TemporaryDirectory temp_dir;
  OfflineLogsDb db(temp_dir.Path() / "update-logs.db");
  ASSERT_TRUE(db.Ok());

  InstallId nonexistent = InstallId::FromDb(99999);
  // Should not crash, logs a warning about no rows affected
  db.CompleteInstall(nonexistent, 42, "manifest");
}

// ============================================================================
// Symlink Security Tests
// ============================================================================

TEST(OfflineLogsDb, OpenRefusesSymlinkToFile) {
  TemporaryDirectory temp_dir;

  // Create a real database file
  auto real_db_path = temp_dir.Path() / "real.db";
  {
    OfflineLogsDb real_db(real_db_path);
    ASSERT_TRUE(real_db.Ok());
  }

  // Create a symlink pointing to the real database
  auto symlink_path = temp_dir.Path() / "symlink.db";
  boost::filesystem::create_symlink(real_db_path, symlink_path);
  ASSERT_TRUE(boost::filesystem::is_symlink(symlink_path));

  // Opening via symlink should fail
  OfflineLogsDb db(symlink_path);
  EXPECT_FALSE(db.Ok()) << "Open() should refuse to follow symlinks";
}

TEST(OfflineLogsDb, OpenRefusesSymlinkToNonexistent) {
  TemporaryDirectory temp_dir;

  // Create a symlink pointing to a non-existent file
  auto symlink_path = temp_dir.Path() / "symlink.db";
  boost::filesystem::create_symlink("/nonexistent/target.db", symlink_path);
  ASSERT_TRUE(boost::filesystem::is_symlink(symlink_path));

  // Opening via symlink should fail (even though target doesn't exist)
  OfflineLogsDb db(symlink_path);
  EXPECT_FALSE(db.Ok()) << "Open() should refuse to follow symlinks even to non-existent targets";
}

TEST(OfflineLogsDb, OpenSucceedsForRegularFile) {
  TemporaryDirectory temp_dir;
  auto db_path = temp_dir.Path() / "regular.db";

  // First open creates the file
  {
    OfflineLogsDb db(db_path);
    ASSERT_TRUE(db.Ok());
  }

  // Second open should also succeed (file exists, not a symlink)
  OfflineLogsDb db(db_path);
  EXPECT_TRUE(db.Ok()) << "Open() should succeed for regular files";
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
#endif
