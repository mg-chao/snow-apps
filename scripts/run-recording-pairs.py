"""Run frozen Release recording executables in independent alternating pairs.

Example (from the repository root):
  python scripts/run-recording-pairs.py --baseline build/.../baseline.exe \
    --candidate build/.../candidate.exe --output build/recording-throughput/comparison \
    --config build/recording-throughput/options.json

The optional JSON contains common_args, baseline_args and candidate_args lists.
Duration, warmup, sample count and output location are owned by this runner.
No build occurs here. A compiler/build/test process invalidates a recording;
the complete interrupted experiment is retained for inspection.
"""
import argparse
import csv
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def completed_pair(output, index):
    return all((output / f"{name}-{index}" / "exit-code.txt").is_file()
               and (output / f"{name}-{index}" / "exit-code.txt").read_text().strip() == "0"
               and (output / f"{name}-{index}" / "realtime-recording-benchmark.csv").is_file()
               and not (output / f"{name}-{index}" / "measurement-invalid.json").exists()
               for name in ("baseline", "candidate"))


def preserve_incomplete_pair(output, index):
    """Archive both members before repeating an invalid/incomplete pair."""
    paths = [output / f"{name}-{index}" for name in ("baseline", "candidate")]
    paths = [path for path in paths if path.exists()]
    if not paths:
        return
    destination = output / "invalid-attempts" / f"pair-{index}-{time.time_ns()}"
    # Validate resolved paths before any directory move on Windows.
    if not destination.resolve().is_relative_to(output.resolve()) or any(
            not path.resolve().is_relative_to(output.resolve()) for path in paths):
        raise RuntimeError("Archive paths escaped the benchmark output directory")
    destination.mkdir(parents=True)
    for path in paths:
        path.rename(destination / path.name)
    print("Preserved incomplete pair in", destination, flush=True)


def competing_processes():
    if os.name != "nt":
        raise RuntimeError("The realtime recording benchmark requires Windows")

    class Process(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                    ("pid", wintypes.DWORD), ("heap", ctypes.c_size_t),
                    ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG),
                    ("flags", wintypes.DWORD), ("name", wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Process)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Process)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    entry = Process()
    entry.size = ctypes.sizeof(entry)
    found = []
    try:
        available = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while available:
            name = entry.name.lower()
            if name in {"cargo.exe", "rustc.exe", "cl.exe", "link.exe", "cmake.exe",
                        "ctest.exe", "ninja.exe"} or name.endswith("-tests.exe"):
                found.append({"pid": entry.pid, "name": name})
            available = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return found


def recording_interference(directory, observations):
    """Protect all work up to final encoder drain, including warmup and setup.

    Older harnesses and incomplete/invalid boundary files fail closed. Keep all
    process observations separately, including those during offline inspection.
    """
    boundaries = list(directory.glob("*-sample-*.recording-window.csv"))
    if len(boundaries) != 1:
        return observations
    try:
        with boundaries[0].open() as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != 1:
            return observations
        start = float(rows[0]["started_unix_seconds"])
        stop = float(rows[0]["stopped_unix_seconds"])
        if not 0 < start <= stop < float("inf"):
            return observations
    except (OSError, KeyError, ValueError):
        return observations
    # Polls are one second apart. A process first observed immediately after
    # drain could have started before it; conservatively retain that overlap.
    return [event for event in observations if event["unix_time"] <= stop + 1.0]


def record_competing_work(observations, competitors, on_competing=None):
    """Record interference before an optional session-authorized stop action.

    A separate watcher can kill a build between the runner's polls. Keeping
    observation and action together ensures that a stopped build still
    invalidates the protected recording window, using its observation time.
    """
    if not competitors:
        return
    event = {"unix_time": time.time(), "processes": competitors}
    observations.append(event)
    if on_competing is not None:
        try:
            on_competing(competitors)
        except Exception as error:
            # Preserve the recording and its invalidity even if the optional
            # action fails. A callback must not abandon a running benchmark.
            event["interference_handler_error"] = str(error)


