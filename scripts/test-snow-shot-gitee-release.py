#!/usr/bin/env python3
"""Offline contracts for direct publication of audited local Gitee releases."""

import base64
import importlib.util
import io
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError


SCRIPT = Path(__file__).with_name("publish-snow-shot-gitee-release.py")
SPEC = importlib.util.spec_from_file_location("snow_gitee_publisher", SCRIPT)
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)
TAG = "v1.2.3_snow-shot"


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.release = {"schema": 1, "tag": TAG, "sourceCommit": "a" * 40,
                        "title": "Snow Shot 1.2.3", "body": "Detailed bilingual notes.\n",
                        "assets": []}
        self.add_asset("package.zip", b"binary package")
        self.add_asset("latest-version.json", b"signed manifest")

    def add_asset(self, name, data):
        path = self.directory / name
        path.write_bytes(data)
        self.release["assets"].append({"name": name, "path": str(path),
                                       "size": len(data), "sha256": publisher.sha256(path)})
        return path

    def test_authentication_stays_out_of_process_arguments(self):
        calls = []
        def invoke(command, **kwargs):
            calls.append((command, kwargs))
            return '{}'
        with patch.object(publisher.subprocess, 'check_output', side_effect=invoke):
            publisher.git_with_gitee_auth('secret-token', 'account', 'ls-remote', publisher.GITEE_GIT)
            publisher.post_form(publisher.GITEE_API, {'tag_name': TAG}, 'secret-token')
        self.assertNotIn('secret-token', str(calls[0][0]))
        self.assertNotIn('secret-token', str(calls[1][0]))
        self.assertIn('Authorization: Basic', calls[0][1]['env']['GIT_CONFIG_VALUE_0'])
        self.assertIn('secret-token', calls[1][1]['input'])

    def test_tag_and_attachment_urls(self):
        self.assertEqual(publisher.checked_tag(TAG), TAG)
        for bad in ("main", "v01.2.3_snow-shot", "v1.2.3-01_snow-shot",
                    "v1.2.3_other", "v1.2.3_snow-shot/evil"):
            with self.assertRaises(ValueError):
                publisher.checked_tag(bad)
        name = "latest-version.json"
        good = f"https://gitee.com/mg-chao/snow-apps/releases/download/{TAG}/{name}"
        self.assertEqual(publisher.asset_url({"name": name, "browser_download_url": good}, TAG, name), good)
        for bad in (good.replace("gitee.com", "example.com"), good + "?token=secret",
                    good.replace(TAG, "v9.9.9_snow-shot")):
            with self.assertRaises(ValueError):
                publisher.asset_url({"name": name, "browser_download_url": bad}, TAG, name)

    def test_local_input_rejects_changed_or_duplicate_bytes(self):
        self.assertEqual(set(publisher.local_assets(self.release)), {"package.zip", "latest-version.json"})
        (self.directory / "package.zip").write_bytes(b"changed package")
        with self.assertRaisesRegex(ValueError, "differs from its audit"):
            publisher.local_assets(self.release)
        (self.directory / "package.zip").write_bytes(b"binary package")
        self.release["assets"].append(dict(self.release["assets"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            publisher.local_assets(self.release)

    def test_signed_metadata_and_tag_gate(self):
        packages = []
        self.release["assets"] = []
        for variant, kind in (("online", "installer"), ("online", "update"),
                              ("offline", "installer"), ("offline", "update"),
                              ("portable", "portable")):
            suffix = ".exe" if kind == "installer" else "-update.zip" if kind == "update" else ".zip"
            path = self.add_asset(f"snow-shot-1.2.3-windows-x64-{variant}{suffix}", b"package")
            packages.append({"variant": variant, "kind": kind,
                             "path": f"setup/snow-shot_windows-x64-{variant}{suffix}",
                             "size": path.stat().st_size, "sha256": publisher.sha256(path)})
        envelope = {"payload": base64.b64encode(json.dumps({"version": "1.2.3", "packages": packages}).encode()).decode()}
        self.add_asset("latest-version.json", json.dumps(envelope).encode())
        assets = publisher.local_assets(self.release)
        auditor = self.directory / "auditor.exe"
        auditor.write_bytes(b"fixture")
        calls = []
        def fake_run(*args):
            calls.append(args)
            return "a" * 40 if args[0] == "git" else ""
        with patch.object(publisher, "run", side_effect=fake_run):
            publisher.verify_local_release(self.release, assets, auditor)
        self.assertEqual(calls[0][1], "--verify-release")
        self.assertEqual(calls[1], ("git", "rev-list", "-n", "1", TAG))
        with patch.object(publisher, "run", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "source commit"):
                publisher.verify_local_release(self.release, assets, auditor)
        packages[0]["sha256"] = "b" * 64
        envelope["payload"] = base64.b64encode(json.dumps({"version": "1.2.3", "packages": packages}).encode()).decode()
        assets["latest-version.json"].write_text(json.dumps(envelope))
        with patch.object(publisher, "run") as run:
            with self.assertRaisesRegex(ValueError, "signed metadata"):
                publisher.verify_local_release(self.release, assets, auditor)
            run.assert_not_called()

    def test_direct_upload_order_retry_conflict_and_readonly_verification(self):
        assets = publisher.local_assets(self.release)
        remote = {"id": 7, "tag_name": TAG, "name": self.release["title"],
                  "body": self.release["body"], "prerelease": False}
        files = {}
        events = []
        def fake_post(url, fields, token, file=None):
            if file is None:
                events.append("create")
                self.assertEqual(fields["body"], self.release["body"])
                return remote
            self.assertEqual(file, assets[file.name])
            events.append(file.name)
            files[file.name] = {"name": file.name, "bytes": file.read_bytes()}
            return files[file.name]
        def fake_verify(file, tag, name, expected, directory):
            if file["bytes"] != expected.read_bytes():
                raise ValueError("conflicting remote bytes")
            events.append("verify:" + name)
        with patch.object(publisher, "run", side_effect=AssertionError("No GitHub invocation")), \
             patch.object(publisher, "sync_tag", side_effect=lambda *args: events.append("tag")), \
             patch.object(publisher, "existing_release", side_effect=lambda *args: remote if files else None), \
             patch.object(publisher, "post_form", side_effect=fake_post), \
             patch.object(publisher, "attachments", side_effect=lambda _, token="": list(files.values())), \
             patch.object(publisher, "verify_attachment", side_effect=fake_verify):
            publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag", "create", "package.zip", "verify:package.zip",
                                      "latest-version.json", "verify:latest-version.json"])
            events.clear()
            publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag", "verify:package.zip", "verify:latest-version.json"])
            files.pop("package.zip")
            events.clear()
            with self.assertRaisesRegex(ValueError, "missing local assets"):
                publisher.publish(self.release, assets, "", "", verify_only=True)
            self.assertEqual(events, ["verify:latest-version.json"])
            events.clear()
            publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag", "verify:latest-version.json", "package.zip", "verify:package.zip"])
            files["latest-version.json"]["bytes"] = b"conflicting remote bytes"
            files.pop("package.zip")
            events.clear()
            with self.assertRaisesRegex(ValueError, "conflicting"):
                publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag"])

    def paired_windows_release(self):
        self.release['assets'] = []
        for product, feed in (('snow-shot', 'latest-version.json'),
                              ('snow-shot-mini', 'latest-version-mini.json')):
            packages = []
            kinds = [('online', 'installer'), ('online', 'update'), ('portable', 'portable')]
            if product == 'snow-shot':
                kinds += [('offline', 'installer'), ('offline', 'update')]
            for variant, kind in kinds:
                suffix = '.exe' if kind == 'installer' else '-update.zip' if kind == 'update' else '.zip'
                path = self.add_asset(f'{product}-1.2.3-windows-x64-{variant}{suffix}', b'fixture')
                packages.append({'variant': variant, 'kind': kind,
                                 'path': f'setup/{product}_windows-x64-{variant}{suffix}',
                                 'size': path.stat().st_size, 'sha256': publisher.sha256(path)})
            payload = {'version': '1.2.3', 'product': product, 'packages': packages}
            self.add_asset(feed, json.dumps({'payload': base64.b64encode(
                json.dumps(payload).encode()).decode()}).encode())
        auditors = [self.directory / 'full-auditor.exe', self.directory / 'mini-auditor.exe']
        for auditor in auditors:
            auditor.write_bytes(b'fixture')
        return publisher.local_assets(self.release), auditors

    def test_paired_release_uses_each_compiled_product_auditor(self):
        assets, auditors = self.paired_windows_release()
        with patch.object(publisher, 'run', side_effect=['', '', 'a' * 40]) as invoke:
            publisher.verify_local_release(self.release, assets, *auditors)
        self.assertEqual(invoke.call_args_list[0].args,
                         (str(auditors[0]), '--verify-release', '--manifest',
                          str(assets['latest-version.json'])))
        self.assertEqual(invoke.call_args_list[1].args,
                         (str(auditors[1]), '--verify-release', '--manifest',
                          str(assets['latest-version-mini.json'])))
        self.assertEqual(invoke.call_args_list[2].args, ('git', 'rev-list', '-n', '1', TAG))

    def test_paired_release_rejects_missing_mini_feed_or_auditor(self):
        assets, auditors = self.paired_windows_release()
        with patch.object(publisher, 'run') as invoke:
            with self.assertRaisesRegex(ValueError, 'signed Mini metadata'):
                publisher.verify_local_release(self.release, assets, auditors[0])
            assets.pop('latest-version-mini.json')
            with self.assertRaisesRegex(ValueError, 'signed Mini metadata'):
                publisher.verify_local_release(self.release, assets, *auditors)
            invoke.assert_not_called()

    def test_mini_feed_rejects_wrong_product_or_offline_package(self):
        for failure in ('product', 'offline'):
            with self.subTest(failure=failure):
                assets, auditors = self.paired_windows_release()
                envelope = json.loads(assets['latest-version-mini.json'].read_text())
                payload = json.loads(base64.b64decode(envelope['payload']))
                if failure == 'product':
                    payload['product'] = 'snow-shot'
                else:
                    payload['packages'][0]['variant'] = 'offline'
                envelope['payload'] = base64.b64encode(json.dumps(payload).encode()).decode()
                assets['latest-version-mini.json'].write_text(json.dumps(envelope))
                with patch.object(publisher, 'run', return_value=''):
                    with self.assertRaisesRegex(ValueError, 'incorrect product|three Windows'):
                        publisher.verify_local_release(self.release, assets, *auditors)

    def test_differing_release_notes_reject_uploads(self):
        remote = {"id": 7, "tag_name": TAG, "name": self.release["title"], "body": "Other notes"}
        with patch.object(publisher, "existing_release", return_value=remote), \
             patch.object(publisher, "post_form") as post:
            with self.assertRaisesRegex(ValueError, "notes differ"):
                publisher.publish(self.release, publisher.local_assets(self.release), "", "", verify_only=True)
            post.assert_not_called()

    def test_homebrew_preparation_uses_local_dmg_and_tagged_source(self):
        dmg = self.add_asset("snow-shot-1.2.3-macos-arm64.dmg", b"local audited dmg")
        self.add_asset(dmg.name + ".sha256", (publisher.sha256(dmg) + "  " + dmg.name + "\n").encode())
        sources = {"CMakeLists.txt": b'set(SNOW_SHOT_VERSION "1.2.3")\n',
                   "scripts/install-snow-shot-macos.sh": b"--prepare-app)\n",
                   "scripts/prepare-snow-shot-homebrew.sh": b"#!/bin/sh\n"}
        def fake_git(command):
            self.assertEqual(command[:2], ("git", "show"))
            tag, name = command[2].split(":", 1)
            self.assertEqual(tag, TAG)
            return sources[name]
        with patch.object(publisher.subprocess, "check_output", side_effect=fake_git):
            publisher.prepare_homebrew(self.release, publisher.local_assets(self.release), self.directory)
            first = self.release["assets"][-1].copy()
            publisher.prepare_homebrew(self.release, publisher.local_assets(self.release), self.directory)
        self.assertEqual(self.release["assets"][-1], first)
        self.assertEqual(first["name"], "snow-shot-1.2.3-macos-arm64-homebrew.tar.gz")

    def test_api_reads_authenticate_and_retry_only_transient_read_failures(self):
        url = publisher.GITEE_API + "?per_page=100"
        response = io.BytesIO(b'[]')
        output = io.StringIO()
        with patch.object(publisher, "urlopen", side_effect=[
                HTTPError(url, 502, "Bad Gateway", {}, None), response]) as request, \
             patch.object(publisher.time, "sleep") as sleep, \
             patch("sys.stdout", output):
            self.assertEqual(publisher.api_json(url, "secret-token"), [])
        self.assertEqual(request.call_count, 2)
        self.assertEqual(request.call_args.args[0], url + "&access_token=secret-token")
        sleep.assert_called_once_with(2)
        self.assertNotIn("secret-token", output.getvalue())
        with patch.object(publisher, "urlopen", side_effect=HTTPError(url, 403, "Forbidden", {}, None)) as request, \
             patch.object(publisher.time, "sleep") as sleep:
            with self.assertRaises(HTTPError):
                publisher.api_json(url, "secret-token")
            request.assert_called_once()
            sleep.assert_not_called()
        with patch.object(publisher, "urlopen", side_effect=HTTPError(url, 502, "Bad Gateway", {}, None)) as request, \
             patch.object(publisher.time, "sleep"), patch("sys.stdout", io.StringIO()):
            with self.assertRaises(HTTPError):
                publisher.api_json(url)
            self.assertEqual(request.call_count, 4)

    def test_release_listings_use_the_publication_token(self):
        with patch.object(publisher, "api_json", return_value=[]) as api:
            publisher.attachments(7, "secret-token")
            self.assertEqual(api.call_args.args[1], "secret-token")
            publisher.existing_release(TAG, "secret-token")
            self.assertEqual(api.call_args.args[1], "secret-token")

    def test_utf8_release_response_through_real_curl_transport(self):
        notes = "Snow Shot release notes: 中文，繁體。"
        body = json.dumps({"body": notes}, ensure_ascii=False).encode("utf-8")
        received = []
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                received.append(self.rfile.read(int(self.headers["Content-Length"])))
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            response = publisher.post_form(f"http://127.0.0.1:{server.server_port}/releases",
                                           {"body": notes}, "fixture-token")
            self.assertEqual(response["body"], notes)
            self.assertIn(notes.encode("utf-8"), received[0])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
