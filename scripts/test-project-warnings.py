#!/usr/bin/env python3
"""Check Clang diagnostics for both strict-warning entry points."""

import argparse
import pathlib
import re
import subprocess
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, errors="replace", check=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", help="CMake generator to use")
    parser.add_argument("--architecture", help="CMake generator architecture")
    parser.add_argument("--toolset", help="CMake generator toolset")
    parser.add_argument("--compiler", help="C++ compiler, such as clang-cl on Windows")
    args = parser.parse_args()

    module = pathlib.Path(__file__).resolve().parents[1] / "cmake" / "ProjectWarnings.cmake"
    with tempfile.TemporaryDirectory(prefix="snow-project-warnings-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        directory_policy = source / "directory_policy"
        directory_policy.mkdir(parents=True)
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.30)\n"
            "project(snow_warning_probe LANGUAGES CXX)\n"
            f'include("{module.as_posix()}")\n'
            "add_executable(target_signed warning.cpp)\n"
            "target_compile_definitions(target_signed PRIVATE TEST_SIGNED)\n"
            "snow_apply_strict_warnings(target_signed)\n"
            "add_executable(target_aggregate warning.cpp)\n"
            "target_compile_definitions(target_aggregate PRIVATE TEST_AGGREGATE)\n"
            "snow_apply_strict_warnings(target_aggregate)\n"
            "add_executable(explicit_cast warning.cpp)\n"
            "snow_apply_strict_warnings(explicit_cast)\n"
            "add_subdirectory(directory_policy)\n",
            encoding="utf-8",
        )
        (directory_policy / "CMakeLists.txt").write_text(
            "snow_enable_strict_warnings()\n"
            "add_executable(directory_signed ../warning.cpp)\n"
            "target_compile_definitions(directory_signed PRIVATE TEST_SIGNED)\n"
            "add_executable(directory_aggregate ../warning.cpp)\n"
            "target_compile_definitions(directory_aggregate PRIVATE TEST_AGGREGATE)\n",
            encoding="utf-8",
        )
        (source / "warning.cpp").write_text(
            "#include <cstddef>\n"
            "struct Pair { int first; int second; };\n"
            "long long bytes(std::size_t count) {\n"
            "#ifdef TEST_SIGNED\n"
            "    return count;\n"
            "#else\n"
            "    return static_cast<long long>(count);\n"
            "#endif\n"
            "}\n"
            "int main() {\n"
            "#ifdef TEST_AGGREGATE\n"
            "    Pair pair{1};\n"
            "#else\n"
            "    Pair pair{1, 2};\n"
            "#endif\n"
            "    return bytes(1) == 1 && pair.first == 1 ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )

        configure = ["cmake", "-S", str(source), "-B", str(build)]
        if args.compiler:
            configure.append(f"-DCMAKE_CXX_COMPILER={args.compiler}")
        generator_options = (
            ("-G", args.generator),
            ("-A", args.architecture),
            ("-T", args.toolset),
        )
        for option, value in generator_options:
            if value:
                configure.extend((option, value))
        result = run(configure)
        if result.returncode:
            raise SystemExit(f"CMake configure failed:\n{result.stdout}{result.stderr}")

        checks = (
            ("explicit_cast", None),
            ("target_signed", r"[-]Wsign-conversion"),
            ("directory_signed", r"[-]Wsign-conversion"),
            ("target_aggregate", r"[-]Wmissing-field-initializers"),
            ("directory_aggregate", r"[-]Wmissing-field-initializers"),
        )
        for target, diagnostic in checks:
            result = run([
                "cmake", "--build", str(build), "--config", "Debug", "--target", target
            ])
            output = result.stdout + result.stderr
            if diagnostic is None and result.returncode:
                raise SystemExit(f"{target} unexpectedly failed:\n{output}")
            if diagnostic is not None and (
                result.returncode == 0 or not re.search(diagnostic, output)
            ):
                raise SystemExit(f"{target} did not fail on {diagnostic}:\n{output}")
            print(f"{target}: {'passed' if diagnostic is None else 'rejected expected warning'}")


if __name__ == "__main__":
    main()
