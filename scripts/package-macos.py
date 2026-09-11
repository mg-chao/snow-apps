#!/usr/bin/env python3
"""Bundle a native Snow Shot build and audit it before creating an ad-hoc signed DMG."""

import argparse
from functools import lru_cache
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
QT = ROOT / ".tools/qt/6.11.1/macos"
MACHO_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"}


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def macho_files(directory):
    for path in sorted(directory.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) in MACHO_MAGIC:
                    yield path


@lru_cache(maxsize=None)
def inspect_macho(path, option, modified):
    # Dependency graphs contain many shared edges. Invalidate after bundle edits.
    return run("otool", "-arch", "arm64", option, path)


def load_commands(path, option="-l"):
    return inspect_macho(path, option, path.stat().st_mtime_ns)


def dependencies(path):
    return [line.strip().split(" (compatibility version", 1)[0]
            for line in load_commands(path, "-L").splitlines()[1:]]


def system_library(name):
    return name.startswith(("/System/Library/", "/usr/lib/"))


def rpaths(path):
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset",
                      load_commands(path))


def resolve_library(name, loader, executable, frameworks):
    def expand(value):
        return value.replace("@loader_path", str(loader.parent)).replace(
            "@executable_path", str(executable.parent))

    if name.startswith("@rpath/"):
        roots = [frameworks, QT / "lib"]
        roots += [Path(expand(value)) for value in rpaths(loader) + rpaths(executable)]
        candidates = [directory / name[len("@rpath/"):] for directory in roots]
    else:
        candidates = [Path(expand(name))]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(f"Cannot resolve {name} referenced by {loader}")


def native_closure(executables, frameworks):
    pending = list(executables)
    visited = set()
    while pending:
        loader = pending.pop().resolve()
        if loader in visited:
            continue
        visited.add(loader)
        for name in dependencies(loader):
            if not system_library(name):
                pending.append(resolve_library(name, loader, executables[0], frameworks))
    return visited


def collect_local_ffmpeg_notices(prefix, destination):
    notices = prefix / "share/snow-apps/ffmpeg"
    manifest = notices / "source-build.json"
    if not manifest.is_file():
        raise RuntimeError(f"Local FFmpeg has no source and license metadata: {manifest}")
    metadata = json.loads(manifest.read_text())
    version = metadata.get("version", "")
    archive_name = metadata.get("source_archive", "")
    configuration = metadata.get("configure_args", [])
    if (not isinstance(version, str) or not re.fullmatch(r"[A-Za-z0-9._+-]+", version)
            or not isinstance(archive_name, str) or not archive_name
            or Path(archive_name).name != archive_name or "\\" in archive_name
            or not isinstance(configuration, list) or not configuration
            or not all(isinstance(option, str) for option in configuration)
            or not isinstance(metadata.get("source_url"), str)
            or not metadata["source_url"].startswith("https://")
            or not isinstance(metadata.get("patches"), list)):
        raise RuntimeError(f"Incomplete local FFmpeg source metadata: {manifest}")
    archive = notices / archive_name
    if (not archive.is_file()
            or hashlib.sha256(archive.read_bytes()).hexdigest() != metadata.get("source_sha256")):
        raise RuntimeError(f"Local FFmpeg source archive failed SHA-256 verification: {archive}")
    if "--enable-nonfree" in configuration:
        raise RuntimeError("Cannot redistribute an FFmpeg build configured with --enable-nonfree")
    gpl = "--enable-gpl" in configuration
    version3 = "--enable-version3" in configuration
    license_name = ("GPL-3.0-or-later" if version3 else "GPL-2.0-or-later") if gpl else (
        "LGPL-3.0-or-later" if version3 else "LGPL-2.1-or-later")
    license_file = ("COPYING.GPLv3" if version3 else "COPYING.GPLv2") if gpl else (
        "COPYING.LGPLv3" if version3 else "COPYING.LGPLv2.1")
    if metadata.get("license") != license_name or not (notices / license_file).is_file():
        raise RuntimeError(f"Local FFmpeg license does not match its build configuration: {manifest}")
    for patch in metadata["patches"]:
        if (not isinstance(patch, str) or Path(patch).name != patch or "\\" in patch
                or not (notices / patch).is_file()):
            raise RuntimeError(f"Local FFmpeg is missing its recorded source patch: {patch}")
    # Keep the corresponding source archive and build instructions alongside the
    # license texts; the shared native collector preserves every file verbatim.
    shutil.copytree(notices, destination / ("ffmpeg-" + version))


