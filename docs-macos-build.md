# Building Snow Shot on macOS

The app presets target macOS 15 or newer, with separate Apple Silicon (`arm64`)
and Intel (`x64`) builds. Each build uses one architecture throughout CMake,
vcpkg and Cargo; universal builds are not supported by these presets.

## Snow Shot Mini

Apple Silicon presets build both `snow_shot` and `snow_shot_mini` from the same
configuration. Intel presets build Snow Shot only; Snow Shot Mini requires
`arm64` on macOS. `SNOW_APPS_BUILD_SNOW_SHOT_MINI` controls the additional target
and defaults to `ON` in the Apple Silicon presets. Use an explicit `--target`
to build one edition, or configure with this option `OFF` for a full-only build.
The build wrappers select the enabled editions automatically. Paired CMake build
presets retain both target names, so a direct build with Mini disabled must use
`cmake --build --preset build-<preset> --target snow_shot`. Apple Silicon release
packaging requires both editions and rejects a Mini-disabled cache.

Mini removes QR, table, Markdown, HTML, LaTeX, translation, Extended Features,
and API Configuration. Manual text recognition remains available; its toolbar
button is hidden by default, and Pin to Screen automatic recognition is off.
Both macOS bundles include the local OCR worker and runtime. Full also bundles
the default Small V6 model; Mini downloads the selected model on first use and
reuses its verified cache afterward.

```sh
scripts/build.sh snow-shot-macos-arm64-debug --target snow_shot_mini
scripts/run-snow-shot.sh snow-shot-macos-arm64-debug --edition mini
scripts/package-snow-shot.sh snow-shot-macos-arm64-release
bash install-snow-shot-macos.sh --edition mini --lang en
```

The Apple Silicon package command produces both
`snow-shot-<version>-macos-arm64.dmg` and
`snow-shot-mini-<version>-macos-arm64.dmg`, each with a `.sha256` sidecar.
Mini installs as `Snow Shot Mini.app`, with executable `snow_shot_mini` and
bundle ID `com.snowshot.snow_shot_mini`. Its settings, launch-at-login entry,
MCP discovery, and installer state use separate product identities. For direct
staging, use `cmake --install build/<preset> --component SnowShotMini --prefix
/path/to/staging`. Both editions share a version and release tag.

## Homebrew installation

The project tap is `mg-chao/homebrew-tap`. Stable releases update `snow-shot`;
beta releases update `snow-shot@beta`, independently of the stable channel.
Tags accept `v<version>` with an optional `_snow-shot` suffix; beta versions use
`-beta` or `-beta.<number>`. Other prerelease versions are not supported. Initially,
Homebrew installation supports Apple Silicon and macOS 15 or later only. Keep
Homebrew current with `brew update`.

```sh
brew update
brew install --cask mg-chao/tap/snow-shot
brew upgrade --cask snow-shot
brew uninstall --cask snow-shot
```

For beta releases, use `brew install --cask mg-chao/tap/snow-shot@beta` and
`brew upgrade --cask snow-shot@beta`. Quit the app and uninstall the current cask
before switching channels because both install `Snow Shot.app`. User data and
the signing identity are retained. Each release needs its versioned DMG and a
GitHub SHA-256 digest or matching `.sha256` asset. When both are present, both
must match; the workflow packages only verified release assets.

If Homebrew says `Casks/snow-shot.rb` does not exist and suggests only
`snow-shot@beta`, its local tap checkout has not received the stable cask yet.
Refresh the tap before retrying:

```sh
brew update
brew info --cask mg-chao/tap/snow-shot
brew install --cask mg-chao/tap/snow-shot
```

Homebrew may defer automatic updates, and `HOMEBREW_NO_AUTO_UPDATE` disables
them. If the stable cask is still missing after `brew update`, inspect the tap
checkout and its remote before changing or removing it:

```sh
tap_dir="$(brew --repository mg-chao/tap)"
git -C "$tap_dir" status --short --branch
git -C "$tap_dir" remote -v
ls "$tap_dir/Casks/snow-shot.rb"
```

Finish recordings and quit Snow Shot before upgrading or uninstalling. Homebrew
owns the final application placement and removal; the cask's preflight verifies
and locally signs a staged copy. The archive includes the DMG, its checksum, and
the installer from the matching release source. Its SHA-256 is pinned in the tap.
Installation never downloads a newer installer or follows the website's mutable
latest-package URL.

