# Releasing

## Cutting a release

1. Update `CHANGELOG.md`: move the `Unreleased` entries under a new version
   heading with today's date.
2. Bump the version in `CMakeLists.txt` (`project(sidescopes VERSION ...)`).
3. Commit, tag, and push:

   ```sh
   git tag v0.1.0
   git push origin main v0.1.0
   ```

The `Release` workflow creates a draft release first, then the macOS and
Windows jobs build, test, and upload their own artifacts
(`SideScopes-<tag>-macos.zip` and `SideScopes-<tag>-windows.zip`, each with a
SHA-256 checksum) to it in parallel. A final job publishes the release only
after both platforms succeed, so it never goes live incomplete. If a platform
fails, the release stays a draft; re-running the failed job (Actions →
"Re-run failed jobs") rebuilds only that platform and then re-runs publish, so
a fixed run finishes the release on its own. The Windows executable links the
C runtime statically, so it needs no Visual C++ redistributable. Verify a
Windows build on a clean machine before relying on it; a `v*-rc` pre-release
tag is a safe way to exercise the full pipeline first.

## Signing and notarization

The workflow signs and notarizes automatically when these repository
secrets exist; without them it ships an ad-hoc build, which Gatekeeper
only opens via right-click - Open.

| Secret | Content |
| ------ | ------- |
| `MACOS_CERTIFICATE_P12` | base64 of the Developer ID Application certificate (.p12) |
| `MACOS_CERTIFICATE_PASSWORD` | the .p12 password |
| `MACOS_SIGNING_IDENTITY` | e.g. `Developer ID Application: Name (TEAMID)` |
| `APPLE_NOTARIZATION_ID` | Apple ID used for notarization |
| `APPLE_NOTARIZATION_PASSWORD` | app-specific password for that Apple ID |
| `APPLE_TEAM_ID` | the developer team identifier |

All six require Apple Developer Program membership.

## Homebrew cask

Once releases are notarized, create a tap repository named
`sidescopes/homebrew-tap` containing `Casks/sidescopes.rb`:

```ruby
cask "sidescopes" do
  version "0.1.0"
  sha256 "<contents of the .sha256 asset>"

  url "https://github.com/sidescopes/sidescopes/releases/download/v#{version}/SideScopes-v#{version}-macos.zip"
  name "SideScopes"
  desc "Real-time color scopes for photo editing"
  homepage "https://sidescopes.org"

  depends_on macos: ">= :sonoma"

  app "SideScopes.app"

  zap trash: [
    "~/Library/Application Support/SideScopes",
  ]
end
```

Users then install with:

```sh
brew tap sidescopes/tap
brew install --cask sidescopes
```

Update `version` and `sha256` on every release; the checksum ships as a
release asset.
