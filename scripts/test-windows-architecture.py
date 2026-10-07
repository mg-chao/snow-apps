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
set(APPLE FALSE)
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

    def test_arm64_static_dependencies_use_native_optimized_objects(self):
        output = self.run_cmake(f'''
include("{ROOT.as_posix()}/cmake/vcpkg-overlay-triplets/arm64-windows-static.cmake")
message(STATUS "C_FLAGS=${{VCPKG_C_FLAGS_RELEASE}}")
message(STATUS "CXX_FLAGS=${{VCPKG_CXX_FLAGS_RELEASE}}")
message(STATUS "LINK_FLAGS=${{VCPKG_LINKER_FLAGS_RELEASE}}")
''')
        for variable in ('C_FLAGS', 'CXX_FLAGS'):
            flags = re.search(rf'{variable}=([^\n]*)', output).group(1).split()
            self.assertIn('/GL-', flags)
            self.assertNotIn('/GL', flags)
            for flag in ('/O2', '/Gw', '/Gy'):
                self.assertIn(flag, flags)
        link_flags = re.search(r'LINK_FLAGS=([^\n]*)', output).group(1).split()
        self.assertFalse(any(flag.upper().startswith('/LTCG') for flag in link_flags))
        self.assertIn('/OPT:REF', link_flags)
        self.assertIn('/OPT:ICF', link_flags)

    def test_x64_static_dependencies_retain_ltcg(self):
        output = self.run_cmake(f'''
include("{ROOT.as_posix()}/cmake/vcpkg-overlay-triplets/x64-windows-static.cmake")
message(STATUS "C_FLAGS=${{VCPKG_C_FLAGS_RELEASE}}")
message(STATUS "CXX_FLAGS=${{VCPKG_CXX_FLAGS_RELEASE}}")
message(STATUS "LINK_FLAGS=${{VCPKG_LINKER_FLAGS_RELEASE}}")
''')
        for variable in ('C_FLAGS', 'CXX_FLAGS'):
            flags = re.search(rf'{variable}=([^\n]*)', output).group(1).split()
            self.assertIn('/GL', flags)
            self.assertNotIn('/GL-', flags)
        self.assertIn('/LTCG', re.search(r'LINK_FLAGS=([^\n]*)', output).group(1).split())

    def test_arm64_presets_disable_cpp_ipo_without_changing_rust_target(self):
        presets = json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))
        by_name = {preset['name']: preset for preset in presets['configurePresets']}

        def variables(name):
            preset = by_name[name]
            inherited = preset.get('inherits', [])
            if isinstance(inherited, str):
                inherited = [inherited]
            result = {}
            for parent in reversed(inherited):
                result.update(variables(parent))
            result.update(preset.get('cacheVariables', {}))
            return result

        for suffix in ('performance', 'release', 'fast'):
            with self.subTest(preset=suffix):
                cache = variables(f'snow-shot-msvc-arm64-{suffix}')
                self.assertEqual(cache['CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE'], 'OFF')
                self.assertEqual(cache['SNOW_RUST_TARGET'], 'aarch64-pc-windows-msvc')
        for name in ('windows-msvc-performance', 'snow-shot-msvc-release'):
            with self.subTest(preset=name):
                self.assertEqual(variables(name)['CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE'],
                                 'ON')

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


