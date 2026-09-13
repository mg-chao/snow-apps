"""Offscreen tests for feature catalog ownership, preservation and stable updates."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET


PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "translation_catalogs", PROJECT / "scripts/translation_catalogs.py")
catalogs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(catalogs)


class TranslationCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.directory = self.root / "i18n"
        self.directory.mkdir()
        self.inputs = self.root / "input"
        self.inputs.mkdir()
        self.manifest = self.directory / "modules.json"
        self.manifest.write_text(json.dumps({"core": ["Core"], "settings": ["Settings"]}))
        for locale in catalogs.LOCALES:
            root = catalogs.empty_catalog(locale)
            for context_name in ("Settings", "Core"):
                context = ET.SubElement(root, "context")
                ET.SubElement(context, "name").text = context_name
                for source in ("Value %1", "Apply"):
                    message = ET.SubElement(context, "message")
                    ET.SubElement(message, "location", filename="../widget.cpp", line="100")
                    ET.SubElement(message, "source").text = source
                    ET.SubElement(message, "translation").text = source
            self.save_input(locale, root)
        catalogs.split(self.directory, self.inputs)

    def save_input(self, locale, root):
        ET.ElementTree(root).write(self.inputs / f"snow_shot_{locale}.ts", encoding="utf-8")

    def fragment(self, locale="en_US", module="core"):
        return self.directory / module / f"snow_shot_{module}_{locale}.ts"

    def snapshot(self):
        return {p.relative_to(self.directory): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.directory.rglob("*.ts")}

    def test_repository_catalogs_are_complete_and_canonical(self):
        catalogs.check(PROJECT / "i18n")

    def test_split_merge_update_is_byte_and_mtime_stable(self):
        before = self.snapshot()
        catalogs.merge(self.directory, self.inputs)
        catalogs.split(self.directory, self.inputs)
        self.assertEqual(before, self.snapshot())
        catalogs.check(self.directory)

    def test_source_order_and_locations_do_not_change_fragments(self):
        before = self.snapshot()
        for locale in catalogs.LOCALES:
            root = ET.parse(self.inputs / f"snow_shot_{locale}.ts").getroot()
            root[:] = list(reversed(root))
            for location in root.findall(".//location"):
                location.set("line", "999")
                location.set("filename", "../moved.cpp")
            self.save_input(locale, root)
        catalogs.split(self.directory, self.inputs)
        self.assertEqual(before, self.snapshot())

    def test_translation_edit_changes_only_its_feature_and_locale(self):
        before = self.snapshot()
        root = ET.parse(self.inputs / "snow_shot_zh_CN.ts").getroot()
        for context in root.findall("context"):
            if context.findtext("name") == "Core":
                context.find("message/translation").text = "Value translated %1"
        self.save_input("zh_CN", root)
        catalogs.split(self.directory, self.inputs)
        after = self.snapshot()
        changed = {path for path in before if before[path] != after[path]}
        self.assertEqual(changed, {Path("core/snow_shot_core_zh_CN.ts")})
        catalogs.check(self.directory)

    def test_added_and_removed_messages_leave_other_features_untouched(self):
        before = self.snapshot()
        for locale in catalogs.LOCALES:
            root = ET.parse(self.inputs / f"snow_shot_{locale}.ts").getroot()
            for context in root.findall("context"):
                if context.findtext("name") == "Core":
                    context.remove(context.find("message"))
                    message = ET.SubElement(context, "message")
                    ET.SubElement(message, "source").text = "New action"
                    ET.SubElement(message, "translation").text = "New action"
            self.save_input(locale, root)
        catalogs.split(self.directory, self.inputs)
        after = self.snapshot()
        changed = {path for path in before if before[path] != after[path]}
        self.assertEqual(changed, {
            Path(f"core/snow_shot_core_{locale}.ts") for locale in catalogs.LOCALES})
        catalogs.check(self.directory)

    def test_unknown_context_does_not_partially_write_any_locale(self):
        before = self.snapshot()
        for locale in catalogs.LOCALES:
            root = ET.parse(self.inputs / f"snow_shot_{locale}.ts").getroot()
            root.find("context/name").text = "NewFeature"
            self.save_input(locale, root)
        with self.assertRaisesRegex(ValueError, "Unassigned context"):
            catalogs.split(self.directory, self.inputs)
        self.assertEqual(before, self.snapshot())

    def test_parity_failure_does_not_partially_write_any_locale(self):
        before = self.snapshot()
        root = ET.parse(self.inputs / "snow_shot_zh_TW.ts").getroot()
        root.find("context/message/source").text = "Only one language has this key"
        self.save_input("zh_TW", root)
        with self.assertRaisesRegex(ValueError, "message keys differ"):
            catalogs.split(self.directory, self.inputs)
        self.assertEqual(before, self.snapshot())

    def test_metadata_plural_disambiguation_and_mixed_text_survive(self):
        for locale in catalogs.LOCALES:
            root = catalogs.empty_catalog(locale)
            root.set("extra-po-header-language_team", "Snow Shot")
            ET.SubElement(root, "extra-po-header", {"example": "preserve"}).text = "metadata"
            context = ET.SubElement(root, "context")
            ET.SubElement(context, "name").text = "Core"
            ET.SubElement(context, "comment").text = "Context note"
            message = ET.SubElement(context, "message", id="items", numerus="yes")
            ET.SubElement(message, "source").text = "%n item(s) %1"
            ET.SubElement(message, "comment").text = "noun"
            ET.SubElement(message, "extracomment").text = "Keep placeholders"
            ET.SubElement(message, "translatorcomment").text = "翻译备注"
            ET.SubElement(message, "oldsource").text = "%n file(s) %1"
            translation = ET.SubElement(message, "translation")
            ET.SubElement(translation, "numerusform").text = "%n 项 %1"
            ET.SubElement(translation, "numerusform").text = "%n 项目 %1"
            mixed = ET.SubElement(context, "message")
            ET.SubElement(mixed, "source").text = "Whitespace"
            text = ET.SubElement(mixed, "translation")
            text.text = " "
            ET.SubElement(text, "byte", value="x9").tail = " \n "
            # Same source with a different disambiguation is a separate key.
            alternate = copy.deepcopy(message)
            alternate.set("id", "items-verb")
            alternate.find("comment").text = "verb"
            context.append(alternate)
            self.save_input(locale, root)
        before = {locale: catalogs.canonical_bytes(
            ET.parse(self.inputs / f"snow_shot_{locale}.ts").getroot())
                  for locale in catalogs.LOCALES}
        catalogs.split(self.directory, self.inputs)
        catalogs.merge(self.directory, self.root / "merged")
        for locale in catalogs.LOCALES:
            self.assertEqual(before[locale],
                             (self.root / "merged" / f"snow_shot_{locale}.ts").read_bytes())

    def test_duplicate_lookup_key_is_rejected_even_with_different_numerus(self):
        path = self.fragment()
        root = ET.parse(path).getroot()
        duplicate = copy.deepcopy(root.find("context/message"))
        duplicate.set("numerus", "yes")
        root.find("context").append(duplicate)
        ET.ElementTree(root).write(path, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate translation key"):
            catalogs.merge(self.directory, self.inputs)

    def test_duplicate_ids_across_modules_are_rejected(self):
        for module in ("core", "settings"):
            path = self.fragment(module=module)
            root = ET.parse(path).getroot()
            root.find("context/message").set("id", "same-id")
            ET.ElementTree(root).write(path, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate translation key"):
            catalogs.merge(self.directory, self.inputs)

    def test_wrong_module_is_rejected(self):
        self.manifest.write_text(json.dumps({"core": ["Settings"], "settings": ["Core"]}))
        with self.assertRaisesRegex(ValueError, "belongs to"):
            catalogs.merge(self.directory, self.inputs)

    def test_duplicate_context_assignment_is_rejected(self):
        self.manifest.write_text(json.dumps({"core": ["Core"], "settings": ["Core"]}))
        with self.assertRaisesRegex(ValueError, "Duplicate context assignment"):
            catalogs.merge(self.directory, self.inputs)

    def test_invalid_module_path_is_rejected(self):
        self.manifest.write_text(json.dumps({"../escape": ["Core"]}))
        with self.assertRaisesRegex(ValueError, "Invalid module name"):
            catalogs.merge(self.directory, self.inputs)

    def test_missing_or_unexpected_catalog_is_rejected(self):
        unexpected = self.directory / "unexpected.ts"
        unexpected.write_bytes(self.fragment().read_bytes())
        with self.assertRaisesRegex(ValueError, "Catalog files differ"):
            catalogs.merge(self.directory, self.inputs)
        unexpected.unlink()
        self.fragment().unlink()
        with self.assertRaisesRegex(ValueError, "Catalog files differ"):
            catalogs.merge(self.directory, self.inputs)

    def test_wrong_locale_is_rejected(self):
        path = self.fragment()
        root = ET.parse(path).getroot()
        root.set("language", "zh_CN")
        ET.ElementTree(root).write(path, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "expected a TS catalog for en_US"):
            catalogs.merge(self.directory, self.inputs)

    def test_unfinished_can_be_updated_but_fails_check(self):
        path = self.fragment("zh_CN")
        root = ET.parse(path).getroot()
        root.find("context/message/translation").set("type", "unfinished")
        path.write_bytes(catalogs.canonical_bytes(root))
        catalogs.merge(self.directory, self.inputs)
        catalogs.split(self.directory, self.inputs)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            catalogs.check(self.directory)

    def test_changed_placeholder_is_rejected(self):
        path = self.fragment("zh_CN")
        root = ET.parse(path).getroot()
        root.find("context/message/translation").text = "Unexpected %2"
        path.write_bytes(catalogs.canonical_bytes(root))
        with self.assertRaisesRegex(ValueError, "changed placeholders"):
            catalogs.check(self.directory)

    def test_nonidentity_english_is_rejected(self):
        path = self.fragment()
        root = ET.parse(path).getroot()
        root.find("context/message/translation").text = "Different English"
        path.write_bytes(catalogs.canonical_bytes(root))
        with self.assertRaisesRegex(ValueError, "en_US must be identity"):
            catalogs.check(self.directory)


if __name__ == "__main__":
    unittest.main()
