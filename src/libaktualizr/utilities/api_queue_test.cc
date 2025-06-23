#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include "utilities/apiqueue.h"
#include "utilities/flow_control.h"

using std::cout;
using std::future;
using std::future_status;
using namespace std::chrono_literals;

class CheckLifetime {
 public:
  CheckLifetime() { cout << "ctor\n"; }
  ~CheckLifetime() {
    valid = 999;
    cout << "dtor\n";
  }
  CheckLifetime(const CheckLifetime& other) {
    (void)other;
    cout << "copy-ctor\n";
  }
  CheckLifetime& operator=(const CheckLifetime&) = delete;
  CheckLifetime& operator=(CheckLifetime&&) = delete;
  CheckLifetime(CheckLifetime&&) = delete;

  int valid{100};
};

TEST(ApiQueue, Simple) {
  api::CommandQueue dut;
  future<int> result;
  {
    CheckLifetime checkLifetime;
    std::function<int()> task([checkLifetime] {
      cout << "Running task..." << checkLifetime.valid << "\n";
      return checkLifetime.valid;
    });
    result = dut.enqueue(std::move(task), 42);
    cout << "Leaving scope..";
  }
  EXPECT_EQ(result.wait_for(100ms), future_status::timeout);

  dut.run();
  // Include a timeout to avoid a failing test handing forever
  ASSERT_EQ(result.wait_for(10s), future_status::ready);
  EXPECT_EQ(result.get(), 100);
}

TEST(ApiQueue, RunTwice) {
  api::CommandQueue dut;
  dut.run();
  dut.run();
}

/**
 * The dtor cancels all running tasks
 */
TEST(ApiQueue, Dtor) {
  std::future<int> res;
  {
    api::CommandQueue dut;
    dut.run();
    std::promise<void> am_running;
    auto is_running = am_running.get_future();

    res = dut.enqueue<int>(
        [&am_running](const api::FlowControlToken* flow_control) mutable {
          am_running.set_value();
          cout << "Job started\n";
          for (int i = 0; i < 100; i++) {
            if (flow_control->hasAborted()) {
              cout << "Job was aborted\n";
              std::this_thread::sleep_for(100ms);
              return 2;
            }
            std::this_thread::sleep_for(100ms);
          }
          cout << "Job was not aborted after 10s\n";
          return 99;
        },
        9);
    // Wait for the job to start
    ASSERT_EQ(is_running.wait_for(1s), future_status::ready) << "Job should be running";
    // dut dtor is run here
  }

  ASSERT_EQ(res.wait_for(100ms), future_status::ready) << "Job should complete";
  EXPECT_EQ(res.get(), 2) << "Job should have been stopped with FlowControlToken";
}

TEST(ApiQueue, Pause) {
  api::CommandQueue dut;
  dut.run();

  dut.pause(true);
  auto t1 = dut.enqueue<int>([] { return 1; }, 2);
  ASSERT_EQ(t1.wait_for(100ms), future_status::timeout) << "Jobs should not run when paused";
  dut.Cancel();

  ASSERT_EQ(t1.wait_for(100ms), future_status::ready) << "Cancelled jobs should complete";
  EXPECT_EQ(t1.get(), 2) << "Job should get cancel value";

  auto t2 = dut.enqueue<int>([] { return 3; }, 4);
  ASSERT_EQ(t2.wait_for(100ms), future_status::timeout) << "Cancelling doesn't resume";
  dut.pause(false);
  ASSERT_EQ(t2.wait_for(100ms), future_status::ready) << "Queue should resume";
  EXPECT_EQ(t2.get(), 3) << "Job should return value";
}

TEST(ApiQueue, Cancel) {
  api::CommandQueue dut;
  dut.run();

  std::promise<void> am_running;
  auto is_running = am_running.get_future();

  auto r1 = dut.enqueue<int>(
      [&am_running](const api::FlowControlToken* flow_control) mutable {
        am_running.set_value();
        cout << "r1 started\n";
        for (int i = 0; i < 100; i++) {
          if (flow_control->hasAborted()) {
            cout << "r1 was aborted\n";
            std::this_thread::sleep_for(100ms);
            return 2;
          }
          std::this_thread::sleep_for(100ms);
        }
        cout << "r1 was not aborted after 10s\n";
        return 99;
      },
      9);

  auto r2 = dut.enqueue<int>([] { return 20; }, 2);
  ASSERT_EQ(is_running.wait_for(1s), future_status::ready) << "r1 should be running";
  ASSERT_EQ(r1.wait_for(0ms), future_status::timeout) << "r1 should not have finished";

  // Perform the cancellation
  auto cancel_future = dut.Cancel();
  auto r3 = dut.enqueue<int>([] { return 30; }, 3);
  EXPECT_EQ(r3.wait_for(0s), future_status::timeout) << "r3 should not have run yet";

  ASSERT_TRUE(cancel_future.valid()) << "Cancel should return a valid future";
  ASSERT_EQ(cancel_future.wait_for(1s), future_status::ready) << "Cancel should complete";
  ASSERT_EQ(r1.wait_for(1s), future_status::ready) << "r1 should finish";
  EXPECT_EQ(r1.get(), 2) << "r1 should have aborted through flow_control_token";
  ASSERT_EQ(r2.wait_for(1s), future_status::ready) << "r2 should finish";
  EXPECT_EQ(r2.get(), 2) << "r2 should have got default result";

  ASSERT_EQ(r3.wait_for(1s), future_status::ready) << "r3 should complete";
  EXPECT_EQ(r3.get(), 30) << "r3 should execute normally";
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
