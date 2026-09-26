"""Actual stdio -> private IPC -> Qt fixture integration and scoped measurements.

Only synthetic fixture data is used. Native capture/provider/encoder behavior is
outside this test. Use a performance-preset Release fixture for --benchmark.
"""
import argparse
import base64
import ctypes
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
import platform
from pathlib import Path
import queue
import random
import statistics
import struct
import subprocess
import tempfile
import threading
import time
import zlib
from concurrent.futures import ThreadPoolExecutor


class RecognitionProvider:
    """Bounded loopback replies exercise actual HTTP, table and conversion code."""
    def __enter__(self):
        self.calls = []
        calls = self.calls
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def respond_json(self, value):
                body = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if not self.path.startswith("/api/v2/chat/models"):
                    self.send_error(404)
                    return
                calls.append("models")
                self.respond_json({"data": [
                    {"model": "fixture-vision", "name": "Fixture vision", "supports_vision": True},
                    {"model": "fixture-translation", "name": "Fixture translation", "translation_mode": "default"}]})

            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 2 * 1024 * 1024:
                    self.send_error(400)
                    return
                body = self.rfile.read(size)
                if self.path == "/api/v1/table/extract":
                    assert b"image/webp" in body and b"RIFF" in body
                    calls.append("table")
                    self.respond_json({"data": {"html": "<table><tr><th>A</th><th>B</th></tr>"
                                                         "<tr><td>C</td><td>D</td></tr></table>"}})
                    return
                if self.path != "/api/v1/chat/completions":
                    self.send_error(404)
                    return
                request = json.loads(body)
                system = request["messages"][0]["content"]
                if "Convert the image" in system:
                    content = request["messages"][1]["content"]
                    assert any(item.get("image_url", {}).get("url", "").startswith("data:image/webp;base64,")
                               for item in content)
                    kind = "markdown" if "GitHub-flavored Markdown" in system else "html"
                    response = "# Fixture heading\n\nFixture **content**." if kind == "markdown" else "<h1>Fixture heading</h1><p>Fixture <strong>content</strong>.</p>"
                else:
                    kind, response = "translation", "Fixture translated text"
                calls.append(kind)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                middle = len(response) // 2
                for content in (response[:middle], response[middle:]):
                    self.wfile.write(b"data: " + json.dumps({"choices": [{"delta": {"content": content}}]}).encode() + b"\n\n")
                    self.wfile.flush()
                    time.sleep(.01)
                self.wfile.write(b"data: [DONE]\n\n")
                self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class Client:
    executed_tools = set()
    read_resources = set()
    def __init__(self, executable, descriptor, modern=False, tasks=False, launch=False):
        environment = dict(os.environ, SNOW_SHOT_MCP_DESCRIPTOR=str(descriptor))
        self.process = subprocess.Popen([str(executable)] + (["--launch-app"] if launch else []), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, encoding="utf-8", env=environment)
        self.messages = queue.Queue()
        self.notifications = []
        self.diagnostics = []
        self.modern = modern
        self.capabilities = {"extensions": {"io.modelcontextprotocol/tasks": {}}} if tasks else {}
        self.sequence = 0
        self.bytes_received = 0
        self.samples = {}
        def read_stdout():
            for line in self.process.stdout:
                self.bytes_received += len(line.encode("utf-8"))
                try:
                    self.messages.put(json.loads(line))
                except ValueError:
                    self.messages.put({"invalid_stdout": line})
        def read_stderr():
            self.diagnostics.extend(self.process.stderr)
        threading.Thread(target=read_stdout, daemon=True).start()
        threading.Thread(target=read_stderr, daemon=True).start()
        if modern:
            self.request("server/discover")
        else:
            self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                         "clientInfo": {"name": "snow-shot-fixture", "version": "1"}})
            self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, message):
        if self.modern and "id" in message:
            message.setdefault("params", {}).setdefault("_meta", {}).update({
                "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientInfo": {"name": "snow-shot-fixture", "version": "1"},
                "io.modelcontextprotocol/clientCapabilities": self.capabilities,
            })
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def request(self, method, params=None, label=None):
        if method == "tools/call" and params:
            Client.executed_tools.add(params["name"])
        if method == "resources/read" and params:
            Client.read_resources.add(params["uri"])
        self.sequence += 1
        identifier = self.sequence
        started = time.perf_counter()
        self.send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params or {}})
        deadline = time.monotonic() + 30
        while True:
            message = self.messages.get(timeout=max(0.01, deadline - time.monotonic()))
            assert "invalid_stdout" not in message, message
            if message.get("id") != identifier:
                self.notifications.append(message)
                continue
            assert "error" not in message, message
            if label:
                self.samples.setdefault(label, []).append((time.perf_counter() - started) * 1000)
            return message["result"]

    def tool(self, name, arguments=None, error=None, label=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}}, label)
        if result.get("resultType") == "task":
            return result
        content = result["structuredContent"]
        if error:
            assert result.get("isError") and content["error"]["code"] == error, result
        else:
            assert not result.get("isError", False), result
            assert content.get("ok", False) or content.get("reachable") is True, content
            for block in result.get("content", []):
                if block.get("type") == "image":
                    payload = base64.b64decode(block["data"], validate=True)
                    assert payload.startswith(b"\x89PNG\r\n\x1a\n"), block.get("mimeType")
                    digest = content.get("result", {}).get("sha256")
                    if digest:
                        assert hashlib.sha256(payload).hexdigest() == digest
        return content

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                assert self.process.wait(timeout=10) == 0, self.diagnostics
            finally:
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait(timeout=5)

    def listen(self, uri):
        self.sequence += 1
        identifier = self.sequence
        self.send({"jsonrpc": "2.0", "id": identifier, "method": "subscriptions/listen",
                   "params": {"notifications": {"resourceSubscriptions": [uri]}}})
        deadline = time.monotonic() + 10
        while True:
            message = self.messages.get(timeout=max(.01, deadline - time.monotonic()))
            assert "error" not in message and "invalid_stdout" not in message, message
            self.notifications.append(message)
            if message.get("method") == "notifications/subscriptions/acknowledged":
                params = message["params"]
                assert params["_meta"]["io.modelcontextprotocol/subscriptionId"] == identifier, message
                assert params["notifications"]["resourceSubscriptions"] == [uri], message
                return identifier


