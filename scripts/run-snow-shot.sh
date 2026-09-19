#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: run-snow-shot.sh [macOS-preset] [--clean] [--no-build] [-- APP_ARGUMENTS...]'
    echo '--no-build launches the existing deployed app without reinstalling or signing it.'
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
deployed_app="$snow_build_dir/run/snow_shot.app"
if [[ "$build" == false && "$clean" == true ]]; then
    snow_die '--clean cannot be combined with --no-build'
fi
if [[ "$build" == false ]]; then
    [[ -x "$deployed_app/Contents/MacOS/snow_shot" ]] || snow_die "The deployed Snow Shot was not found for $snow_preset. Run without --no-build to create it."
    # Preserve the exact signed bundle to which the user granted permissions.
    exec open -n "$deployed_app" --args "$@"
fi
build_args=("$snow_preset" --target snow_shot)
if [[ "$clean" == true ]]; then build_args+=(--clean); fi
"$(dirname "$0")/build.sh" "${build_args[@]}"
app="$snow_build_dir/snow_shot/snow_shot.app"
[[ -x "$app/Contents/MacOS/snow_shot" ]] || snow_die "Snow Shot was not found for $snow_preset. Run without --no-build to create it."
# Deploy a separate development copy so dlopen-only OCR dependencies and Qt
# plugins resolve exactly as they do in a package. Leave build products intact.
export PATH="$snow_repo_root/.tools/macos-dev/bin:$PATH"
cmake --install "$snow_build_dir" --component SnowShot --prefix "$snow_build_dir/run"
# The development bundle keeps the same path and version between builds. Force
# LaunchServices to discard stale metadata (including a previously missing icon)
# before Finder and the Dock resolve the bundle for the next launch.
touch "$deployed_app"
default_launch_services_register='/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
launch_services_register="${SNOW_LAUNCH_SERVICES_REGISTER:-$default_launch_services_register}"
"$launch_services_register" -f "$deployed_app"
# LaunchServices establishes the bundle identity used by macOS permissions.
exec open -n "$deployed_app" --args "$@"
