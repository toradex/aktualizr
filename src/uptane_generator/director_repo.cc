#include "director_repo.h"

#include <boost/filesystem.hpp>

#include "crypto/crypto.h"
#include "utilities/utils.h"

void DirectorRepo::addTarget(const std::string &target_name, const Json::Value &target, const std::string &hardware_id,
                             const std::string &ecu_serial, const std::string &url, const std::string &expires) {
  const boost::filesystem::path current = path_ / DirectorRepo::dir / "targets.json";
  const boost::filesystem::path staging = path_ / DirectorRepo::dir / "staging/targets.json";

  Json::Value director_targets;
  if (boost::filesystem::exists(staging)) {
    director_targets = Utils::parseJSONFile(staging);
  } else if (boost::filesystem::exists(current)) {
    director_targets = Utils::parseJSONFile(current)["signed"];
  } else {
    throw std::runtime_error(std::string("targets.json not found at ") + staging.c_str() + " or " + current.c_str() +
                             "!");
  }
  if (!expires.empty()) {
    director_targets["expires"] = expires;
  }
  director_targets["targets"][target_name] = target;
  director_targets["targets"][target_name]["custom"].removeMember("hardwareIds");
  director_targets["targets"][target_name]["custom"]["ecuIdentifiers"][ecu_serial]["hardwareId"] = hardware_id;
  if (!url.empty()) {
    director_targets["targets"][target_name]["custom"]["uri"] = url;
  } else {
    director_targets["targets"][target_name]["custom"].removeMember("uri");
  }
  director_targets["targets"][target_name]["custom"].removeMember("version");
  director_targets["version"] = (Utils::parseJSONFile(current)["signed"]["version"].asUInt()) + 1;
  Utils::writeFile(staging, Utils::jsonToCanonicalStr(director_targets));
  updateRepo();
}

void DirectorRepo::revokeTargets(const std::vector<std::string> &targets_to_remove) {
  auto targets_path = path_ / DirectorRepo::dir / "targets.json";
  auto targets_unsigned = Utils::parseJSONFile(targets_path)["signed"];

  Json::Value new_targets;
  for (auto it = targets_unsigned["targets"].begin(); it != targets_unsigned["targets"].end(); ++it) {
    if (std::find(targets_to_remove.begin(), targets_to_remove.end(), it.key().asString()) == targets_to_remove.end()) {
      new_targets[it.key().asString()] = *it;
    }
  }
  targets_unsigned["targets"] = new_targets;
  targets_unsigned["version"] = (targets_unsigned["version"].asUInt()) + 1;
  Utils::writeFile(path_ / DirectorRepo::dir / "targets.json",
                   Utils::jsonToCanonicalStr(signTuf(Uptane::Role::Targets(), targets_unsigned)));
  updateRepo();
}

void DirectorRepo::signTargets() {
  const boost::filesystem::path current = path_ / DirectorRepo::dir / "targets.json";
  const boost::filesystem::path staging = path_ / DirectorRepo::dir / "staging/targets.json";
  Json::Value targets_unsigned;

  if (boost::filesystem::exists(staging)) {
    targets_unsigned = Utils::parseJSONFile(staging);
  } else if (boost::filesystem::exists(current)) {
    targets_unsigned = Utils::parseJSONFile(current)["signed"];
  } else {
    throw std::runtime_error(std::string("targets.json not found at ") + staging.c_str() + " or " + current.c_str() +
                             "!");
  }

  Utils::writeFile(path_ / DirectorRepo::dir / "targets.json",
                   Utils::jsonToCanonicalStr(signTuf(Uptane::Role::Targets(), targets_unsigned)));
  boost::filesystem::remove(path_ / DirectorRepo::dir / "staging/targets.json");
  updateRepo();
}

void DirectorRepo::emptyTargets() {
  const boost::filesystem::path current = path_ / DirectorRepo::dir / "targets.json";
  const boost::filesystem::path staging = path_ / DirectorRepo::dir / "staging/targets.json";

  Json::Value targets_current = Utils::parseJSONFile(current);
  Json::Value targets_unsigned;
  targets_unsigned = Utils::parseJSONFile(staging);
  targets_unsigned["_type"] = "Targets";
  targets_unsigned["expires"] = expiration_time_;
  targets_unsigned["version"] = (targets_current["signed"]["version"].asUInt()) + 1;
  targets_unsigned["targets"] = Json::objectValue;
  if (repo_type_ == Uptane::RepositoryType::Director() && !correlation_id_.empty()) {
    targets_unsigned["custom"]["correlationId"] = correlation_id_;
  }
  Utils::writeFile(staging, Utils::jsonToCanonicalStr(targets_unsigned));
}

void DirectorRepo::oldTargets() {
  const boost::filesystem::path current = path_ / DirectorRepo::dir / "targets.json";
  const boost::filesystem::path staging = path_ / DirectorRepo::dir / "staging/targets.json";

  if (!boost::filesystem::exists(current)) {
    throw std::runtime_error(std::string("targets.json not found at ") + current.c_str() + "!");
  }
  Json::Value targets_current = Utils::parseJSONFile(current);
  Json::Value targets_unsigned = targets_current["signed"];
  Utils::writeFile(staging, Utils::jsonToCanonicalStr(targets_unsigned));
}

