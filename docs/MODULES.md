# Scope modules

Every analysis scope loads through one C ABI - the built-ins included.
The contract lives in `include/sidescopes/module.h`.

A module declares one or more scopes. Each carries an id, a display name,
a shortcut letter and its parameters, and the host builds the scope
selector, the menus, the shortcuts and the preferences from those
declarations rather than from a list of its own. What the scopes do with
that is [SCOPES.md](SCOPES.md).

## Shape

- Modules are pure analysis: (frame, region, parameters) -> image plus
  declarative overlay data. The host owns capture, region math, change
  detection, rendering, panes, gestures, menus, preferences, styling.
- The core is tiny and will freeze at ABI 1.0; growth happens only
  through extensions queried by string id on both sides. At 0.x there is
  no stability promise.
- Instances are single-threaded. The host runs one analysis instance on
  its worker thread and a separate overlay instance on the main thread,
  configured identically.
- No allocation, exceptions, or toolkit types cross the boundary.

## Where it stands

- The built-in engines are modules: each wraps into an `SsScopeInstance`
  vtable and registers through `SsModuleEntry`, and the analysis worker
  consumes instances rather than an enum of scopes.
- The host is registry-driven throughout. The scope selector, the menus,
  the sliders, the markers and the graticules are loops over descriptors
  and declarative primitives, so a scope reaches the interface without the
  host naming it.
- Dynamic loading works on the supported desktop platforms behind
  `-DSIDESCOPES_MODULES_DYNAMIC=ON`, where each module builds as a shared
  object exporting `ss_module_entry` and a loader gates it on the ABI
  version (both numbers must match before 1.0). CI builds and tests that
  configuration on all native platforms. Release builds stay
  statically registered until there is operational reason to change.
- The ABI is at 0.6 and carries no stability promise. It freezes at 1.0,
  which is also when the first out-of-tree module is expected to matter.

## HDR frame data

The four-byte SDR pixel layouts are unchanged. A frame can additionally carry
`hdr_luminance`, a tightly packed float plane with the same dimensions and
origin. Values are linear luminance relative to SDR white: 1 is white, and
values above 1 retain highlights. `hdr_white_nits` records the nominal encoding
reference, not measured panel brightness. A null plane means unavailable;
non-finite samples are invalid. Modules must not synthesize HDR headroom from
clipped SDR codes. Both ABI version numbers must match, so rebuild older modules.

HDR Luminance is a separate module because its bins and axis differ from the
SDR waveform family. The `sidescopes.reading/1` instance extension supplies
bounded text from the completed analysis pass. The host copies it beside the
image, with the same frame and region provenance; overlay instances do not
produce measurement text.
