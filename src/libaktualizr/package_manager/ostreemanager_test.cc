#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "libaktualizr/config.h"
#include "package_manager/ostreemanager.h"
#include "http/httpclient.h"
#include "storage/invstorage.h"
#include "test_utils.h"
#include "utilities/utils.h"

boost::filesystem::path test_sysroot;

/* Reject bad OSTree server URIs. */
TEST(OstreeManager, PullBadUriNoCreds) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.ostree_server = "bad-url";
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  KeyManager keys(storage, config.keymanagerConfig());
  keys.loadKeys();
  Json::Value target_json_test;
  target_json_test["hashes"]["sha256"] = "some_hash";
  target_json_test["length"] = 0;
  Uptane::Target target_test("test.deb", target_json_test);
  data::InstallationResult result =
      OstreeManager::pull(config.pacman.sysroot, config.pacman.ostree_server, keys, target_test);

  EXPECT_EQ(result.result_code.num_code, data::ResultCode::Numeric::kInstallFailed);
  EXPECT_EQ(result.description, "Failed to parse uri: bad-url");
}

/* Reject bad OSTree server URIs. */
TEST(OstreeManager, PullBadUriWithCreds) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.ostree_server = "bad-url";
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  std::string ca = Utils::readFile("tests/test_data/prov/root.crt");
  std::string pkey = Utils::readFile("tests/test_data/prov/pkey.pem");
  std::string cert = Utils::readFile("tests/test_data/prov/client.pem");
  storage->storeTlsCa(ca);
  storage->storeTlsPkey(pkey);
  storage->storeTlsCert(cert);
  KeyManager keys(storage, config.keymanagerConfig());
  keys.loadKeys();
  Json::Value target_json_test;
  target_json_test["hashes"]["sha256"] = "some_hash";
  target_json_test["length"] = 0;
  Uptane::Target target_test("test.deb", target_json_test);
  data::InstallationResult result =
      OstreeManager::pull(config.pacman.sysroot, config.pacman.ostree_server, keys, target_test);

  EXPECT_EQ(result.result_code.num_code, data::ResultCode::Numeric::kInstallFailed);
  EXPECT_EQ(result.description, "Failed to parse uri: bad-url");
}

TEST(OstreeManager, PullRetryPolicyFromConfig) {
  PackageConfig config;
  OstreePullRetryPolicy policy = OstreePullRetryPolicy::fromConfig(config);
  EXPECT_EQ(policy.retries, 5U);
  EXPECT_EQ(policy.initial_delay, std::chrono::seconds(5));
  EXPECT_EQ(policy.max_delay, std::chrono::seconds(300));

  config.ostree_pull_retries = 3;
  config.ostree_pull_retry_delay = 2;
  config.ostree_pull_retry_max_delay = 60;
  policy = OstreePullRetryPolicy::fromConfig(config);
  EXPECT_EQ(policy.retries, 3U);
  EXPECT_EQ(policy.initial_delay, std::chrono::seconds(2));
  EXPECT_EQ(policy.max_delay, std::chrono::seconds(60));

  config.ostree_pull_retry = false;
  policy = OstreePullRetryPolicy::fromConfig(config);
  EXPECT_EQ(policy.retries, 0U);
}

TEST(OstreeManager, PullRetryBackoff) {
  OstreePullRetryPolicy policy;
  policy.initial_delay = std::chrono::seconds(5);
  policy.max_delay = std::chrono::seconds(30);
  EXPECT_EQ(policy.delayBefore(1), std::chrono::seconds(5));
  EXPECT_EQ(policy.delayBefore(2), std::chrono::seconds(10));
  EXPECT_EQ(policy.delayBefore(3), std::chrono::seconds(20));
  EXPECT_EQ(policy.delayBefore(4), std::chrono::seconds(30));
  EXPECT_EQ(policy.delayBefore(1000), std::chrono::seconds(30));
}