def collect_native_notices(libraries, destination):
    cellar = Path(run("brew", "--cellar")).resolve()
    packages = set()
    local_ffmpeg = set()
    for library in libraries:
        if library.is_relative_to(cellar):
            parts = library.relative_to(cellar).parts
            packages.add(cellar / parts[0] / parts[1])
        elif re.fullmatch(r"lib(?:avcodec|avdevice|avfilter|avformat|avutil|swresample|swscale)"
                          r"(?:\.[0-9]+)*\.dylib", library.name):
            local_ffmpeg.add(library.parent.parent)
    for prefix in sorted(local_ffmpeg):
        collect_local_ffmpeg_notices(prefix, destination)
    for package in sorted(packages):
        output = destination / (package.parent.name + "-" + package.name)
        output.mkdir(parents=True)
        notices = [path for path in package.iterdir() if path.is_file() and re.match(
            r"(?i)^(LICENSE|LICENCE|COPYING|NOTICE|COPYRIGHT|AUTHORS)([._-].*)?$", path.name)]
        if not notices:
            raise RuntimeError(f"No license notices in {package}")
        for notice in notices:
            shutil.copy2(notice, output / notice.name)
        for name in ("INSTALL_RECEIPT.json", "sbom.spdx.json"):
            if (package / name).is_file():
                shutil.copy2(package / name, output / name)
    zlib = destination / "zlib-ng-2.3.3"
    zlib.mkdir(parents=True)
    shutil.copy2(ROOT / ".tools/zlib-ng/LICENSE.md", zlib / "LICENSE.md")


def collect_licenses(app, libraries, staging, pwsh):
    license_root = app / "Contents/Resources/licenses"
    license_root.mkdir(parents=True, exist_ok=True)
    for component in ("snow_shot", "snow_image", "ant_design_qt", "snow-crates",
                      "snow_draw_engine_qt", "snow_rust_ffi"):
        output = license_root / "components" / component
        output.mkdir(parents=True)
        for name in ("LICENSE", "COPYRIGHT", "THIRD_PARTY_NOTICES.md"):
            if (ROOT / component / name).is_file():
                shutil.copy2(ROOT / component / name, output / name)
    native = staging / "native-licenses"
    collect_native_notices(libraries, native)
    # Use the shared collector so the selected Cargo feature graph and fallback
    # license policy remain identical to Windows packaging.
    def literal(value):
        return "'" + str(value).replace("'", "''") + "'"

    ffi = ROOT / "snow_rust_ffi/Cargo.toml"
    ocr = ROOT / "snow-crates/crates/snow-ocr-process/Cargo.toml"
    command = (
        f"& {literal(ROOT / 'scripts/collect-third-party-licenses.ps1')} "
        f"-Destination {literal(license_root / 'third-party')} "
        f"-AllowedRoot {literal(staging)} -NativeLicenseRoot {literal(native)} "
        f"-QtPrefix {literal(QT)} -CargoManifest @({literal(ffi)}, {literal(ocr)}) "
        f"-CargoOptions @{{{literal(ocr)} = @('--no-default-features', '--features', 'dynamic-onnx-runtime')}} "
        f"-CargoTarget aarch64-apple-darwin "
        f"-AntDesignNotice {literal(ROOT / 'ant_design_qt/THIRD_PARTY_NOTICES.md')} "
        f"-FallbackLicenseDirectory {literal(ROOT / 'licenses')}"
    )
    print(run(pwsh, "-NoProfile", "-Command", command))


def deploy_and_audit(app, originals):
    executable = app / "Contents/MacOS/snow_shot"
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)
    # macdeployqt handles Qt plugins, framework resources and qt.conf.
    print(run(QT / "bin/macdeployqt", app, "-always-overwrite", "-no-strip", "-no-codesign",
              "-executable=" + str(app / "Contents/MacOS/snow-ocr-process")))
    # --update-probe uses Qt's offscreen platform even on macOS. macdeployqt
    # deploys only cocoa by default; keep the built-in startup probe usable.
    shutil.copy2(QT / "plugins/platforms/libqoffscreen.dylib",
                 app / "Contents/PlugIns/platforms/libqoffscreen.dylib")
    for source in originals:
        if source.suffix == ".dylib" and not source.is_relative_to(QT):
            destination = frameworks / source.name
            if not destination.exists():
                shutil.copy2(source, destination)
    minimum = (14, 0)
    for binary in list(macho_files(app)):
        architectures = run("lipo", "-archs", binary).split()
        if "arm64" not in architectures:
            raise RuntimeError(f"Missing ARM64 slice: {binary}")
        if len(architectures) > 1:
            temporary = binary.with_name(binary.name + ".arm64")
            run("lipo", binary, "-thin", "arm64", "-output", temporary)
            temporary.replace(binary)
        commands = load_commands(binary)
        for version in re.findall(r"\bminos (\d+(?:\.\d+)+)", commands):
            minimum = max(minimum, tuple(int(part) for part in version.split(".")))
        for name in dependencies(binary):
            if system_library(name):
                continue
            if ".framework/" in name:
                suffix = name.split(".framework/", 1)
                relative = Path(suffix[0]).name + ".framework/" + suffix[1]
            else:
                relative = Path(name).name
                if not (frameworks / relative).exists():
                    resolved = resolve_library(name, binary, executable, frameworks)
                    relative = resolved.name
            target = frameworks / relative
            if not target.is_file():
                raise RuntimeError(f"Unbundled dependency {name} in {binary}")
            rewritten = "@rpath/" + relative
            if name != rewritten:
                run("install_name_tool", "-change", name, rewritten, binary)
        if binary.suffix == ".dylib":
            run("install_name_tool", "-id", "@rpath/" + binary.name, binary)
        # All Mach-O images get a location-relative search path, including helpers
        # and plugins. Remove development-machine search paths from the bundle.
        desired = "@loader_path/" + os.path.relpath(frameworks, binary.parent)
        for value in rpaths(binary):
            if value.startswith("/"):
                run("install_name_tool", "-delete_rpath", value, binary)
        if desired not in rpaths(binary):
            run("install_name_tool", "-add_rpath", desired, binary)
    # Resolve every load command again with only bundle-local rpaths available.
    for binary in macho_files(app):
        for name in dependencies(binary):
            if not system_library(name):
                resolved = resolve_library(name, binary, executable, frameworks)
                if not resolved.is_relative_to(app):
                    raise RuntimeError(f"External dependency remains: {binary}: {resolved}")
    return ".".join(str(part) for part in minimum)


