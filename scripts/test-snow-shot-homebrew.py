#!/usr/bin/env python3
"""Focused Homebrew release checks; no network, signing, or tap mutations."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
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
        self.assertIn('installer script:', cask)
        self.assertIn('uninstall script:', cask)
        self.assertIn('["-rf", "--", "#{staged_path}/Snow Shot.app"]', cask)
        for forbidden in ('/main/', '/latest/', 'sha256 :no_check', 'zap ', 'preflight', '/Applications/'):
            self.assertNotIn(forbidden, cask)
        self.package()
        self.assertEqual(first, archive.read_bytes())
        if shutil.which('ruby'):
            subprocess.run(['ruby', '-c', str(self.output / 'Casks/snow-shot.rb')], check=True, capture_output=True)

    @unittest.skipUnless(shutil.which('ruby'), 'Requires Ruby for installer metadata')
    def test_installer_passes_versioned_local_paths_and_requires_success(self):
        self.package()
        runner = self.root / 'installer.rb'
        runner.write_text('''require "json"
require "pathname"
class Fixture
  def version(value = nil)
    @version = value if value
    @version
  end
  def method_missing(*); end
  def staged_path; Pathname(ARGV[1]); end
  def installer(script:)
    puts JSON.generate([script.delete(:executable), script])
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
        self.assertFalse(options['sudo'])

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

    def legacy_cask(self, edition='full', architecture='dual', beta=False):
        fixture = Path(__file__).resolve().parent.parent / 'homebrew/test-support/legacy-snow-shot.rb'
        text = fixture.read_text(encoding='utf-8')
        if architecture != 'dual':
            text = text.replace('  arch arm: "arm64", intel: "x86_64"\n\n', '')
            text = re.sub(r'  sha256 arm:.*\n         intel:.*', '  sha256 "' + '0' * 64 + '"', text)
            text = text.replace('#{arch}', architecture)
            text = text.replace('  depends_on macos:', f'  depends_on arch: :{architecture}\n  depends_on macos:')
        if edition == 'mini':
            text = text.replace('/snow-shot-', '/snow-shot-mini-').replace('"snow-shot-', '"snow-shot-mini-')
            text = text.replace('cask "snow-shot"', 'cask "snow-shot-mini"')
            text = text.replace('Snow Shot', 'Snow Shot Mini')
            text = text.replace('staged_path.join("Snow Shot Mini.app")]',
                                'staged_path.join("Snow Shot Mini.app"),\n                                  "mini"]')
        if beta:
            slug = brew.PRODUCTS[edition][0]
            text = text.replace('1.3.4', '1.3.4-beta.2')
            text = text.replace(f'cask "{slug}"', f'cask "{slug}@beta"')
            text = text.replace('  depends_on ', f'  conflicts_with cask: "{slug}"\n  depends_on ', 1)
        return text

    def test_legacy_migration_preserves_published_metadata_for_every_variant(self):
        for edition, architecture in (('full', 'dual'), ('full', 'arm64'),
                                       ('full', 'x86_64'), ('mini', 'arm64')):
            for beta in (False, True):
                with self.subTest(edition=edition, architecture=architecture, beta=beta):
                    version = '1.3.4-beta.2' if beta else '1.3.4'
                    token = brew.cask_name(version, edition)
                    path = self.tap / f'Casks/{token}.rb'
                    path.parent.mkdir(exist_ok=True, parents=True)
                    path.write_text(self.legacy_cask(edition, architecture, beta))
                    checksums = {'arm64': '0' * 64, 'x64': '1' * 64} if architecture == 'dual' else '0' * 64
                    native_arch = 'x64' if architecture == 'x86_64' else 'arm64'
                    expected = brew.cask(version, checksums, edition=edition, architecture=native_arch)
                    with patch.object(brew, 'run') as run:
                        self.assertEqual(brew.migrate_casks(self.tap), 1)
                        self.assertEqual(path.read_text(), expected)
                        self.assertEqual(brew.migrate_casks(self.tap), 0)
                        run.assert_not_called()
                    path.unlink()

    def test_migration_validates_all_casks_before_changing_any(self):
        self.current.parent.mkdir(parents=True)
        original = self.legacy_cask()
        self.current.write_text(original)
        beta = self.current.with_name('snow-shot@beta.rb')
        for invalid in (self.legacy_cask(beta=True).replace('"/bin/bash"', '"/bin/false"'),
                        self.legacy_cask(beta=True).replace('\n  caveats', '\n  uninstall delete: "/unexpected"\n  caveats'),
                        self.legacy_cask(beta=True).replace('cask "snow-shot@beta"', 'cask "unexpected"')):
            beta.write_text(invalid)
            with self.assertRaises(ValueError):
                brew.migrate_casks(self.tap)
            self.assertEqual(self.current.read_text(), original)
            self.assertEqual(beta.read_text(), invalid)

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

    def test_incomplete_upload_cannot_remove_published_architectures(self):
        self.add_intel()
        complete = self.release['assets'][:]
        for beta in (False, True):
            version = '1.2.3-beta.2' if beta else '1.2.3'
            previous = '1.2.3-beta.1' if beta else '1.2.2'
            self.release['tag_name'] = f'v{version}_snow-shot'
            current = self.tap / f'Casks/{brew.cask_name(version)}.rb'
            current.parent.mkdir(parents=True, exist_ok=True)
            for published in ({'arm64': '0' * 64}, {'x64': '1' * 64},
                              {'arm64': '0' * 64, 'x64': '1' * 64}):
                original = brew.cask(previous, published)
                current.write_text(original)
                for missing in published:
                    suffix = brew.ARCHITECTURES[missing]
                    self.release['assets'] = [
                        asset | dict(name=asset['name'].replace('1.2.3', version))
                        for asset in complete if f'-{suffix}.' not in asset['name']]
                    with self.subTest(beta=beta, published=published, missing=missing), \
                            patch.object(brew, 'run', side_effect=self.fake_run) as run:
                        with self.assertRaisesRegex(ValueError, 'published architecture'):
                            brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
                        self.assertEqual(run.call_count, 1)
                        self.assertEqual(current.read_text(), original)
                        self.assertFalse(self.output.exists())
            current.unlink()

    def test_incomplete_upload_cannot_leave_published_mini_behind(self):
        self.add_intel()
        self.current_version('1.2.2')
        mini = self.current.with_name('snow-shot-mini.rb')
        mini.write_text(brew.cask('1.2.2', '1' * 64, edition='mini'))
        before = {path: path.read_bytes() for path in (self.current, mini)}
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            with self.assertRaisesRegex(ValueError, 'published architecture'):
                brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
        self.assertEqual(run.call_count, 1)
        self.assertEqual({path: path.read_bytes() for path in before}, before)
        self.assertFalse(self.output.exists())

    def test_complete_upload_can_retry_after_incomplete_snapshot(self):
        self.current_version('1.2.2')
        self.current.write_text(brew.cask('1.2.2', {'arm64': '0' * 64, 'x64': '1' * 64}))
        with patch.object(brew, 'run', side_effect=self.fake_run):
            with self.assertRaisesRegex(ValueError, 'published architecture'):
                brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
            self.add_intel()
            brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
        self.assertIn('arch arm: "arm64", intel: "x86_64"', self.current.read_text())
        self.assertIn('version "1.2.3"', self.current.read_text())

    def add_mini(self):
        name = 'snow-shot-mini-1.2.3-macos-arm64.dmg'
        (self.assets / name).write_bytes(b'mini dmg')
        sha = brew.digest(self.assets / name)
        (self.assets / (name + '.sha256')).write_text(sha + '  ' + name + '\n')
        self.release['assets'] += [dict(name=name), dict(name=name + '.sha256')]

    def add_intel(self):
        name = 'snow-shot-1.2.3-macos-x86_64.dmg'
        (self.assets / name).write_bytes(b'intel dmg')
        sha = brew.digest(self.assets / name)
        (self.assets / (name + '.sha256')).write_text(sha + '  ' + name + '\n')
        self.release['assets'] += [dict(name=name, digest='sha256:' + sha),
                                   dict(name=name + '.sha256')]
        return name

    def test_dual_archives_preserve_arm_bytes_and_use_architecture_cask(self):
        arm_archive, historical_cask = self.package()
        historical_bytes = arm_archive.read_bytes()
        intel = self.add_intel()
        archives, generated = brew.package_all(self.release, self.source, self.assets,
                                               self.output, self.current)
        self.assertEqual([archive.name for archive in archives], [
            'snow-shot-1.2.3-macos-arm64-homebrew.tar.gz',
            'snow-shot-1.2.3-macos-x86_64-homebrew.tar.gz'])
        self.assertEqual(archives[0].read_bytes(), historical_bytes)
        self.assertEqual(brew.cask('1.2.3', brew.digest(archives[0])), historical_cask)
        self.assertIn('arch arm: "arm64", intel: "x86_64"', generated)
        self.assertIn(f'sha256 arm:   "{brew.digest(archives[0])}"', generated)
        self.assertIn(f'intel: "{brew.digest(archives[1])}"', generated)
        self.assertIn('macos-#{arch}-homebrew.tar.gz', generated)
        self.assertIn('staged_path.join("snow-shot-#{version}-macos-#{arch}.dmg")', generated)
        self.assertNotIn('depends_on arch:', generated)
        with tarfile.open(archives[1]) as tar:
            self.assertEqual(tar.extractfile(intel).read(), b'intel dmg')
            self.assertEqual(tar.extractfile(intel + '.sha256').read(),
                             (brew.digest(self.assets / intel) + '  ' + intel + '\n').encode())
        first = [archive.read_bytes() for archive in archives]
        self.assertEqual(brew.package_all(self.release, self.source, self.assets,
                                          self.output, self.current), (archives, generated))
        self.assertEqual(first, [archive.read_bytes() for archive in archives])

    @unittest.skipUnless(shutil.which('ruby'), 'Requires Ruby for installer metadata')
    def test_dual_cask_installer_selects_the_local_architecture(self):
        self.add_intel()
        brew.package_all(self.release, self.source, self.assets, self.output, self.current)
        runner = self.root / 'dual-installer.rb'
        runner.write_text('''require "json"
require "pathname"
class Fixture
  def version(value = nil)
    @version = value if value
    @version
  end
  def arch(arm: nil, intel: nil)
    @arch = ARGV[2] == "x64" ? intel : arm if arm || intel
    @arch
  end
  def method_missing(*); end
  def staged_path; Pathname(ARGV[1]); end
  def installer(script:)
    puts JSON.generate([script.delete(:executable), script])
  end
end
def cask(_name, &block)
  Fixture.new.instance_eval(&block)
end
load ARGV[0]
''')
        for architecture, suffix in brew.ARCHITECTURES.items():
            with self.subTest(architecture=architecture):
                result = subprocess.run(['ruby', str(runner),
                                         str(self.output / 'Casks/snow-shot.rb'),
                                         str(self.output), architecture],
                                        capture_output=True, text=True, check=True)
                _, options = json.loads(result.stdout)
                self.assertEqual(options['args'][1],
                                 str(self.output / f'snow-shot-1.2.3-macos-{suffix}.dmg'))

    def test_intel_only_release_with_windows_mini_prepares_only_full(self):
        self.release['assets'] = []
        intel = self.add_intel()
        self.release['assets'] += [dict(name='snow-shot-mini-1.2.3-windows-x64-online.exe'),
                                   dict(name='latest-version-mini.json')]
        self.assertEqual(brew.products_for_release(self.release), ['full'])
        archives, generated = brew.package_all(self.release, self.source, self.assets,
                                               self.output, self.current)
        self.assertEqual(len(archives), 1)
        self.assertIn('macos-x86_64-homebrew.tar.gz', generated)
        self.assertIn('depends_on arch: :x86_64', generated)
        self.assertIn(f'staged_path.join("{intel.replace("1.2.3", "#{version}")}")', generated)
        self.assertNotIn('#{arch}', generated)
        self.assertEqual(brew.package(self.release, self.source, self.assets, self.output,
                                     self.current, architecture='x64'), (archives[0], generated))
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        uploads = [call.args for call in run.call_args_list
                   if call.args[:3] == ('gh', 'release', 'upload')]
        self.assertEqual(len(uploads), 1)
        self.assertFalse((self.tap / 'Casks/snow-shot-mini.rb').exists())

    def test_architecture_discovery_rejects_partial_duplicate_and_invalid_intel_assets(self):
        name = self.add_intel()
        valid = self.release['assets']
        for assets in ([asset for asset in valid if asset['name'] != name],
                       [dict(name=name)] + [asset for asset in valid
                                            if not asset['name'].startswith(name)],
                       valid + [dict(name=name)], valid + [dict(name=name + '.sha256')],
                       [asset | dict(digest='sha512:' + self.sha) if asset['name'] == name
                        else asset for asset in valid]):
            with self.subTest(assets=assets), self.assertRaises(ValueError):
                brew.products_for_release(self.release | dict(assets=assets))

    def test_intel_checksum_failure_prevents_all_archives(self):
        name = self.add_intel()
        (self.assets / (name + '.sha256')).write_text('0' * 64)
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            brew.package_all(self.release, self.source, self.assets, self.output, self.current)
        self.assertFalse(self.output.exists())

    def test_mini_intel_assets_are_rejected(self):
        self.add_mini()
        for suffix in ('.dmg', '.dmg.sha256', '-homebrew.tar.gz'):
            name = 'snow-shot-mini-1.2.3-macos-x86_64' + suffix
            self.release['assets'].append(dict(name=name))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'only.*ARM64'):
                brew.products_for_release(self.release)
            self.release['assets'].pop()
        with self.assertRaisesRegex(ValueError, 'only.*ARM64'):
            brew.cask('1.2.3', {'arm64': self.sha, 'x64': self.sha}, edition='mini')

    def test_adding_intel_to_same_cask_version_is_rejected(self):
        _, generated = self.package()
        self.current.parent.mkdir(parents=True)
        self.current.write_text(generated)
        self.add_intel()
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            with self.assertRaisesRegex(ValueError, 'already published'):
                brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        self.assertEqual(self.current.read_text(), generated)
        self.assertFalse(any(call.args[:3] == ('gh', 'release', 'upload')
                             for call in run.call_args_list))

    def test_explicit_architecture_repair_preserves_existing_archive_and_metadata(self):
        self.add_intel()
        for beta in (False, True):
            version = '1.2.3-beta.2' if beta else '1.2.3'
            release = self.release | dict(tag_name=f'v{version}')
            if beta:
                for path in list(self.assets.iterdir()):
                    path.rename(path.with_name(path.name.replace('1.2.3', version)))
                release['assets'] = [asset | dict(name=asset['name'].replace('1.2.3', version))
                                     for asset in release['assets']]
                (self.source / 'CMakeLists.txt').write_text(f'set(SNOW_SHOT_VERSION "{version}")')
            current = self.tap / f'Casks/{brew.cask_name(version)}.rb'
            current.parent.mkdir(parents=True, exist_ok=True)
            for architecture in brew.ARCHITECTURES:
                with self.subTest(beta=beta, architecture=architecture):
                    archives, old = brew.package_all(release, self.source, self.assets, self.output,
                                                     current, architectures=[architecture])
                    original_archive = archives[0].read_bytes()
                    current.write_text(old)
                    archives, repaired = brew.package_all(
                        release, self.source, self.assets, self.output, current,
                        repair_architectures=True)
                    selected = next(path for path in archives
                                    if f'-{brew.ARCHITECTURES[architecture]}-' in path.name)
                    self.assertEqual(selected.read_bytes(), original_archive)
                    self.assertIn(brew.digest(selected), repaired)
                    self.assertIn('arch arm: "arm64", intel: "x86_64"', repaired)
                    self.assertNotIn('depends_on arch:', repaired)
                    self.assertEqual(current.read_text(), old)  # Local preparation never edits the tap.
                    current.write_text(repaired)
                    self.assertEqual(brew.package_all(
                        release, self.source, self.assets, self.output, current,
                        repair_architectures=True), (archives, repaired))
                    current.unlink()

    def test_architecture_repair_rejects_changed_existing_cask_settings(self):
        archive, original = self.package()
        self.add_intel()
        self.current.parent.mkdir(parents=True)
        for altered in (
                original.replace(brew.digest(archive), '0' * 64),
                original.replace('_snow-shot/', '/'),
                original.replace('must_succeed: true', 'must_succeed: false'),
                original.replace('macos: :sequoia', 'macos: :sonoma'),
                original.replace('Snow Shot.app', 'Different.app')):
            self.current.write_text(altered)
            with self.subTest(altered=altered), \
                    patch.object(brew, 'run', side_effect=self.fake_run) as run:
                with self.assertRaisesRegex(ValueError, 'already published'):
                    brew.publish(self.release['tag_name'], self.source, self.tap, self.output,
                                 repair_architectures=True)
                self.assertEqual(self.current.read_text(), altered)
                self.assertFalse(any(call.args[:3] == ('gh', 'release', 'upload')
                                     or call.args[:1] == ('git',) for call in run.call_args_list))

    def test_architecture_repair_does_not_allow_replacing_dual_cask_bytes(self):
        self.add_intel()
        _, generated = brew.package_all(self.release, self.source, self.assets, self.output, self.current)
        self.current.parent.mkdir(parents=True)
        self.current.write_text(generated)
        (self.assets / self.name).write_bytes(b'changed arm dmg')
        (self.assets / (self.name + '.sha256')).write_text(brew.digest(self.assets / self.name))
        with self.assertRaisesRegex(ValueError, 'already published'):
            brew.package_all(self.release, self.source, self.assets, self.output, self.current,
                             repair_architectures=True)
        self.assertEqual(self.current.read_text(), generated)

    def test_local_packaging_cannot_remove_a_published_architecture(self):
        self.add_intel()
        self.current_version('1.2.2')
        self.current.write_text(brew.cask('1.2.2', {'arm64': '0' * 64, 'x64': '1' * 64}))
        for architecture in brew.ARCHITECTURES:
            with self.subTest(architecture=architecture), self.assertRaisesRegex(ValueError, 'published architecture'):
                brew.package_all(self.release, self.source, self.assets, self.output, self.current,
                                 architectures=[architecture], repair_architectures=True)
            self.assertFalse(self.output.exists())

    def test_unknown_published_architecture_metadata_fails_before_downloads(self):
        self.current_version('1.2.2')
        original = self.current.read_text().replace('depends_on arch: :arm64', 'depends_on arch: :unknown')
        self.current.write_text(original)
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            with self.assertRaisesRegex(ValueError, 'Unrecognized architecture'):
                brew.publish(self.release['tag_name'], self.source, self.tap, self.output)
        self.assertEqual(run.call_count, 1)
        self.assertEqual(self.current.read_text(), original)

    def test_repair_publication_checks_all_existing_archives_before_tap_changes(self):
        self.add_intel()
        intel, original = brew.package(self.release, self.source, self.assets, self.output,
                                       self.current, architecture='x64')
        self.current.parent.mkdir(parents=True)
        self.current.write_text(original)
        self.release['assets'].append(dict(name=intel.name))
        (self.assets / intel.name).write_bytes(b'conflicting published archive')
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            with self.assertRaisesRegex(ValueError, 'Existing Homebrew archive differs'):
                brew.publish(self.release['tag_name'], self.source, self.tap, self.output,
                             repair_architectures=True)
            self.assertFalse(any(call.args[:3] == ('gh', 'release', 'upload')
                                 or call.args[:1] == ('git',) for call in run.call_args_list))
        self.assertEqual(self.current.read_text(), original)
        shutil.copyfile(intel, self.assets / intel.name)
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish(self.release['tag_name'], self.source, self.tap, self.output,
                         repair_architectures=True)
        self.assertIn('arch arm: "arm64", intel: "x86_64"', self.current.read_text())
        self.assertIn(brew.digest(intel), self.current.read_text())

    def test_dual_full_and_arm_mini_publish_all_archives_before_tap_changes(self):
        self.add_intel()
        self.add_mini()
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        commands = [call.args for call in run.call_args_list]
        downloads = next(args for args in commands if args[:3] == ('gh', 'release', 'download'))
        for name in (self.name, 'snow-shot-1.2.3-macos-x86_64.dmg',
                     'snow-shot-mini-1.2.3-macos-arm64.dmg'):
            self.assertIn(name, downloads)
            self.assertIn(name + '.sha256', downloads)
        uploads = [i for i, args in enumerate(commands) if args[:3] == ('gh', 'release', 'upload')]
        tap_change = next(i for i, args in enumerate(commands) if args[:2] == ('git', 'add'))
        self.assertEqual(len(uploads), 3)
        self.assertLess(max(uploads), tap_change)
        self.assertIn('macos-#{arch}', self.current.read_text())
        self.assertIn('depends_on arch: :arm64', (self.tap / 'Casks/snow-shot-mini.rb').read_text())

    def test_mini_identity_and_paired_publication(self):
        self.add_mini()
        archive, cask = brew.package(self.release, self.source, self.assets, self.output,
                                    self.tap / 'Casks/snow-shot-mini.rb', 'mini')
        self.assertEqual(archive.name, 'snow-shot-mini-1.2.3-macos-arm64-homebrew.tar.gz')
        self.assertIn('cask "snow-shot-mini"', cask)
        self.assertIn('app "Snow Shot Mini.app"', cask)
        self.assertIn('"mini"]', cask)
        self.assertIn('/v#{version}_snow-shot/', cask)
        self.assertIn('Support/Snow Shot Mini/Installer', cask)
        beta = brew.cask('1.2.3-beta', '0' * 64, edition='mini')
        self.assertIn('conflicts_with cask: "snow-shot-mini"', beta)
        self.assertNotIn('conflicts_with cask: "snow-shot"', beta)
        with patch.object(brew, 'run', side_effect=self.fake_run) as run:
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        commands = [call.args for call in run.call_args_list]
        uploads = [i for i, args in enumerate(commands) if args[:3] == ('gh', 'release', 'upload')]
        commit = next(i for i, args in enumerate(commands) if args[:2] == ('git', 'add'))
        self.assertEqual(len(uploads), 2)
        self.assertLess(max(uploads), commit)
        self.assertTrue((self.tap / 'Casks/snow-shot-mini.rb').is_file())

    def test_partial_paired_release_never_publishes(self):
        self.add_mini()
        self.release['assets'] = [a for a in self.release['assets'] if a['name'] != self.name]
        with patch.object(brew, 'run', side_effect=self.fake_run) as run, self.assertRaises(ValueError):
            brew.publish('v1.2.3_snow-shot', self.source, self.tap, self.output)
        self.assertEqual(run.call_count, 1)
        self.assertFalse(self.tap.exists())

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
