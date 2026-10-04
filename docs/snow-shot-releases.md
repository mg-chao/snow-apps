# Snow Shot releases and updates

Approved specification: the user accepted the implementation plan on 2026-09-10.
Priorities: data preservation and authenticity > recovery > testability > maintainability.

| Requirement | Contract | Verification |
| --- | --- | --- |
| U1 | One SemVer feed, signed metadata, no automatic downgrade | update contract tests |
| U2 | Background check/download; explicit restart; preserve active work | update service and About tests |
| U3 | Apply only verified owned files; journal, probe, recover | update transaction tests |
| R1 | Audited local Windows/macOS packages published directly to GitHub and Gitee | publisher tests |
| R2 | Private local signing key; immutable verified release assets | signing and publisher tests |

The first updater release is `1.0.0-beta`. Binaries without an updater require one manual
installation/replacement. This release introduces no user-data migration, delta patches,
macOS in-app installation, or OCR model-host deployment.

## GitHub and Gitee release channels

The Windows updater starts GitHub and Gitee release discovery together. Each channel
searches at most ten pages of 100 published releases, accepts previews, ignores drafts,
and considers releases in descending SemVer order. A channel becomes eligible only after
the release tag, exact asset URL, signed `latest-version.json`, and signed version have
passed validation. The first eligible channel wins, even if the other channel later
reports a newer version. An update is offered only when the winning version is newer
than the installed version.

The selected channel is saved with the authenticated release. Existing cached
`githubSource` state migrates to GitHub. Downloads use the selected channel first;
after a package failure, the updater looks for the same release version on the other
channel and verifies the signed package size and SHA-256. Resume data is bound to the
package URL, so a source change starts a fresh transfer. A failed mirror never changes
the published GitHub release.

The macOS Qt updater races the same release APIs. It requires an exact architecture DMG
and matching `.sha256` asset before offering the update; About opens the winning
release page. The standalone installer races both lists and downloads the chosen DMG
and checksum, with same-version package fallback. The app does not install macOS
updates in-app.

The local publisher signs and audits all Windows packages, optionally packages a
macOS DMG and standalone installer, publishes GitHub assets, verifies their bytes,
then publishes the release. With `-DeployWebsite`, it then runs the separate official
website workflow after both release destinations are published and verified.
Stable releases become GitHub's latest; preview releases remain marked as previews.
Identical retries reuse existing assets; conflicting bytes fail without overwrite.

```powershell
& scripts/publish-snow-shot-release.ps1 -SigningKeyPath C:/private/snow-shot-release/private.pem -ReleaseNotesPath artifacts/release-notes.md
& scripts/publish-snow-shot-release.ps1 -Operation Verify
```

Configure `GITEE_TOKEN` locally with Gitee tag and release write access. The publisher
reads the process environment, then the Windows user/system environment when the
running shell predates the setting. Never paste tokens into release notes, command
arguments, or checked-in configuration. Optional `GITEE_USERNAME` defaults to `mg-chao`.

The default publisher authenticates Gitee before building. After the compiled updater
audits the signed packages, it saves `artifacts/publish-*/local-release.json`, prepares
the reproducible Homebrew archive from the local DMG and tagged source, and publishes
the same local files and detailed notes to both destinations. Gitee uploads run on
the local machine; there is no GitHub release synchronization workflow. Main branch
source mirroring remains separate. `-SkipGitee` explicitly selects GitHub-only
publication; `-AuditOnly` and `-WhatIf` do not contact Gitee or require a token.

Gitee API reads use the local publication token to avoid anonymous rate limits.
Transient HTTP 502/503/504 read failures receive at most three short retries;
upload POST requests are never automatically repeated after an ambiguous response.
Gitee retries verify all existing bytes before filling missing assets, upload signed
metadata last, and download each attachment to check its size and SHA-256. Conflicting
bytes, source tags, titles, or notes fail without overwrite. Resume an interrupted
publication with its saved local manifest, without rebuilding or downloading GitHub
packages:

```powershell
python scripts/publish-snow-shot-gitee-release.py --manifest artifacts/publish-TRANSACTION/local-release.json --auditor build/snow-shot-msvc-release/snow_shot/Release/snow-shot-updater.exe
python scripts/publish-snow-shot-gitee-release.py --manifest artifacts/publish-TRANSACTION/local-release.json --auditor build/snow-shot-msvc-release/snow_shot/Release/snow-shot-updater.exe --verify-only
```

`--verify-only` requires no token and performs no uploads; it uses a locally configured
token when available for authenticated reads. Keep the local manifest and
all referenced files until both public release pages and asset downloads are verified.

Gitee retains the newest Snow Shot release. After a publication passes all attachment
checks, the publisher deletes older published Snow Shot release entries and their
attachments. SemVer ordering protects the current version and any newer version;
drafts and unrelated releases are excluded. Source Git tags and GitHub releases are
retained. Verification-only runs never delete releases.

