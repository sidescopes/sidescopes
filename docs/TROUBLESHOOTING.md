# Troubleshooting

## SideScopes cannot capture the screen on macOS

Screen capture requires permission in **System Settings → Privacy & Security →
Screen Recording**. Enable SideScopes, quit it completely, and reopen it. The
application reports a missing permission rather than presenting empty traces.

If the setting is enabled but capture remains unavailable, macOS may hold a
stale permission entry after the application was replaced with a differently
signed build. Reset the entry in Terminal, then reopen SideScopes and grant the
permission again:

```sh
tccutil reset ScreenCapture org.sidescopes.app
```

Remove duplicate copies of SideScopes when diagnosing this problem. Copies
share the same application identifier, and macOS may open a different one than
the copy you changed.

Windows does not require a separate screen-capture permission.

## macOS blocks the download

Current releases are not signed or notarized. If you have verified the download
and choose to run it, first try to open SideScopes, then open **System Settings
→ Privacy & Security**, scroll to Security, and select **Open Anyway**. macOS
will ask you to confirm the exception.

Do not override a warning that says the application will damage your computer
or appears damaged. Those messages are distinct from the unidentified-developer
warning. Download the release again from the official releases page and report
the result if its published SHA-256 does not match.

## Screen capture was interrupted

Display sleep, screen locking, disconnection, or a display-configuration change
can end the capture stream. SideScopes clears the live trace, reports the
interruption, and attempts to reconnect when the display becomes available. It
does not retain a stale frame as a current reading.

## The interface is too small or too large

Choose **UI Scaling** from the right-click menu. Values from 50% to 200% multiply
the scale reported by the operating system. At the default setting, SideScopes
follows the display density as its window moves between displays.

## Resetting preferences

Choose **Diagnostics → Reset to Defaults**, or quit SideScopes and remove its
preferences file:

- macOS: `~/Library/Application Support/SideScopes/preferences.txt`
- Windows: `%APPDATA%\SideScopes\preferences.txt`

The file is plain text with one `key=value` pair per line. Unknown keys are
ignored. Before version 1.0, an update may reset settings when the file format
changes.

## Recording a diagnostic log

Choose **Diagnostics → Record Diagnostic Log** before reproducing a problem.
SideScopes writes timestamped state and event lines to
`sidescopes/sidescopes-diag.log` inside the system temporary directory. Choose
**Show Diagnostic Log** to open the folder.

The log records window titles and application names. Review it before sharing.
The previous recording is retained beside it as `sidescopes-diag.prev.log`.

On Windows, **Show in Screen Captures** temporarily makes SideScopes visible to
capture and screenshot tools. Its windows are normally excluded so the scopes
do not analyze themselves.

## Development diagnostics

`SIDESCOPES_DIAG` starts logging at launch for a comma-separated list of
channels, or for `all`:

| Channel | Records |
| --- | --- |
| `attach` | Window-focus routing |
| `border` | Region-border drawing |
| `suggestions` | Window suggestions and picker mapping |
| `facelock` | Face-tracking decisions |
| `perf` | Frame, analysis, and capture-cadence timing |
| `modules` | Scope-module discovery and loading |
| `interface` | Interface-toolkit validation errors |

For example:

```sh
SIDESCOPES_DIAG=attach,border ./SideScopes
```

`SIDESCOPES_DIAG_FILE` sets a different log path and
`SIDESCOPES_PREFS_FILE` sets a different preferences path. Use the latter for a
test instance that must not change regular settings.

Logs flush on a short interval by default. `SIDESCOPES_DIAG_FLUSH=1` flushes
every line when investigating a crash. `SIDESCOPES_DIAG_FLUSH=0` buffers until
recording stops to reduce measurement overhead.

## Reporting a problem

Open an issue at <https://github.com/sidescopes/sidescopes/issues> with the
platform and SideScopes version, the steps that reproduce the problem, and a
reviewed diagnostic log when it is relevant.
