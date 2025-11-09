/**
 * \file
 * Tests for Aktualizr's offline update operation using PURE-2 LockBoxes
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
#include "libaktualizr/aktualizr.h"
#include "uptane_repo.h"
#include "uptane_test_common.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

#ifdef BUILD_OFFLINE_UPDATES

/**
 * Testing base class that sets up an Aktualizr instance with a generated offline update LockBox.
 *
 * This test exercises the offline update scaffolding:
 * - Aktualizr is the Device Under Test (DUT)
 * - UptaneRepo generates an offline update LockBox
 * - Test does not trigger the actual offline update yet
 */
class AktualizrOfflineUpdate : public testing::Test {
 public:
  AktualizrOfflineUpdate(AktualizrOfflineUpdate&&) = delete;
  AktualizrOfflineUpdate(const AktualizrOfflineUpdate&) = delete;
  AktualizrOfflineUpdate& operator=(const AktualizrOfflineUpdate&) = delete;
  AktualizrOfflineUpdate& operator=(AktualizrOfflineUpdate&&) = delete;
  ~AktualizrOfflineUpdate() override = default;

 protected:
  AktualizrOfflineUpdate()
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
 * Initialize Aktualizr with the LockBox present and verify it loads successfully
 */
TEST_F(AktualizrOfflineUpdate, AktualizrInitWithLockBox) {  // NOLINT
  auto http = std::make_shared<HttpFake>(temp_dir_.Path(), "", uptane_metadata_dir_ / "repo");
  auto conf = UptaneTestCommon::makeTestConfig(temp_dir_, http->tls_server);
  conf.import.base_path = aktualizr_dir_ / "import";

  auto storage = INvStorage::newStorage(conf.storage);

  UptaneTestCommon::TestAktualizr dut(conf, storage, http);
  dut.Initialize();

  std::string director_root;
  EXPECT_TRUE(storage->loadLatestRoot(&director_root, Uptane::RepositoryType::Director()))
      << "Director root metadata should be imported";
  EXPECT_FALSE(director_root.empty()) << "Director root metadata should not be empty";
  std::string image_root;
  EXPECT_TRUE(storage->loadLatestRoot(&image_root, Uptane::RepositoryType::Image()))
      << "Image root metadata should be imported";
  EXPECT_FALSE(image_root.empty()) << "Image root metadata should not be empty";

  auto update_result = dut.CheckUpdatesOffline(lockbox_dir_).get();
  ASSERT_EQ(update_result.status, result::UpdateStatus::kUpdatesAvailable);
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