The first installation may request login Keychain access or code-signing trust.
The same local identity and designated requirement used by the standalone
installer are retained under `~/Library/Application Support/Snow Shot/Installer`.
Use the same installing account for upgrades. Uninstall does not delete the
identity, installer state, settings, or captures; there is no destructive `zap`
rule. Local signing is not Developer ID notarization and does not guarantee that
macOS will retain Screen Recording or Accessibility consent.

### Migrating an existing installation

Quit Snow Shot and move `/Applications/Snow Shot.app` (or the legacy
`/Applications/snow_shot.app`) to a backup folder outside Applications before
running `brew install`. Keep the installer state directory and original Keychain
identity. Do not use `--force` to overwrite an existing app. Confirm that the new
copy launches and can capture before deleting the backup. If installation fails,
restore the backup to its original location. Use Homebrew for subsequent upgrades.

A custom destination is supported through Homebrew's standard option:

```sh
mkdir -p "$HOME/Applications"
brew install --cask --appdir="$HOME/Applications" mg-chao/tap/snow-shot
```

If signing fails, follow the identity recovery instructions below; do not delete
state to bypass a missing identity. Homebrew handles application rollback during
an upgrade. Its restored staged backup is signature-verified without requiring
the signing key again. Inspect its reported recovery result before retrying. Retain the
previous app backup and original identity until the installation is working.

### Installer staging interface

`install-snow-shot-macos.sh --dmg PATH --prepare-app /absolute/New.app` validates
and signs a local DMG into a fresh `.app` directory. The adjacent `PATH.sha256`
is mandatory. The destination's parent must already exist, and an existing output
or symlink is rejected. This mode saves the validated signing requirement but
does not close, replace, or launch any installed application. On failure it removes
only its own incomplete output. The standalone installer still installs normally
when `--prepare-app` is absent.

Focused checks:

```sh
python3 scripts/test-snow-shot-homebrew.py
python3 scripts/test-macos-installer.py
bash -n scripts/install-snow-shot-macos.sh
```

Native release qualification uses a disposable macOS desktop account with
Homebrew: install, launch, upgrade between two stable releases, uninstall, and
repeat with `--appdir`. Check that the signing identity stays the same, the OCR
helper and manifest bytes remain unchanged, and uninstall preserves user data.
In that disposable account, test an inaccessible signing identity and verify
that Homebrew restores the previous app after the failed upgrade. Restore the
identity and retry. This qualification is separate from the mocked regression
checks and requires real release assets and interactive Keychain/privacy consent.

## Install a packaged release (no build tools required)

The standalone installer supports macOS 15+, Apple Silicon (including a terminal
running under Rosetta), and Intel. It uses only macOS system tools. After the
release assets are published, choose a Snow Shot release tag and run its installer
asset as your normal desktop user:

```sh
curl --fail --location --proto '=https' --proto-redir '=https' \
  --output install-snow-shot-macos.sh \
  "https://github.com/mg-chao/snow-apps/releases/download/v<version>_snow-shot/install-snow-shot-macos.sh"
bash install-snow-shot-macos.sh --lang en
```

The installer verifies the downloaded package, creates or reuses a local signing
identity, gracefully closes Snow Shot, and installs `/Applications/Snow Shot.app`.
Existing `/Applications/snow_shot.app` installations migrate to the new name,
with the old path restored if installation fails.
It asks for administrator access only when installation requires it. Do not run
the whole command as root; an invocation through `sudo` hands execution back to
the desktop user. Existing app data and preferences are preserved. Failed
replacement checks restore the previous app; failed recovery prints the backup
directory instead of deleting it.

Language is detected automatically without `--lang`. Supported overrides are
`en`, `zh-CN`, and `zh-TW`. Use `--no-launch` to skip launch after installation, or
`--dmg "/path/to/package.dmg"` to use a local DMG with a required adjacent
`package.dmg.sha256`. Use `--help` for the complete interface. System diagnostics
and macOS authentication dialogs use the system's own language. Missing uploads,
invalid packages, and release API limits produce a nonzero exit without replacing
the installed application.

### 中文安装说明

无需安装 Homebrew、Python、Xcode 或购买 Apple 开发者会员。使用上面的 `curl`
命令下载安装脚本后，以日常桌面用户运行：

```sh
bash install-snow-shot-macos.sh --lang zh-CN
```

脚本会自动识别芯片架构，并行检查 GitHub 和 Gitee Releases，优先使用先完成验证的渠道，
验证后安装到 `/Applications/Snow Shot.app`（已有旧路径安装会迁移到新名称）。仅在替换应用需要管理员权限时请求密码，
无需在命令前添加 `sudo`。首次创建签名身份时，系统可能要求确认钥匙串访问或代码签名信任。
首次安装或从旧签名迁移后，仍需按系统提示授予屏幕录制和辅助功能权限。
以后请继续使用此脚本更新，以复用本地签名身份；跨 macOS 版本的权限保留尚未完成实机验证，
系统仍可能要求授权。脚本不会重置隐私权限，也不会关闭系统安全机制。

