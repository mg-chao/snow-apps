#!/bin/bash
# Standalone capture and optional recording build; Qt and OCR are not built.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
arch="${1:-$(uname -m)}"
case "$arch" in
  arm64|aarch64) target=aarch64-apple-darwin ;;
  x86_64|x64) target=x86_64-apple-darwin ;;
  *) echo "usage: $0 [arm64|x86_64] [--recording]" >&2; exit 2 ;;
esac
export MACOSX_DEPLOYMENT_TARGET=15.0
export CARGO_NET_GIT_FETCH_WITH_CLI=true
rustup target add "$target"
packages=(-p snow-capture-c)
if [[ "${2:-}" == --recording ]]; then
  : "${FFMPEG_DIR:?Set FFMPEG_DIR to the matching macOS FFmpeg installation}"
  packages+=(-p snow-recording-c)
fi
cargo build --locked --manifest-path snow-crates/Cargo.toml --target "$target" --release "${packages[@]}"
cargo build --locked --manifest-path snow-crates/Cargo.toml --target "$target" --release -p snow-macos --example media_harness
app="$repo_root/build/macos-media/$target/SnowMediaHarness.app"
mkdir -p "$app/Contents/MacOS"
cp "$repo_root/build/cargo/$target/release/examples/media_harness" "$app/Contents/MacOS/SnowMediaHarness"
cp "$repo_root/snow-crates/native/Info.plist" "$app/Contents/Info.plist"
codesign --force --sign - "$app"
printf 'Built %s\nRun: "%s/Contents/MacOS/SnowMediaHarness" status\n' "$app" "$app"
if [[ "${2:-}" == --recording ]]; then
  cargo build --locked --manifest-path snow-crates/Cargo.toml --target "$target" --release -p snow-recording-runtime --example macos_record --example macos_export
  cargo rustc --locked --manifest-path snow-crates/Cargo.toml --target "$target" --release -p snow-recording-runtime --example macos_record -- -C link-arg=-Wl,-rpath,@executable_path/../Frameworks -C link-arg=-Wl,-headerpad_max_install_names
  recording_app="$repo_root/build/macos-media/$target/SnowRecordingHarness.app"
  mkdir -p "$recording_app/Contents/MacOS"
  cp "$repo_root/build/cargo/$target/release/examples/macos_record" "$recording_app/Contents/MacOS/SnowRecordingHarness"
  cp "$repo_root/snow-crates/native/Info.plist" "$recording_app/Contents/Info.plist"
  /usr/libexec/PlistBuddy -c 'Set :CFBundleExecutable SnowRecordingHarness' "$recording_app/Contents/Info.plist"
  /usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier app.snow.recording-harness' "$recording_app/Contents/Info.plist"
  /usr/libexec/PlistBuddy -c 'Set :CFBundleName Snow Recording Harness' "$recording_app/Contents/Info.plist"
  python3 scripts/package-macos-media-harness.py "$recording_app" "$FFMPEG_DIR"
  printf 'Recording harness: %s\n' "$recording_app"
fi
