# Snow Shot — free, open-source screenshot and screen recording

[English](README.md) | [简体中文](README.zh-CN.md)

<img src="docs/imgs/app-icon.png" alt="Snow Shot app icon" width="96" height="96">

**Snow Shot (SnowShot)** is a free, open-source screenshot tool for **Windows and
macOS**. Capture and annotate your screen, take scrolling screenshots, extract
text with local OCR, pin references above other windows, and record your screen.

**免费开源截图工具**：支持 Windows 和 macOS，集成截图标注、滚动长截图、离线 OCR
文字识别、贴图与录屏。[阅读中文介绍](README.zh-CN.md)。

[**Download the latest release**](https://github.com/mg-chao/snow-apps/releases/latest)
· [Getting started](docs/guide/getting-started.md)
· [User guides](docs/guide/README.md)
· [Official website](https://snowshot.top/)
· [Report an issue](https://github.com/mg-chao/snow-apps/issues)

This is the **official Snow Shot repository**, maintained by
[mg-chao](https://github.com/mg-chao). The repository is named **Snow Apps** because
it also contains Snow Image Viewer and shared libraries. The previous
`mg-chao/snow-shot` address redirects here. The official website is **snowshot.top**.

## What you can do with Snow Shot

| Task | Features | Guide |
| --- | --- | --- |
| Explain a bug or share a detail | Region, window, full-screen and delayed capture; arrows, shapes, text, highlights and blur | [Capture and annotate](docs/guide/getting-started.md) |
| Save a long page or conversation | Scrolling capture with a stitched preview | [Scrolling screenshots](docs/guide/scrolling-screenshots.md) |
| Copy text from an image | Local text recognition; table and QR recognition in the full edition | [Offline OCR](docs/guide/offline-ocr.md) |
| Keep a reference in view | Pin screenshots and clipboard content above other windows | [Pin to screen](docs/guide/pin-and-record.md#pin-a-reference-above-other-windows) |
| Show a sequence of actions | Screen recording with configurable audio and cursor presentation | [Screen recording](docs/guide/pin-and-record.md#record-a-short-demonstration) |
| Find a previous capture | Local screenshot history with search and retention settings | [History and privacy](docs/guide/getting-started.md#history-and-privacy) |

## Download

Get packages from [GitHub Releases](https://github.com/mg-chao/snow-apps/releases/latest).
The [official download page](https://snowshot.top/download.html) helps select an
architecture and also provides the maintained Gitee mirror.

| System | Full edition | Mini |
| --- | --- | --- |
| Windows x64 | Online installer, offline installer, portable ZIP | Online installer, portable ZIP |
| Windows ARM64 | Native online installer, offline installer, portable ZIP | Native online installer, portable ZIP |
| macOS 15+ · Apple silicon | DMG, Homebrew | DMG |
| macOS 15+ · Intel | DMG, Homebrew | Not available |

**Choose the full edition** for text, table and QR recognition. The Windows
offline installer includes the default OCR resources. **Choose Mini** for a
smaller package focused on capture, annotation, pinning and recording; text OCR
resources download when needed, and table/QR recognition is omitted. Mini has no
offline installer. See [package selection and setup](docs/guide/getting-started.md).

Windows users can also install from the
[Microsoft Store](https://apps.microsoft.com/detail/9NRTJ1GBX69V).

## Install Snow Shot on Windows

Download the installer from [Snow Shot releases](https://github.com/mg-chao/snow-apps/releases).
Install the offline Windows x64 package from the WinGet community repository with:

```powershell
winget install --exact --id mg-chao.snow-shot --source winget
```

This package includes published beta versions and the default OCR resources. See
[WinGet release support](docs/snow-shot-releases.md#winget) for upgrades, removal,
and maintainer setup.

Install the portable package with [Scoop](https://scoop.sh) from the official
[Extras bucket](https://github.com/ScoopInstaller/Extras/blob/master/bucket/snowshot.json):

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

Apple silicon and Intel Macs running macOS 15 or later can install the stable
full edition with:

```sh
brew update
brew install --cask mg-chao/tap/snow-shot
brew upgrade --cask snow-shot
brew uninstall --cask snow-shot
```

Installation reuses Snow Shot's local signing identity and may request Keychain
access and macOS privacy permissions. See [Homebrew installation and migration](docs-macos-build.md#homebrew-installation)
for existing installations, custom app directories, and recovery.

## Your first screenshot

1. Launch Snow Shot. On macOS, grant screen recording permission when requested.
2. Press **F1**, select a region or window, and use the toolbar to annotate it.
3. Copy, save or pin the result. **Ctrl+F1** (**Command+F1** on macOS) starts a capture that copies the
   confirmed selection directly; **F3** pins clipboard content.

These are the default shortcuts. If your keyboard uses function keys for media
controls, use **Fn** or assign another binding in Snow Shot's shortcut settings.
Existing custom bindings take precedence. Read the
[step-by-step guide](docs/guide/getting-started.md) for setup and troubleshooting.

## Frequently asked questions

### Is Snow Shot free and open source?

Yes. Snow Shot is licensed under **GPL-3.0-or-later**. The repository contains
other components under their own licenses; see the license table below.

### Does OCR work offline?

Standard text recognition runs locally once its model resources are installed.
For an offline Windows setup, choose the full offline installer. Translation and
optional online or AI features have separate network and provider requirements.
See [offline OCR](docs/guide/offline-ocr.md) and the
[privacy policy](https://snowshot.top/privacy.html).

### Does Snow Shot support Linux?

The published desktop packages currently target Windows and macOS. There is no
official Linux package. Source availability does not imply Linux runtime support.

### Where are the official downloads and source code?

Use this repository's [Releases](https://github.com/mg-chao/snow-apps/releases/latest)
or **snowshot.top**. Forks and third-party download sites may describe older
versions. Report problems against the version and package you actually installed.

## Development and repository layout

Snow Apps contains the Snow Shot app (`snow_shot/`), Snow Image Viewer
(`snow_image_viewer/`), shared Qt/C++ libraries, and Rust components in
`snow-crates/`. See [AGENTS.md](AGENTS.md) for build commands and repository rules,
[Windows ARM64 build support](docs-windows-arm64-build.md), and
[macOS build support](docs-macos-build.md). User documentation lives in
[docs/guide/](docs/guide/README.md).

## Maintain the OCR submodule

[`snow-crates/crates/simd-paddle-ocr-rs/`](snow-crates/crates/simd-paddle-ocr-rs/)
is a Git submodule with its own
[repository](https://github.com/mg-chao/simd-paddle-ocr-rs), history, branches,
and releases. Snow Apps records the exact OCR commit to check out.
The submodule is excluded from the Snow Crates Cargo workspace so it retains
its own pinned nightly toolchain and lockfile. Run its Cargo commands from
the submodule directory.

After cloning Snow Apps, initialize the submodule from the repository root:

```sh
git submodule update --init --recursive
```

For a new clone, `git clone --recurse-submodules` initializes it automatically.
Submodule updates normally check out a detached HEAD. Before editing OCR code,
switch to `main` or a feature branch inside the submodule:

```sh
git -C snow-crates/crates/simd-paddle-ocr-rs switch main
```

Follow the submodule's README and AGENTS.md for its build and validation commands.
Commit and push OCR changes to its own repository first, then record the new
revision in Snow Apps:

```sh
git add snow-crates/crates/simd-paddle-ocr-rs
git commit -m "build(ocr): update simd paddle ocr submodule"
```

## Open Source Licenses

This is a multi-license repository. See [LICENSE.md](LICENSE.md) for the
repository-level scope rules and third-party material policy.

| Project | License |
| --- | --- |
| `ant_design_qt/` | [Apache License 2.0](ant_design_qt/LICENSE) |
| `snow-crates/` | [GNU GPL v3.0 or later](snow-crates/COPYRIGHT) |
| `snow_draw_engine_qt/` | [Apache License 2.0](snow_draw_engine_qt/LICENSE) |
| `snow_rust_ffi/` | [GNU GPL v3.0 or later](snow_rust_ffi/COPYRIGHT) |
| `snow-crates/crates/simd-paddle-ocr-rs/` | [Apache License 2.0](snow-crates/crates/simd-paddle-ocr-rs/LICENSE) |
| `snow_image/` | [GNU GPL v3.0 or later](snow_image/COPYRIGHT) |
| `snow_image_viewer/` | [GNU GPL v3.0 or later](snow_image_viewer/COPYRIGHT) |
| `snow_shot/` | [GNU GPL v3.0 or later](snow_shot/COPYRIGHT) |

Synchronized and bundled third-party materials retain their upstream licenses.
See [Ant Design Qt third-party notices](ant_design_qt/THIRD_PARTY_NOTICES.md)
and [Snow Shot third-party notices](snow_shot/THIRD_PARTY_NOTICES.md).

## Contributing / 贡献指南

Only fix PRs will be reviewed. Feature PRs are not accepted directly; please start
a community discussion first if you need a specific feature.
See the [contribution guidelines](CONTRIBUTING.md) for details.

仅审查修复问题的 PR，不直接接受新增功能或功能增强的 PR。如有功能需求，请先在社区发起讨论。
详情请阅读[贡献指南](CONTRIBUTING.md)。

## Community

[![LINUX DO](https://img.shields.io/badge/LINUX%20DO-Community-blue)](https://linux.do)
