/**
 * \file
 * Tests for SotaUptaneClient's offline update operation using PURE-2 LockBoxes
 */

#include <gtest/gtest.h>
#include <json/json.h>
#include <libaktualizr/config.h>
#include <libaktualizr/events.h>
#include <libaktualizr/packagemanagerinterface.h>
#include <libaktualizr/types.h>
#include <logging/logging.h>
#include <sqlite3.h>
#include <utilities/utils.h>
#include <sstream>

#include "httpfake.h"
#include "primary/sotauptaneclient.h"
#include "primary/test_journal.h"
#include "storage/offline_logs_db.h"
#include "uptane_repo.h"
#include "uptane_test_common.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

#ifdef BUILD_OFFLINE_UPDATES

/**
 * Testing base class that sets up a SotaUptaneClient instance with a generated offline update LockBox.
 *
 * This test exercises the offline update functionality:
 * - SotaUptaneClient is the Device Under Test (DUT)
 * - UptaneRepo generates an offline update LockBox
 */
class SotaUptaneClientOfflineUpdate : public testing::Test {
 public:
  SotaUptaneClientOfflineUpdate(SotaUptaneClientOfflineUpdate &&) = delete;
  SotaUptaneClientOfflineUpdate(const SotaUptaneClientOfflineUpdate &) = delete;
  SotaUptaneClientOfflineUpdate &operator=(const SotaUptaneClientOfflineUpdate &) = delete;
  SotaUptaneClientOfflineUpdate &operator=(SotaUptaneClientOfflineUpdate &&) = delete;
  ~SotaUptaneClientOfflineUpdate() override = default;

 protected:
  SotaUptaneClientOfflineUpdate()
      : uptane_metadata_dir_{temp_dir_.Path() / "uptane"},
        repo_dir_{temp_dir_.Path() / "repo"},
        lockbox_dir_{temp_dir_.Path() / "lockbox"},
        aktualizr_dir_{temp_dir_.Path() / "aktualizr"},
        repo_{repo_dir_, "2029-07-04T16:33:27Z", "urn:tdx-ota:lockbox:test:1:abc123"} {
    // Generate Uptane repository with offline update support
    repo_.generateRepo(KeyType::kED25519);

    // Create and add a target image
    const std::string hwid = "primary_hw";
    auto firmware_path = repo_dir_ / "targets/primary_firmware.txt";
    Utils::writeFile(firmware_path, std::string("offline update firmware"));
    repo_.addImage(firmware_path, "primary_firmware.txt", hwid);

    // Add offline update target
    const std::string lockbox_name = "test_lockbox";
    repo_.addOfflineUpdateTarget("primary_firmware.txt", hwid, lockbox_name, "2029-06-01T00:00:00Z");
    repo_.signOfflineTargets(lockbox_name);

    // Export LockBox
    repo_.exportLockBox(lockbox_dir_, {lockbox_name}, "2029-06-01T00:00:00Z");

    // Provision the device with root metadata
    fs::path import = aktualizr_dir_ / "import";
    fs::path director_import = import / "director";
    fs::create_directories(director_import);
    fs::copy_file(repo_dir_ / "repo/director/1.root.json", director_import / "root.json");
    fs::path image_import = import / "repo";
    fs::create_directories(image_import);
    fs::copy_file(repo_dir_ / "repo/repo/1.root.json", image_import / "root.json");
  }

  TemporaryDirectory temp_dir_;
  fs::path uptane_metadata_dir_;
  fs::path repo_dir_;
  fs::path lockbox_dir_;
  fs::path aktualizr_dir_;
  UptaneRepo repo_;
};

/**
 * Initialize SotaUptaneClient with the LockBox present and verify fetchMetaOffUpd works successfully
 */
