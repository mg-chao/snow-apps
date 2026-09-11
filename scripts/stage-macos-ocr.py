#!/usr/bin/env python3
"""Stage trusted OCR metadata and optional verified models in a macOS app.

Run again after signing nested Mach-O files and before signing the outer bundle:
installation names, architecture thinning and code signing change their hashes.
The native runtime is always bundled; model downloads retain the upstream pins.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent


def descriptor(path):
    return {"name": path.name, "size": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def verified(path, expected):
    return (path.is_file() and not path.is_symlink()
            and descriptor(path) == {key: expected[key] for key in ("name", "size", "sha256")})


def previous_runtime_descriptor(assets):
    try:
        manifest = json.loads((assets / "asset-manifest.json").read_text())
        runtime = manifest["runtime"]
        library = runtime["library"]
        name = library["name"]
        if (manifest["schema"] != 3 or runtime.get("bundled") is not True
                or not isinstance(name, str) or Path(name).name != name
                or not name.startswith("libonnxruntime") or not name.endswith(".dylib")
                or not isinstance(library["size"], int) or library["size"] <= 0
                or not isinstance(library["sha256"], str) or len(library["sha256"]) != 64):
            return None
        return library
    except (OSError, ValueError, KeyError, TypeError):
        return None


def acquire_model_file(expected, cache):
    destination = cache / expected["name"]
    if verified(destination, expected):
        return destination
    url = expected["url"]
    if not url.startswith("https://") or Path(expected["name"]).name != expected["name"]:
        raise ValueError("Invalid model descriptor")
    cache.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={
        "User-Agent": "SnowShot/ocr-assets", "Referer": "https://www.modelscope.cn/"})
    with tempfile.TemporaryDirectory(prefix=".download-", dir=cache) as temporary:
        downloaded = Path(temporary) / expected["name"]
        with urllib.request.urlopen(request, timeout=120) as response, downloaded.open("wb") as output:
            if not response.url.startswith("https://"):
                raise ValueError("Insecure model redirect")
            shutil.copyfileobj(response, output)
        if not verified(downloaded, expected):
            raise ValueError(f"Model size or SHA-256 mismatch: {expected['name']}")
        downloaded.replace(destination)
    return destination


def stage(app, onnx_runtime=None, offline_models=(), cache=ROOT / "artifacts/ocr-macos"):
    manifest = json.loads((ROOT / "snow_shot/packaging/snow-shot-ocr-asset-manifest.json").read_text())
    executable = app / "Contents/MacOS/snow-ocr-process"
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(f"Missing executable OCR helper: {executable}")
    architecture = subprocess.check_output(["lipo", "-archs", str(executable)], text=True).strip()
    if architecture not in ("arm64", "x86_64"):
        raise ValueError(f"Expected a single native OCR architecture, got {architecture}")
    assets = app / "Contents/Resources/assets/ocr"
    previous_runtime = previous_runtime_descriptor(assets)
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    if onnx_runtime is not None:
        source = onnx_runtime.resolve(strict=True)
        library = frameworks / source.name
        # Homebrew files can be read-only. Never reopen an existing copied
        # dylib for writing (or follow a stale symlink back into the Cellar).
        if not verified(library, descriptor(source)):
            with tempfile.TemporaryDirectory(prefix=".ocr-runtime-", dir=frameworks) as temporary:
                staged = Path(temporary) / source.name
                shutil.copy2(source, staged)
                staged.replace(library)
    else:
        declared = frameworks / previous_runtime["name"] if previous_runtime is not None else None
        if declared is not None and declared.is_file() and not declared.is_symlink():
            # macdeployqt may expand SONAME aliases into ordinary files. Retain
            # the staged library identity, but rehash its final signed bytes.
            library = declared
        else:
            candidates = [path for path in frameworks.glob("libonnxruntime*.dylib")
                          if path.is_file() and not path.is_symlink()]
            if len(candidates) != 1:
                raise ValueError(f"Expected one bundled ONNX Runtime library, got {candidates}")
            library = candidates[0]
    manifest["schema"] = 3
    manifest["runtime"] = {
        "version": manifest["runtime"]["version"],
        "platform": "macos-arm64" if architecture == "arm64" else "macos-x64",
        "bundled": True, "files": [descriptor(executable)], "library": descriptor(library),
    }
    assets.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "snow_shot/packaging/ocr-model-LICENSE", assets / "LICENSE")
    requested = set(offline_models)
    for model in manifest["models"]:
        if model["type"] not in requested:
            continue
        requested.remove(model["type"])
        destination = assets / "models" / model["id"]
        destination.mkdir(parents=True, exist_ok=True)
        for expected in model["files"]:
            source = acquire_model_file(expected, cache / "models" / model["id"])
            shutil.copy2(source, destination / expected["name"])
        (destination / ".complete.json").write_text(json.dumps({"schema": 1, "component": model["id"]}) + "\n")
    if requested:
        raise ValueError(f"Unknown OCR models: {sorted(requested)}")
    (assets / "asset-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if onnx_runtime is not None and previous_runtime is not None:
        previous_library = frameworks / previous_runtime["name"]
        # Only retire the exact bytes previously staged by this script. An unrelated or
        # modified dylib, including a symlink, is never removed by a runtime upgrade.
        if previous_library != library and verified(previous_library, previous_runtime):
            previous_library.unlink()
    print(f"Staged {manifest['runtime']['platform']} OCR manifest in {assets}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--onnx-runtime", type=Path,
                        help="Copy this native dylib into Frameworks (development builds)")
    parser.add_argument("--offline-model", action="append", default=[],
                        help="Include a verified model, e.g. small; otherwise download on first use")
    parser.add_argument("--cache", type=Path, default=ROOT / "artifacts/ocr-macos")
    args = parser.parse_args()
    stage(args.app.resolve(), args.onnx_runtime, args.offline_model, args.cache)


if __name__ == "__main__":
    main()
