"""Exercise the staged MCP executable without enabling capture or touching user settings."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


LEGACY_TOOLS = {
    "snow_shot_status", "screenshot_begin", "screenshot_state", "screenshot_set_selection",
    "screenshot_set_tool", "screenshot_apply_annotations", "screenshot_undo", "screenshot_redo",
    "screenshot_render", "screenshot_save", "screenshot_copy", "screenshot_pin", "screenshot_finish",
    "screenshot_cancel", "screenshot_direct_capture", "screenshot_set_selection_style",
    "screenshot_set_tool_style", "screenshot_edit_elements", "screenshot_recapture",
    "screenshot_scrolling", "screenshot_scroll_once", "screenshot_recognize", "screenshot_translate",
    "screenshot_auto_filter", "screenshot_operation", "screenshot_edit_recognition",
    "screenshot_export_recognition", "screenshot_draw_template",
}


def exercise(executable, version):
    with tempfile.TemporaryDirectory(prefix="snow-shot-mcp-smoke-") as directory:
        environment = dict(os.environ)
        environment["SNOW_SHOT_MCP_DESCRIPTOR"] = str(Path(directory) / "absent.json")
        environment["RUST_LOG"] = "trace"
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
            if version == "2026-07-28" and "id" in message:
                message.setdefault("params", {}).setdefault("_meta", {}).update({
                    "io.modelcontextprotocol/protocolVersion": version,
                    "io.modelcontextprotocol/clientInfo": {"name":"snow-shot-package-smoke","version":"1"},
                    "io.modelcontextprotocol/clientCapabilities": {},
                })
            process.stdin.write(json.dumps(message) + "\n")
            process.stdin.flush()
        def response(identifier, expect_error=False):
            while True:
                message = messages.get(timeout=15)
                assert "invalid_stdout" not in message, message
                if message.get("id") == identifier:
                    assert ("error" in message) == expect_error, message
                    return message["error"] if expect_error else message["result"]
        try:
            if version == "2026-07-28":
                send({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})
                discovery = response(1)
                assert version in discovery["supportedVersions"], discovery
                assert discovery["ttlMs"] >= 0, discovery
            else:
                send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
                    "protocolVersion":version,"capabilities":{},
                    "clientInfo":{"name":"snow-shot-package-smoke","version":"1"}}})
                discovery = response(1)
                assert discovery["protocolVersion"] == version, discovery
                send({"jsonrpc":"2.0","method":"notifications/initialized"})
            for capability in ["tools", "resources", "prompts", "completions"]:
                assert capability in discovery["capabilities"], discovery
            send({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})
            listed = response(2)
            tools = listed["tools"]
            names = {tool["name"] for tool in tools}
            assert LEGACY_TOOLS <= names and len(names) == len(tools), names
            assert {"snow_shot_document_open", "snow_shot_recording_start", "snow_shot_settings_update"} <= names
            if version == "2026-07-28":
                assert listed["ttlMs"] == 300000 and listed["cacheScope"] == "public", listed
            assert all("outputSchema" in tool for tool in tools)
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
            send({"jsonrpc":"2.0","id":4,"method":"resources/list","params":{}})
            assert len(response(4)["resources"]) >= 3
            send({"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"snow-shot://capabilities"}})
            capabilities = response(5)
            catalog = json.loads(capabilities["contents"][0]["text"])
            assert {tool["name"] for tool in catalog["tools"]} == names
            send({"jsonrpc":"2.0","id":6,"method":"prompts/list","params":{}})
            assert {p["name"] for p in response(6)["prompts"]} == {"screenshot_workflow","background_image_workflow","recording_workflow"}
            send({"jsonrpc":"2.0","id":7,"method":"prompts/get","params":{"name":"background_image_workflow","arguments":{"path":"C:/example.png","goal":"Add an arrow","output_format":"png"}}})
            assert "document_id" in response(7)["messages"][0]["content"]["text"]
            send({"jsonrpc":"2.0","id":8,"method":"completion/complete","params":{"ref":{"type":"ref/prompt","name":"background_image_workflow"},"argument":{"name":"output_format","value":"p"}}})
            assert response(8)["completion"]["values"] == ["png","pdf"]
            send({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"snow_shot_document_close","arguments":{"document_id":"example"}}})
            assert response(9,True)["code"] == -32602
            send({"jsonrpc":"2.0","id":10,"method":"resources/read","params":{"uri":"file:///should-not-open"}})
            assert response(10,True)["code"] == -32602
            send({"jsonrpc":"2.0","id":11,"method":"tools/list","params":{"cursor":"invalid"}})
            assert response(11,True)["code"] == -32602
            secret = "fixture-secret-never-in-diagnostics-72619"
            send({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{
                "name":"snow_shot_credentials_set","arguments":{
                    "provider":"fixture-model","secret":secret,"expected_revision":1}}})
            assert response(12)["isError"] is True
            invalid_secret = "fixture-invalid-secret-never-in-diagnostics-91374"
            send({"jsonrpc":"2.0","id":13,"method":"tools/call","params":{
                "name":"screenshot_set_tool","arguments":{
                    "session_id":"s","expected_revision":1,"tool":invalid_secret}}})
            assert response(13, True)["code"] == -32602
            for identifier, prompt, expected in ((14, "screenshot_workflow", "session_id"),
                                                  (15, "recording_workflow", "recording_id")):
                send({"jsonrpc":"2.0","id":identifier,"method":"prompts/get",
                      "params":{"name":prompt,"arguments":{"goal":"Fixture workflow"}}})
                assert expected in response(identifier)["messages"][0]["content"]["text"]
            process.stdin.close()
            assert process.wait(timeout=10) == 0, diagnostics
            assert secret not in "".join(diagnostics), "SDK logs exposed credential contents"
            assert invalid_secret not in "".join(diagnostics), "SDK error logs exposed invalid argument contents"
            print(f"MCP {version}: discovery, preserved legacy contracts, schemas, resources, prompts, completion, errors and clean exit passed.")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    help_result = subprocess.run([str(executable), "--help"], capture_output=True, timeout=5)
    assert help_result.returncode == 0 and not help_result.stdout and b"--launch-app" in help_result.stderr
    invalid_result = subprocess.run([str(executable), "--unknown"], capture_output=True, timeout=5)
    assert invalid_result.returncode != 0 and not invalid_result.stdout
    startup_secret = "fixture-startup-secret-never-in-diagnostics-18735"
    premature_call = {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{
        "name":"snow_shot_credentials_set","arguments":{"secret":startup_secret}}}
    premature_result = subprocess.run([str(executable)], input=(json.dumps(premature_call) + "\n").encode(),
                                      capture_output=True, timeout=5)
    assert premature_result.returncode != 0
    assert startup_secret.encode() not in premature_result.stderr, "Initialization errors exposed a rejected request"
    for version in ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2026-07-28"):
        exercise(executable, version)
    # Large SDK input lines must be bounded before JSON parsing or local dispatch.
    process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output, diagnostics = process.communicate(b"x" * (1024 * 1024 + 65537) + b"\n", timeout=10)
    assert output == b"" and b"exceeds its byte limit" in diagnostics, diagnostics
    # A non-reading client cannot retain an unbounded number of catalog responses.
    # Keep stdin open deliberately to verify shutdown is independent of its EOF.
    process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        requests = [{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
            "protocolVersion":"2025-06-18","capabilities":{},
            "clientInfo":{"name":"slow-reader","version":"1"}}},
            {"jsonrpc":"2.0","method":"notifications/initialized"}]
        requests.extend({"jsonrpc":"2.0","id":index,"method":"tools/list","params":{}}
                        for index in range(2, 28))
        process.stdin.write(b"".join(json.dumps(message).encode() + b"\n" for message in requests))
        process.stdin.flush()
        process.wait(timeout=15)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdin.close()
        process.stdout.close()
    print("MCP stdio oversized-input and non-reading-peer shutdown bounds passed.")


if __name__ == "__main__":
    main()