def process_metrics(process):
    """Windows kernel counters; null on hosts without this counter implementation."""
    if os.name != "nt":
        return None
    from ctypes import wintypes
    class Memory(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (field, ctypes.c_size_t) for field in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    memory_api = ctypes.WinDLL("psapi", use_last_error=True)
    get_times = kernel.GetProcessTimes
    get_times.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    get_times.restype = wintypes.BOOL
    get_memory = memory_api.GetProcessMemoryInfo
    get_memory.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
    get_memory.restype = wintypes.BOOL
    times = [wintypes.FILETIME() for _ in range(4)]
    handle = wintypes.HANDLE(int(process._handle))
    assert get_times(handle, *(ctypes.byref(value) for value in times))
    ticks = lambda value: (value.dwHighDateTime << 32) | value.dwLowDateTime
    memory = Memory()
    memory.cb = ctypes.sizeof(memory)
    assert get_memory(handle, ctypes.byref(memory), memory.cb)
    return {"cpu_ms": (ticks(times[2]) + ticks(times[3])) / 10000,
            "working_set_bytes": memory.WorkingSetSize,
            "peak_working_set_bytes": memory.PeakWorkingSetSize,
            "private_bytes": memory.PrivateUsage}


def summarize(client, before, after):
    samples = {}
    for name, values in client.samples.items():
        ordered = sorted(values)
        percentile = lambda fraction: ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]
        samples[name] = {"count": len(values), "p50_ms": statistics.median(values),
                         "p95_ms": percentile(.95), "p99_ms": percentile(.99)}
    return {"latency": samples, "stdout_bytes": client.bytes_received,
            "process_before": before, "process_after": after,
            "cpu_delta_ms": after["cpu_ms"] - before["cpu_ms"] if before and after else None}


def document_workflow(client, directory, samples):
    opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png"),
                         "idempotency_key": "fixture-open"}, label="document_open")
    identifier, revision = opened["result"]["document_id"], opened["revision"]
    duplicate = client.tool("snow_shot_document_open", {"path": str(directory / "source.png"),
                            "idempotency_key": "fixture-open"})
    assert duplicate["result"]["document_id"] == identifier
    arguments = {"document_id": identifier, "expected_revision": revision}
    uri = "snow-shot://documents/" + identifier
    client.request("resources/subscribe", {"uri": uri})
    changed = client.tool("snow_shot_document_apply_annotations", dict(arguments,
                          operations=[{"type": "rectangle", "bounds": [10, 10, 20, 20]}]))
    revision = changed["revision"]
    assert revision > arguments["expected_revision"]
    client.tool("snow_shot_document_undo", arguments, error="stale_revision")
    arguments["expected_revision"] = revision
    rendered = client.tool("snow_shot_document_render", arguments, label="document_render_cold")
    assert rendered["result"]["width"] == 80 and rendered["result"]["height"] == 60, rendered
    for _ in range(samples):
        rendered = client.tool("snow_shot_document_render", arguments, label="document_render_cached")
        assert rendered["result"]["cache_hit"], rendered
    saved = client.tool("snow_shot_document_save", dict(arguments, path=str(directory / "output.png")))
    assert (directory / "output.png").is_file(), saved
    job = client.tool("snow_shot_document_recognize", dict(arguments, kind="text"))["result"]
    job_uri = "snow-shot://jobs/" + job["job_id"]
    client.request("resources/subscribe", {"uri": job_uri})
    deadline = time.monotonic() + 10
    while job["status"] == "running" and time.monotonic() < deadline:
        time.sleep(.25)
        job = client.tool("snow_shot_job_get", {"job_id": job["job_id"]})["result"]
    assert job["status"] == "completed", job
    assert "Fixture recognized text" in json.dumps(job), job
    cancel_job = client.tool("snow_shot_document_recognize", dict(arguments, kind="qr"))["result"]
    canceled = client.tool("snow_shot_job_cancel", {"job_id": cancel_job["job_id"]})["result"]
    assert canceled["status"] == "canceled", canceled
    resource = client.request("resources/read", {"uri": uri})
    assert json.loads(resource["contents"][0]["text"])["result"]["document_id"] == identifier
    # A synchronous read after a short coalescing interval drains preceding notifications.
    time.sleep(.2)
    client.tool("snow_shot_document_state", {"document_id": identifier})
    updated = {notice.get("params", {}).get("uri") for notice in client.notifications
               if notice.get("method") == "notifications/resources/updated"}
    assert uri in updated and job_uri in updated, client.notifications
    client.request("resources/unsubscribe", {"uri": uri})
    client.request("resources/unsubscribe", {"uri": job_uri})
    client.tool("snow_shot_document_close", arguments)
    return identifier


