#include <gtest/gtest.h>

#include <boost/property_tree/ptree.hpp>

#include "libaktualizr/config.h"
#include "logging/logging.h"

TEST(PackageManagerConfig, WriteToStream) {
  PackageConfig config;
  config.os = "amiga";
  config.fake_need_reboot = true;
  config.extra["foo"] = "bar";
  std::stringstream out;
  config.writeToStream(out);
  std::string cfg = out.str();

  ASSERT_NE(std::string::npos, cfg.find("os = \"amiga\""));
  ASSERT_NE(std::string::npos, cfg.find("fake_need_reboot = 1"));
  ASSERT_NE(std::string::npos, cfg.find("foo = \"bar\""));
}

TEST(PackageManagerConfig, OstreePullRetry) {
  PackageConfig config;
  EXPECT_TRUE(config.ostree_pull_retry);

  boost::property_tree::ptree pt;
  pt.put("ostree_pull_retry", "false");
  pt.put("ostree_pull_retries", "3");
  pt.put("ostree_pull_retry_delay", "2");
  pt.put("ostree_pull_retry_max_delay", "60");
  config.updateFromPropertyTree(pt);
  EXPECT_FALSE(config.ostree_pull_retry);
  EXPECT_EQ(config.ostree_pull_retries, 3U);
  EXPECT_EQ(config.ostree_pull_retry_delay, 2U);
  EXPECT_EQ(config.ostree_pull_retry_max_delay, 60U);
  EXPECT_TRUE(config.extra.empty());

  std::stringstream out;
  config.writeToStream(out);
  EXPECT_NE(std::string::npos, out.str().find("ostree_pull_retry = 0"));
  EXPECT_NE(std::string::npos, out.str().find("ostree_pull_retries = 3"));
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
