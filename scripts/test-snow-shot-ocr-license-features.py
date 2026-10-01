#!/usr/bin/env python3
"""Verify OCR notice selection follows the shipped worker's dependency closure."""
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKER_MANIFEST = ROOT / 'snow-crates/crates/snow-ocr-process/Cargo.toml'
LEGACY_DEPENDENCIES = {'clap', 'kamadak-exif', 'reqwest', 'rustls', 'serde_yaml', 'sha2'}


def packages(target, features):
    result = subprocess.run(
        ['cargo', 'tree', '--locked', '--offline', '--manifest-path', str(WORKER_MANIFEST),
         '--package', 'snow-ocr-process', '--target', target, '--no-default-features',
         '--features', features, '--edges', 'normal,build', '--prefix', 'none',
         '--format', '{p}'],
        check=True, capture_output=True, text=True, timeout=60,
    )
    return {line.split()[0] for line in result.stdout.splitlines() if line.strip()}


@unittest.skipUnless(shutil.which('cargo'), 'Cargo is required to inspect release dependencies')
class OcrLicenseFeatures(unittest.TestCase):
    def test_pinned_windows_worker_retains_its_original_dependency_notices(self):
        packaging = (ROOT / 'scripts/package-snow-shot.ps1').read_text()
        selection = re.search(
            r"\$ocrCargoManifest\s*=\s*@\('--no-default-features',\s*'--features',\s*'([^']+)'\)",
            packaging,
        )
        self.assertIsNotNone(selection, 'Windows packaging must select the OCR notice features')
        selected = packages('x86_64-pc-windows-msvc', selection.group(1))
        self.assertTrue(LEGACY_DEPENDENCIES <= selected,
                        f'Published worker notices are missing: {LEGACY_DEPENDENCIES - selected}')
        self.assertTrue({'png', 'zune-jpeg'} <= selected)
        self.assertTrue({'turbojpeg', 'ravif', 'image-webp'}.isdisjoint(selected),
                        'The pinned worker does not include the full-image/ORT default features')

    def test_local_macos_worker_omits_unused_convenience_notices(self):
        selected = packages('aarch64-apple-darwin', 'static-onnx-runtime,crash-diagnostics')
        self.assertTrue(LEGACY_DEPENDENCIES.isdisjoint(selected),
                        f'Unused local worker notices were retained: {LEGACY_DEPENDENCIES & selected}')
        self.assertTrue({'png', 'zune-jpeg', 'turbojpeg', 'ravif'}.isdisjoint(selected))
        self.assertIn('ort', selected)
        self.assertIn('ort-sys', selected)


if __name__ == '__main__':
    unittest.main()