TEST(OstreeManager, PullRetryJitter) {
  std::mt19937 rng{42};
  bool varies = false;
  const auto first = OstreePullRetryPolicy::jitter(std::chrono::seconds(10), rng);
  for (int i = 0; i < 1000; ++i) {
    const auto delay = OstreePullRetryPolicy::jitter(std::chrono::seconds(10), rng);
    EXPECT_GE(delay, std::chrono::milliseconds(5000));
    EXPECT_LE(delay, std::chrono::milliseconds(10000));
    varies = varies || delay != first;
  }
  EXPECT_TRUE(varies);
  EXPECT_EQ(OstreePullRetryPolicy::jitter(std::chrono::seconds(0), rng), std::chrono::milliseconds(0));
}

TEST(OstreeManager, PullRetryableErrors) {
  auto retryable = [](GQuark domain, gint code) {
    GError *error = g_error_new_literal(domain, code, "test");
    bool res = OstreePullRetryPolicy::isRetryable(error);
    g_error_free(error);
    return res;
  };
  EXPECT_FALSE(OstreePullRetryPolicy::isRetryable(nullptr));
  EXPECT_TRUE(retryable(G_IO_ERROR, G_IO_ERROR_TIMED_OUT));
  EXPECT_TRUE(retryable(G_IO_ERROR, G_IO_ERROR_BUSY));
  EXPECT_TRUE(retryable(G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE));
  EXPECT_TRUE(retryable(G_RESOLVER_ERROR, G_RESOLVER_ERROR_TEMPORARY_FAILURE));
  EXPECT_FALSE(retryable(G_IO_ERROR, G_IO_ERROR_CANCELLED));
  EXPECT_FALSE(retryable(G_IO_ERROR, G_IO_ERROR_NOT_FOUND));
  EXPECT_FALSE(retryable(G_IO_ERROR, G_IO_ERROR_FAILED));
}

/* An HTTP 502 is retried by libostree and then again by the backoff loop. */
TEST(OstreeManager, PullRetriesOn502) {
  TemporaryDirectory temp_dir;
  Config config;
  config.storage.path = temp_dir.Path();
  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  KeyManager keys(storage, config.keymanagerConfig());
  keys.loadKeys();

  const std::string port = TestUtils::getFreePort();
  const std::string server = "http://127.0.0.1:" + port;
  boost::process::child server_process("tests/http_502_server.py", port);
  TestUtils::waitForServer(server + "/");

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "some_hash";
  target_json["length"] = 0;
  Uptane::Target target("test-ostree", target_json);
  auto count = [&server]() {
    HttpClient http;
    return std::stoi(http.get(server + "/count", HttpInterface::kNoLimit).body);
  };

  const int before = count();
  data::InstallationResult result = OstreeManager::pull(test_sysroot, server, keys, target);
  EXPECT_FALSE(result.isSuccess());
  const int single = count() - before;
  EXPECT_GT(single, 1);  // libostree's own retries

  OstreePullRetryPolicy policy;
  policy.retries = 2;
  result = OstreeManager::pull(test_sysroot, server, keys, target, nullptr, nullptr, nullptr, boost::none, policy);
  EXPECT_FALSE(result.isSuccess());
  EXPECT_NE(result.description.find("502"), std::string::npos);
  EXPECT_EQ(count() - before - single, 3 * single);
}

/* Reject bad OSTree server URIs. */
TEST(OstreeManager, InstallBadUri) {
  Json::Value target_json;
  target_json["hashes"]["sha256"] = "hash";
  target_json["length"] = 0;
  Uptane::Target target("branch-name-hash", target_json);
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  OstreeManager ostree(config.pacman, config.bootloader, storage, nullptr);
  data::InstallationResult result = ostree.install(target);
  EXPECT_EQ(result.result_code.num_code, data::ResultCode::Numeric::kInstallFailed);
  EXPECT_EQ(result.description, "Refspec 'hash' not found");
}

