#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: bootstrap-macos.sh [snow-shot-macos-{arm64|x64}-{debug|performance|release|fast}]'
    echo 'Requires Xcode command-line tools, Rust (rustup), Qt 6.11.1, CMake >= 4.2, Ninja and pkg-config.'
    exit 0
fi
[[ $# -le 1 ]] || snow_die 'Expected at most one preset. Use --help.'
snow_require_macos
snow_select_preset "${1:-}"
xcrun --find clang >/dev/null
snow_setup_tools
cd "$snow_repo_root"
rustup target add "$snow_rust_target"
# Use the same pinned registry baseline as the Windows bootstrap.
vcpkg_root="$snow_repo_root/.tools/vcpkg"
if [[ ! -f "$vcpkg_root/bootstrap-vcpkg.sh" ]]; then
    git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
    git -C "$vcpkg_root" checkout 4497409a47f19db373a410a0efb84eca4747adbf
fi
if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
    "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi
# Configure installs the exact manifest feature set and validates the Qt kit.
cmake --preset "$snow_preset" -D "Qt6_DIR=$snow_qt_dir"
