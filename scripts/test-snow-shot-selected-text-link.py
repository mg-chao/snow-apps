"""Native linker regression for Full's separate selected-text Rust archive."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SelectedTextLinkContract(unittest.TestCase):
    def test_host_runtime_precedes_separate_selected_text_archive(self):
        with tempfile.TemporaryDirectory(prefix='snow-selected-text-link-') as directory:
            fixture = Path(directory)
            sources = {
                'bundle.cpp': 'int runtime_value() { return 7; }\nint shared_entry() { return 11; }\n',
                'selected.cpp': 'int runtime_value();\nint selected_entry() { return runtime_value(); }\n',
                'runtime.cpp': 'int runtime_value() { return 7; }\n',
                'main.cpp': 'int shared_entry();\nint selected_entry();\n'
                            'int main() { return shared_entry() + selected_entry() == 18 ? 0 : 1; }\n',
            }
            for name, source in sources.items():
                (fixture / name).write_text(source, encoding='utf-8')
            # Model the actual archive boundary: fat-LTO bundles runtime symbols
            # with required C ABI code; the separate archive also ships a runtime.
            # Use tiny native objects to exercise extraction order without Cargo.
            (fixture / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 4.2)
project(SelectedTextLink LANGUAGES CXX)
add_library(snow_shot_rust_ffi_bundle STATIC bundle.cpp)
function(snow_add_rust_static_library target)
    add_library(${{target}} STATIC selected.cpp runtime.cpp)
endfunction()
set(SNOW_SHOT_CAPTURE_CRATES_DIR "{ROOT.as_posix()}/snow-crates")
include("{ROOT.as_posix()}/cmake/SnowSelectedText.cmake")
add_executable(link_contract main.cpp)
target_link_libraries(link_contract PRIVATE snow_selected_text_c)
''', encoding='utf-8')
            build = fixture / 'build'
            for command in (['cmake', '-S', str(fixture), '-B', str(build)],
                            ['cmake', '--build', str(build), '--config', 'Release']):
                result = subprocess.run(command, capture_output=True, text=True,
                                        encoding='utf-8', errors='replace')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            executable = build / ('Release/link_contract.exe' if sys.platform == 'win32'
                                  else 'link_contract')
            self.assertEqual(subprocess.run([str(executable)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