本地安装使用 `--dmg "/路径/安装包.dmg"`，并将对应的 `.dmg.sha256` 放在同一目录。
`--no-launch` 表示安装后不启动应用。失败时请根据诊断信息重试；若显示恢复目录，
请保留该目录中的 `previous.app`，并先恢复到安装器提示的安装路径。

### 繁體中文安裝說明

使用上面的 `curl` 命令下載指令碼後，以日常桌面使用者執行：

```sh
bash install-snow-shot-macos.sh --lang zh-TW
```

不需要 Homebrew、Python、Xcode 或 Apple 開發者會員。指令碼會自動辨識晶片架構，
並行檢查 GitHub 與 Gitee Releases，優先使用先完成驗證的管道，驗證後安裝到
`/Applications/Snow Shot.app`（既有舊路徑安裝會遷移至新名稱）。不必加上 `sudo`；需要管理員權限時才會要求密碼。
首次建立簽署身分時，系統可能要求確認鑰匙圈存取或程式碼簽署信任。首次安裝或從舊簽署遷移後，
請依系統提示授予螢幕錄製和輔助使用權限。之後請繼續使用此指令碼更新，以保留本機簽署身分；
跨 macOS 版本的權限保留尚未完成實機驗證，系統仍可能要求授權。
本機安裝使用 `--dmg "/路徑/安裝套件.dmg"`，並提供同目錄的 `.dmg.sha256`。
`--no-launch` 可略過啟動。若還原失敗，請保留顯示的目錄，先將其中的 `previous.app`
還原到安裝程式提示的安裝路徑再重試。

### Installer publishing contract

The local publisher includes `install-snow-shot-macos.sh`, a versioned DMG,
and its matching `.sha256` asset in the GitHub release when macOS packaging
is configured. The Gitee release workflow mirrors these exact bytes after
GitHub publication.

| Architecture | Release asset |
| --- | --- |
| Apple Silicon | `snow-shot-<version>-macos-arm64.dmg` |
| Intel | `snow-shot-<version>-macos-x86_64.dmg` |

Append `.sha256` for each checksum asset. A sidecar has exactly one nonempty
line beginning with the 64-character SHA-256 digest. The standalone installer
races the GitHub and Gitee release lists, includes published previews, and
ignores drafts. It selects the newest valid SemVer release within each channel.
A channel is ready only when it has exact architecture DMG and sidecar asset
names and release download URLs. The first ready channel provides the package;
if download or validation fails, the installer tries the same version on the
other channel. The checksum and DMG come from one channel over HTTPS.

The DMG contains `Snow Shot.app` at its root (the installer also accepts legacy
`snow_shot.app` packages), with bundle ID
`com.snowshot.snow_shot`, executable `snow_shot`, a minimum macOS version, and a
valid bundle signature (ad-hoc is supported). Publishing these assets and this
installer remains a separate release operation. Nothing is uploaded by the script.

### DMG installation window

The release DMG opens a branded Finder window with Snow Shot on the left, an
Applications shortcut on the right, and drag-to-install instructions in English,
Simplified Chinese, and Traditional Chinese. Finder backgrounds are static images,
so all three languages appear together rather than selecting the packaging
machine’s language. Native privacy prompts use macOS language selection. Drag the
app to Applications, eject the image, and open Snow Shot from Applications.
The bundle includes its icon, full release version, copyright, Productivity
category, supported languages, and localized macOS privacy prompts. The internal
executable and settings identifier remain `snow_shot`.

Packaging uses Finder through AppleScript to save the disk image layout. Run it
in a logged-in macOS desktop session; macOS may request permission for the
packaging terminal to control Finder. Deployment and ad-hoc signing happen before
the packaging hook renames the distributable bundle. Development build/run paths
remain unchanged. Developer ID signing and notarization are still separate release
steps.

### Signing identity, recovery, and qualification

The installer creates a dedicated ten-year self-signed code-signing certificate
and private key in the user's login Keychain. Trust is scoped to the user's
code-signing policy; private-key access is limited to `/usr/bin/codesign` and
normal Keychain authorization. Temporary private-key files are removed after
import or on failure. macOS requires a nonempty PKCS#12 import passphrase; the
installer generates a one-time random value, which the `security` CLI briefly
exposes as a process argument during import. State is stored under
`~/Library/Application Support/Snow Shot/Installer`; retain this directory and
the original Keychain identity across reinstalls. The installer signs only the
outer bundle/main executable, preserving embedded helper signatures and the OCR
manifest. It checks the saved designated requirement before replacing the app.

