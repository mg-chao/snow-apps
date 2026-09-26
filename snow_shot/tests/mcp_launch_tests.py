"""Verify opt-in adjacent launch with a private sentinel, never an installed app."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from mcp_fixture_tests import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bridge", type=lambda value: Path(value).resolve(strict=True))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="snow-shot-mcp-launch-") as temporary:
        directory = Path(temporary)
        extension = ".exe" if os.name == "nt" else ""
        bridge = directory / ("snow-shot-mcp" + extension)
        sentinel = directory / ("snow_shot" + extension)
        source = directory / "launch_sentinel.rs"
        marker = directory / "launched.txt"
        descriptor = directory / "absent-descriptor.json"
        shutil.copy2(args.bridge, bridge)
        source.write_text('''#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]
use std::io::Write;
fn main() {
    let path = std::env::var_os("SNOW_SHOT_MCP_LAUNCH_SENTINEL").expect("private marker");
    let mut file = std::fs::OpenOptions::new().create(true).append(true).open(path).unwrap();
    file.write_all(b"launched\\n").unwrap();
}
''', encoding="utf-8")
        subprocess.run(["rustc", "--edition=2024", "-C", "opt-level=s", str(source), "-o", str(sentinel)],
                       check=True, capture_output=True, timeout=60)
        original = os.environ.get("SNOW_SHOT_MCP_LAUNCH_SENTINEL")
        os.environ["SNOW_SHOT_MCP_LAUNCH_SENTINEL"] = str(marker)
        observations = []
        try:
            for launch in (False, True):
                client = Client(bridge, descriptor, launch=launch)
                try:
                    for attempt in range(2):
                        start = time.perf_counter()
                        result = client.request("tools/call", {"name": "snow_shot_status", "arguments": {}})
                        content = result["structuredContent"]
                        assert content["reachable"] is False and content["mcp_enabled"] is None, content
                        assert content["error"]["code"] == "unavailable", content
                        assert not descriptor.exists(), "Bridge must never fabricate application enablement"
                        lines = marker.read_text().splitlines() if marker.exists() else []
                        assert len(lines) == int(launch), (launch, attempt, lines)
                        observations.append({"launch": launch, "attempt": attempt,
                                             "elapsed_ms": (time.perf_counter() - start) * 1000})
                finally:
                    client.close()
        finally:
            if original is None:
                os.environ.pop("SNOW_SHOT_MCP_LAUNCH_SENTINEL", None)
            else:
                os.environ["SNOW_SHOT_MCP_LAUNCH_SENTINEL"] = original
        print(json.dumps({"adjacent_launch": observations,
              "scope": "Private compiled sentinel; default never launches, opt-in launches once, missing descriptor remains unavailable; installed app untouched"}, indent=2))


if __name__ == "__main__":
    main()