TEST_F(SotaUptaneClientOfflineUpdate, FetchMetaOfflineWithLockBox) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";

  auto storage = INvStorage::newStorage(conf.storage);
  // Import root metadata from the import path before initialization.
  // This is done automatically by Aktualizr::Initialize() but must be done manually for SotaUptaneClient.
  storage->importData(conf.import);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  std::string director_root;
  EXPECT_TRUE(storage->loadLatestRoot(&director_root, Uptane::RepositoryType::Director()))
      << "Director root metadata should be imported";
  EXPECT_FALSE(director_root.empty()) << "Director root metadata should not be empty";
  std::string image_root;
  EXPECT_TRUE(storage->loadLatestRoot(&image_root, Uptane::RepositoryType::Image()))
      << "Image root metadata should be imported";
  EXPECT_FALSE(image_root.empty()) << "Image root metadata should not be empty";

  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
}

/**
 * Test that fetchMetaOffUpd returns appropriate status when lockbox path does not exist
 */
TEST_F(SotaUptaneClientOfflineUpdate, FetchMetaOfflineNonExistentPath) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";

  auto storage = INvStorage::newStorage(conf.storage);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  auto update_result = dut.fetchMetaOffUpd(temp_dir_.Path() / "nonexistent_lockbox");
  ASSERT_EQ(update_result.status, result::UpdateStatus::kError);
}

/**
 * Test that fetchMetaOffUpd stores the offline update path when updates are available
 */
TEST_F(SotaUptaneClientOfflineUpdate, FetchMetaStoresOfflineUpdatePath) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";

  auto storage = INvStorage::newStorage(conf.storage);
  storage->importData(conf.import);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  // Initially, no offline update path should be stored
  auto path_before = storage->loadOfflineUpdatePath();
  EXPECT_FALSE(path_before) << "No offline update path should be stored initially";

  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);

  // After successful fetchMetaOffUpd, the path should be stored
  auto path_after = storage->loadOfflineUpdatePath();
  EXPECT_TRUE(path_after) << "Offline update path should be stored after fetchMetaOffUpd";
  EXPECT_EQ(*path_after, lockbox_dir_) << "Stored path should match the lockbox directory";
}

/**
 * Test that offline logging is NOT started during fetchMetaOffUpd (it starts in downloadImages)
 */
TEST_F(SotaUptaneClientOfflineUpdate, FetchMetaDoesNotCreateOfflineLogsDb) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";
  conf.logger.offline_logs_enabled = true;
  conf.logger.offline_logs_file = "update-logs.db";

  auto storage = INvStorage::newStorage(conf.storage);
  storage->importData(conf.import);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  // Offline logs database should not exist before the update
  fs::path logs_db_path = lockbox_dir_ / "update-logs.db";
  EXPECT_FALSE(fs::exists(logs_db_path)) << "Offline logs database should not exist initially";

  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);

  // Offline logs database should NOT exist yet - it's created during downloadImages
  EXPECT_FALSE(fs::exists(logs_db_path))
      << "Offline logs database should NOT be created after fetchMetaOffUpd (created in downloadImages)";
}

/**
 * Test that offline logging can be disabled via configuration
 */
TEST_F(SotaUptaneClientOfflineUpdate, FetchMetaWithLoggingDisabled) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";
  conf.logger.offline_logs_enabled = false;  // Disable offline logging

  auto storage = INvStorage::newStorage(conf.storage);
  storage->importData(conf.import);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);

  // Offline logs database should NOT be created when logging is disabled
  fs::path logs_db_path = lockbox_dir_ / "update-logs.db";
  EXPECT_FALSE(fs::exists(logs_db_path)) << "Offline logs database should not be created when logging is disabled";

  // But the offline update path should still be stored
  auto path_after = storage->loadOfflineUpdatePath();
  EXPECT_TRUE(path_after) << "Offline update path should still be stored even with logging disabled";
}

/**
 * Test that offline logging is started during downloadImages for offline updates
 */
