#!/usr/bin/env python3
"""Focused regression checks for repeatable native OCR bundle staging."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("stage_ocr", Path(__file__).with_name("stage-macos-ocr.py"))
stage_ocr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage_ocr)


class NativeRuntimeStagingTests(unittest.TestCase):
    def test_read_only_runtime_can_be_restaged_without_modifying_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "Cellar/onnxruntime/1.0/lib/libonnxruntime.1.0.dylib"
            source.parent.mkdir(parents=True)
            source.write_bytes(b"native ONNX runtime")
            source.chmod(0o444)
            app = root / "Snow Shot.app"
            helper = app / "Contents/MacOS/snow-ocr-process"
            helper.parent.mkdir(parents=True)
            helper.write_bytes(b"native helper")
            helper.chmod(0o755)
            library = app / "Contents/Frameworks" / source.name
            with patch.object(stage_ocr.subprocess, "check_output", return_value="arm64\n"):
                stage_ocr.stage(app, source)
                initial_stat = library.stat()
                stage_ocr.stage(app, source)
                self.assertEqual(library.stat().st_mtime_ns, initial_stat.st_mtime_ns)
                self.assertEqual(library.read_bytes(), source.read_bytes())
                library.unlink()
                library.write_bytes(b"outdated read-only runtime")
                library.chmod(0o444)
                stage_ocr.stage(app, source)
                self.assertEqual(library.read_bytes(), b"native ONNX runtime")
                library.unlink()
                library.symlink_to(source)
                stage_ocr.stage(app, source)
                self.assertFalse(library.is_symlink())
                self.assertEqual(library.read_bytes(), b"native ONNX runtime")
            self.assertEqual(source.read_bytes(), b"native ONNX runtime")
            self.assertEqual(source.stat().st_mode & 0o777, 0o444)

    def test_upgrade_retires_only_the_verified_previously_managed_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "libonnxruntime.1.29.dylib"
            second = root / "libonnxruntime.1.30.dylib"
            first.write_bytes(b"runtime version 1.29")
            second.write_bytes(b"runtime version 1.30")
            first.chmod(0o444)
            second.chmod(0o444)
            app = root / "Snow Shot.app"
            helper = app / "Contents/MacOS/snow-ocr-process"
            helper.parent.mkdir(parents=True)
            helper.write_bytes(b"native helper")
            helper.chmod(0o755)
            frameworks = app / "Contents/Frameworks"
            with patch.object(stage_ocr.subprocess, "check_output", return_value="arm64\n"):
                stage_ocr.stage(app, first)
                unrelated = frameworks / "libcustom.dylib"
                unrelated.write_bytes(b"unmanaged custom library")
                stage_ocr.stage(app, second)
                self.assertFalse((frameworks / first.name).exists())
                self.assertEqual((frameworks / second.name).read_bytes(), second.read_bytes())
                self.assertEqual(unrelated.read_bytes(), b"unmanaged custom library")
                stage_ocr.stage(app)  # The packaging pass must now resolve one native runtime.
                untracked = frameworks / "libonnxruntime.unmanaged.dylib"
                untracked.write_bytes(b"unmanaged ONNX build")
                stage_ocr.stage(app, first)
                self.assertEqual(untracked.read_bytes(), b"unmanaged ONNX build")
                modified = frameworks / first.name
                modified.unlink()
                modified.write_bytes(b"locally modified runtime")
                stage_ocr.stage(app, second)
                self.assertEqual(modified.read_bytes(), b"locally modified runtime")
            self.assertEqual(first.read_bytes(), b"runtime version 1.29")
            self.assertEqual(second.read_bytes(), b"runtime version 1.30")


if __name__ == "__main__":
    unittest.main()
