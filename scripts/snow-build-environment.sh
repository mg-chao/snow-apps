#!/bin/bash
# Shared by the macOS entry points; compatible with Apple's Bash 3.2.
set -euo pipefail
snow_repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
snow_die() { printf '%s\n' "$*" >&2; exit 1; }
snow_load_qt_policy() {
    snow_qt_version="$(python3 "$snow_repo_root/scripts/validate-static-qt.py" --print-version)" || snow_die 'The Qt toolchain policy is invalid.'
    snow_qt_source_sha256="$(python3 "$snow_repo_root/scripts/validate-static-qt.py" --print-source-sha256)" || snow_die 'The Qt source archive policy is invalid.'
    snow_qt_deployment_target="$(python3 "$snow_repo_root/scripts/validate-static-qt.py" --print-macos-deployment-target)" || snow_die 'The macOS Qt deployment policy is invalid.'
}
snow_install_qt_license_metadata() {
    local source_dir="$1" license_root="$2" component component_source metadata_file
    # Validate every component before copying any part of the bundle.
    for component in root qtbase qtsvg qttools qttranslations; do
        component_source="$source_dir"
        [[ "$component" == root ]] || component_source="$source_dir/$component"
        metadata_file=REUSE.toml
        # Qt Translations retains upstream license rules rather than REUSE metadata.
        [[ "$component" != qttranslations ]] || metadata_file=licenseRule.json
        [[ -f "$component_source/$metadata_file" && -d "$component_source/LICENSES" ]] || snow_die "Qt licensing metadata is incomplete for $component"
    done
    mkdir -p "$license_root"
    for component in root qtbase qtsvg qttools qttranslations; do
        component_source="$source_dir"
        [[ "$component" == root ]] || component_source="$source_dir/$component"
        metadata_file=REUSE.toml
        [[ "$component" != qttranslations ]] || metadata_file=licenseRule.json
        mkdir -p "$license_root/$component"
        cp "$component_source/$metadata_file" "$license_root/$component/"
        cp -R "$component_source/LICENSES" "$license_root/$component/"
    done
}
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
snow_require_target_tools() {
    local target_arch="$1"
    snow_requires_rosetta=0
    if [[ "$target_arch" == x64 ]] &&
        [[ "$(snow_default_arch)" == arm64 || "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" == 1 ]]; then
        arch -x86_64 /usr/bin/true >/dev/null 2>&1 || snow_die \
            'macOS x64 builds on Apple Silicon require Rosetta to run Qt tools and validate the OCR worker. Install it with: softwareupdate --install-rosetta --agree-to-license'
        snow_requires_rosetta=1
    fi
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
    for tool in cmake ninja cargo rustup pkg-config python3; do
        command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
    done
    snow_require_target_tools "$snow_arch"
    snow_load_qt_policy
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
        snow_qt_dir="${SNOW_QT_STATIC_DIR:-${Qt6_DIR:-$HOME/Qt/$snow_qt_version/macos-static-$snow_arch/lib/cmake/Qt6}}"
    else
        snow_qt_dir="${Qt6_DIR:-${SNOW_QT_DIR:-$HOME/Qt/$snow_qt_version/macos/lib/cmake/Qt6}}"
    fi
    [[ -f "$snow_qt_dir/Qt6Config.cmake" ]] || snow_die "Set Qt6_DIR to the Qt $snow_qt_version macOS lib/cmake/Qt6 directory."
    snow_qt_dir="$(cd "$snow_qt_dir" && pwd)"
    if [[ "${snow_static_build:-0}" == 1 ]]; then
        snow_qt_prefix="$(cd "$snow_qt_dir/../../.." && pwd)"
        snow_qt_stamp="$snow_qt_prefix/share/snow-apps/static-qt-build.json"
        [[ -f "$snow_qt_stamp" ]] || snow_die "The audited static Qt build stamp was not found: $snow_qt_stamp. Run scripts/build-static-qt.sh."
        python3 "$snow_repo_root/scripts/validate-static-qt.py" --prefix "$snow_qt_prefix" \
            --arch "$snow_arch" || snow_die 'Rebuild the audited static Qt kit with scripts/build-static-qt.sh.'
        export SNOW_QT_STATIC_DIR="$snow_qt_dir"
    else
        python3 "$snow_repo_root/scripts/validate-static-qt.py" --prefix "$(cd "$snow_qt_dir/../../.." && pwd)" \
            --arch "$snow_arch" --kit-only || snow_die "Install the Qt $snow_qt_version macOS kit for $snow_arch."
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
