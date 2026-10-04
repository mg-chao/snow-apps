#!/usr/bin/env python3
"""Focused Qt/CMake contract tests; no compiler, Qt installation, or network required."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = json.loads((ROOT / 'scripts/qt-toolchain.json').read_text(encoding='utf-8'))
QT_VERSION = CONTRACT['qtVersion']
DEPRECATION_VERSION = CONTRACT['deprecationVersion']


class QtCMakeContract(unittest.TestCase):
    def setUp(self):
        build_root = ROOT / 'build'
        build_root.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='qt-cmake-contract-', dir=build_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.kit = self.root / 'kit'
        self.source = self.root / 'source'
        self.source.mkdir()
        self.cmake = shutil.which('cmake')
        self.assertIsNotNone(self.cmake, 'CMake must be available')

    def write_package(self, name, version, contents):
        directory = self.kit / f'lib/cmake/{name}'
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f'{name}Config.cmake').write_text(contents, encoding='utf-8')
        (directory / f'{name}ConfigVersion.cmake').write_text(f'''
set(PACKAGE_VERSION "{version}")
if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
''', encoding='utf-8')

    def write_qt(self, version=QT_VERSION, linkage='STATIC', gui_features='', core_features=''):
        self.write_package('Qt6', version, f'''
if(NOT Qt6_FIND_VERSION_EXACT)
    message(FATAL_ERROR "The public Qt request must be exact")
endif()
set(Qt6_VERSION "{version}")
set(Qt6Gui_VERSION "{version}")
if(NOT TARGET Qt6::Core)
    add_library(Qt6::Core {linkage} IMPORTED GLOBAL)
    add_library(Qt6::Gui {linkage} IMPORTED GLOBAL)
    set_target_properties(Qt6::Core PROPERTIES QT_ENABLED_PUBLIC_FEATURES "{core_features}")
    set_target_properties(Qt6::Gui PROPERTIES QT_ENABLED_PRIVATE_FEATURES "{gui_features}")
endif()
''')
        self.write_package('Qt6GuiPrivate', version, '''
if(NOT Qt6GuiPrivate_FIND_VERSION_EXACT)
    message(FATAL_ERROR "The private Qt request must be exact")
endif()
if(NOT TARGET Qt6::GuiPrivate)
    add_library(Qt6::GuiPrivate INTERFACE IMPORTED)
endif()
''')

    def configure(self, contents, success=True):
        (self.source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.25)
project(SnowQtContract NONE)
include("{ROOT.as_posix()}/cmake/SnowQt.cmake")
{contents}
''', encoding='utf-8')
        result = subprocess.run([
            self.cmake, '-S', str(self.source), '-B', str(self.root / 'configured'),
            '-G', 'Ninja', f'-DCMAKE_PREFIX_PATH={self.kit.as_posix()}',
            '-DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF',
            '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
        ], capture_output=True, text=True, encoding='utf-8', errors='replace', check=False)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_manifest_version_and_deprecation_policy_apply_to_nested_projects(self):
        self.write_qt()
        child = self.source / 'child'
        child.mkdir()
        (child / 'CMakeLists.txt').write_text(f'''
include("{ROOT.as_posix()}/cmake/SnowQt.cmake")
snow_find_qt(COMPONENTS Core)
get_property(definitions DIRECTORY PROPERTY COMPILE_DEFINITIONS)
if(NOT "QT_DISABLE_DEPRECATED_UP_TO={DEPRECATION_VERSION}" IN_LIST definitions)
    message(FATAL_ERROR "Nested project lost the deprecation policy")
endif()
''', encoding='utf-8')
        self.configure(f'''
snow_find_qt(COMPONENTS Core Gui GLOBAL)
if(NOT SNOW_QT_VERSION STREQUAL "{QT_VERSION}")
    message(FATAL_ERROR "Qt version does not match the manifest")
endif()
get_target_property(definitions Qt6::Core INTERFACE_COMPILE_DEFINITIONS)
if(definitions)
    message(FATAL_ERROR "Project policy modified imported Qt compile definitions")
endif()
add_subdirectory(child)
''')

    def test_wrong_public_version_is_rejected(self):
        self.write_qt(version='6.11.1')
        output = self.configure('snow_find_qt(COMPONENTS Core)', success=False)
        self.assertIn(QT_VERSION, output)

    def test_qt_package_version_bypass_cannot_accept_an_unaudited_kit(self):
        self.write_qt(version='6.11.1')
        version_file = self.kit / 'lib/cmake/Qt6/Qt6ConfigVersion.cmake'
        version_file.write_text('''
set(PACKAGE_VERSION "6.11.1")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT TRUE)
''', encoding='utf-8')
        output = self.configure('snow_find_qt(COMPONENTS Core)', success=False)
        self.assertIn('does not match the audited Qt', output)

    def test_private_module_matches_resolved_public_kit_and_is_global(self):
        self.write_qt()
        child = self.source / 'child'
        child.mkdir()
        (child / 'CMakeLists.txt').write_text('snow_find_qt_private(Gui)\n', encoding='utf-8')
        self.configure('''
snow_find_qt(COMPONENTS Core Gui GLOBAL)
add_subdirectory(child)
if(NOT TARGET Qt6::GuiPrivate)
    message(FATAL_ERROR "Private Qt target is unavailable to sibling consumers")
endif()
''')

    def test_private_module_from_another_version_is_rejected(self):
        self.write_qt()
        self.write_package('Qt6GuiPrivate', '6.11.1', '')
        output = self.configure('''
snow_find_qt(COMPONENTS Core Gui)
snow_find_qt_private(Gui)
''', success=False)
        self.assertIn('Qt6GuiPrivate', output)

    def test_private_module_requires_public_resolution(self):
        output = self.configure('snow_find_qt_private(Gui)', success=False)
        self.assertIn('Resolve Qt with snow_find_qt', output)

    def test_qtpaths_tool_location_works_without_qmake(self):
        self.write_qt()
        self.configure('''
snow_find_qt(COMPONENTS Core Gui)
add_executable(Qt6::qtpaths IMPORTED)
set_target_properties(Qt6::qtpaths PROPERTIES
    IMPORTED_CONFIGURATIONS RELEASE
    IMPORTED_LOCATION_RELEASE "${CMAKE_BINARY_DIR}/custom-tools/qtpaths")
snow_qt_bin_directory(bin)
if(NOT bin STREQUAL "${CMAKE_BINARY_DIR}/custom-tools")
    message(FATAL_ERROR "Qt tools were not resolved from the imported qtpaths target")
endif()
''')

    def test_qt_install_paths_work_when_optional_tools_are_absent(self):
        self.write_qt()
        self.configure('''
snow_find_qt(COMPONENTS Core Gui)
set(QT6_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/kit")
set(QT6_INSTALL_BINS "custom-tools")
snow_qt_bin_directory(bin)
if(NOT bin STREQUAL "${CMAKE_BINARY_DIR}/kit/custom-tools")
    message(FATAL_ERROR "Qt's exported install paths were ignored")
endif()
''')

    def assert_workspace(self, *, linkage, gui_features, core_features, png, jpeg, crt):
        self.write_qt(linkage=linkage, gui_features=gui_features, core_features=core_features)
        self.configure(f'''
snow_find_qt(COMPONENTS Core Gui)
include("{ROOT.as_posix()}/cmake/SnowWorkspace.cmake")
set(MSVC TRUE)
set(SNOW_APPS_QT_STATIC ON)
set(SNOW_APPS_RELEASE_STATIC OFF)
set(SNOW_APPS_API_BASE_URL "https://example.invalid")
set(Qt6_DIR "a-misleading-static-path-that-does-not-exist")
snow_workspace_configure_options()
if(NOT SNOW_IMAGE_LINK_PNG_DEPENDENCIES STREQUAL "{png}" OR
   NOT SNOW_IMAGE_LINK_JPEG_DEPENDENCIES STREQUAL "{jpeg}")
    message(FATAL_ERROR "Codec linkage does not match imported target metadata")
endif()
if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "{crt}")
    message(FATAL_ERROR "CRT linkage does not match imported target metadata")
endif()
''')

    def test_bundled_codecs_and_static_crt_use_imported_features(self):
        self.assert_workspace(linkage='STATIC', gui_features='', core_features='static_runtime',
                              png='OFF', jpeg='OFF', crt='MultiThreaded$<$<CONFIG:Debug>:Debug>')

    def test_static_library_with_dynamic_crt_and_system_codecs(self):
        self.assert_workspace(linkage='STATIC', gui_features='system_png;system_jpeg',
                              core_features='', png='ON', jpeg='ON',
                              crt='MultiThreaded$<$<CONFIG:Debug>:Debug>DLL')

    def test_codec_features_are_independent(self):
        self.assert_workspace(linkage='STATIC', gui_features='system_png', core_features='',
                              png='ON', jpeg='OFF', crt='MultiThreaded$<$<CONFIG:Debug>:Debug>DLL')

    def test_shared_qt_retains_codec_dependencies(self):
        self.assert_workspace(linkage='SHARED', gui_features='', core_features='',
                              png='ON', jpeg='ON', crt='MultiThreaded$<$<CONFIG:Debug>:Debug>DLL')

    def test_production_static_linkage_rejects_the_dynamic_crt(self):
        self.write_qt(linkage='STATIC', core_features='')
        output = self.configure(f'''
snow_find_qt(COMPONENTS Core Gui)
include("{ROOT.as_posix()}/cmake/SnowWorkspace.cmake")
set(MSVC TRUE)
set(SNOW_APPS_RELEASE_STATIC ON)
snow_workspace_configure_options()
''', success=False)
        self.assertIn('requires a Qt kit built with the static MSVC runtime', ' '.join(output.split()))


if __name__ == '__main__':
    unittest.main()
