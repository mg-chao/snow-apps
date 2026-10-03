#!/usr/bin/env python3
"""Run an explicit before/after benchmark plan serially, preserving provenance.

The plan is JSON: {"versions": {label: {"revision": str, "cwd": str,
"env": {str: str}}}, "jobs": [{"name": str, "commands": {label: [str]}}]}.
Command arguments may contain {run_dir}, {round}, and {version}. Use absolute
executable paths. Build with the platform performance preset before invoking
this runner; it deliberately does not configure, build, or run a test suite.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


def digest(path):
    value = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--only", help="Comma-separated job names")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    plan = json.loads(args.plan.read_text())
    labels = list(plan["versions"])
    if len(labels) < 2:
        parser.error("The plan must contain at least two versions")
    jobs = plan["jobs"]
    if args.only:
        selected = set(args.only.split(","))
        jobs = [job for job in jobs if job["name"] in selected]
        if {job["name"] for job in jobs} != selected:
            parser.error("--only includes an unknown job")
    # The manifest describes the selected campaign, so a later completeness
    # check does not expect jobs excluded by --only.
    plan = dict(plan, jobs=jobs)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "manifest.jsonl").exists():
        parser.error("Output already contains a run; choose a fresh directory")
    binaries = {}
    for job in jobs:
        for label, command in job["commands"].items():
            if label not in labels:
                parser.error(f"Unknown version {label}")
            executable = Path(command[0]).resolve()
            if not executable.is_file():
                parser.error(f"Missing executable: {executable}")
            binaries[str(executable)] = digest(executable)
    metadata = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "rounds": args.rounds,
        "plan": plan,
        "binary_sha256": binaries,
        "runner_sha256": digest(__file__),
        "protocol": "Serial fresh processes; alternating version order; no build step",
        "peak_rss": "macOS time -l bytes" if sys.platform == "darwin" else "unavailable",
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    with (output / "manifest.jsonl").open("w") as manifest:
        for round_index in range(args.rounds):
            order = labels if round_index % 2 == 0 else list(reversed(labels))
            for label in order:
                version = plan["versions"][label]
                run_dir = output / "runs" / f"round-{round_index + 1}" / label
                run_dir.mkdir(parents=True, exist_ok=True)
                for job in jobs:
                    if label not in job["commands"]:
                        continue
                    variables = {"run_dir": str(run_dir), "round": round_index + 1,
                                 "version": label}
                    command = [arg.format(**variables) for arg in job["commands"][label]]
                    env = dict(os.environ, **plan.get("env", {}), **version.get("env", {}))
                    # macOS time strips DYLD_* at startup. Set these after time
                    # launches env, so FFmpeg-linked Rust binaries use the same libs.
                    if sys.platform == "darwin":
                        assignments = [f"{key}={value}" for key, value in env.items()
                                       if key.startswith("DYLD_")]
                        command = ["/usr/bin/time", "-l", "/usr/bin/env", *assignments, *command]
                    print(f"round {round_index + 1}: {label} {job['name']}", flush=True)
                    started = time.monotonic()
                    with (run_dir / (job["name"] + ".stdout")).open("w") as stdout, \
                         (run_dir / (job["name"] + ".stderr")).open("w") as stderr:
                        result = subprocess.run(command, cwd=version.get("cwd"), env=env,
                                                stdout=stdout, stderr=stderr, check=False)
                    record = {"round": round_index + 1, "version": label,
                              "workload": job["name"], "command": command,
                              "exit_code": result.returncode,
                              "wall_seconds": time.monotonic() - started}
                    manifest.write(json.dumps(record) + "\n")
                    manifest.flush()
                    if result.returncode:
                        raise SystemExit(f"{label} {job['name']} failed; see {run_dir}")
    for path, expected in binaries.items():
        if digest(path) != expected:
            raise SystemExit(f"Executable changed during measurement: {path}")
    print(f"Saved measurements and commands to {output}")


if __name__ == "__main__":
    main()
