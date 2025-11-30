#include <gtest/gtest.h>

#include <logging/logging.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include "libaktualizr/config.h"
#include "libaktualizr/types.h"
#include "storage/sqlstorage.h"
#include "uptane/directorrepository.h"
#include "uptane/exceptions.h"
#include "uptane/fetcher.h"
#include "uptane_repo.h"

namespace fs = boost::filesystem;
fs::path offline_update_path;  // NOLINT

#ifdef BUILD_OFFLINE_UPDATES

using Uptane::DirectorRepository;
using Uptane::EcuSerial;
using Uptane::HardwareIdentifier;
using Uptane::OfflineUpdateFetcher;

// NOLINTNEXTLINE
TEST(DirectorOffline, Simple) {
  DirectorRepository dut;
  const TemporaryDirectory dir;

  StorageConfig storage_config;
  storage_config.path = dir.Path();

  // Simulate the initial device provisioning in /var/sota/import
  fs::path import = dir.Path() / "import";
  fs::path director_import = import / "director";
  fs::create_directories(director_import);
  fs::copy_file(offline_update_path / "metadata/director/1.root.json", director_import / "root.json");

  SQLStorage storage{storage_config, false};

  ImportConfig import_config;
  import_config.base_path = import;
  storage.importData(import_config);

  EcuSerials const ecu_serials{std::make_pair(EcuSerial("serial1"), HardwareIdentifier("hw1"))};

  storage.storeEcuSerials(ecu_serials);
  storage.stashEcuSerialsForHwId(ecu_serials);

  OfflineUpdateFetcher const fetcher(offline_update_path);
  dut.ForceNowForTesting(TimeStamp("2024-01-01T20:01:00Z"));
  dut.updateMetaOffUpd(storage, fetcher);

  auto correlation_id = dut.getCorrelationId();
  EXPECT_EQ(correlation_id, "urn:tdx-ota:lockbox:test1:1:188c4ce5faa5");
}

// NOLINTNEXTLINE
TEST(DirectorOffline, Unprovisioned) {
  DirectorRepository dut;
  const TemporaryDirectory dir;
  StorageConfig storage_config;
  storage_config.path = dir.Path();
  SQLStorage storage{storage_config, false};

  EcuSerials const ecu_serials{std::make_pair(EcuSerial("serial1"), HardwareIdentifier("hw1"))};

  storage.storeEcuSerials(ecu_serials);
  storage.stashEcuSerialsForHwId(ecu_serials);

  OfflineUpdateFetcher const fetcher(offline_update_path);
  dut.ForceNowForTesting(TimeStamp("2024-01-01T20:01:00Z"));
  // NOLINTNEXTLINE
  EXPECT_THROW(dut.updateMetaOffUpd(storage, fetcher), Uptane::Exception)
      << "Shouldn't accept an update before provisioning";
}

// NOLINTNEXTLINE
TEST(DirectorOffline, GeneratedMetadata) {
  DirectorRepository dut;
  const TemporaryDirectory dir;
  const TemporaryDirectory repo_dir;

  // Generate an Uptane repository with offline update metadata
  UptaneRepo repo(repo_dir.Path(), "2024-12-31T23:59:59Z", "urn:tdx-ota:lockbox:test2:1:generated");
  repo.generateRepo();

  // Add a dummy image to the image repo
  const std::string target_name = "6/qemuarm64/torizon/torizon-core-docker/test-generated";
  const std::string hardware_id = "qemuarm64";
  repo.addCustomImage(target_name,
                      Hash(Hash::Type::kSha256, "a4a3a4a67aa274d7276b1824759debe64178530149daea25845017b28a54a397"), 0,
                      hardware_id);

  // Add offline update target
  repo.addOfflineUpdateTarget(target_name, hardware_id, "test2", "2024-06-12T20:08:06Z");
  repo.signOfflineTargets("test2");

  // Export the LockBox (this generates the offline-snapshot and exports everything)
  const TemporaryDirectory lockbox_dir;
  repo.exportLockBox(lockbox_dir.Path(), {"test2"}, "2024-07-06T15:31:56Z");

  // Set up storage and import the generated metadata
  StorageConfig storage_config;
  storage_config.path = dir.Path();

  fs::path import = dir.Path() / "import";
  fs::path director_import = import / "director";
  fs::create_directories(director_import);

  // Copy root metadata from generated repo
  fs::copy_file(repo_dir.Path() / "repo/director/1.root.json", director_import / "root.json");

  SQLStorage storage{storage_config, false};

  ImportConfig import_config;
  import_config.base_path = import;
  storage.importData(import_config);

  EcuSerials const ecu_serials{std::make_pair(EcuSerial("serial1"), HardwareIdentifier(hardware_id))};

  storage.storeEcuSerials(ecu_serials);
  storage.stashEcuSerialsForHwId(ecu_serials);

  OfflineUpdateFetcher const fetcher(lockbox_dir.Path());
  dut.ForceNowForTesting(TimeStamp("2024-01-01T20:01:00Z"));
  dut.updateMetaOffUpd(storage, fetcher);

  auto correlation_id = dut.getCorrelationId();
  // For offline updates, the correlation ID is derived from the offline targets metadata hash
  EXPECT_TRUE(correlation_id.find("urn:tdx-ota:lockbox:test2:1:") == 0);
  EXPECT_GT(correlation_id.length(), std::string("urn:tdx-ota:lockbox:test2:1:").length());
}

#endif  // BUILD_OFFLINE_UPDATES

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc != 2) {
    // NOLINTNEXTLINE
    std::cerr << "Error: " << argv[0] << " requires a path to tests/test_data/offline1\n";
    return EXIT_FAILURE;
  }
  // NOLINTNEXTLINE
  offline_update_path = argv[1];

  if (!boost::filesystem::is_directory(offline_update_path)) {
    std::cerr << "Error: " << offline_update_path << " is not a directory\n";
    return EXIT_FAILURE;
  }

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
