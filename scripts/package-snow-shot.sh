#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: package-snow-shot.sh [snow-shot-macos-{arm64|x64}-release]'
    echo 'Builds an ad-hoc signed DMG; Developer ID signing/notarization is a separate release step.'
    exit 0
fi
[[ $# -le 1 ]] || snow_die 'Expected at most one preset. Use --help.'
snow_require_macos
snow_select_preset "${1:-snow-shot-macos-$(snow_default_arch)-release}"
[[ "$snow_preset" == *-release ]] || snow_die 'Packaging requires a macOS release preset.'
snow_setup_tools
"$snow_repo_root/scripts/build.sh" "$snow_preset"
cd "$snow_repo_root"
cpack --preset "package-$snow_preset"
