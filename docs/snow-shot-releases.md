# Snow Shot releases and updates

Approved specification: the user accepted the implementation plan on 2026-09-10.
Priorities: data preservation and authenticity > recovery > testability > maintainability.

| Requirement | Contract | Verification |
| --- | --- | --- |
| U1 | One SemVer feed, signed metadata, no automatic downgrade | update contract tests |
| U2 | Background check/download; explicit restart; preserve active work | update service and About tests |
| U3 | Apply only verified owned files; journal, probe, recover | update transaction tests |
| R1 | Windows packages plus configured macOS DMG/installer; root latest-version.txt published last | release publisher tests |
| R2 | Private local keys/SSH settings; allowlisted, reversible deployment | signing and publisher tests |

The first updater release is `1.0.0-beta`. Binaries without an updater require one manual
installation/replacement. This release introduces no user-data migration, delta patches,
additional channels, macOS in-app installation, or OCR model-host deployment.

## GitHub fallback and local publication

Clients try the configured official website first. Failed HTTP requests, timeouts, and
invalid metadata fall back to published stable `v<version>_snow-shot` releases in
`mg-chao/snow-apps`. Discovery selects the highest stable SemVer and is bounded to ten
pages of 100 releases. A valid website response saying there is no update does not query
GitHub. Drafts and prereleases are never offered by the fallback.

Windows requires the release asset `latest-version.json`, authenticated with the existing
embedded RSA keys. GitHub transports the same schema-1 envelope; its version must match
the tag. Signed website package identities map to versioned GitHub assets, retaining all
signed sizes, hashes, inventories, and downgrade protections. A failed website package
transfer retries against the exact accepted version on GitHub. Resume validators are
bound to the source URL and partial downloads restart when changing sources. The helper
restores authenticated cached release state between operation-scoped processes.

macOS requires a DMG and checksum for its architecture before announcing a GitHub update.
About then offers **Download from GitHub**, opening the exact release page. It still does
not install updates in-app. Intel clients require separately provided x64 assets; the
existing remote Mac publisher produces arm64 only.

The tag workflow continues to create drafts and never receives the private signing key.
Finalize through the local publisher, using the same audited bytes as any existing draft:

```powershell
& scripts/publish-snow-shot-release.ps1 -Destination GitHub `
    -SigningKeyPath C:/private/snow-shot-release/private.pem -SkipBuild
