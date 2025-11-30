#ifndef DIRECTOR_REPO_H_
#define DIRECTOR_REPO_H_

#include "repo.h"

class DirectorRepo : public Repo {
 public:
  DirectorRepo(boost::filesystem::path path, const std::string &expires, std::string correlation_id)
      : Repo(Uptane::RepositoryType::Director(), std::move(path), expires, std::move(correlation_id)) {}
  void addTarget(const std::string &target_name, const Json::Value &target, const std::string &hardware_id,
                 const std::string &ecu_serial, const std::string &url = "", const std::string &expires = "");
  void revokeTargets(const std::vector<std::string> &targets_to_remove);
  void signTargets();
  void emptyTargets();
  void oldTargets();

  // Offline update support
  void addOfflineUpdateTarget(const std::string &target_name, const Json::Value &target, const std::string &hardware_id,
                              const std::string &offline_targets_name, const std::string &expires = "");
  void signOfflineTargets(const std::string &offline_targets_name);
  void exportLockBox(const boost::filesystem::path &lockbox_path, const std::vector<std::string> &offline_targets_names,
                     const std::string &snapshot_expires = "");

  static constexpr const char *dir{"repo/director"};
};

#endif  // DIRECTOR_REPO_H_
