#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
[[ "$(uname -s)" == Darwin ]] || { echo "This bootstrap requires macOS." >&2; exit 1; }
[[ "$(uname -m)" == arm64 ]] || { echo "This preset requires an Apple Silicon Mac." >&2; exit 1; }
command -v brew >/dev/null || { echo "Install Homebrew before running this script." >&2; exit 1; }
command -v cargo >/dev/null || { echo "Install Rust with rustup before running this script." >&2; exit 1; }
xcrun --find clang >/dev/null

# macOS requires Qt 6.11.2 for the native cursor lifetime fix (QTBUG-147602).
# Codec dependencies come from Homebrew; no vcpkg Windows overlay is applied.
# Keep the FFmpeg API compatible with the repository's Rust bindings.
HOMEBREW_NO_AUTO_UPDATE=1 brew install cmake ninja pkg-config ffmpeg@8 zxing-cpp minizip-ng \
    jpeg-xl libheif webp onnxruntime
# Homebrew supplies the codec libraries; our pinned FFmpeg also enables WebP.
"$root/scripts/build-macos-ffmpeg.sh"
qt_version=6.11.2
if [[ ! -x "$root/.tools/qt/$qt_version/macos/bin/qmake" ]]; then
    python3 -m venv "$root/.tools/python"
    "$root/.tools/python/bin/python" -m pip install 'aqtinstall==3.3.0'
    "$root/.tools/python/bin/aqt" install-qt mac desktop "$qt_version" clang_64 -O "$root/.tools/qt"
fi

# Keep the exact Qt release's REUSE metadata beside the binary kit for packaging.
for component in qtbase qtsvg; do
    source="$root/.tools/$component-$qt_version-licenses"
    if [[ ! -d "$source/.git" ]]; then
        git clone --depth 1 --branch "v$qt_version" --filter=blob:none --sparse \
            "https://github.com/qt/$component.git" "$source"
    fi
    [[ "$(git -C "$source" describe --tags --exact-match)" == "v$qt_version" ]] || {
        echo "Qt license checkout has the wrong version: $source" >&2; exit 1;
    }
    git -C "$source" sparse-checkout set LICENSES
    destination="$root/.tools/qt/$qt_version/macos/share/snow-apps/qt-licenses/$component"
    mkdir -p "$destination"
    cp "$source/REUSE.toml" "$destination/"
    cp -R "$source/LICENSES" "$destination/"
    git -C "$source" rev-parse HEAD > "$destination/source-revision.txt"
done

# The shared license collector is PowerShell. Keep its runtime local to the repo
# when PowerShell is not already installed, and verify the official archive.
if ! command -v pwsh >/dev/null && [[ ! -x "$root/.tools/powershell/pwsh" ]]; then
    archive="$root/.tools/powershell.tar.gz"
    curl -fL --retry 3 https://github.com/PowerShell/PowerShell/releases/download/v7.6.6/powershell-7.6.6-osx-arm64.tar.gz -o "$archive"
    expected=6df833d094ebac1c1a74340d7b3437f4aaf5e03ce640484a1c4359f3ce8b3db1
    actual="$(shasum -a 256 "$archive" | cut -d ' ' -f 1)"
    [[ "$actual" == "$expected" ]] || { echo "PowerShell archive checksum mismatch." >&2; exit 1; }
    mkdir -p "$root/.tools/powershell"
    tar -xzf "$archive" -C "$root/.tools/powershell"
    chmod +x "$root/.tools/powershell/pwsh"
fi

# Snow Image's PNG encoder requires zlib-ng in zlib compatibility mode. Build the
# same pinned revision as cmake/vcpkg-overlay-ports/zlib, without replacing system zlib.
zlib_revision=12731092979c6d07f42da27da673a9f6c7b13586
if [[ ! -d "$root/.tools/zlib-ng/.git" ]]; then
    git clone --depth 1 --branch 2.3.3 https://github.com/zlib-ng/zlib-ng.git "$root/.tools/zlib-ng"
fi
[[ "$(git -C "$root/.tools/zlib-ng" rev-parse HEAD)" == "$zlib_revision" ]] || {
    echo "The local zlib-ng checkout does not match the pinned 2.3.3 revision." >&2; exit 1;
}
cmake -S "$root/.tools/zlib-ng" -B "$root/.tools/zlib-ng-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$root/.tools/macos-deps" \
    -DZLIB_COMPAT=ON -DBUILD_TESTING=OFF -DWITH_GZFILEOP=ON -DWITH_NEW_STRATEGIES=ON \
    -DWITH_NATIVE_INSTRUCTIONS=OFF -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build "$root/.tools/zlib-ng-build" --parallel "${SNOW_BUILD_JOBS:-4}"
cmake --install "$root/.tools/zlib-ng-build"
rustup show active-toolchain
# The shared offline license collector resolves metadata for each workspace,
# including packages that are not compiled by the screenshot preset.
for manifest in snow-crates/Cargo.toml snow_rust_ffi/Cargo.toml; do
    cargo fetch --locked --manifest-path "$manifest" --target aarch64-apple-darwin
done