If signing fails, unlock the login Keychain and retry. If the saved identity is
missing or expired, restore the original certificate **and private key** from
your backup; the installer deliberately does not create a replacement. An
interrupted first-time identity creation leaves a `creating` marker. Inspect
Keychain Access for the `Snow Shot Local Installer ...` identity: restore its
user code-signing trust if needed, then save its 40-character SHA-1 fingerprint
(without colons) as the `identity` file in the state directory and remove the
`creating` marker. Do not change an existing identity fingerprint to another
certificate when expecting to retain permissions. A stale `lock` directory may
be removed only after confirming no installer is running.

If the original key cannot be recovered, explicitly move the state directory
aside before reinstalling. This creates a new identity and requires granting
permissions again. Users sharing a Mac should use the same installing account
for updates to the shared `/Applications` copy. The application updater and
Finder drag-and-drop do not reuse this installer's identity; use the script or
Homebrew for subsequent updates. Local signing does not provide Apple notarization or silently
grant privacy permissions. Only the validated staged copy is eligible for removal
of its quarantine attribute; Gatekeeper and TCC remain enabled.

中文恢复提示：签名失败时先解锁登录钥匙串。身份丢失时需要恢复原证书和私钥，不能仅重新生成。
请保留上述 Installer 状态目录。首次创建中断时，可在“钥匙串访问”中确认原身份，恢复其用户级
代码签名信任，并将原证书的 SHA-1 指纹（40 位、不含冒号）写入 `identity` 文件后移除
`creating` 标记。仅在确认安装器未运行后清理过期的 `lock` 目录。若无法恢复原私钥，
可以主动移走状态目录再安装，但这将创建新身份并需要重新授权。以后请使用同一用户运行脚本更新。

繁體中文還原提示：簽署失敗時先解鎖登入鑰匙圈。身分遺失時需要還原原憑證和私密金鑰，不能僅重新產生。
請保留上述 Installer 狀態目錄。首次建立中斷時，可在「鑰匙圈存取」中確認原身分，還原其使用者層級
程式碼簽署信任，並將原憑證的 SHA-1 指紋（40 位、不含冒號）寫入 `identity` 檔案後移除
`creating` 標記。僅在確認安裝程式未執行後清理過期的 `lock` 目錄。若無法還原原私密金鑰，
可以主動移走狀態目錄再安裝，但這將建立新身分並需要重新授權。之後請使用同一使用者執行指令碼更新。

Run only the focused installer checks for changes to this flow:

```sh
/bin/bash -n scripts/install-snow-shot-macos.sh
python3 scripts/test-macos-installer.py
```

**Permission retention is not yet qualified on supported macOS versions.** Mocked
regression tests do not prove TCC behavior. Before advertising it as reliable,
use a disposable macOS account: install build A, grant Accessibility and Screen
Recording, exercise both capabilities and OCR, then install a different build B
using the same identity. Verify those capabilities again after relaunch, along
with same-version reinstallation. Repeat on supported macOS versions and
architectures. Do not run this experiment against a developer's personal Keychain
or privacy grants.

## Prerequisites

- Xcode command-line tools (`xcode-select --install`) or Xcode, with a macOS 15+ SDK.
- Rust installed through rustup. The checked-in toolchain pins Rust 1.97.1.
- CMake 4.2+, Ninja, pkg-config, Git and Python 3. Intel codec builds also need NASM.
- The official **Qt 6.11.1 macOS** kit, including Qt SVG and Linguist tools,
  for Debug and performance builds. Release and fast builds use the repository's
  audited static Qt build for the selected architecture.

For example, install host tools with `brew install cmake ninja pkgconf nasm`.
The scripts also recognize tools already installed under `.tools/macos-dev/bin`
and `.tools/macos-media/host/bin`. They do not modify global tool installations.

Set `Qt6_DIR` if Qt is not at `$HOME/Qt/6.11.1/macos/lib/cmake/Qt6`:

```sh
export Qt6_DIR=/path/to/Qt/6.11.1/macos/lib/cmake/Qt6
scripts/bootstrap-macos.sh
scripts/build.sh
scripts/run-snow-shot.sh
```