def modern_task_workflow(executable, descriptor, directory):
    client = Client(executable, descriptor, modern=True, tasks=True)
    try:
        opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png")})
        if opened.get("resultType") == "task":
            deadline = time.monotonic() + 10
            while True:
                task = client.request("tasks/get", {"taskId": opened["taskId"]})
                if task["status"] != "working" or time.monotonic() >= deadline:
                    break
                time.sleep(.025)
            assert task["status"] == "completed", task
            document = task["result"]["structuredContent"]["result"]["result"]
        else:
            assert opened["result"]["status"] == "completed", opened
            document = opened["result"]["result"]
        arguments = {"document_id": document["document_id"], "expected_revision": document["revision"]}
        uri = "snow-shot://documents/" + arguments["document_id"]
        subscription = client.listen(uri)
        changed = client.tool("snow_shot_document_apply_annotations", dict(arguments,
                              operations=[{"type": "rectangle", "bounds": [1, 1, 12, 12]}]))
        arguments["expected_revision"] = changed["revision"]
        created = client.tool("snow_shot_document_recognize", dict(arguments, kind="text"))
        task_id = created["taskId"]
        deadline = time.monotonic() + 10
        while True:
            task = client.request("tasks/get", {"taskId": task_id})
            if task["status"] != "working" or time.monotonic() >= deadline:
                break
            time.sleep(.25)
        assert task["status"] == "completed", task
        assert "Fixture recognized text" in json.dumps(task), task
        created = client.tool("snow_shot_document_recognize", dict(arguments, kind="qr"))
        client.request("tasks/cancel", {"taskId": created["taskId"]})
        canceled = client.request("tasks/get", {"taskId": created["taskId"]})
        assert canceled["status"] == "cancelled", canceled
        time.sleep(.2)
        client.tool("snow_shot_document_state", {"document_id": arguments["document_id"]})
        updates = [notice for notice in client.notifications
                   if notice.get("method") == "notifications/resources/updated"]
        assert any(notice["params"].get("uri") == uri and
                   notice["params"]["_meta"]["io.modelcontextprotocol/subscriptionId"] == subscription
                   for notice in updates), updates
        client.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                     "params": {"requestId": subscription}})
        client.tool("snow_shot_document_close", arguments)
    finally:
        client.close()


def artifact_workflow(executable, descriptor, directory):
    client = Client(executable, descriptor)
    other = Client(executable, descriptor)
    try:
        opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png")})
        arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
        job = client.tool("snow_shot_document_recognize", dict(arguments, kind="text"))["result"]
        deadline = time.monotonic() + 10
        while job["status"] == "running" and time.monotonic() < deadline:
            time.sleep(.25)
            job = client.tool("snow_shot_job_get", {"job_id": job["job_id"]})["result"]
        assert job["status"] == "completed", job
        state = client.tool("snow_shot_document_recognition_state", arguments)["result"]
        edited = client.tool("snow_shot_document_edit_recognition", dict(arguments,
            expected_recognition_revision=state["recognition_revision"], action="set_text", text="x" * 600000))
        assert edited["ok"], edited
        exported = client.tool("snow_shot_document_export_recognition", dict(arguments, output="return", format="json"))
        artifact = exported["result"]["artifact"]
        artifact_id = artifact["artifact_id"]
        assert artifact["byte_count"] > 1024 * 1024, artifact
        assert any(item["artifact_id"] == artifact_id for item in
                   client.tool("snow_shot_artifact_list")["result"]["artifacts"])
        other.tool("snow_shot_artifact_read", {"artifact_id": artifact_id}, error="artifact_not_found")
        resource = client.request("resources/read", {"uri": artifact["uri"]})
        first = json.loads(resource["contents"][0]["text"])["result"]
        assert len(base64.b64decode(first["data_base64"])) <= 65536
        payload = bytearray()
        while True:
            response = client.request("tools/call", {"name": "snow_shot_artifact_read", "arguments": {
                "artifact_id": artifact_id, "offset": len(payload), "max_bytes": 262144}})
            assert not response.get("isError"), response
            assert all(block["type"] != "image" for block in response["content"])
            chunk = response["structuredContent"]["result"]
            data = base64.b64decode(chunk["data_base64"], validate=True)
            assert len(data) <= 262144 and chunk["offset"] == len(payload)
            payload.extend(data)
            assert chunk["next_offset"] == len(payload)
            if chunk["eof"]:
                break
        assert len(payload) == artifact["byte_count"] and hashlib.sha256(payload).hexdigest() == chunk["sha256"]
        assert json.loads(payload)["text"] == "x" * 600000
        eof = client.tool("snow_shot_artifact_read", {"artifact_id": artifact_id, "offset": len(payload)})["result"]
        assert eof["eof"] and eof["data_base64"] == ""
        client.tool("snow_shot_artifact_release", {"artifact_id": artifact_id})
        client.tool("snow_shot_artifact_read", {"artifact_id": artifact_id}, error="artifact_not_found")
        client.tool("snow_shot_document_close", arguments)
        return {"bytes": len(payload), "digest_verified": True, "ownership_and_release_verified": True}
    finally:
        other.close()
        client.close()


