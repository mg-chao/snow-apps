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
import re
import subprocess
import sys
import time


def digest(path):
    value = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def validate_plan(plan):
    versions = plan.get("versions", {})
    if not isinstance(versions, dict) or len(versions) < 2:
        raise ValueError("The plan must contain at least two versions")
    names = set()
    for label, version in versions.items():
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", label) or label in (".", ".."):
            raise ValueError(f"Unsafe version name: {label}")
        if not version.get("revision"):
            raise ValueError(f"Missing revision for {label}")
        if version.get("cwd") and not Path(version["cwd"]).is_dir():
            raise ValueError(f"Missing working directory for {label}")
    if not plan.get("jobs"):
        raise ValueError("The plan must contain at least one job")
    for job in plan["jobs"]:
        name = job.get("name", "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name) or name in (".", ".."):
            raise ValueError(f"Unsafe job name: {name}")
        if name in names:
            raise ValueError(f"Duplicate job: {name}")
        names.add(name)
        if set(job.get("commands", {})) != set(versions):
            raise ValueError(f"{name} must define a command for every version")
        arguments = []
        for label, command in job["commands"].items():
            if (not isinstance(command, list) or not command or
                    any(not isinstance(argument, str) for argument in command)):
                raise ValueError(f"Invalid command for {name}/{label}")
            executable = Path(command[0])
            if not executable.is_absolute() or not executable.is_file():
                raise ValueError(f"Missing absolute executable: {executable}")
            for argument in command:
                try:
                    argument.format(run_dir="RUN", round=1, version="VERSION")
                except (KeyError, ValueError) as error:
                    raise ValueError(f"Invalid command template for {name}/{label}: {error}") from error
            arguments.append(command[1:])
        if any(arguments[0] != argument for argument in arguments[1:]) and not job.get(
                "argument_difference_reason"):
            raise ValueError(f"{name} has different workload arguments; provide "
                             "argument_difference_reason for an intentional implementation switch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--only", help="Comma-separated job names")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    try:
        validate_plan(plan)
    except ValueError as error:
        parser.error(str(error))
    labels = list(plan["versions"])
    jobs = plan["jobs"]
    if args.only:
        selected = {name.strip() for name in args.only.split(",")}
        jobs = [job for job in jobs if job["name"] in selected]
        if {job["name"] for job in jobs} != selected:
            parser.error("--only includes an unknown job")
    # The manifest describes the selected campaign, so a later completeness
    # check does not expect jobs excluded by --only.
    plan = dict(plan, jobs=jobs)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Output already contains a run; choose a fresh directory")
    binaries = {}
    for job in jobs:
        for command in job["commands"].values():
            executable = Path(command[0]).resolve()
            if not executable.is_file():
                parser.error(f"Missing executable: {executable}")
            binaries[str(executable)] = digest(executable)
    metadata = {
        "schema_version": 2,
        "completed": False,
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "rounds": args.rounds,
        "plan": plan,
        "binary_sha256": binaries,
        "runner_sha256": digest(__file__),
        "protocol": "Serial fresh processes; adjacent before/after jobs; alternating version order; no build step",
        "peak_rss": "macOS time -l bytes" if sys.platform == "darwin" else "unavailable",
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    with (output / "manifest.jsonl").open("w", encoding="utf-8") as manifest:
        for round_index in range(args.rounds):
            order = labels if round_index % 2 == 0 else list(reversed(labels))
            for job in jobs:
                for label in order:
                    version = plan["versions"][label]
                    run_dir = output / "runs" / f"round-{round_index + 1}" / label / job["name"]
                    run_dir.mkdir(parents=True, exist_ok=True)
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
                    failure = None
                    with (run_dir / (job["name"] + ".stdout")).open("w", encoding="utf-8") as stdout, \
                         (run_dir / (job["name"] + ".stderr")).open("w", encoding="utf-8") as stderr:
                        try:
                            result = subprocess.run(command, cwd=version.get("cwd"), env=env,
                                                    stdout=stdout, stderr=stderr, check=False)
                            exit_code = result.returncode
                        except OSError as error:
                            failure = str(error)
                            exit_code = 127
                            stderr.write(failure + "\n")
                    record = {"round": round_index + 1, "version": label,
                              "workload": job["name"], "command": command,
                              "artifact_directory": str(run_dir.relative_to(output)),
                              "exit_code": exit_code,
                              "wall_seconds": time.monotonic() - started}
                    if failure:
                        record["error"] = failure
                    manifest.write(json.dumps(record) + "\n")
                    manifest.flush()
                    if exit_code:
                        raise SystemExit(f"{label} {job['name']} failed; see {run_dir}")
    for path, expected in binaries.items():
        if digest(path) != expected:
            raise SystemExit(f"Executable changed during measurement: {path}")
    metadata["completed"] = True
    metadata["completed_timestamp_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"Saved measurements and commands to {output}")


if __name__ == "__main__":
    main()
