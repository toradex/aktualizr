# D-Bus API

<!-- Last Updated 2026-08-11 (post-consent replace can fail the new update) -->

## Introduction

Aktualizr provides an API over D-Bus to allow users to:

* Approve updates (required by the EU Cyber Resilience Act)
* Trigger an immediate update check (useful if you have an out-of-band wake-up source already)
* Cancel an in-progress update

Note this is the extent of the API: Aktualizr is not intended to be used as a library.
If you want to use Aktualizr for a use case that we don't support today, implement the missing features directly in the upstream codebase and enable them via configuration.
We're a friendly bunch!

The EU Cyber Resilience Act mandates user consent when installing updates.
Aktualizr implements this by pausing the update state machine and waiting for confirmation over D-Bus.

The D-Bus API is enabled at build time via the `BUILD_DBUS` flag.
It uses the systemd sdbus library.

## Configuration API

By default devices install updates automatically.
A user who wants to individually approve each update that is installed will go into the settings section of the device and clear the "Automatically install updates" property.
The device manufacturer will implement this UI using the API that Aktualizr provides, which is a settable `InstallUpdatesAutomatically` D-Bus Property that takes the following values:

| Value over D-Bus | Meaning |
|:---|:---|
| 0 | (default) Updates proceed automatically. |
| 1 | Updates require consent to continue. |

Customers who want finer-grained policies can implement these by automatically responding to Aktualizr's consent requests without prompting the user.
In the future we can add more options like "apply pure security updates automatically", which would remain backwards compatible with UIs that only know about the 2 initial options.

This property is stored in Aktualizr's non-volatile sqlite database and its default value is not currently configurable.
The UI should not try and store its own copy of this state, and instead query Aktualizr as needed.
If Aktualizr starts up and finds that the `InstallUpdatesAutomatically` property is set in the sqlite database but D-Bus isn't compiled in, then it will ignore the property and continue with a warning in order to avoid getting stuck in an un-updatable state.

Users should directly consume this D-Bus API using whatever bindings their language provides.
For manual testing, `busctl` can be used:

    # Require user consent for future updates
    busctl set-property org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr InstallUpdatesAutomatically i 1


## Consent API

This is the API that will drive a UI to display "An update is available, do you want to install it?" and handle the user's response.

Aktualizr exposes a read-only property with change notifications called `ConsentRequired`.
If this is non-empty, then it contains a list of Uptane Targets in JSON format (Director metadata with Image-repo custom fields merged in; Director wins on conflicts), plus a top-level `correlationId` field identifying the update offer, for example:

    {
        "_type" : "Targets",
        "correlationId" : "urn:tdx-ota:lockbox:my-update:42",
        "targets" :
        {
            "primary_firmware.txt" :
            {
                "custom" :
                {
                    "ecuIdentifiers" :
                    {
                        "CA:FE:A6:D2:84:9D" :
                        {
                            "hardwareId" : "primary_hw"
                        }
                    },
                    "foo" : "bar",
                    "targetFormat" : "BINARY",
                    "uri" : "http://customurl/primary.txt"
                },
                "hashes" :
                {
                    "sha256" : "ef7dbbe324eab86ab67a198f95b46d7fbf79d5ebf4a4c2c72fc7da9d724aac96",
                    "sha512" : "74743b8e9588842cdb6d4eb1f851b3b7fc3ef1da3caa81c8a536ce200ae7822c12b02297153dd37aeb9117852e98843d9c7a1fe78901543e200f401bfcb1f111"
                },
                "length" : 13
            }
        }
    }


For manual testing, this can be read with:

    busctl get-property org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr ConsentRequired

### `ConsentRequired` is a live property

`ConsentRequired` can change at any time, including **while it is non-empty**:
Aktualizr keeps polling for updates while it waits for consent (see below), and if a newer update supersedes the pending one, the property is replaced with the new offer (carrying a different `correlationId`).

Clients must subscribe to `org.freedesktop.DBus.Properties.PropertiesChanged` and re-read the property on every change.
The signal fires on every transition: when a request appears, when it is superseded by a new offer, when it is withdrawn or cancelled, and when it is resolved via the `Consent` method.

When the `correlationId` changes, treat it as a **new offer**:
dismiss or replace any prompt that is currently showing, and discard any in-flight user action tied to the old `correlationId`.

If the update is retracted on the server, `ConsentRequired` becomes empty (with a `PropertiesChanged` signal) and the device returns to idle.
No `ConsentOutcome` event is sent in that case, since the user never decided.

### The `Consent` method

The user's response should be provided back to Aktualizr via a method call called `Consent` with the following parameters:

  * `granted` (boolean): If the installation should continue
  * `reason` (string): A human-readable description
  * `correlationId` (string): The `correlationId` from the `ConsentRequired` JSON that this response refers to