/* Abort if the OSTree sysroot is invalid. */
TEST(OstreeManager, BadSysroot) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = "sysroot-that-is-missing";
  config.storage.path = temp_dir.Path();
  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  EXPECT_THROW(OstreeManager ostree(config.pacman, config.bootloader, storage, nullptr), std::runtime_error);
}

/* Parse a provided list of installed packages. */
TEST(OstreeManager, ParseInstalledPackages) {
  TemporaryDirectory temp_dir;
  boost::filesystem::path packages_file = temp_dir / "package.manifest";

  std::string content;
  {
    content += "vim 1.0\n";
    content += "emacs 2.0\n";
    content += "bash 1.1\n";
  }
  Utils::writeFile(packages_file, content);

  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.pacman.packages_file = packages_file;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  OstreeManager ostree(config.pacman, config.bootloader, storage, nullptr);
  Json::Value packages = ostree.getInstalledPackages();
  EXPECT_EQ(packages[0]["name"].asString(), "vim");
  EXPECT_EQ(packages[0]["version"].asString(), "1.0");
  EXPECT_EQ(packages[1]["name"].asString(), "emacs");
  EXPECT_EQ(packages[1]["version"].asString(), "2.0");
  EXPECT_EQ(packages[2]["name"].asString(), "bash");
  EXPECT_EQ(packages[2]["version"].asString(), "1.1");
}

/**
 * Check that OstreeManager::getCurrent() returns a sensible result, even if it
 * can't match the currently booted OSTree commit against a known target.
 * See: https://github.com/uptane/aktualizr/issues/1
 */
TEST(OstreeManager, GetCurrentTarget) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();
  config.pacman.booted = BootedType::kStaged;
  auto storage = INvStorage::newStorage(config.storage);
  OstreeManager dut(config.pacman, config.bootloader, storage, nullptr);
  auto current_target = dut.getCurrent();

  EXPECT_TRUE(current_target.IsValid()) << "shouldn't be Uptane::Target::Unknown()";

  EXPECT_TRUE(current_target.IsOstree()) << "should be an OSTree target";
  // This is a slightly circular test, but OstreeManager::getCurrentHash()
  // fetches the hash straight from libostree.
  EXPECT_EQ(dut.getCurrentHash(), current_target.sha256Hash()) << "hash should match";
}

/* Communicate with a remote OSTree server without credentials. */
TEST(OstreeManager, AddRemoteNoCreds) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  KeyManager keys(storage, config.keymanagerConfig());
  keys.loadKeys();

  OstreeRepo *repo = nullptr;
  GError *error = nullptr;
  std::shared_ptr<OstreeSysroot> sysroot = OstreeManager::LoadSysroot(config.pacman.sysroot);
  EXPECT_TRUE(ostree_sysroot_get_repo(sysroot.get(), &repo, nullptr, &error));
  EXPECT_TRUE(OstreeManager::addRemote(repo, config.pacman.ostree_server, &keys));

  g_autofree char *url = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "url", nullptr, &url, &error));
  EXPECT_EQ(url, config.pacman.ostree_server);

  gboolean out_gpg_verify;
  EXPECT_TRUE(ostree_repo_get_remote_boolean_option(repo, remote, "gpg-verify", FALSE, &out_gpg_verify, &error));

  g_autofree char *ostree_cert = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-client-cert-path", nullptr, &ostree_cert, &error));
  EXPECT_EQ(ostree_cert, nullptr);

  g_autofree char *ostree_key = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-client-key-path", nullptr, &ostree_key, &error));
  EXPECT_EQ(ostree_key, nullptr);

  g_autofree char *ostree_ca = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-ca-path", nullptr, &ostree_ca, &error));
  EXPECT_EQ(ostree_ca, nullptr);

  g_object_unref(repo);
}

