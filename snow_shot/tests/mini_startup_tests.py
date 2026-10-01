"""Start Mini offscreen without borrowing DLLs from a development PATH."""

import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--application", required=True, type=Path)
    args = parser.parse_args()
    application = args.application.resolve(strict=True)
    environment = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    windows = Path(environment["SYSTEMROOT"])
    environment["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    for name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QTDIR"):
        environment.pop(name, None)
    # Missing DLLs must produce an exit code instead of an interactive loader dialog.
    previous_error_mode = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    try:
        with tempfile.TemporaryDirectory(prefix="snow-mini-startup-") as temporary:
            directory = Path(temporary)
            descriptor = directory / "descriptor.json"
            with (directory / "application.log").open("w", encoding="utf-8") as log:
                process = subprocess.Popen(
                    [str(application), "--mcp-fixture", str(directory)],
                    cwd=directory, env=environment, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 30
                    while not descriptor.is_file() and process.poll() is None:
                        if time.monotonic() >= deadline:
                            break
                        time.sleep(0.05)
                    if not descriptor.is_file():
                        raise AssertionError(
                            f"Mini did not initialize: exit={process.poll()}, "
                            f"log={(directory / 'application.log').read_text(encoding='utf-8')}")
                    assert process.poll() is None, "Mini exited after initializing"
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=10)
    finally:
        ctypes.windll.kernel32.SetErrorMode(previous_error_mode)
    print("PASS: Mini initializes Qt, storage, and its controller with a clean PATH.")


if __name__ == "__main__":
    main()
