#!/usr/bin/env python3
"""Focused installer contracts. No network, sudo, real signing, or Keychain changes.

JXA parser cases use the real macOS JavaScript runtime when available.
"""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().with_name('install-snow-shot-macos.sh')
PREFLIGHT = SCRIPT.with_name('prepare-snow-shot-homebrew.sh')
FINGERPRINT = 'A' * 40
REQUIREMENT = 'identifier "com.snowshot.snow_shot" and anchor = H"' + FINGERPRINT + '"'
API = 'https://api.github.com/repos/mg-chao/snow-apps/releases?per_page=100&page=1'
GITEE_API = 'https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases?per_page=100&page=1'
GITEE_ATTACH = 'https://gitee.com/api/v5/repos/mg-chao/snow-apps/releases/123/attach_files?per_page=100'
ASSET = 'snow-shot-1.2.3-macos-arm64.dmg'
TAG = 'v1.2.3_snow-shot'
GITHUB = 'https://github.com/mg-chao/snow-apps/releases/download/' + TAG + '/' + ASSET
GITEE = 'https://gitee.com/mg-chao/snow-apps/releases/download/' + TAG + '/' + ASSET

MOCK = r'''#!/usr/bin/env python3
import hashlib, json, os, pathlib, plistlib, shutil, signal, subprocess, sys, time
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
root = pathlib.Path(os.environ['FIXTURE'])
with (root / 'calls.jsonl').open('a') as f: f.write(json.dumps([name] + args) + '\n')
def fail(code=1): sys.exit(code)
if name == 'curl':
    if args[-1] == os.environ.get('BLOCK_URL'):
        def stopped(signum, frame):
            (root / 'blocked-stopped').touch()
            sys.exit(128 + signum)
        signal.signal(signal.SIGTERM, stopped)
        signal.alarm(10)  # Bound a failing cancellation regression without leaving an orphan.
        (root / 'blocked-ready').write_text(str(os.getpid()))
        while True: signal.pause()
    if args[-1] == os.environ.get('WAIT_FOR_BLOCK_URL'):
        while not (root / 'blocked-ready').exists(): time.sleep(0.01)
    if args[-1] == os.environ.get('DELAY_URL'): time.sleep(0.5)
    responses = json.loads((root / 'responses.json').read_text())
    entry = responses.get(args[-1])
    if entry is None: fail(22)
    shutil.copyfile(entry, args[args.index('--output') + 1])
elif name == 'hdiutil':
    if args[0] == 'verify':
        if pathlib.Path(args[1]).read_bytes().startswith(b'BAD'): fail()
    elif args[0] == 'attach':
        if os.environ.get('FAIL_ATTACH'): fail()
        mount = pathlib.Path(args[args.index('-mountpoint') + 1])
        shutil.copytree(root / 'bundle', mount / os.environ.get('BUNDLE_NAME', 'Snow Shot.app'), dirs_exist_ok=True)
        (root / 'mounted').write_text(str(mount))
    elif args[0] == 'detach':
        if os.environ.get('FAIL_DETACH'): fail()
        (root / 'mounted').unlink(missing_ok=True)
elif name == 'mount':
    if (root / 'mounted').exists():
        reported = (root / 'mounted').read_text()
        if os.environ.get('CANONICAL_MOUNT_PATH'):
            reported = os.path.normpath(reported)
            if reported.startswith('/var/'): reported = '/private' + reported
        print('/dev/disk99 on ' + reported + ' (apfs, local, read-only)')
elif name == 'plutil':
    data = plistlib.loads(pathlib.Path(args[-1]).read_bytes())
    key = args[args.index('-extract') + 1]
    if key not in data: fail()
    print(data[key])
elif name == 'file': print('Mach-O 64-bit executable ' + os.environ.get('BINARY_ARCH', 'arm64'))
elif name == 'codesign':
    if '--force' in args:
        assert '--deep' not in args
        executable = pathlib.Path(args[-1]) / 'Contents/MacOS/snow_shot'
        executable.write_bytes(executable.read_bytes() + b' locally signed')
    elif '-d' in args: print('designated => ' + os.environ['REQUIREMENT'])
    elif (pathlib.Path(args[-1]) / 'reject-signature').exists(): fail()
    elif os.environ.get('FAIL_FINAL') and args[-1] == str(root / 'Applications/Snow Shot.app'): fail()
    elif os.environ.get('FAIL_REQUIREMENT') and '-R' in args: fail()
elif name == 'ditto': shutil.copytree(args[0], args[1], dirs_exist_ok=True, symlinks=True)
elif name == 'security':
    if os.environ.get('FAIL_SECURITY'): fail()
    if args[0] == 'find-identity':
        print('0 valid identities found' if os.environ.get('MISSING_IDENTITY') else '1) ' + 'A'*40 + ' "Snow Shot Local Installer"')
elif name == 'openssl':
    if args[0] == 'req':
        pathlib.Path(args[args.index('-keyout')+1]).write_text('private fixture')
        pathlib.Path(args[args.index('-out')+1]).write_text('certificate fixture')
    elif args[0] == 'rand': print('B'*64)
    elif args[0] == 'x509': print('SHA1 Fingerprint=' + ':'.join(['AA']*20))
    elif args[0] == 'pkcs12': pathlib.Path(args[args.index('-out')+1]).write_text('fixture p12')
elif name == 'uuidgen': print('TEST-UUID')
elif name == 'xattr': fail()
elif name == 'pgrep':
    if os.environ.get('RUNNING'): print('54321')
    else: fail()
elif name == 'osascript':
    if len(args) >= 4 and args[:3] == ['-l', 'JavaScript', '-'] and args[3].endswith('.json'):
        # Exercise the actual shipped JSON parser, never a Python reimplementation.
        if sys.platform != 'darwin': fail(77)
        sys.exit(subprocess.run(['/usr/bin/osascript'] + args, input=sys.stdin.buffer.read()).returncode)
    if os.environ.get('FAIL_QUIT'): fail()
elif name == 'sleep': pass
elif name == 'open':
    if os.environ.get('FAIL_OPEN'): fail()
elif name == 'sudo':
    if args == ['-v']: pass
    elif args[0] == '--': sys.exit(subprocess.run(args[1:]).returncode)
    else: fail()
elif name == 'uname': print('Darwin' if args == ['-s'] else os.environ.get('MACHINE', 'arm64'))
elif name == 'sysctl': print(os.environ.get('ARM_CAPABLE', '1'))
elif name == 'defaults': print('(\n    "' + os.environ.get('APPLE_LANGUAGE', 'en-US') + '"\n)')
else: raise RuntimeError(name)
'''


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='snow installer tests ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'
        self.state = self.home / 'Library/Application Support/Snow Shot/Installer'
        self.state.mkdir(parents=True)
        (self.home / 'Library/Keychains').mkdir()
        (self.home / 'Library/Keychains/login.keychain-db').touch()
        self.work = self.root / 'work'
        self.work.mkdir()
        self.apps = self.root / 'Applications'
        self.apps.mkdir()
        self.destination = self.apps / 'Snow Shot.app'
        self.bundle = self.root / 'bundle'
        (self.bundle / 'Contents/MacOS').mkdir(parents=True)
        (self.bundle / 'Contents/Resources/assets/ocr').mkdir(parents=True)
        self.info = dict(CFBundleIdentifier='com.snowshot.snow_shot', CFBundleExecutable='snow_shot', LSMinimumSystemVersion='15.0')
        self.write_info()
        self.executable = self.bundle / 'Contents/MacOS/snow_shot'
        self.executable.write_bytes(b'fixture executable')
        self.executable.chmod(0o755)
        (self.bundle / 'Contents/MacOS/snow-ocr-process').write_bytes(b'signed helper')
        (self.bundle / 'Contents/Resources/assets/ocr/asset-manifest.json').write_bytes(b'unchanged manifest')
        self.dmg = self.root / 'package.dmg'
        self.dmg.write_bytes(b'GOOD fixture disk image')
        self.sum = self.root / 'package.dmg.sha256'
        self.sum.write_text(hashlib.sha256(self.dmg.read_bytes()).hexdigest() + '  package.dmg\n')
        self.release = self.root / 'metadata.json'
        self.metadata = dict(tag_name=TAG, draft=False, prerelease=False, assets=[
            dict(name=ASSET, browser_download_url=GITHUB),
            dict(name=ASSET+'.sha256', browser_download_url=GITHUB+'.sha256')])
        self.release.write_text(json.dumps([self.metadata]))
        self.gitee_release = self.root / 'gitee-metadata.json'
        self.gitee_metadata = json.loads(json.dumps(self.metadata))
        self.gitee_metadata['id'] = 123
        attachments = self.gitee_metadata.pop('assets')
        for asset in attachments:
            asset['browser_download_url'] = asset['browser_download_url'].replace('github.com', 'gitee.com')
        self.gitee_release.write_text(json.dumps([self.gitee_metadata]))
        self.gitee_attachments = self.root / 'gitee-attachments.json'
        self.gitee_attachments.write_text(json.dumps(attachments))
        self.responses = {API: str(self.release), GITEE_API: str(self.gitee_release),
                          GITEE_ATTACH: str(self.gitee_attachments),
                          GITHUB: str(self.dmg), GITHUB+'.sha256': str(self.sum),
                          GITEE: str(self.dmg), GITEE+'.sha256': str(self.sum)}
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.mock = self.bin / 'mock'
        self.mock.write_text(MOCK.replace('#!/usr/bin/env python3', '#!' + sys.executable))
        self.mock.chmod(0o755)
        self.commands = ['curl', 'hdiutil', 'mount', 'plutil', 'file', 'codesign', 'ditto', 'security', 'openssl', 'uuidgen', 'xattr', 'pgrep', 'osascript', 'sleep', 'open', 'sudo', 'uname', 'sysctl', 'defaults']
        for command in self.commands:
            (self.bin / command).symlink_to(self.mock)

    def test_accepts_legacy_dmg_bundle_name(self):
        self.shell('validate_and_stage "$FIXTURE/package.dmg" "$FIXTURE/package.dmg.sha256"',
                   BUNDLE_NAME='snow_shot.app')
        self.assertTrue((self.work / 'snow_shot.app/Contents/MacOS/snow_shot').is_file())

    def write_info(self):
        (self.bundle / 'Contents/Info.plist').write_bytes(plistlib.dumps(self.info))

    def shell(self, code, *, success=True, **environment):
        (self.root / 'responses.json').write_text(json.dumps(self.responses))
        env = dict(os.environ, HOME=str(self.home), FIXTURE=str(self.root), REQUIREMENT=REQUIREMENT, LC_ALL='C', LANG='C', **environment)
        setup = '\n'.join([
            'source ' + shlex.quote(str(SCRIPT)),
            'export PATH=' + shlex.quote(str(self.bin)) + ':"$PATH"',
            'work=' + shlex.quote(str(self.work)),
            'state=' + shlex.quote(str(self.state)),
            'destination=' + shlex.quote(str(self.destination)),
            'arch=arm64; asset_arch=arm64; os_version=15.5; language=en',
        ])
        result = subprocess.run(['/bin/bash', '-c', setup + '\n' + code], env=env, text=True, capture_output=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def calls(self, name=None):
        path = self.root / 'calls.jsonl'
        calls = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
        return [call for call in calls if name is None or call[0] == name]

    def stage(self):
        shutil.copytree(self.bundle, self.work / 'snow_shot.app')

    def identity(self):
        (self.state / 'identity').write_text(FINGERPRINT + '\n')

    def previous(self):
        self.destination.mkdir()
        (self.destination / 'old-marker').write_text('previous application')

    def test_help_all_languages(self):
        for language, expected in [('en', 'Usage:'), ('zh-CN', '用法：'), ('zh-TW', '用法：')]:
            result = subprocess.run(['/bin/bash', str(SCRIPT), '--lang', language, '--help'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0)
            self.assertIn(expected, result.stdout)
            self.assertNotIn('\x1b', result.stdout)

    def test_invalid_arguments(self):
        for args in [['--lang'], ['--lang', 'fr'], ['--dmg'], ['--prepare-app'], ['--prepare-app', '/tmp/snow-shot-test.app'], ['--unexpected']]:
            self.assertNotEqual(subprocess.run(['/bin/bash', str(SCRIPT)] + args, capture_output=True).returncode, 0)

    def test_language_detection(self):
        for locale, preference, expected in [('zh_CN.UTF-8', 'en-US', 'zh-CN'), ('zh_TW.UTF-8', 'en-US', 'zh-TW'), ('C', 'zh-Hant-TW', 'zh-TW'), ('C', 'zh-Hans-CN', 'zh-CN'), ('de_DE.UTF-8', 'en-US', 'en')]:
            # LC_ALL drives the installer; no tools rely on these locales being installed.
            result = self.shell('language=auto; LC_ALL="$TEST_LOCALE"; select_language; echo "$language"', TEST_LOCALE=locale, APPLE_LANGUAGE=preference)
            self.assertEqual(result.stdout.strip(), expected)

    def test_all_message_keys_have_three_translations(self):
        result = self.shell("for key in usage help arguments platform root primary fallback unavailable verify invalid signing keychain identity continuity install sudo quit running done permissions failed rollback recovery detach state launch; do for language in en zh-CN zh-TW; do message \"$key\"; done; done")
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), 26 * 3)
        for offset in range(0, len(lines), 3):
            self.assertTrue(all(lines[offset:offset+3]))
            self.assertNotEqual(lines[offset], lines[offset+1])

    def test_native_architecture_and_rosetta(self):
        for machine, capable, expected in [('arm64', '1', 'arm64 arm64'), ('x86_64', '1', 'arm64 arm64'), ('x86_64', '0', 'x64 x86_64')]:
            self.assertEqual(self.shell('detect_architecture; echo "$arch $asset_arch"', MACHINE=machine, ARM_CAPABLE=capable).stdout.strip(), expected)
        self.shell('detect_architecture', success=False, MACHINE='unknown', ARM_CAPABLE='0')

    def test_version_validation(self):
        self.shell('version_supported 15.2 15; version_supported 16 15.9; ! version_supported 14.9 15; ! version_supported 15.1 15.2; ! version_supported invalid 15')

    def test_checksum_requires_one_valid_hash(self):
        self.shell('verify_checksum "$FIXTURE/package.dmg" "$FIXTURE/package.dmg.sha256"')
        for value in ['', 'A'*64+'\n'+'A'*64, '0'*64, 'invalid']:
            self.sum.write_text(value)
            self.shell('verify_checksum "$FIXTURE/package.dmg" "$FIXTURE/package.dmg.sha256"', success=False)

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_release_download_and_validation(self):
        self.shell('obtain_package')
        calls = [c[-1] for c in self.calls('curl')]
        self.assertIn(API, calls)
        self.assertIn(GITEE_API, calls)
        self.assertTrue(GITHUB in calls or GITEE in calls)
        self.assertTrue((self.work / 'snow_shot.app').is_dir())
        self.assertFalse((self.root / 'mounted').exists())
        self.assertFalse(self.calls('security'))

    def test_detach_does_not_depend_on_mount_path_spelling(self):
        self.shell('work="$FIXTURE//work"; local_dmg="$FIXTURE/package.dmg"; obtain_package', CANONICAL_MOUNT_PATH='1')
        self.assertFalse((self.root / 'mounted').exists())
        self.assertEqual(len([call for call in self.calls('hdiutil') if call[1] == 'detach']), 1)
        self.assertFalse(self.calls('mount'))

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_missing_github_uses_gitee(self):
        del self.responses[API]
        self.shell('obtain_package')
        self.assertIn(GITEE, [c[-1] for c in self.calls('curl')])

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_success_cancels_and_reaps_blocked_discovery(self):
        for blocked, winner in [(GITEE_API, API), (API, GITEE_API)]:
            with self.subTest(blocked=blocked):
                for path in self.work.glob('*'):
                    if path.is_dir(): shutil.rmtree(path)
                    else: path.unlink()
                for name in ['blocked-ready', 'blocked-stopped']:
                    (self.root / name).unlink(missing_ok=True)
                self.shell('obtain_package; [[ -z "$github_pid" && -z "$gitee_pid" ]]',
                           BLOCK_URL=blocked, WAIT_FOR_BLOCK_URL=winner)
                self.assertTrue((self.work / 'snow_shot.app/Contents/MacOS/snow_shot').is_file())
                self.assertTrue((self.root / 'blocked-stopped').exists())
                pid = int((self.root / 'blocked-ready').read_text())
                with self.assertRaises(ProcessLookupError): os.kill(pid, 0)

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_invalid_selected_image_falls_back_to_same_version(self):
        bad = self.root / 'bad.dmg'
        bad.write_bytes(b'BAD disk image')
        checksum = self.root / 'bad.sha256'
        checksum.write_text(hashlib.sha256(bad.read_bytes()).hexdigest())
        self.responses[GITHUB] = str(bad)
        self.responses[GITHUB+'.sha256'] = str(checksum)
        # Delay Gitee metadata so GitHub is always selected first.
        self.responses[GITEE_API] = str(self.gitee_release)
        self.shell('obtain_package', DELAY_URL=GITEE_API)
        self.assertIn(GITEE, [c[-1] for c in self.calls('curl')])

    def test_unavailable_download_preserves_installation_and_cleans_up(self):
        self.previous()
        self.responses = {}
        self.shell('trap cleanup EXIT; prepare_state; obtain_package', success=False)
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertFalse(self.work.exists())
        self.assertFalse((self.state / 'lock').exists())
        self.assertFalse(self.calls('security'))

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_real_release_parser_rejects_missing_ambiguous_and_foreign_assets(self):
        for mutation in ['missing', 'duplicate', 'foreign', 'draft', 'malformed']:
            metadata = json.loads(json.dumps(self.metadata))
            if mutation == 'missing': metadata['assets'].pop()
            if mutation == 'duplicate': metadata['assets'].append(metadata['assets'][0])
            if mutation == 'foreign': metadata['assets'][0]['browser_download_url'] = 'https://example.com/a.dmg'
            if mutation == 'draft': metadata['draft'] = True
            self.release.write_text('{' if mutation == 'malformed' else json.dumps([metadata]))
            self.shell('release_urls "$FIXTURE/metadata.json" github', success=False)

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_preview_and_newest_semver(self):
        preview = json.loads(json.dumps(self.metadata))
        preview['tag_name'] = 'v2.0.0-beta_snow-shot'
        preview['prerelease'] = True
        for asset in preview['assets']:
            asset['name'] = asset['name'].replace('1.2.3', '2.0.0-beta')
            asset['browser_download_url'] = asset['browser_download_url'].replace('v1.2.3/', 'v2.0.0-beta_snow-shot/').replace('1.2.3', '2.0.0-beta')
        self.release.write_text(json.dumps([self.metadata, preview]))
        result = self.shell('release_urls "$FIXTURE/metadata.json" github')
        self.assertTrue(result.stdout.startswith('v2.0.0-beta_snow-shot\n'))

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_gitee_attachment_validation_and_exact_version(self):
        result = self.shell('release_urls "$FIXTURE/gitee-metadata.json" gitee-list')
        self.assertEqual(result.stdout.strip(), TAG + ' 123')
        result = self.shell('release_urls "$FIXTURE/gitee-metadata.json" gitee "' + TAG + '" "$FIXTURE/gitee-attachments.json"')
        self.assertIn(GITEE, result.stdout)
        attachments = json.loads(self.gitee_attachments.read_text())
        attachments[0]['browser_download_url'] = 'https://evil.invalid/package.dmg'
        self.gitee_attachments.write_text(json.dumps(attachments))
        self.shell('release_urls "$FIXTURE/gitee-metadata.json" gitee "' + TAG + '" "$FIXTURE/gitee-attachments.json"', success=False)

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires built-in JXA')
    def test_real_release_parser_intel(self):
        self.release.write_text(json.dumps([self.metadata]).replace('arm64', 'x86_64'))
        result = self.shell('asset_arch=x86_64; release_urls "$FIXTURE/metadata.json" github')
        self.assertIn('macos-x86_64.dmg', result.stdout)

    def test_local_package_never_downloads(self):
        self.shell('local_dmg="$FIXTURE/package.dmg"; obtain_package')
        self.assertFalse(self.calls('curl'))

    def test_wrong_bundle_identifier_rejected_and_unmounted(self):
        self.info['CFBundleIdentifier'] = 'other.app'
        self.write_info()
        self.shell('trap cleanup EXIT; local_dmg="$FIXTURE/package.dmg"; obtain_package', success=False)
        self.assertFalse((self.root / 'mounted').exists())
        self.assertFalse(self.work.exists())

    def test_wrong_architecture_rejected(self):
        self.shell('trap cleanup EXIT; local_dmg="$FIXTURE/package.dmg"; obtain_package', success=False, BINARY_ARCH='x86_64')
        self.assertFalse(self.calls('security'))

    def test_intel_local_package_is_validated_and_installed(self):
        self.previous()
        self.shell('trap cleanup EXIT; detect_architecture; local_dmg="$FIXTURE/package.dmg"; '
                   'obtain_package; requirement="$REQUIREMENT"; launch=0; install_application',
                   MACHINE='x86_64', ARM_CAPABLE='0', BINARY_ARCH='x86_64')
        self.assertTrue((self.destination / 'Contents/MacOS/snow_shot').is_file())
        self.assertFalse((self.destination / 'old-marker').exists())
        self.assertFalse(self.calls('curl'))
        self.assertFalse(self.calls('open'))
        self.assertFalse((self.root / 'mounted').exists())

    def test_intel_rejects_arm64_package_before_signing_or_replacement(self):
        self.previous()
        self.shell('trap cleanup EXIT; detect_architecture; local_dmg="$FIXTURE/package.dmg"; '
                   'obtain_package', success=False, MACHINE='x86_64', ARM_CAPABLE='0',
                   BINARY_ARCH='arm64')
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertFalse(self.calls('security'))
        self.assertFalse(any('--force' in call for call in self.calls('codesign')))

    def test_newer_os_requirement_rejected(self):
        self.info['LSMinimumSystemVersion'] = '99.0'
        self.write_info()
        self.shell('trap cleanup EXIT; local_dmg="$FIXTURE/package.dmg"; obtain_package', success=False)

    def test_bad_original_signature_rejected(self):
        (self.bundle / 'reject-signature').touch()
        self.shell('trap cleanup EXIT; local_dmg="$FIXTURE/package.dmg"; obtain_package', success=False)
        self.assertFalse(self.calls('security'))

    def test_attach_failure_does_not_leave_cleanup_mount(self):
        self.shell('trap cleanup EXIT; local_dmg="$FIXTURE/package.dmg"; obtain_package', success=False, FAIL_ATTACH='1')
        self.assertFalse(self.work.exists())

    def test_detach_failure_preserves_mount_and_removes_private_material(self):
        (self.work / 'private.pem').write_text('secret fixture')
        self.shell('trap cleanup EXIT; local_dmg="$FIXTURE/package.dmg"; obtain_package', success=False, FAIL_DETACH='1')
        self.assertTrue(self.work.exists())
        self.assertTrue((self.root / 'mounted').exists())
        self.assertFalse((self.work / 'private.pem').exists())

    def test_new_identity_trust_is_scoped_and_key_access_is_restricted(self):
        self.shell('prepare_identity')
        self.assertEqual((self.state / 'identity').read_text().strip(), FINGERPRINT)
        security = self.calls('security')
        trust = next(c for c in security if c[1] == 'add-trusted-cert')
        self.assertNotIn('-d', trust)
        self.assertEqual(trust[trust.index('-p')+1], 'codeSign')
        imported = next(c for c in security if c[1] == 'import')
        self.assertNotIn('-A', imported)
        self.assertEqual(imported[imported.index('-T')+1], '/usr/bin/codesign')
        self.assertEqual(imported[imported.index('-P')+1], 'B'*64)
        exported = next(c for c in self.calls('openssl') if c[1] == 'pkcs12')
        self.assertEqual(exported[exported.index('-passout')+1],
                         'env:SNOW_INSTALLER_P12_PASSWORD')
        self.assertFalse((self.work / 'private.pem').exists())
        self.assertFalse((self.work / 'identity.p12').exists())

    def test_existing_identity_is_reused(self):
        self.identity()
        self.shell('prepare_identity')
        self.assertEqual([c[1] for c in self.calls('security')], ['find-identity'])
        self.assertFalse(self.calls('openssl'))

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires macOS system LibreSSL')
    def test_native_certificate_generation_without_keychain_changes(self):
        self.shell('''export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 077
security() {
    # Fake only Keychain operations; exercise the actual certificate and PKCS#12 tools.
    if [[ "$1" == find-identity ]]; then printf '1) %s\\n' "$fingerprint"; fi
}
prepare_identity
openssl x509 -in "$work/certificate.pem" -noout -text > "$FIXTURE/certificate.txt"
''')
        description = (self.root / 'certificate.txt').read_text()
        self.assertIn('Code Signing', description)
        self.assertIn('CA:FALSE', description)
        self.assertEqual(len((self.state / 'identity').read_text().strip()), 40)

    @unittest.skipUnless(sys.platform == 'darwin', 'Requires native codesign')
    def test_native_outer_resigning_preserves_embedded_code(self):
        # System binaries supply a fixture without a compiler or developer tools.
        shutil.copyfile('/bin/echo', self.executable)
        helper = self.bundle / 'Contents/MacOS/snow-ocr-process'
        shutil.copyfile('/bin/echo', helper)
        helper.chmod(0o755)
        self.stage()
        # Ad-hoc fixture tests sealing only; permission continuity needs a real identity.
        (self.state / 'requirement').write_text('identifier "com.snowshot.snow_shot"')
        self.shell('''codesign() { /usr/bin/codesign "$@"; }
prepare_identity() { signing_identity=-; }
sign_application
''')
        for path in ['Contents/MacOS/snow-ocr-process', 'Contents/Resources/assets/ocr/asset-manifest.json']:
            self.assertEqual((self.bundle / path).read_bytes(), (self.work / 'snow_shot.app' / path).read_bytes())

    def test_missing_identity_never_regenerates(self):
        self.identity()
        self.shell('prepare_identity', success=False, MISSING_IDENTITY='1')
        self.assertFalse(self.calls('openssl'))

    def test_lowercase_saved_fingerprint_is_accepted(self):
        (self.state / 'identity').write_text(FINGERPRINT.lower())
        self.shell('prepare_identity')
        self.assertFalse(self.calls('openssl'))

    def test_incomplete_identity_never_regenerates(self):
        (self.state / 'creating').touch()
        self.shell('prepare_identity', success=False)
        self.assertFalse(self.calls('openssl'))

    def test_signature_preserves_helpers_and_manifest(self):
        self.stage()
        self.identity()
        self.shell('sign_application')
        for path in ['Contents/MacOS/snow-ocr-process', 'Contents/Resources/assets/ocr/asset-manifest.json']:
            self.assertEqual((self.bundle / path).read_bytes(), (self.work / 'snow_shot.app' / path).read_bytes())
        signing = [c for c in self.calls('codesign') if '--force' in c]
        self.assertEqual(len(signing), 1)
        self.assertNotIn('--deep', signing[0])

    def test_saved_requirement_must_match(self):
        self.stage()
        self.identity()
        (self.state / 'requirement').write_text(REQUIREMENT)
        self.shell('sign_application', success=False, FAIL_REQUIREMENT='1')
        self.assertFalse(self.destination.exists())

    def test_build_specific_requirement_is_never_saved(self):
        self.stage()
        self.identity()
        self.shell('''codesign() {
    if [[ "$1" == -d ]]; then echo '# designated => cdhash H"0123"'; fi
}
sign_application''', success=False)
        self.assertFalse((self.state / 'requirement').exists())

    def test_state_lock_rejects_concurrent_installer(self):
        (self.state / 'lock').mkdir()
        self.shell('prepare_state', success=False)
        self.assertTrue((self.state / 'lock').is_dir())

    def test_state_symlink_is_rejected(self):
        (self.state / 'identity').symlink_to(self.root / 'outside')
        self.shell('trap cleanup EXIT; prepare_state', success=False)
        self.assertFalse((self.root / 'outside').exists())
        self.assertFalse((self.state / 'lock').exists())

    def test_homebrew_backup_does_not_require_signing_key(self):
        self.previous()
        code = 'source ' + shlex.quote(str(PREFLIGHT)) + '; homebrew_prepare_application "$FIXTURE/package.dmg" "$destination"'
        self.shell(code, MISSING_IDENTITY='1')
        self.assertEqual(self.calls(), [['codesign', '--verify', '--deep', '--strict', str(self.destination)]])
        (self.destination / 'reject-signature').touch()
        self.shell(code, success=False)
        self.assertTrue((self.destination / 'old-marker').exists())

    def test_homebrew_backup_rejects_symlink(self):
        self.destination.symlink_to(self.bundle)
        code = 'source ' + shlex.quote(str(PREFLIGHT)) + '; homebrew_prepare_application "$FIXTURE/package.dmg" "$destination"'
        self.shell(code, success=False)
        self.assertFalse(self.calls())

    def test_homebrew_fresh_preflight_forwards_local_paths(self):
        wrapper = self.root / PREFLIGHT.name
        shutil.copyfile(PREFLIGHT, wrapper)
        (self.root / SCRIPT.name).write_text("#!/bin/bash\nprintf '%s\\n' \"$@\"\n")
        for arguments, edition in (([], 'full'), (['full'], 'full'), (['mini'], 'mini')):
            with self.subTest(edition=edition, arguments=arguments):
                result = subprocess.run(
                    ['/bin/bash', str(wrapper), str(self.dmg), str(self.destination), *arguments],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), [
                    '--edition', edition, '--dmg', str(self.dmg),
                    '--prepare-app', str(self.destination)])
                self.assertFalse(self.destination.exists())

    def prepare_code(self):
        return '''trap cleanup EXIT
local_dmg="$FIXTURE/package.dmg"
prepare_app="$FIXTURE/Prepared Snow Shot.app"
validate_prepare_output
prepare_state
obtain_package
sign_application
prepare_application
'''

    def test_prepare_app_reuses_identity_without_install_or_launch(self):
        self.previous()
        self.identity()
        self.shell(self.prepare_code())
        output = self.root / 'Prepared Snow Shot.app'
        self.assertTrue((output / 'Contents/MacOS/snow_shot').exists())
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertEqual((self.state / 'requirement').read_text().strip(), REQUIREMENT)
        for name in ('curl', 'open', 'sudo', 'osascript', 'pgrep', 'openssl'):
            self.assertFalse(self.calls(name), name)
        for path in ['Contents/MacOS/snow-ocr-process', 'Contents/Resources/assets/ocr/asset-manifest.json']:
            self.assertEqual((self.bundle / path).read_bytes(), (output / path).read_bytes())
        self.assertFalse(self.work.exists())
        self.assertFalse((self.state / 'lock').exists())

    def test_prepare_app_requires_local_dmg_and_fresh_absolute_path(self):
        self.previous()
        for output in (str(self.destination), 'relative.app', str(self.root / 'missing/Output.app')):
            self.shell('local_dmg="$FIXTURE/package.dmg"; prepare_app="$OUTPUT"; validate_prepare_output',
                       success=False, OUTPUT=output)
        self.shell('prepare_app="$FIXTURE/New.app"; validate_prepare_output', success=False)
        (self.root / 'link.app').symlink_to(self.root / 'missing')
        self.shell('local_dmg="$FIXTURE/package.dmg"; prepare_app="$FIXTURE/link.app"; validate_prepare_output', success=False)
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertFalse(self.calls('security'))

    def test_prepare_failure_keeps_installed_app_and_cleans_staging(self):
        self.previous()
        self.identity()
        (self.state / 'requirement').write_text(REQUIREMENT)
        self.shell(self.prepare_code(), success=False, FAIL_REQUIREMENT='1')
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertFalse((self.root / 'Prepared Snow Shot.app').exists())
        self.assertFalse(self.work.exists())
        self.assertFalse((self.state / 'lock').exists())
        self.assertEqual((self.state / 'requirement').read_text(), REQUIREMENT)

    def test_prepare_copy_failure_removes_owned_output(self):
        self.previous()
        self.identity()
        code = self.prepare_code().replace('prepare_application', '''ditto() { return 1; }
prepare_application''')
        self.shell(code, success=False)
        self.assertFalse((self.root / 'Prepared Snow Shot.app').exists())
        self.assertFalse((self.state / 'requirement').exists())
        self.assertTrue((self.destination / 'old-marker').exists())

    def test_prepare_bad_checksum_never_signs(self):
        self.sum.write_text('0' * 64)
        self.shell(self.prepare_code(), success=False)
        self.assertFalse(self.calls('security'))
        self.assertFalse((self.root / 'Prepared Snow Shot.app').exists())
        self.assertFalse((self.state / 'lock').exists())

    def test_prepare_concurrent_install_retains_other_lock(self):
        (self.state / 'lock').mkdir()
        self.shell(self.prepare_code(), success=False)
        self.assertTrue((self.state / 'lock').exists())
        self.assertFalse(self.calls('security'))

    def test_legacy_installation_migrates_to_product_name(self):
        self.stage()
        legacy = self.apps / 'snow_shot.app'
        legacy.mkdir()
        (legacy / 'old-marker').write_text('previous application')
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; install_application')
        self.assertFalse(legacy.exists())
        self.assertTrue((self.destination / 'Contents/MacOS/snow_shot').is_file())

    def test_failed_legacy_migration_restores_original_path(self):
        self.stage()
        legacy = self.apps / 'snow_shot.app'
        legacy.mkdir()
        (legacy / 'old-marker').write_text('previous application')
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; install_application',
                   success=False, FAIL_FINAL='1')
        self.assertTrue((legacy / 'old-marker').is_file())
        self.assertFalse(self.destination.exists())

    def test_successful_install_replaces_old_app_and_cleans_backup(self):
        self.previous()
        self.stage()
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; launch=0; install_application')
        self.assertFalse((self.destination / 'old-marker').exists())
        self.assertTrue((self.destination / 'Contents/MacOS/snow_shot').exists())
        self.assertFalse(list(self.apps.glob('.snow-shot-install.*')))
        self.assertEqual((self.state / 'requirement').read_text().strip(), REQUIREMENT)
        self.assertFalse(self.calls('open'))
        self.assertFalse(self.calls('sudo'))

    def test_final_verification_failure_restores_old_app(self):
        self.previous()
        self.stage()
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; install_application', success=False, FAIL_FINAL='1')
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertFalse(list(self.apps.glob('.snow-shot-install.*')))
        self.assertFalse((self.state / 'requirement').exists())

    def test_failed_first_install_leaves_no_partial_app(self):
        self.stage()
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; install_application', success=False, FAIL_FINAL='1')
        self.assertFalse(self.destination.exists())

    def test_launch_failure_keeps_successful_install(self):
        self.previous()
        self.stage()
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; install_application', success=False, FAIL_OPEN='1')
        self.assertTrue((self.destination / 'Contents/MacOS/snow_shot').exists())
        self.assertFalse((self.destination / 'old-marker').exists())

    def test_running_app_is_never_force_killed(self):
        self.previous()
        self.stage()
        self.shell('trap cleanup EXIT; requirement="$REQUIREMENT"; install_application', success=False, RUNNING='1')
        self.assertTrue((self.destination / 'old-marker').exists())
        self.assertEqual(len(self.calls('sleep')), 20)

    def test_interrupt_between_moves_restores_previous_app(self):
        self.previous()
        self.shell('trap cleanup EXIT; previous_destination="$destination"; slot=$(mktemp -d "$FIXTURE/Applications/.snow-shot-install.XXXXXX"); mv "$destination" "$slot/previous.app"; exit 143', success=False)
        self.assertTrue((self.destination / 'old-marker').exists())

    def test_failed_recovery_preserves_backup(self):
        self.previous()
        self.shell('''trap cleanup EXIT
previous_destination="$destination"; slot=$(mktemp -d "$FIXTURE/Applications/.snow-shot-install.XXXXXX")
mv "$destination" "$slot/previous.app"
as_install() { return 1; }
exit 1''', success=False)
        backups = list(self.apps.glob('.snow-shot-install.*/previous.app/old-marker'))
        self.assertEqual(len(backups), 1)

    def test_root_rejected_before_keychain_operations(self):
        # Shell functions survive main's PATH reset, allowing safe bootstrap tests.
        result = self.shell('''uname() { echo Darwin; }
id() { echo 0; }
unset SUDO_USER
main --lang en''', success=False)
        self.assertIn('desktop user', result.stderr)
        self.assertFalse(self.calls('security'))

    def test_sudo_handoff_precedes_keychain_operations(self):
        bootstrap = '''uname() { echo Darwin; }
id() { if [[ $# == 1 ]]; then echo 0; else echo 501; fi; }
stat() { echo alice; }
sudo() { [[ "$1 $2 $3 $4" == '-H -u alice --' ]]; exit $?; }
exec() { "$@"; }
SUDO_USER=alice
'''
        self.shell(bootstrap + 'main --lang en')
        self.shell(bootstrap + 'main')
        self.assertFalse(self.calls('security'))

    def test_privileged_wrapper_only_escalates_when_needed(self):
        self.shell('as_install true; needs_sudo=1; as_install true')
        self.assertEqual(self.calls('sudo'), [['sudo', '--', 'true']])


if __name__ == '__main__':
    unittest.main()
