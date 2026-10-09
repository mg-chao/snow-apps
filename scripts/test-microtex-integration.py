#!/usr/bin/env python3
"""Focused MicroTeX source regeneration checks without Qt or network access."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MicroTeXIntegration(unittest.TestCase):
    def test_patch_changes_regenerate_a_pristine_source_copy(self):
        with tempfile.TemporaryDirectory(prefix='snow microtex tests ') as temporary:
            root = Path(temporary)
            support = root / 'cmake/microtex'
            support.mkdir(parents=True)
            shutil.copyfile(ROOT / 'cmake/MicroTeX.cmake', root / 'cmake/MicroTeX.cmake')
            upstream = root / 'upstream'
            source = upstream / 'src/latex.cpp'
            source.parent.mkdir(parents=True)
            source.write_text('// pristine source\n', encoding='utf-8')
            os.utime(source, (1700000000, 1700000000))
            for relative in ('src/render.cpp', 'src/platform/qt/graphic_qt.cpp'):
                path = upstream / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            patch = support / 'patch_source.py'
            patch.write_text('''from pathlib import Path
import sys
path = Path(sys.argv[1]) / 'src/latex.cpp'
assert path.read_text() == '// pristine source\\n', 'source was patched twice'
path.write_text('// patched source\\n')
''', encoding='utf-8')
            for name in ('snow_preview.h', 'snow_preview.cpp'):
                (support / name).touch()
            for relative in (
                'snow_shot/include/snow_shot/presentation/screenshotlatexrenderer.h',
                'snow_shot/src/presentation/ocr/screenshotlatexrenderer.cpp',
            ):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            package = root / 'tinyxml2'
            package.mkdir()
            (package / 'tinyxml2Config.cmake').write_text(
                'add_library(tinyxml2::tinyxml2 INTERFACE IMPORTED)\n', encoding='utf-8')
            (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 4.2)
project(MicroTeXSourceRegeneration LANGUAGES CXX)
add_library(Qt6::Core INTERFACE IMPORTED)
add_library(Qt6::Gui INTERFACE IMPORTED)
function(qt_add_resources output)
    set(${output} "" PARENT_SCOPE)
endfunction()
include(cmake/MicroTeX.cmake)
snow_shot_add_latex_renderer()
''', encoding='utf-8')
            command = [
                'cmake', '-S', str(root), '-B', str(root / 'build'),
                f'-DFETCHCONTENT_SOURCE_DIR_SNOW_MICROTEX_SOURCE={upstream}',
                f'-Dtinyxml2_DIR={package}',
            ]

            def configure():
                result = subprocess.run(command, capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            configure()
            copied = root / 'build/microtex-source/src/latex.cpp'
            self.assertEqual(copied.read_text(), '// patched source\n')
            # CMake's timestamp-based copy can skip an already patched destination.
            os.utime(copied, (source.stat().st_atime, source.stat().st_mtime))
            obsolete = copied.with_name('obsolete.cpp')
            obsolete.touch()
            with patch.open('a', encoding='utf-8') as stream:
                stream.write('\n# A reviewed integration patch changed.\n')
            configure()
            self.assertEqual(copied.read_text(), '// patched source\n')
            self.assertEqual(source.read_text(), '// pristine source\n')
            self.assertFalse(obsolete.exists(), 'regeneration retained stale source files')


if __name__ == '__main__':
    unittest.main()
