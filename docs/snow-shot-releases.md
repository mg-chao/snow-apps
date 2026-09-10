# Snow Shot releases and updates

Approved specification: the user accepted the implementation plan on 2026-09-10.
Priorities: data preservation and authenticity > recovery > testability > maintainability.

| Requirement | Contract | Verification |
| --- | --- | --- |
| U1 | One SemVer feed, signed metadata, no automatic downgrade | update contract tests |
| U2 | Background check/download; explicit restart; preserve active work | update service and About tests |
| U3 | Apply only verified owned files; journal, probe, recover | update transaction tests |
| R1 | Five fixed-name packages; root latest-version.txt published last | release publisher tests |
| R2 | Private local keys/SSH settings; allowlisted, reversible deployment | signing and publisher tests |

The first updater release is `1.0.0-beta`. Binaries without an updater require one manual
installation/replacement. This release introduces no user-data migration, delta patches,
additional channels, macOS updater, or OCR model-host deployment.

## Release contract

The application consumes `/latest-version.json`, not the unsigned text file. The root
`/latest-version.txt` remains the compatibility endpoint for older clients and the website;
there is deliberately no `/setup/latest-version.txt` requirement.

The publisher changes only these public paths, in this order:

1. `setup/snow-shot_windows-x64-offline.exe`
2. `setup/snow-shot_windows-x64-online.exe`
3. `setup/snow-shot_windows-x64-portable.zip`
4. `setup/snow-shot_windows-x64-offline-update.zip`
5. `setup/snow-shot_windows-x64-online-update.zip`
6. `setup/SHA256SUMS`
7. `latest-version.json`
8. `latest-version.txt`

Other setup files, including older Windows and macOS downloads, are untouched. Local build
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
| Fresh install | Background download mode; first check after 30 seconds |
| Successful check less than 24 hours ago | Scheduled check skipped; manual check still allowed |
| Settings mode `manual` / `check` / `download` | No scheduled requests / metadata only / metadata and payload |
| Newer authenticated release | Select the same installation variant; download with bounded retries |
| Interrupted transfer with a strong ETag | Resume with Range and If-Range; verify full signed size/hash |
| Signature failure or changed same-version payload | Reject; do not apply or trust new state |
| Lower SemVer than the newest observed feed | Reject replay/downgrade |
| Verified payload ready | Notify in tray and About; never restart without explicit confirmation |
| Capture/recording/export active or settings cannot flush | Refuse restart; check again at the final helper handoff |
| User declines elevation or the ready/go handshake fails | Keep the current app running |
| Apply interrupted or startup probe fails | Restore the previous owned payload from its journaled backup |
| Failed version offered automatically again | Suppress automatic apply readiness; explicit manual retry is possible |
| Helper watchdog fires after handoff | Do not start a possibly incomplete app; manual launch enters recovery |
| Development copy without installation metadata | Show updates unavailable; never infer an install layout |

The helper verifies the parent process's real executable path, stages outside `bin`, and
requests elevation only for a matching registered installation. The broker remains under
the original user's identity for relaunch. Named pipes are random and user-restricted.
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

`snow_shot_update_core` depends on Qt Core and minizip, not capture, presentation, storage,
or Rust. `snow_shot_updates` adds network/state coordination; About and ApplicationController
consume it. These boundaries are enforced by CMake target dependencies.

## Operator setup and commands

Use PowerShell 7 and the repository's documented Windows release toolchain. The tracked
publisher accepts all machine-specific values as parameters. Copy
`scripts/publish-snow-shot-release.local.example.ps1` to the ignored
`scripts/publish-snow-shot-release.local.ps1` and supply the SSH host/user/port, key and
known-hosts files, private signing-key path, public HTTPS origin, and web root there.
Never put the IP, credentials, private signing key, or local wrapper in Git. SSH uses strict
host-key checking; enroll and verify a new server's fingerprint out of band first.

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
downloads all eight files and checks size/hash before commit. Promotion is not a single
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
& scripts/test-snow-shot-release-symbols.ps1 -ReleaseHelperPath build/snow-shot-msvc-release/snow_shot/Release/snow-shot-updater.exe
& scripts/test-snow-shot-installer.ps1
& scripts/test-snow-shot-installer-i18n.ps1

