#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"

usage() {
    echo 'Usage: build-static-qt.sh --install-prefix PATH [--arch arm64|x64] [--parallel JOBS]'
    echo '       [--dependency-prefix PATH] [--source-dir PATH] [--build-dir PATH] [--force]'
}

qt_version=6.11.1
qt_deployment_target=14.0
arch="$(snow_default_arch)"
install_prefix=''
dependency_prefix=''
source_dir=''
build_dir=''
parallelism=4
force=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --install-prefix) [[ $# -ge 2 ]] || snow_die '--install-prefix needs a path'; install_prefix="$2"; shift 2 ;;
        --dependency-prefix) [[ $# -ge 2 ]] || snow_die '--dependency-prefix needs a path'; dependency_prefix="$2"; shift 2 ;;
        --source-dir) [[ $# -ge 2 ]] || snow_die '--source-dir needs a path'; source_dir="$2"; shift 2 ;;
        --build-dir) [[ $# -ge 2 ]] || snow_die '--build-dir needs a path'; build_dir="$2"; shift 2 ;;
        --arch) [[ $# -ge 2 ]] || snow_die '--arch needs arm64 or x64'; arch="$2"; shift 2 ;;
        --parallel) [[ $# -ge 2 ]] || snow_die '--parallel needs a positive integer'; parallelism="$2"; shift 2 ;;
        --force) force=1; shift ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
done
[[ -n "$install_prefix" ]] || snow_die '--install-prefix is required'
[[ "$arch" == arm64 || "$arch" == x64 ]] || snow_die '--arch needs arm64 or x64'
[[ "$parallelism" =~ ^[1-9][0-9]*$ ]] || snow_die '--parallel needs a positive integer'
snow_require_macos

case "$arch" in
    arm64) cmake_arch=arm64 ;;
    x64) cmake_arch=x86_64 ;;
esac
triplet="${arch}-osx-snow-shot-static"
if [[ -z "$dependency_prefix" ]]; then
    dependency_prefix="$snow_repo_root/.tools/macos/installed/static/$triplet"
fi

export PATH="$snow_repo_root/.tools/macos-dev/bin:$snow_repo_root/.tools/macos-media/host/bin:$PATH"
for tool in cmake ninja curl tar python3 shasum xcrun; do
    command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
done
export MACOSX_DEPLOYMENT_TARGET="$qt_deployment_target"

# Qt's source configure requires a discoverable full-Xcode version by default,
# even though Apple Command Line Tools provide the compiler and SDK needed by
# this Ninja build. Keep Qt's SDK checks, but skip only its Xcode-version check
# when xcodebuild reports that the selected developer directory is CLT-only.
qt_apple_options=()
if ! xcodebuild -version >/dev/null 2>&1; then
    if ! xcrun --show-sdk-path >/dev/null 2>&1; then
        snow_die 'The selected Apple developer tools do not provide a macOS SDK.'
    fi
    qt_apple_options+=(-DQT_NO_XCODE_MIN_VERSION_CHECK=ON)
    printf 'Full Xcode is unavailable; building Qt with Apple Command Line Tools.\n'
fi

canonical_path() {
    python3 - "$1" <<'PY'
import pathlib, sys
print(pathlib.Path(sys.argv[1]).expanduser().resolve())
PY
}
install_prefix="$(canonical_path "$install_prefix")"
dependency_prefix="$(canonical_path "$dependency_prefix")"
work_root="${TMPDIR:-/tmp}"
[[ -n "$source_dir" ]] || source_dir="$work_root/qt-everywhere-src-$qt_version"
[[ -n "$build_dir" ]] || build_dir="$work_root/qt-build-$qt_version-macos-$arch-static"
source_dir="$(canonical_path "$source_dir")"
build_dir="$(canonical_path "$build_dir")"

python3 - "$snow_repo_root" "$install_prefix" "$source_dir" "$build_dir" <<'PY'
import pathlib, sys
repo, install, source, build = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
paths = {'install': install, 'source': source, 'build': build}
for name, path in paths.items():
    if path == pathlib.Path(path.anchor) or path in (repo, pathlib.Path.home().resolve()):
        raise SystemExit(f'Refusing unsafe {name} directory: {path}')
for left_name, left in paths.items():
    for right_name, right in paths.items():
        if left_name < right_name and (left in right.parents or right in left.parents):
            raise SystemExit(f'Qt {left_name} and {right_name} directories overlap: {left}, {right}')
PY

for required in include/zlib.h include/png.h share/zlib/vcpkg_abi_info.txt share/libpng/vcpkg_abi_info.txt; do
    [[ -f "$dependency_prefix/$required" ]] || snow_die "Static Qt dependency is missing: $dependency_prefix/$required"
done
dependency_fingerprint="$(
    shasum -a 256 "$dependency_prefix/share/zlib/vcpkg_abi_info.txt" \
        "$dependency_prefix/share/libpng/vcpkg_abi_info.txt" | shasum -a 256 | awk '{print $1}'
)"
qt_config="$install_prefix/lib/cmake/Qt6/Qt6Config.cmake"
stamp="$install_prefix/share/snow-apps/static-qt-build.json"
if [[ -f "$qt_config" && "$force" == 0 ]]; then
    if [[ -f "$stamp" ]] &&
        grep -Eq '"SchemaVersion"[[:space:]]*:[[:space:]]*1' "$stamp" &&
        grep -Eq '"QtVersion"[[:space:]]*:[[:space:]]*"6\.11\.1"' "$stamp" &&
        grep -Eq '"Architecture"[[:space:]]*:[[:space:]]*"'"$arch"'"' "$stamp" &&
        grep -Eq '"Configuration"[[:space:]]*:[[:space:]]*"Release"' "$stamp" &&
        grep -Eq '"DeploymentTarget"[[:space:]]*:[[:space:]]*"14\.0"' "$stamp" &&
        grep -Eq '"Ltcg"[[:space:]]*:[[:space:]]*true' "$stamp" &&
        grep -Eq '"SystemPng"[[:space:]]*:[[:space:]]*true' "$stamp" &&
        grep -Eq '"SystemZlib"[[:space:]]*:[[:space:]]*true' "$stamp" &&
        grep -Fq "\"DependencyFingerprint\": \"$dependency_fingerprint\"" "$stamp" &&
        [[ -d "$install_prefix/share/snow-apps/qt-licenses" ]]; then
        printf 'Validated static Qt %s (%s) at %s\n' "$qt_version" "$arch" "$install_prefix"
        exit 0
    fi
    snow_die "The Qt installation at $install_prefix is not the validated build. Use a distinct prefix or pass --force."
fi
if [[ "$force" == 1 ]]; then
    [[ ! -e "$install_prefix" ]] || rm -rf -- "$install_prefix"
    [[ ! -e "$build_dir" ]] || rm -rf -- "$build_dir"
fi

archive="$(dirname "$source_dir")/qt-everywhere-src-$qt_version.tar.xz"
release_series="${qt_version%.*}"
source_path="qt/$release_series/$qt_version/single/qt-everywhere-src-$qt_version.tar.xz"
source_url="https://download.qt.io/official_releases/$source_path"
if [[ ! -d "$source_dir" ]]; then
    if [[ ! -f "$archive" ]]; then
        printf 'Downloading %s\n' "$source_url"
        curl --fail --location --retry 3 --output "$archive.part" "$source_url"
        mv "$archive.part" "$archive"
    fi
    mkdir -p "$(dirname "$source_dir")"
    tar -xf "$archive" -C "$(dirname "$source_dir")"
    [[ -d "$source_dir" ]] || snow_die "Qt source archive did not produce $source_dir"
fi

mkdir -p "$build_dir"
(
    cd "$build_dir"
    "$source_dir/configure" \
        -static -release -ltcg -system-zlib -system-libpng -no-opengl \
        -opensource -confirm-license -prefix "$install_prefix" \
        -submodules qtbase,qtsvg,qttools \
        -skip qtactiveqt -skip qtdeclarative -skip qtimageformats \
        -skip qtlanguageserver -skip qtshadertools \
        -nomake tests -nomake examples -- \
        -DCMAKE_OSX_ARCHITECTURES="$cmake_arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$qt_deployment_target" \
        -DCMAKE_PREFIX_PATH="$dependency_prefix" \
        -DZLIB_ROOT="$dependency_prefix" -DPNG_ROOT="$dependency_prefix" \
        -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON \
        -DQT_FEATURE_concurrent=OFF -DQT_FEATURE_dbus=OFF \
        -DQT_FEATURE_linguist=ON -DQT_FEATURE_printsupport=OFF \
        -DQT_FEATURE_qdoc=OFF -DQT_FEATURE_qmake=OFF -DQT_FEATURE_sql=OFF \
        -DQT_FEATURE_testlib=OFF -DQT_FEATURE_assistant=OFF \
        -DQT_FEATURE_designer=OFF -DQT_FEATURE_pixeltool=OFF \
        -DQT_FEATURE_qdbus=OFF -DQT_FEATURE_qtattributionsscanner=OFF \
        -DQT_FEATURE_qtdiag=OFF -DQT_FEATURE_qtplugininfo=OFF \
        "${qt_apple_options[@]}"
)

cache="$build_dir/CMakeCache.txt"
[[ -f "$cache" ]] || snow_die "Qt configure did not produce $cache"
for entry in \
    'FEATURE_ltcg:BOOL=ON' 'QT_FEATURE_ltcg:INTERNAL=ON' \
    'FEATURE_system_png:BOOL=ON' 'QT_FEATURE_system_png:INTERNAL=ON' \
    'FEATURE_system_zlib:BOOL=ON' 'QT_FEATURE_system_zlib:INTERNAL=ON'; do
    grep -Fqx "$entry" "$cache" || snow_die "Qt configuration is missing $entry"
done
cmake --build "$build_dir" --parallel "$parallelism"
cmake --install "$build_dir"
[[ -f "$qt_config" ]] || snow_die "Qt installation did not produce $qt_config"

license_root="$install_prefix/share/snow-apps/qt-licenses"
mkdir -p "$license_root"
for component in root qtbase qtsvg qttools; do
    component_source="$source_dir"
    [[ "$component" == root ]] || component_source="$source_dir/$component"
    [[ -f "$component_source/REUSE.toml" && -d "$component_source/LICENSES" ]] || snow_die "Qt licensing metadata is incomplete for $component"
    mkdir -p "$license_root/$component"
    cp "$component_source/REUSE.toml" "$license_root/$component/"
    cp -R "$component_source/LICENSES" "$license_root/$component/"
done
mkdir -p "$(dirname "$stamp")"
python3 - "$stamp" "$qt_version" "$arch" "$qt_deployment_target" \
    "$dependency_fingerprint" "$source_url" "$parallelism" <<'PY'
import json, pathlib, sys
path, version, arch, deployment_target, fingerprint, source, parallelism = sys.argv[1:]
value = {
    'SchemaVersion': 1, 'QtVersion': version, 'Architecture': arch,
    'Configuration': 'Release', 'DeploymentTarget': deployment_target,
    'DependencyFingerprint': fingerprint,
    'Ltcg': True, 'SystemPng': True, 'SystemZlib': True,
    'LicenseBundle': 'share/snow-apps/qt-licenses', 'SourceArchive': source,
    'Submodules': ['qtbase', 'qtsvg', 'qttools'], 'Parallelism': int(parallelism),
}
pathlib.Path(path).write_text(json.dumps(value, indent=2) + '\n')
PY
printf 'Static Qt %s (%s) installed at %s\n' "$qt_version" "$arch" "$install_prefix"