Bootstrap checks the host tools, installs the matching Rust target, bootstraps
repository-local vcpkg at the registry baseline, and installs the selected
manifest dependency set. `build.sh` validates that setup before configuring and
automatically refreshes a stale CMake cache; pass `--skip-bootstrap` only after
provisioning the selected preset. `--clean` removes that preset's build tree,
matching the Windows wrapper, and `--parallel N` controls build concurrency.
The run script rebuilds the `snow_shot` target before launching by default; pass
`--no-build` to launch an existing build or `--clean` to request a clean rebuild.
CMake installs native dependencies from the manifest. The first run builds
FFmpeg, image codecs, OpenCV, and CPU ONNX Runtime and can take considerable time.
Dynamic and static dependencies live in `.tools/macos/installed/{dynamic,static}`,
isolated from Windows and the standalone macOS media harness. `LIBCLANG_PATH`
can override Xcode's libclang.

Provision a production static kit once per architecture. The first bootstrap
installs the static libpng/zlib packages used by Qt as well as the application
dependency closure:

```sh
scripts/bootstrap-macos.sh snow-shot-macos-arm64-release --skip-qt-validation
scripts/build-static-qt.sh \
  --arch arm64 \
  --install-prefix "$HOME/Qt/6.11.1/macos-static-arm64" \
  --parallel 8
export SNOW_QT_STATIC_DIR="$HOME/Qt/6.11.1/macos-static-arm64/lib/cmake/Qt6"
scripts/build.sh snow-shot-macos-arm64-release --parallel 8
```

Use `x64` and `macos-static-x64` for an Intel build. The static Qt script pins
the architecture and Qt's required 14.0 library deployment target (the Snow Shot
app still targets macOS 15.0), enables LTO and system libpng/zlib, installs Qt
source-license metadata, disables the macOS-27-only `dup3` path for compatibility
with the supported deployment range, and records an audited build stamp. It
reuses a matching installation; pass `--force` only when intentionally replacing
that prefix. Release and fast entry points reject a shared, unstamped, wrong-arch,
or wrong-version Qt kit.

The feature-policy fingerprint and installed Qt target exports are also checked
before reusing a production kit. Time-zone handling remains enabled; Cocoa
continues to provide native time-zone names, and Qt's optional CLDR name tables
remain disabled on Apple platforms. Use a distinct install prefix to preserve
an older kit when refreshing the audited schema.

## Presets and targeted checks

Names follow `snow-shot-macos-{arm64|x64}-{debug|performance|release|fast}`.
The default architecture follows the current shell's `uname -m` (including
Rosetta); pass a preset explicitly to select another architecture.

| Mode | CMake configuration | Tests | Benchmarks | Packaging |
| --- | --- | --- | --- | --- |
| debug | Debug | Enabled | Disabled | Disabled |
| performance | Release | Enabled | Enabled | Disabled |
| release | Static Release with LTO | Disabled | Disabled | Static DMG |
| fast | Static Release without app C++ LTO, Cargo release-fast | Disabled | Disabled | Disabled |

```sh
scripts/build.sh snow-shot-macos-arm64-debug --target snow-shot-ocr-layout-tests
ctest --preset test-snow-shot-macos-arm64-debug -R '^snow-shot-ocr-layout-tests$'
scripts/build.sh snow-shot-macos-arm64-fast --clean
scripts/build.sh snow-shot-macos-x64-release
scripts/run-snow-shot.sh snow-shot-macos-arm64-debug -- --help
python3 scripts/test-macos-build-support.py
# Optional native bundle/DMG fixture after provisioning Qt and FFmpeg:
SNOW_TEST_MACOS_BUNDLE=1 python3 scripts/test-macos-build-support.py MacOSBundle
```

The native deployment fixture tests the packaging rules with a small Qt executable
and a simulated versioned OCR library; it does not validate the full app.
It requires CMake, Ninja, pkg-config and CPack on PATH.

Only run tests covering your change. Test presets exclude Windows-only,
interactive, end-to-end and benchmark labels. Build a benchmark explicitly with
`--target` using the performance preset. Running an Intel executable on Apple
Silicon requires Rosetta. `build.sh` defaults to the application target; use
`--target snow-all` only when all enabled targets are needed. On Apple Silicon,
the default application build includes both Snow Shot editions.

For direct CMake use, put the host tools on PATH and supply `-DQt6_DIR=...`:
`cmake --preset snow-shot-macos-arm64-debug`, then
`cmake --build --preset build-snow-shot-macos-arm64-debug`.
The shell wrapper also sets Xcode libclang discovery for Rust bindgen.

Formatting and lint entry points are `scripts/check-cpp-format.sh [--fix]` and
`scripts/check-rust.sh PACKAGE [-- CARGO_OPTIONS...]`. The `snow-format` and
`snow-lint` CMake targets use native shell scripts on macOS; `snow-lint` checks
the CPU OCR helper. Other changed Rust crates should be linted individually.

