#!/usr/bin/env python3
"""Focused checks for native macOS dialog language selection; no GUI is opened."""
import importlib.util
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import unittest


spec = importlib.util.spec_from_file_location(
    "package_macos", Path(__file__).with_name("package-macos.py"))
package_macos = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_macos)
PLIST = package_macos.ROOT / "snow_shot/resources/macos-info.plist.in"


class BundleLocalizationTests(unittest.TestCase):
    def test_source_manifest_passes_packaging_validation(self):
        package_macos.verify_localizations(plistlib.loads(PLIST.read_bytes()))

    def test_packaging_rejects_missing_chinese_support(self):
        for languages in (None, [], ["en"], ["en", "zh-Hans"], ["en", "zh-Hant"]):
            with self.subTest(languages=languages):
                info = {"CFBundleDevelopmentRegion": "en"}
                if languages is not None:
                    info["CFBundleLocalizations"] = languages
                with self.assertRaisesRegex(RuntimeError, "Bundle must declare"):
                    package_macos.verify_localizations(info)

    def test_packaging_rejects_incorrect_development_language(self):
        info = plistlib.loads(PLIST.read_bytes())
        info["CFBundleDevelopmentRegion"] = "zh-Hans"
        with self.assertRaisesRegex(RuntimeError, "development language"):
            package_macos.verify_localizations(info)

    @unittest.skipUnless(sys.platform == "darwin" and shutil.which("swift"),
                         "Apple Foundation and the Swift toolchain are required")
    def test_foundation_selects_chinese_from_the_manifest(self):
        # Query Apple's real language matcher, without launching the app or
        # depending on the test machine's configured language preferences.
        source = r'''
import Foundation
let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
let info = try PropertyListSerialization.propertyList(from: data, format: nil) as! [String: Any]
let languages = info["CFBundleLocalizations"] as? [String] ?? []
var resolved: [String: String] = [:]
for preference in ["zh-Hans-CN", "zh-Hant-TW", "en-US", "fr-FR"] {
    resolved[preference] = Bundle.preferredLocalizations(
        from: languages, forPreferences: [preference]).first ?? ""
}
let result = try JSONSerialization.data(withJSONObject: resolved)
print(String(data: result, encoding: .utf8)!)
'''
        result = subprocess.run(["swift", "-e", source, str(PLIST)],
                                text=True, capture_output=True, check=True, timeout=60)
        self.assertEqual(json.loads(result.stdout), {
            "zh-Hans-CN": "zh-Hans", "zh-Hant-TW": "zh-Hant",
            "en-US": "en", "fr-FR": "en",
        })


if __name__ == "__main__":
    unittest.main()
