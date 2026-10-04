#!/usr/bin/env python3
"""Offline contracts for direct publication of audited local Gitee releases."""

import base64
import importlib.util
import io
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import subprocess
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
             patch.object(publisher, "verify_attachment", side_effect=fake_verify), \
             patch.object(publisher, "prune_old_releases", side_effect=lambda *args: events.append("cleanup")):
            publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag", "create", "package.zip", "verify:package.zip",
                                      "latest-version.json", "verify:latest-version.json", "cleanup"])
            events.clear()
            publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag", "verify:package.zip", "verify:latest-version.json", "cleanup"])
            files.pop("package.zip")
            events.clear()
            with self.assertRaisesRegex(ValueError, "missing local assets"):
                publisher.publish(self.release, assets, "", "", verify_only=True)
            self.assertEqual(events, ["verify:latest-version.json"])
            events.clear()
            publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag", "verify:latest-version.json", "package.zip", "verify:package.zip", "cleanup"])
            files["latest-version.json"]["bytes"] = b"conflicting remote bytes"
            files.pop("package.zip")
            events.clear()
            with self.assertRaisesRegex(ValueError, "conflicting"):
                publisher.publish(self.release, assets, "test-token", "test-user")
            self.assertEqual(events, ["tag"])

    def test_semver_retention_preserves_current_newer_unrelated_and_draft_releases(self):
        versions = ["alpha", "alpha.1", "alpha.beta", "beta", "beta.2", "beta.11", "rc.1"]
        ordered = [f"v1.2.3-{preview}_snow-shot" for preview in versions] + [TAG]
        self.assertEqual(sorted(ordered, key=publisher.release_version_key), ordered)
        self.assertLess(publisher.release_version_key("v1.9.0_snow-shot"),
                        publisher.release_version_key("v1.10.0_snow-shot"))
        remote = [{"id": 1, "tag_name": "v1.2.2_snow-shot"},
                  {"id": 2, "tag_name": "v1.2.3-beta.11_snow-shot"},
                  {"id": 3, "tag_name": TAG},
                  {"id": 4, "tag_name": "v1.10.0_snow-shot"},
                  {"id": 5, "tag_name": "v1.0.0_other"},
                  {"id": 6, "tag_name": "v1.2.1_snow-shot", "draft": True},
                  {"id": 7, "tag_name": "v1.2.3-01_snow-shot"},
                  {"id": 8, "tag_name": None}]
        with patch.object(publisher, "release_listing", return_value=remote), \
             patch.object(publisher, "delete_release") as delete, \
             patch("sys.stdout", io.StringIO()):
            self.assertEqual(publisher.prune_old_releases(TAG, "secret-token"), 2)
        self.assertEqual([call.args for call in delete.call_args_list],
                         [(1, "secret-token"), (2, "secret-token")])

    def test_invalid_cleanup_plan_is_rejected_before_any_deletion(self):
        old = {"id": 1, "tag_name": "v1.2.2_snow-shot"}
        for remote in ([old, old], [old, {"id": False, "tag_name": "v1.2.1_snow-shot"}]):
            with self.subTest(remote=remote), \
                 patch.object(publisher, "release_listing", return_value=remote), \
                 patch.object(publisher, "delete_release") as delete:
                with self.assertRaises(ValueError):
                    publisher.prune_old_releases(TAG, "secret-token")
                delete.assert_not_called()

    def test_delete_authentication_and_ambiguous_response_reconciliation(self):
        error = subprocess.CalledProcessError(56, ["curl"])
        with patch.object(publisher.subprocess, "check_output", side_effect=error) as request, \
             patch.object(publisher, "api_json", return_value=None) as read:
            publisher.delete_release(7, "secret-token")
        request.assert_called_once()
        self.assertNotIn("secret-token", str(request.call_args.args))
        self.assertIn("access_token=secret-token", request.call_args.kwargs["input"])
        self.assertEqual(read.call_args.args, (publisher.GITEE_API + "/7", "secret-token"))
        with patch.object(publisher.subprocess, "check_output", side_effect=error) as request, \
             patch.object(publisher, "api_json", return_value={"id": 7}):
            with self.assertRaises(subprocess.CalledProcessError):
                publisher.delete_release(7, "secret-token")
        request.assert_called_once()

    def test_transport_recognizes_only_explicit_attachment_quota_rejections(self):
        error = subprocess.CalledProcessError(22, ["curl"],
                                             output=json.dumps({"message": "仓库附件配额：1 GB"}))
        with patch.object(publisher.subprocess, "check_output", side_effect=error):
            with self.assertRaises(publisher.GiteeQuotaError):
                publisher.post_form(publisher.GITEE_API, {}, "secret-token")
        error.output = '{"message":"permission denied"}'
        with patch.object(publisher.subprocess, "check_output", side_effect=error):
            with self.assertRaises(subprocess.CalledProcessError):
                publisher.post_form(publisher.GITEE_API, {}, "secret-token")

    def test_quota_retry_reads_remote_state_prunes_then_retries_only_missing_upload(self):
        for accepted in (False, True):
            with self.subTest(accepted=accepted):
                assets = publisher.local_assets(self.release)
                remote = {"id": 7, "tag_name": TAG, "name": self.release["title"],
                          "body": self.release["body"], "prerelease": False}
                files = {}
                events = []
                attempts = []
                def upload(url, fields, token, file=None):
                    attempts.append(file.name)
                    events.append("upload:" + file.name)
                    if attempts == ["package.zip"]:
                        if accepted:
                            files[file.name] = {"name": file.name}
                        raise publisher.GiteeQuotaError("quota")
                    files[file.name] = {"name": file.name}
                def cleanup(*args):
                    events.append("cleanup")
                    return 1
                with patch.object(publisher, "sync_tag"), \
                     patch.object(publisher, "existing_release", return_value=remote), \
                     patch.object(publisher, "attachments", side_effect=lambda *args: list(files.values())), \
                     patch.object(publisher, "post_form", side_effect=upload), \
                     patch.object(publisher, "verify_attachment", side_effect=lambda file, tag, name, *args: events.append("verify:" + name)), \
                     patch.object(publisher, "prune_old_releases", side_effect=cleanup):
                    publisher.publish(self.release, assets, "test-token", "test-user")
                if accepted:
                    self.assertEqual(attempts, ["package.zip", "latest-version.json"])
                    self.assertEqual(events.count("cleanup"), 1)
                else:
                    self.assertEqual(attempts, ["package.zip", "package.zip", "latest-version.json"])
                    self.assertEqual(events[:3], ["upload:package.zip", "cleanup", "upload:package.zip"])
                self.assertEqual(events[-2:], ["verify:latest-version.json", "cleanup"])

    def test_failed_uploads_do_not_trigger_unrelated_or_repeated_cleanup(self):
        remote = {"id": 7, "tag_name": TAG, "name": self.release["title"],
                  "body": self.release["body"], "prerelease": False}
        for error, quota in ((subprocess.CalledProcessError(22, ["curl"]), False),
                             (publisher.GiteeQuotaError("quota"), True)):
            with self.subTest(quota=quota), patch.object(publisher, "sync_tag"), \
                 patch.object(publisher, "existing_release", return_value=remote), \
                 patch.object(publisher, "attachments", return_value=[]), \
                 patch.object(publisher, "post_form", side_effect=error) as upload, \
                 patch.object(publisher, "prune_old_releases", return_value=0) as cleanup:
                with self.assertRaises(type(error)):
                    publisher.publish(self.release, publisher.local_assets(self.release), "token", "user")
                upload.assert_called_once()
                self.assertEqual(cleanup.call_count, int(quota))

    def paired_windows_release(self, architectures=('x64',)):
        self.release.pop('nativeValidation', None)
        self.release['assets'] = []
        for architecture in architectures:
          platform = 'windows-' + architecture
          for product, feed in (('snow-shot', 'latest-version.json'),
                              ('snow-shot-mini', 'latest-version-mini.json')):
            if architecture == 'arm64':
                feed = feed[:-5] + '-windows-arm64.json'
            packages = []
            kinds = [('online', 'installer'), ('online', 'update'), ('portable', 'portable')]
            if product == 'snow-shot':
                kinds += [('offline', 'installer'), ('offline', 'update')]
            for variant, kind in kinds:
                suffix = '.exe' if kind == 'installer' else '-update.zip' if kind == 'update' else '.zip'
                path = self.add_asset(f'{product}-1.2.3-{platform}-{variant}{suffix}', b'fixture')
                packages.append({'variant': variant, 'kind': kind,
                                 'path': f'setup/{product}_{platform}-{variant}{suffix}',
                                 'size': path.stat().st_size, 'sha256': publisher.sha256(path)})
            payload = {'platform': platform, 'version': '1.2.3', 'product': product, 'packages': packages}
            self.add_asset(feed, json.dumps({'payload': base64.b64encode(
                json.dumps(payload).encode()).decode()}).encode())
        auditors = [self.directory / 'full-auditor.exe', self.directory / 'mini-auditor.exe']
        for auditor in auditors:
            auditor.write_bytes(b'fixture')
        return publisher.local_assets(self.release), auditors

    def test_dual_architecture_release_requires_native_proof_for_exact_bytes(self):
        assets, auditors = self.paired_windows_release(('x64', 'arm64'))
        invoke = lambda *args: 'a' * 40 if args[0] == 'git' else ''
        with patch.object(publisher, 'run', side_effect=invoke):
            with self.assertRaisesRegex(ValueError, 'native validation evidence'):
                publisher.verify_local_release(self.release, assets, *auditors)
            proofs = []
            for platform in ('windows-x64', 'windows-arm64'):
                artifacts = [{'Name': name, 'Bytes': path.stat().st_size, 'Sha256': publisher.sha256(path)}
                             for name, path in assets.items() if platform in name and name.endswith(('.exe', '.zip'))]
                proofs.append({'Platform': platform, 'HostPlatform': platform, 'Passed': True, 'Artifacts': artifacts})
            self.release['nativeValidation'] = proofs
            publisher.verify_local_release(self.release, assets, *auditors, require_complete=True)
            proofs[1]['HostPlatform'] = 'windows-x64'
            with self.assertRaisesRegex(ValueError, 'does not match published bytes'):
                publisher.verify_local_release(self.release, assets, *auditors)
            proofs[1]['HostPlatform'] = 'windows-arm64'
            proofs[1]['Artifacts'][0]['Sha256'] = '0' * 64
            with self.assertRaisesRegex(ValueError, 'does not match published bytes'):
                publisher.verify_local_release(self.release, assets, *auditors)

    def test_historical_x64_verification_cannot_authorize_new_publication(self):
        assets, auditors = self.paired_windows_release()
        invoke = lambda *args: 'a' * 40 if args[0] == 'git' else ''
        with patch.object(publisher, 'run', side_effect=invoke):
            publisher.verify_local_release(self.release, assets, *auditors)
            with self.assertRaisesRegex(ValueError, 'both editions and both x64/ARM64'):
                publisher.verify_local_release(self.release, assets, *auditors, require_complete=True)

    def test_partial_arm_release_and_feed_platform_mismatch_are_rejected(self):
        assets, auditors = self.paired_windows_release(('x64', 'arm64'))
        assets.pop('latest-version-mini-windows-arm64.json')
        with patch.object(publisher, 'run', return_value=''):
            with self.assertRaisesRegex(ValueError, 'requires signed metadata'):
                publisher.verify_local_release(self.release, assets, *auditors)
        assets, auditors = self.paired_windows_release(('x64', 'arm64'))
        feed = assets['latest-version-windows-arm64.json']
        envelope = json.loads(feed.read_text())
        payload = json.loads(base64.b64decode(envelope['payload']))
        payload['platform'] = 'windows-x64'
        envelope['payload'] = base64.b64encode(json.dumps(payload).encode()).decode()
        feed.write_text(json.dumps(envelope))
        with patch.object(publisher, 'run', return_value=''):
            with self.assertRaisesRegex(ValueError, 'platform differs'):
                publisher.verify_local_release(self.release, assets, *auditors)

    def test_paired_release_uses_each_compiled_product_auditor(self):
        assets, auditors = self.paired_windows_release()
        with patch.object(publisher, 'run', side_effect=['', '', 'a' * 40]) as invoke:
            publisher.verify_local_release(self.release, assets, *auditors)
        self.assertEqual(invoke.call_args_list[0].args,
                         (str(auditors[0]), '--verify-release', '--platform', 'windows-x64', '--manifest',
                          str(assets['latest-version.json'])))
        self.assertEqual(invoke.call_args_list[1].args,
                         (str(auditors[1]), '--verify-release', '--platform', 'windows-x64', '--manifest',
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
