#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: build.sh [macOS-preset] [--target TARGET] [--parallel JOBS] [--clean] [--skip-bootstrap] [-- CMAKE_OPTIONS...]'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
target=snow_shot
parallelism=''
clean=0
skip_bootstrap=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) [[ $# -ge 2 ]] || snow_die '--target needs a target'; target="$2"; shift 2 ;;
        --parallel) [[ $# -ge 2 ]] || snow_die '--parallel needs a positive integer'; parallelism="$2"; shift 2 ;;
        --clean) clean=1; shift ;;
        --skip-bootstrap) skip_bootstrap=1; shift ;;
        --) shift; break ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
done
if [[ -n "$parallelism" && ! "$parallelism" =~ ^[1-9][0-9]*$ ]]; then
    snow_die '--parallel needs a positive integer'
fi
snow_setup_tools
cd "$snow_repo_root"
[[ -x .tools/vcpkg/vcpkg ]] || snow_die 'Run scripts/bootstrap-macos.sh first.'
if [[ "$skip_bootstrap" == 0 ]]; then
    "$snow_repo_root/scripts/bootstrap-macos.sh" "$snow_preset" --skip-dependency-install
fi
if [[ "$clean" == 1 && -e "$snow_build_dir" ]]; then
    case "$snow_build_dir" in
        "$snow_repo_root"/build/snow-shot-macos-*) rm -rf -- "$snow_build_dir" ;;
        *) snow_die "Refusing to remove an unexpected build directory: $snow_build_dir" ;;
    esac
fi
configure=(--preset "$snow_preset" -D "Qt6_DIR=$snow_qt_dir")
if [[ -f "$snow_build_dir/CMakeCache.txt" ]]; then
    if snow_cache_aligned "$snow_build_dir/CMakeCache.txt"; then
        printf 'Reusing the existing CMake cache for preset %s.\n' "$snow_preset"
    else
        printf 'The existing CMake cache does not match preset %s; configuring from a fresh cache.\n' "$snow_preset"
        configure=(--fresh "${configure[@]}")
    fi
fi
cmake "${configure[@]}" "$@"
build=(--build --preset "build-$snow_preset" --target "$target" --parallel)
if [[ -n "$parallelism" ]]; then build+=("$parallelism"); fi
cmake "${build[@]}"
