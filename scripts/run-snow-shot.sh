#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: run-snow-shot.sh [macOS-preset] [--clean] [--no-build] [-- APP_ARGUMENTS...]'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
clean=false
build=true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) clean=true; shift ;;
        --no-build) build=false; shift ;;
        --) shift; break ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
done
if [[ "$build" == true ]]; then
    build_args=("$snow_preset" --target snow_shot)
    if [[ "$clean" == true ]]; then build_args+=(--clean); fi
    "$(dirname "$0")/build.sh" "${build_args[@]}"
fi
app="$snow_build_dir/snow_shot/snow_shot.app"
[[ -x "$app/Contents/MacOS/snow_shot" ]] || snow_die "Snow Shot was not found for $snow_preset. Run without --no-build to create it."
# Deploy a separate development copy so dlopen-only OCR dependencies and Qt
# plugins resolve exactly as they do in a package. Leave build products intact.
export PATH="$snow_repo_root/.tools/macos-dev/bin:$PATH"
cmake --install "$snow_build_dir" --component SnowShot --prefix "$snow_build_dir/run"
# LaunchServices establishes the bundle identity used by macOS permissions.
exec open -n "$snow_build_dir/run/snow_shot.app" --args "$@"
