#!/usr/bin/env python3
"""Bundle the provisioned FFmpeg dependency closure and ad-hoc sign a harness.
Only copies dependencies from the explicitly supplied FFmpeg profile.
"""
import pathlib
import shutil
import subprocess
import sys


def package(app, profile):
    app = pathlib.Path(app).resolve()
    profile = pathlib.Path(profile).resolve()
    executable = app / "Contents/MacOS/SnowRecordingHarness"
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    queue = [executable]
    seen = set()
    copied = set()
    while queue:
        binary = queue.pop()
        if binary in seen:
            continue
        seen.add(binary)
        dependencies = subprocess.check_output(["otool", "-L", str(binary)], text=True).splitlines()[1:]
        for line in dependencies:
            name = line.strip().split(" (compatibility", 1)[0]
            if name.startswith(("/System/Library/", "/usr/lib/")):
                continue
            source = profile / "lib" / pathlib.Path(name).name
            if not source.is_file():
                raise SystemExit(f"Unprovisioned dependency: {name}")
            destination = frameworks / source.name
            # A rebuilt profile can keep the same dylib install name while its
            # pixel-depth support or ABI changes. Refresh once per invocation,
            # even when this app was packaged previously.
            if destination not in copied:
                copied.add(destination)
                shutil.copyfile(source, destination)
                destination.chmod(0o755)
                subprocess.run(["install_name_tool", "-id", f"@rpath/{destination.name}", str(destination)], check=True)
            if destination not in seen:
                queue.append(destination)
            desired = f"@rpath/{source.name}"
            if name != desired:
                subprocess.run(["install_name_tool", "-change", name, desired, str(binary)], check=True)
    for binary in sorted(seen):
        subprocess.run(["codesign", "--force", "--sign", "-", str(binary)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: package-macos-media-harness.py APP FFMPEG_PROFILE")
    package(sys.argv[1], sys.argv[2])
