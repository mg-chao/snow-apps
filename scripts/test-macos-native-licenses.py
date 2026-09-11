#!/usr/bin/env python3
"""Focused checks for collecting the libraries selected by macOS packaging."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("package_macos", Path(__file__).with_name("package-macos.py"))
package_macos = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_macos)


class NativeLicenseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.cellar = self.root / "Cellar"
        self.prefix = self.root / "local/ffmpeg"
        self.notices = self.prefix / "share/snow-apps/ffmpeg"
        self.notices.mkdir(parents=True)
        self.library = self.prefix / "lib/libavcodec.62.dylib"
        self.library.parent.mkdir(parents=True)
        self.library.write_bytes(b"fixture FFmpeg library")
        self.archive = self.notices / "ffmpeg-8.1.2.tar.xz"
        self.archive.write_bytes(b"fixture corresponding source archive")
        self.metadata = {
            "version": "8.1.2", "source_archive": self.archive.name,
            "source_url": "https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz",
            "source_sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest(),
            "configure_args": ["--enable-shared", "--enable-gpl", "--enable-version3",
                               "--enable-libwebp"],
            "license": "GPL-3.0-or-later", "patches": [],
        }
        (self.notices / "COPYING.GPLv3").write_text("fixture GPLv3 license")
        self.write_metadata()
        zlib = self.root / ".tools/zlib-ng/LICENSE.md"
        zlib.parent.mkdir(parents=True)
        zlib.write_text("fixture zlib license")

    def write_metadata(self):
        (self.notices / "source-build.json").write_text(json.dumps(self.metadata))

    def collect(self, libraries=None):
        with patch.object(package_macos, "ROOT", self.root), patch.object(
                package_macos, "run", return_value=str(self.cellar)):
            package_macos.collect_native_notices(
                libraries or {self.library}, self.root / "native-licenses")

    def test_local_ffmpeg_and_homebrew_dependency_keep_corresponding_notices(self):
        webp = self.cellar / "webp/1.6.0"
        webp.mkdir(parents=True)
        (webp / "COPYING").write_text("fixture WebP license")
        (webp / "INSTALL_RECEIPT.json").write_text('{"version":"1.6.0"}')
        self.collect({self.library, webp / "lib/libwebp.7.dylib"})
        collected = self.root / "native-licenses/ffmpeg-8.1.2"
        for source in self.notices.iterdir():
            self.assertEqual((collected / source.name).read_bytes(), source.read_bytes())
        self.assertEqual((self.root / "native-licenses/webp-1.6.0/COPYING").read_text(),
                         "fixture WebP license")

    def test_local_ffmpeg_missing_provenance_is_not_silently_omitted(self):
        (self.notices / "source-build.json").unlink()
        with self.assertRaisesRegex(RuntimeError, "no source and license metadata"):
            self.collect()

    def test_mismatched_source_archive_and_license_fail_collection(self):
        self.archive.write_bytes(b"a different FFmpeg source release")
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            self.collect()
        self.metadata["source_sha256"] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.metadata["license"] = "LGPL-2.1-or-later"
        self.write_metadata()
        with self.assertRaisesRegex(RuntimeError, "license does not match"):
            self.collect()


if __name__ == "__main__":
    unittest.main()
