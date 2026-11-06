"""Replace pinned MicroTeX constructor expansion with primitive descriptor rows.

Rows come from the pristine upstream copy plus the established Snow Shot fixes;
no duplicate hand-maintained registry is used. Unexpected source shapes fail closed.
"""
from pathlib import Path
import re


def strip_comments(text):
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda match: "" if match[0].startswith(("//", "/*")) else match[0], text)


def split_rows(text, separator=","):
    text = strip_comments(text)
    rows, start, depth = [], 0, 0
    quote, escaped = None, False
    for index, char in enumerate(text):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char in "({[":
            depth += 1
        elif char in ")}]":
            depth -= 1
            if depth < 0:
                raise RuntimeError("Unbalanced MicroTeX descriptor row")
        elif char == separator and depth == 0:
            if text[start:index].strip():
                rows.append(text[start:index].strip())
            start = index + 1
    if quote or depth:
        raise RuntimeError("Unterminated MicroTeX descriptor row")
    if text[start:].strip():
        rows.append(text[start:].strip())
    return rows


def initializer_span(text, anchor):
    # Strip comments while scanning, not before finding offsets into the source.
    matches = list(re.finditer(re.escape(anchor) + r"\s*(?:=\s*)?\{", text))
    if len(matches) != 1:
        raise RuntimeError(f"MicroTeX descriptor anchor changed: {anchor} ({len(matches)} matches)")
    opening = matches[0].end() - 1
    depth, index = 1, opening + 1
    quote, escaped = None, False
    line_comment, block_comment = False, False
    while index < len(text):
        char, pair = text[index], text[index:index + 2]
        if line_comment:
            line_comment = char != "\n"
        elif block_comment:
            if pair == "*/":
                block_comment = False
                index += 1
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif pair == "//":
            line_comment = True
            index += 1
        elif pair == "/*":
            block_comment = True
            index += 1
        elif char in "\"'":
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return opening, index
        index += 1
    raise RuntimeError(f"Unclosed MicroTeX descriptor initializer: {anchor}")


def checked_replace(text, before, after, count=1):
    if text.count(before) != count:
        raise RuntimeError(f"MicroTeX descriptor expected {count} occurrences: {before[:100]!r}")
    return text.replace(before, after)


def fields(row, count):
    if not row.startswith("{") or not row.endswith("}"):
        raise RuntimeError(f"Expected aggregate MicroTeX row: {row[:100]}")
    values = split_rows(row[1:-1])
    if len(values) != count:
        raise RuntimeError(f"Expected {count} fields in MicroTeX row: {row[:100]}")
    return values


def source_include(text):
    return '#include "snow_tables.h"\n' + text


def rewrite_map(text, anchor, map_type, row_type, expected, transform=None, factory="makeMap"):
    opening, closing = initializer_span(text, anchor)
    rows = split_rows(text[opening + 1:closing])
    if len(rows) != expected:
        raise RuntimeError(f"MicroTeX {anchor}: expected {expected} rows, found {len(rows)}")
    if transform:
        rows = [transform(row) for row in rows]
    body = "\n".join("    " + row + "," for row in rows)
    expression = ("[] {\n"
                  f"  static constexpr {row_type} rows[] = {{\n{body}\n  }};\n"
                  f"  return tex::snow_tables::{factory}<{map_type}>(rows, std::size(rows));\n"
                  "}()")
    prefix = text[:opening]
    if not prefix.rstrip().endswith("="):
        prefix += " = "
    return prefix + expression + text[closing + 1:]


def rewrite_special(text, anchor, row_type, expected, factory, transform):
    opening, closing = initializer_span(text, anchor)
    rows = split_rows(text[opening + 1:closing])
    if len(rows) != expected:
        raise RuntimeError(f"MicroTeX {anchor}: expected {expected} rows, found {len(rows)}")
    body = "\n".join("    " + transform(row) + "," for row in rows)
    expression = ("[] {\n"
                  f"  static constexpr tex::snow_tables::{row_type} rows[] = {{\n{body}\n  }};\n"
                  f"  return tex::snow_tables::{factory}(rows, std::size(rows));\n"
                  "}()")
    prefix = text[:opening]
    if not prefix.rstrip().endswith("="):
        prefix += " = "
    return prefix + expression + text[closing + 1:]


def symbol_row(row):
    match = re.fullmatch(r"(sym|del)\((\w+),\s*([^\s,)]+)\)", row)
    if not match:
        raise RuntimeError(f"MicroTeX symbol row changed: {row}")
    # Stringization permits punctuation such as |, not just C++ identifiers.
    kind, atom_type, name = match.groups()
    return f'{{"{name}", {atom_type}, {str(kind == "del").lower()}}}'


def color_row(row):
    if row.startswith("c("):
        values = split_rows(row[2:-1])
        if len(values) != 5:
            raise RuntimeError(f"MicroTeX CMYK row changed: {row}")
        return "{" + ", ".join([values[0], "0", *values[1:], "true"]) + "}"
    name, value = fields(row, 2)
    return "{" + ", ".join([name, value, "0.f", "0.f", "0.f", "0.f", "false"]) + "}"


