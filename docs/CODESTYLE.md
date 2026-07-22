# Code style

Layout is enforced by `clang-format` (`.clang-format`) and checked in CI. The
conventions below cover what the formatter cannot express. `clang-tidy`
(`.clang-tidy`) enforces a curated set of correctness, performance, and
portability checks.

## Formatting

Google base, 4-space indentation, 120-column lines. Functions, classes,
structs, and enums open their brace on its own line; namespaces and control
statements keep the brace attached. `switch` case labels sit flush with the
`switch`. Braced initializer lists carry no interior spaces (`{1, 2}`).
Constructor initializers break onto their own lines, leading colon, one
initializer per line. Namespaces are not indented. Pointers and references
bind to the type (`int* p`, `const Frame& frame`). No single-line collapsing:
every `if`, `for`, `while`, and `else` carries braces and a real line. A blank
line separates definitions.

## Naming

- **Types** (classes, structs, enums, aliases): `PascalCase`.
- **Functions and methods**: `camelCase` — `drawWaveform`, `isVisible`.
- **Local variables and parameters**: `camelCase`.
- **Member variables**: an `m_` prefix with a `camelCase` body — `m_frameCount`.
- **Public data members** of plain structs: `camelCase`, no prefix.
- **File-scope globals** (mutable state in anonymous namespaces, typically
  platform callback state): a `g_` prefix with a `camelCase` body —
  `g_borderEditing`.
- **Constants** (`constexpr` and `const` at namespace scope, enumerators):
  `PascalCase` with no prefix — `DefaultVectorscopeSize`.
- Because functions and variables share `camelCase`, a local can shadow a
  same-named function. Disambiguate by renaming the local, qualifying a member
  call with `this->`, or giving the free function a distinct verb name.
- Prefer spelled-out names over abbreviations.

## Vocabulary

Feature names come from the interface: an action a user can see and the code
behind it share the same distinctive word, so searching for one finds the
other. `Attach to Window...` is `AttachWindow`, `shortcut_attach_window`.

- **Region** — the rectangle the scopes read. Not "area": `area` is left for
  plain geometry with no region behind it, like an ImGui content area.
- **Attached region** — bound to a window, following it, in effect only while
  that window is focused. **Global region** — bound to nothing, living on a
  display. `RegionKind` names the pair; almost everything about regions hangs
  off which one is in hand.
- **Attached** is membership, whether a window holds a region at all;
  **active** is the one attached window in effect right now.
- **Suggestion** — what the picker overlay draws for one-click confirmation.
  **Candidate** — what the application keeps so a confirmed pick resolves back
  to the window or face it came from.
- **Pin** belongs to the colour-reference feature alone. Locking a region onto
  a face is `face_lock`; the region border's toggle speaks attach. (The
  `Icon::Pin` glyph keeps its name — icons are named for what they depict.)
- **Display** for a monitor, and **captured display** for the one the scopes
  read; `monitor` and `screen` appear only where a platform API brings them.

One word per concept. A second word for something that already has one is
drift: replace it rather than letting both live.

## The C module ABI

`include/sidescopes/module.h` is a frozen C boundary: `Ss`-prefixed types,
`SS_`-prefixed macros and constants, and `snake_case` struct fields. Its
identifiers stay stable across versions and follow C conventions rather than
the rules above. The C++ that wraps the boundary translates to the conventions
above; the two naming worlds meet only at that seam.

## Comments

- Public declarations in headers carry Doxygen `///` documentation comments,
  using JavaDoc-style tags (`@brief`, `@param`, `@return`). Editors surface
  these on hover.
- Implementation-internal comments use plain `//` prose.
- Documentation comments are written in sentence case with terminal
  punctuation.

## Vertical spacing

A blank line separates top-level definitions — the formatter enforces this.
Within a function, use blank lines to separate distinct logical steps, by
judgment; the code reads as prose, not as one dense block.

## Files and directories

- Source and header filenames are `snake_case`: `analysis_worker.cpp`,
  `module_registry.h`. Directory names are lowercase.
- Extensions are `.cpp` and `.h`, with `.mm` for Objective-C++ on macOS.
- A header pairs with a source file of the same stem (`foo.h` and `foo.cpp`).
- Public headers live under `include/sidescopes/`; the directory namespaces
  them, so their filenames carry no project prefix.
- A file groups a coherent unit — a class together with its close
  collaborators — rather than strictly one class per file.

## Dependencies

The dependency list is deliberately short and closed. Adding one is a design
decision, not a convenience.