& scripts/publish-snow-shot-release.ps1 -Destination GitHub -Operation Verify
```

`-Destination` accepts `Website` (the compatibility default), `GitHub`, or `Both`.
The checked-in local wrapper example selects `GitHub`. `-GitHubRepository` defaults to
`mg-chao/snow-apps`; changing the publication repository does not redirect shipped clients.
Use an authenticated GitHub CLI with repository Contents write permission. Push the
matching tag first: its commit must match the local source checkout. Avoid rebuilding
an existing version with different bytes; download the draft's versioned artifacts into
the build directory for `-SkipBuild`, or publish a new version. The local compiled updater
is still required to audit the complete staged release.

GitHub-only publication and auditing require no website settings or website access.
They still validate the immutable OCR runtime and, when configured, use the Mac build host.
The publisher signs locally, audits before upload, verifies all uploaded bytes, then
publishes the draft. Stable versions become latest; beta versions remain prereleases.
Identical retries reuse the existing authenticated envelope and assets; conflicting
assets or incomplete already-public releases fail without overwrite. `-AuditOnly` performs
no upload/publication, and `-WhatIf` only lists the intended assets. Existing ignored local
wrappers are not changed automatically; add `Destination = 'GitHub'` to opt in.

`Both` publishes GitHub before starting website deployment. Website failure does not undo
GitHub publication. `Verify` checks signed Windows payloads and any macOS DMG/checksum
pairs. GitHub rollback is unsupported; select `Website` explicitly for website rollback,
or publish a higher corrective version. Homebrew and WinGet publication still follow
their existing release-event workflows. When macOS is configured, its versioned DMG and
checksum are included before the release becomes public.

Legacy GitHub releases without signed metadata cannot drive Windows automatic updates.
Clients predating this fallback require a manual upgrade while the website is unavailable.
No production publication is needed to run the focused tests:

```powershell
& scripts/test-snow-shot-github-release.ps1
cargo test --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml --lib service::tests
cargo test --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml --lib github::tests
ctest --preset test-windows-msvc-debug -R '^snow-shot-(macos-update|update-adapter|about-page)-tests$'
```

## Release contract

The Windows updater consumes `/latest-version.json`, not the unsigned text file. The root
`/latest-version.txt` remains the compatibility endpoint for older clients and the website;
there is deliberately no `/setup/latest-version.txt` requirement.

The publisher changes only these public paths, in this order:

1. `setup/snow-shot_windows-x64-offline.exe`
2. `setup/snow-shot_windows-x64-online.exe`
3. `setup/snow-shot_windows-x64-portable.zip`
4. `setup/snow-shot_windows-x64-offline-update.zip`
5. `setup/snow-shot_windows-x64-online-update.zip`
6. `setup/snow-shot_macos-arm64.dmg` (when the Mac host is configured)
7. `setup/snow-shot_macos-arm64.dmg.sha256` (when configured)
8. `setup/install-snow-shot-macos.sh` (when configured)
9. `setup/SHA256SUMS`
10. `latest-version.json`
11. `latest-version.txt`

Other setup files, including older versioned Windows and macOS downloads, are untouched. Local build
artifacts and GitHub assets keep versions in their names; public website URLs do not.

The JSON envelope is `{schema:1,keyId,payload,signature}`. `payload` and `signature` are
Base64. The signature is RSA-3072/PSS/SHA-256 (32-byte salt) over the exact decoded UTF-8
payload bytes. The payload includes `schema`, strict SemVer `version`, ISO-8601
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
external CodeView/PDB information. macOS bundles and signs the same executable, but the
service reports updates unavailable and does not expose a self-update channel there.

## Operator setup and commands

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
This workflow neither publishes application releases nor changes the website feed.

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

Installation is machine-wide and requires elevation. Close Snow Shot before a silent
upgrade or uninstall: installer exit code 10 maps to WinGet's `packageInUse` response.
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
Confirm the version changes, directory/settings survive, the app does not launch during
silent installation, and an upgrade while the app is running refuses without killing it.
Finally uninstall silently and verify owned files/registration are removed and user data
is preserved. Fixture tests and manifest validation do not substitute for this VM check.

The **Snow Shot WinGet verification** workflow automates this lifecycle on a disposable
GitHub-hosted Windows runner. It runs installation checks for WinGet changes in pull
requests; manual runs accept `tag` and `previous_tag`. Pushes to `codex/winget-*`
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

Use PowerShell 7 and the repository's documented Windows release toolchain. The tracked
publisher accepts all machine-specific values as parameters. Copy
`scripts/publish-snow-shot-release.local.example.ps1` to the ignored
`scripts/publish-snow-shot-release.local.ps1` and supply the SSH host/user/port, key and
known-hosts files, private signing-key path, public HTTPS origin, and web root there.
Never put the IP, credentials, private signing key, or local wrapper in Git. SSH uses strict
host-key checking; enroll and verify a new server's fingerprint out of band first.

### Coordinated Windows and macOS packaging

Add `MacHost`, `MacUser`, and `MacProjectDirectory` to the ignored local settings.
`MacPort` defaults to 22; `MacIdentityFile` and `MacKnownHostsFile` are optional and
independent of the production server's SSH credentials. If omitted, OpenSSH uses
the local SSH config/agent and default known-hosts file. The project path must be
an absolute POSIX path without spaces, shell metacharacters, or `..`.

With a Mac host configured, the normal publish command starts an SSH packaging job
alongside Windows packaging, then waits for both. The current macOS target is native
Apple Silicon (`snow-shot-macos-arm64-release`). The Mac must already have the
documented release toolchain, audited static Qt, and repository dependencies.
Provisioning dependencies is separate from a release.

The workflow builds the existing Mac checkout. It does **not** pull, reset, stash,
or overwrite source files. Prepare both checkouts before release and set the same
new `SNOW_SHOT_VERSION` in each. Uncommitted work is supported and recorded: the
Mac worker checks the source version, captures HEAD and a fingerprint of tracked
changes/untracked non-ignored files, and rejects source edits made during packaging.
It takes an exclusive `artifacts/.macos-release.lock`, runs the audited package
script, verifies CPack's checksum, `hdiutil verify`, and the DMG code signature,
then copies the result into an immutable transaction directory for transfer.
After an interrupted SSH session, confirm no packaging process remains before
manually removing a stale empty lock directory.

The Windows client verifies the downloaded DMG size and SHA-256, writes a checksum
using the public filename, and stages the standalone installer with LF line endings
and no BOM. Logs and a source receipt are kept under
`artifacts/publish-<id>/setup/macos-build.{log,json}`; these are not public uploads.
Remote transaction copies remain under `artifacts/remote-release-<id>` for diagnosis
and may be removed after a completed release when no transfer is using them.

`-SkipBuild` reuses packages on **both** platforms. On the Mac it requires the source
receipt from a previous successful coordinated build and an identical source
fingerprint/DMG hash. It does not repackage. `-AuditOnly` still builds unless combined
with `-SkipBuild`; it performs verification but does not stage or publish to production.
`-WhatIf` is the preview command with no build or SSH side effects.

The DMG, checksum, and script join the same staged transaction, public HTTPS checks,
and rollback journal as Windows. The signed Windows updater envelope stays exactly
the same schema and still lists only its five Windows packages. Once the macOS files
are published, the server refuses Windows-only publishing to avoid advancing the
shared version without a matching Mac package. Older rollback journals retain their
original Windows-only scope. A release version cannot be reused to add or change
files: the first combined release must use a new version on both platforms.

The site's English and Chinese download pages offer the installer at
`https://snowshot.top/setup/install-snow-shot-macos.sh`, using HTTPS-only download
followed by `bash` only if the download succeeds. Deploy the site option only after
the first combined release makes that script and the DMG/checksum available.
Website deployment is separate from the release publisher.

