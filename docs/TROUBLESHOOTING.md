# Troubleshooting

## SideScopes cannot see the screen

On macOS, reading the screen needs a permission, and without it the
application says so on its own front page instead of showing empty scopes.
The setting is System Settings → Privacy & Security → Screen Recording,
and the application has to be quit and reopened after the switch is turned
on, because macOS only hands the permission over at launch. Windows needs
no permission for this.

**The permission loop.** macOS ties the grant to the application's code
signature rather than to its name, so replacing SideScopes with a
differently signed build leaves a stale grant behind: the switch reads as
on while the application still cannot see anything. Clear the stale grant
and approve once more:

```sh
tccutil reset ScreenCapture org.sidescopes.app
```

Then relaunch and grant the permission when prompted.

If several copies of SideScopes are on the disk, keep only one. They share
an identifier, and the system may open a copy other than the one you meant.

## macOS refuses to open the download

If macOS reports that SideScopes cannot be opened because it comes from an
unidentified developer, right-click the application and choose Open. That
dialog offers to open it anyway, and the answer is remembered. Only a build
signed by an identified developer opens on a double-click.

## Screen capture was interrupted

The capture stream ends when the display configuration changes — a display
sleeping, the screen locking, a monitor unplugged. SideScopes says so on
the panes and reconnects by itself when the display returns. It never
presents a stale reading as a live one.

## The interface is too small, or too large

The right-click menu has a UI Scaling submenu, from 50% to 200%. It
multiplies the scale the operating system already applies rather than
replacing it, so a window keeps the setting as it crosses between displays
of different densities. Until you choose a size, the default is the one the
display's own density suggests.

## Starting over

"Reset to Defaults", in the Diagnostics submenu of the right-click menu,
returns every setting to its shipped value. So does deleting the
preferences file, which is `preferences.txt` under `Application
Support/SideScopes` on macOS and `%APPDATA%\SideScopes` on Windows. It is
plain text, one `key=value` per line, and unknown keys are ignored.

Before 1.0 the file is allowed to change shape without a migration, so an
update may return settings to their defaults.

## Reporting a problem

The Diagnostics submenu of the right-click menu records what the
application is doing. "Record Diagnostic Log" streams timestamped state
lines to `sidescopes-diag.log`, in a `sidescopes` folder inside the system
temporary directory, while you reproduce the issue; "Show Diagnostic Log"
opens that folder. The recording opens by stating what is already true —
the capture format, its crop and cadence, the scopes on offer and the
letters they hold — so it is worth starting one before reproducing rather
than after.

The log records window titles and application names. Glance over it before
sharing.

On Windows, "Show in Screen Captures" makes the SideScopes windows visible
to screenshot tools, which they normally are not, so that the scopes never
end up analyzing themselves.

## Diagnostics from the environment

For development, `SIDESCOPES_DIAG` starts a recording at launch with only
the named channels — for example `SIDESCOPES_DIAG=attach,border`, or `all`:

| Channel | Records |
| ------- | ------- |
| `attach` | window-focus routing |
| `border` | region-border drawing |
| `suggestions` | the picker's window suggestions and pick mapping |
| `facelock` | face-lock probe verdicts |
| `perf` | frame, analysis-pass and capture-cadence timings |
| `modules` | which scope modules loaded, and why one did not |
| `interface` | errors the interface toolkit reports about its own use |

The `interface` channel is worth including in a recording attached to a
report even when the interface looked fine. The toolkit checks its own use
as it draws and normally says so in a window over the application, which
only whoever is sitting in front of the build ever sees; on that channel it
becomes a line in the file. The same error repeats on every frame it is
raised, so only a message the recording has not already been told is
written.

The previous run is kept beside the log as `sidescopes-diag.prev.log`.
`SIDESCOPES_DIAG_FILE` places the log elsewhere, and
`SIDESCOPES_PREFS_FILE` places the preferences file, so a throwaway
instance can take its own settings without disturbing the real ones.

Writes reach the disk on a short interval by default.
`SIDESCOPES_DIAG_FLUSH=1` flushes every line when chasing a crash, and
`SIDESCOPES_DIAG_FLUSH=0` buffers until recording stops, so logging does
not distort a performance measurement.
