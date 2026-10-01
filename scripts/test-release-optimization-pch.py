#!/usr/bin/env python3
"""Build a tiny MSVC fixture for mixed optimization policies and precompiled headers."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


REPOSITORY = Path(__file__).resolve().parents[1]
WARNINGS = REPOSITORY / "cmake/ProjectWarnings.cmake"
MSBUILD = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}


def run(command, succeeds=True, expected_diagnostic=None):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    output = result.stdout + result.stderr
    if (result.returncode == 0) != succeeds:
        raise SystemExit(f"Unexpected command result ({result.returncode}): {' '.join(command)}\n{output}")
    if expected_diagnostic and expected_diagnostic not in output:
        raise SystemExit(f"Missing expected {expected_diagnostic}:\n{output}")


def property_value(node, key, configuration):
    values = [item.text for item in node.findall(f"m:{key}", MSBUILD)
              if not item.get("Condition") or f"{configuration}|" in item.get("Condition", "")]
    return values[-1] if values else None


def check_project(build, configuration, optimized):
    tree = ET.parse(build / "probe.vcxproj")
    group = next(item for item in tree.findall("m:ItemDefinitionGroup", MSBUILD)
                 if f"{configuration}|" in item.get("Condition", ""))
    target = group.find("m:ClCompile", MSBUILD)
    sources = {Path(item.get("Include")).name: item
               for item in tree.findall(".//m:ClCompile[@Include]", MSBUILD)}
    cold = sources["cold.cpp"]
    hot = sources["hot.cpp"]

    def effective(source, key):
        return property_value(source, key, configuration) or property_value(target, key, configuration)

    expected_cold_pch = "NotUsing" if optimized else "Use"
    if effective(cold, "PrecompiledHeader") != expected_cold_pch:
        raise SystemExit(f"{configuration}: cold source PCH policy is incorrect")
    if effective(hot, "PrecompiledHeader") != "Use":
        raise SystemExit(f"{configuration}: hot source must retain the target PCH")
    if configuration == "Release":
        expected_optimization = "MinSpace" if optimized else "MaxSpeed"
        if effective(cold, "Optimization") != expected_optimization:
            raise SystemExit("Cold source's effective Release optimization is incorrect")
        if effective(hot, "Optimization") != "MaxSpeed":
            raise SystemExit("Hot source's effective Release optimization changed")
        if optimized and effective(cold, "FavorSizeOrSpeed") != "Size":
            raise SystemExit("Cold source still inherits the target's speed preference")
    print(f"{configuration}: cold PCH={expected_cold_pch}; hot PCH=Use; effective flags verified")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", default="Visual Studio 18 2026")
    parser.add_argument("--architecture", default="x64")
    parser.add_argument("--toolset", default="host=x64,version=14.51")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="snow-release-pch-") as temporary:
        source = Path(temporary) / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.30)\n'
            'project(ReleasePch LANGUAGES CXX)\n'
            'if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")\n'
            '  message(FATAL_ERROR "This PCH regression fixture requires native MSVC")\n'
            'endif()\n'
            'set(CMAKE_CXX_STANDARD 20)\n'
            f'include("{WARNINGS.as_posix()}")\n'
            'add_executable(probe main.cpp hot.cpp cold.cpp)\n'
            'target_compile_options(probe PRIVATE /W4 /WX)\n'
            'target_precompile_headers(probe PRIVATE pch.h)\n'
            'snow_apply_release_options(probe POLICY speed)\n'
            'snow_apply_release_size_sources(probe SOURCES cold.cpp)\n'
            # Reproduce the original defect independently of the fixed helper.
            'add_executable(inconsistent main.cpp hot.cpp incompatible.cpp)\n'
            'target_compile_options(inconsistent PRIVATE /W4 /WX)\n'
            'target_precompile_headers(inconsistent PRIVATE pch.h)\n'
            'snow_apply_release_options(inconsistent POLICY speed)\n'
            '_snow_release_compile_options(size size_options)\n'
            'set_property(SOURCE incompatible.cpp APPEND PROPERTY COMPILE_OPTIONS ${size_options})\n',
            encoding="utf-8")
        (source / "pch.h").write_text("#pragma once\n#include <array>\n", encoding="utf-8")
        (source / "main.cpp").write_text(
            "int coldValue();\nint hotValue();\n"
            "int main() { return coldValue() + hotValue() == 11 ? 0 : 1; }\n", encoding="utf-8")
        (source / "hot.cpp").write_text(
            "#include <array>\nint hotValue() { return std::array{2, 4, 8}[2]; }\n", encoding="utf-8")
        for name in ("cold.cpp", "incompatible.cpp"):
            (source / name).write_text(
                "#include <array>\nint coldValue() { return std::array{1, 3, 5}[1]; }\n",
                encoding="utf-8")
        for name, optimization, size, configurations in (
            ("shipping", True, True, ("Release",)),
            ("default", True, False, ("Release", "Debug")),
            ("fast", False, True, ("Release",)),
        ):
            build = Path(temporary) / name
            run(["cmake", "-S", str(source), "-B", str(build), "-G", args.generator,
                 "-A", args.architecture, "-T", args.toolset,
                 f"-DSNOW_APPS_ENABLE_RELEASE_OPTIMIZATION={'ON' if optimization else 'OFF'}",
                 f"-DSNOW_APPS_ENABLE_RELEASE_SIZE_OPTIMIZATION={'ON' if size else 'OFF'}"])
            if name == "shipping":
                run(["cmake", "--build", str(build), "--config", "Release", "--target", "inconsistent"],
                    succeeds=False, expected_diagnostic="C4653")
                print("Original mixed-PCH defect reproduced as C4653 with /WX")
            for configuration in configurations:
                run(["cmake", "--build", str(build), "--config", configuration, "--target", "probe"])
                check_project(build, configuration, optimized=optimization and size)
                run([str(build / configuration / "probe.exe")])
        print("MSVC mixed-policy PCH regression passed")


if __name__ == "__main__":
    main()
