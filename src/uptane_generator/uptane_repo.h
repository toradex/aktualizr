#ifndef UPTANE_REPO_H_
#define UPTANE_REPO_H_

#include "director_repo.h"
#include "image_repo.h"

/**
 * @brief Test utility for generating Uptane repository metadata
 *
 * UptaneRepo provides a convenient interface for creating both Director and Image
 * repositories with their associated metadata. It supports two primary use cases:
 * standard "online" updates and PURE-2 "offline" updates via LockBoxes.
 *
 * @section online_usage Online Update Workflow
 *
 * For standard Uptane updates where devices connect to repositories:
 *
 * @code
 * // 1. Create repository
 * UptaneRepo repo(repo_path, "2029-07-04T16:33:27Z", "correlation_id");
 * repo.generateRepo(KeyType::kED25519);
 *
 * // 2. Add image(s) to the image repository
 * repo.addImage(image_file_path, "target_name.txt", "hardware_id");
 *
 * // 3. Add target(s) to the director repository
 * repo.addTarget("target_name.txt", "hardware_id", "ecu_serial");
 *
 * // 4. Sign the director targets metadata
 * repo.signTargets();
 * @endcode
 *
 * @section offline_usage Offline Update Workflow (PURE-2 LockBoxes)
 *
 * For offline updates delivered via removable media:
 *
 * @code
 * // 1. Create repository
 * UptaneRepo repo(repo_path, "2029-07-04T16:33:27Z", "urn:tdx-ota:lockbox:name:1:id");
 * repo.generateRepo();
 *
 * // 2. Add image(s) to the image repository
 * repo.addCustomImage("target_name", hash, length, "hardware_id");
 *
 * // 3. Add target(s) to offline-updates metadata (can call multiple times)
 * repo.addOfflineUpdateTarget("target_name", "hardware_id", "lockbox_name", "2024-06-12T20:08:06Z");
 *
 * // 4. Sign the offline-updates metadata
 * repo.signOfflineTargets("lockbox_name");
 *
 * // 5. Export complete LockBox (generates offline-snapshot and exports metadata)
 * repo.exportLockBox(lockbox_path, {"lockbox_name"}, "2024-07-06T15:31:56Z");
 * @endcode
 *
 * The LockBox directory structure will be:
 * @code
 * lockbox_path/
 *   metadata/
 *     director/
 *       1.root.json
 *       2.root.json (if rotated)
 *       offline-snapshot.json
 *       lockbox_name.json
 * @endcode
 *
 * @note Key difference: Offline updates use hardware IDs (not ECU serials) in the
 * offline-updates metadata, and the offline-snapshot is generated per-LockBox during export.
 */
class UptaneRepo {
 public:
  UptaneRepo(const boost::filesystem::path &path, const std::string &expires, const std::string &correlation_id);
  void generateRepo(KeyType key_type = KeyType::kRSA2048);
  void addTarget(const std::string &target_name, const std::string &hardware_id, const std::string &ecu_serial,
                 const std::string &url = "", const std::string &expires = "");
  void addImage(const boost::filesystem::path &image_path, const boost::filesystem::path &targetname,
                const std::string &hardware_id, const std::string &url = "", int32_t custom_version = 0,
                const Delegation &delegation = {}, const Json::Value &custom = {});
  void addCustomImage(const std::string &name, const Hash &hash, uint64_t length, const std::string &hardware_id,
                      const std::string &url = "", int32_t custom_version = 0, const Delegation &delegation = {},
                      const Json::Value &custom = {});
  void addDelegation(const Uptane::Role &name, const Uptane::Role &parent_role, const std::string &path,
                     bool terminating, KeyType key_type);
  void revokeDelegation(const Uptane::Role &name);
  void signTargets();
  void emptyTargets();
  void oldTargets();
  void generateCampaigns();
  void refresh(Uptane::RepositoryType repo_type, const Uptane::Role &role, const TimeStamp &expiry = TimeStamp());
  void rotate(Uptane::RepositoryType repo_type, const Uptane::Role &role, KeyType key_type = KeyType::kRSA2048);

  // Offline update support
  void addOfflineUpdateTarget(const std::string &target_name, const std::string &hardware_id,
                              const std::string &offline_targets_name, const std::string &expires = "");
  void signOfflineTargets(const std::string &offline_targets_name);
  void exportLockBox(const boost::filesystem::path &lockbox_path, const std::vector<std::string> &offline_targets_names,
                     const std::string &snapshot_expires = "");

 private:
  DirectorRepo director_repo_;
  ImageRepo image_repo_;
};

#endif  // UPTANE_REPO_H_