If an attachment upload is explicitly rejected for exceeding Gitee's repository
attachment quota, the publisher first checks whether that attachment was added. If
it is still missing, it removes older Gitee releases and retries only that upload
once. This exception runs only after the new release entry and its existing bytes
have passed validation; the main workflow has already published and verified GitHub.
Ambiguous upload failures do not trigger deletion or automatic retries. Ambiguous
release deletions are reconciled by reading the exact release id before proceeding.

## Release contract

### Production Qt feature policy

Windows production Qt 6.12.0 LTS keeps time-zone handling and native Windows zone
mappings and daylight-saving rules, while omitting the bundled CLDR localized
time-zone display-name tables. The audited source patch preserves the native
registry's long names in the system language and supplies unambiguous UTC-offset
fallbacks for short and offset names in every requested locale. Numeric timestamps,
UTC storage, local conversions, and daylight-saving transitions retain their behavior.

`scripts/qt-toolchain.json` is the version and source-archive SHA256 contract for
CMake, Windows/macOS provisioning, and release CI. Both source builders verify
the archive before extraction. `scripts/build-static-qt.ps1` applies the patch
idempotently and installs it under the kit's source-license bundle. The build
stamp fingerprints the feature policy and patch contents. Bootstrap and packaging
verify the installed Qt version and feature exports as well as the stamp;
packaging also checks the installed patch hashes. Rebuild into a distinct
installation prefix when upgrading an older kit. All presets require Qt 6.12.0;
Debug source kits include Qt Test and Concurrent for the widget tests and image
viewer and use bundled codecs to match the Debug runtime. Windows bootstrap also
discovers kits under `.tools/qt/<version>/`.

For a Release development kit used by `windows-msvc-performance`, pass
`-DevelopmentModules` to `scripts/build-static-qt.ps1`. It enables Test and
Concurrent and can add them incrementally to an already validated production
kit. Performance discovery requires these modules. They are separate static
libraries and are not linked into the production Snow Shot executable.

macOS continues using Cocoa time-zone names; Qt already excludes these CLDR
tables on Apple platforms. Its static kit uses the same feature policy and
validates both its build stamp and installed exports.

The standalone `test-support/qt-timezone` fixture can be built against the exact
production kit to verify timestamp round trips, named zones, daylight-saving
boundaries, and name fallbacks without starting the application.

### Production size policy

Full and Mini Windows installers share `/SOLID lzma` compression with a 32 MiB
dictionary. Their deterministic update and portable ZIP archives use .NET
`CompressionLevel.SmallestSize`, preserving standard Deflate compatibility.

`SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION` is enabled by the shipping Release
presets and disabled in Debug, performance/benchmark, and fast presets. It
generates the Ant icon subset from application sources, public headers, drawing
code, and widget defaults under the build directory. Every theme of a referenced
icon is retained; pack enumeration or registration triggers a complete-pack
fallback for dynamic lookups. Changes to sources, headers, manifests, or SVGs
regenerate the subset without altering the checked-in library or public API.

Cold configuration, settings forms, import/export, and shell code use `/O1 /Os`
on MSVC or `-Os` on Clang. Capture, recording, drawing, image processing, history
thumbnails, clipboard decoding, and shortcut dispatch retain their speed policy.
Optimized Release targets keep LTO; MSVC `/Gw` and `/Gy` allow `/OPT:REF` and
`/OPT:ICF` to discard unused data/functions and fold identical code. macOS enables
`-dead_strip` only for Release builds with the shipping size option enabled.
The source and target policies are centralized in
`cmake/SnowShotReleaseOptimization.cmake` for both editions.

### Snow Shot Mini paired releases

Snow Shot and Snow Shot Mini are built from one CMake configuration and published
under the same `v<version>_snow-shot` tag. The `snow_shot` and `snow_shot_mini`
targets keep separate executable, helper, install, storage, and MCP identities.
`SNOW_APPS_BUILD_SNOW_SHOT_MINI=ON` enables Mini alongside the full edition in
Windows and macOS Apple Silicon presets; macOS Intel presets exclude Mini.
The `scripts/build.ps1` and `scripts/build.sh` wrappers select the enabled
editions automatically. After configuring with Mini `OFF`, override the paired
targets for a direct build with `cmake --build --preset build-<preset> --target
snow_shot`. Windows and ARM64 release packaging requires both editions.
`scripts/package-snow-shot.ps1` produces both Windows editions, and
`scripts/package-snow-shot.sh snow-shot-macos-arm64-release` produces both DMGs.

| Mini Windows asset | Purpose |
| --- | --- |
| `snow-shot-mini-<version>-windows-x64-online.exe` | Installer |
| `snow-shot-mini-<version>-windows-x64-online-update.zip` | Installed-copy update |
| `snow-shot-mini-<version>-windows-x64-portable.zip` | Portable install and update |

Windows Mini bundles only the trusted OCR asset manifest; manual text recognition
downloads the local OCR payload on demand. It has no offline installer. macOS
Mini supports Apple Silicon only and bundles its OCR worker and runtime in
`snow-shot-mini-<version>-macos-arm64.dmg`. It bundles no model files; selected
models download on demand into its verified application cache. Mini hides its
text-recognition toolbar button by default on both platforms and disables
automatic Pin to Screen recognition; users can enable manual text recognition
in Mini settings.