TEST_F(SotaUptaneClientOfflineUpdate, OfflineLoggingStartsDuringDownload) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";
  conf.logger.offline_logs_enabled = true;
  conf.logger.offline_logs_file = "update-logs.db";

  auto storage = INvStorage::newStorage(conf.storage);
  storage->importData(conf.import);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  // Step 1: Fetch metadata from lockbox - this stores the path but doesn't start logging
  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  ASSERT_FALSE(update_result.updates.empty());

  // Verify the logs database does NOT exist yet after fetchMetaOffUpd
  fs::path logs_db_path = lockbox_dir_ / "update-logs.db";
  EXPECT_FALSE(fs::exists(logs_db_path)) << "Offline logs database should NOT exist after fetchMetaOffUpd";

  // Verify offline update path is stored
  auto path_after_fetch = storage->loadOfflineUpdatePath();
  EXPECT_TRUE(path_after_fetch) << "Offline update path should be stored after fetchMetaOffUpd";
  EXPECT_EQ(*path_after_fetch, lockbox_dir_) << "Stored path should match lockbox directory";

  // Step 2: Call downloadImages - this should start offline logging
  // Note: This will fail to download since images aren't in lockbox, but logging should still start
  auto download_result = dut.downloadImages(update_result.updates, UpdateType::kOffline);

  // The download may fail (no images in test lockbox), but the logs database should be created
  EXPECT_TRUE(fs::exists(logs_db_path)) << "Offline logs database should exist after downloadImages";
}

/**
 * Test that offline update path persistence survives storage close/reopen
 */
TEST_F(SotaUptaneClientOfflineUpdate, OfflineUpdatePathPersistence) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";

  // First session: fetch metadata and store path
  {
    auto storage = INvStorage::newStorage(conf.storage);
    storage->importData(conf.import);

    UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
    dut.initialize();

    auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
    ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  }

  // Second session: verify path is still stored
  {
    auto storage = INvStorage::newStorage(conf.storage);
    auto path = storage->loadOfflineUpdatePath();
    EXPECT_TRUE(path) << "Offline update path should persist across storage sessions";
    EXPECT_EQ(*path, lockbox_dir_) << "Persisted path should match original";
  }
}

/**
 * HttpFake subclass that simulates network failure on PUT /manifest
 */
class HttpFakeOffline : public HttpFake {
 public:
  using HttpFake::HttpFake;

  HttpResponse put(const std::string &url, const Json::Value &data) override {
    if (url.find("/manifest") != std::string::npos) {
      // Simulate network failure
      last_manifest = data;
      return HttpResponse("", 0, CURLE_COULDNT_CONNECT, "Connection failed");
    }
    return HttpFake::put(url, data);
  }
};

/**
 * Test that manifest is written to offline logs database even when device is offline.
 *
 * This validates the fix for a bug where the manifest was never written to the
 * 'installs' table when the device was offline (i.e., when putManifest failed
 * to send to the server).
 */
