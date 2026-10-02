"""Native linker regression for Full's unified Rust archive and Mini isolation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]


class SelectedTextLinkContract(unittest.TestCase):
    def test_full_resolves_runtime_once_without_loading_mini_archive(self):
        self.run_link_fixture(mock_apple_native=False)

    def test_apple_native_dependencies_follow_each_selected_archive(self):
        self.run_link_fixture(mock_apple_native=True)

    def run_link_fixture(self, mock_apple_native):
        with tempfile.TemporaryDirectory(prefix='snow-selected-text-link-') as directory:
            fixture = Path(directory)
            runtime = ('int native_value();\nint runtime_value() { return native_value(); }\n'
                       if mock_apple_native else 'int runtime_value() { return 7; }\n')
            sources = {
                'bundle.cpp': runtime + 'int shared_entry() { return 11; }\n',
                'selected.cpp': 'int runtime_value();\nint extra_runtime_value();\n'
                                'int selected_entry() { return runtime_value() + extra_runtime_value(); }\n',
                'runtime.cpp': 'int runtime_value() { return 7; }\n'
                               'int extra_runtime_value() { return 0; }\n',
                'full.cpp': runtime +
                            'int extra_runtime_value() { return 0; }\n'
                            'int shared_entry() { return 11; }\n'
                            'int selected_entry() { return runtime_value() + extra_runtime_value(); }\n',
                'wrapper.cpp': 'int wrapper_entry() { return 0; }\n',
                'main.cpp': 'int shared_entry();\nint selected_entry();\n'
                            'int main() { return shared_entry() + selected_entry() == 18 ? 0 : 1; }\n',
                'mini.cpp': 'int shared_entry();\nint main() { return shared_entry() == 11 ? 0 : 1; }\n',
                'native.cpp': 'int native_value() { return 7; }\n',
            }
            for name, source in sources.items():
                (fixture / name).write_text(source, encoding='utf-8')
            native_setup = ''
            if mock_apple_native:
                # Execute the actual macOS archive dependency wiring with mock
                # discovery. This keeps the regression portable and checks the
                # generated native link closure without an Apple SDK.
                native_source = (ROOT / 'cmake/SnowShotMacOS.cmake').read_text()
                native_source, separator, _ = native_source.partition(
                    'set_target_properties(snow_shot PROPERTIES')
                self.assertTrue(separator, 'macOS native linkage must precede packaging')
                (fixture / 'apple-native.cmake').write_text(native_source, encoding='utf-8')
                framework_helper = 'SnowPkgConfigAppleFrameworks.cmake'
                (fixture / framework_helper).write_text(
                    (ROOT / 'cmake' / framework_helper).read_text(), encoding='utf-8')
                native_setup = '''
add_library(native_closure STATIC native.cpp)
add_library(objc INTERFACE)
function(find_package)
endfunction()
function(pkg_check_modules)
    add_library(PkgConfig::SNOW_SHOT_FFMPEG INTERFACE IMPORTED GLOBAL)
    set_property(TARGET PkgConfig::SNOW_SHOT_FFMPEG PROPERTY
        INTERFACE_LINK_LIBRARIES native_closure)
endfunction()
function(find_library output)
    set(${output} native_closure PARENT_SCOPE)
endfunction()
include("${CMAKE_CURRENT_SOURCE_DIR}/apple-native.cmake")
'''
            # Model the actual archive boundary: fat-LTO bundles runtime symbols
            # with required C ABI code; the separate archive also ships a runtime.
            # The extra function forces extraction of the original std object
            # even when the bundle's runtime symbols have already been loaded.
            (fixture / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 4.2)
project(SelectedTextLink LANGUAGES CXX)
include("{ROOT.as_posix()}/cmake/RustStaticLibrary.cmake")
add_library(snow_shot_rust_ffi_bundle STATIC bundle.cpp)
snow_add_rust_bundle_selector(snow_shot_rust_ffi_selector snow_shot_rust_ffi_bundle)
function(snow_add_rust_static_library target)
    if(target STREQUAL "snow_shot_full_rust_ffi_bundle")
        add_library(${{target}} STATIC EXCLUDE_FROM_ALL full.cpp)
    else()
        add_library(${{target}} STATIC EXCLUDE_FROM_ALL selected.cpp runtime.cpp)
    endif()
endfunction()
set(SNOW_RUST_CARGO_TARGET_DIR "${{CMAKE_BINARY_DIR}}/cargo")
set(SNOW_SHOT_CAPTURE_CRATES_DIR "{ROOT.as_posix()}/snow-crates")
include("{ROOT.as_posix()}/cmake/SnowSelectedText.cmake")
{native_setup}
add_library(translation STATIC wrapper.cpp)
target_link_libraries(translation PRIVATE snow_selected_text_c)
add_library(shared_consumer STATIC wrapper.cpp)
target_link_libraries(shared_consumer PRIVATE snow_shot_rust_ffi_selector)
add_executable(link_contract main.cpp)
target_link_libraries(link_contract PRIVATE translation shared_consumer)
add_executable(reverse_link_contract main.cpp)
target_link_libraries(reverse_link_contract PRIVATE shared_consumer translation)
add_executable(mini_contract mini.cpp)
target_link_libraries(mini_contract PRIVATE shared_consumer)
include("{ROOT.as_posix()}/cmake/SnowShotMiniBuildContract.cmake")
snow_shot_assert_mini_build_contract(mini_contract)
''', encoding='utf-8')
            build = fixture / 'build'
            for command in (['cmake', '-S', str(fixture), '-B', str(build)],
                            ['cmake', '--build', str(build), '--config', 'Release',
                             '--target', 'link_contract', 'reverse_link_contract', 'mini_contract']):
                result = subprocess.run(command, capture_output=True, text=True,
                                        encoding='utf-8', errors='replace')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for target, expected in (
                    ('link_contract', 'snow_shot_full_rust_ffi_bundle'),
                    ('reverse_link_contract', 'snow_shot_full_rust_ffi_bundle'),
                    ('mini_contract', 'snow_shot_rust_ffi_bundle')):
                project = build / (target + '.vcxproj')
                if project.exists():
                    root = ET.parse(project).getroot()
                    namespace = {'msbuild': 'http://schemas.microsoft.com/developer/msbuild/2003'}
                    links = [node.text or '' for node in root.findall(
                        './/msbuild:Link/msbuild:AdditionalDependencies', namespace)]
                    suffix = '.lib'
                else:
                    links = [(build / 'CMakeFiles' / (target + '.dir') / 'link.txt').read_text()]
                    suffix = '.a'
                self.assertTrue(links, target + ' must expose its generated link dependencies')
                for link in links:
                    archives = {name for name in (
                        'snow_shot_rust_ffi_bundle', 'snow_shot_full_rust_ffi_bundle')
                        if name + suffix in link}
                    self.assertEqual(archives, {expected}, target + ': ' + link)
                    if mock_apple_native:
                        self.assertIn('native_closure' + suffix, link, target + ': ' + link)
            executable = build / ('Release/link_contract.exe' if sys.platform == 'win32'
                                  else 'link_contract')
            self.assertEqual(subprocess.run([str(executable)]).returncode, 0)
            reverse = build / ('Release/reverse_link_contract.exe' if sys.platform == 'win32'
                               else 'reverse_link_contract')
            self.assertEqual(subprocess.run([str(reverse)]).returncode, 0)
            mini = build / ('Release/mini_contract.exe' if sys.platform == 'win32'
                            else 'mini_contract')
            self.assertEqual(subprocess.run([str(mini)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
