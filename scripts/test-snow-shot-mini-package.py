"""Configure-only regression for Mini's binary CPack configuration."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MiniPackageConfiguration(unittest.TestCase):
    def test_source_package_state_does_not_leak_into_mini_binary_package(self):
        generator = 'DragNDrop' if sys.platform == 'darwin' else 'NSIS'
        with tempfile.TemporaryDirectory(prefix='snow-mini-package-') as directory:
            fixture = Path(directory)
            build = fixture / 'build'
            (fixture / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 4.2)
project(MiniPackage LANGUAGES NONE)
set(CMAKE_SOURCE_DIR "{ROOT.as_posix()}")
set(SNOW_SHOT_VERSION 1.1.9)
set(SNOW_SHOT_VERSION_NUMERIC 1.1.9)
set(SNOW_SHOT_VENDOR "Snow Apps")
set(SNOW_SHOT_COPYRIGHT "Copyright Snow Apps")
set(_snow_dmg_assets "{ROOT.as_posix()}/snow_shot/packaging/macos")
add_library(snow_shot_mini INTERFACE)
install(FILES "{ROOT.as_posix()}/snow_shot/packaging/README.txt"
    DESTINATION . COMPONENT SnowShot)
install(FILES "{ROOT.as_posix()}/snow_shot/packaging/README-mini.txt"
    DESTINATION . COMPONENT SnowShotMini)
install(FILES "{ROOT.as_posix()}/snow_shot/tests/installer_updater_stub.cpp"
    DESTINATION bin RENAME snow-shot-mini-updater.exe COMPONENT SnowShotMini)
set(CPACK_PACKAGE_NAME snow-shot)
set(CPACK_PACKAGE_VERSION "${{SNOW_SHOT_VERSION}}")
set(CPACK_GENERATOR {generator})
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_INSTALL_CMAKE_PROJECTS "${{CMAKE_BINARY_DIR}};MiniPackage;SnowShot;/")
if(NOT APPLE)
    include("{ROOT.as_posix()}/cmake/SnowShotPackageCompression.cmake")
    snow_shot_apply_nsis_compression()
endif()
include(CPack)
file(SHA256 "${{CMAKE_BINARY_DIR}}/CPackConfig.cmake" full_before)
include("{ROOT.as_posix()}/cmake/SnowShotMiniPackage.cmake")
file(SHA256 "${{CMAKE_BINARY_DIR}}/CPackConfig.cmake" full_after)
if(NOT full_before STREQUAL full_after)
    message(FATAL_ERROR "Mini changed the full binary package configuration")
endif()
''', encoding='utf-8')
            result = subprocess.run(['cmake', '-S', str(fixture), '-B', str(build)],
                                    capture_output=True, text=True, encoding='utf-8',
                                    errors='replace')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            config = (build / 'CPackSnowShotMiniConfig.cmake').read_text(encoding='utf-8')
            values = dict(re.findall(r'^set\((CPACK_\w+) "(.*)"\)$', config, re.MULTILINE))
            self.assertEqual(values['CPACK_GENERATOR'], generator)
            self.assertEqual(values.get('CPACK_INSTALLED_DIRECTORIES', ''), '')
            self.assertFalse(values.get('CPACK_TOPLEVEL_TAG', '').endswith('-Source'))
            self.assertNotEqual(values.get('CPACK_RPM_PACKAGE_SOURCES', ''), 'ON')
            self.assertTrue(values['CPACK_INSTALL_CMAKE_PROJECTS'].endswith(';MiniPackage;SnowShotMini;/'))
            self.assertEqual(values['CPACK_PACKAGE_NAME'], 'snow-shot-mini')
            if sys.platform != 'darwin':
                self.assertEqual(values['CPACK_NSIS_COMPRESSOR'], '/SOLID lzma')
                self.assertIn('SetCompressorDictSize 32', config)
                # Inspect the actual NSIS directive, not an unused CPack variable.
                nsis = Path('C:/Program Files (x86)/NSIS/makensis.exe')
                if not nsis.is_file():
                    self.skipTest('NSIS is required to verify the generated installer script')
                for filename in ('CPackConfig.cmake', 'CPackSnowShotMiniConfig.cmake'):
                    result = subprocess.run(
                        ['cpack', '--config', str(build / filename), '-G', 'NSIS',
                         '-D', f'CPACK_NSIS_EXECUTABLE={nsis.as_posix()}'],
                        cwd=build, capture_output=True, text=True, encoding='utf-8',
                        errors='replace')
                    logs = list(build.glob('_CPack_Packages/*/NSIS/NSISOutput.log'))
                    diagnostics = result.stdout + result.stderr + ''.join(
                        path.read_text(encoding='utf-8', errors='replace') for path in logs)
                    self.assertEqual(result.returncode, 0, diagnostics)
                    scripts = list(build.glob('_CPack_Packages/*/NSIS/project.nsi'))
                    self.assertEqual(len(scripts), 1)
                    script = scripts[0].read_text(encoding='utf-8')
                    self.assertRegex(script, r'(?m)^\s*SetCompressor /SOLID lzma\s*$')
                    self.assertRegex(script, r'(?m)^\s*SetCompressorDictSize 32\s*$')
                result = subprocess.run(
                    ['cmake', '-S', str(fixture), '-B', str(build),
                     '-DSNOW_WINDOWS_ARCHITECTURE=arm64', '-DSNOW_WINDOWS_PLATFORM=windows-arm64'],
                    capture_output=True, text=True, encoding='utf-8', errors='replace')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                arm_config = (build / 'CPackSnowShotMiniConfig.cmake').read_text(encoding='utf-8')
                self.assertIn('snow-shot-mini-1.1.9-windows-arm64', arm_config)
                result = subprocess.run(
                    ['cpack', '--config', str(build / 'CPackSnowShotMiniConfig.cmake'), '-G', 'NSIS',
                     '-D', f'CPACK_NSIS_EXECUTABLE={nsis.as_posix()}'], cwd=build,
                    capture_output=True, text=True, encoding='utf-8', errors='replace')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                scripts = list(build.glob('_CPack_Packages/*/NSIS/project.nsi'))
                self.assertEqual(len(scripts), 1)
                script = scripts[0].read_text(encoding='utf-8')
                self.assertIn('!define SNOW_SHOT_INSTALLER_ARCHITECTURE "arm64"', script)
                init = re.search(r'Function \.onInit\n(.*?)FunctionEnd', script, re.DOTALL).group(1)
                self.assertLess(init.index('!insertmacro SnowShotCheckArchitecture'),
                                init.index('Call SnowShotEnsureMainAppClosed'))
                self.assertLess(init.index('SetRegView 64'), init.index('ReadRegStr'))


if __name__ == '__main__':
    unittest.main()