def setting_row(row):
    name, value = fields(row, 2)
    font = re.fullmatch(r"static_cast<float>\(__id\((\w+)\)\)", value)
    if font:
        return f'{{{name}, 0.f, "{font[1]}"}}'
    return f"{{{name}, {value}, nullptr}}"


def text_style_row(row):
    name, characters = fields(row, 2)
    entries = fields(characters, 4)
    output = []
    for entry in entries:
        if entry == "nullptr":
            output.append("{0, nullptr}")
            continue
        match = re.fullmatch(r"cf\((\d+),\s*(\w+)\)", entry)
        if not match:
            raise RuntimeError(f"MicroTeX text-style character changed: {entry}")
        output.append(f'{{{match[1]}, "{match[2]}"}}')
    return "{" + name + ", {" + ", ".join(output) + "}}"


def indirect_row(row):
    key, value = fields(row, 2)
    if not re.fullmatch(r"DefaultTeXFont::\w+", value):
        raise RuntimeError(f"MicroTeX range constant changed: {value}")
    return f"{{{key}, &{value}}}"


def compact_commands(text):
    text = checked_replace(text, "{ L##code, m(argc, name) }", "{ L##code, argc, 0, name }")
    text = checked_replace(text, "{ L##code, m(argc, posOpts, name) }",
                           "{ L##code, argc, posOpts, name }")
    text = checked_replace(text, """inline static PreDefMacro* m(int argc, int posOpts, MacroDelegate del) {
  return new PreDefMacro(argc, posOpts, del);
}

inline static PreDefMacro* m(int argc, MacroDelegate del) {
  return new PreDefMacro(argc, del);
}

""", "")
    factory_start = text.index("static map<wstring, MacroInfo*> makeBuiltinCommands() {")
    tail = text[factory_start:]
    opening, closing = initializer_span(tail, "return")
    body = tail[opening + 1:closing]
    # Preprocessor branches and the two existing optional-environment fixes are
    # retained verbatim; GRAPHICS_DEBUG still adds its two original commands.
    declarations = re.findall(r"^\s*mac(?:4)?\(", strip_comments(body), re.MULTILINE)
    if len(declarations) != 296:
        raise RuntimeError(f"MicroTeX expected 296 builtin command rows, found {len(declarations)}")
    end = closing + 1
    if tail[end:end + 3] != ";\n}":
        raise RuntimeError("MicroTeX builtin command factory terminator changed")
    replacement = ("static map<wstring, MacroInfo*> makeBuiltinCommands() {\n"
                   "  static constexpr tex::snow_tables::CommandRow rows[] = {" + body + "\n  };\n"
                   "  return tex::snow_tables::makeCommands(rows, std::size(rows));\n}")
    tail = replacement + tail[end + 3:]
    text = text[:factory_start] + tail

    start, end = initializer_span(text, "void NewCommandMacro::_init_()")
    calls = split_rows(text[start + 1:end], ";")
    rows = []
    counts = {"env": 0, "cmd": 0}
    for call in calls:
        match = re.fullmatch(r"(env|cmd)\((.*)\)", call, re.DOTALL)
        if not match:
            raise RuntimeError(f"MicroTeX predefined macro call changed: {call[:100]}")
        kind, arguments = match.groups()
        values = split_rows(arguments)
        if len(values) != (4 if kind == "env" else 3):
            raise RuntimeError(f"MicroTeX predefined macro argument count changed: {call[:100]}")
        counts[kind] += 1
        if kind == "cmd":
            values.append("nullptr")
        rows.append("    {" + ", ".join(values) + "},")
    if counts != {"env": 23, "cmd": 37}:
        raise RuntimeError(f"MicroTeX predefined macro rows changed: {counts}")
    body = ("\n  static constexpr tex::snow_tables::MacroTemplateRow rows[] = {\n" +
            "\n".join(rows) + "\n  };\n"
            "  tex::snow_tables::initializeMacroTemplates(rows, std::size(rows));\n")
    return text[:start + 1] + body + text[end:]


