#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: package-snow-shot.sh [snow-shot-macos-{arm64|x64}-release] [--parallel JOBS] [--skip-build]'
    echo 'Builds an ad-hoc signed DMG; Developer ID signing/notarization is a separate release step.'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "${preset:-snow-shot-macos-$(snow_default_arch)-release}"
[[ "$snow_preset" == *-release ]] || snow_die 'Packaging requires a macOS release preset.'
parallelism=4
skip_build=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --parallel) [[ $# -ge 2 ]] || snow_die '--parallel needs a positive integer'; parallelism="$2"; shift 2 ;;
        --skip-build) skip_build=1; shift ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
done
[[ "$parallelism" =~ ^[1-9][0-9]*$ ]] || snow_die '--parallel needs a positive integer'
snow_setup_tools
if [[ "$skip_build" == 0 ]]; then
    "$snow_repo_root/scripts/build.sh" "$snow_preset" --clean \
        --target snow_shot --parallel "$parallelism"
fi
cache="$snow_build_dir/CMakeCache.txt"
[[ -f "$cache" ]] || snow_die "CMake cache was not found: $cache"
for entry in \
    'CMAKE_BUILD_TYPE:STRING=Release' \
    'SNOW_APPS_BUILD_TESTS:BOOL=OFF' \
    'SNOW_APPS_BUILD_BENCHMARKS:BOOL=OFF' \
    'SNOW_APPS_RELEASE_STATIC:BOOL=ON' \
    'SNOW_APPS_QT_STATIC:BOOL=ON' \
    'SNOW_APPS_PACKAGE_SNOW_SHOT:BOOL=ON' \
    'SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC:INTERNAL=ON' \
    'QT_FEATURE_static:INTERNAL=ON'; do
    grep -Fqx "$entry" "$cache" || snow_die "Release cache is not production-safe; missing '$entry'."
done
grep -Eq "^VCPKG_TARGET_TRIPLET:.*=$snow_vcpkg_triplet$" "$cache" || snow_die 'Release cache does not use the static macOS vcpkg triplet.'
cd "$snow_repo_root"
cpack --preset "package-$snow_preset"