void DirectorRepo::addOfflineUpdateTarget(const std::string &target_name, const Json::Value &target,
                                          const std::string &hardware_id, const std::string &offline_targets_name,
                                          const std::string &expires) {
  const boost::filesystem::path targets_file = path_ / DirectorRepo::dir / (offline_targets_name + ".json");
  const boost::filesystem::path staging_dir = path_ / DirectorRepo::dir / "staging";
  const boost::filesystem::path staging_file = staging_dir / (offline_targets_name + ".json");

  boost::filesystem::create_directories(staging_dir);

  Json::Value offline_targets;
  if (boost::filesystem::exists(staging_file)) {
    offline_targets = Utils::parseJSONFile(staging_file);
  } else if (boost::filesystem::exists(targets_file)) {
    offline_targets = Utils::parseJSONFile(targets_file)["signed"];
  } else {
    // Initialize new offline targets metadata
    offline_targets["_type"] = "Offline-Updates";
    offline_targets["expires"] = expires.empty() ? expiration_time_ : expires;
    offline_targets["version"] = 1;
    offline_targets["targets"] = Json::objectValue;
  }

  if (!expires.empty()) {
    offline_targets["expires"] = expires;
  }

  // Add the target - for offline updates, we only include hardware IDs, not ECU serials
  offline_targets["targets"][target_name] = target;
  offline_targets["targets"][target_name]["custom"].removeMember("ecuIdentifiers");
  offline_targets["targets"][target_name]["custom"]["hardwareIds"] = Json::arrayValue;
  offline_targets["targets"][target_name]["custom"]["hardwareIds"].append(hardware_id);
  offline_targets["targets"][target_name]["custom"].removeMember("uri");
  offline_targets["targets"][target_name]["custom"].removeMember("version");

  Utils::writeFile(staging_file, Utils::jsonToCanonicalStr(offline_targets));
}

void DirectorRepo::signOfflineTargets(const std::string &offline_targets_name) {
  const boost::filesystem::path targets_file = path_ / DirectorRepo::dir / (offline_targets_name + ".json");
  const boost::filesystem::path staging_file = path_ / DirectorRepo::dir / "staging" / (offline_targets_name + ".json");

  Json::Value targets_unsigned;
  if (boost::filesystem::exists(staging_file)) {
    targets_unsigned = Utils::parseJSONFile(staging_file);
  } else if (boost::filesystem::exists(targets_file)) {
    targets_unsigned = Utils::parseJSONFile(targets_file)["signed"];
  } else {
    throw std::runtime_error(std::string("Offline targets file not found: ") + offline_targets_name);
  }

  // If updating an existing file, increment version
  if (boost::filesystem::exists(targets_file)) {
    Json::Value existing = Utils::parseJSONFile(targets_file)["signed"];
    targets_unsigned["version"] = existing["version"].asUInt() + 1;
  }

  Utils::writeFile(targets_file, Utils::jsonToCanonicalStr(signTuf(Uptane::Role::OfflineUpdates(), targets_unsigned)));
  boost::filesystem::remove(staging_file);
}

void DirectorRepo::exportLockBox(const boost::filesystem::path &lockbox_path,
                                 const std::vector<std::string> &offline_targets_names,
                                 const std::string &snapshot_expires) {
  const boost::filesystem::path repo_dir = path_ / DirectorRepo::dir;
  const boost::filesystem::path lockbox_metadata_dir = lockbox_path / "metadata" / "director";

  // Create the lockbox directory structure
  boost::filesystem::create_directories(lockbox_metadata_dir);

  // Copy all root metadata files (1.root.json, 2.root.json, etc.)
  if (boost::filesystem::exists(repo_dir)) {
    for (auto &entry : boost::filesystem::directory_iterator(repo_dir)) {
      if (!boost::filesystem::is_regular_file(entry)) {
        continue;
      }

      std::string filename = entry.path().filename().string();

      // Copy all versioned root files
      if (filename.find(".root.json") != std::string::npos) {
        boost::filesystem::copy_file(entry.path(), lockbox_metadata_dir / filename,
                                     boost::filesystem::copy_options::overwrite_existing);
      }
    }
  }

  // Generate offline-snapshot for this LockBox
  Json::Value snapshot;
  snapshot["_type"] = "Offline-Snapshot";
  snapshot["expires"] = snapshot_expires.empty() ? expiration_time_ : snapshot_expires;
  snapshot["version"] = 1;
  snapshot["meta"] = Json::objectValue;

  // Add metadata for each specified offline targets file
  for (const auto &targets_name : offline_targets_names) {
    const boost::filesystem::path targets_file = repo_dir / (targets_name + ".json");
    if (!boost::filesystem::exists(targets_file)) {
      throw std::runtime_error("Offline targets file not found: " + targets_name + ".json");
    }

    // Read and parse the targets file
    Json::Value targets_meta = Utils::parseJSONFile(targets_file);
    if (!targets_meta.isMember("signed") || !targets_meta["signed"].isMember("_type") ||
        targets_meta["signed"]["_type"].asString() != "Offline-Updates") {
      throw std::runtime_error("Invalid offline targets file: " + targets_name + ".json");
    }

    // Add to snapshot metadata
    std::string signed_content = Utils::readFile(targets_file);
    std::string filename = targets_name + ".json";
    snapshot["meta"][filename]["version"] = targets_meta["signed"]["version"].asUInt();
    snapshot["meta"][filename]["length"] = signed_content.size();
    snapshot["meta"][filename]["hashes"]["sha256"] = Crypto::sha256digestHex(signed_content);

    // Copy the targets file to lockbox
    boost::filesystem::copy_file(targets_file, lockbox_metadata_dir / filename,
                                 boost::filesystem::copy_options::overwrite_existing);
  }

  // Write the offline-snapshot to the lockbox
  const boost::filesystem::path lockbox_snapshot = lockbox_metadata_dir / "offline-snapshot.json";
  Utils::writeFile(lockbox_snapshot, Utils::jsonToCanonicalStr(signTuf(Uptane::Role::OfflineSnapshot(), snapshot)));
}
