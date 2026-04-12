**Offline Update Installation Results**

*Last Updated 2025-11-09 by Phil Wise.*

# Introduction

A technician installing an offline update does not have a way to get information about the progress, success, or failure
of an update. If the device is connected to the internet, then the manifest with the new version will be sent to the
server.

However, if the device were reliably connected to the internet, then there would be little need for offline updates.
Instead, we will use the installation media as a back channel for the installation results. This will include the manifest
(which in turn contains the installation results) as well as the relevant systemd journal logs and Report Events. One
piece of installation media can collect the results from installations on multiple devices.

# Operation

During an offline update, Aktualizr will write debugging information to a SQLite database in the root directory of the
offline update media. The database has the following tables:

### Table: installs

Each row in this table corresponds to an attempted offline install.


| **Column name** | **SQL Type** | **Definition** | **Example** |
|-----------------|--------------|----------------|-------------|
| **id** | PRIMARY KEY | Incrementing serial number, used as a FK by the following tables | |
| device_id | TEXT | The claimed device id | a7f40be0-1d3b-4761-b8a8-aaa8dbba3117 |
| name | TEXT | The name of the offline update (from the offline update metadata) |
| version | INTEGER | The version of the snapshot (from the offline update metadata) |
| report_counter | INTEGER | The Report Counter of the primary, null if the install is in progress. | |
| manifest | TEXT |  The signed manifest, as would be PUT to the /manifest endpoint. Includes installation reports. Null if the install didn't complete. | |

The `id` column is the primary key. The combination of `device_id` and `report_counter` also uniquely identifies rows in this table, *but only if report_counter is not null*.

Rows in this table are created at the start of installation and updated at the end:

1. At the start of an offline update installation, a row is created with a `device_id` and the `name` and `version` of the update being applied. The `report_counter` and `manifest` columns are null.
2. The `id` of this row is used as a foreign key for log entries and reports.
3. After a reboot, the device looks for the last entry in this table with a matching `device_id` and non-null `manifest`. If it exists, then more log lines and reports can be attached to this in-progress update.
4. When the update completes, the `report_counter` and `manifest` are updated.

If the installation aborts partway through, there may be dangling rows in this table with null manifests and report counters.
This is fine, and the next installation attempt will create a fresh row for the new install.

### Table: logs

This table contains a copy of the systemd journal for the relevant services over the period of the update.

| **Column name** | **SQL Type** | **Definition** | **Journald field** | **Example** |
|-----------------|--------------|----------------|--------------------|-------------|
| **id** | INTEGER PRIMARY KEY | Serial number to order entries | N/A
| install_id | INT FK installs(id) | The installation that this log message is part of | N/A
| timestamp | INTEGER (UTC microseconds since the epoch) | The local device time of the log entry | \_SOURCE_REALTIME_TIMESTAMP | |
| service | TEXT | The systemd unit that created this log entry | \_SYSTEMD_UNIT | aktualizr-torizon.service |
| message | TEXT | The log message | MESSAGE | |

Logs are copied from the systemd journal into this table at two possible points during the installation:

1. When an offline update has completed
2. During initialization after reboot, if the update required finalization

Torizon does not persist the systemd journal through reboots to avoid wearing out flash, which means that logs need to be copied out before the reboot.

### Table: reports

These are Report Events that would normally be sent via the `/events` endpoint.
Only events that have not already been successfully sent will be stored, but writing them to offline media will not stop them being sent via the online route in the future.


| **Column name** | **SQL Type** | **Definition** | **Equivalent JSON field in reports posted to the /events endpoint** |
|-----------------|--------------|----------------|---------------------------------------------------------------------|
| **install_id** | INT FK installs(id) | | N/A
| **report_id** | TEXT | UUID given to the report when it was created | id |
| **timestamp** | INTEGER (UTC microseconds since the epoch) | The time of the report | deviceTime |
| type | TEXT | The type of event, e.g. EcuDownloadStarted | eventType.id |
| version | INTEGER | Event version number, usually 0 | eventType.version |
| event | TEXT | The JSON serialization of the report | event |

# Error Handling

If the `update-logs.db` file cannot be opened for writing, for example if the volume is mounted read-only or full, then Aktualizr will log the failure and continue.
The alternative---failing the offline installation---has a high risk of bricking the device.

