**Online Update Installation Logs**

*Last Updated 2026-04-19 by Phil Wise.*

# Introduction

During an online update the device is connected to the internet, so some update information is already
communicated back to the server automatically:

- **Report Events** are sent via the `/events` endpoint by the `ReportQueue` as they occur. These correspond to the
  `reports` table in the offline logs design.
- **The manifest**, which includes the installation results for each ECU, is PUT to the `/manifest` endpoint after
  the update completes. This corresponds to the `installs` table in the offline logs design.

The missing piece is the **systemd journal logs** captured in the `logs` table during offline updates. These provide
detailed debugging information useful for triaging failed or unexpected updates, and are not currently sent to the
server.

This document describes how to send these journal logs to the server after online updates.

# Operation

1. At the start of the download, a journal cursor is captured marking the current position in the journal.
2. During the installation operation, journal entries starting from the cursor to the present are read and POSTed to the server.

If the update requires a reboot, then on the reboot:

3. After reboot, a new journal cursor is captured at startup.
4. During  `finalizeAfterReboot()` journal entries from the post-reboot cursor to now are POSTed to the server.

Torizon does not persist the systemd journal across reboots (to avoid wearing out flash), so capturing and
transmitting the pre-reboot logs before rebooting is important. If the pre-reboot send fails (see Error Handling
below) those log entries are lost, which is acceptable.

# Server Endpoint

Journal logs are sent by POSTing a JSON document to `<server>/install_logs`:

```
POST /install_logs
Content-Type: application/json
```

Request body:

```json
{
    "correlationId": "urn:here-ota:campaign:12345678-1234-1234-1234-123456789abc",
    "startCursor": "s=99138c7c30cc4fa18627...",
    "logs": [
        {
            "timestamp": "2025-11-09T10:00:00.123456Z",
            "service": "aktualizr-torizon.service",
            "message": "Checking for updates..."
        },
        {
            "timestamp": "2025-11-09T10:00:01.456789Z",
            "service": "docker-compose.service",
            "message": "Pulling image sha256:abc123..."
        }
    ]
}
```

Submitting multiple logs at once amortises the HTTP protocol overhead (both in terms of device bandwidth and CPU overhead on the server).
Aktualizr will stream logs during the installation, buffering log messages for up to 5 seconds or 1000 messages, which ever comes first.

The `startCursor` field is used to de-dupe messages.
The server should silently discard POSTs if it has seen the `startCursor` before.
These appear to be globally unique ([ref](https://github.com/systemd/systemd/blob/main/src/libsystemd/sd-journal/sd-journal.c#L1252))
and allows Aktualizr to retry POSTS that fail.

Aktualizr will stream up any pending logs before going down for a reboot.
This will have a safety timeout of around 20s, so that bad networking will not stop an update.

The `correlationId` ties the log batch to the specific update campaign, consistent with how it is used in report
events sent to `/events`. The device identity is determined from the TLS client certificate, consistent with how
the `/manifest` and `/events` endpoints work.

A `200 OK` or `204 No Content` response indicates success. As with the `/events` endpoint, a `404` response means
the server does not support this feature; the client should discard the logs and not retry.

The 'startCursor' is an opaque string that makes the API call idempotent. 

# Error Handling

Sending logs is a best-effort operation. If the POST fails for any reason then Aktualizr logs the
failure and continues. Log entries that could not be sent are retried once but not persisted; this is acceptable because
the corresponding report events and manifest are already safely queued for delivery via their own retry mechanisms.

The alternative—failing the update because logs could not be sent—is not acceptable.

# Data Sources

Journal entries are read using the same `JournalCopier` component introduced for offline logs, which uses the
[sd-journal](https://www.freedesktop.org/software/systemd/man/latest/sd-journal.html) library.

The correlation ID is already available as part of the normal update flow and is present in the report events sent
to the `/events` endpoint.

# Configuration

The set of services to capture logs from is shared with the offline logs configuration option:

    [logger]
    capture_services = "aktualizr aktualizr-torizon docker-compose greenboot-status"

This controls log capture for both online and offline updates. The rationale for a shared option is that the set of
services worth capturing is a property of the device type rather than the update transport.

The online log feature can be disabled independently:

    [logger]
    online_logs_enabled = false

The default is `true` (enabled).

The uploader should auto-disable (for one update) if the server returns 404.

# Build Configuration

This feature depends on the `JournalCopier` component, which requires libsystemd. It therefore shares the
`BUILD_OFFLINE_UPDATES` guard that already makes libsystemd a required dependency. If `BUILD_OFFLINE_UPDATES`
is not set the feature is compiled out and no log capture or transmission occurs.

Make libsystemd a hard requirement?

(think about -native builds?

