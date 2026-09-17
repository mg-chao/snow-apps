#!/bin/bash
# Shared by the macOS entry points; compatible with Apple's Bash 3.2.
set -euo pipefail
snow_repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
snow_die() { printf '%s\n' "$*" >&2; exit 1; }
snow_require_macos() {
    [[ "$(uname -s)" == Darwin ]] || snow_die 'This entry point requires macOS.'
}
snow_default_arch() {
    case "$(uname -m)" in
        arm64) printf arm64 ;;
        x86_64) printf x64 ;;
        *) snow_die 'Unsupported host architecture.' ;;
    esac
}
snow_select_preset() {
    snow_preset="${1:-snow-shot-macos-$(snow_default_arch)-debug}"
    [[ "$snow_preset" =~ ^snow-shot-macos-(arm64|x64)-(debug|performance|release|fast)$ ]] || snow_die "Unsupported macOS preset: $snow_preset"
    case "$snow_preset" in
        snow-shot-macos-arm64-*) snow_arch=arm64; snow_rust_target=aarch64-apple-darwin ;;
        snow-shot-macos-x64-*) snow_arch=x64; snow_rust_target=x86_64-apple-darwin ;;
        *) snow_die "Unsupported macOS preset: $snow_preset" ;;
    esac
    case "${snow_preset##*-}" in
        debug|performance|release|fast) ;;
        *) snow_die "Unsupported macOS preset: $snow_preset" ;;
    esac
    snow_build_dir="$snow_repo_root/build/$snow_preset"
}
snow_setup_tools() {
    export PATH="$snow_repo_root/.tools/macos-dev/bin:$snow_repo_root/.tools/macos-media/host/bin:$PATH"
    for tool in cmake ninja cargo rustup pkg-config; do
        command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
    done
    export MACOSX_DEPLOYMENT_TARGET=15.0
    export CARGO_NET_GIT_FETCH_WITH_CLI=true
    if [[ -z "${LIBCLANG_PATH:-}" ]]; then
        LIBCLANG_PATH="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib"
        [[ -f "$LIBCLANG_PATH/libclang.dylib" ]] || LIBCLANG_PATH="$(xcode-select -p)/usr/lib"
        export LIBCLANG_PATH
    fi
    # Explicit Qt6_DIR wins; the standard Qt installer location is a convenience.
    snow_qt_dir="${Qt6_DIR:-${SNOW_QT_DIR:-$HOME/Qt/6.11.1/macos/lib/cmake/Qt6}}"
    [[ -f "$snow_qt_dir/Qt6Config.cmake" ]] || snow_die 'Set Qt6_DIR to the Qt 6.11.1 macOS lib/cmake/Qt6 directory.'
}
