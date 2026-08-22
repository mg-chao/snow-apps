from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GENERATOR = Path(__file__).resolve().parents[1] / "generate_icon_pack.py"
SPEC = importlib.util.spec_from_file_location("generate_icon_pack", GENERATOR)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)


class GeneratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write_svg(self, source: str) -> None:
        (self.root / "icon.svg").write_text(source, encoding="utf-8")

    def manifest(self, **entry_overrides: object) -> Path:
        entry = {
            "variant": "outlined",
            "symbol": "Sample",
            "name": "sample",
            "source": "icon.svg",
            "colorModel": "monochrome",
            "fit": "contain",
        }
        entry.update(entry_overrides)
        data = {
            "schemaVersion": 1,
            "pack": "test-pack",
            "cppNamespace": "test::icons",
            "entries": [entry],
        }
        path = self.root / "icons.manifest.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def load_error(self, manifest: Path) -> str:
        with self.assertRaises(generator.ManifestError) as caught:
            generator._load_manifest(manifest)
        return str(caught.exception)

    def test_output_is_deterministic_and_check_detects_drift(self) -> None:
        self.write_svg('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><path fill="currentColor" d="M1 1h14v14H1z"/></svg>')
        manifest = self.manifest()
        header = self.root / "icons.h"
        source = self.root / "icons.cpp"
        command = [sys.executable, str(GENERATOR), str(manifest), "--header", str(header), "--source", str(source)]
        subprocess.run(command, check=True)
        first = (header.read_bytes(), source.read_bytes())
        generated_source = source.read_text(encoding="utf-8")
        self.assertIn("constexpr IconDescriptor kEntries[]", generated_source)
        self.assertIn("std::string_view", generated_source)
        self.assertIn("return pack().icon(0, colors);", generated_source)
        self.assertNotIn("QByteArray", generated_source)
        self.assertNotIn("QList", generated_source)
        self.assertNotIn("QString", generated_source)
        subprocess.run(command, check=True)
        self.assertEqual(first, (header.read_bytes(), source.read_bytes()))
        subprocess.run([*command, "--check"], check=True)
        source.write_text(source.read_text(encoding="utf-8") + "// stale\n", encoding="utf-8")
        self.assertNotEqual(subprocess.run([*command, "--check"], check=False).returncode, 0)

    def test_schema_and_identity_errors_are_rejected(self) -> None:
        self.write_svg('<svg viewBox="0 0 16 16"><path d="M0 0h1v1z"/></svg>')
        manifest = self.manifest()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        for field, value in (("schemaVersion", 2), ("pack", "Bad Pack"), ("cppNamespace", "bad-name")):
            with self.subTest(field=field):
                changed = dict(data)
                changed[field] = value
                manifest.write_text(json.dumps(changed), encoding="utf-8")
                self.assertTrue(self.load_error(manifest))

    def test_duplicate_keys_and_symbols_are_rejected(self) -> None:
        self.write_svg('<svg viewBox="0 0 16 16"><path d="M0 0h1v1z"/></svg>')
        manifest = self.manifest()
        data = json.loads(manifest.read_text(encoding="utf-8"))
        duplicate = dict(data["entries"][0])
        data["entries"].append(duplicate)
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertIn("duplicate icon key", self.load_error(manifest))
        duplicate["name"] = "other"
        manifest.write_text(json.dumps(data), encoding="utf-8")
        self.assertIn("duplicate exported symbol", self.load_error(manifest))

    def test_invalid_xml_viewbox_and_slot_model_are_rejected(self) -> None:
        manifest = self.manifest(colorModel="twoTone")
        for source, message in (
            ("<svg>", "invalid SVG XML"),
            ('<svg xmlns="http://www.w3.org/2000/svg"/>', "has no viewBox"),
            ('<svg viewBox="0 0 0 16"/>', "positive four-value viewBox"),
            ('<svg viewBox="0 0 16 16"><path fill="currentColor"/></svg>', "requires slots"),
        ):
            with self.subTest(message=message):
                self.write_svg(source)
                self.assertIn(message, self.load_error(manifest))

    def test_external_and_non_data_image_references_are_rejected(self) -> None:
        manifest = self.manifest(colorModel="fullColor", allowEmbeddedDataImages=True)
        for reference, message in (
            ("https://example.com/icon.png", "external network reference"),
            ("local.png", "non-data image reference"),
        ):
            with self.subTest(reference=reference):
                self.write_svg(f'<svg viewBox="0 0 16 16"><image href="{reference}"/></svg>')
                self.assertIn(message, self.load_error(manifest))

    def test_embedded_data_images_require_explicit_full_color_permission(self) -> None:
        self.write_svg('<svg viewBox="0 0 16 16"><image href="data:image/png;base64,AA=="/></svg>')
        rejected = self.manifest(colorModel="fullColor")
        self.assertIn("without fullColor allowEmbeddedDataImages", self.load_error(rejected))
        accepted = self.manifest(colorModel="fullColor", allowEmbeddedDataImages=True)
        _, entries = generator._load_manifest(accepted)
        self.assertEqual(len(entries), 1)

    def test_large_assets_are_emitted_as_separate_msvc_safe_literals(self) -> None:
        expression = generator._byte_array_expression("x" * 30_000, 7)
        self.assertEqual(expression.count(".append("), 3)
        self.assertIn("ADQT_SVG_7_0", expression)
        self.assertIn("ADQT_SVG_7_2", expression)
        self.assertNotIn(')ADQT_SVG_7_0"\n          R"', expression)


if __name__ == "__main__":
    unittest.main()
