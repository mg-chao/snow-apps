#!/usr/bin/env python3
"""Focused AVX2 source-flag, unity, and precompiled-header contract tests."""

import json
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SimdCMakeFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='snow-simd-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.build = self.root / 'build'
        self.cmake = shutil.which('cmake')
        self.assertIsNotNone(self.cmake, 'CMake must be available')

    def configure(self, contents, *arguments):
        (self.source / 'CMakeLists.txt').write_text(contents, encoding='utf-8')
        result = subprocess.run(
            [self.cmake, '-S', str(self.source), '-B', str(self.build), '-G', 'Ninja',
             *arguments], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class SimdCMakeContract(SimdCMakeFixture):
    def properties(self, *, processor, apple=False, architectures='', msvc=False,
                   windows_architecture='', compiler='AppleClang'):
        self.configure(f'''
cmake_minimum_required(VERSION 3.22)
project(SnowSimdContract NONE)
set(APPLE {"TRUE" if apple else "FALSE"})
set(MSVC {"TRUE" if msvc else "FALSE"})
set(CMAKE_SYSTEM_PROCESSOR "{processor}")
set(CMAKE_OSX_ARCHITECTURES "{architectures}")
set(CMAKE_CXX_COMPILER_ID "{compiler}")
set(SNOW_WINDOWS_ARCHITECTURE "{windows_architecture}")
include("{ROOT.as_posix()}/cmake/SnowSimd.cmake")
set_property(SOURCE filter.cpp pen.cpp PROPERTY COMPILE_OPTIONS "-DSNOW_KEEP_OPTION")
snow_configure_avx2_sources(filter.cpp pen.cpp)
foreach(source IN ITEMS filter pen baseline)
    foreach(property IN ITEMS COMPILE_OPTIONS SKIP_UNITY_BUILD_INCLUSION SKIP_PRECOMPILE_HEADERS)
        get_source_file_property(value "${{source}}.cpp" "${{property}}")
        file(APPEND "${{CMAKE_BINARY_DIR}}/properties.txt" "${{source}}.${{property}}=${{value}}\n")
    endforeach()
endforeach()
''')
        return dict(line.split('=', 1) for line in
                    (self.build / 'properties.txt').read_text(encoding='utf-8').splitlines())

    def assert_kernels(self, properties, options='', pch=False):
        expected = '-DSNOW_KEEP_OPTION' + (';' + options if options else '')
        for source in ('filter', 'pen'):
            self.assertEqual(properties[f'{source}.COMPILE_OPTIONS'], expected)
            self.assertEqual(properties[f'{source}.SKIP_UNITY_BUILD_INCLUSION'],
                             'ON' if options else 'NOTFOUND')
            self.assertEqual(properties[f'{source}.SKIP_PRECOMPILE_HEADERS'],
                             'ON' if pch else 'NOTFOUND')
        self.assertEqual(properties['baseline.COMPILE_OPTIONS'], 'NOTFOUND')
        self.assertEqual(properties['baseline.SKIP_UNITY_BUILD_INCLUSION'], 'NOTFOUND')
        self.assertEqual(properties['baseline.SKIP_PRECOMPILE_HEADERS'], 'NOTFOUND')

    def test_arm_host_x64_macos_selects_avx2_and_isolates_clang_kernels(self):
        self.assert_kernels(self.properties(processor='arm64', apple=True,
                                           architectures='x86_64'), '-mavx2', pch=True)

    def test_x64_host_arm_macos_does_not_select_avx2(self):
        self.assert_kernels(self.properties(processor='x86_64', apple=True,
                                           architectures='arm64'))

    def test_native_arm_macos_keeps_baseline_flags(self):
        self.assert_kernels(self.properties(processor='arm64', apple=True,
                                           architectures='arm64'))

    def test_macos_without_explicit_architecture_uses_processor(self):
        self.assert_kernels(self.properties(processor='x86_64', apple=True),
                            '-mavx2', pch=True)

    def test_universal_macos_limits_avx2_to_x64_slice(self):
        self.assert_kernels(self.properties(processor='arm64', apple=True,
                                           architectures='arm64;x86_64'),
                            '-Xarch_x86_64;-mavx2', pch=True)

    def test_windows_x64_uses_selected_compiler_architecture(self):
        self.assert_kernels(self.properties(processor='arm64', msvc=True,
                                           windows_architecture='x64', compiler='MSVC'),
                            '/arch:AVX2')

    def test_windows_clang_x64_isolates_pch(self):
        self.assert_kernels(self.properties(processor='arm64', msvc=True,
                                           windows_architecture='x64', compiler='Clang'),
                            '/arch:AVX2', pch=True)

    def test_windows_arm_does_not_inherit_host_x64_flags(self):
        self.assert_kernels(self.properties(processor='AMD64', msvc=True,
                                           windows_architecture='arm64', compiler='MSVC'))

    def test_other_x86_compilers_keep_avx2_sources_out_of_unity(self):
        self.assert_kernels(self.properties(processor='x86_64', compiler='GNU'), '-mavx2')

    def test_other_arm_compilers_keep_baseline_flags(self):
        self.assert_kernels(self.properties(processor='aarch64', compiler='GNU'))


@unittest.skipUnless(platform.system() == 'Darwin', 'Requires the Apple compiler')
class MacOSSimdCompilation(SimdCMakeFixture):
    def compile_architecture(self, architecture):
        (self.source / 'baseline.cpp').write_text('''
#ifdef __AVX2__
#error AVX2 flags leaked into a baseline translation unit
#endif
int baseline() { return 1; }
''', encoding='utf-8')
        for name in ('filter', 'pen'):
            (self.source / f'{name}.cpp').write_text(f'''
#if defined(__x86_64__)
#ifndef __AVX2__
#error x64 kernel requires AVX2
#endif
#include <immintrin.h>
int {name}(const int* source) {{
    return _mm256_extract_epi32(_mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(source)), 0);
}}
#else
int {name}(const int* source) {{ return source[0]; }}
#endif
''', encoding='utf-8')
        self.configure(f'''
cmake_minimum_required(VERSION 3.22)
project(SnowSimdCompilation CXX)
# Reproduce the host processor metadata from the failed x64 app configure.
set(CMAKE_SYSTEM_PROCESSOR arm64)
include("{ROOT.as_posix()}/cmake/SnowSimd.cmake")
add_library(kernels STATIC baseline.cpp filter.cpp pen.cpp)
set_target_properties(kernels PROPERTIES UNITY_BUILD ON)
target_precompile_headers(kernels PRIVATE <vector>)
snow_configure_avx2_sources(filter.cpp pen.cpp)
''', f'-DCMAKE_OSX_ARCHITECTURES={architecture}', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON')
        commands = json.loads((self.build / 'compile_commands.json').read_text(encoding='utf-8'))
        unity = ''.join(path.read_text(encoding='utf-8') for path in
                        self.build.glob('CMakeFiles/kernels.dir/Unity/*.cxx'))
        if 'x86_64' in architecture:
            for name in ('filter', 'pen'):
                command = next(item['command'] for item in commands
                               if Path(item['file']).name == f'{name}.cpp')
                self.assertIn('-mavx2', command)
                self.assertNotIn('-include-pch', command)
                self.assertNotIn(f'/{name}.cpp', unity)
            baseline_commands = [item['command'] for item in commands
                                 if Path(item['file']).name not in ('filter.cpp', 'pen.cpp')]
            self.assertTrue(baseline_commands)
            self.assertTrue(all('-mavx2' not in command for command in baseline_commands))
        else:
            self.assertTrue(all('-mavx2' not in item['command'] for item in commands))
        result = subprocess.run([self.cmake, '--build', str(self.build), '--parallel', '2'],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_compile_x64_kernels_with_baseline_unity_and_pch(self):
        self.compile_architecture('x86_64')

    def test_compile_native_arm_kernels_with_baseline_unity_and_pch(self):
        self.compile_architecture('arm64')

    def test_compile_universal_kernels_with_architecture_specific_flags(self):
        self.compile_architecture('arm64;x86_64')


if __name__ == '__main__':
    unittest.main()
