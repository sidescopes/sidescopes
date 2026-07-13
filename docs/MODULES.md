# Scope modules

Every analysis scope loads through one C ABI - the built-ins included.
The contract lives in `include/sidescopes/module.h`; the design record
and its precedent research live outside the repository.

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

## Phasing

- P1 (in progress on this branch): the built-in engines wrap into
  SsScopeInstance vtables and register statically through SsModuleEntry;
  the analysis worker consumes instances. Behavior identical.
- P2: the scope registry replaces the built-in enum; menus, sliders,
  markers, and graticules become loops over descriptors and primitives.
- P3: the dynamic loader (dev/CI first: -DSIDESCOPES_MODULES_DYNAMIC=ON);
  release stays statically registered until operational evidence says
  otherwise.
- P4: the first out-of-tree module; ABI freezes at 1.0.

## P1 continuation checklist

- [x] include/sidescopes/module.h - the boundary contract
- [x] src/modules/module_registry.{h,cpp} - static registration, host
      struct, C++ RAII instance wrapper
- [x] src/modules/vectorscope_module.cpp - the pattern-setter
- [x] src/modules/waveform_module.cpp - registers the waveform AND the
      parade (one module, two scopes)
- [x] src/modules/histogram_module.cpp
- [x] AnalysisWorker drives analysis through instances
- [x] Adaptive image sizing via the "sidescopes.adaptive_image/1"
      instance extension (worker side)
- [ ] Per-module CMake targets that see only module.h at their boundary
      (the build-enforced hourglass; engines compile into their module)
- [ ] Dev/CI dynamic mode behind SIDESCOPES_MODULES_DYNAMIC
