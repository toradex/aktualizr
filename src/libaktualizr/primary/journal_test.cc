#include <gtest/gtest.h>

#include "logging/logging.h"
#include "primary/systemd_journal.h"

#ifdef BUILD_OFFLINE_UPDATES

TEST(SystemdJournal, CanReadEntries) {
  SystemdJournal journal;
  if (!journal) {
    GTEST_SKIP() << "No systemd journal available in this environment";
  }
  if (!journal.MoveTail()) {
    GTEST_SKIP() << "Journal is empty; cannot obtain a tail cursor";
  }
  ASSERT_GE(journal.Next(), 0);

  auto cursor = journal.GetCurrentCursor();
  ASSERT_NE("", cursor);
}

// Clone() should produce an independent, freshly-opened handle.
TEST(SystemdJournal, CloneProducesIndependentHandle) {
  SystemdJournal journal;
  if (!journal) {
    GTEST_SKIP() << "No systemd journal available in this environment";
  }
  auto clone = journal.Clone();
  ASSERT_NE(clone, nullptr);
  EXPECT_TRUE(static_cast<bool>(*clone));
}

#endif  // BUILD_OFFLINE_UPDATES

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  return RUN_ALL_TESTS();
}
#endif
