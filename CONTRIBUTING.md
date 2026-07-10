# Contributing

## Development setup

```sh
git clone git@github.com:sidescopes/sidescopes.git
cd sidescopes
cmake -B build
cmake --build build
ctest --test-dir build
```

CMake 3.24+, Ninja (recommended), and a C++20 compiler. macOS builds need the
Command Line Tools; the app target builds on macOS only, while the core
library and its tests build everywhere.

## Checks

Run what CI runs before pushing:

```sh
clang-format --dry-run --Werror $(git ls-files '*.h' '*.cpp' '*.mm')
cmake -B build -DSIDESCOPES_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Expectations

- Every change ships with its tests and any needed documentation in the same
  commit. Every commit builds and passes the test suite.
- Prefer clear, spelled-out names over abbreviations. Match the surrounding
  style; `.clang-format` is authoritative for layout.
- The core library stays free of platform includes; platform code lives under
  `src/platform/<os>/` behind the interfaces in `src/platform/`.
- Dependencies are a deliberately short list. Adding one is a design decision,
  not a convenience — expect it to be questioned.
- Never copy code from other projects, regardless of license. Reference
  implementations are for understanding; everything here is written for this
  codebase.
- SideScopes in prose; `sidescopes` for the repository, binary, and paths.
  Never "Side Scopes".
- Commit messages follow Conventional Commits (`feat:`, `fix:`, `docs:`,
  `ci:`, `chore:`, `perf:`, `refactor:`). Subjects are short, lowercase
  phrases; bodies explain the why. History stays linear.

## Licensing of contributions

The project is GPL-3.0-or-later. To keep long-term options open (such as
signed store builds that GPL terms alone cannot cover), contributors will be
asked to sign a lightweight CLA when submitting their first pull request. The
promise in the README holds regardless: what is free stays free.

## Releasing

1. Update `CHANGELOG.md`: retitle `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD`.
2. Bump the version in the top-level `CMakeLists.txt`.
3. Commit as `chore: release X.Y.Z`, tag `vX.Y.Z`, push with tags.
