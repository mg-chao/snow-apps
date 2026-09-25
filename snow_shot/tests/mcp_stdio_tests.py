"""Exercise the staged MCP executable without enabling capture or touching user settings."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="snow-shot-mcp-smoke-") as directory:
        environment = dict(os.environ)
        environment["SNOW_SHOT_MCP_DESCRIPTOR"] = str(Path(directory) / "absent.json")
        process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", env=environment)
        messages = queue.Queue()
        diagnostics = []
        def read_stdout():
            for line in process.stdout:
                try:
                    messages.put(json.loads(line))
                except ValueError:
                    messages.put({"invalid_stdout": line})
        def read_stderr():
            diagnostics.extend(process.stderr)
        threading.Thread(target=read_stdout, daemon=True).start()
        threading.Thread(target=read_stderr, daemon=True).start()
        def send(message):
            process.stdin.write(json.dumps(message) + "\n")
            process.stdin.flush()
        def response(identifier):
            while True:
                message = messages.get(timeout=15)
                assert "invalid_stdout" not in message, message
                if message.get("id") == identifier:
                    assert "error" not in message, message
                    return message["result"]
        try:
            send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
                "protocolVersion":"2025-11-25","capabilities":{},
                "clientInfo":{"name":"snow-shot-package-smoke","version":"1"}}})
            assert response(1)["capabilities"]["tools"] is not None
            send({"jsonrpc":"2.0","method":"notifications/initialized"})
            send({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})
            tools = response(2)["tools"]
            assert len(tools) == 28, tools
            schemas = {tool["name"]:tool["inputSchema"] for tool in tools}
            assert "expected_revision" in schemas["screenshot_apply_annotations"]["required"]
            assert "operations" in schemas["screenshot_apply_annotations"]["required"]
            send({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
                "name":"snow_shot_status","arguments":{}}})
            status = response(3)
            assert not status.get("isError", False), status
            assert status["structuredContent"]["reachable"] is False, status
            assert status["structuredContent"]["error"]["code"] == "unavailable", status
            assert "Snow Shot is not running" in status["content"][0]["text"], status
            process.stdin.close()
            assert process.wait(timeout=10) == 0, diagnostics
            print("Staged MCP stdio initialization, typed tool list, unavailable status, and clean exit passed.")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
