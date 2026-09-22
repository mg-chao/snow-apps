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

    def test_wrong_version_fails_before_build(self):
        self.request['version'] = '1.2.4'
        with self.assertRaisesRegex(ValueError, 'versions must match'):
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
