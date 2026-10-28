#!/usr/bin/env python3
"""Focused Cargo notice collection and CMake installation regression tests."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    'rust_licenses', ROOT / 'scripts/collect-rust-licenses.py')
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class RustLicenseCollection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snow Rust notices ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.destination = self.root / 'notice bundle'

    def package(self, name, license_text=None, declared='MIT'):
        directory = self.root / name
        directory.mkdir()
        (directory / 'Cargo.toml').write_text('')
        if license_text is not None:
            (directory / 'LICENSE-MIT').write_text(license_text, encoding='utf-8')
        return {'id': name, 'name': name, 'version': '1.0.0',
                'source': 'registry+fixture', 'manifest_path': str(directory / 'Cargo.toml'),
                'license': declared}

    def test_tree_selects_runtime_features_and_excludes_metadata_only_packages(self):
        root = self.package('root')
        root['source'] = None
        runtime = self.package('runtime', 'runtime notice')
        unused = self.package('development-only')
        metadata = {'packages': [root, runtime, unused]}
        responses = [subprocess.CompletedProcess([], 0, json.dumps(metadata)),
                     subprocess.CompletedProcess([], 0, 'root v1.0.0\nruntime v1.0.0\n')]
        with patch.object(collector.subprocess, 'run', side_effect=responses) as run:
            selected = collector.resolved_packages(
                'cargo', Path(root['manifest_path']), 'root', 'aarch64-apple-darwin',
                'selected-text,language-detection', True)
        self.assertEqual(selected, [runtime])
        for call in run.call_args_list:
            command = call.args[0]
            for argument in ('--locked', '--offline', '--no-default-features',
                             'selected-text,language-detection', 'aarch64-apple-darwin'):
                self.assertIn(argument, command)
        self.assertIn('normal,build', run.call_args_list[1].args[0])

    def test_original_transitive_notices_and_versioned_provenance_are_copied(self):
        transitive = self.package('transitive', 'original transitive copyright\n')
        whatlang = self.package('whatlang')
        whatlang['version'] = '0.18.0'
        objc2 = self.package('objc2')
        objc2['version'] = '0.6.5'
        collector.collect_licenses(
            [transitive, whatlang, objc2], self.destination, ROOT / 'licenses', 'full')
        for label, license_name in (('whatlang-0.18.0', 'LICENSE'), ('objc2-0.6.5', 'LICENSE.md')):
            upstream = ROOT / 'licenses/cargo' / label
            for name in (license_name, 'SOURCE.md'):
                self.assertEqual((self.destination / label / name).read_bytes(),
                                 (upstream / name).read_bytes())
            self.assertIn(f'{label}/{license_name}',
                          (self.destination / 'full.md').read_text())
        self.assertEqual((self.destination / 'transitive-1.0.0/LICENSE-MIT').read_text(),
                         'original transitive copyright\n')

    def test_missing_notice_fails_before_writing_any_output(self):
        valid = self.package('valid', 'original notice')
        missing = self.package('missing')
        with self.assertRaisesRegex(ValueError, 'missing 1.0.0'):
            collector.collect_licenses(
                [valid, missing], self.destination, ROOT / 'licenses', 'full')
        self.assertFalse(self.destination.exists())

    def test_only_declared_canonical_license_fallbacks_are_used(self):
        apache = self.package('apache', declared='Apache-2.0 OR MIT')
        notices = collector.license_files(apache, ROOT / 'licenses')
        self.assertEqual(notices, [ROOT / 'licenses/Apache-2.0.txt'])
        mit = self.package('mit-only')
        with self.assertRaises(ValueError):
            collector.license_files(mit, ROOT / 'licenses')

    def test_sidecars_cannot_overwrite_a_conflicting_original_notice(self):
        package = self.package('shared', 'first original notice')
        collector.collect_licenses([package], self.destination, ROOT / 'licenses', 'first-root')
        Path(package['manifest_path']).with_name('LICENSE-MIT').write_text('different notice')
        with self.assertRaisesRegex(ValueError, 'Conflicting original Rust notices'):
            collector.collect_licenses(
                [package], self.destination, ROOT / 'licenses', 'second-root')
        self.assertEqual((self.destination / 'shared-1.0.0/LICENSE-MIT').read_text(),
                         'first original notice')

    def test_path_submodule_keeps_component_notices(self):
        package = self.package('simd-paddle-ocr-rs', 'Apache notice', 'Apache-2.0')
        package['source'] = None
        directory = Path(package['manifest_path']).parent
        (directory / 'NOTICE').write_text('upstream attribution')
        (directory / 'licenses').mkdir()
        for name in ('dotnet-runtime-MIT.txt', 'amd-win-libm-MIT.txt'):
            (directory / 'licenses' / name).write_text('original ' + name)
        responses = [subprocess.CompletedProcess([], 0, json.dumps({'packages': [package]})),
                     subprocess.CompletedProcess([], 0, 'simd-paddle-ocr-rs v1.0.0\n')]
        with patch.object(collector.subprocess, 'run', side_effect=responses):
            selected = collector.resolved_packages('cargo', directory / 'Cargo.toml',
                                                  package['name'], 'aarch64-apple-darwin', '', False)
        self.assertEqual(selected, [package])
        collector.collect_licenses(selected, self.destination, ROOT / 'licenses', 'worker')
        self.assertEqual({p.name for p in (self.destination / 'simd-paddle-ocr-rs-1.0.0').iterdir()},
                         {'LICENSE-MIT', 'NOTICE', 'dotnet-runtime-MIT.txt', 'amd-win-libm-MIT.txt'})

    @unittest.skipUnless(os.name == 'posix' and shutil.which('cmake'),
                         'CMake installation fixture requires a POSIX host and CMake')
    def test_cmake_installs_each_editions_built_features_and_propagates_errors(self):
        root = self.package('root')
        root['source'] = None
        transitive = self.package('transitive', 'original transitive notice')
        whatlang = self.package('whatlang')
        whatlang['version'] = '0.18.0'
        unused = self.package('development-only')
        shared_root = self.package('shared')
        shared_root['source'] = None
        shared_dependency = self.package('shared-dependency', 'original shared notice')
        metadata = {'packages': [root, transitive, whatlang, unused,
                                 shared_root, shared_dependency]}
        cargo = self.root / 'fake cargo'
        cargo.write_text(
            '#!/usr/bin/env python3\nimport json, sys\n'
            f'metadata = {metadata!r}\n'
            "if sys.argv[1] == 'metadata':\n"
            "    print(json.dumps(metadata))\n"
            "else:\n"
            "    package = sys.argv[sys.argv.index('--package') + 1]\n"
            "    print(package + ' v1.0.0\\ntransitive v1.0.0')\n"
            "    if package == 'shared':\n"
            "        print('shared-dependency v1.0.0')\n"
            "    if '--features' in sys.argv and 'language-detection' in sys.argv:\n"
            "        print('whatlang v0.18.0')\n", encoding='utf-8')
        cargo.chmod(0o755)
        (self.root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 4.2)
project(RustNotices NONE)
set(CARGO_EXECUTABLE "{cargo.as_posix()}")
include("{ROOT.as_posix()}/cmake/SnowRustLicenses.cmake")
foreach(edition IN ITEMS Full Mini Shared)
    add_library(${{edition}} INTERFACE)
    set_target_properties(${{edition}} PROPERTIES
        SNOW_RUST_MANIFEST_DIR "{Path(root['manifest_path']).parent.as_posix()}"
        SNOW_RUST_PACKAGE root
        SNOW_RUST_TARGET aarch64-apple-darwin
        SNOW_RUST_FEATURES "")
endforeach()
set_target_properties(Full PROPERTIES SNOW_RUST_FEATURES language-detection)
set_target_properties(Shared PROPERTIES SNOW_RUST_PACKAGE shared)
snow_install_rust_licenses(COMPONENT Full DESTINATION full/cargo TARGETS Full Shared)
snow_install_rust_licenses(COMPONENT Mini DESTINATION mini/cargo TARGETS Mini Shared)
''', encoding='utf-8')
        build = self.root / 'build'
        stage = self.root / 'installed notices'
        subprocess.run(['cmake', '-S', str(self.root), '-B', str(build)], check=True,
                       capture_output=True, text=True)
        for edition in ('Full', 'Mini'):
            subprocess.run(['cmake', '--install', str(build), '--component', edition,
                            '--prefix', str(stage)], check=True, capture_output=True, text=True)
        self.assertTrue((stage / 'full/cargo/whatlang-0.18.0/LICENSE').is_file())
        self.assertFalse((stage / 'mini/cargo/whatlang-0.18.0').exists())
        for edition in ('full', 'mini'):
            self.assertTrue((stage / edition / 'cargo/transitive-1.0.0/LICENSE-MIT').is_file())
            self.assertTrue((stage / edition / 'cargo/shared-dependency-1.0.0/LICENSE-MIT').is_file())
            self.assertFalse((stage / edition / 'cargo/development-only-1.0.0').exists())
        # A path dependency can change its notices without changing its package
        # version. Reinstalling must replace the previous bundle, then merge all
        # roots from this installation without carrying obsolete packages along.
        Path(transitive['manifest_path']).with_name('LICENSE-MIT').write_text('updated notice')
        for edition in ('Full', 'Mini'):
            destination = stage / edition.lower() / 'cargo'
            obsolete = destination / 'obsolete-0.1.0/LICENSE'
            obsolete.parent.mkdir()
            obsolete.write_text('previous build')
            subprocess.run(['cmake', '--install', str(build), '--component', edition,
                            '--prefix', str(stage)], check=True, capture_output=True, text=True)
            self.assertEqual((destination / 'transitive-1.0.0/LICENSE-MIT').read_text(),
                             'updated notice')
            self.assertTrue((destination / 'shared-dependency-1.0.0/LICENSE-MIT').is_file())
            self.assertTrue((destination / 'root.md').is_file())
            self.assertTrue((destination / 'shared.md').is_file())
            self.assertEqual((destination / 'whatlang-0.18.0/LICENSE').is_file(), edition == 'Full')
            self.assertFalse(obsolete.parent.exists())
        Path(transitive['manifest_path']).with_name('LICENSE-MIT').unlink()
        failed = subprocess.run(
            ['cmake', '--install', str(build), '--component', 'Full', '--prefix', str(stage)],
            capture_output=True, text=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('Rust license collection failed', failed.stderr)


if __name__ == '__main__':
    unittest.main()
