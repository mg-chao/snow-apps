#!/usr/bin/env python3
"""Deterministic dependency-refresh regression checks; no native tools needed."""
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "packager", pathlib.Path(__file__).with_name("package-macos-media-harness.py")
)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackageTests(unittest.TestCase):
    def test_repackaging_refreshes_the_complete_dependency_closure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            app = root / "Harness.app"
            profile = root / "profile"
            executable = app / "Contents/MacOS/SnowRecordingHarness"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"executable")
            libraries = profile / "lib"
            libraries.mkdir(parents=True)
            for name in ("libavcodec.dylib", "libx265.dylib"):
                (libraries / name).write_bytes(b"old 8-bit profile")

            def dependencies(command, **_):
                binary = pathlib.Path(command[-1])
                names = {
                    "SnowRecordingHarness": ["libavcodec.dylib"],
                    "libavcodec.dylib": ["libavcodec.dylib", "libx265.dylib"],
                    "libx265.dylib": ["libx265.dylib"],
                }[binary.name]
                return str(binary) + ":\n" + "".join(
                    "\t@rpath/" + name + " (compatibility version 1.0.0)\n"
                    for name in names
                ) + "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"

            with mock.patch.object(packager.subprocess, "check_output", dependencies), \
                    mock.patch.object(packager.subprocess, "run"):
                packager.package(app, profile)
                (libraries / "libavcodec.dylib").write_bytes(b"new ffmpeg")
                (libraries / "libx265.dylib").write_bytes(b"new Main10 profile")
                with mock.patch.object(packager.shutil, "copyfile", wraps=packager.shutil.copyfile) as copy:
                    packager.package(app, profile)
                    self.assertEqual(copy.call_count, 2)
            bundled = app / "Contents/Frameworks"
            self.assertEqual((bundled / "libx265.dylib").read_bytes(), b"new Main10 profile")
            self.assertEqual((bundled / "libavcodec.dylib").read_bytes(), b"new ffmpeg")

    def test_unprovisioned_dependency_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            with mock.patch.object(packager.subprocess, "check_output", return_value=
                    "binary:\n\t@rpath/missing.dylib (compatibility version 1.0.0)\n"):
                with self.assertRaisesRegex(SystemExit, "Unprovisioned dependency"):
                    packager.package(root / "Harness.app", root / "profile")


if __name__ == "__main__":
    unittest.main()