@unittest.skipUnless(CMAKE and shutil.which('git'), 'CMake and Git are required')
class OnnxMlasCompilerPolicy(unittest.TestCase):
    """Execute the overlay's applied CMake block without configuring ONNX itself."""

    patch_name = 'fix-arm64-msvc-mlas-stack-cookie.patch'
    port = ROOT / 'cmake/vcpkg-overlay-ports/onnxruntime'
    baseline_options = ['/O2', '/GS', '/sdl', '/guard:cf', '/Ob3', '/GL-']

    def applied_policy(self, directory):
        patch_text = (self.port / self.patch_name).read_text(encoding='utf-8')
        hunks = list(re.finditer(r'^@@ -(\d+),(\d+) \+(\d+),(\d+) @@[^\n]*\n',
                                patch_text, re.MULTILINE))
        self.assertEqual(len(hunks), 1, 'Update the fixture if the overlay gains other changes')
        hunk = hunks[0]
        lines = patch_text[hunk.end():].splitlines(keepends=True)
        self.assertTrue(all(line.startswith((' ', '+')) for line in lines),
                        'This policy fixture expects an insertion-only patch')
        before = [line[1:] for line in lines if line.startswith(' ')]
        added = [line[1:] for line in lines if line.startswith('+')]
        self.assertEqual(len(before), int(hunk.group(2)))
        self.assertEqual(len(before) + len(added), int(hunk.group(4)))
        # Reconstruct the hunk's upstream context, then apply the real overlay.
        # The unrelated upstream target definitions need not run in this fixture.
        first_line = int(hunk.group(1)) - 1
        upstream = directory / 'cmake/onnxruntime_mlas.cmake'
        upstream.parent.mkdir()
        upstream.write_text('# Omitted upstream line\n' * first_line + ''.join(before),
                            encoding='utf-8')
        result = subprocess.run(['git', 'apply', str(self.port / self.patch_name)],
                                cwd=directory, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        leading_context = next(index for index, line in enumerate(lines)
                               if line.startswith('+'))
        applied = upstream.read_text(encoding='utf-8').splitlines(keepends=True)
        block = ''.join(applied[first_line + leading_context:
                                first_line + leading_context + len(added)])
        self.assertEqual(block, ''.join(added))
        return block

    def check_policy(self, windows, compiler, architecture, enabled):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            policy = self.applied_policy(root)
            options = ';'.join(self.baseline_options)
            (root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.30)
project(OnnxMlasPolicyContract LANGUAGES NONE)
set(WIN32 {str(windows).upper()})
set(CMAKE_CXX_COMPILER_ID "{compiler}")
set(onnxruntime_target_platform "{architecture}")
set(MLAS_SRC_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}/mlas")
set(dequant "${{MLAS_SRC_DIR}}/sqnbitgemm_kernel_neon_fp32.cpp")
set(neighbor "${{MLAS_SRC_DIR}}/sqnbitgemm_kernel_neon_fp16.cpp")
set_property(SOURCE "${{dequant}}" "${{neighbor}}" PROPERTY COMPILE_OPTIONS "{options}")
add_library(onnxruntime_mlas INTERFACE)
target_compile_options(onnxruntime_mlas INTERFACE {options})
set(CMAKE_CXX_FLAGS "/O2 /GS /sdl /guard:cf /GL-")
{policy}
get_property(dequant_options SOURCE "${{dequant}}" PROPERTY COMPILE_OPTIONS)
get_property(neighbor_options SOURCE "${{neighbor}}" PROPERTY COMPILE_OPTIONS)
get_property(target_options TARGET onnxruntime_mlas PROPERTY INTERFACE_COMPILE_OPTIONS)
message(STATUS "DEQUANT_OPTIONS=${{dequant_options}}")
message(STATUS "NEIGHBOR_OPTIONS=${{neighbor_options}}")
message(STATUS "TARGET_OPTIONS=${{target_options}}")
message(STATUS "CXX_FLAGS=${{CMAKE_CXX_FLAGS}}")
''', encoding='utf-8')
            result = subprocess.run([CMAKE, '-S', str(root), '-B', str(root / 'build')],
                                    capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, output)
            for name in ('DEQUANT_OPTIONS', 'NEIGHBOR_OPTIONS', 'TARGET_OPTIONS'):
                actual = re.search(rf'{name}=([^\n]*)', output).group(1).split(';')
                expected = self.baseline_options + (['/Ob0'] if enabled and
                                                    name == 'DEQUANT_OPTIONS' else [])
                self.assertEqual(actual, expected, output)
            self.assertIn('CXX_FLAGS=/O2 /GS /sdl /guard:cf /GL-', output)

    def test_windows_msvc_arm64_disables_only_dequantizer_inlining(self):
        self.check_policy(True, 'MSVC', 'ARM64', True)

    def test_other_architectures_compilers_and_platforms_are_unchanged(self):
        for windows, compiler, architecture in [
                (True, 'MSVC', 'x64'), (True, 'MSVC', 'ARM64EC'),
                (True, 'Clang', 'ARM64'), (True, 'GNU', 'ARM64'),
                (False, 'MSVC', 'ARM64'), (False, 'AppleClang', 'ARM64')]:
            with self.subTest(windows=windows, compiler=compiler, architecture=architecture):
                self.check_policy(windows, compiler, architecture, False)

    def test_port_applies_the_compiler_policy_patch(self):
        portfile = (self.port / 'portfile.cmake').read_text(encoding='utf-8')
        github = re.search(r'vcpkg_from_github\((.*?)\n\)', portfile, re.DOTALL).group(1)
        patches = github.split('PATCHES', 1)[1].split()
        self.assertEqual(patches.count(self.patch_name), 1)


@unittest.skipUnless(CMAKE, 'CMake is required')
class OnnxInstallPolicy(unittest.TestCase):
    def check_install(self, *, windows=True, mingw=False, architecture='arm64',
                      configuration='release', linkage='static', concurrency=None,
                      serial=False, expected_concurrency=None):
        port = ROOT / 'cmake/vcpkg-overlay-ports/onnxruntime/portfile.cmake'
        contents = port.read_text(encoding='utf-8')
        start = contents.rindex('if(VCPKG_BUILD_TYPE STREQUAL "release" AND '
                                'VCPKG_LIBRARY_LINKAGE STREQUAL "static")')
        end = contents.index('vcpkg_cmake_config_fixup(', start)
        policy = contents[start:end]
        initial_concurrency = ('unset(VCPKG_CONCURRENCY)' if concurrency is None else
                               f'set(VCPKG_CONCURRENCY {concurrency})')
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / 'install-policy.cmake'
            script.write_text(f'''
cmake_minimum_required(VERSION 3.30)
set(VCPKG_TARGET_IS_WINDOWS {str(windows).upper()})
set(VCPKG_TARGET_IS_MINGW {str(mingw).upper()})
set(VCPKG_TARGET_ARCHITECTURE "{architecture}")
set(VCPKG_BUILD_TYPE "{configuration}")
set(VCPKG_LIBRARY_LINKAGE "{linkage}")
{initial_concurrency}
set(install_calls 0)
function(vcpkg_cmake_install)
    math(EXPR calls "${{install_calls}} + 1")
    set(install_calls "${{calls}}" PARENT_SCOPE)
    set(install_arguments "${{ARGV}}" PARENT_SCOPE)
endfunction()
{policy}
if(NOT install_calls EQUAL 1)
    message(FATAL_ERROR "The port must install exactly once")
endif()
message(STATUS "INSTALL_ARGUMENTS=${{install_arguments}}")
if(DEFINED VCPKG_CONCURRENCY)
    message(STATUS "CONCURRENCY=${{VCPKG_CONCURRENCY}}")
else()
    message(STATUS "CONCURRENCY=undefined")
endif()
''', encoding='utf-8')
            result = subprocess.run([CMAKE, '-P', str(script)], capture_output=True,
                                    text=True, timeout=30)
            output = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, output)
            arguments = re.search(r'INSTALL_ARGUMENTS=([^\n]*)', output).group(1)
            self.assertEqual(arguments, 'DISABLE_PARALLEL' if serial else '', output)
            actual_concurrency = re.search(r'CONCURRENCY=([^\n]*)', output).group(1)
            self.assertEqual(actual_concurrency, 'undefined' if expected_concurrency is None
                             else str(expected_concurrency), output)

    def test_native_arm64_release_install_caps_parallelism_and_preserves_lower_limits(self):
        for concurrency in (None, 1, 2, 3, 4, 8, 32):
            with self.subTest(concurrency=concurrency):
                self.check_install(concurrency=concurrency,
                                   expected_concurrency=min(concurrency, 4)
                                   if concurrency is not None else 4)

    def test_other_release_static_targets_keep_serial_install_and_caller_limit(self):
        for selection in [{'architecture': 'x64'}, {'architecture': 'ARM64EC'},
                          {'mingw': True}, {'windows': False}]:
            for concurrency in (None, 2, 12):
                with self.subTest(selection=selection, concurrency=concurrency):
                    self.check_install(**selection, concurrency=concurrency, serial=True,
                                       expected_concurrency=concurrency)

    def test_debug_dynamic_and_multiconfiguration_installs_keep_existing_policy(self):
        for selection in [{'configuration': 'debug'}, {'linkage': 'dynamic'},
                          {'configuration': ''}]:
            for concurrency in (None, 2, 12):
                with self.subTest(selection=selection, concurrency=concurrency):
                    self.check_install(**selection, concurrency=concurrency,
                                       expected_concurrency=concurrency)


if __name__ == '__main__':
    unittest.main()
