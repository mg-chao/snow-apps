# Snow Shot Homebrew tap

Snow Shot supports Apple Silicon and Intel Macs running macOS 15 or later when
the release provides the corresponding package. Snow Shot Mini requires Apple
Silicon. After the first stable cask is published:

```sh
brew update
brew install --cask mg-chao/tap/snow-shot
brew upgrade --cask snow-shot
brew uninstall --cask snow-shot
```

If Homebrew reports that `Casks/snow-shot.rb` is missing and offers only
`snow-shot@beta`, run `brew update` and retry the stable install. This means
the local tap checkout has not received the published stable cask. See the
[macOS Homebrew troubleshooting steps](https://github.com/mg-chao/snow-apps/blob/main/docs-macos-build.md#homebrew-installation)
if it is still missing after the update.

Beta releases use a separate cask:

```sh
brew install --cask mg-chao/tap/snow-shot@beta
brew upgrade --cask snow-shot@beta
```

Both channels install `Snow Shot.app`. Quit the app and uninstall the current
cask before switching channels; uninstall preserves user data and signing state.

Homebrew manages the app; installation reuses Snow Shot's local signing identity.
The first installation may request Keychain access and macOS privacy permissions.
Keep `~/Library/Application Support/Snow Shot/Installer` and the original Keychain
identity across upgrades and reinstalls. Uninstall retains these and your user data.
Local signing is not Apple notarization; macOS may request permission again.

For an existing manual installation, quit Snow Shot and move `Snow Shot.app` (or
legacy `snow_shot.app`) out of Applications to a backup folder before installing.
Keep its signing state and Keychain identity. Do not use `--force` to overwrite it.
Delete the backup only after confirming the Homebrew installation works.

Use `brew install --cask --appdir="$HOME/Applications" mg-chao/tap/snow-shot` for
another application directory (create it first). Use the same desktop account for
upgrades. Quit Snow Shot and finish recordings before upgrading or uninstalling.

Release and recovery details: [Snow Shot macOS documentation](https://github.com/mg-chao/snow-apps/blob/main/docs-macos-build.md).

The publisher verifies the downloaded DMG against GitHub's asset SHA-256 digest
and the `.sha256` sidecar when present. At least one checksum source is required;
if both exist, both must match. The Homebrew archive always includes a checksum
sidecar for the installer, even when the original release uses only GitHub's digest.
Each architecture has its own reproducible archive. Full casks select the Apple
Silicon or Intel archive automatically when both are present; releases containing
only one architecture retain that requirement. Existing archives and casks cannot
change at the same version, so Intel support for an ARM64-only published version
must be added in a new release.