Focused workflow checks (no builds, SSH, or production mutations):

```powershell
python scripts/test-snow-shot-publisher.py
python scripts/test-remote-macos-release.py
pwsh -NoProfile -File scripts/test-remote-macos-release.ps1
```

Run the packaging entry point's static-dependency preflight before a manual application
build. If it rejects a stale dependency prefix after syncing main, restore the checked-in
overlay contract with `scripts/bootstrap.ps1 -VcpkgVariants Static` and rebuild; do not edit
installed ABI receipts, component headers, or the audit expectations. vcpkg includes the
PowerShell version in package ABIs, so a tool patch update can invalidate otherwise unchanged
packages. Use the actual matching tool version or rebuild the affected dependency graph.

```powershell
# Preview the exact allowlist, without build, signing, SSH, or public mutations.
& scripts/publish-snow-shot-release.local.ps1 -WhatIf

# Build, audit, package, sign, upload, activate, verify through HTTPS, and commit.
& scripts/publish-snow-shot-release.local.ps1

# Retry with the exact existing audited packages. Does not rebuild or repackage.
& scripts/publish-snow-shot-release.local.ps1 -SkipBuild

# Sign and run all packaged audits; only read production state, without uploading.
& scripts/publish-snow-shot-release.local.ps1 -SkipBuild -AuditOnly

# Read current public-file size/hash metadata over authenticated SSH.
& scripts/publish-snow-shot-release.local.ps1 -Operation Verify

# Restore the previous public allowlisted files (or undo a pending activation).
& scripts/publish-snow-shot-release.local.ps1 -Operation Rollback
```

The source version must equal `SNOW_SHOT_VERSION` in root CMake. Do not reuse a version for
different bytes. For an identical retry, the publisher reuses the existing authenticated
envelope because PSS signing uses random salt. The compiled updater audits all package
hashes, signed inventories, archive extraction, and packaged app startup probes before any
upload. `-SkipBuild` does not bypass these audits. Installer manifests must record static
Qt/CRT, x64, and the production release preset.

Before building or SSH staging, the publisher also downloads the checked-in immutable OCR
runtime URL and verifies its pinned size/SHA-256. This prevents releasing an online installer
whose required runtime cannot be obtained, and catches missing assets before a long build.

Application packaging imports that exact published archive, verifies every inventory entry
before extraction, and retains the archive and runtime-manifest bytes unchanged. It still
audits the published PE architecture/dependencies, version resources, every model set, and
the complete checked-in manifest. Locally recompiling an immutable OCR version is not a
reproducibility guarantee. Only the explicit `-PrepareOcrRuntimeOnly` workflow builds a new
OCR upload candidate; it does not authorize replacing an already published runtime version.
The application symbol archive requires matching app/helper PDBs and records the external
OCR runtime identity instead of including an unrelated local OCR rebuild's symbols. Keep
OCR runtime symbols with their originating OCR release; an application rebuild cannot
reconstruct those exact PDBs.