Mini's signed feed is `latest-version-mini.json`. Its payload requires
`product: "snow-shot-mini"` and exactly three package entries. Its updater rejects
the full product's feed or installation record; full-edition helpers continue to
accept historical full metadata without a product field. Mini ownership is stored
in `snow-shot-mini-installation.json`, its portable marker is
`bin/__mini_data_directory`, and its installed helpers are
`bin/snow-shot-mini-updater.exe` and `bin/snow-shot-mini-mcp.exe` beside
`bin/snow_shot_mini.exe`. Publish and verify both signed feeds together. Package
manager submissions use separate Mini identities (`mg-chao.snow-shot-mini`,
`snowshot-mini`, and `snow-shot-mini` Homebrew casks).

GitHub and Gitee publish versioned Windows installer and update assets, their checksum
sidecars and audit manifests, plus `latest-version.json`. When macOS packaging is
configured, the release also includes the versioned arm64 DMG, its `.sha256` sidecar,
and `install-snow-shot-macos.sh`. Signed metadata is uploaded last. The Windows
updater uses the signed JSON envelope and never fetches the website's legacy update
feed. The website homepage and unrelated application API remain configured separately.

The JSON envelope is `{schema:1,keyId,payload,signature}`. `payload` and `signature` are
Base64. The signature is RSA-3072/PSS/SHA-256 (32-byte salt) over the exact decoded UTF-8
payload bytes. The full-edition payload includes `schema`, strict SemVer `version`, ISO-8601
`publishedAt`, `platform: "windows-x64"`, and exactly five `packages`. Each package carries
`variant`, `kind`, fixed relative `path`, byte `size`, and lowercase hex `sha256`. Each ZIP
also carries an exhaustive `files` array of `{path,size,sha256}` entries. Installers are
for initial/manual installation; installed copies update through their corresponding ZIP.

Archive inventories require the application, updater, and installation record. The record
must contain the identical signed ownership list, excluding the record itself. Unknown ZIP
entries, missing/duplicate entries, links, Windows device names, traversal, case collisions,
overlapping paths, files outside the owned directories, and excessive sizes are rejected.
The limits are 8 MiB for metadata, 20,000 files and 4 GiB expanded payload per package.

## Application behavior and recovery

| Initial state/event | Result |
| --- | --- |
| Fresh install | Background download mode; short cache probe at startup, first check after 30 seconds |
| Completed automatic check | Updater exits; Snow Shot schedules the next check after 24 hours |
| Settings mode `manual` / `check` / `download` | No scheduled requests / metadata only / metadata and payload |
| Newer authenticated release | Select the same installation variant; download with bounded retries |
| Interrupted transfer with a strong ETag | Resume with Range and If-Range; verify full signed size/hash |
| Signature failure or changed same-version payload | Reject; do not apply or trust new state |
| Lower SemVer than the newest observed feed | Reject replay/downgrade |
| Check-only mode discovers a newer release | Show a system notification that opens About |
| Verified payload ready | Notify in tray and About; never restart without explicit confirmation |
| Capture/recording/export active or settings cannot flush | Refuse restart; check again at the final helper handoff |
| User declines elevation or the ready/go handshake fails | Keep the current app running |
| Apply interrupted or startup probe fails | Restore the previous owned payload from its journaled backup |
| Failed version offered automatically again | Suppress automatic apply readiness; explicit manual retry is possible |
| Helper watchdog fires after handoff | Do not start a possibly incomplete app; manual launch enters recovery |
| Development copy without installation metadata | Show updates unavailable; never infer an install layout |

The GPL-3.0-only Rust sidecar is an operation-scoped child. Snow Shot briefly launches it to
restore cached state at startup and launches a fresh child for each check, download, or apply
handoff; the child exits after reporting a stable result. A lightweight Qt timer schedules the
30-second startup check and subsequent 24-hour checks. Private inherited stdin/stdout pipes
carry a versioned, 64-KiB-bounded NDJSON protocol; stdout is reserved for frames and bounded
operational diagnostics use stderr. Rust owns release validation, network/cache policy, archive
processing, transactions, recovery, elevation, installer commands, and release audits. The
remaining C++ `snow_shot_updates` target owns process lifetime, scheduling, signal delivery, and
translated-error lookup.

For apply and recovery, the helper verifies the parent process's real executable path,
stages outside `bin`, and requests elevation only for a matching registered installation.
The installed service copies itself to a narrowly named temporary broker and exits before
its installed path is replaced. The broker remains under the original user's identity for
relaunch. Named pipes are random and restricted to the current user, Administrators, and
SYSTEM with medium-integrity and remote-client rejection.
The worker authenticates the broker before mutation and retains that connection for the
ready/go exchange and final success/failure report. The broker acknowledges the final report
before the worker exits. Neither side re-authenticates completion against the installed
helper, whose bytes may now belong to the next release (or a restored earlier release).
Only a validated ready/go exchange lets the app exit. The transaction then acquires a
per-install lock, extracts and checks the inventory, checks free space and user-file
collisions, persists backups and a journal, replaces owned files, updates matching uninstall
registration, and runs an isolated offscreen startup probe with temporary user storage.