## App bundles and packaging

```sh
scripts/package-snow-shot.sh snow-shot-macos-arm64-release
```

The release script builds and produces edition DMGs plus SHA-256 checksums under the
preset's build directory. Pass `--parallel N` to control the build or
`--skip-build` to package an already validated release cache. CPack installs only
the corresponding Snow Shot component and verifies the bundle signature. Static packages import
the Cocoa, macOS style, Secure Transport, SVG, and offscreen Qt plugins at link
time and reject every non-system Mach-O dependency. The bundle includes the OCR
helper, shutter audio, and project license notices; the full edition also includes
QR models. Executable-relative
asset lookup paths remain under `Contents/MacOS`; deployment moves data into
`Contents/Resources` and creates relative `assets`/`audios` directory links so
code signing seals them as resources. Static Qt and vcpkg source-license notices
are included under `Contents/Resources/snow-shot/licenses/third-party`. A plain
`cmake --install build/<preset> --component SnowShot --prefix /path/to/staging`
performs the same deployment and static-link audit without a DMG. Shared Debug
and performance deployments continue to put libraries in `Contents/Frameworks`.

Debug and performance presets automatically create `Snow Shot Development (Local)`,
a self-signed code-signing identity in the user's default keychain. The private key
is restricted to `/usr/bin/codesign`. CMake signs the raw app executable for direct
IDE debugging, and the run script uses the same certificate for its deployed copy.
Because the certificate remains stable across rebuilds, macOS can retain privacy
grants after the app's code changes. The certificate is only for local development;
it is not trusted for distribution. Release and fast presets remain ad-hoc signed.

To use another code-signing certificate from the keychain, pass its name or SHA-1:

```sh
scripts/run-snow-shot.sh snow-shot-macos-arm64-debug \
  --codesign-identity "Your code-signing certificate name or SHA-1"
```

The override applies to that run and is stored in the selected preset's CMake
cache. Later default runs restore the local development identity. Pass
`--codesign-identity -` to explicitly use ad-hoc signing for one run. A missing or
unusable override fails deployment; it does not fall back to ad-hoc signing.
Public distribution still requires Developer ID signing and Apple notarization;
this setting does not notarize the app.
The run script deploys a signed development copy under `build/<preset>/run`
before launching; this also resolves the OCR helper's dynamically loaded libraries.
Launch through the run script/Finder and grant Screen Recording, Accessibility,
and microphone permissions when using the corresponding capabilities.

### Accessibility enabled in Settings but reported missing

Ad-hoc signatures identify a particular build by its code hash. After rebuilding,
TCC can reject the old grant while System Settings still shows Snow Shot enabled.
The system log then reports `Failed to match existing code requirement` for
`com.snowshot.snow_shot` and `kTCCServiceAccessibility`.

Quit Snow Shot, remove its old entry from System Settings > Privacy & Security >
Accessibility with the minus button, then add and enable the exact deployed app:
`build/<preset>/run/snow_shot.app`. Restart that copy with
`scripts/run-snow-shot.sh <preset> --no-build`. This option launches the existing
deployment without rebuilding, reinstalling, or signing it again. The raw app
under `build/<preset>/snow_shot` has a different signature and is not the copy
launched by the run script. Ad-hoc builds may need this recovery again after a
rebuild; use the run script's local signing identity for subsequent builds to
avoid that identity change. Switching from ad-hoc to certificate signing requires
granting permission to the new identity once; later rebuilds retain that grant.

Apple Silicon OCR uses native CPU inference with all seven existing V4/V5/V6
models. Both ARM64 editions bundle their worker and ONNX Runtime. Full also
bundles Small V6; Mini bundles no model files. Missing selected models download
on demand into application storage. No OCR runtime code
is downloaded on macOS. Intel OCR qualification is outside this delivery.
The Windows updater/installer and DirectML remain Windows-only.

macOS diagnostics use the shared rotating JSON log service and a bundled Crashpad
handler. The handler captures native faults, stack overflow, fatal Qt errors,
C++ termination, and Rust panics. The OCR worker inherits the Mach exception
handler and adds its own role, session, and operation breadcrumbs. Reports stay
local; automatic uploads are disabled. Daily log export includes crash summaries,
while binary dumps remain in the `crashes` database under the selected log directory.
Permission transitions and display geometry/scaling changes are recorded without
hardware serial numbers or captured content.
Fatal/terminate/panic paths also write bounded emergency evidence independently
of the asynchronous log queue. Forced termination and power loss may leave only
the prior logs and an unexpected-session-termination marker.

