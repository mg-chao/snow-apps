#!/usr/bin/env python3
"""Offscreen build-script contract tests; no compiler, Qt, or network required."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MacOSBuildScripts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="snow build tests ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copytree(ROOT / "scripts", self.root / "scripts")
        self.bin = self.root / ".tools/macos-dev/bin"
        self.bin.mkdir(parents=True)
        self.log = self.root / "commands.jsonl"
        # A mock command logs its argv as JSON, preserving spaces and boundaries.
        mock = """#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['SNOW_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name] + sys.argv[1:]) + '\\n')
if name == 'uname': print('Darwin' if sys.argv[1] == '-s' else 'arm64')
if name == 'xcode-select': print('/mock Xcode')
if name == 'cmake' and '--preset' in sys.argv and os.environ.get('FAIL_CONFIGURE'): sys.exit(17)
"""
        for name in ("cmake", "cpack", "ninja", "cargo", "rustup", "pkg-config", "uname", "open", "xcode-select", "xcrun", "git"):
            path = self.bin / name
            path.write_text(mock)
            path.chmod(0o755)
        vcpkg = self.root / ".tools/vcpkg"
        vcpkg.mkdir()
        (vcpkg / "bootstrap-vcpkg.sh").touch()
        shutil.copyfile(self.bin / "git", vcpkg / "vcpkg")
        (vcpkg / "vcpkg").chmod(0o755)
        qt = self.root / "Qt kit/lib/cmake/Qt6"
        qt.mkdir(parents=True)
        (qt / "Qt6Config.cmake").touch()
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        Qt6_DIR=str(qt), SNOW_TEST_LOG=str(self.log))

    def run_script(self, script, *args, success=True):
        result = subprocess.run(["/bin/bash", str(self.root / "scripts" / script), *args],
                                env=self.env, text=True, capture_output=True, cwd="/")
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return [] if not self.log.exists() else [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_build_preserves_arguments_and_clean_target(self):
        calls = self.run_script("build.sh", "snow-shot-macos-x64-debug", "--target", "some-test",
                                "--clean", "--", "-DEXAMPLE=a path with spaces")
        configure, build = [c for c in calls if c[0] == "cmake"]
        self.assertIn("-DEXAMPLE=a path with spaces", configure)
        self.assertEqual(build, ["cmake", "--build", "--preset", "build-snow-shot-macos-x64-debug",
                                 "--target", "some-test", "--clean-first", "--parallel"])

    def test_default_build_and_empty_array_on_system_bash(self):
        calls = self.run_script("build.sh")
        self.assertIn(["cmake", "--build", "--preset", "build-snow-shot-macos-arm64-debug",
                       "--target", "snow_shot", "--parallel"], calls)

    def test_configure_failure_stops_build(self):
        self.env['FAIL_CONFIGURE'] = '1'
        calls = self.run_script("build.sh", success=False)
        self.assertFalse(any('--build' in c for c in calls))

    def test_invalid_arguments_do_not_configure(self):
        for args in (("windows-msvc-debug",), ("snow-shot-macos-arm64-other-debug",),
                     ("--target",), ("--unknown",)):
            calls = self.run_script("build.sh", *args, success=False)
            self.assertFalse(any(c[0] == 'cmake' for c in calls))

    def test_bootstrap_selects_matching_rust_target(self):
        calls = self.run_script("bootstrap-macos.sh", "snow-shot-macos-x64-release")
        self.assertIn(["rustup", "target", "add", "x86_64-apple-darwin"], calls)

    def test_package_builds_before_cpack(self):
        calls = self.run_script("package-snow-shot.sh")
        build = next(i for i, c in enumerate(calls) if '--build' in c)
        pack = next(i for i, c in enumerate(calls) if c[0] == 'cpack')
        self.assertLess(build, pack)
        self.assertEqual(calls[pack], ['cpack', '--preset', 'package-snow-shot-macos-arm64-release'])

    def test_package_rejects_nonrelease(self):
        calls = self.run_script("package-snow-shot.sh", "snow-shot-macos-arm64-debug", success=False)
        self.assertFalse(any(c[0] in ('cmake', 'cpack') for c in calls))

    def test_launch_bundle_with_arguments(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        calls = self.run_script('run-snow-shot.sh', '--', '--example', 'a path')
        self.assertEqual(calls[-1], ['open', '-n', str(app.parent.parent / 'run/snow_shot.app'),
                                    '--args', '--example', 'a path'])
        self.assertIn('--install', calls[-2])


@unittest.skipUnless(os.environ.get("SNOW_TEST_MACOS_BUNDLE") == "1",
                     "Set SNOW_TEST_MACOS_BUNDLE=1 for the native deployment fixture")
class MacOSBundle(unittest.TestCase):
    def test_deploy_helpers_and_package(self):
        arch = "arm64" if os.uname().machine == "arm64" else "x64"
        prefix = ROOT / f".tools/macos/installed/{arch}-osx-snow-shot"
        qt = os.environ.get("Qt6_DIR", str(Path.home() / "Qt/6.11.1/macos/lib/cmake/Qt6"))
        with tempfile.TemporaryDirectory(prefix="snow bundle test ") as temp:
            out = Path(temp) / "build"
            stage = Path(temp) / "stage"
            def run(*args, **kwargs):
                result = subprocess.run(args, text=True, capture_output=True, **kwargs)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout
            run("cmake", "-S", str(ROOT / "test-support/macos-build"), "-B", str(out),
                "-G", "Ninja", f"-DSNOW_ROOT={ROOT}", f"-DSNOW_FFMPEG_ROOT={prefix}",
                f"-DQt6_DIR={qt}", "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_OSX_ARCHITECTURES={os.uname().machine}")
            run("cmake", "--build", str(out))
            run("cmake", "--install", str(out), "--component", "SnowShot", "--prefix", str(stage))
            app = stage / "snow_shot.app"
            for name in ("snow_shot", "snow-ocr-process", "snow-shot-updater"):
                binary = app / "Contents/MacOS" / name
                run(str(binary), cwd="/")
                rpaths = run("otool", "-l", str(binary))
                self.assertNotIn(str(ROOT), rpaths)
                self.assertNotIn(str(out), rpaths)
            run("codesign", "--verify", "--deep", "--strict", str(app))
            run("cpack", "--config", str(out / "CPackConfig.cmake"), cwd=str(out))
            self.assertEqual(len(list(out.glob("*.dmg"))), 1)
            self.assertEqual(len(list(out.glob("*.dmg.sha256"))), 1)


if __name__ == '__main__':
    unittest.main()