TEST_F(SotaUptaneClientOfflineUpdate, ManifestWrittenToOfflineLogsWhenDeviceOffline) {  // NOLINT
  // Use HttpFakeOffline which simulates network failure on PUT /manifest
  auto http = std::make_shared<HttpFakeOffline>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";
  conf.logger.offline_logs_enabled = true;
  conf.logger.offline_logs_file = "update-logs.db";

  auto storage = INvStorage::newStorage(conf.storage);
  storage->importData(conf.import);

  UptaneTestCommon::TestUptaneClient dut(conf, storage, http);
  dut.initialize();

  // Step 1: Fetch metadata from lockbox
  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  ASSERT_FALSE(update_result.updates.empty());

  // Step 2: Download images (this starts offline logging)
  auto download_result = dut.downloadImages(update_result.updates, UpdateType::kOffline);

  // Verify the logs database was created
  fs::path logs_db_path = lockbox_dir_ / "update-logs.db";
  ASSERT_TRUE(fs::exists(logs_db_path)) << "Offline logs database should exist after downloadImages";

  // Step 3: Install the update (this will call putManifestSimple which will fail due to HttpFakeOffline)
  auto install_result = dut.uptaneInstall(update_result.updates, UpdateType::kOffline);

  // Step 4: Try to send manifest (this will fail due to network simulation)
  auto manifest_result = dut.putManifest();
  EXPECT_EQ(manifest_result.status, result::PutManifestStatus::kNoNetwork)
      << "Manifest send should fail due to simulated network failure";

  // Step 5: Verify the manifest was still written to the offline logs database
  // Open the database and check for a completed install with manifest
  OfflineLogsDb db(logs_db_path);
  ASSERT_TRUE(db.Ok()) << "Should be able to open the offline logs database";

  // Query for completed installs (those with non-null manifest)
  // We need to use SQLite directly since OfflineLogsDb doesn't expose a query method
  sqlite3 *sqlite_db = nullptr;
  int rc = sqlite3_open(logs_db_path.c_str(), &sqlite_db);
  ASSERT_EQ(rc, SQLITE_OK) << "Should be able to open database with sqlite3";

  sqlite3_stmt *stmt = nullptr;
  const char *query = "SELECT manifest FROM installs WHERE manifest IS NOT NULL ORDER BY id DESC LIMIT 1";
  rc = sqlite3_prepare_v2(sqlite_db, query, -1, &stmt, nullptr);
  ASSERT_EQ(rc, SQLITE_OK) << "Should be able to prepare query";

  rc = sqlite3_step(stmt);
  EXPECT_EQ(rc, SQLITE_ROW) << "Should find a completed install with manifest in offline logs database";

  if (rc == SQLITE_ROW) {
    const char *manifest_text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    EXPECT_NE(manifest_text, nullptr) << "Manifest should not be null";
    if (manifest_text != nullptr) {
      std::string manifest_str(manifest_text);
      EXPECT_FALSE(manifest_str.empty()) << "Manifest should not be empty";
      // Verify it's valid JSON
      Json::Value manifest_json;
      Json::CharReaderBuilder builder;
      std::string errs;
      std::istringstream stream(manifest_str);
      EXPECT_TRUE(Json::parseFromStream(builder, stream, &manifest_json, &errs)) << "Manifest should be valid JSON";
    }
  }

  sqlite3_finalize(stmt);
  sqlite3_close(sqlite_db);
}

/**
 * Drive a complete offline update cycle through SotaUptaneClient with a
 * dependency-injected TestJournal, and verify the offline logs database
 * captures both the install metadata and the journal entries that arrive
 * during the download and install phases.
 *
 * Test stages:
 *   1. fetchMetaOffUpd()       - reads metadata from the lockbox and stores the
 *                                offline update path
 *   2. downloadImages(kOffline) - begins the install in the offline logs db,
 *                                 creates a journal clone and seeks to tail
 *   3. (add journal entries)   - simulates log messages that arrive while the
 *                                install is running
 *   4. uptaneInstall(kOffline)  - applies the update; calls CaptureLogs() and
 *                                 CaptureReports() before returning
 *   5. putManifestSimple()      - calls CompleteInstall() which writes the
 *                                 manifest to the install record
 *
 * After the cycle we directly query the offline logs database to confirm:
 *   - an install row was created with the device id, name, and version,
 *   - a non-null manifest is present on completion,
 *   - the journal entries added during the cycle are present in the logs table
 *     (filtered by capture_services).
 */
