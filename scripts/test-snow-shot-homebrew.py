#!/usr/bin/env python3
"""Focused Homebrew release checks; no network, signing, or tap mutations."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('homebrew', Path(__file__).with_name('snow-shot-homebrew.py'))
brew = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(brew)


class HomebrewTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'source'
        (self.source / 'scripts').mkdir(parents=True)
        # Model the tagged LF source used by packaging, even on core.autocrlf Windows checkouts.
        self.installer = Path(__file__).with_name('install-snow-shot-macos.sh').read_bytes().replace(b'\r\n', b'\n')
        (self.source / 'scripts/install-snow-shot-macos.sh').write_bytes(self.installer)
        self.preflight = Path(__file__).with_name('prepare-snow-shot-homebrew.sh').read_bytes().replace(b'\r\n', b'\n')
        (self.source / 'scripts/prepare-snow-shot-homebrew.sh').write_bytes(self.preflight)
        (self.source / 'CMakeLists.txt').write_text('set(SNOW_SHOT_VERSION "1.2.3")\n')
        self.assets = self.root / 'assets'
        self.assets.mkdir()
        self.name = 'snow-shot-1.2.3-macos-arm64.dmg'
        (self.assets / self.name).write_bytes(b'fixture dmg')
        self.sha = hashlib.sha256(b'fixture dmg').hexdigest()
        (self.assets / (self.name + '.sha256')).write_text(self.sha + '  ' + self.name + '\n')
        self.release = dict(tag_name='v1.2.3_snow-shot', draft=False, prerelease=False,
                            assets=[dict(name=self.name), dict(name=self.name + '.sha256')])
        self.output = self.root / 'output'
        self.tap = self.root / 'tap'
        self.current = self.tap / 'Casks/snow-shot.rb'

    def package(self):
        return brew.package(self.release, self.source, self.assets, self.output, self.current)

    def current_version(self, version):
        self.current.parent.mkdir(parents=True, exist_ok=True)
        self.current.write_text(brew.cask(version, '0' * 64))

    def test_archive_and_cask_are_pinned_and_reproducible(self):
        archive, cask = self.package()
        first = archive.read_bytes()
        with tarfile.open(archive) as tar:
            self.assertEqual(tar.getnames(), [self.name, self.name + '.sha256', 'install-snow-shot-macos.sh', 'prepare-snow-shot-homebrew.sh'])
            self.assertEqual(tar.extractfile('install-snow-shot-macos.sh').read(), self.installer)
            self.assertEqual(tar.extractfile(self.name).read(), b'fixture dmg')
            self.assertEqual(tar.extractfile('prepare-snow-shot-homebrew.sh').read(), self.preflight)
            self.assertTrue(all(item.uid == 0 and item.mtime == 0 for item in tar))
        self.assertIn(brew.digest(archive), cask)
        self.assertIn('depends_on arch: :arm64', cask)
        self.assertIn('depends_on macos: :sequoia', cask)
        self.assertIn('app "Snow Shot.app"', cask)
        self.assertIn('staged_path.join("prepare-snow-shot-homebrew.sh")', cask)
        self.assertIn('preflight do', cask)
        for forbidden in ('/main/', '/latest/', 'sha256 :no_check', 'zap ', 'sudo', '/Applications/'):
            self.assertNotIn(forbidden, cask)
        self.package()
        self.assertEqual(first, archive.read_bytes())
        if shutil.which('ruby'):
            subprocess.run(['ruby', '-c', str(self.output / 'Casks/snow-shot.rb')], check=True, capture_output=True)

    @unittest.skipUnless(shutil.which('ruby'), 'Requires Ruby for preflight execution')
    def test_preflight_passes_versioned_local_paths_and_propagates_errors(self):
        self.package()
        runner = self.root / 'preflight.rb'
        runner.write_text('''require "json"
require "pathname"
class Fixture
  def version(value = nil)
    @version = value if value
    @version
  end
  def method_missing(*); end
  def staged_path; Pathname(ARGV[1]); end
  def preflight(&block); instance_eval(&block); end
  def system_command(executable, **options)
    puts JSON.generate([executable, options])
  end
end
def cask(_name, &block)
  Fixture.new.instance_eval(&block)
end
load ARGV[0]
''')
        result = subprocess.run(['ruby', str(runner), str(self.output / 'Casks/snow-shot.rb'), str(self.output)],
                                capture_output=True, text=True, check=True)
        executable, options = json.loads(result.stdout)
        self.assertEqual(executable, '/bin/bash')
        self.assertEqual(options['args'], [
            str(self.output / 'prepare-snow-shot-homebrew.sh'),
            str(self.output / self.name), str(self.output / 'Snow Shot.app')])
        self.assertTrue(options['must_succeed'])
        self.assertNotIn('sudo', options)

    def test_invalid_releases(self):
        for change in (dict(draft=True), dict(prerelease=True), dict(tag_name='v1.2.3-rc.1_snow-shot'),
                       dict(tag_name='v01.2.3_snow-shot'), dict(tag_name='v1.2.3-beta.01'), dict(tag_name='../../bad')):
            with self.subTest(change=change), self.assertRaises(ValueError):
                brew.release_version(self.release | change)

    def test_supported_tags_and_beta_flags(self):
        for version in ('1.2.3', '1.2.3-beta', '1.2.3-beta.10'):
            for suffix in ('', '_snow-shot'):
                for prerelease in ((False, True) if '-beta' in version else (False,)):
                    release = self.release | dict(tag_name=f'v{version}{suffix}', prerelease=prerelease)
                    self.assertEqual(brew.release_version(release), version)
                    generated = brew.cask(version, self.sha, release['tag_name'])
                    self.assertIn(f'/v#{{version}}{suffix}/', generated)

    def test_beta_publish_preserves_stable_cask(self):
        self.current_version('1.2.3')
        stable = self.current.read_bytes()
        version = '1.2.4-beta.2'
        (self.source / 'CMakeLists.txt').write_text(f'set(SNOW_SHOT_VERSION "{version}")')
        name = f'snow-shot-{version}-macos-arm64.dmg'
        for suffix in ('', '.sha256'):
            (self.assets / (self.name + suffix)).rename(self.assets / (name + suffix))
        self.release.update(tag_name=f'v{version}', assets=[dict(name=name), dict(name=name + '.sha256')])
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
        self.assertEqual(self.current.read_bytes(), stable)
        beta = (self.tap / 'Casks/snow-shot@beta.rb').read_text()
        self.assertIn('cask "snow-shot@beta"', beta)
        self.assertIn('conflicts_with cask: "snow-shot"', beta)
        self.assertIn(('git', 'add', 'Casks/snow-shot@beta.rb', 'README.md'),
                      [call.args for call in run.call_args_list])

    def test_beta_ordering_and_channel_guard(self):
        self.current_version('1.2.3-beta.10')
        for version in ('1.2.3-beta', '1.2.3-beta.2', '1.2.2-beta.99'):
            with self.assertRaisesRegex(ValueError, 'downgrade'):
                brew.check_current(self.current, version)
        brew.check_current(self.current, '1.2.3-beta.11')
        brew.check_current(self.current, '1.2.4-beta')
        with self.assertRaisesRegex(ValueError, 'channel'):
            brew.check_current(self.current, '1.2.4')

    def test_missing_and_duplicate_assets(self):
        for assets in ([], [dict(name=self.name)], self.release['assets'] + [dict(name=self.name)]):
            with self.subTest(assets=assets), self.assertRaises(ValueError):
                brew.required_assets(self.release | dict(assets=assets), '1.2.3')

    def test_checksum_failures(self):
        for checksum in ('', '0' * 64, self.sha + '\n' + self.sha, 'bad'):
            (self.assets / (self.name + '.sha256')).write_text(checksum)
            with self.subTest(checksum=checksum), self.assertRaises(ValueError):
                self.package()
        self.assertFalse(self.output.exists())

    def test_github_digest_without_sidecar(self):
        self.release['assets'] = [dict(name=self.name, digest='sha256:' + self.sha)]
        (self.assets / (self.name + '.sha256')).unlink()
        archive, _ = self.package()
        with tarfile.open(archive) as tar:
            self.assertEqual(tar.extractfile(self.name + '.sha256').read(),
                             (self.sha + '  ' + self.name + '\n').encode())
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
        download = next(call.args for call in run.call_args_list if call.args[:3] == ('gh', 'release', 'download'))
        self.assertNotIn(self.name + '.sha256', download)

    def test_github_digest_is_always_verified(self):
        for value in ('sha256:' + '0' * 64, 'sha512:' + self.sha, ''):
            self.release['assets'][0]['digest'] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.package()
        self.assertFalse(self.output.exists())

    def test_sidecar_cannot_disagree_with_github_digest(self):
        self.release['assets'][0]['digest'] = 'sha256:' + self.sha
        (self.assets / (self.name + '.sha256')).write_text('0' * 64)
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.package()

    def test_source_must_match_and_support_staging(self):
        (self.source / 'CMakeLists.txt').write_text('set(SNOW_SHOT_VERSION "1.2.4")')
        with self.assertRaises(ValueError):
            self.package()
        (self.source / 'CMakeLists.txt').write_text('set(SNOW_SHOT_VERSION "1.2.3")')
        (self.source / 'scripts/install-snow-shot-macos.sh').write_text('# old installer\n')
        with self.assertRaises(ValueError):
            self.package()

    def test_downgrade_and_same_version_conflict(self):
        self.current_version('1.10.0')
        with self.assertRaisesRegex(ValueError, 'downgrade'):
            self.package()
        self.current_version('1.2.3')
        with self.assertRaisesRegex(ValueError, 'already published'):
            self.package()

    def test_identical_cask_retry_and_new_version(self):
        self.current_version('1.2.2')
        archive, generated = self.package()
        self.current.write_text(generated)
        self.assertEqual(self.package(), (archive, generated))

    def test_existing_asset_reused_only_if_identical(self):
        archive, _ = self.package()
        self.release['assets'].append(dict(name=archive.name))
        def download(*args, **kwargs):
            shutil.copyfile(archive, Path(args[-1]) / archive.name)
            return ''
        with patch.object(brew, 'run', side_effect=download) as run:
            brew.ensure_asset(self.release, archive)
            self.assertEqual(run.call_count, 1)
            self.assertIn('download', run.call_args.args)
        def conflict(*args, **kwargs):
            (Path(args[-1]) / archive.name).write_bytes(b'different')
        with patch.object(brew, 'run', side_effect=conflict), self.assertRaisesRegex(ValueError, 'differs'):
            brew.ensure_asset(self.release, archive)

    def fake_run(self, *args, **kwargs):
        if args[:2] == ('gh', 'api'):
            return json.dumps(self.release)
        if args[:3] == ('gh', 'release', 'download'):
            for path in self.assets.iterdir():
                shutil.copyfile(path, Path(args[-1]) / path.name)
        if args[:4] == ('git', 'diff', '--cached', '--name-only'):
            return 'Casks/snow-shot.rb\n'
        return ''

    def test_publish_uploads_before_tap_push(self):
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        commands = [call.args for call in run.call_args_list]
        upload = next(i for i, args in enumerate(commands) if args[:3] == ('gh', 'release', 'upload'))
        push = next(i for i, args in enumerate(commands) if args[:2] == ('git', 'push'))
        self.assertLess(upload, push)
        self.assertTrue(self.current.is_file())
        self.assertTrue((self.tap / 'README.md').is_file())

    def test_missing_assets_leave_tap_and_release_unchanged(self):
        self.release['assets'] = []
        with patch.object(brew, 'run', side_effect=self.fake_run) as run, self.assertRaises(ValueError):
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        self.assertEqual(run.call_count, 1)
        self.assertFalse(self.tap.exists())

    def test_failed_upload_never_changes_tap(self):
        def failing(*args, **kwargs):
            if args[:3] == ('gh', 'release', 'upload'):
                raise subprocess.CalledProcessError(1, args)
            return self.fake_run(*args, **kwargs)
        with patch.object(brew, 'run', side_effect=failing), self.assertRaises(subprocess.CalledProcessError):
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        self.assertFalse(self.tap.exists())


if __name__ == '__main__':
    unittest.main()