Files are uploaded into a private transaction directory beside the web root. The server
uses a lock, verifies every upload, keeps durable backups, promotes individual files by
atomic replacement, and publishes signed metadata and text last. Public HTTPS verification
downloads every allowlisted file and checks size/hash before commit. Promotion is not a single
atomic replacement of the entire website directory: a client racing publication may see a
temporary hash mismatch and must retry metadata. Signature and hash checks prevent applying
mixed payloads.

The server retains the current transaction and two earlier successful transactions. The
oldest retained transaction can restore its before-image, but cannot extend rollback into
deleted transaction history. An active pending deployment has a one-hour lease; another
publish cannot automatically roll it back during HTTPS validation. An interrupted client
can be recovered immediately with explicit `Rollback`; a later publish can recover a stale
lease. Upload-only failures leave private staging for inspection and never change public
files. Operators should remove abandoned upload-only directories after verifying that no
publisher is active. A failed public verification attempts rollback automatically.

A server rollback does **not** downgrade already updated clients. Those clients reject an
older feed; publish a higher corrective version to restore automatic forward progress.

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
python scripts/test-snow-shot-publisher.py
& scripts/test-snow-shot-ocr-release-runtime.ps1
& scripts/test-snow-shot-release-symbols.ps1 `
    -ReleaseHelperPath build/snow-shot-msvc-release/snow_shot/Release/snow-shot-updater.exe
& scripts/test-snow-shot-installer.ps1
& scripts/test-snow-shot-installer-i18n.ps1

cargo test --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml
cargo clippy --manifest-path snow_shot/rust/snow-shot-updater/Cargo.toml `
    --all-targets -- -D warnings

# Production static Qt kit omits QtTest; these focused targets do not require it.
cmake --preset snow-shot-msvc-release -DSNOW_SHOT_BUILD_UPDATE_TESTS=ON
cmake --build --preset build-snow-shot-msvc-release --target `
    snow-shot-update-adapter-tests snow-shot-update-about-tests snow-shot-update-settings-tests
ctest --test-dir build/snow-shot-msvc-release/snow_shot -C Release `
    -R '^snow-shot-update-(adapter-tests|about-tests|settings-tests)$' --output-on-failure
