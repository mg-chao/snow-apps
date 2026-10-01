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
set(CPACK_PACKAGE_NAME snow-shot)
set(CPACK_PACKAGE_VERSION "${{SNOW_SHOT_VERSION}}")
set(CPACK_GENERATOR {generator})
set(CPACK_INSTALL_CMAKE_PROJECTS "${{CMAKE_BINARY_DIR}};MiniPackage;SnowShot;/")
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


if __name__ == '__main__':
    unittest.main()