def verify_startup(app, version):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("DYLD_", "QT_", "QML_", "QML2_"))}
    environment["DYLD_PRINT_LIBRARIES"] = "1"
    result = subprocess.run(
        [str(app / "Contents/MacOS/snow_shot"), "--update-probe", version],
        cwd=app.parent, env=environment, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        raise RuntimeError(f"Bundled startup probe failed ({result.returncode}):\n{result.stderr}")
    for line in result.stderr.splitlines():
        if not line.startswith("dyld[") or "> /" not in line:
            continue
        loaded = line.split("> ", 1)[1]
        if not system_library(loaded) and not Path(loaded).is_relative_to(app):
            raise RuntimeError(f"Startup loaded an external library: {loaded}")
    print("Bundled startup probe passed with isolated Qt and dyld search paths")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/macos-arm64")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/macos-arm64/package")
    parser.add_argument("--pwsh", default=shutil.which("pwsh") or str(ROOT / ".tools/powershell/pwsh"))
    args = parser.parse_args()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = args.build_dir.resolve() / "snow_shot/snow_shot.app"
    if not source.is_dir():
        raise RuntimeError("Build snow_shot with scripts/build-macos.sh first")
    version = re.search(r'set\(SNOW_SHOT_VERSION "([^"]+)"\)',
                        (ROOT / "CMakeLists.txt").read_text()).group(1)
    output = args.output_dir / f"Snow-Shot-{version}-macos-arm64.dmg"
    if output.exists():
        raise RuntimeError(f"Output already exists: {output}; choose a fresh output directory")
    with tempfile.TemporaryDirectory(prefix="snow-macos-", dir=args.output_dir) as temporary:
        staging = Path(temporary)
        volume = staging / "volume"
        volume.mkdir()
        app = volume / "Snow Shot.app"
        shutil.copytree(source, app, symlinks=True)
        executable = app / "Contents/MacOS/snow_shot"
        helper = app / "Contents/MacOS/snow-ocr-process"
        ort = Path(run("brew", "--prefix", "onnxruntime")) / "lib/libonnxruntime.dylib"
        libraries = native_closure([executable, helper, ort], app / "Contents/Frameworks")
        collect_licenses(app, libraries, staging, args.pwsh)
        minimum = deploy_and_audit(app, libraries)
        info_path = app / "Contents/Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        info.update(CFBundleName="Snow Shot", CFBundleDisplayName="Snow Shot",
                    CFBundleShortVersionString=version.split("-")[0],
                    CFBundleVersion=version.split("-")[0], LSMinimumSystemVersion=minimum,
                    NSHighResolutionCapable=True)
        info_path.write_bytes(plistlib.dumps(info))
        provenance = {
            "version": version, "architecture": "arm64", "minimum_macos": minimum,
            "revision": run("git", "-C", ROOT, "rev-parse", "HEAD"),
            "source_modified": bool(run("git", "-C", ROOT, "status", "--porcelain")),
            "qt_version": "6.11.1", "signing": "ad-hoc, not notarized",
        }
        (app / "Contents/Resources/build-info.json").write_text(json.dumps(provenance, indent=2) + "\n")
        for binary in reversed(list(macho_files(app))):
            if binary == executable:
                continue  # Signing the bundle below also signs its main executable.
            run("codesign", "--force", "--sign", "-", "--timestamp=none", binary)
        run("codesign", "--force", "--deep", "--sign", "-", "--timestamp=none", app)
        # The manifest pins the final signed helper and ONNX library. Seal nested code first,
        # then sign only the outer bundle so these hashes cannot be changed by deep signing.
        run("python3", ROOT / "scripts/stage-macos-ocr.py", "--app", app,
            "--offline-model", "small")
        run("codesign", "--force", "--sign", "-", "--timestamp=none", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        verify_startup(app, version)
        (volume / "Applications").symlink_to("/Applications")
        run("hdiutil", "create", "-volname", "Snow Shot", "-srcfolder", volume,
            "-format", "UDZO", output)
    hasher = hashlib.sha256()
    with output.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    digest = hasher.hexdigest()
    output.with_suffix(".dmg.sha256").write_text(f"{digest}  {output.name}\n")
    print(f"Created {output}\nMinimum macOS: {minimum}\nSHA-256: {digest}")


if __name__ == "__main__":
    main()
