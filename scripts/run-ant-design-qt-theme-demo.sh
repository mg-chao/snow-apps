#!/bin/bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run-ant-design-qt-theme-demo.sh [options]

Options:
  --clean       Remove the dedicated build directory before building.
  --no-build    Run the existing executable without configuring or building.
  --build-only  Configure and build without launching the demo.
  --detach      Launch the demo in the background.
  --release     Build the Release configuration instead of Debug.
  -h, --help    Show this help text.
EOF
}

clean=false
no_build=false
build_only=false
detach=false
build_type=Debug

while (($# > 0)); do
    case "$1" in
        --clean) clean=true ;;
        --no-build) no_build=true ;;
        --build-only) build_only=true ;;
        --detach) detach=true ;;
        --release) build_type=Release ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [[ "$(uname -s)" != Darwin ]]; then
    printf 'This launcher requires macOS.\n' >&2
    exit 1
fi
if $clean && $no_build; then
    printf '%s\n' '--clean and --no-build cannot be used together.' >&2
    exit 2
fi
if $build_only && $no_build; then
    printf '%s\n' '--build-only and --no-build cannot be used together.' >&2
    exit 2
fi
if $build_only && $detach; then
    printf '%s\n' '--build-only and --detach cannot be used together.' >&2
    exit 2
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_directory="$repo_root/build/macos-ant-design-qt-theme-demo"
executable="$build_directory/ant_design_qt/theme-demo"

if ! $no_build; then
    if [[ -x "$repo_root/.tools/macos-dev/bin/cmake" ]]; then
        cmake_command="$repo_root/.tools/macos-dev/bin/cmake"
    elif command -v cmake >/dev/null 2>&1; then
        cmake_command="$(command -v cmake)"
    else
        printf 'CMake 4.2 or newer was not found.\n' >&2
        exit 1
    fi

    ninja_command=""
    if [[ -x "$repo_root/.tools/macos-dev/bin/ninja" ]]; then
        ninja_command="$repo_root/.tools/macos-dev/bin/ninja"
    elif command -v ninja >/dev/null 2>&1; then
        ninja_command="$(command -v ninja)"
    fi

    qt_prefix=""
    if [[ -n "${Qt6_DIR:-}" && -f "$Qt6_DIR/Qt6Config.cmake" ]]; then
        qt_prefix="$(cd "$Qt6_DIR/../../.." && pwd)"
    elif [[ -n "${QTDIR:-}" && -f "$QTDIR/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
        qt_prefix="$QTDIR"
    else
        for qt_query in qtpaths6 qtpaths qmake6 qmake; do
            if ! command -v "$qt_query" >/dev/null 2>&1; then
                continue
            fi
            if [[ "$qt_query" == qtpaths* ]]; then
                qt_prefix="$("$qt_query" --query QT_INSTALL_PREFIX)"
            else
                qt_prefix="$("$qt_query" -query QT_INSTALL_PREFIX)"
            fi
            if [[ -f "$qt_prefix/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
                break
            fi
            qt_prefix=""
        done
    fi
    if [[ -z "$qt_prefix" ]]; then
        printf 'Qt 6 was not found. Set Qt6_DIR or QTDIR, or add qmake/qtpaths to PATH.\n' >&2
        exit 1
    fi

    if $clean && [[ -d "$build_directory" ]]; then
        "$cmake_command" -E remove_directory "$build_directory"
    fi

    configure_arguments=(-S "$repo_root" -B "$build_directory")
    if [[ -n "$ninja_command" ]]; then
        configure_arguments+=(-G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja_command")
    fi
    configure_arguments+=(
        "-DCMAKE_BUILD_TYPE=$build_type"
        "-DCMAKE_PREFIX_PATH=$qt_prefix"
        -DSNOW_APPS_BUILD_ANT_DESIGN_QT=ON
        -DSNOW_APPS_BUILD_SNOW_IMAGE=ON
        -DSNOW_APPS_BUILD_DRAW_ENGINE=OFF
        -DSNOW_APPS_BUILD_IMAGE_VIEWER=OFF
        -DSNOW_APPS_BUILD_SNOW_SHOT=OFF
        -DSNOW_APPS_BUILD_TESTS=OFF
        -DSNOW_APPS_BUILD_BENCHMARKS=OFF
        # Keep this local demo build usable with Clang's additional diagnostics.
        -DSNOW_APPS_STRICT_COMPILE=OFF
        -DSNOW_IMAGE_PROFILE=redistributable
    )
    "$cmake_command" "${configure_arguments[@]}"
    "$cmake_command" --build "$build_directory" --target theme-demo --parallel
fi

if $build_only; then
    exit 0
fi
if [[ ! -x "$executable" ]]; then
    printf 'Theme demo executable was not found at %s.\n' "$executable" >&2
    printf 'Run this script without --no-build to create it.\n' >&2
    exit 1
fi

working_directory="$(dirname "$executable")"
if $detach; then
    log_file="$build_directory/theme-demo.log"
    (cd "$working_directory" && nohup "$executable" >"$log_file" 2>&1 &)
    printf 'Theme demo started. Log: %s\n' "$log_file"
    exit 0
fi

cd "$working_directory"
exec "$executable"
