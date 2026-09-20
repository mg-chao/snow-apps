#!/usr/bin/env python3
"""Offscreen build-script contract tests; no compiler, Qt, or network required."""
import json
import os
import plistlib
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


class MacOSBundleMetadata(unittest.TestCase):
    def test_native_languages_match_the_application_catalogs(self):
        plist = plistlib.loads((ROOT / 'snow_shot/packaging/macos/Info.plist.in').read_bytes())
        native_languages = {'en_US': 'en', 'zh_CN': 'zh-Hans', 'zh_TW': 'zh-Hant'}
        catalog_languages = {ET.parse(path).getroot().attrib['language']
                             for path in (ROOT / 'snow_shot/i18n').rglob('*.ts')}
        self.assertEqual(set(plist['CFBundleLocalizations']),
                         {native_languages[language] for language in catalog_languages})
        self.assertEqual(plist['CFBundleDevelopmentRegion'], native_languages['en_US'])


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
if name == 'ps' and os.environ.get('SNOW_TEST_PS_OUTPUT'): print(os.environ['SNOW_TEST_PS_OUTPUT'])
"""
        tools = ("cmake", "cpack", "ninja", "cargo", "rustup", "pkg-config", "uname",
                 "open", "ps", "xcode-select", "xcrun", "git", "lsregister")
        for name in tools:
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
                        Qt6_DIR=str(qt), SNOW_TEST_LOG=str(self.log),
                        SNOW_LAUNCH_SERVICES_REGISTER=str(self.bin / 'lsregister'))

    def run_script(self, script, *args, success=True):
        result = subprocess.run(["/bin/bash", str(self.root / "scripts" / script), *args],
                                env=self.env, text=True, capture_output=True, cwd="/")
        self.last_result = result
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
        deployed = app.parent.parent / 'run/snow_shot.app'
        deployed.mkdir(parents=True)
        calls = self.run_script('run-snow-shot.sh', '--', '--example', 'a path')
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot', '--parallel'], calls)
        self.assertEqual(calls[-1], ['open', '-n', str(deployed), '--args', '--example', 'a path'])
        self.assertIn('--install', calls[-3])
        self.assertEqual(calls[-2], ['lsregister', '-f', str(deployed)])

    def test_launch_stops_selected_build_instances_before_rebuilding(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        running = app.parent.parent / 'run/snow_shot.app/Contents/MacOS/snow_shot'
        running.parent.mkdir(parents=True)
        unrelated = self.root / 'other/snow_shot'
        process = subprocess.Popen(['/bin/sleep', '60'])
        reaper = threading.Thread(target=process.wait)
        reaper.start()
        self.env['SNOW_TEST_PS_OUTPUT'] = f'{process.pid} {running}\n2147483646 {unrelated}'

        try:
            calls = self.run_script('run-snow-shot.sh')
        finally:
            if process.poll() is None:
                process.kill()
            reaper.join(timeout=2)

        inspect = calls.index(['ps', '-axww', '-o', 'pid=', '-o', 'comm='])
        build = next(i for i, call in enumerate(calls) if '--build' in call)
        self.assertLess(inspect, build)
        self.assertIn(f'Stopping the running development instance (PID {process.pid})...',
                      self.last_result.stdout)
        self.assertNotIn('2147483646', self.last_result.stdout)
        self.assertFalse(reaper.is_alive())
        self.assertEqual(process.returncode, -signal.SIGTERM)

    def test_launch_can_skip_or_clean_the_automatic_build(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)

        deployed = app.parent.parent / 'run/snow_shot.app/Contents/MacOS/snow_shot'
        deployed.parent.mkdir(parents=True)
        deployed.touch()
        deployed.chmod(0o755)

        calls = self.run_script('run-snow-shot.sh', '--no-build')
        self.assertFalse(any(c[0] == 'cmake' for c in calls))
        self.assertEqual(calls[-1], ['open', '-n', str(deployed.parents[2]), '--args'])

        self.log.unlink()
        calls = self.run_script('run-snow-shot.sh', '--clean')
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot', '--clean-first', '--parallel'], calls)

    def test_launch_can_cache_a_persistent_codesign_identity(self):
        app = self.root / 'build/snow-shot-macos-arm64-debug/snow_shot/snow_shot.app'
        binary = app / 'Contents/MacOS/snow_shot'
        binary.parent.mkdir(parents=True)
        binary.touch()
        binary.chmod(0o755)
        deployed = app.parent.parent / 'run/snow_shot.app'
        deployed.mkdir(parents=True)

        identity = 'Snow Shot Development (Local)'
        calls = self.run_script('run-snow-shot.sh', '--codesign-identity', identity)
        configure = next(c for c in calls if c[0] == 'cmake' and '--preset' in c)
        self.assertIn('-DSNOW_MACOS_CODESIGN_IDENTITY=' + identity, configure)
        self.assertIn(['cmake', '--build', '--preset', 'build-snow-shot-macos-arm64-debug',
                       '--target', 'snow_shot', '--parallel'], calls)

    def test_no_build_requires_deployment_and_rejects_clean(self):
        for args in (('--no-build',), ('--no-build', '--clean'),
                     ('--no-build', '--codesign-identity', 'Development')):
            calls = self.run_script('run-snow-shot.sh', *args, success=False)
            self.assertFalse(any(c[0] in ('cmake', 'open') for c in calls))

    def test_codesign_identity_requires_a_value(self):
        calls = self.run_script('run-snow-shot.sh', '--codesign-identity', success=False)
        self.assertFalse(any(c[0] in ('cmake', 'open') for c in calls))

    def test_deployment_uses_the_matching_vcpkg_library_configuration(self):
        deployment = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
        self.assertIn('if(_snow_install_config STREQUAL "debug")', deployment)
        self.assertIn('set(_snow_vcpkg_library_dir "@SNOW_FFMPEG_ROOT@/debug/lib")', deployment)
        self.assertIn('set(_snow_vcpkg_library_dir "@SNOW_FFMPEG_ROOT@/lib")', deployment)
        self.assertEqual(deployment.count('"-libpath=${_snow_vcpkg_library_dir}"'), 1)

    def test_macos_icon_uses_native_visual_bounds_and_bundle_metadata(self):
        generator = (ROOT / 'cmake/GenerateMacOSIcon.cmake').read_text()
        bounds = 'x=\\"100\\" y=\\"100\\" width=\\"824\\" height=\\"824\\"'
        self.assertIn(bounds, generator)

        macos = (ROOT / 'cmake/SnowShotMacOS.cmake').read_text()
        self.assertIn('snow_shot/resources/app-icon.svg', macos)
        self.assertNotIn('packaging/macos/app-icon.svg', macos)

        plist = (ROOT / 'snow_shot/packaging/macos/Info.plist.in').read_text()
        self.assertIn('<key>CFBundleIconFile</key>', plist)
        self.assertIn('${MACOSX_BUNDLE_ICON_FILE}', plist)

        main = (ROOT / 'snow_shot/src/app/main.cpp').read_text()
        self.assertNotIn('installApplicationIconFromBundle', main)
        self.assertNotIn('setApplicationIconImage', main)
        self.assertIn('#ifndef Q_OS_MACOS\n    QApplication::setWindowIcon(', main)


class MacOSSigningDeployment(unittest.TestCase):
    """Exercise the deployment script with fake external tools, including OCR resealing."""

    def test_identity_survives_deployment_and_ocr_reseal(self):
        for identity in ('-', 'Snow Shot Development (Local)'):
            with self.subTest(identity=identity):
                calls, result = self.deploy(identity)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                deploy = next(c for c in calls if c[0] == 'macdeployqt')
                self.assertIn('-codesign=' + identity, deploy)
                sign = next(c for c in calls if c[0] == 'codesign' and '--sign' in c)
                self.assertEqual(sign[sign.index('--sign') + 1], identity)
                finalize = next(i for i, c in enumerate(calls) if 'finalize' in c)
                self.assertLess(finalize, calls.index(sign))
                self.assertEqual(calls[-1][0:4], ['codesign', '--verify', '--deep', '--strict'])

    def test_signing_failure_stops_without_ad_hoc_fallback(self):
        calls, result = self.deploy('Unavailable Certificate', fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sum(c[0] == 'macdeployqt' for c in calls), 1)
        self.assertFalse(any(c[0] == 'codesign' for c in calls))

    def deploy(self, identity, fail=False):
        with tempfile.TemporaryDirectory(prefix='snow signing tests ') as temp:
            root = Path(temp)
            log = root / 'calls.jsonl'
            mock = """#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
