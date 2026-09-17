#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: run-snow-shot.sh [macOS-preset] [-- APP_ARGUMENTS...]'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
if [[ "${1:-}" == -- ]]; then shift; fi
app="$snow_build_dir/snow_shot/snow_shot.app"
[[ -x "$app/Contents/MacOS/snow_shot" ]] || snow_die "Build $snow_preset first."
# Deploy a separate development copy so dlopen-only OCR dependencies and Qt
# plugins resolve exactly as they do in a package. Leave build products intact.
export PATH="$snow_repo_root/.tools/macos-dev/bin:$PATH"
cmake --install "$snow_build_dir" --component SnowShot --prefix "$snow_build_dir/run"
# LaunchServices establishes the bundle identity used by macOS permissions.
exec open -n "$snow_build_dir/run/snow_shot.app" --args "$@"
