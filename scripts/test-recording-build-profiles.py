"""Focused Cargo feature-graph contracts for application and compatibility recording."""

import json
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
CARGO = shutil.which('cargo')


@unittest.skipUnless(CARGO, 'Cargo is required')
class RecordingBuildProfiles(unittest.TestCase):
    def cargo(self, *arguments):
        result = subprocess.run([CARGO, *arguments], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def recording_feature_graph(self, manifest, package, target, *features):
        # Cargo tree evaluates the selected target's dependency edges. Metadata
        # feature lists can unify target-specific dependencies and hide a Windows
        # regression caused by macOS's intentional HDR opt-in.
        output = self.cargo(
            'tree', '--manifest-path', manifest, '--locked', '--offline',
            '-p', package, '--target', target, '--edges', 'normal,build',
            '--prefix', 'none', '--format', '{p}|{f}', *features)
        chain = {'snow-recording-c', 'snow-screen-recorder',
                 'snow-recording-runtime', 'snow-recording-export'}
        resolved = {}
        for line in output.splitlines():
            description, separator, active = line.partition('|')
            name = description.split()[0] if description else ''
            if separator and name in chain:
                enabled = {feature for feature in active.removesuffix(' (*)').split(',')
                           if feature}
                resolved.setdefault(name, set()).update(enabled)
        self.assertEqual(set(resolved), chain,
                         f'Recording chain missing from {package} ({target})')
        return resolved

    def test_windows_ffi_excludes_hdr_encoding_for_each_recording_crate(self):
        for target in ['x86_64-pc-windows-msvc', 'aarch64-pc-windows-msvc']:
            for edition, features in [('Mini', ()),
                                      ('Full', ('--features',
                                                'selected-text,language-detection'))]:
                with self.subTest(target=target, edition=edition):
                    graph = self.recording_feature_graph(
                        'snow_rust_ffi/Cargo.toml', 'snow-shot-rust-ffi', target, *features)
                    for package, active in graph.items():
                        self.assertNotIn('hdr-encoding', active, package)
                        self.assertNotIn('legacy-capture', active, package)

    def test_standalone_hdr_encoding_follows_default_features(self):
        cases = [('default', (), True),
                 ('minimal', ('--no-default-features',), False),
                 ('hdr-only', ('--no-default-features', '--features', 'hdr-encoding'), True)]
        for profile, flags, hdr in cases:
            with self.subTest(profile=profile):
                graph = self.recording_feature_graph(
                    'snow-crates/Cargo.toml', 'snow-recording-c',
                    'x86_64-pc-windows-msvc', *flags)
                for package, active in graph.items():
                    self.assertEqual('hdr-encoding' in active, hdr, package)
                    if profile == 'hdr-only':
                        self.assertNotIn('legacy-capture', active, package)

    def test_macos_ffi_retains_hdr_encoding_for_each_recording_crate(self):
        for target in ['aarch64-apple-darwin', 'x86_64-apple-darwin']:
            with self.subTest(target=target):
                graph = self.recording_feature_graph(
                    'snow_rust_ffi/Cargo.toml', 'snow-shot-rust-ffi', target)
                for package, active in graph.items():
                    self.assertIn('hdr-encoding', active, package)
                    self.assertNotIn('legacy-capture', active, package)

    def test_application_excludes_capture_compatibility_and_retains_recovery(self):
        # Match Cargo tree's host-platform scope below. Unfiltered metadata also
        # loads packages for other platforms, which need not be cached after a
        # successful native application build (for example macOS accessibility
        # dependencies on Windows).
        host = next(line.removeprefix('host: ') for line in self.cargo('-vV').splitlines()
                    if line.startswith('host: '))
        metadata = json.loads(self.cargo(
            'metadata', '--manifest-path', 'snow_rust_ffi/Cargo.toml', '--locked',
            '--offline', '--format-version', '1', '--filter-platform', host))
        packages = {package['id']: package['name'] for package in metadata['packages']}
        nodes = {packages[node['id']]: node for node in metadata['resolve']['nodes']}
        for package in ('snow-recording-c', 'snow-screen-recorder', 'snow-recording-runtime'):
            self.assertNotIn('legacy-capture', nodes[package]['features'])
        self.assertNotIn('snow-pipeline', nodes)
        self.assertIn('snow-recording-export', nodes)
        self.assertIn('snow-recording-model', nodes)
        # The Windows direct GPU startup probe reads encoded packets through FFmpeg.
        runtime_dependencies = {packages[dependency['pkg']]
                                for dependency in nodes['snow-recording-runtime']['deps']}
        self.assertIn('ffmpeg-next', runtime_dependencies)

    def compatibility_packages(self, *features):
        output = self.cargo(
            'tree', '--manifest-path', 'snow-crates/Cargo.toml', '--locked', '--offline',
            '-p', 'snow-recording-c', '--edges', 'normal,build', '--prefix', 'none',
            '--format', '{p}', *features)
        return {line.split()[0] for line in output.splitlines()}

    def test_standalone_defaults_retain_legacy_capture(self):
        self.assertIn('snow-pipeline', self.compatibility_packages())

    def test_standalone_can_select_the_application_capture_surface(self):
        packages = self.compatibility_packages('--no-default-features')
        self.assertNotIn('snow-pipeline', packages)
        self.assertIn('snow-recording-export', packages)
        self.assertIn('snow-recording-model', packages)


if __name__ == '__main__':
    unittest.main()
