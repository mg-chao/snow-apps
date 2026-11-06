"""Focused source/semantic checks for the pinned MicroTeX descriptor patch."""
import argparse
import ast
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from compact_tables import fields, initializer_span, split_rows, strip_comments


arguments = argparse.ArgumentParser()
arguments.add_argument("--upstream", required=True, type=Path)
options, remaining = arguments.parse_known_args()


def original_rows(text, anchor):
    begin, end = initializer_span(text, anchor)
    return split_rows(text[begin + 1:end])


def descriptor_rows(text, anchor):
    begin = text.index(anchor)
    end = text.index("\n}()", begin)
    return original_rows(text[begin:end], "rows[]")


class CompactTablesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="snow-microtex-tables-")
        cls.root = Path(cls.temporary.name)
        cls.upstream = options.upstream / "src"
        shutil.copytree(cls.upstream, cls.root / "src")
        subprocess.run([sys.executable, str(Path(__file__).with_name("patch_source.py")),
                        str(cls.root)], check=True, capture_output=True, text=True, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def read(self, relative, original=False):
        return ((self.upstream if original else self.root / "src") / relative).read_text(encoding="utf-8")

    def test_all_string_scalar_maps_preserve_every_row_and_order(self):
        for relative, anchor, count in (
            ("res/builtin/formula_mappings.res.cpp", "tex::Formula::_symbolFormulaMappings", 826),
            ("res/builtin/symbol_mapping.res.cpp", "Formula::_symbolMappings", 65),
            ("res/builtin/symbol_mapping.res.cpp", "Formula::_symbolTextMappings", 56),
            ("core/formula_def.cpp", "Formula::_predefinedTeXFormulasAsString", 130),
            ("res/builtin/tex_param.res.cpp", "tex::DefaultTeXFont::_parameters", 19),
            ("core/parser.cpp", "TeXParser::SUP_SCRIPT_MAP", 16),
            ("core/parser.cpp", "TeXParser::SUB_SCRIPT_MAP", 15),
            ("core/parser.cpp", "TeXParser::_unparsedContents", 6),
            ("res/parser/font_parser.cpp", "DefaultTeXFontParser::_charChildParsers", 4),
            ("res/parser/formula_parser.cpp", "TeXSymbolParser::_typeMappings", 8),
        ):
            with self.subTest(table=anchor):
                before = original_rows(self.read(relative, True), anchor)
                after = descriptor_rows(self.read(relative), anchor)
                self.assertEqual(len(before), count)
                self.assertEqual(after, before)

    def test_symbols_preserve_punctuation_duplicates_and_flags(self):
        source = self.read("res/builtin/tex_symbols.res.cpp", True)
        expected = []
        for row in original_rows(source, "SymbolAtom::_symbols"):
            match = re.fullmatch(r"(sym|del)\((\w+),\s*([^\s,)]+)\)", row)
            self.assertIsNotNone(match)
            expected.append((match[3], match[2], match[1] == "del"))
        actual = []
        for row in descriptor_rows(self.read("res/builtin/tex_symbols.res.cpp"), "SymbolAtom::_symbols"):
            name, atom_type, delimiter = fields(row, 3)
            actual.append((ast.literal_eval(name), atom_type, delimiter == "true"))
        self.assertEqual(actual, expected)
        self.assertEqual((len(actual), len({row[0] for row in actual})), (629, 620))
        self.assertIn(("|", "ord", False), actual)

    def test_color_rows_keep_exact_numeric_tokens_and_first_priority(self):
        expected = []
        for row in original_rows(self.read("atom/colors_def.cpp", True), "tex::ColorAtom::_colors"):
            if row.startswith("c("):
                name, *cmyk = split_rows(row[2:-1])
                expected.append([name, "0", *cmyk, "true"])
            else:
                name, value = fields(row, 2)
                expected.append([name, value, "0.f", "0.f", "0.f", "0.f", "false"])
        actual = [fields(row, 7) for row in descriptor_rows(self.read("atom/colors_def.cpp"),
                                                          "tex::ColorAtom::_colors")]
        self.assertEqual(actual, expected)
        self.assertEqual(len(actual), 69)
        self.assertEqual(next(row for row in actual if row[0] == '"magenta"')[1], "magenta")

    def test_settings_and_font_ids_remain_runtime_resolved(self):
        relative = "res/builtin/tex_param.res.cpp"
        original = original_rows(self.read(relative, True), "tex::DefaultTeXFont::_generalSettings")
        current = descriptor_rows(self.read(relative), "tex::DefaultTeXFont::_generalSettings")
        self.assertEqual(len(current), 5)
        for old, new in zip(original, current):
            name, value = fields(old, 2)
            actual_name, actual_value, font = fields(new, 3)
            self.assertEqual(name, actual_name)
            if "__id(" in value:
                self.assertEqual(font, '"' + re.search(r"__id\((\w+)\)", value)[1] + '"')
                self.assertEqual(actual_value, "0.f")
            else:
                self.assertEqual((actual_value, font), (value, "nullptr"))
        original = original_rows(self.read(relative, True), "tex::DefaultTeXFont::_textStyleMappings")
        current = descriptor_rows(self.read(relative), "tex::DefaultTeXFont::_textStyleMappings")
        self.assertEqual(len(current), 7)
        for old, new in zip(original, current):
            name, old_chars = fields(old, 2)
            actual_name, new_chars = fields(new, 2)
            self.assertEqual(name, actual_name)
            for before, after in zip(fields(old_chars, 4), fields(new_chars, 4)):
                code, font = fields(after, 2)
                if before == "nullptr":
                    self.assertEqual((code, font), ("0", "nullptr"))
                else:
                    match = re.fullmatch(r"cf\((\d+),\s*(\w+)\)", before)
                    self.assertEqual((code, font), (match[1], '"' + match[2] + '"'))

    def test_private_dispatch_constants_use_addresses_without_value_changes(self):
        relative, anchor = "res/parser/font_parser.cpp", "DefaultTeXFontParser::_rangeTypeMappings"
        before = original_rows(self.read(relative, True), anchor)
        after = descriptor_rows(self.read(relative), anchor)
        self.assertEqual(len(after), 4)
        for old, new in zip(before, after):
            key, value = fields(old, 2)
            self.assertEqual(fields(new, 2), [key, "&" + value])

    def test_primitive_macro_rows_and_debug_branch_are_unchanged(self):
        old = self.read("core/macro_def.cpp", True)
        old = old.replace('mac(3, macro_newenvironment, "newenvironment")',
                          'mac4(3, 2, macro_newenvironment, "newenvironment")')
        old = old.replace('mac(3, macro_renewenvironment, "renewenvironment")',
                          'mac4(3, 2, macro_renewenvironment, "renewenvironment")')
        begin, end = initializer_span(old, "MacroInfo::_commands")
        expected = strip_comments(old[begin + 1:end]).split()
        text = self.read("core/macro_def.cpp")
        start, stop = initializer_span(text, "static map<wstring, MacroInfo*> makeBuiltinCommands()")
        function = text[start + 1:stop]
        begin, end = initializer_span(function, "rows[]")
        self.assertEqual(strip_comments(function[begin + 1:end]).split(), expected)
        self.assertIn("#ifdef GRAPHICS_DEBUG", function)
        self.assertIn('macro_debug, "debug"', function)
        self.assertIn('macro_undebug, "undebug"', function)
        self.assertNotIn("new PreDefMacro", text)

    def test_every_template_command_environment_call_keeps_order_and_types(self):
        relative = "core/macro_def.cpp"
        old = self.read(relative, True)
        begin, end = initializer_span(old, "void NewCommandMacro::_init_()")
        expected = []
        for call in split_rows(old[begin + 1:end], ";"):
            kind, arguments = re.fullmatch(r"(env|cmd)\((.*)\)", call, re.DOTALL).groups()
            row = split_rows(arguments)
            if kind == "cmd":
                row.append("nullptr")
            expected.append(row)
        text = self.read(relative)
        begin, end = initializer_span(text, "void NewCommandMacro::_init_()")
        actual = [fields(row, 4) for row in original_rows(text[begin + 1:end], "rows[]")]
        self.assertEqual(actual, expected)
        self.assertEqual(len(actual), 60)

    def test_font_registration_and_numeric_metric_sources_are_byte_identical(self):
        for folder in ("res/font", "res/sym"):
            for source in (self.upstream / folder).glob("*.cpp"):
                with self.subTest(source=source.name):
                    self.assertEqual((self.root / "src" / folder / source.name).read_bytes(), source.read_bytes())
        self.assertEqual(self.read("res/font_def.res.h"), self.read("res/font_def.res.h", True))
        source = self.read("res/symbol_def.res.h")
        self.assertIn("static constexpr tex::snow_tables::GlyphRow x[]", source)
        self.assertIn("__push_symbol_rows(x, __len(x))", source)
        self.assertIn("__push_symbols(const __symbol_component*", self.read("fonts/fonts.h"))
        self.assertIn("_symbolMappings[row.name] = new CharFont", self.read("fonts/fonts.cpp"))

    def test_balanced_literals_and_unsupported_shapes_fail_closed(self):
        self.assertEqual(split_rows(r'''{L"x,{y}", L"\\macro{a,b}"}, {'\'', "//literal"},'''),
                         [r'''{L"x,{y}", L"\\macro{a,b}"}''', r'''{'\'', "//literal"}'''])
        with self.assertRaises(RuntimeError):
            split_rows('{"unterminated}')
        with self.assertRaises(RuntimeError):
            initializer_span("a = {}; a = {};", "a")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
