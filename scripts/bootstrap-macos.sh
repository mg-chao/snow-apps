#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: bootstrap-macos.sh [macOS-preset] [--skip-dependency-install] [--skip-qt-validation]'
    echo 'Requires Xcode command-line tools, Rust (rustup), Qt 6.11.1, CMake >= 4.2, Ninja and pkg-config.'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
skip_dependencies=0
skip_qt=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-dependency-install) skip_dependencies=1 ;;
        --skip-qt-validation) skip_qt=1 ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
    shift
done
xcrun --find clang >/dev/null
if [[ "$skip_qt" == 1 ]]; then
    export PATH="$snow_repo_root/.tools/macos-dev/bin:$snow_repo_root/.tools/macos-media/host/bin:$PATH"
    for tool in cmake ninja cargo rustup pkg-config; do
        command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
    done
else
    snow_setup_tools
fi
for tool in git python3; do
    command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
done
cmake_version="$(cmake --version | head -n 1)"
if [[ ! "$cmake_version" =~ ^cmake[[:space:]]version[[:space:]]([0-9]+)\.([0-9]+) ]] ||
    (( BASH_REMATCH[1] < 4 || (BASH_REMATCH[1] == 4 && BASH_REMATCH[2] < 2) )); then
    snow_die "CMake 4.2 or newer is required; found: ${cmake_version:-unknown}"
fi
cd "$snow_repo_root"
rustup toolchain install 1.97.1 --profile minimal --component rustfmt --component clippy --target "$snow_rust_target"
# Use the same pinned registry baseline and isolated dynamic/static install roots as Windows.
vcpkg_root="$snow_repo_root/.tools/vcpkg"
vcpkg_baseline=4497409a47f19db373a410a0efb84eca4747adbf
if [[ ! -d "$vcpkg_root/.git" ]]; then
    [[ ! -e "$vcpkg_root" ]] || snow_die "Repository-local vcpkg must be a Git checkout: $vcpkg_root"
    git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
fi
vcpkg_head="$(git -C "$vcpkg_root" rev-parse HEAD 2>/dev/null || true)"
if [[ "$vcpkg_head" != "$vcpkg_baseline" ]]; then
    git -C "$vcpkg_root" fetch origin "$vcpkg_baseline" --depth=1
    git -C "$vcpkg_root" checkout --detach "$vcpkg_baseline"
fi
if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
    "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi
if [[ "$skip_dependencies" == 0 ]]; then
    "$vcpkg_root/vcpkg" install \
        "--x-manifest-root=$snow_repo_root" \
        "--x-install-root=$snow_vcpkg_installed" \
        "--triplet=$snow_vcpkg_triplet" \
        --x-feature=snow-shot \
        "--overlay-ports=$snow_repo_root/cmake/vcpkg-overlay-ports" \
        "--overlay-triplets=$snow_repo_root/cmake/vcpkg-overlay-triplets" \
        --clean-after-build
fi
