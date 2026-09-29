#!/usr/bin/env python3
"""Offline contracts for the published GitHub to Gitee release mirror."""

import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).with_name("mirror-snow-shot-gitee-release.py")
SPEC = importlib.util.spec_from_file_location("snow_gitee_mirror", SCRIPT)
mirror = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mirror)
TAG = "v1.2.3_snow-shot"


class MirrorTests(unittest.TestCase):
    def test_authentication_stays_out_of_process_arguments(self):
        calls = []
        def invoke(command, **kwargs):
            calls.append((command, kwargs))
            return '{}'
        with patch.object(mirror.subprocess, 'check_output', side_effect=invoke):
            mirror.git_with_gitee_auth('secret-token', 'account', 'ls-remote', mirror.GITEE_GIT)
            mirror.post_form(mirror.GITEE_API, {'tag_name': TAG}, 'secret-token')
        self.assertNotIn('secret-token', str(calls[0][0]))
        self.assertNotIn('secret-token', str(calls[1][0]))
        self.assertIn('Authorization: Basic', calls[0][1]['env']['GIT_CONFIG_VALUE_0'])
        self.assertIn('secret-token', calls[1][1]['input'])

    def test_tag_and_attachment_urls(self):
        self.assertEqual(mirror.checked_tag(TAG), TAG)
        for bad in ("main", "v01.2.3_snow-shot", "v1.2.3-01_snow-shot",
                    "v1.2.3_other", "v1.2.3_snow-shot/evil"):
            with self.assertRaises(ValueError):
                mirror.checked_tag(bad)
        name = "latest-version.json"
        good = f"https://gitee.com/mg-chao/snow-apps/releases/download/{TAG}/{name}"
        self.assertEqual(mirror.asset_url({"name": name, "browser_download_url": good}, TAG, name), good)
        for bad in (good.replace("gitee.com", "example.com"), good + "?token=secret",
                    good.replace(TAG, "v9.9.9_snow-shot")):
            with self.assertRaises(ValueError):
                mirror.asset_url({"name": name, "browser_download_url": bad}, TAG, name)

    def test_signed_metadata_uploaded_last_and_idempotent(self):
        assets = {"package.zip": b"binary package", "latest-version.json": b"signed manifest"}
        release = {"tag_name": TAG, "draft": False, "assets": [{"name": name} for name in assets]}
        remote = {"id": 7, "tag_name": TAG, "target_commitish": "a" * 40}
        files = {}
        events = []

        def fake_run(*args):
            if args[:2] == ("gh", "api"):
                return json.dumps(release)
            if args[:3] == ("git", "rev-list", "-n"):
                return "a" * 40
            if args[:2] == ("git", "rev-parse"):
                return "a" * 40
            if args[:3] == ("gh", "release", "download"):
                directory = Path(args[-1])
                for name, data in assets.items():
                    (directory / name).write_bytes(data)
                return ""
            raise AssertionError(args)

        def fake_post(url, fields, token, file=None):
            if file is None:
                events.append("create")
                return remote
            events.append(file.name)
            files[file.name] = {
                "name": file.name,
                "browser_download_url": f"https://gitee.com/mg-chao/snow-apps/releases/download/{TAG}/{file.name}",
                "bytes": file.read_bytes(),
            }
            return files[file.name]

        def fake_verify(file, tag, name, expected, directory):
            self.assertEqual(tag, TAG)
            self.assertEqual(file["bytes"], expected.read_bytes())
            mirror.asset_url(file, tag, name)
            events.append("verify:" + name)

        def fake_attachments(_):
            return list(files.values())

        with patch.object(mirror, "run", side_effect=fake_run), \
             patch.object(mirror, "sync_tag", side_effect=lambda *args: events.append("tag")), \
             patch.object(mirror, "api_json", side_effect=lambda *args: [] if not files else [remote]), \
             patch.object(mirror, "post_form", side_effect=fake_post), \
             patch.object(mirror, "attachments", side_effect=fake_attachments), \
             patch.object(mirror, "verify_attachment", side_effect=fake_verify):
            mirror.mirror(TAG, "test-token", "test-user")
            self.assertEqual(events, ["tag", "create", "package.zip", "verify:package.zip",
                                      "latest-version.json", "verify:latest-version.json"])
            events.clear()
            mirror.mirror(TAG, "test-token", "test-user")
            self.assertEqual(events, ["tag", "verify:package.zip", "verify:latest-version.json"])
            files.pop("package.zip")
            events.clear()
            mirror.mirror(TAG, "test-token", "test-user")
            self.assertEqual(events, ["tag", "package.zip", "verify:package.zip",
                                      "verify:latest-version.json"])
            files["package.zip"]["bytes"] = b"conflicting remote bytes"
            events.clear()
            with self.assertRaises(AssertionError):
                mirror.mirror(TAG, "test-token", "test-user")
            self.assertNotIn("package.zip", events)


if __name__ == "__main__":
    unittest.main()
