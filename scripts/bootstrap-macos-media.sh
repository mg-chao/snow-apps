#!/bin/bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
case "${1:-$(uname -m)}" in
  arm64|aarch64) triplet=arm64-osx-snow-media ;;
  x64|x86_64) triplet=x64-osx-snow-media ;;
  *) echo "usage: $0 [arm64|x86_64]" >&2; exit 2 ;;
esac
# vcpkg needs a host pkg-config before it can build even its first library.
# Bootstrap a pinned copy locally when the host does not provide one.
media_tools="$repo_root/.tools/macos-media"
if ! command -v pkg-config >/dev/null 2>&1; then
  if [[ ! -x "$media_tools/host/bin/pkg-config" ]]; then
    mkdir -p "$media_tools/sources"
    archive="$media_tools/sources/pkgconf-2.5.1.tar.xz"
    curl --fail --location --retry 3 https://distfiles.ariadne.space/pkgconf/pkgconf-2.5.1.tar.xz -o "$archive"
    printf '%s  %s\n' e654c3a460e5f0f801e8ac43ad9086f397d1da0553186ff05f5f0e18ffdac99fb652fd9b6c0379db4bc8307699699d69bc66d13cc85a4a6b0cd36462f5948a1d "$archive" | shasum -a 512 -c -
    tar -xf "$archive" -C "$media_tools/sources"
    (cd "$media_tools/sources/pkgconf-2.5.1"
     ./configure --prefix="$media_tools/host" --disable-shared
     make -j "$(sysctl -n hw.logicalcpu)"
     make install)
    ln -sf pkgconf "$media_tools/host/bin/pkg-config"
  fi
  export PATH="$media_tools/host/bin:$PATH"
fi
# Intel codec assembly uses NASM even when cross-compiling from Apple Silicon.
if [[ "$triplet" == x64-* ]] && ! command -v nasm >/dev/null 2>&1; then
  if [[ ! -x "$media_tools/host/bin/nasm" ]]; then
    archive="$media_tools/sources/nasm-2.16.03.tar.xz"
    curl --fail --location --retry 3 https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz -o "$archive"
    printf '%s  %s\n' 0c706e41a9c33e1ac3bad5056e8bf8cbcd51785b551a6e34ce7d0d723df8eaab8603a033e89b3dcda1004b558f9e9ef3196691500f10d8201bf47a323a516f84 "$archive" | shasum -a 512 -c -
    tar -xf "$archive" -C "$media_tools/sources"
    (cd "$media_tools/sources/nasm-2.16.03"
     ./configure --prefix="$media_tools/host"
     make -j "$(sysctl -n hw.logicalcpu)"
     make install)
  fi
  export PATH="$media_tools/host/bin:$PATH"
fi
vcpkg_root="$repo_root/.tools/vcpkg"
if [[ ! -f "$vcpkg_root/bootstrap-vcpkg.sh" ]]; then
  git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
  git -C "$vcpkg_root" checkout 4497409a47f19db373a410a0efb84eca4747adbf
fi
if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
  "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi
MACOSX_DEPLOYMENT_TARGET=15.0 "$vcpkg_root/vcpkg" install --triplet "$triplet" \
  --x-feature=macos-media --x-no-default-features \
  --overlay-ports="$repo_root/cmake/vcpkg-overlay-ports" \
  --overlay-triplets="$repo_root/cmake/vcpkg-overlay-triplets" \
  --x-install-root="$repo_root/.tools/macos-media/installed/$triplet"
printf 'export FFMPEG_DIR="%s/.tools/macos-media/installed/%s/%s"\n' "$repo_root" "$triplet" "$triplet"
