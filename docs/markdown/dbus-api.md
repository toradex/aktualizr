# D-Bus API

<!-- Last Updated 2025-05-06 by Phil Wise -->

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
If this is non-empty, then it contains a list of Uptane Targets from the director in JSON format, for example:

    {
        "_type" : "Targets",
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

The user's response should be provided back to Aktualizr via a method call called `Consent` with the following parameters:

  * `granted` (boolean): If the installation should continue
  * `reason` (string): A human-readable description

For example:

    # Install whatever ConsentRequired is asking about
    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr Consent bs true "All good"

If the user declines, the update fails with a result code indicating that condition.
This will get posted up with the next put manifest as `CONSENT_REFUSED`, and fail the update in the Web UI.
The Aktualizr state machine pauses after fetching Updane metadata but before downloading the update itself.
While the system is waiting for consent we don't poll for online updates, but an offline update can cause it to cancel.
This is the same as today where a offline update can cancel a download operation.

During this process, events are send to the server using the reliable `ReportQueue` transport at 2 points:

| Event Name | Fields | Send When... |
|:--|:--|:--|
| `AwaitingConsent` | `correlationId` | **Installation is waiting for consent**<br/>This is sent in trivial cases too. |
| `ConsentOutcome` | `correlationId`,<br/>`granted`&nbsp;(boolean),<br/>`reason` (string) | **Consent is given or refused**<br/>If `granted` is true, then installation is proceeding. Cancellations via offline updates are reported as `granted`:false and `reason`:"Cancelled by offline update". Trivial cases are reported with a reason like "User has not requested consent" or "D-Bus not complied into Aktualizr". |

## Check For Updates

The `CheckForUpdates` method short-circuits the online polling timer and causes an immediate update check.
If the device is not currently in the idle state, then we do nothing.

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
