"""Production ApplicationController router coverage with isolated temporary storage.

Exercises real media models, file outputs, and a local HTTP translation fixture
without external providers or recording devices by default. --recording adds a
brief native desktop region recording with both audio inputs disabled.
"""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time
import zlib
import zipfile

from mcp_fixture_tests import Client


class LocalProvider:
    """Exercise the actual asynchronous network provider with bounded loopback replies."""
    def __enter__(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                if not self.path.startswith("/api/v2/chat/models"):
                    self.send_error(404)
                    return
                body = json.dumps({"data": [{"model": "fixture", "name": "Local fixture",
                                             "translation_mode": "default"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                if self.path != "/api/v1/chat/completions" or size > 65536:
                    self.send_error(400)
                    return
                json.loads(self.rfile.read(size))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                for content in ("Local ", "translation"):
                    self.wfile.write(b"data: " + json.dumps(
                        {"choices": [{"delta": {"content": content}}]}).encode() + b"\n\n")
                    self.wfile.flush()
                    time.sleep(.025)
                self.wfile.write(b"data: [DONE]\n\n")
                self.close_connection = True

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return f"http://127.0.0.1:{self.server.server_port}"

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def exercise_administration(client, directory):
    permissions = client.tool("snow_shot_permissions_request", {
        "permission": "screen-recording", "open_settings": False})["result"]["permissions"]
    assert any(item["id"] == "screen-recording" for item in permissions)
    # Cancel is a successful no-op when the updater has no active operation.
    assert client.tool("snow_shot_updates_action", {"action": "cancel"})["result"]["state"]
    target = directory / "settings-action-export.zip"
    assert client.tool("snow_shot_settings_action", {
        "field_id": "configuration.export", "path": str(target),
        "expected_revision": client.tool("snow_shot_settings_get")["revision"]})["result"]["credentials_redacted"]
    assert target.is_file()
    deadline = time.monotonic() + 10
    while True:
        catalog = client.tool("snow_shot_translation_catalog")["result"]
        if any(model["id"] == "fixture" for model in catalog["models"]):
            break
        assert time.monotonic() < deadline, catalog
        time.sleep(.02)
    assert catalog["languages"]
    job = await_job(client, client.tool("snow_shot_translation_start", {
        "texts": ["hello"], "model_id": "fixture", "source_language": "en", "target_language": "ja"}))
    assert job["result"]["units"][0]["text"] == "Local translation", job
    retry = await_job(client, client.tool("snow_shot_translation_start", {"retry_job_id": job["job_id"]}))
    assert retry["result"]["units"][0]["text"] == "Local translation", retry


def exercise_modern_administration(bridge, descriptor):
    client = Client(bridge, descriptor, modern=True, tasks=True)
    try:
        created = client.tool("snow_shot_translation_start", {
            "texts": ["hello"], "model_id": "fixture", "source_language": "en", "target_language": "ja"})
        assert created["resultType"] == "task", created
        deadline = time.monotonic() + 10
        while True:
            task = client.request("tasks/get", {"taskId": created["taskId"]})
            if task["status"] != "working":
                break
            assert time.monotonic() < deadline, task
            time.sleep(.025)
        assert task["status"] == "completed" and "Local translation" in json.dumps(task), task
    finally:
        client.close()


def png(path, width=80, height=60):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    pixels = b"".join(b"\0" + bytes((40, 150, 210, 255)) * width for _ in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def dimensions(path):
    data = path.read_bytes()
    assert data.startswith(b"\x89PNG\r\n\x1a\n"), path
    return struct.unpack(">II", data[16:24])


def await_job(client, response):
    identifier = response["result"]["job_id"]
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        job = client.tool("snow_shot_job_get", {"job_id": identifier})["result"]
        if job["status"] != "running":
            assert job["status"] == "completed", job
            return job
        time.sleep(.02)
    raise AssertionError(f"job {identifier} did not complete")


def resource(client, uri):
    response = client.request("resources/read", {"uri": uri})
    contents = response["contents"]
    assert len(contents) == 1 and contents[0]["uri"] == uri, response
    result = json.loads(contents[0]["text"])
    assert result["ok"], result
    return result


def exercise_recording(client, directory):
    display = client.tool("snow_shot_app_displays")["result"]["displays"][0]
    bounds = display.get("physical_bounds", display["logical_bounds"])
    target = directory / "fixture-recording.mp4"
    client.tool("snow_shot_recording_start", {
        "region": [bounds[0] + 10, bounds[1] + 10, 160, 120],
        "options": {"path": str(target), "format": "mp4", "frame_rate": 5,
                    "clarity": "480p", "encoder": "h264", "encoding_preset": "ultrafast",
                    "microphone": False, "system_audio": False, "show_cursor": False,
                    "show_keyboard": False, "start_delay_seconds": 0}})

    def state():
        value = client.tool("snow_shot_recording_state")
        assert not value["result"].get("error"), value
        return value

    def wait_for(predicate):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            value = state()
            if predicate(value["result"]):
                return value
            time.sleep(.05)
        raise AssertionError(f"recording did not reach expected state: {value}")

    def control(action, **arguments):
        value = state()
        return client.tool("snow_shot_recording_control", {
            "recording_id": value["result"]["recording_id"],
            "expected_revision": value["revision"], "action": action, **arguments})

    wait_for(lambda value: value["state"] == "recording" and not value["busy"])
    # Recording, capture acquisition and background editing have distinct leases.
    captured = await_job(client, client.tool("snow_shot_document_open", {
        "source": "capture", "target": "current_monitor", "as_job": True}))["result"]
    client.tool("snow_shot_document_close", {"document_id": captured["document_id"],
                "expected_revision": captured["revision"]})
    time.sleep(.35)
    control("pause")
    wait_for(lambda value: value["state"] == "paused")
    control("annotations", payload={"version": 1, "operations": [
        {"type": "rectangle", "bounds": [bounds[0] + 25, bounds[1] + 25, 30, 20]}]})
    control("resume")
    time.sleep(.35)
    control("stop")
    final = wait_for(lambda value: value["finalized"] and value.get("artifact_status") == "ready")
    assert final["result"]["duration_ms"] > 0, final
    assert target.is_file() and target.stat().st_size > 32
    artifact = final["result"]["artifact"]
    assert artifact["mime_type"] == "video/mp4" and artifact["byte_count"] == target.stat().st_size
    expected = hashlib.sha256(target.read_bytes()).hexdigest()
    assert artifact["sha256"] == expected
    # The retained artifact must not change if the user's destination is replaced.
    target.write_bytes(b"changed after finalization")
    read = client.request("tools/call", {"name": "snow_shot_artifact_read", "arguments": {
        "artifact_id": artifact["artifact_id"], "offset": 0, "max_bytes": 64}})
    assert not read.get("isError"), read
    assert read["structuredContent"]["result"]["sha256"] == expected
    control("close")


def exercise_application(client, directory):
    exercise_administration(client, directory)
    assert client.tool("snow_shot_app_status")["result"]["storage"]["write_available"]
    assert client.tool("snow_shot_app_displays")["result"]["displays"]
    assert len(client.tool("snow_shot_permissions_get")["result"]["permissions"]) == 4
    assert client.tool("snow_shot_updates_status")["result"]["state"]
    client.sequence += 1
    canceled_request = client.sequence
    client.send({"jsonrpc": "2.0", "id": canceled_request, "method": "tools/call", "params": {
        "name": "snow_shot_document_open", "arguments": {
            "source": "capture", "target": "current_monitor", "delay_seconds": 2,
            "as_job": False}}})
    time.sleep(.1)
    client.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                 "params": {"requestId": canceled_request}})
    time.sleep(2.2)
    assert not client.tool("snow_shot_document_list")["result"]["documents"]
    scheduled = client.tool("snow_shot_document_open", {
        "source": "capture", "target": "current_monitor", "delay_seconds": 2, "as_job": True})
    job_id = scheduled["result"]["job_id"]
    client.tool("snow_shot_job_cancel", {"job_id": job_id})
    assert client.tool("snow_shot_job_get", {"job_id": job_id})["result"]["status"] == "canceled"
    time.sleep(2.2)
    assert not client.tool("snow_shot_document_list")["result"]["documents"]
    settings = client.tool("snow_shot_settings_get")
    # Provider-backed imports use the same preallocation admission as files.
    # A rejected provider must release its reservation so a later import works.
    text_documents = [client.tool("snow_shot_document_open", {
        "source": "text", "text": f"Source budget fixture {index}"}) for index in range(4)]
    client.tool("snow_shot_document_open", {"source": "html", "html": "<b>Limited</b>"},
                error="resource_limit")
    first = text_documents.pop()
    client.tool("snow_shot_document_close", {"document_id": first["result"]["document_id"],
                "expected_revision": first["revision"]})
    text_documents.append(client.tool("snow_shot_document_open", {
        "source": "html", "html": "<b>Released provider budget</b>"}))
    for document in text_documents:
        client.tool("snow_shot_document_close", {"document_id": document["result"]["document_id"],
                    "expected_revision": document["revision"]})
    fields = {field["id"]: field for field in settings["result"]["fields"]}
    automatic_ocr = fields["pin-to-screen.automatic-text-recognition"]

    def revision():
        return client.tool("snow_shot_settings_get")["revision"]

    initial = settings["revision"]
    client.tool("snow_shot_settings_update", {"expected_revision": initial,
                "values": {automatic_ocr["id"]: True}})
    client.tool("snow_shot_settings_update", {"expected_revision": initial,
                "values": {automatic_ocr["id"]: False}}, error="stale_revision")
    client.tool("snow_shot_settings_reset", {"expected_revision": revision(),
                "page_id": automatic_ocr["page_id"], "section_id": automatic_ocr["section_id"]})
    client.tool("snow_shot_settings_update", {"expected_revision": revision(),
                "values": {automatic_ocr["id"]: False}})
    delay = next(field for field in fields.values() if field["id"].endswith(".seconds"))
    client.tool("snow_shot_settings_update", {"expected_revision": revision(),
                "values": {delay["id"]: 3}})
    updated = client.tool("snow_shot_settings_get")["result"]["fields"]
    assert next(field for field in updated if field["id"] == delay["id"])["value"] == 3
    # Whole-patch structural validation must run before any field is applied.
    client.tool("snow_shot_settings_update", {"expected_revision": revision(),
                "values": {delay["id"]: 5, "unknown.fixture.setting": True}}, error="invalid_parameters")
    updated = client.tool("snow_shot_settings_get")["result"]["fields"]
    assert next(field for field in updated if field["id"] == delay["id"])["value"] == 3

    model = {"id": "95ca84ed-5dcb-42ea-bf6c-c47bf6e87d4b", "name": "Fixture model",
             "base_url": "https://example.invalid/v1", "model": "fixture",
             "supports_vision": False, "supports_reasoning": True}
    client.tool("snow_shot_models_update", {"expected_revision": revision(), "models": [model]})
    secret = "fixture-only-secret-do-not-expose"
    client.tool("snow_shot_credentials_set", {"expected_revision": revision(),
                "provider": model["id"], "secret": secret})
    listed = client.tool("snow_shot_models_list")
    assert listed["result"]["models"][0]["credential_set"]
    assert secret not in json.dumps(listed) and "api_key" not in json.dumps(listed)
    model["name"] = "Renamed fixture model"
    client.tool("snow_shot_models_update", {"expected_revision": revision(), "models": [model]})
    assert client.tool("snow_shot_models_list")["result"]["models"][0]["credential_set"]
    archive = directory / "configuration.zip"
    assert client.tool("snow_shot_configuration_export", {"path": str(archive)})["result"]["credentials_redacted"]
    with zipfile.ZipFile(archive) as contents:
        assert all(secret.encode() not in contents.read(name) for name in contents.namelist())
    await_job(client, client.tool("snow_shot_configuration_import", {
        "expected_revision": revision(), "path": str(archive)}))
    assert client.tool("snow_shot_models_list")["result"]["models"][0]["credential_set"]
    assert secret not in json.dumps(client.tool("snow_shot_settings_get"))
    client.tool("snow_shot_models_update", {"expected_revision": revision(), "models": []})

    client.tool("snow_shot_templates_update", {"expected_revision": revision(),
                "kind": "watermark", "templates": [{"name": "Fixture", "text": "Owned fixture"}]})
    assert client.tool("snow_shot_templates_list", {"kind": "watermark"})["result"]["templates"] == [
        {"name": "Fixture", "text": "Owned fixture"}]
    client.tool("snow_shot_templates_list", {"kind": "drawing"})

    history = client.tool("snow_shot_history_list", {"limit": 1})
    assert history["result"]["records"], history
    history_id = history["result"]["records"][0]["history_id"]
    assert resource(client, "snow-shot://history/" + history_id)["result"]["history_id"] == history_id
    previous_pins = {pin["id"] for pin in client.tool("snow_shot_pinned_list")["result"]["windows"]}
    client.tool("snow_shot_history_action", {"history_id": history_id, "action": "pin"})
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        pinned = [pin for pin in client.tool("snow_shot_pinned_list")["result"]["windows"]
                  if pin["id"] not in previous_pins]
        if pinned:
            break
        time.sleep(.05)
    assert len(pinned) == 1, pinned
    pinned_id = pinned[0]["id"]
    pinned_state = resource(client, "snow-shot://pinned/" + pinned_id)
    assert pinned_state["result"]["id"] == pinned_id
    client.tool("snow_shot_pinned_action", {"id": pinned_id,
                "expected_revision": pinned_state["revision"], "action": "destroy"})
    item = client.tool("snow_shot_history_get", {"history_id": history_id, "include_image": True})
    artifact = item["result"]["artifact"]
    assert artifact["mime_type"] == "image/png"
    chunks = client.request("tools/call", {"name": "snow_shot_artifact_read", "arguments": {
        "artifact_id": artifact["artifact_id"], "offset": 0, "max_bytes": 262144}})
    assert not chunks.get("isError"), chunks
    payload = base64.b64decode(chunks["structuredContent"]["result"]["data_base64"])
    assert payload.startswith(b"\x89PNG\r\n\x1a\n")
    assert hashlib.sha256(payload).hexdigest() == chunks["structuredContent"]["result"]["sha256"]
    assert client.tool("snow_shot_artifact_list")["result"]["artifacts"]
    client.tool("snow_shot_artifact_release", {"artifact_id": artifact["artifact_id"]})
    document = client.tool("snow_shot_document_open", {"source": "history", "source_id": history_id})
    client.tool("snow_shot_document_close", {"document_id": document["result"]["document_id"],
                "expected_revision": document["revision"]})
    await_job(client, client.tool("snow_shot_history_delete", {
        "history_id": history_id, "expected_revision": history["revision"]}))
    assert not client.tool("snow_shot_history_list")["result"]["records"]
    current_history = client.tool("snow_shot_history_list")
    await_job(client, client.tool("snow_shot_history_clear", {
        "expected_revision": current_history["revision"]}))
    # Empty caches are disabled in the shared settings action model.
    client.tool("snow_shot_storage_cleanup", {
        "kind": "thumbnail_cache", "expected_revision": revision()}, error="busy")
    await_job(client, client.tool("snow_shot_storage_cleanup", {
        "kind": "history", "expected_revision": revision()}))
    assert client.tool("snow_shot_storage_status")["result"]["write_available"]
    client.tool("snow_shot_settings_action", {"field_id": "unknown.fixture.action",
                "expected_revision": revision()}, error="invalid_parameters")


def exercise(client, directory):
    tools = {tool["name"] for tool in client.request("tools/list")["tools"]}
    assert "snow_shot_pinned_create" in tools and "snow_shot_recording_start" in tools
    exercise_application(client, directory)
    state = client.tool("snow_shot_recording_state")
    assert state["result"]["state"] == "idle" and not state["result"]["open"], state
    client.tool("snow_shot_recording_control", {"recording_id": "missing", "expected_revision": 1,
                "action": "stop"}, error="recording_not_found")
    groups = client.tool("snow_shot_group_list")
    original_group = groups["result"]["active_group_id"]
    created = client.tool("snow_shot_group_update", {"expected_revision": groups["revision"],
                          "action": "create", "name": "MCP fixture", "idempotency_key": "group-create"})
    custom = next(group["id"] for group in created["result"]["groups"] if group["name"] == "MCP fixture")
    client.tool("snow_shot_group_update", {"expected_revision": groups["revision"],
                "action": "activate", "group_id": custom}, error="stale_revision")
    created = client.tool("snow_shot_group_update", {"expected_revision": created["revision"],
                          "action": "activate", "group_id": custom})

    source = directory / "source.png"
    png(source)
    arguments = {"source": {"kind": "file", "path": str(source)}, "group_id": custom,
                 "idempotency_key": "pin-file"}
    pin = client.tool("snow_shot_pinned_create", arguments)
    identifier = pin["result"]["id"]
    repeated = client.tool("snow_shot_pinned_create", arguments)
    assert repeated["result"]["id"] == identifier
    assert pin["result"]["group_id"] == custom and pin["result"]["ready"]

    def pin_state():
        return client.tool("snow_shot_pinned_get", {"id": identifier})

    def mutate(tool, **kwargs):
        # Appearance events may finish asynchronously after showing a native pin.
        # The server must reject stale requests; a caller can explicitly re-read.
        for _ in range(5):
            current = pin_state()
            result = client.request("tools/call", {"name": tool, "arguments": dict(
                id=identifier, expected_revision=current["revision"], **kwargs)})
            content = result["structuredContent"]
            if not result.get("isError", False):
                return content
            assert content["error"]["code"] == "stale_revision", result
            time.sleep(.05)
        raise AssertionError("pin never reached a stable revision")

    initial = pin_state()
    mutate("snow_shot_pinned_edit", action="tool", payload={"tool": "arrow"})
    assert pin_state()["result"]["editing"] and pin_state()["result"]["active_tool"] == "arrow"
    mutate("snow_shot_pinned_edit", action="tool_style", payload={
        "target": "arrow", "style": {"arrow_shaft_type": "tapered", "arrow_ratio": 2.0}})
    mutate("snow_shot_pinned_edit", action="editing", payload={"enabled": False})
    assert not pin_state()["result"]["editing"]
    changed = mutate("snow_shot_pinned_update", properties={"opacity_percent": 70,
                     "rotation": "clockwise", "show_border": False})
    assert changed["revision"] > initial["revision"] and changed["result"]["quarter_turns"] == 1
    client.tool("snow_shot_pinned_update", {"id": identifier, "expected_revision": initial["revision"],
                "properties": {"opacity_percent": 50}}, error="stale_revision")
    # Cancellation suppresses artifact completion callbacks. Admission must be
    # released by cancellation itself, otherwise eight canceled saves fill the
    # client's queue permanently and the following normal save is rejected.
    for index in range(9):
        current = pin_state()
        client.sequence += 1
        request_id = client.sequence
        client.send({"jsonrpc": "2.0", "id": request_id, "method": "tools/call", "params": {
            "name": "snow_shot_pinned_export", "arguments": {
                "id": identifier, "expected_revision": current["revision"], "output": "save",
                "path": str(directory / f"cancelled-{index}.png")}}})
        client.send({"jsonrpc": "2.0", "method": "notifications/cancelled",
                     "params": {"requestId": request_id}})
        pin_state()
    original_path = directory / "pin-original.png"
    rendered_path = directory / "pin-rendered.png"
    mutate("snow_shot_pinned_export", output="save", original=True, path=str(original_path))
    mutate("snow_shot_pinned_export", output="save", path=str(rendered_path))
    assert dimensions(original_path) == (80, 60), dimensions(original_path)
    assert dimensions(rendered_path) == (60, 80), dimensions(rendered_path)
    assert original_path.read_bytes() != rendered_path.read_bytes()

    # Importing a live pin and pinning an edited background document must retain
    # an actual editable canvas, including undo, rather than baking its pixels.
    document = client.tool("snow_shot_document_open", {"source": "pinned", "source_id": identifier})
    assert document["result"]["document_id"]
    assert document["result"]["canvas_bounds"][2:] == [60, 80], document
    imported_path = directory / "rotated-pin-document.png"
    client.tool("snow_shot_document_save", {"document_id": document["result"]["document_id"],
                "expected_revision": document["revision"], "path": str(imported_path)})
    assert dimensions(imported_path) == (60, 80)
    client.tool("snow_shot_document_close", {"document_id": document["result"]["document_id"],
                "expected_revision": document["revision"]})
    document = client.tool("snow_shot_document_open", {"path": str(source)})
    document_id = document["result"]["document_id"]
    document = client.tool("snow_shot_document_apply_annotations", {
        "document_id": document_id, "expected_revision": document["revision"],
        "operations": [{"type": "rectangle", "bounds": [10, 10, 30, 20],
                        "style": {"stroke": [255, 0, 0, 255], "stroke_width": 3}}]})
    existing = {pin["id"] for pin in client.tool("snow_shot_pinned_list")["result"]["windows"]}
    client.tool("snow_shot_document_pin", {"document_id": document_id,
                "expected_revision": document["revision"]})
    imported_pin = next(pin["id"] for pin in client.tool("snow_shot_pinned_list")["result"]["windows"]
                        if pin["id"] not in existing)
    prior_identifier = identifier
    identifier = imported_pin
    assert pin_state()["result"]["can_undo"], pin_state()
    mutate("snow_shot_pinned_edit", action="undo")
    assert pin_state()["result"]["can_redo"], pin_state()
    mutate("snow_shot_pinned_edit", action="redo")
    assert pin_state()["result"]["can_undo"], pin_state()
    mutate("snow_shot_pinned_action", action="destroy")
    identifier = prior_identifier
    client.tool("snow_shot_document_close", {"document_id": document_id,
                "expected_revision": document["revision"]})
    for kind, value in (("text", "MCP fixture text"), ("html", "<b>MCP fixture HTML</b>")):
        imported = client.tool("snow_shot_pinned_create", {"source": {"kind": kind, kind: value}})
        imported_id = imported["result"]["id"]
        assert imported["result"]["ready"]
        current_import = client.tool("snow_shot_pinned_get", {"id": imported_id})
        client.tool("snow_shot_pinned_action", {"id": imported_id,
                    "expected_revision": current_import["revision"], "action": "destroy"})
    mutate("snow_shot_pinned_action", action="hide")
    assert not pin_state()["result"]["visible"]
    mutate("snow_shot_pinned_action", action="show")
    assert pin_state()["result"]["visible"]
    replacement = directory / "replacement.png"
    png(replacement, 100, 70)
    mutate("snow_shot_pinned_replace", source={"kind": "file", "path": str(replacement)})
    replaced_path = directory / "pin-replaced.png"
    mutate("snow_shot_pinned_export", output="save", original=True, path=str(replaced_path))
    assert dimensions(replaced_path) == (100, 70)
    mutate("snow_shot_pinned_action", action="destroy")
    def mutate_group(action, group_id):
        for _ in range(5):
            groups = client.tool("snow_shot_group_list")
            response = client.request("tools/call", {"name": "snow_shot_group_update", "arguments": {
                "expected_revision": groups["revision"], "action": action, "group_id": group_id}})
            if not response.get("isError"):
                return response["structuredContent"]
            assert response["structuredContent"]["error"]["code"] == "stale_revision", response
            time.sleep(.05)
        raise AssertionError("group never reached a stable revision")
    mutate_group("activate", original_group)
    mutate_group("delete", custom)
    return {"domains": ["production_router", "recording_state", "groups", "pin_file_text_html",
                        "pin_transforms", "original_rendered_export", "pin_document_source",
                        "editable_document_pin", "pin_replace", "revision_conflicts", "idempotency",
                        "settings_update_reset", "auxiliary_settings", "models_credentials",
                        "redacted_archive_roundtrip", "history", "artifacts", "storage_cleanup",
                        "templates", "permission_request", "updater_cancel", "settings_action",
                        "local_translation_and_tasks", "provider_source_admission",
                        "quit_acknowledgement"],
            "scope": "Isolated app storage and local HTTP provider; no external providers or recording devices"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bridge", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("application", type=lambda value: Path(value).resolve(strict=True))
    parser.add_argument("--platform", default="offscreen", choices=("offscreen", "windows", "cocoa"))
    parser.add_argument("--recording", action="store_true",
                        help="Also record a small native region with microphone and system audio disabled")
    parser.add_argument("--disable-mcp", action="store_true",
                        help="Verify disabling acknowledges before private IPC teardown")
    args = parser.parse_args()
    with LocalProvider() as provider, tempfile.TemporaryDirectory(prefix="snow-shot-mcp-app-") as temporary:
        directory = Path(temporary)
        descriptor = directory / "descriptor.json"
        environment = dict(os.environ, QT_QPA_PLATFORM=args.platform, SNOW_SHOT_API_BASE_URL=provider)
        native_libraries = Path(__file__).resolve().parents[2] / ".tools/vcpkg/installed/dynamic/x64-windows/bin"
        if native_libraries.is_dir():
            environment["PATH"] = str(native_libraries) + os.pathsep + environment.get("PATH", "")
        with (directory / "application.log").open("w", encoding="utf-8") as log:
            app = subprocess.Popen([str(args.application), "--mcp-fixture", str(directory)],
                                   env=environment, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 30
                while not descriptor.is_file() and app.poll() is None and time.monotonic() < deadline:
                    time.sleep(.05)
                assert descriptor.is_file(), (app.poll(), list(directory.iterdir()),
                                              (directory / "application.log").read_text(encoding="utf-8"))
                client = Client(args.bridge, descriptor)
                try:
                    report = exercise(client, directory)
                    exercise_modern_administration(args.bridge, descriptor)
                    if args.recording:
                        exercise_recording(client, directory)
                        report["domains"].append("native_recording_finalized_immutable_artifact")
                        report["scope"] = "Isolated storage, local HTTP provider, native region recording; no audio or external providers"
                    if args.disable_mcp:
                        settings = client.tool("snow_shot_settings_get")
                        mcp_field = next(field for field in settings["result"]["fields"]
                                         if field["id"].endswith("mcp-enabled"))
                        response = client.tool("snow_shot_settings_update", {
                            "expected_revision": settings["revision"],
                            "values": {mcp_field["id"]: False}})
                        assert mcp_field["id"] in response["result"]["applied_fields"], response
                        deadline = time.monotonic() + 10
                        while descriptor.exists() and time.monotonic() < deadline:
                            time.sleep(.025)
                        assert not descriptor.exists() and app.poll() is None
                        status = client.tool("snow_shot_status")
                        assert status["reachable"] is False, status
                        report["domains"].remove("quit_acknowledgement")
                        report["domains"].append("disable_acknowledgement_before_teardown")
                    else:
                        response = client.tool("snow_shot_app_action", {"action": "quit"})
                        assert response["result"]["accepted"]
                        assert app.wait(timeout=15) == 0, app.returncode
                    report["executed_tools"] = sorted(Client.executed_tools)
                    report["read_resources"] = sorted(Client.read_resources)
                finally:
                    client.close()
                print(json.dumps(report, indent=2))
            except Exception:
                print((directory / "application.log").read_text(encoding="utf-8"))
                raise
            finally:
                if app.poll() is None:
                    app.terminate()
                    app.wait(timeout=10)


if __name__ == "__main__":
    main()
