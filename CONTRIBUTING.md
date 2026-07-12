# Contributing

## Development setup

```sh
git clone git@github.com:sidescopes/sidescopes.git
cd sidescopes
cmake -B build
cmake --build build
ctest --test-dir build
```

CMake 3.24+, a C++20 compiler, and `clang-format` for the formatting check.
The application builds on macOS and Windows; the core library and its tests
build everywhere.

### macOS

Xcode Command Line Tools provide the compiler. The rest comes from
[Homebrew](https://brew.sh):

```sh
xcode-select --install
brew install cmake ninja clang-format
```

### Windows

Visual Studio 2022 or newer with the "Desktop development with C++" workload
provides the compiler. The rest comes from
[winget](https://learn.microsoft.com/windows/package-manager/):

```powershell
winget install Kitware.CMake Ninja-build.Ninja LLVM.LLVM
```

`LLVM.LLVM` supplies `clang-format`; add `C:\Program Files\LLVM\bin` to
`PATH` if the installer did not. The Visual Studio generator finds MSVC on
its own, so any shell works; build multi-config trees with `--config`:

```powershell
cmake -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` describes the same tree, so the presets workflow is
equivalent: `cmake --preset vs`, `cmake --build --preset release`,
`ctest --preset release`. The `werror` presets mirror CI's
warnings-as-errors build in a separate `build-werror/` tree.

To work in the Visual Studio IDE, use File > Open > Folder on the
repository: it picks the presets up automatically, builds into the same
`build/` directory as the command line, and offers the Debug and Release
configurations. Pick `SideScopes.exe` as the startup item to run or
debug.

## Screen-recording permission for development builds (macOS)

macOS binds the screen-recording permission to an application's code-signing
identity. An ad-hoc signature's identity is the binary hash, so it changes on
every build and the permission you granted is silently lost — the app reports
the permission as missing even though System Settings still shows it enabled.

Create a stable self-signed identity once, and a single grant survives every
rebuild:

```sh
cat > /tmp/sidescopes-cert.conf <<'CONF'
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = SideScopes Dev
[v3]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
CONF
openssl req -x509 -newkey rsa:2048 -keyout /tmp/sidescopes-key.pem \
    -out /tmp/sidescopes-cert.pem -days 3650 -nodes -config /tmp/sidescopes-cert.conf
openssl pkcs12 -export -out /tmp/sidescopes.p12 -inkey /tmp/sidescopes-key.pem \
    -in /tmp/sidescopes-cert.pem -passout pass:sidescopes -name "SideScopes Dev" \
    -legacy -macalg sha1
security import /tmp/sidescopes.p12 -k ~/Library/Keychains/login.keychain-db \
    -P sidescopes -T /usr/bin/codesign -A
```

Reconfigure so CMake picks up the identity (`cmake -B build`); it reports
`SideScopes code-signing identity: SideScopes Dev`. Reset any stale grant
once with `tccutil reset ScreenCapture org.sidescopes.app`, launch, and grant
the prompt. Without the certificate the build still works — it falls back to
ad-hoc signing and you re-grant after each rebuild.

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