def complete_document_surface(client, directory):
    opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png")})
    arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
    def mutate(suffix, **fields):
        result = client.tool("snow_shot_document_" + suffix, dict(arguments, **fields))
        if result.get("revision") is not None:
            arguments["expected_revision"] = result["revision"]
        return result
    mutate("set_selection", type="rectangle", bounds=[0, 0, 80, 60])
    mutate("set_selection_style", corner_radius=0, shadow_width=0, aspect_ratio_locked=False)
    assert mutate("set_tool", tool="rectangle")["result"]["tool"] == "rectangle"
    mutate("set_tool_style", target="rectangle", style={"stroke_width": 3, "stroke": [255, 0, 0, 255]})
    annotated = mutate("apply_annotations", version=1, operations=[
        {"type": "rectangle", "bounds": [8, 8, 20, 15], "style": {"stroke": [255, 0, 0, 255], "stroke_width": 3}}])
    selected_id = annotated["result"]["transaction"]["created_element_ids"][0]
    mutate("edit_elements", action="select", id=selected_id)
    mutate("edit_elements", action="opacity", opacity=.75)
    template = mutate("draw_template", action="export")["result"]["payload"]
    assert json.loads(template)["elements"]
    mutate("draw_template", action="insert", payload=template)
    assert mutate("sample_color", point=[0, 0])["result"]["rgba"] == [255, 255, 255, 255]
    cloned = mutate("clone")
    # Cloning returns the clone revision; refresh the unchanged original separately.
    original = client.tool("snow_shot_document_state", {"document_id": arguments["document_id"]})
    arguments["expected_revision"] = original["revision"]
    clone_args = {"document_id": cloned["result"]["document_id"], "expected_revision": cloned["revision"]}
    before = client.tool("snow_shot_document_render", clone_args)["result"]["sha256"]
    erased = client.tool("snow_shot_document_edit_elements", dict(clone_args, action="erase_path", points=[[8, 8], [8, 20]]))
    clone_args["expected_revision"] = erased["revision"]
    assert client.tool("snow_shot_document_render", clone_args)["result"]["sha256"] != before
    undone = client.tool("snow_shot_document_undo", clone_args)
    clone_args["expected_revision"] = undone["revision"]
    assert client.tool("snow_shot_document_render", clone_args)["result"]["sha256"] == before
    redone = client.tool("snow_shot_document_redo", clone_args)
    clone_args["expected_revision"] = redone["revision"]
    client.tool("snow_shot_document_close", clone_args)
    mutate("copy")
    assert mutate("pin")["result"]["pinned"]
    assert mutate("present")["result"]["presented"]
    previous = arguments["expected_revision"]
    mutate("recapture", source="file", path=str(directory / "source.png"))
    assert arguments["expected_revision"] > previous
    job = mutate("auto_filter", categories=["text"])["result"]
    assert any(item["job_id"] == job["job_id"] for item in client.tool("snow_shot_job_list")["result"]["jobs"])
    deadline = time.monotonic() + 10
    while job["status"] == "running" and time.monotonic() < deadline:
        time.sleep(.025)
        job = client.tool("snow_shot_job_get", {"job_id": job["job_id"]})["result"]
    assert job["status"] == "completed", job
    resource = client.request("resources/read", {"uri": "snow-shot://jobs/" + job["job_id"]})
    assert json.loads(resource["contents"][0]["text"])["result"]["status"] == "completed"
    arguments["expected_revision"] = client.tool("snow_shot_document_state", {"document_id": arguments["document_id"]})["revision"]
    mutate("close")
    original = client.tool("snow_shot_document_open", {"source": "text", "text": "Original fixture content"})
    original_args = {"document_id": original["result"]["document_id"], "expected_revision": original["revision"]}
    exported = client.tool("snow_shot_document_original_content", dict(original_args, output="return", format="text"))
    assert exported["result"]["text"] == "Original fixture content"
    client.tool("snow_shot_document_close", original_args)


def cleanup_cycles(client, fixture, directory):
    result = []
    for cycle in range(4):
        before = process_metrics(fixture)
        opened = client.tool("snow_shot_document_open", {"path": str(directory / "4k.png")})
        arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
        client.tool("snow_shot_document_render", arguments)
        loaded = process_metrics(fixture)
        client.tool("snow_shot_document_close", arguments)
        assert not client.tool("snow_shot_document_list")["result"]["documents"]
        time.sleep(.2)
        result.append({"cycle": cycle, "fixture_before": before, "fixture_loaded": loaded,
                       "fixture_after_close": process_metrics(fixture), "bridge": process_metrics(client.process)})
    return result