Packaging builds `snow-shot-diagnostics-symbols`, placing UUID-matched application
and OCR worker dSYMs under `build/<preset>/symbols/`. Archive these beside each
release; they are deliberately outside the shipped app. The same target can be
built manually for development builds. The handler is deployed and signed with
the other nested executables before the outer bundle is sealed.
Run the focused diagnostics checks with
`ctest --test-dir build/<debug-preset> -R '^snow-shot-diagnostics(-crash)?-tests$' --output-on-failure`.
The crash fixtures use temporary storage and intentionally crash isolated children.
The standalone `bootstrap-macos-media.sh` and `build-macos-media.sh` workflows
remain available for media harness development.

## Apple Silicon OCR validation

The runtime uses protocol 4. Its generated schema-3 manifest records
`macos-arm64`, `delivery: bundled`, static linkage, the executable name, and the
size/SHA-256 of the worker. ONNX Runtime is linked into the worker in release
packages; shared development builds still stage `libonnxruntime.dylib`. The
existing Windows schema-2 manifest remains the
source of the seven pinned model contracts; its Windows runtime is never staged
on macOS. Runtime binaries live in `Contents/MacOS`; models and the manifest are
accessed through `Contents/MacOS/assets/ocr` (a resource link in signed bundles).
Development workers load their adjacent ONNX library by absolute path,
independent of the shell environment. A damaged runtime requires reinstalling
the ARM64 app; verified downloaded models remain reusable.

Build staging checks content on every relevant target build, including a worker-only
change followed by a host build. Model downloads are verified before promotion and
cached under `artifacts/`. Full packaging requires a complete Small V6 payload;
Mini staging removes previously bundled models and packaging rejects model files
or directories. Deployment
resolves the pinned native dependency closure, rewrites relocatable Mach-O loads,
and signs nested code, generates hashes of the finalized
runtime, then seals the enclosing app. CPack signs and verifies the DMG before
calculating its SHA-256 checksum. No runtime binary may be modified afterward.
The installer writes `macos-ocr-verification.json` in the build directory, listing
verified Mach-O binaries and runtime hashes. This is local integrity validation;
ad-hoc signing is not Developer ID signing or notarization.

Focused deterministic checks (the asset executable needs no worker or real models):

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-ocr-assets-tests
ctest --preset test-snow-shot-macos-arm64-performance -R '^snow-shot-ocr-assets-tests$'
python3 scripts/test-snow-shot-macos-ocr.py
scripts/check-rust.sh snow-ocr-process -- --no-default-features --features dynamic-onnx-runtime
scripts/check-rust.sh rapid-ocr-rs -- --no-default-features --features dynamic-onnx-runtime
```

Build the recognition test target and fetch all verified models for the opt-in
native checks. Use only the performance preset when passing `--measure`:

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-ocr-recognition-service-tests
ctest --preset test-snow-shot-macos-arm64-performance \
  -R '^snow-shot-ocr-(cpu-recognition-service|process-lifecycle|managed-runtime)-tests$'
python3 scripts/snow-shot-macos-ocr.py fetch-models --model-root build/ocr-versioned-models
export SNOW_TEST_OCR_TEXT_FIXTURE="$PWD/snow_shot/tests/baselines/ocr-model-versions.png"
# Use the recognition test executable emitted by this build:
OCR_TEST="$PWD/build/snow-shot-macos-arm64-performance/snow_shot/test-bin/snow-shot-ocr-recognition-service-tests"
"$OCR_TEST" --model-root="$PWD/build/ocr-versioned-models" --measure
"$OCR_TEST" --resident-text-fixture
"$OCR_TEST" --model-root="$PWD/build/ocr-versioned-models" --resident-models
```

The seven-model run checks bilingual text, geometry, FIFO callbacks and repeated
inference. Measurements report the first cold request, the following request using
the same session, and the worker's resident bytes sampled after each result (not
peak RSS). Cold means a new worker/model session, without flushing the operating
system's file cache. They are hardware-dependent observations, not a latency guarantee.

For a shared performance deployment, relocate the app to a path containing
spaces or Unicode and test the exact bundle with the matching shared-runtime
recognition helper, without modifying its signature:

```sh
"$OCR_TEST" --bundle="/path/to/Relocated Snow Shot.app" --offline
"$OCR_TEST" --bundle="/path/to/Relocated Snow Shot.app" --model=extra_small --cache="/tmp/snow-ocr-cache"
"$OCR_TEST" --bundle="/path/to/Relocated Snow Shot.app" --model=extra_small --cache="/tmp/snow-ocr-cache" --offline
```

