"""Configure-only regression tests for Mini's transitive build exclusions."""

from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[2]
CONTRACT = REPOSITORY / "cmake/SnowShotMiniBuildContract.cmake"


class MiniBuildContractTests(unittest.TestCase):
    def test_shared_rust_archive_does_not_compile_full_only_selected_text_engines(self):
        compiler = subprocess.run(["rustc", "-vV"], capture_output=True, text=True)
        self.assertEqual(compiler.returncode, 0, compiler.stderr)
        host = next(line.removeprefix("host: ") for line in compiler.stdout.splitlines()
                    if line.startswith("host: "))
        result = subprocess.run(
            ["cargo", "metadata", "--locked", "--offline", "--format-version", "1",
             "--filter-platform", host,
             "--manifest-path", str(REPOSITORY / "snow_rust_ffi/Cargo.toml")],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata = json.loads(result.stdout)
        nodes = {node["id"]: node for node in metadata["resolve"]["nodes"]}
        packages = {package["id"]: package["name"] for package in metadata["packages"]}
        pending = [metadata["resolve"]["root"]]
        visited = set()
        while pending:
            package = pending.pop()
            if package in visited:
                continue
            visited.add(package)
            self.assertNotIn(packages[package], ("snow-selected-text", "snow-selected-text-c"))
            pending.extend(dependency["pkg"] for dependency in nodes[package]["deps"])

    def configure(self, fixture, succeeds=True):
        with tempfile.TemporaryDirectory(prefix="snow-mini-contract-") as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 4.2)\n'
                'project(MiniContract LANGUAGES NONE)\n'
                f'include("{CONTRACT.as_posix()}")\n'
                'add_library(mini INTERFACE)\n'
                + fixture + '\nsnow_shot_assert_mini_build_contract(mini)\n',
                encoding="utf-8")
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
            return result.stdout + result.stderr

    def test_retained_dependencies_and_mini_sources_are_allowed(self):
        self.configure('''
add_library(opencv_core INTERFACE)
add_library(opencv_imgproc INTERFACE)
add_library(snow_shot_storage_mini INTERFACE)
target_link_libraries(mini INTERFACE opencv_imgproc snow_shot_storage_mini)
target_link_libraries(opencv_imgproc INTERFACE opencv_core)
target_sources(snow_shot_storage_mini INTERFACE screenshotocrcontroller.h)
''')

    def test_disabled_packages_cannot_enter_transitively_or_conditionally(self):
        targets = ["snow_shot_translation", "snow_selected_text_c", "snow_shot_full_rust_ffi_bundle",
                   "opencv_wechat_qrcode", "opencv_objdetect", "opencv_dnn",
                   "snow_shot_latex_renderer", "snow_shot_microtex",
                   "snow_shot_storage", "snow_shot_edition_full"]
        if sys.platform == "win32":
            targets.extend(("snow_ocr_process", "onnxruntime::onnxruntime"))
        for target in targets:
            with self.subTest(target=target):
                output = self.configure(f'''
add_library(shared INTERFACE)
add_library({target} INTERFACE IMPORTED GLOBAL)
target_link_libraries(mini INTERFACE shared)
target_link_libraries(shared INTERFACE "$<$<CONFIG:Release>:$<LINK_ONLY:{target}>>")
''', succeeds=False)
                self.assertIn(f"Full-only target: {target}", output)

    def test_full_only_sources_and_resources_are_rejected_in_shared_libraries(self):
        for source in ("screenshotqrcontroller.cpp", "screenshottabledocument.cpp",
                       "screenshotimageconversioncontroller.h", "texttranslationsettingswidget.cpp",
                       "translationservice.cpp", "qrc_snow_shot_translations.cpp",
                       "snow_shot_translations.qrc",
                       "screenshotlatexrenderer.cpp", "screenshotlatexrenderer.h",
                       "qrc_snow_shot_microtex.cpp", "snow_shot_microtex.qrc",
                       "snow_shot_en_US.qm"):
            with self.subTest(source=source):
                output = self.configure(f'''
add_library(shared INTERFACE)
target_link_libraries(mini INTERFACE shared)
target_sources(shared INTERFACE "path/{source}")
''', succeeds=False)
                self.assertIn("Full-only source:", output)
                self.assertIn(f"/path/{source}", output)

    def test_object_resources_and_aliases_cannot_bypass_the_graph_check(self):
        output = self.configure('''
add_library(snow_shot_translation INTERFACE)
add_library(translation_alias ALIAS snow_shot_translation)
target_link_libraries(mini INTERFACE translation_alias)
''', succeeds=False)
        self.assertIn("Full-only target: snow_shot_translation", output)
        output = self.configure('''
add_library(shared INTERFACE)
set_property(TARGET mini PROPERTY INTERFACE_SOURCES "$<TARGET_OBJECTS:shared>")
target_sources(shared INTERFACE path/screenshottableeditor.cpp)
''', succeeds=False)
        self.assertIn("Full-only source:", output)
        self.assertIn("/path/screenshottableeditor.cpp", output)

    def test_conditional_sources_are_excluded_in_every_configuration(self):
        output = self.configure('''
target_sources(mini INTERFACE "$<$<CONFIG:Release>:path/screenshotqrcontroller.cpp>")
''', succeeds=False)
        self.assertIn("Full-only source:", output)

    def test_only_mini_translation_catalogs_are_allowed(self):
        self.configure('''
target_sources(mini INTERFACE path/i18n/mini/snow_shot_en_US.qm)
''')

    def test_source_filter_removes_full_only_automoc_headers_and_implementations(self):
        self.configure('''
snow_shot_mini_filter_sources(sources
    path/screenshotqrcontroller.cpp path/screenshotqrcontroller.h
    path/texttranslationsettingswidget.cpp path/screenshotimageconversioncontroller.h
    path/screenshotlatexrenderer.cpp path/screenshotlatexrenderer.h
    path/qrc_snow_shot_microtex.cpp path/snow_shot_microtex.qrc
    path/qrc_snow_shot_translations.cpp path/snow_shot_translations.qrc
    path/snow_shot_en_US.qm path/snow_shot_zh_CN.qm path/snow_shot_zh_TW.qm
    path/screenshotocrcontroller.h)
if(NOT sources STREQUAL "path/screenshotocrcontroller.h")
    message(FATAL_ERROR "Unexpected Mini sources: ${sources}")
endif()
''')


if __name__ == "__main__":
    unittest.main()
