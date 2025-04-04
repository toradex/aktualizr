#include <gtest/gtest.h>
#include <libaktualizr/events.h>
#include <logging/logging.h>
#include <primary/dbus.h>
#include <utilities/utils.h>

#include "httpfake.h"
#include "libaktualizr/aktualizr.h"
#include "metafake.h"
#include "uptane_test_common.h"

#include <systemd/sd-bus.h>
#include <boost/filesystem.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

boost::filesystem::path uptane_repos_dir;  // NOLINT
boost::filesystem::path fake_meta_dir;     // NOLINT

class AktualizrDbus : public testing::Test {
 public:
  AktualizrDbus(AktualizrDbus&&) = delete;
  AktualizrDbus(const AktualizrDbus&) = delete;
  AktualizrDbus& operator=(const AktualizrDbus&) = delete;
  AktualizrDbus& operator=(AktualizrDbus&&) = delete;

 protected:
  AktualizrDbus() {
    int err = sd_bus_open(&dut_bus_.ptr);
    if (err < 0) {
      throw std::system_error(-err, std::system_category(), "Failed to open dut bus");
    }
    sd_bus_get_unique_name(dut_bus_.ptr, &bus_name_);

    err = sd_bus_open(&client_bus_);  // Must be 'open' not 'default' to get a
                                      // separate connection
    if (err < 0) {
      throw std::system_error(-err, std::system_category(), "Failed to open client bus");
    }
  }

  ~AktualizrDbus() override { sd_bus_unref(client_bus_); }
  SdBus dut_bus_;
  sd_bus* client_bus_{nullptr};
  const char* bus_name_{nullptr};
};

TEST_F(AktualizrDbus, ShoulderTap) {
  TemporaryDirectory temp_dir;
  auto http = std::make_shared<HttpFake>(temp_dir.Path(), "noupdates", fake_meta_dir);
  Config conf = UptaneTestCommon::makeTestConfig(temp_dir, http->tls_server);
  conf.uptane.polling_sec = 600;

  auto storage = INvStorage::newStorage(conf.storage);
  UptaneTestCommon::TestAktualizr aktualizr(conf, storage, http);

  aktualizr.SetDbusInterface(std::move(dut_bus_));

  struct CheckEvents {
    std::mutex m;
    std::condition_variable cv;
    int update_checks{0};

    void HandleEvent(const std::shared_ptr<event::BaseEvent>& event) {
      if (event->variant == "UpdateCheckComplete") {
        std::lock_guard<std::mutex> guard{m};
        update_checks++;
        cv.notify_all();
      }
    }
  };

  CheckEvents check_events;
  auto conn = aktualizr.SetSignalHandler(std::bind(&CheckEvents::HandleEvent, &check_events, std::placeholders::_1));

  aktualizr.Initialize();
  auto ak_future = aktualizr.RunForever();

  {
    std::unique_lock guard{check_events.m};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (check_events.update_checks < 1) {
      if (check_events.cv.wait_until(guard, deadline) == std::cv_status::timeout) {
        FAIL() << "Timed out waiting for update_check";
      }
    }
  }
  LOG_INFO << "First update check successful. Triggering a 2nd via D-Bus";

  // Give time to get back the the Idle state
  std::this_thread::sleep_for(std::chrono::seconds(2));

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int res =
      sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, "ShoulderTap", &ret_error, &reply, "");
  ASSERT_GE(res, 0) << "Call failed";

  {
    std::unique_lock guard{check_events.m};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (check_events.update_checks < 2) {
      if (check_events.cv.wait_until(guard, deadline) == std::cv_status::timeout) {
        aktualizr.Shutdown();
        ASSERT_TRUE(false) << "Timed out waiting for second update_check";
        return;
      }
    }
  }

  aktualizr.Shutdown();
  ak_future.wait();
}

/**
 * Validate the D-Bus interface can receive a shoulder tap
 */
TEST_F(AktualizrDbus, DbusShoulderTap) {
  Dbus dut(std::move(dut_bus_));

  std::atomic<int> taps = 0;

  dut.SetShoulderTapCallback([&] { taps++; });

  EXPECT_EQ(taps, 0);

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  int res =
      sd_bus_call_method(client_bus_, bus_name_, Dbus::Path, Dbus::Interface, "ShoulderTap", &ret_error, &reply, "");
  ASSERT_GE(res, 0) << "Call failed";

  // int64_t c;
  res = sd_bus_message_read(reply, "");
  sd_bus_message_unref(reply);
  ASSERT_EQ(res, 0);
  EXPECT_EQ(taps, 1);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (argc != 2) {
    // NOLINTNEXTLINE
    std::cerr << "Error: " << argv[0] << " requires the path to the base directory of Uptane repos.\n";
    return EXIT_FAILURE;
  }
  // NOLINTNEXTLINE
  uptane_repos_dir = argv[1];

  logger_init();
  logger_set_threshold(boost::log::trivial::trace);

  TemporaryDirectory tmp_dir;
  fake_meta_dir = tmp_dir.Path();
  CreateFakeRepoMetaData(fake_meta_dir);

  return RUN_ALL_TESTS();
}
