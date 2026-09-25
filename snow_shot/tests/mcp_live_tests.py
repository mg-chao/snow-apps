"""Manually exercise every MCP tool against an isolated live Snow Shot instance.

Usage: python snow_shot/tests/mcp_live_tests.py <snow_shot.exe> <snow-shot-mcp.exe>
This captures the current desktop and changes the clipboard.
"""

import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import winreg


TOOLS = {
    "snow_shot_status", "screenshot_begin", "screenshot_state",
    "screenshot_set_selection", "screenshot_set_tool", "screenshot_apply_annotations",
    "screenshot_undo", "screenshot_redo", "screenshot_render", "screenshot_save",
    "screenshot_copy", "screenshot_pin", "screenshot_finish", "screenshot_cancel",
    "screenshot_direct_capture", "screenshot_set_selection_style", "screenshot_set_tool_style",
    "screenshot_edit_elements", "screenshot_recapture", "screenshot_scrolling", "screenshot_scroll_once",
    "screenshot_recognize", "screenshot_translate", "screenshot_auto_filter", "screenshot_operation",
    "screenshot_edit_recognition", "screenshot_export_recognition", "screenshot_draw_template",
}
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def startup_registration():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            return winreg.QueryValueEx(key, "SnowShot")
    except FileNotFoundError:
        return None


def restore_startup_registration(original):
    current = startup_registration()
    if current == original:
        return
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0,
                            winreg.KEY_SET_VALUE) as key:
        if original is None:
            winreg.DeleteValue(key, "SnowShot")
        else:
            value, kind = original
            winreg.SetValueEx(key, "SnowShot", 0, kind, value)