with open(os.environ['SNOW_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name] + sys.argv[1:]) + '\\n')
if name == 'macdeployqt' and os.environ.get('SNOW_TEST_FAIL_SIGN'): sys.exit(1)
"""
            for name in ('macdeployqt', 'codesign', 'install_name_tool', 'ocr'):
                tool = root / name
                tool.write_text(mock)
                tool.chmod(0o755)
            script = (ROOT / 'cmake/DeploySnowShotMacOS.cmake.in').read_text()
            values = {'SNOW_MACOS_CODESIGN_IDENTITY': identity,
                      'SNOW_MACDEPLOYQT': str(root / 'macdeployqt'),
                      'SNOW_MACOS_OCR_ASSETS_ENABLED': 'ON',
                      'Python3_EXECUTABLE': str(root / 'ocr'),
                      'SNOW_MACOS_OCR_TOOL': 'ocr.py', 'SNOW_MACOS_OCR_MANIFEST': 'manifest.json',
                      'SNOW_FFMPEG_ROOT': str(root / 'ffmpeg'), 'CMAKE_BINARY_DIR': str(root)}
            for key, value in values.items():
                script = script.replace('@' + key + '@', value)
            for name in ('codesign', 'install_name_tool'):
                script = script.replace('/usr/bin/' + name, '"' + str(root / name) + '"')
            path = root / 'deploy.cmake'
            path.write_text(script)
            cmake = shutil.which('cmake') or str(ROOT / '.tools/macos-dev/bin/cmake')
            env = dict(os.environ, SNOW_TEST_LOG=str(log))
            if fail:
                env['SNOW_TEST_FAIL_SIGN'] = '1'
            result = subprocess.run([cmake, '-DCMAKE_INSTALL_PREFIX=' + str(root),
                                     '-DCMAKE_INSTALL_CONFIG_NAME=Debug', '-P', str(path)],
                                    env=env, text=True, capture_output=True)
            return [json.loads(line) for line in log.read_text().splitlines()], result


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
            info = run("plutil", "-extract", "CFBundleIconFile", "raw", "-o", "-",
                       str(app / "Contents/Info.plist")).strip()
            self.assertEqual(info, "snow-shot.icns")
            self.assertTrue((app / "Contents/Resources/snow-shot.icns").is_file())
            self.assertTrue((app / "Contents/PlugIns/platforms/libqoffscreen.dylib").is_file())
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
            dmg = next(out.glob("*.dmg"))
            run("codesign", "--verify", "--strict", str(dmg))
            import hashlib
            checksum = dmg.with_suffix(".dmg.sha256").read_text().split()[0]
            self.assertEqual(checksum, hashlib.sha256(dmg.read_bytes()).hexdigest())


if __name__ == '__main__':
    unittest.main()
