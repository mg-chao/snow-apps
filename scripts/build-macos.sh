#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
[[ "$(uname -s)" == Darwin ]] || { echo "This build requires macOS." >&2; exit 1; }
export SNOW_MACOS_BREW_PREFIX="$(brew --prefix)"
export SNOW_MACOS_FFMPEG_PREFIX="$root/.tools/ffmpeg-8.1.2"
export FFMPEG_DIR="$SNOW_MACOS_FFMPEG_PREFIX"
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-2}"
export PKG_CONFIG_PATH="$SNOW_MACOS_FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ -f "$SNOW_MACOS_FFMPEG_PREFIX/share/snow-apps/ffmpeg/source-build.json" && \
    -f "$SNOW_MACOS_FFMPEG_PREFIX/lib/libavcodec.dylib" ]] || {
    echo "Run scripts/bootstrap-macos.sh to build FFmpeg with WebP support." >&2; exit 1;
}
[[ -x "$root/.tools/qt/6.11.2/macos/bin/qmake" ]] || {
    echo "Run scripts/bootstrap-macos.sh first." >&2; exit 1;
}
cmake --preset macos-arm64 "$@"
cmake --build --preset build-macos-arm64 --parallel "${SNOW_BUILD_JOBS:-2}"
