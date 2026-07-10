# SideScopes

Real-time color scopes for photo editing. SideScopes watches a region of your
screen and shows a live vectorscope and waveform beside any editor — Lightroom,
Capture One, darktable, or anything else that draws pixels.

Photo editors ship a histogram and little else. Colorists have relied on
vectorscopes and waveforms for decades because they answer questions a
histogram cannot: is the white balance actually neutral, how far does the
sky stretch toward cyan, do the highlights clip past the level you care
about. SideScopes brings those instruments to photographers, reading the
screen itself, so it works with every editor and needs no plugins.

## Features

- Vectorscope (BT.601/BT.709) with classic graticule and skin-tone line
- Waveform with luma, RGB overlay, and combined modes
- Drag-to-select the scoped screen region, with a persistent border marker
- Live marker showing the color under your cursor on every scope
- Compact, always-on-top window designed for small laptop screens
- Native context menus and direct manipulation; scroll to adjust trace intensity

## Status

Early development. macOS support lands first; Windows follows; Linux is
planned. Watch the releases page.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

Requires CMake 3.24+ and a C++20 compiler. Dependencies are fetched during
configuration.

## License

SideScopes is free software, licensed under the
[GNU GPL v3.0 or later](LICENSE).

The full application is free and always will be: features that are free today
stay free. If optional paid extras ever appear, they will only ever be
additions on top of the complete, free tool.
