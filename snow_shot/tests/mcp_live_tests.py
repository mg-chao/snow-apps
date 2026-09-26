"""Exercise all 28 legacy routes against the isolated Windows application fixture.

Usage: python snow_shot/tests/mcp_live_tests.py <snow_shot.exe> <snow-shot-mcp.exe>
This captures the desktop. Clipboard ownership is retained and restored. Provider
and scrolling routes use explicit ownership guards, not claimed provider success.
"""

import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


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
class ClipboardGuard:
    """Retain the original COM data object, including non-text clipboard formats."""
    @staticmethod
    def retry(operation, *arguments):
        from ctypes import wintypes
        user = ctypes.WinDLL("user32")
        message = wintypes.MSG()
        deadline = time.monotonic() + 5
        while True:
            try:
                return operation(*arguments)
            except OSError as error:
                if not 0x800401D0 <= (error.winerror & 0xffffffff) <= 0x800401D4 or time.monotonic() >= deadline:
                    raise
                # The retained data object may require COM/window-message dispatch.
                while user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                    user.TranslateMessage(ctypes.byref(message))
                    user.DispatchMessageW(ctypes.byref(message))
                time.sleep(.05)

    def __enter__(self):
        self.ole = ctypes.OleDLL("ole32")
        self.ole.OleInitialize.argtypes = [ctypes.c_void_p]
        self.ole.OleGetClipboard.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        self.ole.OleSetClipboard.argtypes = [ctypes.c_void_p]
        self.ole.OleInitialize(None)
        self.original = ctypes.c_void_p()
        try:
            self.retry(self.ole.OleGetClipboard, ctypes.byref(self.original))
        except OSError:
            self.ole.OleUninitialize()
            raise
        return self

    def __exit__(self, *_):
        try:
            self.retry(self.ole.OleSetClipboard, self.original)
            if self.original:
                self.retry(self.ole.OleFlushClipboard)
        finally:
            if self.original:
                table = ctypes.cast(self.original,
                    ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
                release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])
                release(self.original)
            self.ole.OleUninitialize()


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
        self.outcomes = {}

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

    def tool(self, name, arguments=None, expected_error=None):
        response = self.send("tools/call", {"name": name, "arguments": arguments or {}})
        assert "error" not in response, (name, response.get("error"))
        result = response["result"]
        content = result["structuredContent"]
        if expected_error:
            assert result.get("isError") and content["error"]["code"] in expected_error, (name, content)
            outcome = "ownership_guard:" + content["error"]["code"]
        else:
            assert not result.get("isError") and content.get("ok", True), (name, content)
            outcome = "success"
        self.outcomes[name] = outcome
        print(name, outcome, flush=True)
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
    app = None
    client = None
    temporary = tempfile.TemporaryDirectory(prefix="snow-shot-mcp-live-")
    try:
        directory = temporary.name
        root = Path(directory)
        descriptor = root / "descriptor.json"
        environment = dict(os.environ, SNOW_SHOT_MCP_DESCRIPTOR=str(descriptor))
        app = subprocess.Popen([
            str(app_executable), "--mcp-fixture", str(root),
            "--e2e-allow-overlay-capture",
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
        assert TOOLS <= {tool["name"] for tool in listing["result"]["tools"]}

        status, _ = client.tool("snow_shot_status")
        assert status["result"]["reachable"] is True
        assert TOOLS <= set(status["result"]["capabilities"])
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
            revision = content["result"].get("revision", content.get("revision", revision))
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
        call("screenshot_set_selection_style", corner_radius=4, shadow_width=0,
             aspect_ratio_locked=False)
        call("screenshot_set_tool_style", target="rectangle",
             style={"stroke": [255, 0, 0, 255], "stroke_width": 2})
        annotated, _ = call("screenshot_apply_annotations", version=1, operations=[{
            "type": "rectangle", "bounds": [x + 20, y + 20, 40, 40],
            "style": {"stroke": [255, 0, 0, 255], "stroke_width": 2},
        }])
        assert annotated["result"]["can_undo"] is True
        undone, _ = call("screenshot_undo")
        assert undone["result"]["can_redo"] is True
        redone, _ = call("screenshot_redo")
        assert redone["result"]["can_undo"] is True
        element_id = annotated["result"]["transaction"]["created_element_ids"][0]
        call("screenshot_edit_elements", action="select", id=element_id)
        call("screenshot_edit_elements", action="opacity", opacity=0.8)
        call("screenshot_draw_template", action="export")
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

        # These exercise the real application dispatch and ownership boundary.
        # Provider availability and native input injection require dedicated gates.
        fixtures = json.loads(Path(__file__).with_name("mcp_contract_fixtures.json")
                              .read_text(encoding="utf-8"))["fixtures"]
        guarded = {"screenshot_scrolling", "screenshot_scroll_once", "screenshot_recognize",
                   "screenshot_translate", "screenshot_auto_filter", "screenshot_operation",
                   "screenshot_edit_recognition", "screenshot_export_recognition"}
        for fixture in fixtures:
            if fixture["name"] in guarded:
                client.tool(fixture["name"], fixture["arguments"],
                            expected_error={"session_not_found", "session_mismatch", "not_owner",
                                            "session_required", "stale_session"})

        begin, _ = client.tool("screenshot_begin", {
            "presentation": "silent", "target": "current_monitor",
        })
        client.tool("screenshot_recapture", {
            "session_id": begin["result"]["session_id"],
            "expected_revision": begin["result"]["revision"],
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
        assert TOOLS <= client.outcomes.keys(), sorted(TOOLS - client.outcomes.keys())
        print(json.dumps({"legacy_routes": client.outcomes,
              "scope": "real Windows capture/edit/export plus explicit provider/scrolling ownership guards"},
              sort_keys=True), flush=True)
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
        temporary.cleanup()


if __name__ == "__main__":
    with ClipboardGuard():
        main()
