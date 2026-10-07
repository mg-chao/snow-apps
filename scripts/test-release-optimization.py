#!/usr/bin/env python3
"""Configure-only regression fixtures for selective release optimization."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
WARNINGS = REPOSITORY / "cmake/ProjectWarnings.cmake"
POLICY = REPOSITORY / "cmake/SnowShotReleaseOptimization.cmake"


class ReleaseOptimizationTests(unittest.TestCase):
    def configure(self, compiler="MSVC", optimization=True, size=True, fixture="", succeeds=True,
                  apple=False, architecture="x64", initial_ipo=False):
        with tempfile.TemporaryDirectory(prefix="snow-release-policy-") as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.30)\n'
                'project(ReleasePolicy LANGUAGES NONE)\n'
                'set(CMAKE_CONFIGURATION_TYPES "Debug;Release")\n'
                # No compiler/dependencies are needed: only evaluate target/source properties.
                'set(CMAKE_CXX_CREATE_STATIC_LIBRARY "echo")\n'
                f'set(MSVC {"TRUE" if compiler == "MSVC" else "FALSE"})\n'
                f'set(CMAKE_CXX_COMPILER_ID "{compiler}")\n'
                f'set(APPLE {"TRUE" if apple else "FALSE"})\n'
                f'set(SNOW_WINDOWS_ARCHITECTURE "{architecture}")\n'
                f'set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE {"ON" if initial_ipo else "OFF"})\n'
                'set(SNOW_APPS_BUILD_SNOW_SHOT TRUE)\n'
                f'set(SNOW_APPS_ENABLE_RELEASE_OPTIMIZATION {"ON" if optimization else "OFF"})\n'
                f'set(SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION {"ON" if size else "OFF"})\n'
                f'include("{WARNINGS.as_posix()}")\n'
                'file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/cold.txt" "")\n'
                'file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/hot.txt" "")\n'
                'add_library(probe STATIC cold.txt hot.txt)\n'
                'set_target_properties(probe PROPERTIES LINKER_LANGUAGE CXX UNITY_BUILD ON)\n'
                'set_property(SOURCE cold.txt PROPERTY COMPILE_OPTIONS "existing-source-option")\n'
                + fixture + '\n'
                'get_target_property(ipo probe INTERPROCEDURAL_OPTIMIZATION_RELEASE)\n'
                'get_target_property(policy probe SNOW_RELEASE_OPTIMIZATION_POLICY)\n'
                'get_source_file_property(cold cold.txt COMPILE_OPTIONS)\n'
                'get_source_file_property(hot hot.txt COMPILE_OPTIONS)\n'
                'get_source_file_property(cold_pch cold.txt SKIP_PRECOMPILE_HEADERS)\n'
                'get_source_file_property(hot_pch hot.txt SKIP_PRECOMPILE_HEADERS)\n'
                'file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/options-$<CONFIG>.txt"\n'
                '  CONTENT "$<TARGET_PROPERTY:probe,COMPILE_OPTIONS>\\n'
                '$<TARGET_PROPERTY:probe,LINK_OPTIONS>\\n'
                '${ipo}\\n${policy}\\n$<GENEX_EVAL:${cold}>\\n${hot}\\n'
                '${cold_pch}\\n${hot_pch}\\n")\n'
                # IPO was inspected above. The fixture has no real compiler with IPO support.
                'get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)\n'
                'foreach(target IN LISTS targets)\n'
                '  set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)\n'
                'endforeach()\n', encoding="utf-8")
            result = subprocess.run(
                ["cmake", "-G", "Ninja Multi-Config", "-S", str(root), "-B", str(root / "build")],
                capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
            if not succeeds:
                return result.stdout + result.stderr
            return {
                configuration: (root / "build" / f"options-{configuration}.txt")
                .read_text(encoding="utf-8").splitlines()
                for configuration in ("Debug", "Release")
            }

    def test_msvc_policies_preserve_ipo_data_stripping_and_debug(self):
        for policy, wanted, rejected in (("speed", "/O2", "/O1"), ("size", "/O1", "/O2")):
            with self.subTest(policy=policy):
                options = self.configure(fixture=f"snow_apply_release_options(probe POLICY {policy})")
                compile_options, link_options, ipo, named_policy = options["Release"][:4]
                self.assertIn(wanted, compile_options.split(";"))
                self.assertNotIn(rejected, compile_options.split(";"))
                self.assertEqual(named_policy, policy)
                self.assertEqual(ipo, "TRUE")
                for flag in ("/Oi", "/GL", "/Gw", "/Gy", "/Z7"):
                    self.assertIn(flag, compile_options.split(";"))
                for flag in ("/LTCG", "/OPT:REF", "/OPT:ICF", "/DEBUG:FULL"):
                    self.assertIn(flag, link_options.split(";"))
                self.assertEqual(options["Debug"][:2], ["", ""])

    def test_clang_and_gnu_size_policies(self):
        for compiler in ("Clang", "GNU"):
            with self.subTest(compiler=compiler):
                options = self.configure(compiler=compiler,
                                         fixture="snow_apply_release_options(probe POLICY size)")
                self.assertEqual(options["Release"][0].split(";"),
                                 ["-Os", "-flto", "-ffunction-sections", "-fdata-sections"])
                self.assertEqual(options["Release"][1], "-flto")
                self.assertEqual(options["Debug"][:2], ["", ""])

    def test_msvc_arm64_uses_protected_optimized_native_objects(self):
        for policy in ("speed", "size"):
            for optimization in (True, False):
                with self.subTest(policy=policy, optimization=optimization):
                    options = self.configure(
                        architecture="arm64", initial_ipo=True, optimization=optimization,
                        fixture=f'''
snow_apply_strict_warnings(probe)
snow_apply_release_options(probe POLICY {policy})
snow_apply_release_size_sources(probe SOURCES cold.txt)
''')
                    compile_options, link_options, ipo = options["Release"][:3]
                    flags = compile_options.split(";")
                    self.assertEqual(ipo, "FALSE")
                    self.assertIn("/GL-", flags)
                    self.assertNotIn("/GL", flags)
                    self.assertNotIn("/LTCG", link_options.split(";"))
                    self.assertIn("/sdl", flags)
                    self.assertNotIn("/GS-", flags)
                    self.assertIn("/Z7", flags)
                    self.assertIn("/DEBUG:FULL", link_options.split(";"))
                    self.assertNotIn("/GL-", options["Debug"][0].split(";"))
                    if optimization:
                        self.assertIn("/O1" if policy == "size" else "/O2", flags)
                        for flag in ("/Gw", "/Gy"):
                            self.assertIn(flag, flags)
                        for flag in ("/OPT:REF", "/OPT:ICF"):
                            self.assertIn(flag, link_options.split(";"))
                        self.assertIn("/O1", options["Release"][4].split(";"))
                    else:
                        self.assertEqual(options["Release"][4], "existing-source-option")

        # clang-cl/LLVM does not use the affected MSVC ARM64 backend.
        options = self.configure(compiler="Clang", architecture="arm64",
                                 fixture="snow_apply_release_options(probe POLICY speed)")
        self.assertEqual(options["Release"][2], "TRUE")
        self.assertIn("-flto", options["Release"][0].split(";"))

    def test_workspace_arm64_policy_covers_targets_without_release_helper(self):
        workspace = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        start = workspace.index("snow_msvc_arm64_requires_native_codegen(_snow_arm64_native_codegen)")
        end = workspace.index("# Resolve the Snow Image codec profile", start)
        global_policy = workspace[start:end]
        self.configure(architecture="arm64", initial_ipo=True, fixture=global_policy + '''
add_library(inherited STATIC hot.txt)
set_target_properties(inherited PROPERTIES LINKER_LANGUAGE CXX)
get_target_property(inherited_ipo inherited INTERPROCEDURAL_OPTIMIZATION_RELEASE)
if(inherited_ipo OR CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL)
    message(FATAL_ERROR "ARM64 targets must inherit native code generation")
endif()
get_directory_property(options COMPILE_OPTIONS)
if(NOT "$<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:/GL->" IN_LIST options)
    message(FATAL_ERROR "ARM64 sources must override inherited /GL flags")
endif()
snow_apply_release_options(probe)
''')

    def test_standalone_arm64_policy_uses_target_compiler_metadata(self):
        for variable, value in [('CMAKE_CXX_COMPILER_ARCHITECTURE_ID', 'ARM64'),
                                ('CMAKE_CXX_COMPILER_ARCHITECTURE_ID', 'aarch64'),
                                ('MSVC_CXX_ARCHITECTURE_ID', 'ARM64'),
                                ('CMAKE_GENERATOR_PLATFORM', 'ARM64')]:
            with self.subTest(variable=variable, value=value):
                options = self.configure(architecture='', initial_ipo=True, fixture=f'''
unset(SNOW_WINDOWS_ARCHITECTURE)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set({variable} {value})
snow_apply_strict_warnings(probe)
snow_apply_release_options(probe)
''')
                self.assertEqual(options['Release'][2], 'FALSE')
                flags = options['Release'][0].split(';')
                self.assertIn('/GL-', flags)
                self.assertNotIn('/GL', flags)
                self.assertNotIn('/LTCG', options['Release'][1].split(';'))
                for flag in ('/O2', '/sdl', '/Gw', '/Gy'):
                    self.assertIn(flag, flags)

    def test_standalone_metadata_keeps_other_backends_and_host_selection_unchanged(self):
        for compiler, metadata, cache_arch in [('MSVC', 'x64', 'arm64'),
                                              ('MSVC', 'ARM64EC', ''),
                                              ('MSVC', '', ''),
                                              ('Clang', 'ARM64', '')]:
            with self.subTest(compiler=compiler, metadata=metadata, cache_arch=cache_arch):
                options = self.configure(compiler=compiler, architecture=cache_arch, fixture=f'''
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "{metadata}")
snow_apply_release_options(probe)
''')
                self.assertEqual(options['Release'][2], 'TRUE')
                self.assertNotIn('/GL-', options['Release'][0].split(';'))
                if compiler == 'MSVC':
                    self.assertIn('/GL', options['Release'][0].split(';'))
                    self.assertIn('/LTCG', options['Release'][1].split(';'))
                else:
                    self.assertIn('-flto', options['Release'][0].split(';'))

    def test_source_policy_preserves_existing_options_and_hot_sources(self):
        options = self.configure(fixture='''
snow_apply_release_options(probe)
snow_apply_release_size_sources(probe SOURCES cold.txt)
add_library(probe_mini STATIC cold.txt hot.txt)
set_target_properties(probe_mini PROPERTIES LINKER_LANGUAGE CXX)
get_source_file_property(mini_pch cold.txt TARGET_DIRECTORY probe_mini SKIP_PRECOMPILE_HEADERS)
if(NOT mini_pch)
    message(FATAL_ERROR "Mini must inherit the cold source's PCH exclusion")
endif()
''')
        self.assertEqual(options["Release"][4].split(";"),
                         ["existing-source-option", "/O1", "/Os", "/Oi"])
        self.assertEqual([option for option in options["Debug"][4].split(";") if option],
                         ["existing-source-option"])
        self.assertEqual(options["Release"][5], "NOTFOUND")
        self.assertEqual(options["Release"][3], "speed")
        self.assertEqual(options["Release"][6:], ["ON", "NOTFOUND"])

    def test_shipping_macos_dead_stripping_is_release_only(self):
        # The mixed app target keeps the speed policy while its final link strips
        # unreferenced sections in a shipping size build.
        fixture = "snow_apply_release_options(probe POLICY speed)"
        options = self.configure(compiler="AppleClang", apple=True, fixture=fixture)
        self.assertEqual(options["Release"][1].split(";"), ["-flto", "LINKER:-dead_strip"])
        self.assertEqual(options["Debug"][:2], ["", ""])
        for optimization, size in ((True, False), (False, True)):
            with self.subTest(optimization=optimization, size=size):
                options = self.configure(compiler="AppleClang", apple=True,
                                         optimization=optimization, size=size, fixture=fixture)
                self.assertNotIn("LINKER:-dead_strip", options["Release"][1].split(";"))

    def test_fast_build_disables_optimization_and_size_build_can_be_opted_out(self):
        fixture = '''
snow_apply_release_options(probe)
snow_apply_release_size_sources(probe SOURCES cold.txt)
'''
        options = self.configure(optimization=False, fixture=fixture)
        self.assertEqual(options["Release"][0], "/Z7")
        self.assertEqual(options["Release"][1], "/DEBUG:FULL")
        self.assertNotEqual(options["Release"][2], "TRUE")
        self.assertEqual(options["Release"][4], "existing-source-option")
        self.assertEqual(options["Release"][6:], ["NOTFOUND", "NOTFOUND"])
        options = self.configure(size=False, fixture=fixture)
        self.assertIn("/O2", options["Release"][0])
        self.assertEqual(options["Release"][4], "existing-source-option")
        self.assertEqual(options["Debug"][6:], ["NOTFOUND", "NOTFOUND"])

    def test_bad_policies_and_absent_sources_fail_configuration(self):
        for fixture, expected in (
            ("snow_apply_release_options(probe POLICY tiny)", "Unknown release optimization policy"),
            ("snow_apply_release_options(probe POLICY)", "requires POLICY speed or size"),
            ("snow_apply_release_size_sources(probe SOURCES absent.txt)",
             "Size-optimized source is absent"),
        ):
            with self.subTest(fixture=fixture):
                self.assertIn(expected, self.configure(fixture=fixture, succeeds=False))

    def test_production_policy_preserves_hot_libraries_and_mixed_sources(self):
        self.configure(fixture=f'''
# Read the policy's named manifests without applying it yet.
set(SNOW_SHOT_ENABLE_AGGRESSIVE_RELEASE_OPTIMIZATION OFF)
set(SNOW_SHOT_RELEASE_STATIC OFF)
include("{POLICY.as_posix()}")
foreach(target IN LISTS SNOW_SHOT_RELEASE_SPEED_TARGETS SNOW_SHOT_RELEASE_SIZE_TARGETS)
    add_library(${{target}} STATIC hot.txt)
    set_target_properties(${{target}} PROPERTIES LINKER_LANGUAGE CXX)
endforeach()
target_sources(snow_shot_storage PRIVATE ${{SNOW_SHOT_RELEASE_SIZE_STORAGE_SOURCES}}
    src/storage/preparedpngimage.cpp src/storage/capturehistoryrepository.cpp)
target_sources(snow_shot_settings PRIVATE ${{SNOW_SHOT_RELEASE_SIZE_SETTINGS_SOURCES}}
    src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
    src/presentation/components/thumbnailcache.cpp
    src/presentation/services/globalshortcutmanager.cpp)
target_sources(snow_shot PRIVATE ${{SNOW_SHOT_RELEASE_SIZE_SHELL_SOURCES}}
    src/presentation/capture/screenshotcaptureworker.cpp
    src/presentation/overlay/screenshotcanvasrenderer.cpp
    src/presentation/recording/recordingrenderjob.cpp)
get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS targets)
    get_target_property(sources ${{target}} SOURCES)
    foreach(source IN LISTS sources)
        file(WRITE "${{CMAKE_CURRENT_SOURCE_DIR}}/${{source}}" "")
        set_source_files_properties("${{source}}" PROPERTIES HEADER_FILE_ONLY TRUE)
    endforeach()
endforeach()
set(SNOW_SHOT_ENABLE_AGGRESSIVE_RELEASE_OPTIMIZATION ON)
include("{POLICY.as_posix()}")
foreach(target IN LISTS SNOW_SHOT_RELEASE_SPEED_TARGETS)
    get_target_property(named_policy ${{target}} SNOW_RELEASE_OPTIMIZATION_POLICY)
    if(NOT named_policy STREQUAL "speed")
        message(FATAL_ERROR "Hot library unexpectedly uses ${{named_policy}}: ${{target}}")
    endif()
endforeach()
foreach(target IN LISTS SNOW_SHOT_RELEASE_SIZE_TARGETS)
    get_target_property(named_policy ${{target}} SNOW_RELEASE_OPTIMIZATION_POLICY)
    if(NOT named_policy STREQUAL "size")
        message(FATAL_ERROR "Cold library unexpectedly uses ${{named_policy}}: ${{target}}")
    endif()
endforeach()
foreach(source IN LISTS SNOW_SHOT_RELEASE_SIZE_STORAGE_SOURCES
        SNOW_SHOT_RELEASE_SIZE_SETTINGS_SOURCES SNOW_SHOT_RELEASE_SIZE_SHELL_SOURCES)
    get_source_file_property(options "${{source}}" COMPILE_OPTIONS)
    if(NOT "$<$<CONFIG:Release>:/O1>" IN_LIST options)
        message(FATAL_ERROR "Cold source is missing its size option: ${{source}}")
    endif()
    get_source_file_property(skip_pch "${{source}}" SKIP_PRECOMPILE_HEADERS)
    if(NOT skip_pch)
        message(FATAL_ERROR "Cold source still reuses the speed PCH: ${{source}}")
    endif()
endforeach()
foreach(source IN ITEMS src/storage/preparedpngimage.cpp
        src/storage/capturehistoryrepository.cpp
        src/presentation/services/screenshotclipboarddibdecoder_avx2.cpp
        src/presentation/components/thumbnailcache.cpp
        src/presentation/services/globalshortcutmanager.cpp
        src/presentation/capture/screenshotcaptureworker.cpp
        src/presentation/overlay/screenshotcanvasrenderer.cpp
        src/presentation/recording/recordingrenderjob.cpp)
    get_source_file_property(options "${{source}}" COMPILE_OPTIONS)
    if(options)
        message(FATAL_ERROR "Hot source unexpectedly has size options: ${{source}}")
    endif()
    get_source_file_property(skip_pch "${{source}}" SKIP_PRECOMPILE_HEADERS)
    if(skip_pch)
        message(FATAL_ERROR "Hot source unexpectedly skips the PCH: ${{source}}")
    endif()
endforeach()
''')

    def test_shipping_presets_enable_size_and_other_presets_preserve_speed(self):
        presets = json.loads((REPOSITORY / "CMakePresets.json").read_text(encoding="utf-8"))
        by_name = {preset["name"]: preset for preset in presets["configurePresets"]}

        def variables(name):
            preset = by_name[name]
            inherited = preset.get("inherits", [])
            if isinstance(inherited, str):
                inherited = [inherited]
            result = {}
            for parent in reversed(inherited):
                result.update(variables(parent))
            result.update(preset.get("cacheVariables", {}))
            return result

        for name, preset in by_name.items():
            if preset.get("hidden"):
                continue
            with self.subTest(preset=name):
                value = variables(name)["SNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION"]
                self.assertEqual(value, "ON" if name.endswith("-release") else "OFF")


if __name__ == "__main__":
    unittest.main()
