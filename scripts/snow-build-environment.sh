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
        debug|performance) snow_static_build=0 ;;
        release|fast) snow_static_build=1 ;;
        *) snow_die "Unsupported macOS preset: $snow_preset" ;;
    esac
    if [[ "$snow_static_build" == 1 ]]; then
        snow_vcpkg_triplet="${snow_arch}-osx-snow-shot-static"
        snow_vcpkg_installed="$snow_repo_root/.tools/macos/installed/static"
    else
        snow_vcpkg_triplet="${snow_arch}-osx-snow-shot"
        snow_vcpkg_installed="$snow_repo_root/.tools/macos/installed/dynamic"
    fi
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
    # Release and fast presets mirror Windows by using an audited static Qt kit.
    # Development presets continue to use the official shared kit.
    if [[ "${snow_static_build:-0}" == 1 ]]; then
        snow_qt_dir="${SNOW_QT_STATIC_DIR:-${Qt6_DIR:-$HOME/Qt/6.11.1/macos-static-$snow_arch/lib/cmake/Qt6}}"
    else
        snow_qt_dir="${Qt6_DIR:-${SNOW_QT_DIR:-$HOME/Qt/6.11.1/macos/lib/cmake/Qt6}}"
    fi
    [[ -f "$snow_qt_dir/Qt6Config.cmake" ]] || snow_die 'Set Qt6_DIR to the Qt 6.11.1 macOS lib/cmake/Qt6 directory.'
    snow_qt_dir="$(cd "$snow_qt_dir" && pwd)"
    if [[ "${snow_static_build:-0}" == 1 ]]; then
        snow_qt_prefix="$(cd "$snow_qt_dir/../../.." && pwd)"
        snow_qt_stamp="$snow_qt_prefix/share/snow-apps/static-qt-build.json"
        [[ -f "$snow_qt_stamp" ]] || snow_die "The audited static Qt build stamp was not found: $snow_qt_stamp. Run scripts/build-static-qt.sh."
        grep -Eq '"SchemaVersion"[[:space:]]*:[[:space:]]*1' "$snow_qt_stamp" || snow_die 'The static Qt build stamp schema is unsupported.'
        grep -Eq '"QtVersion"[[:space:]]*:[[:space:]]*"6\.11\.1"' "$snow_qt_stamp" || snow_die 'The static Qt build stamp has the wrong Qt version.'
        grep -Eq '"Architecture"[[:space:]]*:[[:space:]]*"'"$snow_arch"'"' "$snow_qt_stamp" || snow_die 'The static Qt build stamp has the wrong architecture.'
        grep -Eq '"Configuration"[[:space:]]*:[[:space:]]*"Release"' "$snow_qt_stamp" || snow_die 'The static Qt build stamp is not a Release kit.'
        grep -Eq '"DeploymentTarget"[[:space:]]*:[[:space:]]*"14\.0"' "$snow_qt_stamp" || snow_die 'The static Qt build has the wrong deployment target.'
        grep -Eq '"Dup3"[[:space:]]*:[[:space:]]*false' "$snow_qt_stamp" || snow_die 'The static Qt build can use dup3 outside its deployment range.'
        grep -Eq '"Ltcg"[[:space:]]*:[[:space:]]*true' "$snow_qt_stamp" || snow_die 'The static Qt build does not enable LTO.'
        grep -Eq '"SystemPng"[[:space:]]*:[[:space:]]*true' "$snow_qt_stamp" || snow_die 'The static Qt build does not use the audited system libpng.'
        grep -Eq '"SystemZlib"[[:space:]]*:[[:space:]]*true' "$snow_qt_stamp" || snow_die 'The static Qt build does not use the audited system zlib.'
        [[ -d "$snow_qt_prefix/share/snow-apps/qt-licenses" ]] || snow_die 'The static Qt source-license bundle is missing.'
        export SNOW_QT_STATIC_DIR="$snow_qt_dir"
    fi
    export Qt6_DIR="$snow_qt_dir"
}

snow_cache_aligned() {
    local cache_path="$1"
    local cached_qt
    [[ -f "$cache_path" ]] || return 1
    cached_qt="$(sed -n 's/^Qt6_DIR:[^=]*=//p' "$cache_path" | head -n 1)"
    [[ "$cached_qt" == "$snow_qt_dir" ]] &&
        grep -Eq "^VCPKG_TARGET_TRIPLET:.*=$snow_vcpkg_triplet$" "$cache_path" &&
        grep -Fqx "VCPKG_INSTALLED_DIR:PATH=$snow_vcpkg_installed" "$cache_path" &&
        grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$snow_repo_root" "$cache_path" &&
        grep -Fqx 'CMAKE_GENERATOR:INTERNAL=Ninja' "$cache_path"
}