def cache_work_counters(client, directory):
    def snapshot():
        metrics_path = directory / "metrics.json"
        def read():
            try:
                return json.loads(metrics_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                return {}
        previous = read().get("ticks", 0)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            value = read()
            if value.get("ticks", 0) > previous:
                return {key: value[key] for key in ("document_renders", "png_encodes")}
            time.sleep(.025)
        raise AssertionError("Fixture counters did not flush")
    opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png")})
    arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
    try:
        before = snapshot()
        cold = client.tool("snow_shot_document_render", arguments)
        after_cold = snapshot()
        assert all(after_cold[key] == before[key] + 1 for key in before), (before, after_cold)
        for index in range(3):
            warm = client.tool("snow_shot_document_render", arguments)
            assert warm["result"]["cache_hit"] and warm["result"]["sha256"] == cold["result"]["sha256"]
            saved = client.tool("snow_shot_document_save", dict(arguments,
                path=str(directory / f"counter-cache-{index}.png"), format="png"))
            assert saved["result"]["sha256"] == cold["result"]["sha256"]
        after_warm = snapshot()
        assert after_warm == after_cold, (after_cold, after_warm)
        changed = client.tool("snow_shot_document_apply_annotations", dict(arguments,
            operations=[{"type": "rectangle", "bounds": [4, 4, 20, 20]}]))
        arguments["expected_revision"] = changed["revision"]
        revised = client.tool("snow_shot_document_render", arguments)
        assert revised["result"]["sha256"] != cold["result"]["sha256"]
        after_revision = snapshot()
        assert all(after_revision[key] == after_warm[key] + 1 for key in after_warm), (after_warm, after_revision)
        return {"before": before, "cold": after_cold, "warm_render_and_save": after_warm,
                "changed_revision": after_revision,
                "warm_render_calls": 3, "warm_save_calls": 3,
                "scope": "Actual worker raster and PNG encoder invocation counters; heartbeat flush waits excluded from latency samples"}
    finally:
        client.tool("snow_shot_document_close", arguments)


def recognition_workflows(client, directory):
    opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png")})
    arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
    recognition_revision = 0
    def await_job(response):
        identifier = response["result"]["job_id"]
        deadline = time.monotonic() + 15
        while True:
            job = client.tool("snow_shot_job_get", {"job_id": identifier})["result"]
            if job["status"] != "running":
                assert job["status"] == "completed", job
                return job["result"]
            assert time.monotonic() < deadline, job
            time.sleep(.025)
    def recognize(kind):
        nonlocal recognition_revision
        value = await_job(client.tool("snow_shot_document_recognize", dict(arguments, kind=kind)))
        assert value["kind"] == kind, value
        recognition_revision = value["recognition_revision"]
        return value
    def edit(action, **fields):
        nonlocal recognition_revision
        value = client.tool("snow_shot_document_edit_recognition", dict(arguments,
            expected_recognition_revision=recognition_revision, action=action, **fields))["result"]
        assert value["recognition_revision"] > recognition_revision
        recognition_revision = value["recognition_revision"]
        return value
    def export(format, output="return", path=None):
        return client.tool("snow_shot_document_export_recognition", dict(arguments,
            format=format, output=output, **({"path": str(path)} if path else {})))["result"]
    try:
        text = recognize("text")
        assert text["text"] == "Fixture recognized text"
        edited = edit("set_text", text="A,B!")
        assert edited["text"] == "A,B!"
        assert edit("punctuation", value="full")["text"] == "A，B！"
        assert edit("undo")["text"] == "A,B!"
        assert edit("redo")["text"] == "A，B！"
        assert edit("punctuation", value="half")["text"] == "A,B!"
        edit("set_text", text="Fixture one\nFixture two")
        assert "\n" not in edit("format", value="remove")["text"]
        assert edit("reset_text")["text"] == "Fixture recognized text"
        edit("show_original", enabled=True)
        assert export("text")["text"] == "Fixture recognized text"

        table = recognize("table")
        assert (table["rows"], table["columns"], len(table["cells"])) == (2, 2, 4), table
        changed = edit("set_cell", row=1, column=0, text="Changed cell")
        assert any(cell["row"] == 1 and cell["column"] == 0 and cell["text"] == "Changed cell"
                   for cell in changed["cells"])
        edit("select_cells", range=[0, 0, 0, 1])
        merged = edit("merge_cells")
        assert len(merged["cells"]) == 3 and merged["cells"][0]["column_span"] == 2, merged
        assert len(edit("undo")["cells"]) == 4
        assert len(edit("redo")["cells"]) == 3
        split = edit("split_cells")
        assert len(split["cells"]) == 4 and all(cell["column_span"] == 1 for cell in split["cells"])
        restored = edit("reset_table")
        assert [cell["text"] for cell in restored["cells"]] == ["A", "B", "C", "D"]
        assert "<table" in export("html")["html"]
        table_path = directory / "recognized-table.html"
        export("html", "save", table_path)
        assert "<table" in table_path.read_text(encoding="utf-8")
        assert export("html", "copy")["copied"]

        qr = recognize("qr")
        assert qr["contents"] == ["fixture-qr-payload"]
        assert export("text")["text"] == "fixture-qr-payload"
        for kind, expected in (("markdown", "# Fixture heading\n\nFixture **content**."),
                               ("html", "<h1>Fixture heading</h1><p>Fixture <strong>content</strong>.</p>")):
            converted = recognize(kind)
            assert converted[kind] == expected, converted
            assert export(kind)[kind] == expected
            path = directory / ("converted." + ("md" if kind == "markdown" else "html"))
            export(kind, "save", path)
            assert path.read_text(encoding="utf-8") == expected

        catalog = client.tool("snow_shot_translation_catalog")["result"]
        assert any(model["id"] == "fixture-translation" for model in catalog["models"]), catalog
        for source in ({"texts": [text["text"]]}, {"source": "selection"}):
            translated = await_job(client.tool("snow_shot_translation_start", dict(source,
                model_id="fixture-translation", source_language="en", target_language="ja")))
            assert translated["units"][0]["text"] == "Fixture translated text", translated
        return {"modes": ["text", "table", "qr", "markdown", "html"],
                "table_operations": ["set_cell", "select_cells", "merge_cells", "undo", "redo", "split_cells", "reset_table"],
                "translations": ["recognized_document_text", "selected_text_port"],
                "provider_scope": "Actual HTTP/streaming and recognition controllers; deterministic loopback content and selected-text port"}
    finally:
        client.tool("snow_shot_document_close", arguments)


def slow_reader_controls(executable, descriptor, directory):
    environment = dict(os.environ, SNOW_SHOT_MCP_DESCRIPTOR=str(descriptor))
    process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True, encoding="utf-8", env=environment)
    observer = Client(executable, descriptor)
    def send(identifier, method, params):
        process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params}) + "\n")
        process.stdin.flush()
    def response(identifier):
        while True:
            value = json.loads(process.stdout.readline())
            if value.get("id") == identifier:
                assert "error" not in value, value
                return value["result"]
    try:
        send(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "slow-reader-fixture", "version": "1"}})
        response(1)
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        process.stdin.flush()
        send(2, "tools/call", {"name": "snow_shot_document_open", "arguments": {"path": str(directory / "4k.png")}})
        opened = response(2)["structuredContent"]
        args = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
        started = time.monotonic()
        for identifier in range(3, 23):
            send(identifier, "tools/call", {"name": "snow_shot_document_render", "arguments": args})
        peaks, control_ms = [], []
        while process.poll() is None and time.monotonic() - started < 18:
            metrics = process_metrics(process)
            if metrics:
                peaks.append(metrics)
            instant = time.perf_counter()
            observer.tool("snow_shot_status")
            control_ms.append((time.perf_counter() - instant) * 1000)
            time.sleep(.1)
        assert process.poll() is not None, "Non-reading peer did not retire within 18 seconds"
        assert max(control_ms) < 5000, control_ms
        return {"exit_ms": (time.monotonic() - started) * 1000,
                "control_count": len(control_ms), "control_max_ms": max(control_ms),
                "memory_samples": peaks, "scope": "Independent client's status remains reachable while another stdout is blocked"}
    finally:
        observer.close()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        process.stdin.close()
        process.stdout.close()


