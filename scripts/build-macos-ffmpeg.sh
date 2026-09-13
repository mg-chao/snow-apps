#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] || {
    echo "This FFmpeg build requires an Apple Silicon Mac." >&2; exit 1;
}
for tool in brew clang make pkg-config python3 curl shasum; do
    command -v "$tool" >/dev/null || { echo "Required tool not found: $tool" >&2; exit 1; }
done

# Match the FFmpeg 8 API used by ffmpeg-next, including Homebrew ffmpeg@8's
# codecs, and add the WebP encoders required by Snow Shot recording.
# The official archive hash is also published by Homebrew's 8.1.2 formula.
version=8.1.2
source_url="https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
source_sha256=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
prefix="$root/.tools/ffmpeg-$version"
archive="$root/.tools/downloads/ffmpeg-$version.tar.xz"
source_dir="$root/.tools/ffmpeg-source/ffmpeg-$version"
build_dir="$root/.tools/ffmpeg-build-$version"
notices="$prefix/share/snow-apps/ffmpeg"
jobs="${SNOW_BUILD_JOBS:-2}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "SNOW_BUILD_JOBS must be positive." >&2; exit 1; }
if (( jobs > 2 )); then jobs=2; fi

codec_formulae=(dav1d lame libvmaf libvpx openssl@3 opus sdl2-compat svt-av1 x264 x265 webp)
codec_pkg_config=
for formula in "${codec_formulae[@]}"; do
    dependency_prefix="$(brew --prefix "$formula")"
    [[ -d "$dependency_prefix/lib" ]] || {
        echo "Missing $formula; run scripts/bootstrap-macos.sh first." >&2; exit 1;
    }
    codec_pkg_config="${codec_pkg_config:+$codec_pkg_config:}$dependency_prefix/lib/pkgconfig"
done
# Limit automatic package discovery to the declared dependencies. Other formulae
# installed on a developer's Mac must not add unrelated X11 capture libraries.
export PKG_CONFIG_PATH="$codec_pkg_config"
export PKG_CONFIG_LIBDIR="$codec_pkg_config"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
mkdir -p "$root/.tools/downloads" "$root/.tools/ffmpeg-source" "$build_dir"

# Rebuild when the script, compiler, SDK, deployment target, or codec kegs change.
{
    shasum -a 256 "$root/scripts/build-macos-ffmpeg.sh"
    clang --version
    xcrun --show-sdk-path
    xcrun --show-sdk-version
    echo "MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
    brew list --versions "${codec_formulae[@]}"
} > "$build_dir/build-inputs.txt"
if [[ -f "$notices/build-inputs.txt" && -x "$prefix/bin/ffmpeg" ]] &&
    cmp -s "$build_dir/build-inputs.txt" "$notices/build-inputs.txt" &&
    "$prefix/bin/ffmpeg" -hide_banner -encoders 2>/dev/null | grep 'libwebp_anim' >/dev/null; then
    echo "FFmpeg $version with WebP is already built at $prefix"
    exit 0
fi

if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 "$source_url" -o "$archive.part"
    actual="$(shasum -a 256 "$archive.part" | cut -d ' ' -f 1)"
    [[ "$actual" == "$source_sha256" ]] || {
        echo "FFmpeg source checksum mismatch: $archive.part" >&2; exit 1;
    }
    mv "$archive.part" "$archive"
fi
actual="$(shasum -a 256 "$archive" | cut -d ' ' -f 1)"
[[ "$actual" == "$source_sha256" ]] || {
    echo "FFmpeg source checksum mismatch: $archive" >&2; exit 1;
}
tar -xJf "$archive" -C "$root/.tools/ffmpeg-source"

configure_args=(
    "--prefix=$prefix"
    "--extra-cflags=-I$(brew --prefix lame)/include"
    "--extra-ldflags=-L$(brew --prefix lame)/lib"
    --enable-shared --disable-static --disable-debug --disable-doc
    --enable-pthreads --enable-version3 --cc=clang --enable-ffplay --enable-gpl
    --enable-libsvtav1 --enable-libopus --enable-libx264 --enable-libmp3lame
    --enable-libdav1d --enable-libvmaf --enable-libvpx --enable-libx265
    --enable-openssl --enable-videotoolbox --enable-audiotoolbox --enable-neon
    --enable-libwebp
)
cd "$build_dir"
if [[ -f ffbuild/config.mak ]]; then make distclean; fi
"$source_dir/configure" "${configure_args[@]}"
make -j "$jobs"
make install

# Keep corresponding source and the exact build inputs beside the libraries so
# the app's license collector can include them in every distributable package.
mkdir -p "$notices"
cp "$source_dir"/COPYING.* "$source_dir/LICENSE.md" "$archive" "$notices/"
cp "$root/scripts/build-macos-ffmpeg.sh" "$notices/build-macos-ffmpeg.sh"
cp "$build_dir/ffbuild/config.log" "$notices/config.log"
printf '%s\n' "${configure_args[@]}" > "$notices/configure-args.txt"
python3 - "$notices/source-build.json" "$version" "$source_url" "$source_sha256" \
    "${configure_args[@]}" <<'PY'
import json
import pathlib
import sys

destination, version, source_url, source_sha256, *configure_args = sys.argv[1:]
metadata = {
    "name": "FFmpeg",
    "version": version,
    "source_url": source_url,
    "source_sha256": source_sha256,
    "source_archive": f"ffmpeg-{version}.tar.xz",
    "configure_args": configure_args,
    "license": "GPL-3.0-or-later",
    "patches": [],
}
pathlib.Path(destination).write_text(json.dumps(metadata, indent=2) + "\n")
PY
"$prefix/bin/ffmpeg" -hide_banner -encoders > "$build_dir/encoders.txt" 2>&1
grep -q 'libwebp_anim' "$build_dir/encoders.txt" || {
    echo "The local FFmpeg build is missing its animated WebP encoder." >&2; exit 1;
}
cp "$build_dir/build-inputs.txt" "$notices/build-inputs.txt"
echo "Built FFmpeg $version with WebP at $prefix"
