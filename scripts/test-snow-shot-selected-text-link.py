"""Native linker regression for Full's unified Rust archive and Mini isolation."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SelectedTextLinkContract(unittest.TestCase):
    def test_full_resolves_runtime_once_without_loading_mini_archive(self):
        with tempfile.TemporaryDirectory(prefix='snow-selected-text-link-') as directory:
            fixture = Path(directory)
            sources = {
                'bundle.cpp': 'int runtime_value() { return 7; }\nint shared_entry() { return 11; }\n',
                'selected.cpp': 'int runtime_value();\nint extra_runtime_value();\n'
                                'int selected_entry() { return runtime_value() + extra_runtime_value(); }\n',
                'runtime.cpp': 'int runtime_value() { return 7; }\n'
                               'int extra_runtime_value() { return 0; }\n',
                'full.cpp': 'int runtime_value() { return 7; }\n'
                            'int extra_runtime_value() { return 0; }\n'
                            'int shared_entry() { return 11; }\n'
                            'int selected_entry() { return runtime_value() + extra_runtime_value(); }\n',
                'wrapper.cpp': 'int wrapper_entry() { return 0; }\n',
                'main.cpp': 'int shared_entry();\nint selected_entry();\n'
                            'int main() { return shared_entry() + selected_entry() == 18 ? 0 : 1; }\n',
                'mini.cpp': 'int shared_entry();\nint main() { return shared_entry() == 11 ? 0 : 1; }\n',
            }
            for name, source in sources.items():
                (fixture / name).write_text(source, encoding='utf-8')
            # Model the actual archive boundary: fat-LTO bundles runtime symbols
            # with required C ABI code; the separate archive also ships a runtime.
            # The extra function forces extraction of the original std object
            # even when the bundle's runtime symbols have already been loaded.
            (fixture / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 4.2)
project(SelectedTextLink LANGUAGES CXX)
add_library(snow_shot_rust_ffi_bundle STATIC bundle.cpp)
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
add_library(translation STATIC wrapper.cpp)
target_link_libraries(translation PRIVATE snow_selected_text_c)
add_library(shared_consumer STATIC wrapper.cpp)
target_link_libraries(shared_consumer PRIVATE snow_shot_rust_ffi_bundle)
add_executable(link_contract main.cpp)
target_link_libraries(link_contract PRIVATE translation shared_consumer)
add_executable(mini_contract mini.cpp)
target_link_libraries(mini_contract PRIVATE shared_consumer)
include("{ROOT.as_posix()}/cmake/SnowShotMiniBuildContract.cmake")
snow_shot_assert_mini_build_contract(mini_contract)
''', encoding='utf-8')
            build = fixture / 'build'
            for command in (['cmake', '-S', str(fixture), '-B', str(build)],
                            ['cmake', '--build', str(build), '--config', 'Release',
                             '--target', 'link_contract', 'mini_contract']):
                result = subprocess.run(command, capture_output=True, text=True,
                                        encoding='utf-8', errors='replace')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            executable = build / ('Release/link_contract.exe' if sys.platform == 'win32'
                                  else 'link_contract')
            self.assertEqual(subprocess.run([str(executable)]).returncode, 0)
            mini = build / ('Release/mini_contract.exe' if sys.platform == 'win32'
                            else 'mini_contract')
            self.assertEqual(subprocess.run([str(mini)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
