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
- Waveform with luma, RGB overlay, combined, and RGB parade modes
- Histogram with per-channel bars
- Smart region selection: click a detected photo, face, or window, or draw
  the area yourself — the screenshot-tool idiom you already know
- Per-application memory: the region you scoped in an editor is offered
  again when you come back to it
- Live marker showing the color under your cursor on every scope, with
  pinnable reference colors for matching skin tones across photos
- Compact, always-on-top window designed for small laptop screens
- Native context menus and direct manipulation; scroll to adjust trace intensity

## Keyboard

| Key | Action |
| --- | ------ |
| V / W / L / R / H | show one scope alone: vectorscope, RGB waveform, luma waveform, RGB parade, histogram |
| Shift+V / W / L / R / H | stack or unstack a scope; panes follow the order they were turned on |
| A | pick a window |
| F | pick a face (macOS) |
| D | draw an area |
| Esc | reset to the full screen |
| P | pin the color under the cursor |

The region border is live on the desktop: drag it to move the region,
drag a corner to resize, and Shift-drag an edge to resize that edge alone.

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

## Troubleshooting

**The permission loop.** macOS ties the Screen Recording permission to the
app's code signature. If SideScopes keeps asking for permission although
System Settings shows it granted - typical after replacing the app with a
differently signed build - clear the stale grant and approve once more:

```sh
tccutil reset ScreenCapture org.sidescopes.app
```

Then relaunch SideScopes and grant the permission when prompted. Toggling
the switch in System Settings only takes effect after the app restarts.

## License

SideScopes is free software, licensed under the
[GNU GPL v3.0 or later](LICENSE).

The full application is free and always will be: features that are free today
stay free. If optional paid extras ever appear, they will only ever be
additions on top of the complete, free tool.
