"""Deterministic remote packaging tests. No SSH, native build, or production access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('remote', Path(__file__).with_name('snow-shot-remote-macos.py'))
remote = importlib.util.module_from_spec(spec)
spec.loader.exec_module(remote)


class RemotePackageTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.repo = Path(temp.name)
        (self.repo / 'CMakeLists.txt').write_text('set(SNOW_SHOT_VERSION "1.2.3-beta")')
        self.build = self.repo / 'build/snow-shot-macos-arm64-release'
        self.build.mkdir(parents=True)
        self.image = self.build / 'snow-shot-1.2.3-beta-macos-arm64.dmg'
        self.request = {'projectDirectory': str(self.repo), 'version': '1.2.3-beta',
                        'id': 'a' * 32, 'parallelism': 4, 'skipBuild': False}
        self.source = {'commit': 'test-commit', 'workingTreeSha256': 'dirty-source-hash'}
        for name, value in [('sys.platform', 'darwin'),
                            ('os.uname', lambda: SimpleNamespace(machine='arm64'))]:
            mock = patch.object(remote.sys, 'platform', value) if name == 'sys.platform' else patch.object(remote.os, 'uname', value, create=True)
            mock.start()
            self.addCleanup(mock.stop)
        identity = patch.object(remote, 'source_identity', return_value=self.source)
        self.identity = identity.start()
        self.addCleanup(identity.stop)
        run = patch.object(remote.subprocess, 'run', side_effect=self.run_command)
        self.run = run.start()
        self.addCleanup(run.stop)

    def write_package(self, checksum=None):
        self.image.write_bytes(b'native DMG fixture')
        self.image.with_suffix('.dmg.sha256').write_text(
            (checksum or remote.digest(self.image)) + '  ' + self.image.name + '\n')
        for name in ['snow_shot', 'snow-ocr-process', 'snow_shot_mini']:
            path = self.build / 'symbols' / (name + '.dSYM') / 'Contents/Resources/DWARF' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('DWARF fixture: ' + name).encode())

    def run_command(self, command, **kwargs):
        if command[0] == 'bash':
            self.write_package()
        return SimpleNamespace(returncode=0)

    def test_build_stages_verified_immutable_copy_and_receipt(self):
        result = remote.package(self.request)
        self.assertEqual(Path(result['path']).read_bytes(), self.image.read_bytes())
        self.assertEqual(result['source'], self.source)
        self.assertEqual(result['sha256'], hashlib.sha256(self.image.read_bytes()).hexdigest())
        self.assertEqual([call.args[0][0] for call in self.run.call_args_list], ['bash', 'hdiutil', 'codesign'])
        self.assertFalse((self.repo / 'artifacts/.macos-release.lock').exists())
        receipt = json.loads((self.build / 'remote-release-source.json').read_text())
        self.assertEqual(receipt['sha256'], result['sha256'])
        self.assertEqual(len(receipt['symbols']), 2)

    def test_wrong_version_fails_before_build(self):
        self.request['version'] = '1.2.4'
        with self.assertRaisesRegex(ValueError, 'versions must match'):
            remote.package(self.request)
        self.run.assert_not_called()

    def select_intel(self):
        self.request['architecture'] = 'x64'
        self.build = self.repo / 'build/snow-shot-macos-x64-release'
        self.build.mkdir(parents=True)
        self.image = self.build / 'snow-shot-1.2.3-beta-macos-x86_64.dmg'

    def test_intel_build_on_apple_silicon_stages_only_full(self):
        self.select_intel()
        result = remote.package(self.request)
        self.assertEqual(result['architecture'], 'x64')
        self.assertEqual([item['product'] for item in result['images']], ['snow-shot'])
        self.assertEqual(len(result['symbols']), 2)
        self.assertEqual(Path(result['path']).name, 'snow-shot_macos-x86_64.dmg')
        build = self.run.call_args_list[0].args[0]
        self.assertEqual(build[2], 'snow-shot-macos-x64-release')
        self.assertEqual(json.loads((self.build / 'remote-release-source.json').read_text())
                         ['architecture'], 'x64')
        self.assertFalse(list(Path(result['path']).parent.glob('*mini*')))

    def test_intel_release_runs_on_an_intel_host(self):
        self.select_intel()
        with patch.object(remote.os, 'uname', return_value=SimpleNamespace(machine='x86_64')):
            result = remote.package(self.request)
        self.assertEqual(result['architecture'], 'x64')

    def test_intel_rejects_mini_before_build_or_staging(self):
        self.request.update(architecture='x64', editions=['Full', 'Mini'])
        with self.assertRaisesRegex(ValueError, 'Full edition only'):
            remote.package(self.request)
        self.run.assert_not_called()
        self.assertFalse((self.repo / 'artifacts').exists())

    def test_invalid_architecture_and_unsupported_hosts_fail_before_build(self):
        self.request['architecture'] = 'x86_64'
        with self.assertRaisesRegex(ValueError, 'architecture'):
            remote.package(self.request)
        self.request['architecture'] = 'arm64'
        with patch.object(remote.os, 'uname', return_value=SimpleNamespace(machine='x86_64')):
            with self.assertRaisesRegex(ValueError, 'Apple Silicon'):
                remote.package(self.request)
        with patch.object(remote.sys, 'platform', 'linux'):
            with self.assertRaisesRegex(ValueError, 'supported Mac'):
                remote.package(self.request)
        self.run.assert_not_called()

    def test_historical_receipt_remains_valid_for_arm64_only(self):
        remote.package(self.request)
        receipt = self.build / 'remote-release-source.json'
        previous = json.loads(receipt.read_text())
        previous.pop('architecture')
        receipt.write_text(json.dumps(previous))
        self.request.update(skipBuild=True, id='b' * 32)
        self.run.reset_mock()
        self.assertEqual(remote.package(self.request)['architecture'], 'arm64')
        self.assertEqual([call.args[0][0] for call in self.run.call_args_list],
                         ['hdiutil', 'codesign'])

    def test_intel_cached_package_requires_an_explicit_matching_architecture(self):
        self.select_intel()
        remote.package(self.request)
        receipt = self.build / 'remote-release-source.json'
        original = json.loads(receipt.read_text())
        self.request.update(skipBuild=True, id='b' * 32)
        for architecture in ('arm64', None):
            previous = original.copy()
            if architecture is None:
                previous.pop('architecture')
            else:
                previous['architecture'] = architecture
            receipt.write_text(json.dumps(previous))
            self.run.reset_mock()
            with self.subTest(architecture=architecture), self.assertRaisesRegex(ValueError,
                                                                                'architecture'):
                remote.package(self.request)
            self.run.assert_not_called()

    def test_intel_cached_receipt_still_pins_package_bytes_and_symbols(self):
        self.select_intel()
        remote.package(self.request)
        self.request.update(skipBuild=True, id='b' * 32)
        binary = self.build / 'symbols/snow_shot.dSYM/Contents/Resources/DWARF/snow_shot'
        binary.write_bytes(b'changed diagnostics')
        self.run.reset_mock()
        with self.assertRaisesRegex(ValueError, 'symbols differ'):
            remote.package(self.request)
        self.run.assert_not_called()

    def test_cargo_worker_symbols_retain_the_hashed_executable_name(self):
        self.write_package()
        path = self.build / 'symbols/snow-ocr-process.dSYM/Contents/Resources/DWARF/snow-ocr-process'
        hashed = path.with_name('snow_ocr_process-2682fb017056abac')
        path.rename(hashed)
        symbols = remote.symbols_inventory(self.build, ['snow-shot', 'snow-shot-mini'])
        self.assertEqual(symbols[1]['file'], str(hashed.relative_to(self.build)))
        self.assertEqual(symbols[1]['sha256'], remote.digest(hashed))
        self.assertEqual(symbols[1]['size'], hashed.stat().st_size)

    def test_worker_symbols_require_exactly_one_dwarf_file(self):
        self.write_package()
        path = self.build / 'symbols/snow-ocr-process.dSYM/Contents/Resources/DWARF/unexpected'
        path.write_bytes(b'extra DWARF')
        with self.assertRaisesRegex(ValueError, 'symbols'):
            remote.symbols_inventory(self.build, ['snow-shot'])

    def test_paired_images_are_audited_and_recorded_together(self):
        self.request['editions'] = ['Full', 'Mini']
        mini = self.build / 'snow-shot-mini-1.2.3-beta-macos-arm64.dmg'
        def paired(command, **kwargs):
            if command[0] == 'bash':
                self.write_package()
                mini.write_bytes(b'mini fixture')
                mini.with_suffix('.dmg.sha256').write_text(remote.digest(mini) + '  ' + mini.name)
            return SimpleNamespace(returncode=0)
        self.run.side_effect = paired
        result = remote.package(self.request)
        self.assertEqual([item['product'] for item in result['images']], ['snow-shot', 'snow-shot-mini'])
        self.assertEqual(len(result['symbols']), 3)
        self.assertEqual(Path(result['images'][1]['path']).read_bytes(), mini.read_bytes())
        self.assertEqual([call.args[0][0] for call in self.run.call_args_list],
                         ['bash', 'hdiutil', 'codesign', 'hdiutil', 'codesign'])
        self.request.update(skipBuild=True, id='b' * 32)
        mini.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'source receipt'):
            remote.package(self.request)

    def test_paired_cached_symbols_reject_corruption(self):
        self.request['editions'] = ['Full', 'Mini']
        def paired(command, **kwargs):
            if command[0] == 'bash':
                self.write_package()
                image = self.build / 'snow-shot-mini-1.2.3-beta-macos-arm64.dmg'
                image.write_bytes(b'mini fixture')
                image.with_suffix('.dmg.sha256').write_text(remote.digest(image) + '  ' + image.name)
            return SimpleNamespace(returncode=0)
        self.run.side_effect = paired
        remote.package(self.request)
        self.request.update(skipBuild=True, id='b' * 32)
        path = self.build / 'symbols/snow_shot_mini.dSYM/Contents/Resources/DWARF/snow_shot_mini'
        path.write_bytes(b'corrupted Mini symbols')
        self.run.reset_mock()
        with self.assertRaisesRegex(ValueError, 'symbols differ'):
            remote.package(self.request)
        self.run.assert_not_called()

    def test_concurrent_packaging_is_rejected(self):
        (self.repo / 'artifacts/.macos-release.lock').mkdir(parents=True)
        with self.assertRaises(FileExistsError):
            remote.package(self.request)
        self.run.assert_not_called()
        self.assertTrue((self.repo / 'artifacts/.macos-release.lock').exists())

    def test_build_failure_releases_lock(self):
        self.run.side_effect = RuntimeError('build failed')
        with self.assertRaisesRegex(RuntimeError, 'build failed'):
            remote.package(self.request)
        self.assertFalse((self.repo / 'artifacts/.macos-release.lock').exists())

    def test_checksum_corruption_rejected(self):
        self.run.side_effect = lambda *args, **kwargs: self.write_package('0' * 64)
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            remote.package(self.request)

    def test_edit_during_build_rejected(self):
        self.identity.side_effect = [self.source, {'commit': 'changed'}]
        with self.assertRaisesRegex(ValueError, 'source changed'):
            remote.package(self.request)
        self.assertFalse((self.build / 'remote-release-source.json').exists())

    def test_skip_build_requires_matching_receipt_and_hash(self):
        remote.package(self.request)
        self.request.update(skipBuild=True, id='b' * 32)
        self.run.reset_mock()
        remote.package(self.request)
        self.assertEqual([call.args[0][0] for call in self.run.call_args_list], ['hdiutil', 'codesign'])
        self.request['id'] = 'c' * 32
        self.image.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'source receipt'):
            remote.package(self.request)

    def test_skip_build_rejects_changed_sources(self):
        remote.package(self.request)
        self.request.update(skipBuild=True, id='b' * 32)
        self.identity.return_value = {'commit': 'other commit'}
        with self.assertRaisesRegex(ValueError, 'source receipt differs'):
            remote.package(self.request)


class SourceIdentityTests(unittest.TestCase):
    def test_tracks_local_edits_and_untracked_files_but_not_build_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            def git(*args):
                subprocess.run(['git', '-C', str(repo), *args], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            git('init')
            (repo / '.gitignore').write_text('build/\nartifacts/\n')
            (repo / 'source.cpp').write_text('original')
            git('add', '.')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-m', 'fixture')
            original = remote.source_identity(repo)
            (repo / 'source.cpp').write_text('edited')
            edited = remote.source_identity(repo)
            self.assertNotEqual(original, edited)
            (repo / 'new.cpp').write_text('new source')
            added = remote.source_identity(repo)
            self.assertNotEqual(edited, added)
            (repo / 'build').mkdir()
            (repo / 'build/package.dmg').write_bytes(b'ignored')
            self.assertEqual(added, remote.source_identity(repo))


if __name__ == '__main__':
    unittest.main()