The `bin/__data_directory` marker and its selected data location are preserved, as are
unowned files. A retained installation record allows the original uninstaller to remove
files added by future updates. This does not migrate user data. If a damaged disk, locked
file, or corrupted backup prevents rollback, the journal is retained and automatic relaunch
is stopped. Close other instances and repair with a verified installer; do not delete the
journal or backups before recovery. A process-termination test is not a simulation of
physical storage failure.

The Rust updater uses native platform TLS, Tokio/Reqwest, Serde, RSA/SHA-256, SemVer, and
ZIP/Deflate without linking Qt or minizip. Windows release packages use the static CRT and a
dedicated size profile with fat LTO, one codegen unit, aborting panics, overflow checks, and
external CodeView/PDB information. macOS uses the Qt release-discovery service
and opens the verified release page for manual installation.

## Operator setup and commands

### Scoop

Snow Shot targets Scoop's official [Extras bucket](https://github.com/ScoopInstaller/Extras).
The initial [package request](https://github.com/ScoopInstaller/Extras/issues/18870)
includes the verified 1.1.8 manifest and is awaiting maintainer acceptance.
After upstream accepts the package, on Windows x64 with Scoop installed:

```powershell
scoop bucket add extras
scoop install extras/snowshot
scoop update
scoop update snowshot
scoop uninstall snowshot
```

The package installs the portable ZIP, including default OCR resources, without an
administrator installer. Launch **Snow Shot** from the Start Menu or run `snowshot`.
The `snowshot` package follows stable releases. Drafts, GitHub prereleases, beta
tags, other Snow Apps releases, and releases without a Windows x64 portable ZIP
are excluded.

Quit Snow Shot and finish recordings before upgrading or uninstalling. Use Scoop
for updates instead of Snow Shot's built-in updater: the app's updater remains
available, but using it can leave Scoop's recorded version out of sync with the
installed files. Existing manual and WinGet installations are not migrated.

Scoop preserves `bin\portable`, selected by the archive's `bin\__data_directory`
marker, under `$env:SCOOP\persist\snowshot\bin\portable` (normally
`$HOME\scoop\persist\snowshot\bin\portable`). Settings and history survive upgrades
and ordinary uninstall. To deliberately delete this persisted data, use
`scoop uninstall snowshot --purge` while the package is installed. Reinstall first
if it was already uninstalled. Back up any wanted data before purging. Custom
data directories selected by the user are outside Scoop's persistence management.

#### Migrating from the retired bucket

The self-maintained `snow-apps` bucket has been removed. Once `extras/snowshot`
is available, quit Snow Shot and switch an existing Scoop installation with:

```powershell
scoop bucket add extras
scoop update
scoop info extras/snowshot
scoop uninstall snowshot
scoop install extras/snowshot
scoop bucket rm snow-apps
```

Confirm the official package is available before uninstalling. Ordinary uninstall
retains the existing `persist\snowshot\bin\portable` data, and reinstalling the
same package name reconnects it. Do not use `--purge` when migrating. Removing a
bucket alone does not change the source recorded for an installed package.

#### Official bucket submission and updates

The manifest belongs in `ScoopInstaller/Extras/bucket/snowshot.json`; this
repository no longer publishes a Scoop bucket. Generate a manifest from a stable,
published GitHub portable ZIP using `v<version>` or `v<version>_snow-shot`. The
generator downloads the ZIP, checks its size, computes SHA-256, and validates its
executable layout and portable marker.

At least one published checksum is required: GitHub's SHA-256 asset digest or the
`<portable.zip>.sha256` sidecar. Every available source must match. The sidecar
must name the exact archive. Stable releases also publish the checksum sidecar
used by Scoop's upstream updater. No mutable download URLs or placeholder hashes
are used.

For the initial submission, follow Scoop's
[contribution process](https://github.com/ScoopInstaller/.github/blob/main/.github/CONTRIBUTING.md):
open a package request in Extras, obtain maintainer acceptance, then add the
verified manifest in a pull request from an Extras fork. Use the title
`snowshot: Add version <version>`, reference the request, and ask Scoop's verifier
to check the PR with `/verify`. Package availability depends on upstream review
and merge.

The generated manifest includes `checkver` and `autoupdate` for Scoop's official
update automation. Version discovery reads the latest 100 GitHub releases,
filters eligible stable Snow Shot portable assets, and sorts numeric versions.
The captured tag preserves the optional `_snow-shot` suffix in the download URL.
Autoupdate reads the corresponding `<portable.zip>.sha256` sidecar. Publish a
higher version for corrections to immutable release assets. Once merged, the
official bucket maintains updates; no local manifest commits, publishing token,
or release-triggered bucket workflow is required.

The **Snow Shot Scoop** workflow verifies Scoop-related changes and supports
manual dispatch to produce a verified submission artifact for a stable release.
It uses only Contents read permission. The generated manifest is retained as the
`snow-shot-scoop-manifest` workflow artifact when generation succeeds, including
when later schema validation fails. It does not publish releases or write to a
bucket. Update the upstream manifest through a reviewed Extras PR when its install
contract or version-discovery rules change.

Local generation and focused checks require PowerShell 7:

```powershell
./scripts/new-snow-shot-scoop-manifest.ps1 -Tag v1.1.8_snow-shot
./scripts/test-snow-shot-scoop.ps1
```

The generator defaults to `build/scoop/snowshot.json`; use `-OutputPath` to choose
another file. Review generated changes before submitting them to Extras.
For schema validation and the Windows lifecycle smoke test, check out
`ScoopInstaller/Scoop` at `b588a06e41d920d2123ec70aee682bae14935939` under
`build/scoop-runtime`, then run:

```powershell
./scripts/test-snow-shot-scoop.ps1 -SchemaPath build/scoop-runtime/schema.json
./scripts/test-snow-shot-scoop-install.ps1 -ScoopRoot build/scoop-runtime
```

CI runs these checks for Scoop-related changes and manual manifest generation.
Fixtures cover release validation, checksums, archive layout, upstream version
discovery, download URL substitution, and repeat runs. The smoke test exercises
Scoop's real install and removal functions with
two locally cached fixture packages, checks shortcut/shim targets, and verifies
settings/history persistence through upgrade and ordinary uninstall. It also
reinstalls the package after uninstall to verify retained data reconnects
when migrating buckets. It redirects
Start Menu and PATH integration into an isolated process/directory, never launches
Snow Shot, and does not modify an existing Scoop installation. It tests lifecycle
functions rather than the complete interactive Scoop CLI.

### WinGet

The independent **Snow Shot WinGet** GitHub Actions workflow submits the offline
Windows x64 installer as `mg-chao.snow-shot` to `microsoft/winget-pkgs`. Stable and
beta releases share this identifier; published betas are eligible even when GitHub
does not mark them as prereleases. Draft releases are never submitted.

Reuse the existing community identifier `mg-chao.snow-shot`; `1.1.5-beta` is already
published upstream. Do not introduce a second identifier for the same application.

The workflow runs on `release: published`, or manually with a published tag such as
`v1.1.5-beta` or `v1.1.5-beta_snow-shot`. It reads automation from the default branch
so existing release tags can be backfilled after this support is merged. Tags and
installer filenames must agree. Manifests reference versioned GitHub release assets,
not the website's mutable `/setup/` URLs. Do not replace an asset after submission;
publish a new version instead.

Maintainer setup:

1. Use a GitHub account with a fork of `microsoft/winget-pkgs` (WinGetCreate can also
   create the fork). Complete any upstream contributor requirements when prompted.
2. Create a **classic** personal access token with `public_repo` scope. Fine-grained
   tokens are not supported by WinGetCreate. Store it as the repository Actions secret
   `WINGET_CREATE_GITHUB_TOKEN`; the default Actions token cannot submit cross-repository PRs.
3. Publish a release or manually run **Snow Shot WinGet** with its tag. The workflow
   uploads manifests before validation/submission, validates them using WinGet, and
   submits with WinGetCreate 1.12.13.0 (verified against its pinned SHA-256).
4. Follow the upstream PR through validation and review. Submission does not imply
   acceptance or immediate availability in the community source.

Missing credentials fail with setup instructions and leave the manifest artifact
available. Retry the workflow after correcting credentials or validation errors.
Runs are serialized per version across both tag styles. Already merged versions and
matching open PRs are reported and skipped; closed, unmerged submissions can be retried.
GitHub lookup failures stop submission instead of treating a failed lookup as absence.
This workflow does not publish application releases.

For local generation and validation (PowerShell 7, WinGet 1.29.380 or newer, and
WinGetCreate 1.12.13.0):

```powershell
$tag = 'v1.1.5-beta'
$manifests = ./scripts/new-snow-shot-winget-manifest.ps1 -Tag $tag
winget validate --manifest $manifests --disable-interactivity
if ($LASTEXITCODE -ne 0) { throw 'Manifest validation failed.' }
# Set WINGET_CREATE_GITHUB_TOKEN through your local secret manager; never commit it.
./scripts/submit-snow-shot-winget.ps1 -Tag $tag -ManifestDirectory $manifests
```

The generator accepts `-OutputDirectory` and defaults to ignored `build/winget`.
`GH_TOKEN` optionally authenticates release metadata and duplicate lookups. The submission
token is read from the environment, never passed as a command-line argument. Generated
manifests use schema 1.12.0, preserve the release version including beta suffixes, and
calculate SHA-256 from the downloaded offline installer.

The existing community package can be installed and updated now:

```powershell
winget install --exact --id mg-chao.snow-shot --source winget
winget upgrade --exact --id mg-chao.snow-shot --source winget
winget uninstall --exact --id mg-chao.snow-shot --source winget
```

Installation is machine-wide and requires elevation. Newly built silent installers
(`winget upgrade --silent` or the installer executable with `/S`) close the affected
Snow Shot installation, install the update, and restart it on the interactive desktop
after successful installation. Closing may interrupt tasks and discard unsaved screenshots
or annotations. Fresh silent installations and upgrades of an already stopped app do not
launch it. Interactive setup still asks before closing; close Snow Shot before a standalone
silent uninstall. Installer exit code 10 maps to WinGet's `packageInUse` response.
The existing in-app updater remains enabled and updates the uninstall registration.
WinGet uses that registration to identify the installed version.

Focused verification:

```powershell
./scripts/test-snow-shot-winget.ps1
./scripts/test-snow-shot-installer-directory.ps1
./scripts/test-snow-shot-installer.ps1
```

Before submitting a new version, use a disposable Windows VM for the real package:
enable local manifests with `winget settings --enable LocalManifestFiles`, install with
`winget install --manifest <manifest-directory> --silent`, and confirm detection with
`winget list --exact --id mg-chao.snow-shot`. Install an older version first to exercise
an upgrade, including a custom installation directory and a user-settings sentinel.
Confirm the version changes, directory/settings survive, a fresh silent install does not
launch the app, and an upgrade of a running app closes and restarts it after installing.
Finally uninstall silently and verify owned files/registration are removed and user data
is preserved. Fixture tests and manifest validation do not substitute for this VM check.

The **Snow Shot WinGet verification** workflow automates this lifecycle on a disposable
GitHub-hosted Windows runner. It runs installation checks for WinGet changes in pull
requests; manual runs accept `tag`, `previous_tag`, and `running_app_behavior`. Choose
`Restart` for releases containing the silent-upgrade restart change; the default `Refuse`
keeps the immutable historical release fixtures valid. The underlying verification script
accepts the same expectation with `-RunningAppBehavior Restart`. Pushes to `codex/winget-*`
preparation branches run fixture and credential checks only, avoiding duplicate installs.
Installation checks need no submission token and never open upstream PRs. Trusted
preparation-branch pushes and manual runs also perform a read-only check of the
submission token's scope and fork access; pull requests skip that credential check.
Both workflows provision WinGet 1.29.380 from Microsoft's signed release bundle when
the installed client is older, avoiding the incomplete preinstalled runner bundle.
The default installation fixtures are
`v1.1.5-beta` and `v1.1.4-beta`; select newer published versions when validating later
releases. Logs and generated manifests are retained in the
`snow-shot-winget-verification` workflow artifact. The underlying
`scripts/test-snow-shot-winget-install.ps1` refuses to run outside a GitHub-hosted
Windows runner or when Snow Shot is already installed.

The published beta installers are unsigned and may trigger a SmartScreen reputation
prompt when testing local manifests. The verification workflow explicitly enables
`-AllowUnrecognizedRelease`: after validating the release manifests, the test temporarily
disables SmartScreen reputation checks only inside its disposable VM and restores the
prior policy in its cleanup path. WinGet's SHA-256 verification and antivirus scanning
remain enabled. Without this switch, an interactive launch prompt fails the test and is
captured in the diagnostic artifact. This consent is separate from the installer's
silent-mode checks and does not guarantee SmartScreen reputation on end-user PCs.
The test also acknowledges Windows' standard file-launch warning only after matching
the displayed installer filename and rechecking the cached executable's SHA-256.

The immutable `1.1.5-beta` installer does not register `QuietUninstallString`, so
`winget uninstall --silent` can still display its wizard. Use the normal interactive
uninstall for that release. Newly built installers register the quoted `/S` command;
the hosted CPack fixture test verifies silent removal through WinGet and preservation
of unowned files. The historical-release lifecycle test invokes the legacy NSIS `/S`
uninstaller directly rather than claiming its missing registration is supported.

### Publisher prerequisites

Use PowerShell 7, the documented Windows release toolchain, an authenticated GitHub
CLI with Contents write permission, and a private release signing key outside Git.
Copy `scripts/publish-snow-shot-release.local.example.ps1` to the ignored
`scripts/publish-snow-shot-release.local.ps1` and set your local key path.
The release tag must point to the local source commit. The publisher verifies the
pinned public OCR runtime before packaging and audits each signed Windows package.

### Coordinated Windows and macOS packaging

Set `MacHost`, `MacUser`, and `MacProjectDirectory` to package arm64 on a
provisioned Mac alongside Windows. `MacPort` defaults to 22;
`MacIdentityFile` and `MacKnownHostsFile` are optional. The Mac checkout must
have the same source version. The packaging worker verifies the DMG and its code
signature, then transfers the DMG and checksum. The publisher adds
`install-snow-shot-macos.sh` as a GitHub release asset. `-SkipBuild` reuses
audited package bytes and the Mac source receipt; it does not bypass audits.

```powershell
& scripts/publish-snow-shot-release.local.ps1 -WhatIf
& scripts/publish-snow-shot-release.local.ps1
& scripts/publish-snow-shot-release.local.ps1 -SkipBuild
& scripts/publish-snow-shot-release.local.ps1 -SkipBuild -AuditOnly
& scripts/publish-snow-shot-release.local.ps1 -Operation Verify
```

`-WhatIf` lists the planned assets without building or publishing.
`-AuditOnly` signs and audits without upload. `Verify` downloads published
GitHub assets and checks the signed Windows packages and any macOS checksum pairs.
A corrective release uses a higher version; published assets are immutable.

## Official website deployment

After publishing the release, use the website workflow separately or pass
`-DeployWebsite` to the release publisher:

```powershell
& scripts/publish-snow-shot-website.ps1
& scripts/publish-snow-shot-release.local.ps1 -ReleaseNotesPath artifacts/release-notes.md -DeployWebsite
```

The target comes from `SNOW_SHOT_VERSION` and must match any explicit `-Version`.
`WebsiteDirectory` defaults to `D:/snow-apps-site`. The website checkout must be clean
and contain `scripts/publish-release.ps1`. That workflow updates the download version,
commits any version change, pushes and verifies the website branch, then builds and
deploys to `root@120.79.232.67:/var/www/html` using local OpenSSH settings. A push
failure stops before building or deploying. Matching versions reuse their existing
commit. `Verify`, `AuditOnly`, and `WhatIf` do not deploy.

Only Rspress pages, icons, `static/`, `images/`, and `zh/` are replaced; `setup/`,
`npm/`, `plugins/`, legacy update feeds, and other server files are preserved.
SHA-256 checks cover the upload and every built file. Previous website files remain
under `/var/www/snow-shot-website-backups/`; replacement errors restore those files.
Public English/Chinese pages and `/website-release.json` are checked after deployment.
For a website-only retry, rerun `publish-snow-shot-website.ps1` without republishing
release packages. Server settings can be overridden on that command. Focused website
workflow tests and their commands are documented in the website repository README.

### Snow Shot Mini website downloads

The official website deploys both editions in one operation. Publish and verify
the paired release on GitHub and Gitee first, including Mini's online installer,
portable archive, and Apple Silicon DMG listed above. Both editions share
`SNOW_SHOT_VERSION` and `v<version>_snow-shot`; Mini needs no separate deployment
command or version setting.

The website's Windows Mini card links to
`snow-shot-mini-<version>-windows-x64-online.exe` and offers
`snow-shot-mini-<version>-windows-x64-portable.zip` as a second download.
The macOS Mini card links to `snow-shot-mini-<version>-macos-arm64.dmg`.
English uses GitHub download URLs and Chinese uses Gitee. Mini cards use `#f759ab`
and describe the lighter feature set; no Mini offline installer or Intel DMG is
advertised.

Commit the website changes in `D:/snow-apps-site`, then run
`scripts/publish-snow-shot-website.ps1` or the publisher with `-DeployWebsite`.
The website workflow synchronizes both editions' links through its single
`releaseVersion`, checks the generated Windows download anchors before packing
and before replacing server files, then checks the public English and Chinese
download anchors against the target release. The platform-specific component
tests cover both macOS DMG links. Mini's existing `setup/` files and
`latest-version-mini.json` are outside the website replacement set and remain
intact. Website-only retries use the same entry point and do not republish packages.

## Signing keys and rotation

`scripts/new-snow-shot-release-key.ps1` generates an RSA-3072 key outside the repository and
the public trust JSON used at compile time. Back up the private key securely and separately
from Git; losing it prevents releases to installed clients trusting only that key. The
generator refuses to overwrite a key. Restrict access to the key directory and prefer a
dedicated release account. Never pass private key contents on the command line or print them.

For rotation, release a bridge version signed by the old key that embeds both public keys,
then sign later releases with the new key after the supported client population has reached
that bridge. With this single-signature, single-latest-feed contract, clients that miss the
bridge need a manual verified installation after signer cutover. Merely keeping the old
public key in new binaries does not upgrade those clients' trust stores. Keep signing the
feed with the old key until the chosen support cutoff. A compromised sole trusted key requires a separately trusted
manual distribution path; HTTPS alone does not repair that trust.

Manifest authenticity is separate from Windows Authenticode. This feature does not add an
Authenticode certificate, remove SmartScreen prompts, or establish installer reputation.

## Focused validation and release gates

```powershell
python scripts/test-snow-shot-gitee-release.py
node scripts/test-macos-installer-parser.js
& scripts/test-snow-shot-github-release.ps1
& scripts/test-remote-macos-release.ps1
cargo test --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml --lib service::tests
cargo test --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml --lib github::tests
cargo test --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml --lib gitee::tests
cargo clippy --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml --all-targets -- -D warnings
cmake --build --preset build-windows-msvc-debug --target snow-shot-macos-update-tests snow-shot-update-adapter-tests snow-shot-about-page-tests snow-shot-settings-catalog-tests
ctest --preset test-windows-msvc-debug -R '^snow-shot-(macos-update|update-adapter|about-page|settings-catalog)-tests$'
```

Run `python3 scripts/test-macos-installer.py` on macOS for the standalone
installer's native JXA and package validation checks. The full CTest suite is
not part of this release change. Public rollout requires a configured
`GITEE_TOKEN`, a successful mirror for a published Snow Shot release, and
verification of both channels' asset bytes.

## macOS version checks

macOS offers **Manual** and **Check automatically**, defaulting to automatic
checks. Automatic checks start 30 seconds after launch and repeat 24 hours
after completion; About can trigger a manual check. The service starts GitHub
and Gitee release requests together, accepts published previews, and ignores
drafts. Each release must have a versioned DMG and checksum asset for the
machine architecture with exact release download URLs. Within a channel it
selects the newest valid SemVer release; the first validated channel wins.
A newer winning version produces an About download action for that release
host and a once-per-session automatic notification. Background failures
remain quiet; manual failures offer retry. macOS does not install updates
in-app.

## Homebrew tap publication

The separate `snow-shot-homebrew.yml` workflow updates
`mg-chao/homebrew-tap` (`main`, `Casks/snow-shot.rb`) from published **stable**
GitHub releases. It does not change the Windows packaging workflow or Gitee
mirror. The tag must be `v<major>.<minor>.<patch>_snow-shot`, matching
`SNOW_SHOT_VERSION` in its source checkout. That source must contain the installer
with `--prepare-app` support. Old releases lacking it cannot be backfilled using
an installer from `main`.

One-time setup:

1. Create the public `mg-chao/homebrew-tap` repository with an initial `main`
   commit. Copy `homebrew/README.md` as its README. The workflow generates the
   first `Casks/snow-shot.rb`; do not publish a placeholder checksum or cask.
2. Configure `HOMEBREW_TAP_TOKEN` as a secret in `mg-chao/snow-apps`. Use a
   fine-grained token restricted to the tap repository with Contents read/write.
   Its branch policy must allow the automation to push to `main`.
3. Include the matching `snow-shot-<version>-macos-arm64.dmg` and `.dmg.sha256`
   assets before publishing the stable GitHub release. macOS packaging/upload
   remains a separate release operation; the existing Windows CI does not build
   these assets. Do not mark a beta version stable to enable Homebrew.

The workflow validates release metadata, source version, checksums, and the current
tap version, then produces `snow-shot-<version>-macos-arm64-homebrew.tar.gz`.
This archive contains the DMG, a normalized checksum sidecar, and both installer scripts
from that tag. Archive entry metadata and gzip timestamps are fixed for repeatable
builds. The generated cask pins the archive's SHA-256 and uses the versioned
GitHub release URL. Intel assets are not required or advertised by this cask.

The archive is uploaded before committing the cask. An existing identical archive
is reused; differing bytes are an error and are never overwritten. An identical
cask needs no commit. Same-version cask changes and version downgrades are rejected.
All workflow versions share one concurrency group, and pushes are never forced.
If a push fails, rerun after resolving the tap's branch policy or concurrent edits.
An archive may remain published after a failed tap push; retry safely reuses it.

Missing macOS assets fail without updating the tap. After uploading the missing
pair, retry with Actions → Publish Snow Shot Homebrew cask → Run workflow, supplying
the stable tag. With GitHub CLI:

```sh
gh workflow run snow-shot-homebrew.yml --repo mg-chao/snow-apps \
  -f tag=v1.2.3_snow-shot
```

If another workflow publishes a release using `GITHUB_TOKEN`, GitHub may suppress
the release-triggered workflow; explicitly dispatch this workflow in that case.
The release token uploads assets only in `snow-apps`; the separate tap token is
used only for checking out and pushing the tap.

For an offline review or initial tap scaffold, save GitHub's release JSON, download
the matching DMG/checksum pair, and check out its tag into a separate source path:

```sh
python3 scripts/snow-shot-homebrew.py package \
  --release-json release.json --assets downloaded-assets \
  --source release-source --current-cask homebrew-tap/Casks/snow-shot.rb \
  --output artifacts/homebrew
```

A nonexistent `--current-cask` means first publication. The output includes the
archive and `Casks/snow-shot.rb`; it performs no network or Git writes. Run
`python3 scripts/test-snow-shot-homebrew.py` for the release helper's focused tests.
With Homebrew installed and the generated cask in the tap, run
`brew style --cask --except Cask/InstallSteps mg-chao/tap/snow-shot` and
`brew audit --cask mg-chao/tap/snow-shot`. Native installation/upgrade qualification
is described in `docs-macos-build.md` and is required separately from these tests.

The pull-request workflow `snow-shot-homebrew-checks.yml` runs the two focused
Python suites plus Homebrew style and offline metadata auditing on macOS. Its
generated fixture is never published or installed. The third-party cask uses the
supported (but deprecated) third-party Ruby preflight API: Homebrew 7's declarative sandbox substitutes HOME
and blocks account lookup, so it cannot preserve this installer's persistent
Keychain identity as-is. The workflow excludes only `Cask/InstallSteps`, the
rule requiring official taps to use declarative hooks. All other style checks
and offline audits run normally; no runtime security settings are changed.

Use `brew style --cask --except Cask/InstallSteps mg-chao/tap/snow-shot` for this
third-party cask. The release's `prepare-snow-shot-homebrew.sh` wrapper handles
rollback without requiring the unavailable signing key again.

After saving or rotating the tap token, run **Check Snow Shot Homebrew support**
manually. In addition to the focused tests, its manual-only job checks out the
existing tap using `HOMEBREW_TAP_TOKEN` and performs `git push --dry-run` to verify
push authentication without changing the tap or publishing release assets.