class McpClient:
    def __init__(self, executable, environment):
        self.process = subprocess.Popen(
            [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=environment,
        )
        self.messages = queue.Queue()
        self.diagnostics = []
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()
        self.identifier = 0

    def _read_stdout(self):
        for line in self.process.stdout:
            try:
                self.messages.put(json.loads(line))
            except ValueError:
                self.messages.put({"invalid_stdout": line})

    def _read_stderr(self):
        self.diagnostics.extend(self.process.stderr)

    def send(self, method, params=None):
        self.identifier += 1
        identifier = self.identifier
        message = {"jsonrpc": "2.0", "id": identifier, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        while True:
            response = self.messages.get(timeout=90)
            assert "invalid_stdout" not in response, response
            if response.get("id") == identifier:
                return response

    def tool(self, name, arguments=None):
        response = self.send("tools/call", {"name": name, "arguments": arguments or {}})
        assert "error" not in response, (name, response.get("error"))
        result = response["result"]
        content = result["structuredContent"]
        assert not result.get("isError") and content.get("ok", True), (name, content)
        print(name, "ok", flush=True)
        return content, result.get("content", [])

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


def main():
    app_executable = Path(sys.argv[1]).resolve(strict=True)
    bridge_executable = Path(sys.argv[2]).resolve(strict=True)
    instance_id = f"mcpqa{os.getpid()}"
    original_startup = startup_registration()
    config_directory = Path(os.environ["APPDATA"]) / "SnowShot" / f"snow_shot-e2e-{instance_id}"
    assert not config_directory.exists(), config_directory
    config_directory.mkdir(parents=True)
    (config_directory / "config.json").write_text(json.dumps({
        "storage": {"schema_version": 3},
        "mcp": {"enabled": True},
        "system": {"auto_start_at_boot": False},
        "updates": {"mode": "manual"},
    }), encoding="utf-8")
    app = None
    client = None
    try:
        with tempfile.TemporaryDirectory(prefix="snow-shot-mcp-live-",
                                         ignore_cleanup_errors=True) as directory:
            root = Path(directory)
            descriptor = root / "snow-shot-mcp.json"
            environment = dict(os.environ, SNOW_SHOT_MCP_DESCRIPTOR=str(descriptor))
            app = subprocess.Popen([
                str(app_executable), "--e2e-allow-overlay-capture",
                f"--e2e-instance-id={instance_id}",
            ], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 45
            while not descriptor.exists() and time.monotonic() < deadline:
                assert app.poll() is None, f"Snow Shot exited with {app.returncode}"
                time.sleep(0.1)
            assert descriptor.exists(), "MCP descriptor was not published"
            client = McpClient(bridge_executable, environment)
            initialized = client.send("initialize", {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "snow-shot-live-probe", "version": "1"},
            })
            assert "error" not in initialized, initialized
            client.process.stdin.write(json.dumps({
                "jsonrpc": "2.0", "method": "notifications/initialized",
            }) + "\n")
            client.process.stdin.flush()
            listing = client.send("tools/list", {})
            assert "error" not in listing, listing
            assert {tool["name"] for tool in listing["result"]["tools"]} == TOOLS

            status, _ = client.tool("snow_shot_status")
            assert status["result"]["reachable"] is True
            assert set(status["result"]["capabilities"]) == TOOLS
            begin, _ = client.tool("screenshot_begin", {
                "presentation": "silent", "target": "current_monitor",
            })
            session = begin["result"]["session_id"]
            revision = begin["result"]["revision"]

            def call(name, **options):
                nonlocal revision
                content, blocks = client.tool(name, {
                    "session_id": session, "expected_revision": revision, **options,
                })
                revision = content["result"]["revision"]
                return content, blocks

            state, _ = client.tool("screenshot_state", {"session_id": session})
            assert state["result"]["active"] is True
            assert state["result"]["presentation"] == "silent"
            assert state["result"]["displays"]
            bounds = state["result"]["canvas_bounds"]
            x, y, width, height = bounds
            assert width >= 100 and height >= 100, bounds
            selected, _ = call("screenshot_set_selection", type="rectangle",
                               bounds=[x + 10, y + 10, 90, 90])
            assert selected["result"]["selection"]["bounds"][2] > 0
            tool, _ = call("screenshot_set_tool", tool="rectangle")
            assert tool["result"]["active_tool"] == "rectangle"
            annotated, _ = call("screenshot_apply_annotations", version=1, operations=[{
                "type": "rectangle", "bounds": [x + 20, y + 20, 40, 40],
                "style": {"stroke": [255, 0, 0, 255], "stroke_width": 2},
            }])
            assert annotated["result"]["can_undo"] is True
            undone, _ = call("screenshot_undo")
            assert undone["result"]["can_redo"] is True
            redone, _ = call("screenshot_redo")
            assert redone["result"]["can_undo"] is True
            rendered, blocks = call("screenshot_render")
            png = base64.b64decode(next(block["data"] for block in blocks
                                        if block["type"] == "image"))
            assert png.startswith(b"\x89PNG\r\n\x1a\n")
            assert hashlib.sha256(png).hexdigest() == rendered["result"]["sha256"]
            assert rendered["result"]["byte_count"] > 0
            save_path = root / "saved.png"
            saved, _ = call("screenshot_save", path=str(save_path), format="png")
            assert save_path.is_file() and save_path.stat().st_size > 0
            assert hashlib.sha256(save_path.read_bytes()).hexdigest() == saved["result"]["sha256"]
            copied, _ = call("screenshot_copy")
            assert copied["result"]["copied"] is True
            pinned, _ = call("screenshot_pin")
            assert pinned["result"]["pinned"] is True
            finished, _ = call("screenshot_finish", output="render")
            assert finished["result"]["finished"] is True

            begin, _ = client.tool("screenshot_begin", {
                "presentation": "silent", "target": "current_monitor",
            })
            canceled, _ = client.tool("screenshot_cancel", {
                "session_id": begin["result"]["session_id"],
            })
            assert canceled["result"]["canceled"] is True
            direct, blocks = client.tool("screenshot_direct_capture", {
                "target": "current_monitor", "output": "render",
            })
            assert any(block["type"] == "image" for block in blocks)
            assert direct["result"]["finished"] is True
            direct_path = root / "direct.png"
            direct_save, _ = client.tool("screenshot_direct_capture", {
                "target": "current_monitor", "output": "save", "path": str(direct_path),
            })
            assert direct_path.is_file() and direct_save["result"]["finished"] is True
            direct_copy, _ = client.tool("screenshot_direct_capture", {
                "target": "current_monitor", "output": "copy",
            })
            assert direct_copy["result"]["copied"] is True
            begin, _ = client.tool("screenshot_begin", {
                "presentation": "silent", "target": "current_monitor",
            })
            finish_path = root / "finished.png"
            finish_save, _ = client.tool("screenshot_finish", {
                "session_id": begin["result"]["session_id"],
                "expected_revision": begin["result"]["revision"],
                "output": "save", "path": str(finish_path),
            })
            assert finish_path.is_file() and finish_save["result"]["finished"] is True
            print("All Snow Shot MCP tools passed live validation.")
    finally:
        if client is not None:
            client.close()
        if app is not None and app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=10)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait(timeout=5)
        restore_startup_registration(original_startup)
        if "directory" in locals():
            shutil.rmtree(directory, ignore_errors=False)
        # This directory belongs solely to this unique test instance.
        assert config_directory.parent == Path(os.environ["APPDATA"]) / "SnowShot"
        assert config_directory.name == f"snow_shot-e2e-{instance_id}"
        shutil.rmtree(config_directory)


if __name__ == "__main__":
    main()
