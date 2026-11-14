"""Exercise the staged MCP executable without enabling capture or touching user settings."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


SCREENSHOT_TOOLS = {
    "snow_shot_mcp_status", "snow_shot_screenshot_begin", "snow_shot_screenshot_state", "snow_shot_screenshot_set_selection",
    "snow_shot_screenshot_set_tool", "snow_shot_screenshot_apply_annotations", "snow_shot_screenshot_undo", "snow_shot_screenshot_redo",
    "snow_shot_screenshot_render", "snow_shot_screenshot_save", "snow_shot_screenshot_copy", "snow_shot_screenshot_pin", "snow_shot_screenshot_finish",
    "snow_shot_screenshot_cancel", "snow_shot_screenshot_direct_capture", "snow_shot_screenshot_set_selection_style",
    "snow_shot_screenshot_set_tool_style", "snow_shot_screenshot_edit_elements", "snow_shot_screenshot_recapture",
    "snow_shot_screenshot_scrolling", "snow_shot_screenshot_scroll_once", "snow_shot_screenshot_recognize", "snow_shot_screenshot_translate",
    "snow_shot_screenshot_auto_filter", "snow_shot_screenshot_operation", "snow_shot_screenshot_edit_recognition",
    "snow_shot_screenshot_export_recognition", "snow_shot_screenshot_draw_template",
}
SUPPORTED_VERSIONS = ["2026-07-28", "2025-11-25"]


def exercise(executable, recognition_only=False):
    version = "2026-07-28"
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
            send({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})
            discovery = response(1)
            assert discovery["supportedVersions"] == SUPPORTED_VERSIONS, discovery
            assert discovery["ttlMs"] >= 0, discovery
            for capability in ["tools", "resources", "prompts", "completions"]:
                assert capability in discovery["capabilities"], discovery
            send({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})
            listed = response(2)
            tools = listed["tools"]
            names = {tool["name"] for tool in tools}
            assert SCREENSHOT_TOOLS <= names and len(names) == len(tools), names
            assert {"snow_shot_document_open", "snow_shot_recording_start", "snow_shot_settings_update"} <= names
            assert listed["ttlMs"] == 300000 and listed["cacheScope"] == "public", listed
            assert all("outputSchema" in tool for tool in tools)
            schemas = {tool["name"]:tool["inputSchema"] for tool in tools}
            assert "expected_revision" in schemas["snow_shot_screenshot_apply_annotations"]["required"]
            assert "operations" in schemas["snow_shot_screenshot_apply_annotations"]["required"]
            for name in ("snow_shot_screenshot_recognize", "snow_shot_document_recognize"):
                kind_schema = schemas[name]["properties"]["kind"]
                if "$ref" in kind_schema:
                    kind_schema = schemas[name]["$defs"][kind_schema["$ref"].rsplit("/", 1)[1]]
                assert kind_schema["enum"] == [
                    "text", "table", "qr", "markdown", "html", "latex", "document"], schemas[name]
            for identifier, name, arguments in (
                    (16, "snow_shot_screenshot_recognize",
                     {"session_id":"s", "expected_revision":1, "kind":"latex"}),
                    (17, "snow_shot_document_recognize",
                     {"document_id":"d", "expected_revision":1, "kind":"latex"}),
                    (18, "snow_shot_screenshot_recognize",
                     {"session_id":"s", "expected_revision":1, "kind":"document"}),
                    (19, "snow_shot_document_recognize",
                     {"document_id":"d", "expected_revision":1, "kind":"document"})):
                send({"jsonrpc":"2.0", "id":identifier, "method":"tools/call",
                      "params":{"name":name, "arguments":arguments}})
                unavailable = response(identifier)
                assert unavailable["isError"] is True, unavailable
                assert unavailable["structuredContent"]["error"]["code"] == "unavailable", unavailable
            if recognition_only:
                process.stdin.close()
                assert process.wait(timeout=10) == 0, diagnostics
                print("MCP recognition schemas and dispatch passed through the stdio bridge.")
                return
            send({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
                "name":"snow_shot_mcp_status","arguments":{}}})
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
                "name":"snow_shot_screenshot_set_tool","arguments":{
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
            print(f"MCP {version}: discovery, screenshot contracts, schemas, resources, prompts, completion, errors and clean exit passed.")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    if "--recognition-only" in sys.argv[2:]:
        exercise(executable, recognition_only=True)
        return
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
    exercise(executable)
    # The bridge retains a legacy revision for stdio initialize negotiation. An older
    # client receives that supported fallback and decides whether it can continue.
    for version in ("2025-11-25", "2024-11-05"):
        initialize = {"jsonrpc":"2.0","id":1,"method":"initialize","params":{
            "protocolVersion":version,"capabilities":{},
            "clientInfo":{"name":"legacy-client","version":"1"}}}
        initialized = subprocess.run([str(executable)],
                                     input=(json.dumps(initialize) + "\n").encode(),
                                     capture_output=True, timeout=5)
        negotiated = json.loads(initialized.stdout)
        assert initialized.returncode == 0 and "error" not in negotiated, negotiated
        assert negotiated["result"]["protocolVersion"] == "2025-11-25", negotiated
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
        metadata = {"_meta": {
            "io.modelcontextprotocol/protocolVersion": "2026-07-28",
            "io.modelcontextprotocol/clientInfo": {"name":"slow-reader","version":"1"},
            "io.modelcontextprotocol/clientCapabilities": {},
        }}
        requests = [{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}}]
        requests.extend({"jsonrpc":"2.0","id":index,"method":"tools/list","params":metadata}
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
