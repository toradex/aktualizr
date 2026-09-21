#include <boost/filesystem.hpp>

#include "compose_manager.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"

static const char *const compose_cmd_prefix = "/usr/bin/docker-compose --file ";
static const char *const docker_cmd_prefix = "/usr/bin/docker ";

// In the future we may want to override the commands for testing.
ComposeManager::ComposeManager() : compose_cmd_{compose_cmd_prefix}, docker_cmd_{docker_cmd_prefix} {}

bool ComposeManager::pull(const boost::filesystem::path &compose_file, const api::FlowControlToken *flow_control) {
  LOG_INFO << "Running docker-compose pull";
  return CommandRunner::run(compose_cmd_ + compose_file.string() + " pull --no-parallel", flow_control);
}

bool ComposeManager::up(const boost::filesystem::path &compose_file) {
  LOG_INFO << "Running docker-compose up";
  return CommandRunner::run(compose_cmd_ + compose_file.string() + " -p torizon up --detach --remove-orphans");
}

bool ComposeManager::down(const boost::filesystem::path &compose_file) {
  LOG_INFO << "Running docker-compose down";
  return CommandRunner::run(compose_cmd_ + compose_file.string() + " -p torizon down");
}

bool ComposeManager::cleanup() {
  LOG_INFO << "Removing not used containers, networks and images";
  return CommandRunner::run(docker_cmd_ + "system prune -a --force");
}
