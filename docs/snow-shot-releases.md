# Snow Shot releases and updates

Approved specification: the user accepted the implementation plan on 2026-09-10.
Priorities: data preservation and authenticity > recovery > testability > maintainability.

| Requirement | Contract | Verification |
| --- | --- | --- |
| U1 | One SemVer feed, signed metadata, no automatic downgrade | update contract tests |
| U2 | Background check/download; explicit restart; preserve active work | update service and About tests |
| U3 | Apply only verified owned files; journal, probe, recover | update transaction tests |
| R1 | Windows packages plus configured macOS DMG/installer on GitHub; Gitee mirrors published releases | publisher and mirror tests |
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
then publishes the release. It has no website deployment or rollback operation.
Stable releases become GitHub's latest; preview releases remain marked as previews.
Identical retries reuse existing assets; conflicting bytes fail without overwrite.

```powershell
& scripts/publish-snow-shot-release.ps1 -SigningKeyPath C:/private/snow-shot-release/private.pem
& scripts/publish-snow-shot-release.ps1 -Operation Verify
```

The repository's `GITEE_TOKEN` secret must have Gitee tag and release write access.
After GitHub publishes a Snow Shot release, `.github/workflows/snow-shot-gitee-release.yml`
synchronizes its tag, creates the matching Gitee release, copies every asset byte for
byte with signed metadata last, and downloads assets to verify size and SHA-256.
`workflow_dispatch` accepts a tag for backfill or retry. Identical reruns fill
missing assets; conflicting assets fail without overwrite. Configure the secret.
If an existing published `*_snow-shot` release is available, run the workflow
manually for that tag before shipping an updater that discovers Gitee. Otherwise,
publish the first eligible GitHub release and verify its automatic mirror. Confirm
both public release pages and asset downloads before rollout.

## Release contract

GitHub and Gitee publish versioned Windows installer and update assets, their checksum
sidecars and audit manifests, plus `latest-version.json`. When macOS packaging is
configured, the release also includes the versioned arm64 DMG, its `.sha256` sidecar,
and `install-snow-shot-macos.sh`. Signed metadata is uploaded last. The Windows
updater uses the signed JSON envelope and never fetches the website's legacy update
feed. The website homepage and unrelated application API remain configured separately.

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