For a static Release package, verify its finalized runtime and startup separately:

```sh
python3 scripts/snow-shot-macos-ocr.py verify \
  --static-runtime \
  --runtime-dir="/path/to/Relocated Snow Shot.app/Contents/MacOS" \
  --app="/path/to/Relocated Snow Shot.app"
"/path/to/Relocated Snow Shot.app/Contents/MacOS/snow_shot" --startup-probe
```

The recognition helper's first run uses a fresh temporary cache and an unreachable
download proxy. The second acquires another model; the third proves cache reuse
without network access.
Run with `DYLD_LIBRARY_PATH`, `DYLD_FALLBACK_LIBRARY_PATH`, and `ORT_DYLIB_PATH` unset.
Also launch the packaged app through Finder and check its screenshot-to-OCR flow.

For Mini, add `--runtime-only` to the Python verification command and use
`Snow Shot Mini.app` / `snow_shot_mini` for the app and executable paths. A fresh Mini
cache needs the selected model download before offline recognition can work;
repeat recognition with the same cache to verify offline reuse.

## Screenshots

See [macOS screenshot support and validation](snow_shot/tests/macos_screenshots.md)
for supported workflows, point-based editing, mixed-display export sizing,
permission recovery, targeted checks and native hardware acceptance steps.
Screenshot capture uses ScreenCaptureKit; Windows-only backend preferences remain
stored but are not used on macOS.

See [macOS cursor coordination](snow_shot/tests/macos_cursors.md) for the shared
screenshot, recording and pinned-window cursor contract and the native checks
required when upgrading Qt or macOS.

## Pin to Screen

See [macOS pinned-window validation](snow_shot/tests/macos_pinned_windows.md) for
placement semantics, targeted tests, and the hardware qualification checklist.
Pinned windows use logical desktop pixels for position, size, interaction, and
restore. A 300×200 selection stays 300×200 on both Retina and non-Retina displays;
its full-resolution image remains independent of window geometry. Pins and their
controls join all Spaces. The unreleased format 2 schema in `pinned_windows_v2`
stores explicit geometry units and window sizes; earlier development schemas are
not migrated.

## Launch at login

A signed Snow Shot app installed under `/Applications` or `~/Applications`
registers itself with macOS on its first eligible normal launch, unless its saved
startup preference is disabled. Development bundles, disk images, translocated
copies, and unsigned or invalidly signed apps cannot register. Moving an eligible
signed copy into Applications allows its initial registration attempt.

Use **Settings > System > General > Launch at login** to enable or disable it.
If approval is required, choose **Open Login Items Settings** and allow Snow Shot
in **System Settings > General > Login Items** (the panel name can vary by macOS
version). Registration awaiting approval is shown as requested, with an explicit
approval hint; it does not mean macOS will launch the app yet. Turning the setting
off also removes a pending registration and does not quit the running app.

Snow Shot observes changes made in System Settings when its settings page opens
or the app becomes active. It does not re-register on later launches after you
remove the item or revoke approval. Resetting the General section explicitly
requests the enabled default again. Login startup stays in the background, but
missing required permissions still open the existing permission guide.

The one-time initialization marker lives in the current user's Qt generic
configuration directory as `SnowShot/macos-login-item.ini`, separate from exported
Snow Shot preferences. A failed automatic registration is not retried on every
launch; use the toggle to retry after addressing the displayed error. A preference
write failure does not undo a successful native change: the setting continues to
show the actual macOS status. Stable bundle identity and signing are important
when replacing or updating an installed app.

### Manual launch-at-login acceptance check

Use a signed app in Applications and record its original login-item state before
checking. Automated tests use injected operations and do not register login items.

1. On a fresh per-user initialization state, launch the installed app; confirm one
   Snow Shot entry appears in Login Items. Check the approval hint if macOS requires
   approval, approve it, then return to Snow Shot and verify the refreshed state.
2. Disable and re-enable through Snow Shot. Disable or remove it in System Settings,
   return to Snow Shot, then quit and reopen it; confirm it stays disabled or pending
   approval instead of re-registering automatically.
3. With launch at login enabled, log out and back in. Confirm one running instance,
   the menu bar icon, and no main window when permissions are granted. With a required
   permission missing, confirm the permission guide still opens. Manually reopen from
   Finder or the Dock and confirm the main window opens.
4. Replace the app with another build signed with the same identity, then verify its
   state and login behavior again. Test a copy outside Applications and confirm the
   disabled setting explains where to install it. Restore the original login-item
   state when finished.
