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
#include <utilities/utils.h>

#include "httpfake.h"
#include "primary/sotauptaneclient.h"
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
  SotaUptaneClientOfflineUpdate(SotaUptaneClientOfflineUpdate&&) = delete;
  SotaUptaneClientOfflineUpdate(const SotaUptaneClientOfflineUpdate&) = delete;
  SotaUptaneClientOfflineUpdate& operator=(const SotaUptaneClientOfflineUpdate&) = delete;
  SotaUptaneClientOfflineUpdate& operator=(SotaUptaneClientOfflineUpdate&&) = delete;
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

#endif  // BUILD_OFFLINE_UPDATES

int main(int argc, char** argv) {
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
