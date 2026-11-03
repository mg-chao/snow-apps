"""Focused contracts for Windows target selection and cross-build dependencies."""

import hashlib
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
GIT = shutil.which('git')


@unittest.skipUnless(GIT, 'Git is required')
class VcpkgCacheLineEndings(unittest.TestCase):
    def test_toolchain_hashes_survive_windows_checkout(self):
        # These shared ABI inputs were first built with LF. A Windows checkout
        # must preserve their bytes even when core.autocrlf is enabled.
        inputs = ('cmake/SnowBuildToolchain.cmake', 'scripts/build-toolchain.json')
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            (fixture / '.gitattributes').write_bytes((ROOT / '.gitattributes').read_bytes())
            expected = {}
            for name in inputs:
                path = fixture / name
                path.parent.mkdir(parents=True, exist_ok=True)
                contents = (ROOT / name).read_bytes().replace(b'\r\n', b'\n')
                path.write_bytes(contents)
                expected[name] = hashlib.sha256(contents).hexdigest()

            def git(*arguments):
                result = subprocess.run(
                    [GIT, '-c', 'core.autocrlf=true', '-c', 'core.eol=crlf',
                     '-c', 'core.safecrlf=false', *arguments], cwd=fixture,
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            git('init', '--quiet')
            git('add', '--', '.gitattributes', *inputs)
            for name in inputs:
                (fixture / name).unlink()
            git('checkout-index', '--all', '--force')
            for name in inputs:
                with self.subTest(path=name):
                    actual = hashlib.sha256((fixture / name).read_bytes()).hexdigest()
                    self.assertEqual(actual, expected[name],
                                     'Checkout changed a shared vcpkg ABI input.')


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

    def test_compiler_minimum_accepts_newer_versions(self):
        for version, supported in [('19.50.99999', False), ('19.51.0', True),
                                   ('19.52.0', True), ('19.100.0', True), ('20.0.0', True)]:
            with self.subTest(version=version):
                output = self.run_cmake(f'''
include("{ROOT.as_posix()}/cmake/SnowBuildToolchain.cmake")
set(CMAKE_CXX_COMPILER_ID MSVC)
set(CMAKE_CXX_COMPILER_VERSION {version})
snow_require_msvc_version()
''', supported)
                if not supported:
                    self.assertIn('or newer is required', output)

    def test_msvc_selection_uses_minimum_and_numeric_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            versions = ('14.50.99999', '14.51.0', '14.99.0', '14.100.0', 'invalid')
            for version in versions:
                (root / 'VC/Tools/MSVC' / version).mkdir(parents=True)
            fixture = f'''
include("{ROOT.as_posix()}/cmake/SnowBuildToolchain.cmake")
set(ENV{{VCToolsInstallDir}} "")
set(ENV{{VSINSTALLDIR}} "{root.as_posix()}")
'''
            output = self.run_cmake(fixture + '''
snow_resolve_msvc_tools_root(selected)
message(STATUS "SELECTED=${selected}")
''')
            self.assertIn('/VC/Tools/MSVC/14.100.0', output)
            output = self.run_cmake(fixture + f'''
include("{ROOT.as_posix()}/cmake/vcpkg-windows-triplet-environment.cmake")
message(STATUS "SELECTED=${{VCPKG_PLATFORM_TOOLSET_VERSION}}")
''')
            self.assertIn('SELECTED=14.100.0', output)
            for version, supported in [('14.50.99999', False), ('14.51.0', True),
                                       ('14.100.0', True)]:
                with self.subTest(version=version):
                    self.run_cmake(fixture + f'''
set(ENV{{VCToolsInstallDir}} "{root.as_posix()}/VC/Tools/MSVC/{version}/")
snow_resolve_msvc_tools_root(selected)
''', supported)

    def test_rust_minimum_accepts_newer_versions(self):
        for version, supported in [('1.97.0', False), ('1.97.1', True), ('1.97.2', True),
                                   ('1.100.0', True), ('2.0.0', True), ('invalid', False)]:
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                rustc = Path(directory) / ('rustc.cmd' if os.name == 'nt' else 'rustc')
                rustc.write_text(f'@echo off\necho rustc {version}\n' if os.name == 'nt'
                                 else f'#!/bin/sh\necho "rustc {version}"\n', encoding='utf-8')
                rustc.chmod(0o755)
                self.run_cmake(f'''
include("{ROOT.as_posix()}/cmake/SnowBuildToolchain.cmake")
set(RUSTC_EXECUTABLE "{rustc.as_posix()}")
snow_require_rust_version()
''', supported)

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
        output = self.triplet_fixture(f'''
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
        output = self.triplet_fixture(f'''
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

    def triplet_fixture(self, contents):
        with tempfile.TemporaryDirectory() as directory:
            msvc = Path(directory) / 'MSVC/14.100.0'
            msvc.mkdir(parents=True)
            return self.run_cmake(f'set(ENV{{VCToolsInstallDir}} "{msvc.as_posix()}")\n' + contents)

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
        contents = (f'@echo off\necho rustc 1.98.0 (fixture)\necho host: {host}-pc-windows-msvc\n'
                    if os.name == 'nt' else
                    f'#!/bin/sh\necho "rustc 1.98.0 (fixture)"\necho "host: {host}-pc-windows-msvc"\n')
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
set(RUSTC_EXECUTABLE "{rustc.as_posix()}")
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
set(APPLE FALSE)
set(MSVC TRUE)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_CXX_COMPILER "C:/VS/bin/Hostx64/arm64/cl.exe")
set(CMAKE_LINKER "C:/VS/bin/Hostx64/arm64/link.exe")
set(CMAKE_AR "C:/VS/bin/Hostx64/arm64/lib.exe")
set(CARGO_EXECUTABLE "{cargo.as_posix()}")
set(SNOW_LIBCLANG_BIN_DIR "{(root / 'x64').as_posix()}")
include("{ROOT.as_posix()}/cmake/RustStaticLibrary.cmake")
snow_add_rust_static_library(fixture PACKAGE fixture MANIFEST_DIR "{root.as_posix()}"
    NO_DEFAULT_FEATURES FEATURES fixture-feature)
snow_add_rust_executable(fixture_executable PACKAGE fixture MANIFEST_DIR "{root.as_posix()}"
    TOOLCHAIN nightly-2026-09-08)
get_target_property(toolchain fixture_executable SNOW_RUST_TOOLCHAIN)
if(NOT toolchain STREQUAL "nightly-2026-09-08")
    message(FATAL_ERROR "Executable lost its toolchain contract for license collection")
endif()
get_target_property(no_defaults fixture SNOW_RUST_NO_DEFAULT_FEATURES)
if(NOT no_defaults)
    message(FATAL_ERROR "Static library lost its Cargo feature contract for license collection")
endif()
''', encoding='utf-8')
            build = root / 'build'
            result = subprocess.run([CMAKE, '-G', 'Ninja', '-S', str(root), '-B', str(build)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = (build / 'build.ninja').read_text(encoding='utf-8')
            self.assertEqual(commands.count(f'LIBCLANG_PATH={(root / "x64").as_posix()}'), 2)
            self.assertIn('--target aarch64-pc-windows-msvc', commands)
            self.assertEqual(commands.count('--no-default-features'), 1)
            self.assertIn('--features fixture-feature', commands)
            self.assertEqual(commands.count('RUSTUP_TOOLCHAIN=nightly-2026-09-08'), 1)

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
message(STATUS "ENCODERS=${{SNOW_SHOT_ENCODERS}} OPTIONS=${{OPTIONS}}")
''')
            self.assertIn('h264_mf', output)
            self.assertIn('hevc_mf', output)
            self.assertIn('--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc', output)
            self.assertIn('libx264,libx265', output)
            for encoder in ('h264_nvenc', 'h264_amf', 'h264_qsv',
                            'hevc_nvenc', 'hevc_amf', 'hevc_qsv'):
                self.assertEqual(encoder in output, arch == 'x64')

    def test_minimal_recording_profiles_enable_only_required_tempo_filters(self):
        port = ROOT / 'cmake/vcpkg-overlay-ports/ffmpeg'
        manifest = json.loads((port / 'vcpkg.json').read_text(encoding='utf-8'))
        text = (port / 'portfile.cmake').read_text(encoding='utf-8')
        start = text.index('if("snow-shot-minimal" IN_LIST FEATURES)',
                           text.index('if("rubberband" IN_LIST FEATURES)'))
        fragment = text[start:text.index('set(OPTIONS_CROSS', start)]
        for profile, platform in [('snow-shot-minimal', 'WINDOWS'),
                                  ('snow-macos-media', 'OSX'),
                                  ('snow-shot-macos-minimal', 'OSX')]:
            with self.subTest(profile=profile):
                dependencies = manifest['features'][profile]['dependencies']
                common = next(dependency['features'] for dependency in dependencies
                              if dependency['name'] == 'ffmpeg' and
                              'platform' not in dependency)
                self.assertIn('avfilter', common)
                output = self.run_cmake(f'''
set(VCPKG_TARGET_IS_{platform} TRUE)
set(FEATURES {profile})
{fragment}
message(STATUS "OPTIONS=${{OPTIONS}}")
''')
                filters = re.findall(r'--enable-filter=([^\s]+)', output)
                self.assertEqual(filters, ['abuffer,abuffersink,atempo'])

    def test_recording_profile_separates_application_outputs_from_compatibility(self):
        port = ROOT / 'cmake/vcpkg-overlay-ports/ffmpeg'
        manifest = json.loads((port / 'vcpkg.json').read_text(encoding='utf-8'))
        compatibility = manifest['features']['snow-recording-compat']['dependencies']
        self.assertTrue(any(dependency.get('features') == ['snow-shot-minimal']
                            for dependency in compatibility))
        text = (port / 'portfile.cmake').read_text(encoding='utf-8')
        start = text.index('if("snow-shot-minimal" IN_LIST FEATURES)',
                           text.index('if("rubberband" IN_LIST FEATURES)'))
        fragment = text[start:text.index('if("snow-macos-media" IN_LIST FEATURES)', start)]
        for compatibility in (False, True):
            with self.subTest(compatibility=compatibility):
                features = 'snow-shot-minimal'
                if compatibility:
                    features += ';snow-recording-compat'
                output = self.run_cmake(f'''
set(VCPKG_TARGET_IS_WINDOWS TRUE)
set(FEATURES {features})
{fragment}
message(STATUS "OPTIONS=${{OPTIONS}}")
''')
                enabled = {}
                for kind, names in re.findall(r'--enable-(encoder|decoder|muxer|demuxer|parser)=([^\s]+)', output):
                    enabled.setdefault(kind, set()).update(names.split(','))
                self.assertTrue({'mp4', 'gif', 'apng', 'webp'} <= enabled['muxer'])
                self.assertTrue({'libx264', 'libx265', 'apng', 'libwebp_anim', 'aac'}
                                <= enabled['encoder'])
                # Older .snowrec sources are still readable in the application profile.
                self.assertIn('matroska', enabled['demuxer'])
                self.assertIn('h264', enabled['decoder'])
                for kind, components in [('encoder', {'mpeg4', 'mp3_mf', 'gif'}),
                                         ('muxer', {'avi', 'matroska'}),
                                         ('decoder', {'mp3'}),
                                         ('parser', {'mpegaudio'})]:
                    self.assertEqual(components <= enabled[kind], compatibility)

    def test_windows_package_audit_retains_gif_inputs_and_muxing_without_encoder(self):
        text = (ROOT / 'scripts/package-snow-shot.ps1').read_text(encoding='utf-8')
        start = text.index('function Get-SnowShotExpectedFfmpegComponents {')
        fragment = text[start:text.index('function Assert-SnowShotStaticDependencies', start)]
        components = {
            kind: set(re.findall(r'"([A-Z0-9_]+)"', values))
            for kind, values in re.findall(r'^\s+([A-Z]+) = @\(([^)]*)\)', fragment, re.M)
        }
        self.assertNotIn('GIF', components['ENCODER'])
        for kind in ['DECODER', 'PARSER', 'DEMUXER', 'MUXER']:
            self.assertIn('GIF', components[kind])
        vendor = {'H264_AMF', 'H264_NVENC', 'H264_QSV',
                  'HEVC_AMF', 'HEVC_NVENC', 'HEVC_QSV'}
        self.assertEqual(components['ENCODER'],
                         vendor | {'AAC', 'APNG', 'H264_MF', 'HEVC_MF',
                                   'LIBWEBP_ANIM', 'LIBX264', 'LIBX265'})
        arm = fragment[fragment.index("if ($Architecture -eq 'arm64')"):]
        excluded = re.search(r'Where-Object \{ \$_ -notin @\(([^)]*)\)', arm)
        self.assertIsNotNone(excluded)
        self.assertEqual(set(re.findall(r"'([A-Z0-9_]+)'", excluded.group(1))), vendor)

    def test_macos_application_media_retains_inputs_without_standalone_outputs(self):
        port = ROOT / 'cmake/vcpkg-overlay-ports/ffmpeg'
        workspace = json.loads((ROOT / 'vcpkg.json').read_text(encoding='utf-8'))
        dependencies = workspace['features']['snow-shot']['dependencies']
        self.assertTrue(any(isinstance(dependency, dict) and
                            dependency.get('name') == 'ffmpeg' and
                            dependency.get('platform') == 'osx' and
                            dependency.get('features') == ['snow-shot-macos-minimal']
                            for dependency in dependencies))
        text = (port / 'portfile.cmake').read_text(encoding='utf-8')
        start = text.index('if("snow-macos-media" IN_LIST FEATURES)',
                           text.index('if("rubberband" IN_LIST FEATURES)'))
        fragment = text[start:text.index('set(OPTIONS_CROSS', start)]
        profiles = {}
        for profile in ['snow-macos-media', 'snow-shot-macos-minimal']:
            output = self.run_cmake(f'''
set(VCPKG_TARGET_IS_OSX TRUE)
set(FEATURES {profile})
{fragment}
message(STATUS "OPTIONS=${{OPTIONS}}")
''')
            enabled = {}
            for kind, names in re.findall(r'--enable-(encoder|decoder|muxer|demuxer|parser)=([^\s]+)', output):
                enabled.setdefault(kind, set()).update(names.split(','))
            profiles[profile] = enabled
        standalone, application = (profiles[name] for name in
                                   ['snow-macos-media', 'snow-shot-macos-minimal'])
        # Every pre-existing input remains available for recovery/import/editing.
        self.assertEqual(application['decoder'], standalone['decoder'])
        self.assertEqual(application['demuxer'], standalone['demuxer'])
        self.assertEqual(application['parser'], standalone['parser'])
        self.assertEqual(application['muxer'], {'mp4', 'gif', 'apng', 'webp'})
        self.assertEqual(standalone['muxer'] - application['muxer'],
                         {'matroska', 'avi', 'wav'})
        self.assertEqual(standalone['encoder'] - application['encoder'],
                         {'mpeg4', 'gif', 'pcm_s16le'})
        self.assertTrue({'libx264', 'libx265', 'h264_videotoolbox', 'hevc_videotoolbox',
                         'apng', 'libwebp_anim', 'aac'} <= application['encoder'])
        self.assertIn('gif', application['parser'])

    def test_crashpad_uses_runnable_host_compilers_and_selected_toolset(self):
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
                        os.environ, {'SNOW_MSVC_HOST_ARCHITECTURE': host,
                                     'VCToolsVersion': '14.100.99999'}, clear=True):
                    arguments = list(namespace['toolchain_arguments']('vcvarsall.bat'))
                    self.assertEqual([argument[1] for argument in arguments], expected)
                    self.assertTrue(all(argument[2:] == ['-vcvars_ver=14.100.99999', '&&', 'set']
                                        for argument in arguments))

    def test_toolchain_separates_host_tools_and_target_libraries(self):
        for host in ('x64', 'arm64'):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                msvc = root / 'MSVC/14.100.0'
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
