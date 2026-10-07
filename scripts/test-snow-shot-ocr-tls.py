#!/usr/bin/env python3
"""Verify OCR HTTP features enforce the Rustls security floor during resolution."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[1]
OCR_MANIFEST = ROOT / 'snow-crates/crates/rapid-ocr-rs/Cargo.toml'
WORKSPACE_MANIFEST = ROOT / 'snow-crates/Cargo.toml'


@unittest.skipUnless(shutil.which('cargo'), 'Cargo is required to resolve OCR dependencies')
class OcrTlsSecurity(unittest.TestCase):
    def test_http_features_reject_vulnerable_rustls_without_existing_lockfile(self):
        workspace = tomllib.loads(WORKSPACE_MANIFEST.read_text())
        for feature in ('remote-input', 'model-download'):
            with self.subTest(feature=feature), tempfile.TemporaryDirectory(
                prefix='snow-ocr-tls-'
            ) as directory:
                fixture = Path(directory)
                (fixture / 'src').mkdir()
                (fixture / 'src/lib.rs').write_text('', encoding='utf-8')
                lines = [
                    '[package]',
                    'name = "snow-ocr-tls-test"',
                    'version = "0.1.0"',
                    'edition = "2024"',
                    '[workspace]',
                    '[dependencies]',
                    'rapid-ocr-rs = { '
                    f'path = {json.dumps(OCR_MANIFEST.parent.as_posix())}, '
                    f'default-features = false, features = [{json.dumps(feature)}] }}',
                    '[patch.crates-io]',
                ]
                for package in ('ort', 'ort-sys'):
                    patch = workspace['patch']['crates-io'][package]
                    lines.append(
                        f'{package} = {{ git = {json.dumps(patch["git"])}, '
                        f'rev = {json.dumps(patch["rev"])} }}'
                    )
                manifest = fixture / 'Cargo.toml'
                manifest.write_text('\n'.join(lines) + '\n', encoding='utf-8')
                command = ['cargo', '--offline', '--manifest-path', str(manifest)]
                generated = subprocess.run(
                    [command[0], 'generate-lockfile', *command[1:]],
                    capture_output=True, text=True, timeout=60,
                )
                self.assertEqual(generated.returncode, 0, generated.stderr)
                for vulnerable_version in ('0.23.43', '0.23.44'):
                    with self.subTest(version=vulnerable_version):
                        downgrade = subprocess.run(
                            [command[0], 'update', *command[1:], '-p', 'rustls',
                             '--precise', vulnerable_version],
                            capture_output=True, text=True, timeout=60,
                        )
                        self.assertNotEqual(
                            downgrade.returncode, 0,
                            f'{feature} permits vulnerable Rustls {vulnerable_version}',
                        )
                        self.assertIn('failed to select a version', downgrade.stderr)
                        self.assertIn('rustls', downgrade.stderr)


if __name__ == '__main__':
    unittest.main()