TEST_F(SotaUptaneClientOfflineUpdate, FullUpdateCycleWithTestJournal) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";
  conf.logger.offline_logs_enabled = true;
  conf.logger.offline_logs_file = "update-logs.db";
  conf.logger.capture_services = {"aktualizr"};

  auto storage = INvStorage::newStorage(conf.storage);
  storage->importData(conf.import);

  // Give the device a known id so we can assert it shows up in the install row.
  const std::string device_id = "test-device-1234";
  storage->storeDeviceId(device_id);

  // Dependency-inject a TestJournal prototype; SotaUptaneClient (and therefore
  // OfflineLogsManager) will clone it when offline logging starts.
  auto journal = std::make_shared<TestJournal>();
  UptaneTestCommon::TestUptaneClient dut(conf, storage, http, journal);
  dut.initialize();

  // --- Step 1: fetchMetaOffUpd --------------------------------------------
  auto update_result = dut.fetchMetaOffUpd(lockbox_dir_);
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
  ASSERT_FALSE(update_result.updates.empty());
  ASSERT_EQ(update_result.updates.size(), 1U);
  EXPECT_EQ(update_result.updates[0].filename(), "primary_firmware.txt");

  // The image binary should be in the lockbox at images/<filename>, otherwise
  // the offline fetcher will not be able to read it during downloadImages.
  fs::path image_in_lockbox = lockbox_dir_ / "images" / "primary_firmware.txt";
  ASSERT_TRUE(fs::exists(image_in_lockbox)) << "Image file should be present in lockbox at " << image_in_lockbox;

  // --- Step 2: downloadImages(kOffline) -----------------------------------
  auto download_result = dut.downloadImages(update_result.updates, UpdateType::kOffline);
  ASSERT_EQ(download_result.status, result::DownloadStatus::kSuccess)
      << "Downloading the image from the lockbox should succeed";
  ASSERT_EQ(download_result.updates.size(), 1U);

  // The offline logs database should now exist on the lockbox media and have
  // an in-progress install row (no manifest yet).
  fs::path logs_db_path = lockbox_dir_ / "update-logs.db";
  ASSERT_TRUE(fs::exists(logs_db_path)) << "Offline logs database should exist after downloadImages";

  {
    OfflineLogsDb db(logs_db_path);
    ASSERT_TRUE(db.Ok());

    InstallId in_progress = db.FindInProgressInstall(device_id);
    ASSERT_TRUE(in_progress.IsValid()) << "An in-progress install should be recorded in the logs db";
  }

  // --- Step 3: simulate journal entries that arrive during the cycle ------
  // CaptureLogs() reads from the journal's current cursor, so the journal
  // clone's position at the time of capture determines which entries are
  // persisted. Add entries with the configured service name plus an entry
  // from a non-captured service to verify filtering.
  journal->AddEntry("aktualizr.service", "Beginning install of primary_firmware.txt", 1731142800000000LL, "cursor-1");
  journal->AddEntry("aktualizr.service", "Image verified, applying", 1731142801000000LL, "cursor-2");
  journal->AddEntry("sshd.service", "Accepted publickey for foo", 1731142801500000LL, "cursor-3");
  journal->AddEntry("aktualizr.service", "Install complete", 1731142802000000LL, "cursor-4");

  // --- Step 4: uptaneInstall(kOffline) -------------------------------------
  auto install_result = dut.uptaneInstall(update_result.updates, UpdateType::kOffline);
  EXPECT_FALSE(install_result.ecu_reports.empty())
      << "uptaneInstall should report the updates it processed (success, needs completion, or failure)";

  // uptaneInstall calls CaptureLogs() and CaptureReports() for offline updates,
  // so the journal entries we added before uptaneInstall() should now be in
  // the database.
  {
    sqlite3 *sqlite_db = nullptr;
    int rc = sqlite3_open(logs_db_path.c_str(), &sqlite_db);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = nullptr;
    const char *query =
        "SELECT service, message FROM logs "
        "WHERE install_id = (SELECT id FROM installs WHERE device_id = ? AND manifest IS NULL LIMIT 1) "
        "ORDER BY id ASC";
    rc = sqlite3_prepare_v2(sqlite_db, query, -1, &stmt, nullptr);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<std::pair<std::string, std::string>> captured;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *svc = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
      const char *msg = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
      captured.emplace_back(svc != nullptr ? svc : "", msg != nullptr ? msg : "");
    }
    sqlite3_finalize(stmt);

    // Only aktualizr.service entries should be captured.
    ASSERT_EQ(captured.size(), 3U) << "Expected 3 captured aktualizr log entries, got " << captured.size();
    EXPECT_EQ(captured[0].first, "aktualizr");
    EXPECT_EQ(captured[0].second, "Beginning install of primary_firmware.txt");
    EXPECT_EQ(captured[1].first, "aktualizr");
    EXPECT_EQ(captured[1].second, "Image verified, applying");
    EXPECT_EQ(captured[2].first, "aktualizr");
    EXPECT_EQ(captured[2].second, "Install complete");

    sqlite3_close(sqlite_db);
  }

  // --- Step 5: putManifest() -----------------------------------------------
  // putManifest calls putManifestSimple(), which in turn calls CompleteInstall()
  // to record the manifest and runs a final CaptureLogs(); any new journal
  // entries that arrived between uptaneInstall() and now will be persisted
  // here too.
  journal->AddEntry("aktualizr.service", "Final flush before completing", 1731142803000000LL, "cursor-5");

  auto manifest_result = dut.putManifest();
  EXPECT_TRUE(manifest_result.success())
      << "putManifest should succeed (it does not need network for offline updates — "
         "the manifest is sent via the regular mechanism if reachable)";

  // --- Final verification of the offline logs database --------------------
  // The install record should now have a non-null manifest and a populated
  // report_counter. A final log entry (added before putManifestSimple) should
  // also be present.
  {
    sqlite3 *sqlite_db = nullptr;
    int rc = sqlite3_open(logs_db_path.c_str(), &sqlite_db);
    ASSERT_EQ(rc, SQLITE_OK);

    sqlite3_stmt *stmt = nullptr;
    const char *query =
        "SELECT manifest, report_counter FROM installs WHERE device_id = ? "
        "ORDER BY id DESC LIMIT 1";
    rc = sqlite3_prepare_v2(sqlite_db, query, -1, &stmt, nullptr);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);

    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW) << "Should find a completed install for the device";

    const char *manifest_text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    ASSERT_NE(manifest_text, nullptr) << "Manifest column should not be null after CompleteInstall";
    std::string manifest_str(manifest_text);
    EXPECT_FALSE(manifest_str.empty()) << "Manifest should be a non-empty JSON document";

    // Verify it's valid JSON.
    Json::Value manifest_json;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(manifest_str);
    EXPECT_TRUE(Json::parseFromStream(builder, stream, &manifest_json, &errs))
        << "Manifest should be valid JSON: " << errs;

    sqlite3_finalize(stmt);

    // The install should no longer be reported as in-progress.
    InstallId in_progress_after;
    {
      OfflineLogsDb db(logs_db_path);
      ASSERT_TRUE(db.Ok());
      in_progress_after = db.FindInProgressInstall(device_id);
    }
    EXPECT_FALSE(in_progress_after.IsValid()) << "No install should be in progress after CompleteInstall";

    // Final journal flush should have captured the "Final flush" entry.
    sqlite3_stmt *log_stmt = nullptr;
    const char *log_query =
        "SELECT message FROM logs WHERE install_id = (SELECT id FROM installs WHERE device_id = ?) "
        "AND message = 'Final flush before completing'";
    rc = sqlite3_prepare_v2(sqlite_db, log_query, -1, &log_stmt, nullptr);
    ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_text(log_stmt, 1, device_id.c_str(), -1, SQLITE_TRANSIENT);
    EXPECT_EQ(sqlite3_step(log_stmt), SQLITE_ROW)
        << "The journal entry added immediately before putManifestSimple should be captured by CompleteInstall's "
           "final CaptureLogs()";
    sqlite3_finalize(log_stmt);

    sqlite3_close(sqlite_db);
  }
}

#endif  // BUILD_OFFLINE_UPDATES

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  // Verify we're running from the aktualizr source root directory
  if (!fs::exists("tests/config/basic.toml")) {
    std::cerr << "Error: This program must be run from the root of an aktualizr source checkout.\n";
    return EXIT_FAILURE;
  }

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