> **Breaking change**: the `Consent` signature changed from `bs` to `bss`.
> Consent UIs written against the old two-parameter signature must be updated
> to extract `correlationId` from the `ConsentRequired` JSON and pass it back.

For example:

    # Read ConsentRequired, extract "correlationId" from the JSON, then:
    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr \
        Consent bss true "All good" "urn:tdx-ota:lockbox:my-update:42"

The `correlationId` must exactly match the pending offer:

  * On mismatch the call fails with `org.freedesktop.DBus.Error.InvalidArgs` and the consent request **remains pending**. This happens when the offer was superseded while the user was deciding; the UI should re-read `ConsentRequired` and prompt again for the new offer.
  * If no request is pending at all, the call fails with `org.freedesktop.DBus.Error.Failed` (as before).

Note that a successful `Consent` reply does **not** guarantee that the update installs:
the update can still be superseded or cancelled on the server between the reply and the commit fetch that follows it.
The device never installs something other than what the matching `correlationId` referred to.

One rare edge case to consider:
if the user consents to update A, and before the commit fetch the server cancels A and replaces it with update B, the commit fetch observes B (and marks it seen) while the device still expects A's `correlationId`.
That mismatch fails the campaign; in practice **update B is what the server ends up marking failed**, even though the user never consented to B.
This should be very rare, and the workaround is simply to retry the update from the server side.

If the user declines, the update fails with a result code indicating that condition.
This will get posted up with the next put manifest as `CONSENT_REFUSED`, and fail the update in the Web UI.
The Aktualizr state machine pauses after fetching Uptane metadata but before downloading the update itself.

### Polling while waiting for consent

While the system is waiting for consent, Aktualizr keeps polling the server on the normal polling interval using "peek" update checks (which do not mark the update as in-progress on the server).
This is what allows a newer update to supersede the pending offer, or a server-side cancellation to withdraw it.
An offline update can still cancel the pending consent request, the same as today where an offline update can cancel a download operation.

During this process, events are send to the server using the reliable `ReportQueue` transport at 2 points:

| Event Name | Fields | Send When... |
|:--|:--|:--|
| `AwaitingConsent` | `correlationId` | **Installation is waiting for consent**<br/>This is sent in trivial cases too. If the offered update changes before consent is given, another `AwaitingConsent` is sent carrying the new `correlationId`, so multiple events per cycle are expected. |
| `ConsentOutcome` | `correlationId`,<br/>`granted`&nbsp;(boolean),<br/>`reason` (string) | **Consent is given or refused**<br/>If `granted` is true, then installation is proceeding. Cancellations via offline updates are reported as `granted`:false and `reason`:"Cancelled by offline update". Trivial cases are reported with a reason like "User has not requested consent" or "D-Bus not complied into Aktualizr". No `ConsentOutcome` is sent for offers that were superseded or withdrawn before the user decided. |

## Check For Updates

The `CheckForUpdates` method short-circuits the online polling timer and causes an immediate update check.
This works in the idle state and also while the device is waiting for consent
(where it triggers an immediate peek check, unless one is already running).
In other states we do nothing.

When working interactively with a device, this can be easier than running a short polling interval or waiting for the next update check:

    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr CheckForUpdates

## Cancel an Update

The `Cancel` method aborts the current update and returns Aktualizr back to an idle state.
The cancel D-Bus call is asynchronous and updates will continue until a suitable cancellation point.

    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr Cancel

## Trigger Offline Updates

The `OfflineUpdate` method searches a specific directory for offline update metadata, then validates and installs any update that is present.
Unlike the normal offline update process, it doesn't require that the metadata appears while Aktualizr is watching.
The normal Uptane security validation does take place, so the security model is the same as someone writing the contents of that directory to a USB pen drive and plugging it into the device.

    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr OfflineUpdate s "/tmp/path/to/update"

`/tmp/path/to/update` will generally be the root of the removable storage containing a takeout image.
It should contain a directory called `metadata`.

## Security Considerations

The key security control is that the D-Bus API doesn't provide any new rights to install software.
It is possible to indefinitely block the installation of updates, but this is an explicit right granted by the CRA.
While it is possible to trigger an offline update over D-Bus, this requires that a suitable signed update package is already present on the device somewhere, which is equivalent power to being able to plug a USB drive into the device.


## Future Features

The following are future features:

  * Disabling (locking) updates

## Testing Approach

It is much easier to keep the entire test in C++, with the DUT and mock consent UI running in the same process and maintaining 2 connections to the D-Bus server.
D-Bus connections have a unique bus name that is provided by the daemon, which test code can use.
Tests inside Docker (or anywhere else where there isn't a native D-Bus daemon) will need to be run inside `dbus-run-session`.