def write_noise_png(path, width, height):
    """Deterministic incompressible RGB exercises byte/encode pressure, not just dimensions."""
    generator = random.Random(20260926)
    def chunk(stream, kind, data):
        stream.write(struct.pack(">I", len(data)) + kind + data +
                     struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    compressor = zlib.compressobj(1)
    with path.open("wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        chunk(stream, b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        for _ in range(height):
            compressed = compressor.compress(b"\0" + generator.randbytes(width * 3))
            if compressed:
                chunk(stream, b"IDAT", compressed)
        chunk(stream, b"IDAT", compressor.flush())
        chunk(stream, b"IEND", b"")


def large_exports(client, directory, samples):
    for name, width, height in [("1080p", 1920, 1080), ("4k", 3840, 2160)]:
        source = directory / (name + ".png")
        write_noise_png(source, width, height)
        opened = client.tool("snow_shot_document_open", {"path": str(source)}, label=name + "_open")
        arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
        first = client.tool("snow_shot_document_render", arguments, label=name + "_render_cold")
        assert (first["result"]["width"], first["result"]["height"]) == (width, height)
        for index in range(min(samples, 5)):
            cached = client.tool("snow_shot_document_render", arguments, label=name + "_render_cached")
            assert cached["result"]["cache_hit"] and cached["result"]["sha256"] == first["result"]["sha256"]
            client.tool("snow_shot_document_save", dict(arguments,
                        path=str(directory / f"{name}-export-{index}.png")), label=name + "_save")
        client.tool("snow_shot_document_close", arguments)


def concurrent_clients(executable, descriptor, directory):
    def exercise(index):
        client = Client(executable, descriptor)
        try:
            opened = client.tool("snow_shot_document_open", {"path": str(directory / "source.png")})
            arguments = {"document_id": opened["result"]["document_id"], "expected_revision": opened["revision"]}
            before = process_metrics(client.process)
            for _ in range(10):
                client.tool("snow_shot_document_render", arguments, label="concurrent_render")
            listed = client.tool("snow_shot_document_list")["result"]["documents"]
            assert len(listed) == 1 and listed[0]["document_id"] == arguments["document_id"], listed
            client.tool("snow_shot_document_close", arguments)
            assert not client.tool("snow_shot_document_list")["result"]["documents"]
            return {"client": index, **summarize(client, before, process_metrics(client.process))}
        finally:
            client.close()
    with ThreadPoolExecutor(max_workers=4) as executor:
        return list(executor.map(exercise, range(4)))


def legacy_workflow(client, directory, samples, current, size=(800, 600)):
    for index in range(samples):
        started = time.perf_counter()
        state = client.tool("screenshot_begin", {"presentation": "silent", "target": "all_displays"}, label="legacy_begin")
        arguments = {"session_id": state["session_id"], "expected_revision": state["revision"]}
        def mutate(name, extra=None, label=None):
            result = client.tool(name, dict(arguments, **(extra or {})), label=label)
            if "revision" in result:
                arguments["expected_revision"] = result["revision"]
            return result
        mutate("screenshot_set_selection", {"type": "rectangle", "bounds": [0, 0, *size]})
        mutate("screenshot_set_tool", {"tool": "rectangle"})
        mutate("screenshot_apply_annotations", {"version": 1, "operations": [
            {"type": "rectangle", "bounds": [10, 10, 50, 40],
             "style": {"stroke": [255, 0, 0, 255], "stroke_width": 2}}]}, label="legacy_annotations")
        mutate("screenshot_undo")
        mutate("screenshot_redo")
        first = mutate("screenshot_render", label="legacy_render_cold")
        cached = mutate("screenshot_render", label="legacy_render_cached")
        assert first["result"]["sha256"] == cached["result"]["sha256"], cached
        if current:
            assert cached["result"]["timings_ms"].get("cache_hit"), cached
            assert "encode" not in cached["result"]["timings_ms"], cached
        mutate("screenshot_save", {"path": str(directory / f"legacy-{index}.png"), "format": "png"}, label="legacy_save")
        mutate("screenshot_copy")  # Fixture acknowledgments; not native clipboard coverage.
        mutate("screenshot_pin")   # Fixture acknowledgments; not native pinned-window coverage.
        mutate("screenshot_finish", {"output": "none"})
        client.samples.setdefault("legacy_complete_workflow", []).append((time.perf_counter() - started) * 1000)


def legacy_wait_operation(client, session, operation):
    deadline = time.monotonic() + 10
    while True:
        result = client.tool("screenshot_operation", {"session_id": session, "operation_id": operation})
        if result["result"]["status"] != "running" or time.monotonic() >= deadline:
            break
        time.sleep(.025)
    assert result["result"]["status"] == "completed", result
    return result


def legacy_contract_outcomes(client, directory, resources=False):
    fixtures = json.loads(Path(__file__).with_name("mcp_contract_fixtures.json").read_text(encoding="utf-8"))["fixtures"]
    outcomes = {}
    for case in fixtures:
        client.tool("screenshot_cancel")
        name = case["name"]
        arguments = dict(case["arguments"])
        if name not in ("snow_shot_status", "screenshot_begin", "screenshot_direct_capture"):
            opened = client.tool("screenshot_begin", {"presentation": "silent", "target": "all_displays"})
            if "session_id" in arguments:
                arguments["session_id"] = opened["session_id"]
            if "expected_revision" in arguments:
                arguments["expected_revision"] = opened["revision"]
            setup = {"session_id": opened["session_id"], "expected_revision": opened["revision"]}
            if name == "screenshot_cancel":
                client.tool(name, dict(arguments), error="request_not_found")
                arguments.pop("request_id")
            if name in ("screenshot_edit_recognition", "screenshot_export_recognition", "screenshot_operation"):
                operation = client.tool("screenshot_recognize", dict(setup, kind="text"))
                completed = legacy_wait_operation(client, opened["session_id"], operation["result"]["operation_id"])
                if name == "screenshot_operation":
                    arguments["operation_id"] = operation["result"]["operation_id"]
                else:
                    arguments["expected_revision"] = completed["revision"]
            if name == "screenshot_scroll_once":
                scrolling = client.tool("screenshot_scrolling", dict(setup, action="start", axis="vertical"))
                arguments["expected_revision"] = scrolling["revision"]
            if name in ("screenshot_edit_elements", "screenshot_draw_template"):
                drawn = client.tool("screenshot_apply_annotations", dict(setup, version=1, operations=[
                    {"type": "rectangle", "bounds": [10, 10, 20, 20]}]))
                setup["expected_revision"] = drawn["revision"]
                selected = client.tool("screenshot_edit_elements", dict(setup, action="select",
                    id=drawn["result"]["transaction"]["created_element_ids"][0]))
                arguments["expected_revision"] = selected["revision"]
        if "path" in arguments:
            arguments["path"] = str(directory / (name + ".png"))
        result = client.request("tools/call", {"name": name, "arguments": arguments})
        envelope = result["structuredContent"]
        assert {"protocol", "request_id", "ok", "result", "attachment_length"} <= envelope.keys(), envelope
        assert result.get("isError", False) == (not envelope["ok"]), result
        assert envelope["ok"], (name, envelope)
        if resources and name == "screenshot_state":
            resource = client.request("resources/read", {"uri": "snow-shot://screenshots/" + arguments["session_id"]})
            assert json.loads(resource["contents"][0]["text"])["session_id"] == arguments["session_id"]
        if name in ("screenshot_recognize", "screenshot_translate", "screenshot_auto_filter"):
            completed = legacy_wait_operation(client, envelope["session_id"], envelope["result"]["operation_id"])
            assert completed["result"]["result"]["fixture_provider"] is True
        if name == "screenshot_draw_template":
            template = json.loads(envelope["result"]["payload"])
            assert template["schemaVersion"] == 1 and len(template["elements"]) == 1
        if name == "screenshot_scroll_once":
            assert envelope["result"]["scroll_steps"] == 1
        if name == "screenshot_edit_recognition":
            assert envelope["result"]["text"] == "Fixture text"
        if name == "screenshot_export_recognition":
            assert envelope["result"]["text"] == "Fixture recognized text"
        outcomes[name] = {"ok": envelope["ok"], "error": envelope.get("error", {}).get("code"),
                          "envelope_keys": sorted(envelope.keys()),
                          "result_keys": sorted(key for key in envelope["result"]
                                                if key not in ("timings_ms", "bridge_total_ms")),
                          "image_contract": {key: envelope["result"][key]
                                             for key in ("width", "height", "format", "byte_count", "sha256")
                                             if key in envelope["result"]},
                          "content_types": [block["type"] for block in result["content"]]}
    client.tool("screenshot_cancel")
    assert len(outcomes) == 28
    return outcomes


def legacy_comparison(args):
    comparisons = {}
    versions = [
        ("baseline", args.baseline_bridge, args.baseline_legacy_fixture),
        ("current", args.bridge, args.legacy_fixture),
    ]
    for label, executable, fixture_path, size in [
        (label, executable, fixture_path, size)
        for size in [(800, 600), (1920, 1080), (3840, 2160)]
        for label, executable, fixture_path in versions
    ]:
        if not executable or not fixture_path:
            continue
        with tempfile.TemporaryDirectory(prefix="snow-shot-mcp-legacy-") as temporary:
            directory = Path(temporary)
            descriptor = directory / "snow-shot-mcp.json"
            with (directory / "fixture.log").open("w", encoding="utf-8") as log:
                fixture = subprocess.Popen([str(fixture_path), "-platform", "offscreen", "--serve", str(directory),
                                            "--size", f"{size[0]}x{size[1]}"],
                                           stdout=log, stderr=log)
                client = None
                try:
                    deadline = time.monotonic() + 20
                    while not descriptor.is_file() and fixture.poll() is None and time.monotonic() < deadline:
                        time.sleep(.05)
                    assert descriptor.is_file(), (directory / "fixture.log").read_text(encoding="utf-8")
                    fixture_before = process_metrics(fixture)
                    client = Client(executable, descriptor)
                    before = process_metrics(client.process)
                    contracts = legacy_contract_outcomes(client, directory, label == "current") if size == (800, 600) else None
                    legacy_workflow(client, directory, args.samples, label == "current", size)
                    entry = summarize(client, before, process_metrics(client.process))
                    comparisons.setdefault(label, {})[f"{size[0]}x{size[1]}"] = entry
                    entry["legacy_contract_outcomes"] = contracts
                    entry["fixture_before"] = fixture_before
                    entry["fixture_after"] = process_metrics(fixture)
                    entry["fixture_sha256"] = hashlib.sha256(fixture_path.read_bytes()).hexdigest()
                    metrics = directory / "metrics.json"
                    entry["gui_thread"] = json.loads(metrics.read_text()) if metrics.is_file() else None
                finally:
                    if client:
                        client.close()
                    fixture.terminate()
                    fixture.wait(timeout=10)
    if "baseline" in comparisons and "current" in comparisons:
        for name, original in comparisons["baseline"]["800x600"]["legacy_contract_outcomes"].items():
            current = comparisons["current"]["800x600"]["legacy_contract_outcomes"][name]
            for key in ("ok", "error", "content_types", "image_contract"):
                assert original[key] == current[key], (name, key, original, current)
            for key in ("envelope_keys", "result_keys"):
                assert set(original[key]) <= set(current[key]), (name, key, original, current)
    return comparisons


def run(args, provider):
    with tempfile.TemporaryDirectory(prefix="snow-shot-mcp-real-") as temporary:
        directory = Path(temporary)
        descriptor = directory / "snow-shot-mcp.json"
        with (directory / "fixture.log").open("w", encoding="utf-8") as log:
            fixture = subprocess.Popen([str(args.fixture), "-platform", "offscreen", "--serve", str(directory)],
                                       stdout=log, stderr=log,
                                       env=dict(os.environ, SNOW_SHOT_MCP_FIXTURE_API=provider.url))
            report = {"fixture": str(args.fixture), "bridge": str(args.bridge),
                      "measured_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                      "scope": "Synthetic Qt ports in offscreen-capable tests; this Windows kit may select its Windows QPA fallback; not native capture/provider/encoder coverage",
                      "baseline_scope": "Same Qt fixture, different bridge binaries only",
                      "environment": {"platform": platform.platform(), "machine": platform.machine(),
                                      "processor": platform.processor(),
                                      "logical_cpus": os.cpu_count(), "python": platform.python_version()},
                      "benchmark": args.benchmark,
                      "samples": args.samples,
                      "binaries": {label: {"path": str(path), "bytes": path.stat().st_size,
                                           "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                                   for label, path in [("fixture", args.fixture), ("current_bridge", args.bridge),
                                                       ("baseline_bridge", args.baseline_bridge)] if path}}
            try:
                deadline = time.monotonic() + 20
                while not descriptor.is_file() and fixture.poll() is None and time.monotonic() < deadline:
                    time.sleep(.05)
                assert descriptor.is_file(), (directory / "fixture.log").read_text(encoding="utf-8")
                fixture_before = process_metrics(fixture)
                for label, executable in [("baseline", args.baseline_bridge), ("current", args.bridge)]:
                    if not executable:
                        continue
                    client = Client(executable, descriptor)
                    try:
                        before = process_metrics(client.process)
                        client.request("tools/list")  # Warm schema and transport state.
                        client.tool("snow_shot_status")
                        if label == "current":
                            for uri in ("snow-shot://capabilities", "snow-shot://application/status", "snow-shot://settings"):
                                resource = client.request("resources/read", {"uri": uri})
                                assert isinstance(json.loads(resource["contents"][0]["text"]), dict)
                        for _ in range(args.samples):
                            client.request("tools/list", label="tools_list_warm")
                            client.tool("snow_shot_status", label="legacy_status_warm")
                        if label == "current":
                            document_workflow(client, directory, args.samples)
                            complete_document_surface(client, directory)
                            report["recognition"] = recognition_workflows(client, directory)
                            report["cache_work"] = cache_work_counters(client, directory)
                            if args.benchmark:
                                large_exports(client, directory, args.samples)
                                report["cleanup_cycles"] = cleanup_cycles(client, fixture, directory)
                        report[label] = summarize(client, before, process_metrics(client.process))
                    finally:
                        client.close()
                modern_task_workflow(args.bridge, descriptor, directory)
                report["artifacts"] = artifact_workflow(args.bridge, descriptor, directory)
                report["concurrent_clients"] = concurrent_clients(args.bridge, descriptor, directory)
                if args.benchmark:
                    report["slow_reader"] = slow_reader_controls(args.bridge, descriptor, directory)
                report["fixture_before"] = fixture_before
                report["fixture_after"] = process_metrics(fixture)
                metrics_file = directory / "metrics.json"
                report["gui_thread"] = json.loads(metrics_file.read_text()) if metrics_file.is_file() else None
                report["gui_metric_limit"] = "cpu_ms/cpu_utilization use GUI thread kernel CPU counters; timer lateness independently measures responsiveness. Process RSS/private-byte retention may include allocator caches, not proof of a leak."
                report["provider_calls"] = list(provider.calls)
                assert {"table", "markdown", "html", "translation"} <= set(provider.calls), provider.calls
            finally:
                fixture.terminate()
                fixture.wait(timeout=10)
            report["legacy_comparison"] = legacy_comparison(args)
            report["legacy_scope"] = "Original/current Qt server+session and bridge with identical synthetic capture/copy/pin ports and common renderer; not native capture performance"
            report["executed_tools"] = sorted(Client.executed_tools)
            report["read_resources"] = sorted(Client.read_resources)
            if args.output:
                args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bridge", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("fixture", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("--baseline-bridge", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("--legacy-fixture", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("--baseline-legacy-fixture", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--benchmark", action="store_true", help="Requires Release performance-preset binaries")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.samples < 1 or args.samples > 10000:
        parser.error("samples must be between 1 and 10000")
    if args.baseline_bridge and not args.benchmark:
        parser.error("baseline comparisons require --benchmark and performance-preset Release binaries")
    with RecognitionProvider() as provider:
        if os.name == "nt":
            from mcp_live_tests import ClipboardGuard
            with ClipboardGuard():
                run(args, provider)
        else:
            run(args, provider)


if __name__ == "__main__":
    main()