# Production static Qt kit omits QtTest; these focused targets do not require it.
cmake --preset snow-shot-msvc-release -DSNOW_SHOT_BUILD_UPDATE_TESTS=ON
cmake --build --preset build-snow-shot-msvc-release --target snow-shot-update-tests snow-shot-update-about-tests snow-shot-update-settings-tests
ctest --test-dir build/snow-shot-msvc-release/snow_shot -C Release -R '^snow-shot-update-(tests|about-tests|settings-tests)$' --output-on-failure
& scripts/test-snow-shot-update-helper.ps1
```

The helper canary uses a native parent/relaunch fixture and the real compiled helper. It
checks handoff cancellation, journal recovery, original-user relaunch, and preservation of
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

### Current delivery evidence (2026-09-10)

- Publisher tests: eight focused tests pass, including every promotion checkpoint failure,
  corruption, concurrent lease, stale recovery, identical retry, version conflict/downgrade,
  repeated commit, retention-boundary rollback and republishing.
- Installer tests and all three installer-language tests pass, including CPack fixture builds.
- Rebased onto main `93108309`, preserving the redesigned About page and new translation/settings
  behavior. All three focused C++ targets pass in the production static Qt build: updater contract/
  service/transaction tests, offscreen About tests, and settings catalog/search tests (12.29 seconds
  combined). The updater core also passed in a separate verification tree before the main sync.
- Covered failure cases include actual updater process termination at journal checkpoints,
  long installation paths, archive traversal/corruption, mismatched ownership inventories,
  unknown-file collisions, reserved-work-directory data collisions, failed-version suppression,
  resumed downloads, canceled requests, and policy changes during metadata requests.
- The updater helper builds and links. Six real-helper canaries pass: cancellation and journal
  recovery/relaunch for online, offline, and portable copies, including original-user SID and
  preservation of unrelated files. The initial main-app Release link succeeded; package
  preflight then correctly rejected stale static FFmpeg components before release creation.
  Installed FFmpeg and ONNX Runtime receipts predated main's overlays; a verified local
  PowerShell 7.6.5 tool limited vcpkg's repair to those two packages instead of an unrelated
  full dependency rebuild. Both packages now match current ABI receipts, ONNX Runtime's
  rebuild/post-build validation passes, and the unchanged 41-component static audit passes.
  The corrected application relink and all three focused C++ tests pass (12.29 seconds).
  Packaging passes: both installers and all three ZIPs were generated, matching release
  symbols retained, five PE dependencies checked, and all 37 required linked FFmpeg
  registrations verified with no disabled providers present.
- Interactive same-account UAC canaries pass with the operator: approval recovers the protected
  fixture and relaunches under the original user and elevation level; declining permission
  keeps the parent running, preserves the pending journal/files, and does not relaunch. The
  fixture ACL and temporary HKCU registration are restored; the machine installation is untouched.
- Translation extraction and lrelease report 1,451 finished messages and zero unfinished messages
  in each of en_US, zh_CN and zh_TW. Changed C++ files pass clang-format; changed PowerShell
  scripts parse; `git diff --check` passes.
- The owner published OCR `1.0.5`; its remote archive now matches the pinned 17,345,756 bytes and
  SHA-256 `8589896e11f00c80520ac1886f73ff90566b3017b0e688225593f337b71ed15f`. Normal application
  packaging imports those exact bytes instead of requiring a byte-identical local recompilation.
  Twelve deterministic import checks pass, and the downloaded executable passes its version/
  protocol invocation. Pinned hashes and remote OCR assets were not changed.
- The wrapper's `-WhatIf` output confirms the exact eight-file public allowlist. `-AuditOnly`
  passed with the actual signing key and compiled updater: all five package hashes, exhaustive
  ZIP inventories/extraction, embedded packaged-helper trust, and isolated online/offline/
  portable startup probes passed without uploading or activating a production transaction.
- The real Release helper now emits a matching PDB with the same symbol flags as the
  application. Packaging exposed CMake's default disabled Release symbols; the original
  synthetic symbol fixture did not cover that target configuration. A new real-helper
  regression check reproduced the missing PDB before the fix and passes afterward, while
  missing-PDB rejection remains enforced. The release workflow runs this check, and all six
  real-helper cancellation/recovery canaries pass again with the rebuilt executable.
- The pre-deployment inventory records `0.8.1-dev`, three current Windows downloads, five
  legacy Windows/macOS downloads, and no signed JSON feed. Deployment must compare the
  legacy files against that snapshot; none belongs to this release's mutation allowlist.
- The earlier interrupted Debug dependency operation was checked: ONNX Runtime's installed
  package inventory reports zero missing files; no dependency repair is outstanding.