/* Communicate with a remote OSTree server without credentials (second form). */
TEST(OstreeManager, AddRemoteNoCreds2ndForm) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();

  OstreeRepo *repo = nullptr;
  GError *error = nullptr;
  std::shared_ptr<OstreeSysroot> sysroot = OstreeManager::LoadSysroot(config.pacman.sysroot);
  EXPECT_TRUE(ostree_sysroot_get_repo(sysroot.get(), &repo, nullptr, &error));
  EXPECT_TRUE(OstreeManager::addRemote(repo, config.pacman.ostree_server));

  g_autofree char *url = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "url", nullptr, &url, &error));
  EXPECT_EQ(url, config.pacman.ostree_server);

  gboolean out_gpg_verify;
  EXPECT_TRUE(ostree_repo_get_remote_boolean_option(repo, remote, "gpg-verify", FALSE, &out_gpg_verify, &error));

  g_autofree char *ostree_cert = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-client-cert-path", nullptr, &ostree_cert, &error));
  EXPECT_EQ(ostree_cert, nullptr);

  g_autofree char *ostree_key = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-client-key-path", nullptr, &ostree_key, &error));
  EXPECT_EQ(ostree_key, nullptr);

  g_autofree char *ostree_ca = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-ca-path", nullptr, &ostree_ca, &error));
  EXPECT_EQ(ostree_ca, nullptr);

  g_object_unref(repo);
}

/* Communicate with a remote OSTree server with credentials. */
TEST(OstreeManager, AddRemoteWithCreds) {
  TemporaryDirectory temp_dir;
  Config config;
  config.pacman.type = PACKAGE_MANAGER_OSTREE;
  config.pacman.sysroot = test_sysroot;
  config.storage.path = temp_dir.Path();

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);
  std::string ca = Utils::readFile("tests/test_data/prov/root.crt");
  std::string pkey = Utils::readFile("tests/test_data/prov/pkey.pem");
  std::string cert = Utils::readFile("tests/test_data/prov/client.pem");
  storage->storeTlsCa(ca);
  storage->storeTlsPkey(pkey);
  storage->storeTlsCert(cert);
  KeyManager keys(storage, config.keymanagerConfig());
  keys.loadKeys();

  OstreeRepo *repo = nullptr;
  GError *error = nullptr;
  std::shared_ptr<OstreeSysroot> sysroot = OstreeManager::LoadSysroot(config.pacman.sysroot);
  EXPECT_TRUE(ostree_sysroot_get_repo(sysroot.get(), &repo, nullptr, &error));
  EXPECT_TRUE(OstreeManager::addRemote(repo, config.pacman.ostree_server, &keys));

  g_autofree char *url = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "url", nullptr, &url, &error));
  EXPECT_EQ(url, config.pacman.ostree_server);

  gboolean out_gpg_verify;
  EXPECT_TRUE(ostree_repo_get_remote_boolean_option(repo, remote, "gpg-verify", FALSE, &out_gpg_verify, &error));

  g_autofree char *ostree_cert = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-client-cert-path", nullptr, &ostree_cert, &error));
  EXPECT_EQ(ostree_cert, keys.getCertFile());

  g_autofree char *ostree_key = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-client-key-path", nullptr, &ostree_key, &error));
  EXPECT_EQ(ostree_key, keys.getPkeyFile());

  g_autofree char *ostree_ca = nullptr;
  EXPECT_TRUE(ostree_repo_get_remote_option(repo, remote, "tls-ca-path", nullptr, &ostree_ca, &error));
  EXPECT_EQ(ostree_ca, keys.getCaFile());

  g_object_unref(repo);
}

#ifndef __NO_MAIN__
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  if (argc != 2) {
    std::cerr << "Error: " << argv[0] << " requires the path to an OSTree sysroot as an input argument.\n";
    return EXIT_FAILURE;
  }

  TemporaryDirectory temp_sysroot;
  test_sysroot = temp_sysroot / "sysroot";
  // uses cp, as boost doesn't like to copy bad symlinks
  int r = system((std::string("cp -r ") + argv[1] + std::string(" ") + test_sysroot.string()).c_str());
  if (r != 0) {
    return -1;
  }

  return RUN_ALL_TESTS();
}
#endif
