"""Focused contracts for Windows target selection and cross-build dependencies."""

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which('cmake')


@unittest.skipUnless(CMAKE, 'CMake is required')
class WindowsArchitecture(unittest.TestCase):
    def run_cmake(self, contents, success=True):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'contract.cmake'
            script.write_text('cmake_minimum_required(VERSION 3.30)\n' + contents,
                              encoding='utf-8')
            result = subprocess.run([CMAKE, '-P', str(script)], capture_output=True,
                                    text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def fixture(self, architecture, extra=''):
        return f'''
set(WIN32 TRUE)
set(MSVC TRUE)
set(CMAKE_SIZEOF_VOID_P 8)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID {architecture})
include("{ROOT.as_posix()}/cmake/RustStaticLibrary.cmake")
{extra}
snow_detect_rust_target(target)
message(STATUS "SELECTED=${{SNOW_WINDOWS_ARCHITECTURE}};${{SNOW_WINDOWS_PLATFORM}};${{target}}")
'''

    def test_compiler_selects_target_independently_of_host(self):
        for compiler, arch, rust in [('x64', 'x64', 'x86_64'), ('ARM64', 'arm64', 'aarch64')]:
            with self.subTest(arch=arch):
                output = self.run_cmake(self.fixture(compiler))
                self.assertIn(f'SELECTED={arch};windows-{arch};{rust}-pc-windows-msvc', output)

    def test_architecture_alias_and_matching_triplet(self):
        self.run_cmake(self.fixture('ARM64', '''
set(SNOW_WINDOWS_ARCHITECTURE aarch64)
set(VCPKG_TARGET_TRIPLET arm64-windows-static)
'''))

    def test_rejects_architecture_triplet_and_rust_mismatches(self):
        for selection in ['set(SNOW_WINDOWS_ARCHITECTURE x64)',
                          'set(VCPKG_TARGET_TRIPLET x64-windows-static)',
                          'set(SNOW_RUST_TARGET x86_64-pc-windows-msvc)']:
            with self.subTest(selection=selection):
                self.assertIn('disagrees', self.run_cmake(self.fixture('ARM64', selection), False))

    def test_rejects_unsupported_windows_architecture(self):
        self.assertIn('Unsupported Windows architecture',
                      self.run_cmake(self.fixture('ARM64EC'), False))

    def test_rust_environment_uses_selected_tools_with_cxx_only_project(self):
        output = self.run_cmake(self.fixture('ARM64') + '''
set(CMAKE_CXX_COMPILER "C:/VS/bin/Hostx64/arm64/cl.exe")
set(CMAKE_LINKER "C:/VS/bin/Hostx64/arm64/link.exe")
set(CMAKE_AR "C:/VS/bin/Hostx64/arm64/lib.exe")
snow_windows_rust_environment(environment "${target}")
message(STATUS "ENVIRONMENT=${environment}")
''')
        self.assertIn('CARGO_TARGET_AARCH64_PC_WINDOWS_MSVC_LINKER=C:/VS/bin/Hostx64/arm64/link.exe', output)
        self.assertIn('CC_aarch64_pc_windows_msvc=C:/VS/bin/Hostx64/arm64/cl.exe', output)

    def test_macos_rust_selection_stays_independent(self):
        output = self.run_cmake(f'''
set(WIN32 FALSE)
set(APPLE TRUE)
set(CMAKE_OSX_ARCHITECTURES arm64)
include("{ROOT.as_posix()}/cmake/RustStaticLibrary.cmake")
snow_detect_rust_target(target)
message(STATUS "RUST=${{target}}")
''')
        self.assertIn('RUST=aarch64-apple-darwin', output)

    def libclang_fixture(self, directory, host):
        rustc = directory / ('rustc.cmd' if os.name == 'nt' else 'rustc')
        contents = (f'@echo off\necho host: {host}-pc-windows-msvc\n' if os.name == 'nt'
                    else f'#!/bin/sh\necho "host: {host}-pc-windows-msvc"\n')
        rustc.write_text(contents, encoding='utf-8')
        rustc.chmod(0o755)
        for arch, machine in [('x64', 0x8664), ('arm64', 0xAA64)]:
            binary = bytearray(134)
            binary[:2] = b'MZ'
            struct.pack_into('<I', binary, 60, 128)
            binary[128:132] = b'PE\0\0'
            struct.pack_into('<H', binary, 132, machine)
            (directory / arch).mkdir()
            (directory / arch / 'libclang.dll').write_bytes(binary)
        return f'''
set(WIN32 TRUE)
set(ENV{{RUSTC}} "{rustc.as_posix()}")
set(ENV{{LIBCLANG_PATH}} "")
include("{ROOT.as_posix()}/cmake/SnowRustLibclang.cmake")
'''

    def test_libclang_environment_overrides_compatible_cached_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = self.libclang_fixture(root, 'aarch64')
            other = root / 'other-arm64'
            shutil.copytree(root / 'arm64', other)
            output = self.run_cmake(fixture + f'''
set(ENV{{LIBCLANG_PATH}} "{(root / 'arm64').as_posix()}")
set(SNOW_LIBCLANG_BIN_DIR "{other.as_posix()}" CACHE PATH "")
snow_default_rust_libclang_directory(default)
snow_resolve_rust_libclang_directory(selected)
message(STATUS "DEFAULT=${{default}};SELECTED=${{selected}}")
''')
            self.assertIn(f'DEFAULT={(root / "arm64").as_posix()};'
                          f'SELECTED={(root / "arm64").as_posix()}', output)

    def test_libclang_matches_rust_host_independently_of_cargo_target(self):
        for host, selected, target in [('x86_64', 'x64', 'aarch64'),
                                       ('aarch64', 'arm64', 'x86_64')]:
            with self.subTest(host=host), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture = self.libclang_fixture(root, host)
                output = self.run_cmake(fixture + f'''
set(CMAKE_HOST_SYSTEM_PROCESSOR ARM64)
set(SNOW_RUST_TARGET {target}-pc-windows-msvc)
set(SNOW_LIBCLANG_BIN_DIR "{(root / selected).as_posix()}")
snow_resolve_rust_libclang_directory(selected)
message(STATUS "SELECTED=${{selected}}")
''')
                self.assertIn(f'SELECTED={(root / selected).as_posix()}', output)

    def test_libclang_rejects_stale_cache_before_environment_override(self):
        for variable in ('SNOW_LIBCLANG_BIN_DIR', 'SNOW_SHOT_LIBCLANG_DIR'):
            with self.subTest(variable=variable), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                fixture = self.libclang_fixture(root, 'aarch64')
                output = self.run_cmake(fixture + f'''
set(ENV{{LIBCLANG_PATH}} "{(root / 'arm64').as_posix()}")
set({variable} "{(root / 'x64').as_posix()}" CACHE PATH "")
snow_resolve_rust_libclang_directory(selected)
''', False)
                self.assertIn(f'{variable} has x64 libclang', output)
                self.assertIn('Rust host requires arm64', output)

    def test_libclang_rejects_invalid_pe_and_wrong_environment_machine(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = self.libclang_fixture(root, 'aarch64')
            output = self.run_cmake(fixture + f'''
set(ENV{{LIBCLANG_PATH}} "{(root / 'x64').as_posix()}")
snow_resolve_rust_libclang_directory(selected)
''', False)
            self.assertIn('LIBCLANG_PATH has x64 libclang', output)
            (root / 'arm64/libclang.dll').write_bytes(b'MZ')
            output = self.run_cmake(fixture + f'''
set(SNOW_LIBCLANG_BIN_DIR "{(root / 'arm64').as_posix()}")
snow_resolve_rust_libclang_directory(selected)
''', False)
            self.assertIn('Invalid Windows libclang PE image', output)

    @unittest.skipUnless(shutil.which('ninja'), 'Ninja is required')
    def test_cargo_commands_capture_host_libclang_for_cross_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = self.libclang_fixture(root, 'x86_64')
            cargo = root / ('cargo.cmd' if os.name == 'nt' else 'cargo')
            cargo.write_text('@exit /b 0\n' if os.name == 'nt' else '#!/bin/sh\nexit 0\n',
                             encoding='utf-8')
            cargo.chmod(0o755)
            (root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.30)
project(CargoLibclangContract LANGUAGES NONE)
{fixture}
set(MSVC TRUE)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_CXX_COMPILER "C:/VS/bin/Hostx64/arm64/cl.exe")
set(CMAKE_LINKER "C:/VS/bin/Hostx64/arm64/link.exe")
set(CMAKE_AR "C:/VS/bin/Hostx64/arm64/lib.exe")
set(CARGO_EXECUTABLE "{cargo.as_posix()}")
set(SNOW_LIBCLANG_BIN_DIR "{(root / 'x64').as_posix()}")
include("{ROOT.as_posix()}/cmake/RustStaticLibrary.cmake")
snow_add_rust_static_library(fixture PACKAGE fixture MANIFEST_DIR "{root.as_posix()}")
snow_add_rust_executable(fixture_executable PACKAGE fixture MANIFEST_DIR "{root.as_posix()}")
''', encoding='utf-8')
            build = root / 'build'
            result = subprocess.run([CMAKE, '-G', 'Ninja', '-S', str(root), '-B', str(build)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = (build / 'build.ninja').read_text(encoding='utf-8')
            self.assertEqual(commands.count(f'LIBCLANG_PATH={(root / "x64").as_posix()}'), 2)
            self.assertIn('--target aarch64-pc-windows-msvc', commands)

    def test_ffmpeg_arm_profile_retains_supported_encoders(self):
        port = ROOT / 'cmake/vcpkg-overlay-ports/ffmpeg'
        manifest = json.loads((port / 'vcpkg.json').read_text(encoding='utf-8'))
        dependencies = manifest['features']['snow-shot-minimal']['dependencies']
        common = next(dependency['features'] for dependency in dependencies
                      if dependency['name'] == 'ffmpeg' and 'platform' not in dependency)
        vendor = next(dependency for dependency in dependencies
                      if dependency['name'] == 'ffmpeg' and dependency.get('platform') == 'x64')
        text = (port / 'portfile.cmake').read_text(encoding='utf-8')
        start = text.index('if("snow-shot-minimal" IN_LIST FEATURES)',
                           text.index('if("rubberband" IN_LIST FEATURES)'))
        fragment = text[start:text.index('if("snow-macos-media" IN_LIST FEATURES)', start)]
        for arch in ('arm64', 'x64'):
            features = ['snow-shot-minimal', *common]
            if arch == 'x64':
                features.extend(vendor['features'])
            output = self.run_cmake(f'''
set(VCPKG_TARGET_IS_WINDOWS TRUE)
set(FEATURES {';'.join(features)})
{fragment}
message(STATUS "ENCODERS=${{SNOW_SHOT_ENCODERS}}")
''')
            self.assertIn('h264_mf', output)
            self.assertIn('libx264,libx265', output)
            for encoder in ('h264_nvenc', 'h264_amf', 'h264_qsv'):
                self.assertEqual(encoder in output, arch == 'x64')

    def test_crashpad_uses_runnable_host_compilers_and_pinned_toolset(self):
        overlay = (ROOT / 'cmake/vcpkg-overlay-ports/crashpad/portfile.cmake').read_text(
            encoding='utf-8')
        build_tools = re.search(r'set\(_snow_build_tools \[=\[(.*?)\]=\]\)', overlay,
                                re.DOTALL).group(1)
        original = '''
vswhere_args = [vswhere_path, '-latest', '-property', 'installationPath']
def toolchain_arguments(script_path):
  for arch in ('x86', 'amd64', 'arm64'):
    args = [script_path]
    script_arch_name = arch
    if script_path.endswith('SetEnv.cmd') and arch == 'amd64':
      script_arch_name = '/x64'
    if arch == 'arm64':
      script_arch_name = 'x86_arm64'
    args.extend((script_arch_name, '&&', 'set'))
    yield args
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            base = source / 'third_party/mini_chromium/mini_chromium/build'
            (base / 'config').mkdir(parents=True)
            (base / 'config/BUILD.gn').write_text('flags = ["/GL"]', encoding='utf-8')
            helper = base / 'win_helper.py'
            helper.write_text(original, encoding='utf-8')
            self.run_cmake(f'''
set(SOURCE_PATH "{source.as_posix()}")
function(vcpkg_replace_string path before after)
    file(READ "${{path}}" text)
    string(REPLACE "${{before}}" "${{after}}" replaced "${{text}}")
    if(replaced STREQUAL text)
        message(FATAL_ERROR "Expected pinned source string is missing")
    endif()
    file(WRITE "${{path}}" "${{replaced}}")
endfunction()
{build_tools}
''')
            namespace = {'os': os, 'vswhere_path': 'vswhere'}
            exec(compile(helper.read_text(encoding='utf-8'), str(helper), 'exec'), namespace)
            for host, expected in [('x64', ['amd64_x86', 'amd64', 'amd64_arm64']),
                                   ('arm64', ['arm64_x86', 'arm64_x64', 'arm64'])]:
                with self.subTest(host=host), patch.dict(
                        os.environ, {'SNOW_MSVC_HOST_ARCHITECTURE': host}, clear=True):
                    arguments = list(namespace['toolchain_arguments']('vcvarsall.bat'))
                    self.assertEqual([argument[1] for argument in arguments], expected)
                    self.assertTrue(all(argument[2:] == ['-vcvars_ver=14.51', '&&', 'set']
                                        for argument in arguments))

    def test_toolchain_separates_host_tools_and_target_libraries(self):
        for host in ('x64', 'arm64'):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                msvc = root / 'MSVC/14.51.fixture'
                sdk = root / 'Windows Kits/10'
                version = '10.0.26100.0'
                compiler = msvc / f'bin/Host{host}/arm64'
                compiler.mkdir(parents=True)
                for name in ('cl.exe', 'link.exe', 'lib.exe', 'armasm64.exe'):
                    (compiler / name).touch()
                sdk_tools = sdk / f'bin/{version}/{host}'
                sdk_tools.mkdir(parents=True)
                for name in ('mt.exe', 'rc.exe'):
                    (sdk_tools / name).touch()
                for path in [msvc / 'lib/arm64', sdk / f'Include/{version}',
                             sdk / f'Lib/{version}/um/arm64',
                             sdk / f'Lib/{version}/ucrt/arm64']:
                    path.mkdir(parents=True)
                platform_toolchain = root / 'vcpkg/scripts/toolchains/windows.cmake'
                platform_toolchain.parent.mkdir(parents=True)
                platform_toolchain.write_text('', encoding='utf-8')
                (root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.30)
project(ToolchainContract LANGUAGES NONE)
set(WIN32 TRUE)
set(_VCPKG_ROOT_DIR "{(root / 'vcpkg').as_posix()}")
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(ENV{{SNOW_MSVC_HOST_ARCHITECTURE}} {host})
set(ENV{{VCToolsInstallDir}} "{msvc.as_posix()}")
set(ENV{{WindowsSdkDir}} "{sdk.as_posix()}")
set(ENV{{WindowsSDKVersion}} {version})
include("{ROOT.as_posix()}/cmake/vcpkg-msvc-145-14.51-toolchain.cmake")
message(STATUS "COMPILER=${{CMAKE_CXX_COMPILER}}")
message(STATUS "RESOURCE=${{CMAKE_RC_COMPILER}}")
get_property(paths DIRECTORY PROPERTY LINK_DIRECTORIES)
message(STATUS "LIBRARIES=${{paths}}")
''', encoding='utf-8')
                result = subprocess.run([CMAKE, '-S', str(root), '-B', str(root / 'build')],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f'/Host{host}/arm64/cl.exe', result.stdout)
                self.assertIn(f'/{version}/{host}/rc.exe', result.stdout)
                self.assertIn(f'/{version}/um/arm64', result.stdout)
                self.assertNotIn('/lib/x64', result.stdout)


if __name__ == '__main__':
    unittest.main()
