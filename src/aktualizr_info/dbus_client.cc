#include "dbus_client.h"

#include "logging/logging.h"

#ifdef BUILD_DBUS

#include <systemd/sd-bus.h>
#include "primary/dbus.h"

int aktualizr_dbus_client_shoulder_tap() {
  sd_bus* bus = nullptr;
  int err = sd_bus_default_system(&bus);
  if (err < 0) {
    LOG_ERROR << "Failed to open system D-Bus err:" << err;
    return 1;
  }

  sd_bus_error ret_error = SD_BUS_ERROR_NULL;
  sd_bus_message* reply = nullptr;
  err = sd_bus_call_method(bus, Dbus::WellKnown, Dbus::Path, Dbus::Interface, "ShoulderTap", &ret_error, &reply, "");
  if (err < 0) {
    LOG_ERROR << "D-Bus call failed:" << err;

    sd_bus_unref(bus);
    return 1;
  }

  err = sd_bus_message_read(reply, "");
  LOG_DEBUG << "Shoulder tap result was:" << err;
  sd_bus_message_unref(reply);

  sd_bus_unref(bus);
  return 0;
}

#else /* BUILD_DBUS */

// Dummy implementation in the case where we are compiled without D-Bus support
int aktualizr_dbus_client_shoulder_tap() {
  LOG_ERROR << "Shoulder tap is not available because aktualizr is compiled without D-Bus support";
  return 1;
}

#endif /* BUILD_DBUS */
