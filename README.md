# Snow Apps

Snow Apps repository, providing source code for Snow Shot and Snow Image Viewer.

<div style="font-size: 128px">🏗️🚧🦺</div>

## Install Snow Shot on Windows

Download the installer from [Snow Shot releases](https://github.com/mg-chao/snow-apps/releases).
Install the offline Windows x64 package from the WinGet community repository with:

```powershell
winget install --exact --id mg-chao.snow-shot --source winget
```

This package includes published beta versions and the default OCR resources. See
[WinGet release support](docs/snow-shot-releases.md#winget) for upgrades, removal,
and maintainer setup.

Once [Snow Shot's submission](https://github.com/ScoopInstaller/Extras/issues/18870)
is accepted into Scoop's official
[Extras bucket](https://github.com/ScoopInstaller/Extras), install the portable
Windows x64 package with [Scoop](https://scoop.sh):

```powershell
scoop bucket add extras
scoop install extras/snowshot
```

The Scoop package follows stable releases and includes the default OCR resources.
Quit Snow Shot before running `scoop update snowshot`; use Scoop rather than the
app's built-in updater. Settings and history persist across upgrades and ordinary
uninstall. See [Scoop release support](docs/snow-shot-releases.md#scoop) for removal,
data locations, and maintainer setup.

## Install Snow Shot on macOS

After the first stable Homebrew release is published, Apple Silicon Macs running
macOS 15 or later can install with:

```sh
brew update
brew install --cask mg-chao/tap/snow-shot
brew upgrade --cask snow-shot
brew uninstall --cask snow-shot
```

Installation reuses Snow Shot's local signing identity and may request Keychain
access and macOS privacy permissions. See [Homebrew installation and migration](docs-macos-build.md#homebrew-installation)
for existing installations, custom app directories, and recovery.

## Open Source Licenses

This is a multi-license repository. See [LICENSE.md](LICENSE.md) for the
repository-level scope rules and third-party material policy.

| Project | License |
| --- | --- |
| `ant_design_qt/` | [Apache License 2.0](ant_design_qt/LICENSE) |
| `snow-crates/` | [Apache License 2.0](snow-crates/LICENSE) |
| `snow_draw_engine_qt/` | [Apache License 2.0](snow_draw_engine_qt/LICENSE) |
| `snow_rust_ffi/` | [Apache License 2.0](snow_rust_ffi/LICENSE) |
| `snow_image/` | [GNU GPL v3.0 or later](snow_image/COPYRIGHT) |
| `snow_image_viewer/` | [GNU GPL v3.0 or later](snow_image_viewer/COPYRIGHT) |
| `snow_shot/` | [GNU GPL v3.0 or later](snow_shot/COPYRIGHT) |

Synchronized and bundled third-party materials retain their upstream licenses.
See [Ant Design Qt third-party notices](ant_design_qt/THIRD_PARTY_NOTICES.md)
and [Snow Shot third-party notices](snow_shot/THIRD_PARTY_NOTICES.md).
