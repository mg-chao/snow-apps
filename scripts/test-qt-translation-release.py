#!/usr/bin/env python3
"""Check Qt stock-dialog translation merging with isolated CMake fixtures."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]


class QtTranslationRelease(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='qt-translation-release-', dir=ROOT / 'build')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.translations = self.root / 'Qt kit/translations'
        self.translations.mkdir(parents=True)
        for language in ('zh_CN', 'zh_TW'):
            (self.translations / f'qtbase_{language}.qm').touch()

    def configure(self, success=True, custom_translation_path=False):
        location = (f'set(QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR "{self.translations.as_posix()}")'
                    if custom_translation_path else
                    f'set(QT6_INSTALL_PREFIX "{self.translations.parent.as_posix()}")\n'
                    'set(QT6_INSTALL_TRANSLATIONS translations)')
        (self.source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.25)
project(SnowQtTranslationRelease NONE)
include("{ROOT.as_posix()}/cmake/SnowShotTranslationRelease.cmake")
{location}
function(qt_add_lrelease)
    cmake_parse_arguments(PARSE_ARGV 0 arg "MERGE_QT_TRANSLATIONS"
        "LRELEASE_TARGET;QM_FILES_OUTPUT_VARIABLE;QM_OUTPUT_DIRECTORY"
        "TS_FILES;QT_TRANSLATION_CATALOGS;OPTIONS")
    if(NOT "-fail-on-unfinished" IN_LIST arg_OPTIONS)
        message(FATAL_ERROR "Translation completeness was not enforced")
    endif()
    set(qm_files)
    foreach(ts IN LISTS arg_TS_FILES)
        get_filename_component(name "${{ts}}" NAME_WE)
        if(name MATCHES "_en_US$")
            if(arg_MERGE_QT_TRANSLATIONS)
                message(FATAL_ERROR "English incorrectly requires an external Qt catalog")
            endif()
        elseif(NOT arg_MERGE_QT_TRANSLATIONS OR NOT arg_QT_TRANSLATION_CATALOGS STREQUAL "qtbase")
            message(FATAL_ERROR "Chinese stock-widget catalogs were not merged")
        endif()
        list(APPEND qm_files "${{arg_QM_OUTPUT_DIRECTORY}}/${{name}}.qm")
    endforeach()
    add_custom_target(${{arg_LRELEASE_TARGET}})
    set(${{arg_QM_FILES_OUTPUT_VARIABLE}} ${{qm_files}} PARENT_SCOPE)
endfunction()
foreach(edition IN ITEMS full mini)
    snow_shot_release_translation_catalogs(${{edition}}-translations
        "${{CMAKE_CURRENT_BINARY_DIR}}/${{edition}}-ts"
        "${{CMAKE_CURRENT_BINARY_DIR}}/${{edition}}-qm" outputs)
    list(LENGTH outputs count)
    if(NOT count EQUAL 3)
        message(FATAL_ERROR "An edition lost a locale resource")
    endif()
    foreach(locale IN ITEMS en_US zh_CN zh_TW)
        if(NOT "${{CMAKE_CURRENT_BINARY_DIR}}/${{edition}}-qm/snow_shot_${{locale}}.qm" IN_LIST outputs)
            message(FATAL_ERROR "An edition's catalog resource path changed")
        endif()
    endforeach()
    get_target_property(dependencies ${{edition}}-translations MANUALLY_ADDED_DEPENDENCIES)
    if(NOT "${{edition}}-translations_source_language" IN_LIST dependencies OR
       NOT "${{edition}}-translations_localized" IN_LIST dependencies)
        message(FATAL_ERROR "The public release target must build every locale")
    endif()
endforeach()
''', encoding='utf-8')
        result = subprocess.run([shutil.which('cmake'), '-S', str(self.source),
                                 '-B', str(self.root / 'configured'), '-G', 'Ninja'],
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_english_and_chinese_catalogs_keep_their_edition_resources(self):
        self.assertFalse((self.translations / 'qtbase_en_US.qm').exists())
        output = self.configure()
        self.assertNotIn('Warning', output)

    def test_a_missing_simplified_catalog_is_rejected(self):
        (self.translations / 'qtbase_zh_CN.qm').unlink()
        self.assertIn('qtbase_zh_CN.qm', self.configure(success=False))

    def test_a_missing_traditional_catalog_is_rejected(self):
        (self.translations / 'qtbase_zh_TW.qm').unlink()
        self.assertIn('qtbase_zh_TW.qm', self.configure(success=False))

    def test_qt_custom_translation_install_path_is_supported(self):
        self.configure(custom_translation_path=True)


@unittest.skipUnless(os.environ.get('SNOW_TEST_QT_TRANSLATION_KIT'),
                     'Set SNOW_TEST_QT_TRANSLATION_KIT to verify the installed Qt Linguist tools')
class InstalledQtTranslationRelease(unittest.TestCase):
    def test_native_release_merges_stock_dialogs_only_into_chinese(self):
        kit = Path(os.environ['SNOW_TEST_QT_TRANSLATION_KIT']).resolve()
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='qt-native-translation-', dir=ROOT / 'build') as directory:
            source = Path(directory) / 'source'
            source.mkdir()
            for language in ('en_US', 'zh_CN', 'zh_TW'):
                (source / f'snow_shot_{language}.ts').write_text(
                    '<?xml version="1.0" encoding="utf-8"?>\n'
                    f'<TS version="2.1" language="{language}"><context>'
                    '<name>SnowTranslationFixture</name><message><source>Fixture message</source>'
                    '<translation>Fixture translation</translation></message></context></TS>\n',
                    encoding='utf-8')
            (source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.25)
project(SnowNativeQtTranslationRelease LANGUAGES CXX)
include("{ROOT.as_posix()}/cmake/SnowQt.cmake")
snow_find_qt(COMPONENTS Core LinguistTools)
include("{ROOT.as_posix()}/cmake/SnowShotTranslationRelease.cmake")
snow_shot_release_translation_catalogs(fixture-translations
    "${{CMAKE_CURRENT_SOURCE_DIR}}" "${{CMAKE_CURRENT_BINARY_DIR}}/qm" outputs)
file(GENERATE OUTPUT "${{CMAKE_CURRENT_BINARY_DIR}}/lconvert-path.txt"
    CONTENT "$<TARGET_FILE:Qt6::lconvert>")
''', encoding='utf-8')
            binary = Path(directory) / 'configured'
            configure = [shutil.which('cmake'), '-S', str(source), '-B', str(binary),
                         '-G', 'Ninja', f'-DQt6_DIR={kit.as_posix()}/lib/cmake/Qt6']
            if os.environ.get('SNOW_TEST_QT_DEPENDENCY_PREFIX'):
                configure.append('-DCMAKE_PREFIX_PATH=' +
                                 Path(os.environ['SNOW_TEST_QT_DEPENDENCY_PREFIX']).resolve().as_posix())
            result = subprocess.run(configure,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('cannot be found for language', result.stderr)
            result = subprocess.run([shutil.which('cmake'), '--build', str(binary),
                                     '--target', 'fixture-translations'],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            converter = (binary / 'lconvert-path.txt').read_text()
            for language in ('en_US', 'zh_CN', 'zh_TW'):
                catalog = binary / f'{language}.ts'
                result = subprocess.run([converter, '-i', str(binary / 'qm' / f'snow_shot_{language}.qm'),
                                         '-o', str(catalog)], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                contexts = {context.findtext('name') for context in ET.parse(catalog).getroot()}
                self.assertIn('SnowTranslationFixture', contexts)
                if language == 'en_US':
                    self.assertEqual(contexts, {'SnowTranslationFixture'})
                else:
                    self.assertIn('QFileDialog', contexts)
                    self.assertIn('QColorDialog', contexts)


if __name__ == '__main__':
    unittest.main()
