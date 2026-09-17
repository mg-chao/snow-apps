#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: build.sh [macOS-preset] [--target TARGET] [--clean] [-- CMAKE_OPTIONS...]'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
target=snow_shot
clean=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) [[ $# -ge 2 ]] || snow_die '--target needs a target'; target="$2"; shift 2 ;;
        --clean) clean=(--clean-first); shift ;;
        --) shift; break ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
done
snow_setup_tools
cd "$snow_repo_root"
[[ -x .tools/vcpkg/vcpkg ]] || snow_die 'Run scripts/bootstrap-macos.sh first.'
cmake --preset "$snow_preset" -D "Qt6_DIR=$snow_qt_dir" "$@"
cmake --build --preset "build-$snow_preset" --target "$target" ${clean[@]+"${clean[@]}"} --parallel