def main(on_competing=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--resume", action="store_true",
                        help="retain valid complete pairs and repeat any incomplete pair")
    options = parser.parse_args()
    config = json.loads(options.config.read_text(encoding="utf-8-sig")) if options.config else {}
    common = config.get("common_args", ["--scenario", "all-effects", "--fps", "60",
                                       "--clarity", "1080p", "--workload", "continuous"])
    variants = {name: config.get(f"{name}_args", []) for name in ("baseline", "candidate")}
    reserved = {"--duration-seconds", "--warmup-seconds", "--samples", "--output", "--allow-debug"}
    for arguments in [common, *variants.values()]:
        if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
            parser.error("Arguments must be JSON arrays of strings")
        if any(arg.split("=", 1)[0] in reserved for arg in arguments):
            parser.error("The pair runner owns duration, warmup, samples, and output arguments")
    if "--scenario" not in common:
        parser.error("common_args must select exactly one --scenario")
    root = Path(__file__).resolve().parents[1]
    output = options.output.resolve()
    if not output.is_relative_to(root / "build"):
        parser.error("Results must be preserved under the repository's ignored build directory")
    executables = {"baseline": options.baseline.resolve(), "candidate": options.candidate.resolve()}
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in executables.items()}
    output.mkdir(parents=True, exist_ok=options.resume)
    environment = os.environ.copy()
    environment["PATH"] = str(root / ".tools/vcpkg/installed/dynamic/x64-windows/bin") + os.pathsep + environment["PATH"]
    protocol = {"schema_version": 1,
        "pairs": 10, "warmup_seconds": 5, "measurement_seconds": 30,
        "configuration": config, "executable_sha256": hashes}
    if options.resume:
        if json.loads((output / "protocol.json").read_text()) != protocol:
            parser.error("Resume requires identical executable hashes and configuration")
    else:
        (output / "protocol.json").write_text(json.dumps(protocol, indent=2) + "\n")
    for index in range(1, 11):
        if options.resume and completed_pair(output, index):
            continue
        if options.resume:
            preserve_incomplete_pair(output, index)
        order = ("baseline", "candidate") if index % 2 else ("candidate", "baseline")
        for name in order:
            competitors = competing_processes()
            if competitors:
                print("Waiting for competing work:", competitors, flush=True)
            while competitors:
                record_competing_work([], competitors, on_competing)
                time.sleep(1)
                competitors = competing_processes()
            directory = output / f"{name}-{index}"
            directory.mkdir()
            arguments = [str(executables[name]), *common, *variants[name],
                         "--duration-seconds", "30", "--warmup-seconds", "5",
                         "--samples", "1", "--output", str(directory)]
            (directory / "build-identity.json").write_text(json.dumps({
                "executable_sha256": hashes[name], "arguments": arguments}, indent=2) + "\n")
            print("START", directory.name, flush=True)
            interference = []
            with (directory / "console.log").open("w", encoding="utf-8") as log:
                process = subprocess.Popen(arguments, stdout=log, stderr=subprocess.STDOUT, env=environment)
                while process.poll() is None:
                    competitors = competing_processes()
                    record_competing_work(interference, competitors, on_competing)
                    time.sleep(1)
            (directory / "exit-code.txt").write_text(str(process.returncode) + "\n")
            (directory / "competing-work.json").write_text(json.dumps(interference, indent=2) + "\n")
            interference = recording_interference(directory, interference)
            if interference or process.returncode:
                (directory / "measurement-invalid.json").write_text(json.dumps({
                    "competing_work": interference, "exit_code": process.returncode}, indent=2) + "\n")
                raise RuntimeError(f"Invalid measurement; preserved all artifacts in {directory}")
            print("END", directory.name, flush=True)
    subprocess.run([os.sys.executable, str(root / "scripts/compare-recording-results.py"), str(output)], check=True)


if __name__ == "__main__":
    main()