def compact(root):
    root = Path(root)

    def edit(relative, transform):
        path = root / "src" / relative
        original = path.read_text(encoding="utf-8")
        updated = transform(original)
        if original == updated:
            raise RuntimeError(f"MicroTeX descriptor made no change: {relative}")
        path.write_text(updated, encoding="utf-8", newline="\n")

    # Upstream mixes class/struct forward declarations for this struct. MSVC
    # encodes that difference in shared_ptr return types; the shared helpers
    # expose the inconsistent declarations unless both agree with its definition.
    for relative in ("atom/atom_row.h", "box/box_single.h"):
        edit(relative, lambda text: checked_replace(text, "class CharFont;", "struct CharFont;"))

    edit("res/builtin/tex_symbols.res.cpp", lambda text: source_include(rewrite_special(
        text, "SymbolAtom::_symbols", "SymbolRow", 629, "makeSymbols", symbol_row)))
    edit("res/builtin/formula_mappings.res.cpp", lambda text: source_include(rewrite_map(
        text, "tex::Formula::_symbolFormulaMappings", "std::map<int, std::string>",
        "tex::snow_tables::Pair<int, const char*>", 826)))
    for anchor, count in (("Formula::_symbolMappings", 65), ("Formula::_symbolTextMappings", 56)):
        edit("res/builtin/symbol_mapping.res.cpp", lambda text, anchor=anchor, count=count:
             source_include(rewrite_map(text, anchor, "std::map<int, std::string>",
                                       "tex::snow_tables::Pair<int, const char*>", count)))
    edit("core/formula_def.cpp", lambda text: source_include(rewrite_map(
        text, "Formula::_predefinedTeXFormulasAsString", "std::map<std::wstring, std::wstring>",
        "tex::snow_tables::Pair<const wchar_t*, const wchar_t*>", 130)))
    edit("atom/colors_def.cpp", lambda text: source_include(rewrite_special(
        text, "tex::ColorAtom::_colors", "ColorRow", 69, "makeColors", color_row)))
    edit("res/builtin/tex_param.res.cpp", lambda text: source_include(rewrite_map(
        text, "tex::DefaultTeXFont::_parameters", "std::map<std::string, float>",
        "tex::snow_tables::Pair<const char*, float>", 19)))
    edit("res/builtin/tex_param.res.cpp", lambda text: rewrite_special(
        text, "tex::DefaultTeXFont::_generalSettings", "SettingRow", 5, "makeSettings", setting_row))
    edit("res/builtin/tex_param.res.cpp", lambda text: rewrite_special(
        text, "tex::DefaultTeXFont::_textStyleMappings", "TextStyleRow", 7, "makeTextStyles", text_style_row))
    for anchor, count in (("TeXParser::SUP_SCRIPT_MAP", 16), ("TeXParser::SUB_SCRIPT_MAP", 15)):
        edit("core/parser.cpp", lambda text, anchor=anchor, count=count: source_include(rewrite_map(
            text, anchor, "std::map<wchar_t, char>", "tex::snow_tables::Pair<wchar_t, char>", count)))
    edit("core/parser.cpp", lambda text: rewrite_map(text, "TeXParser::_unparsedContents",
        "std::set<std::wstring>", "const wchar_t*", 6, factory="makeSet"))
    edit("res/parser/font_parser.cpp", lambda text: source_include(rewrite_map(
        text, "DefaultTeXFontParser::_rangeTypeMappings", "std::map<std::string, int>",
        "tex::snow_tables::Pair<const char*, const int*>", 4, indirect_row, "makeIndirectMap")))
    edit("res/parser/font_parser.cpp", lambda text: rewrite_map(
        text, "DefaultTeXFontParser::_charChildParsers", "std::map<std::string, ChildParser>",
        "tex::snow_tables::Pair<const char*, ChildParser>", 4))
    edit("res/parser/formula_parser.cpp", lambda text: source_include(rewrite_map(
        text, "TeXSymbolParser::_typeMappings", "std::map<std::string, tex::AtomType>",
        "tex::snow_tables::Pair<const char*, tex::AtomType>", 8)))
    edit("core/macro_def.cpp", lambda text: source_include(compact_commands(text)))

    edit("res/symbol_def.res.h", lambda text: source_include(checked_replace(checked_replace(
        text, "const tex::__symbol_component x[] = {",
        "static constexpr tex::snow_tables::GlyphRow x[] = {"),
        "tex::DefaultTeXFont::__push_symbols(x, __len(x));",
        "tex::DefaultTeXFont::__push_symbol_rows(x, __len(x));")))
    edit("fonts/fonts.h", lambda text: checked_replace(checked_replace(text,
        "namespace tex {", "namespace tex::snow_tables { struct GlyphRow; }\n\nnamespace tex {"),
        "  static void __push_symbols(const __symbol_component* symbols, const int len);",
        "  static void __push_symbols(const __symbol_component* symbols, const int len);\n\n"
        "  static void __push_symbol_rows(const snow_tables::GlyphRow* symbols, int len);"))
    edit("fonts/fonts.cpp", lambda text: source_include(checked_replace(text,
        "void DefaultTeXFont::addTeXFontDescription(",
        "SNOW_MICROTEX_NOINLINE void DefaultTeXFont::__push_symbol_rows(\n"
        "    const snow_tables::GlyphRow* symbols, int len) {\n"
        "  for (int index = 0; index < len; ++index) {\n"
        "    const auto& row = symbols[index];\n"
        "    _symbolMappings[row.name] = new CharFont(static_cast<wchar_t>(row.code), row.font);\n"
        "  }\n}\n\nvoid DefaultTeXFont::addTeXFontDescription(")))