It would be possible to define a rotation policy to avoid filling the device, however:

-   It makes lost data less obvious.

-   Most removable storage is huge relative to the size of the information this will record. A USB flash drive will
    generally be tens of GB, whereas this data will be predominantly journal logs totaling less than 1MB in most cases.

-   A log rotation policy would need to be configured.

-   The cost of filling the device is that these logs will stop being captured. The device is unlikely to be used for
    anything other than installing the update.

Therefore, it is simpler and more robust to let SQLite handle any errors that occur.

# Data sources

The manifest is already available as part of normal operation.
The service logs will come from systemd-journal via the [sd-journal](https://www.freedesktop.org/software/systemd/man/latest/sd-journal.html) library.
The reports will come from the report_events table in `/var/sota/sql.db`.

# Runtime Configuration

Configuration requirements:

-   It must be possible to debug a failing offline update without changing Aktualizr configuration, since that would
    require an update to apply.

-   The defaults should be correct for most users.

-   Minimize additional security risks.

The location of the logs database will be at a default to `update-logs.db`, in the root of the offline update media as
a peer to the `metadata` and `images` directories.
This location can be overiden with the `offline_logs_file` configuration option:

    [logger]
    offline_logs_file = "path/mylogfile.db"

If this path is a relative path, then it is relative to the root of the offline update media.
If it starts with `/`, then it is considered an absolute path.

The set of services to capture logs from will be configurable via the normal Aktualizr configuration files.
The default will be `aktualizr`, `aktualizr-torizon`, `docker-compose` and `greenboot-status`.
This will cover the common case where the user is running Torizon and using Docker for their applications.
For torizon-minimal, where the user builds a service directly in Yocto, they can place a configuration fragment in
`/usr/lib/sota/conf.d/90-myapp.toml` (for example):

    [logger]
    capture_services = "aktualizr aktualizr-torizon docker-compose myservice"

(Service names may be space separated since systemd unit names cannot contain spaces
[ref](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html#Description)).

Finally, the entire feature can be disabled by setting `offline_logs_enabled` to false:

    [logger]
    offline_logs_enabled = false

# Build Configuration

The feature will be an integral part of Offline Updates and will be enabled via `BUILD_OFFLINE_UPDATES`.
Reading the systemd journal requires libsystemd, which is currently only required for D-Bus.
It will now also be a required dependency for `BUILD_OFFLINE_UPDATES`.
In practice, the Torizon platform uses systemd, so this will be available.

# Log Viewer

The SQLite database schema will be stable and documented to allow tools to be written that inspect it, which is the
recommended way to programmatically interact with this data.

For interactive usage, a Python script will display the logs.
In the future, it would be possible to provide a tool to upload the Report Events and manifests to the Torizon platform so they are visible on a single dashboard.
This will require additional endpoints to allow an administrator to upload manifests (and report events) for any device they have access to. Currently, the device ID associated with an upload is determined from the credentials (x509 client cert) of the device that authenticated.

# Implementation

## Aktualizr state

The offline logs will be written back to the location where the offline update was fetched from.
This location is known to Aktualizr during the initial installation, but is not available after the reboot.
While Aktualizr does poll for offline updates at a fixed location, it can also be instructed to install updates from an arbitrary location over D-Bus.
In the future, we expect to replace this polling behavior with a route that uses Linux Hotplug to trigger Aktualizr over D-Bus, passing the mount location dynamically.
Therefore this case is important to support.

In the most general case, the offline update media will appear at a completely different location after reboot.
This is very hard to support.
Instead, the following procedure is used:

1. When an offline update is triggered, the location of the offline update media is stored in Aktualizr's persistent storage.
2. After reboot, Aktualizr looks for `update-logs.db` in this location.
3. If `update-logs.db` is present, then it will be used, but it will not be created.

This has the following advantages:
* It works if the removable storage is mounted at a fixed location.
* It also works if the location is stable over reboots.
* If it does end up somewhere else (or the user removed the update media during the reboot), then it will not write logs to the old mount point.

## Manifests

Currently, `SotaUptaneClient` does not return the manifest that it sends to the server.
`SotaUptaneClient::putManifest` will be extended to return the manifest back to `Aktualizr.cc`.