& scripts/test-snow-shot-update-helper.ps1
```

The helper canary uses a native parent/relaunch fixture and the real compiled helper. It
checks handoff cancellation, journal recovery including replacement of the installed helper,
original-user relaunch, and preservation of
unowned data for all three variants without launching the user's application or reading its
settings. For interactive elevation checks, run it from a non-elevated shell with
`-ElevationAction Cancel` and then `-ElevationAction Approve`. The operator must perform the
requested UAC action. It temporarily registers a protected, isolated test directory under
HKCU, refuses to replace an existing nonempty per-user installation registration, and
restores the test ACL and registry state on exit. The machine-wide installation is untouched.

Do not run the full repository test suite for this feature. Before production delivery,
also validate real installed/portable helper handoff, UAC approval/cancellation, restart under
the original user, the three packaged startup probes, and unchanged legacy server files.
Cross-account UAC and physical power-loss behavior require Windows/runtime validation beyond
the deterministic unit tests. A later update prunes generated coordinator/worker files older
than 24 hours, skips running/locked executables, and never recursively sweeps the system temp
directory. Transaction staging and backup payloads are replaced on the next transaction;
download cache cleanup retains only the currently accepted release's ZIP/partial download.

### Current delivery evidence (2026-09-20)

- The standalone updater package passes rustfmt, 28 focused Rust tests, and Clippy for all
  targets with warnings denied. Coverage includes RSA-3072/PSS verification, strict SemVer,
  package and inventory identity, unsafe paths and collisions, ZIP central-directory rejection,
  transaction commit/rollback/recovery, bounded NDJSON framing, generated coordinator paths,
  and injected-clock/network checks for scheduling, timeouts, cancellation, retry delays,
  strong-ETag resume, invalid ranges, and restart-on-200 behavior. Real child-process protocol
  tests cover the Rust service handshake, duplicate IDs, fatal malformed frames, peer closure,
  and orderly shutdown over its inherited standard streams.
- The Debug application and updater adapter build with the ordinary Cargo `dev` profile and
  debug CRT; production-only LTO/linker switches are absent. The focused Debug adapter and
  unrelated administrator-settings tests pass after removal of the old privileged-pipe code.
- The production static-Qt adapter, offscreen About-page, and settings-catalog tests pass. The
  application-wide translation extraction finds no new strings; lrelease reports 1,618 finished
  and zero unfinished messages for each of en_US, zh_CN, and zh_TW.
- Eight publisher tests, the installer process suite, and all three installer-language suites
  pass, including their CPack fixtures. Twelve real-helper canaries pass across online, offline,
  and portable installations: cancellation, applying-journal recovery, helper replacement,
  verified recovery failure, original-user relaunch, and preservation of unowned data.
- Same-account UAC cancellation and approval pass from a non-elevated shell against protected
  fixtures. Approval covers recovery after helper self-replacement, verifies original-user
  relaunch, and creates or updates the matching 32-bit uninstall registration; cancellation
  leaves the protected installation untouched and never relaunches a partially updated app.
- A complete `snow-shot-msvc-release` package build passes. It collects 672 license notices for
  316 resolved Rust packages and 41 vcpkg packages plus Qt; installs the Rust updater; validates
  five PE binaries; verifies matching PDB identities; audits all 44 enabled FFmpeg registrations;
  and produces the online/offline installers plus online/offline update and portable ZIPs.
- The compiled Rust `--verify-release` and `--audit-release` commands accept the live production
  schema-1 envelope and all five downloaded packages, including exhaustive update-archive
  inventories and isolated startup probes. This is direct backward-compatibility evidence for
  the last release produced with the C++ updater.
- Reusing published version `1.0.7` with the rebuilt packages is correctly rejected because the
  signed hashes differ. The first Rust-updater publication must increment `SNOW_SHOT_VERSION`;
  after that explicit release decision, rerun `-AuditOnly` to sign and audit the newly versioned
  local packages before upload.
- The final production updater is 2,306,560 bytes (2.200 MiB), 7,154,176 bytes and 75.620% smaller
  than the 9,460,736-byte Qt/C++ baseline. Raw sections are `.text` 1,639,424, `.rdata` 551,936,
  `.data` 4,608, `.pdata` 56,832, `.fptable` 512, `.rsrc` 32,768, and `.reloc` 19,456 bytes.
  Imports are Windows system DLLs only: Advapi32, the synchronization API set,
  BcryptPrimitives, Crypt32, Kernel32, Ntdll, Ole32, OleAut32, Secur32, Shell32, and Ws2_32.
  The matching external PDB is 33,771,520 bytes (32.207 MiB) and passes RSDS GUID/age checks.
- Both native macOS architecture/package jobs remain platform release gates. Cross-account UAC
  and physical power-loss behavior likewise require dedicated runtime validation.

## macOS version checks

macOS offers only **Manual** and **Check automatically**, defaulting to automatic checks.
Legacy `download` settings normalize to `check`, including when restoring settings. Resetting
settings restores the platform default. Automatic checks start 30 seconds after launch and
repeat 24 hours after completion; manual checks remain available in About. Switching to manual
stops the schedule and cancels an active background check.

The macOS Qt service first fetches `/latest-version.txt` from the configured API base URL over HTTPS,
respects the network proxy setting, limits responses to 4 KiB, and times out after 30 seconds.
It compares strict SemVer precedence (including prereleases and ignoring build metadata).
This website compatibility endpoint is unsigned; it only supplies version display text and
never supplies an executable, installation instructions, or a navigation URL. Publish the
matching macOS installation packages before announcing a shared version on this endpoint.

A newer version discovered automatically shows a system notification once per version per
session. Clicking it opens About, where **Download from website** uses the configured official
website URL, or **Download from GitHub** opens the exact release page when the fallback supplied the update. About retains the available version. Background failures stay quiet; manual
failures display a retry action.
macOS does not build or bundle the Windows updater helper and never downloads or installs an
update in-app. The Windows signed-metadata and installation flow is unchanged.

## Homebrew tap publication

The separate `snow-shot-homebrew.yml` workflow updates
`mg-chao/homebrew-tap` (`main`, `Casks/snow-shot.rb`) from published **stable**
GitHub releases. It does not change the Windows packaging workflow or website
publisher. The tag must be `v<major>.<minor>.<patch>_snow-shot`, matching
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
