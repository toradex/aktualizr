# D-Bus API

Last Updated 2025-05-06 by Phil Wise.

## Introduction

\aktualizr provides an API over D-Bus to allow users to:

* Approve updates (required by the EU Cyber Resilience Act)
* Trigger an immediate update check (useful if you have an out-of-band wake-up source already)
* Cancel an in-progress update

Note this is the extent of the API: \aktualizr is not intended to be used as a library.
If you want to use \aktualizr for a use case that we don't support today, implement the missing features directly in the upstream codebase and enable them via configuration.
We're a friendly bunch!

The EU Cyber Resilience Act mandates user consent when installing updates.
\aktualizr implements this by pausing the update state machine and waiting for confirmation over D-Bus.

The D-Bus API is enabled at build time via the `BUILD_DBUS` flag.
It uses the systemd sdbus library.

## Configuration API

By default devices install updates automatically.
A user who wants to individually approve each update that is installed will go into the settings section of the device and clear the 'Automatically install updates’ property.
The device manufacturer will implement this UI using the API that \aktualizr provides, which is a settable ‘InstallUpdatesAutomatically’ D-Bus Property that takes the following values:

<table>
<caption>InstallUpdatesAutomatically values</caption>
<tr>
  <th>Value over D-Bus</th>
  <th>ConsentRequirement enum</th>
  <th>Meaning</th>
</tr>
<tr>
  <td>0</td>
  <td>InstallUpdatesAutomatically::kProceed</td>
  <td>(default) Updates proceed automatically</td>
</tr>
<tr>
 <td>1</td>
 <td>InstallUpdatesAutomatically::kAsk</td>
 <td>Updates require consent to continue.</td>
</tr>
</table>

Customers who want finer-grained policies can implement these by automatically responding to \aktualizr’s consent requests without prompting the user.
In the future we can add more options like ‘apply pure security updates automatically’, which would remain backwards compatible with UIs that only know about the 2 initial options.

This property is stored in \aktualizr’s non-volatile sqlite database.
There won’t be a way to change the default in the initial version.
The UI should not try and store its own copy of this state, and instead query \aktualizr as needed.
If \aktualizr starts up and finds that the InstallUpdatesAutomatically property is set in the sqlite database but D-Bus isn’t compiled in, then it will ignore the property and continue with a warning in order to avoid getting stuck in an un-updatable state.

Users should directly consume this D-Bus API using whatever bindings their language provides.
For manual testing, `busctl` can be used:

    # Require user consent for future updates
    busctl set-property org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr InstallUpdatesAutomatically i 1


## %Consent API

This is the API that will drive a UI to display "An update is available, do you want to install it?" and handle the user's response.

\aktualizr exposes a read-only property called ‘ConsentRequired’.
If this is non-empty, then it contains a list of Uptane targets from the director in JSON format.
Property change notifications are provided, and when this is non-empty, the UI should ask the user if they want to install an update, and may use the contents to provide extra context.
Online and offline updates can be distinguished by the ‘_type’ field.

In the \aktualizr state machine this pause occurs between fetching metadata and downloading an update.

For manual testing, this can be read with:

    busctl get-property org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr ConsentRequired

The user’s response should be provided back to \aktualizr via a method call called ‘Consent’ with the following parameters:

  * boolean granted – If the installation should continue
  * string reason – A human-readable description

For example:

    # Install whatever ConsentRequired is asking about
    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr Consent bs true "All good"

If the user declines then we fail the update with a new uptane::ResultCode of kConsentRefused.
This will get posted up with the next put manifest as CONSENT_REFUSED, and fail the update in the Web UI.
While the system is waiting for consent we don’t poll for online updates, but an offline update can cause it to cancel.
This is the same as today where a offline update can cancel a download operation.

During this process, events are send to the server using the reliable 'ReportQueue' transport at 2 points:

<table>
<caption>Events sent from \aktualizr to the Update server</caption>
<tr>
  <th>Event Name</th>
  <th>Fields</th>
  <th>Send When...</th></tr>
<tr>
  <td>AwaitingConsent</td>
  <td>

  * correlationId

  </td>
  <td>Installation is waiting for consent. This is sent in trivial cases too.</td>
</tr>
<tr>
  <td>ConsentOutcome</td>
  <td>

  * correlationId
  * granted (boolean)
  * reason (string)

  </td>
  <td>
    When consent is given or refused.
    If ‘granted’ is true, then installation is proceeding.
    Cancellations via offline updates are reported as granted:false reason:”Cancelled by offline update” 
    Trivial cases are reported with a reason like “User has not requested consent” or “D-Bus not complied into \aktualizr”
  </td>
</tr>
</table>

## Check For Updates

CheckForUpdates short-circuits the next_online_poll timer and causes an immediate update check.
If the device is not currently in the idle state, then we do nothing.

When working interactively with a device, this can be easier than running a short polling interval or waiting for the next update check:

    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr CheckForUpdates

## Cancel an Update

Cancel aborts the current update and returns \aktualizr back to an idle state.
The cancel D-Bus call is asynchronous and updates will continue until a suitable cancellation point.

    busctl call org.uptane.Aktualizr /org/uptane/aktualizr org.uptane.Aktualizr Cancel


## Security Considerations

The key security control is that the D-Bus API doesn’t provide any new rights to install software.
It is possible to indefinitely block the installation of updates, but this is an explicit right granted by the CRA.
In the future it will be possible to trigger an offline update over D-Bus, but this will require that a suitable signed update package is already present on the device somewhere, which is equivalent power to being able to plug a USB drive into the device.


## Future Features

The following are future features:

  * Perform offline updates from a specific directory
  * Disabling (locking) updates
  
## Testing Approach

It is much easier to keep the entire test in C++, with the DUT and mock consent UI running in the same process and maintaining 2 connections to the D-Bus server.
D-Bus connections have a unique bus name that is provided by the daemon, which test code can use.
Tests inside Docker (or anywhere else where there isn’t a native D-Bus daemon) will need to be run inside `dbus-run-session`.
