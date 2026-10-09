"""Apply the reviewed Snow Shot integration to an immutable MicroTeX source copy.

Never patch the downloaded tree in place. Exact-match assertions fail closed when the
upstream revision changes; the caller creates a fresh build-local copy beforehand.
"""

from pathlib import Path
import re
import sys


root = Path(sys.argv[1])


def edit(relative, transform):
    path = root / relative
    original = path.read_text(encoding="utf-8")
    updated = transform(original)
    if updated == original:
        raise RuntimeError(f"MicroTeX patch made no change: {relative}")
    path.write_text(updated, encoding="utf-8", newline="\n")


def replace(text, before, after, count=1):
    if text.count(before) != count:
        raise RuntimeError(f"MicroTeX patch expected {count} occurrences: {before[:100]!r}")
    return text.replace(before, after)


def friends(text, classes):
    for name in classes:
        pattern = rf"(class {name}(?:\s*:\s*[^{{]+)?\s*{{)"
        text, count = re.subn(pattern, r"\1\n  friend struct SnowSessionState;", text)
        if count != 1:
            raise RuntimeError(f"MicroTeX friend target changed: {name}")
    return text


for relative, names in (
    ("src/core/macro.h", ["NewCommandMacro"]),
    ("src/core/formula.h", ["Formula"]),
    ("src/atom/atom_basic.h", ["ColorAtom"]),
    ("src/atom/atom_matrix.h", ["MatrixAtom"]),
    ("src/fonts/fonts.h", ["DefaultTeXFont"]),
):
    edit(relative, lambda text, names=names: friends(text, names))


def command_factory(text):
    # The existing environment delegates read their optional argc from args[4].
    # Register its position after the name so standard [argc] syntax reaches them.
    text = replace(text, 'mac(3, macro_newenvironment, "newenvironment")',
                   'mac4(3, 2, macro_newenvironment, "newenvironment")')
    text = replace(text, 'mac(3, macro_renewenvironment, "renewenvironment")',
                   'mac4(3, 2, macro_renewenvironment, "renewenvironment")')
    text = replace(text, "map<wstring, MacroInfo*> MacroInfo::_commands{",
                   "static map<wstring, MacroInfo*> makeBuiltinCommands() {\n  return {")
    text = replace(text, "};\n\nmap<wstring, wstring> NewCommandMacro::_codes;", """  };
}

map<wstring, MacroInfo*> MacroInfo::_commands = makeBuiltinCommands();

void tex::snowRebuildCommands() {
  auto next = makeBuiltinCommands();
  for (const auto& entry : MacroInfo::_commands) delete entry.second;
  MacroInfo::_commands.swap(next);
}

map<wstring, wstring> NewCommandMacro::_codes;""")
    return '#include "snow_preview.h"\n' + text


edit("src/core/macro_def.cpp", command_factory)

edit("src/core/formula.h", lambda text: replace(
    text, "  Formula();", "  Formula();\n\n  Formula(bool isPartial, const std::wstring& latex);"))
edit("src/core/formula.cpp", lambda text: replace(
    text, 'Formula::Formula() : _parser(L"", this, false) {}', """Formula::Formula() : _parser(L"", this, false) {}

Formula::Formula(bool isPartial, const wstring& latex)
  : _parser(isPartial, latex, this, true) {
  _parser.parse();
}"""))


def resource_root(text):
    start = text.index('static string CHECK_FILE = ".clatexmath-res_root";')
    end = text.index("void LaTeX::init(string res_root_path)")
    text = text[:start] + """Formula* LaTeX::_formula = nullptr;
TeXRenderBuilder* LaTeX::_builder = nullptr;

""" + text[end:]
    start = text.index("  try {", text.index("void LaTeX::init"))
    end = text.index("  if (_formula != nullptr) return;", start)
    text = text[:start] + "  RES_BASE = std::move(res_root_path);\n" + text[end:]
    return text


edit("src/latex.cpp", resource_root)


def xml_loader(text):
    text, count = re.subn(r"(\w+)\.LoadFile\(file\.c_str\(\)\)",
                          r"tex::snowLoadXml(\1, file)", text)
    text, count_path = re.subn(r"(\w+)\.LoadFile\(path\.c_str\(\)\)",
                               r"tex::snowLoadXml(\1, path)", text)
    if count + count_path == 0:
        raise RuntimeError("MicroTeX XML loader target changed")
    return '#include "snow_preview.h"\n' + text


for relative in ("src/res/parser/font_parser.h", "src/res/parser/font_parser.cpp",
                 "src/res/parser/formula_parser.cpp"):
    edit(relative, xml_loader)

def qt_font_backend(text):
    # MicroTeX stores math fonts at one logical em and scales the painter afterwards.
    # Qt raster engines cannot reliably hint that one-pixel font. Keep 64x precision
    # inside QFont, then undo it at metric and paint boundaries so layout remains logical.
    text = replace(text, "_font.setPointSizeF(size);",
                   "_font.setPixelSize(qMax(1, qRound(size * 64)));\n  _font.setHintingPreference(QFont::PreferNoHinting);", 2)
    text = replace(text, "return _font.pointSizeF();", "return _font.pixelSize() / 64.f;")
    for attribute, getter in (("x", "left"), ("y", "top"), ("w", "width"), ("h", "height")):
        text = replace(text, f"  r.{attribute} = br.{getter}();",
                       f"  r.{attribute} = br.{getter}() / 64;")
    text = replace(text, """  g.getQPainter()->setFont(_font);
  g.getQPainter()->drawText(QPointF(x, y), _text);""", """  auto* painter = g.getQPainter();
  painter->save();
  painter->translate(x, y);
  painter->scale(1.0 / 64, 1.0 / 64);
  painter->setFont(_font);
  painter->drawText(QPointF(), _text);
  painter->restore();""")
    text = replace(text, "  _painter->setFont(_font->getQFont());", """  _painter->save();
  _painter->translate(x, y);
  _painter->scale(1.0 / 64, 1.0 / 64);
  _painter->setFont(_font->getQFont());""")
    text = replace(text, "  _painter->drawText(QPointF(x, y), text);",
                   "  _painter->drawText(QPointF(), text);\n  _painter->restore();")
    text = replace(text, """  if(!QFile::exists(filename)) {
      filename.prepend(":/");
//      qInfo() << "new filename" << filename;
  }""", "  if (!QFile::exists(filename)) throw tex::SnowPreviewResource();")
    text = replace(text, "  if( id == -1 ) {", "  if( id == -1 ) {\n    throw tex::SnowPreviewResource();")
    text = replace(text, "    } else {\n#ifdef HAVE_LOG\n    __log << file << \" no font families found\\n\";",
                   "    } else {\n      throw tex::SnowPreviewResource();\n#ifdef HAVE_LOG\n    __log << file << \" no font families found\\n\";")
    return '#include "snow_preview.h"\n' + text


edit("src/platform/qt/graphic_qt.cpp", qt_font_backend)


def matrix_storage(text):
    text = replace(text, "        wstring str;\n        for (int j = 0; j < nrep; j++) str += args[2];\n        opt.insert(pos, str);", """        wstring str;
        if (nrep < 0 || nrep > 200000 ||
            (!args[2].empty() && static_cast<size_t>(nrep) > 1000000 / args[2].size()))
          throw tex::SnowPreviewLimit();
        const size_t expanded = static_cast<size_t>(nrep) * args[2].size();
        tex::SnowPreviewBudget::replacement(opt.size(), 0, expanded);
        str.reserve(expanded);
        for (int j = 0; j < nrep; j++) {
          tex::SnowPreviewBudget::check();
          str += args[2];
        }
        opt.insert(pos, str);""")
    text = replace(text, "            opt.insert(spos, it->second);",
                   "            tex::SnowPreviewBudget::replacement(opt.size(), 0, it->second.size());\n            opt.insert(spos, it->second);")
    text = replace(text, "  auto* arr = new float[cols + 1]();",
                   "  std::unique_ptr<float[]> arr(new float[cols + 1]());")
    text = replace(text, "return arr;", "return arr.release();", 3)
    text = replace(text, """  auto* lineDepth = new float[rows]();
  auto* lineHeight = new float[rows]();
  auto* colWidth = new float[cols]();
  auto** boxarr = new sptr<Box>* [rows]();
  for (int i = 0; i < rows; i++) boxarr[i] = new sptr<Box>[cols]();""", """  tex::SnowPreviewBudget::matrix(rows, cols);
  std::unique_ptr<float[]> lineDepthOwner(new float[rows]());
  std::unique_ptr<float[]> lineHeightOwner(new float[rows]());
  std::unique_ptr<float[]> colWidthOwner(new float[cols]());
  auto* lineDepth = lineDepthOwner.get();
  auto* lineHeight = lineHeightOwner.get();
  auto* colWidth = colWidthOwner.get();
  std::vector<std::vector<sptr<Box>>> boxStorage(rows, std::vector<sptr<Box>>(cols));
  std::vector<sptr<Box>*> rowPointers;
  rowPointers.reserve(rows);
  for (auto& row : boxStorage) rowPointers.push_back(row.data());
  auto** boxarr = rowPointers.data();""")
    text = replace(text, "  float* Hsep = getColumnSep(env, matW);",
                   "  std::unique_ptr<float[]> separatorOwner(getColumnSep(env, matW));\n  auto* Hsep = separatorOwner.get();")
    text = replace(text, "          WrapperBox* wb = nullptr;",
                   "          std::shared_ptr<WrapperBox> wb;")
    text = replace(text, "            wb = new WrapperBox(\n",
                   "            wb = sptrOf<WrapperBox>(\n")
    text = replace(text, "            wb = new WrapperBox(b, b->_width, lineHeight[i], lineDepth[i], Alignment::left);",
                   "            wb = sptrOf<WrapperBox>(b, b->_width, lineHeight[i], lineDepth[i], Alignment::left);")
    text = replace(text, "  int count = (int) floor(x);", """  if (!std::isfinite(x) || x < -200000.f || x > 200000.f)
    throw tex::SnowPreviewLimit();
  int count = static_cast<int>(floor(x));""")
    text = replace(text, """  delete[] Hsep;
  delete[] lineDepth;
  delete[] lineHeight;
  delete[] colWidth;
  for (int i = 0; i < rows; i++) delete[] boxarr[i];
  delete[] boxarr;
""", "")
    return '#include "snow_preview.h"\n#include <cmath>\n' + text


edit("src/atom/atom_matrix.cpp", matrix_storage)
edit("src/core/formula.cpp", lambda text: replace(
    replace(text, "void ArrayFormula::addCol(int n) {", """void ArrayFormula::addCol(int n) {
  if (n <= 0) throw ex_parse("Column span must be positive");
  if (n > 200000) throw tex::SnowPreviewLimit();"""),
    "  for (size_t i = 0; i < _row; i++) {",
    "  tex::SnowPreviewBudget::matrix(_row, _col);\n  for (size_t i = 0; i < _row; i++) {"))


def numeric_spans(text):
    text = replace(text, "  auto* arr = new ArrayFormula();",
                   "  auto arr = sptrOf<ArrayFormula>();", 11)
    text = replace(text, ", arr, false);", ", arr.get(), false);", 11)
    text = replace(text, "inline macro(multicolumn) {", """inline macro(multicolumn) {
  if (!tp.isArrayMode()) throw ex_parse("Multicolumn requires an array environment");""")
    text = replace(text, "  tp.addAtom(sptrOf<MultiRowAtom>(n, args[2], Formula(tp, args[3])._root));", """  if (n == 0) throw ex_parse("Row span must be nonzero");
  if (n < -200000 || n > 200000) throw tex::SnowPreviewLimit();
  tp.addAtom(sptrOf<MultiRowAtom>(n, args[2], Formula(tp, args[3])._root));""")
    return text


edit("src/core/macro_impl.h", numeric_spans)
edit("src/core/macro.cpp", lambda text: replace(
    replace(text, "  wstring n = name + L\"@env\";", """  if (argc < 0) throw ex_parse("Environment argument count must be nonnegative");
  if (argc >= 200000) throw tex::SnowPreviewLimit();
  wstring n = name + L"@env";"""),
    "  if (_codes.find(name + L\"@env\") == _codes.end()) {", """  if (argc < 0) throw ex_parse("Environment argument count must be nonnegative");
  if (argc >= 200000) throw tex::SnowPreviewLimit();
  if (_codes.find(name + L"@env") == _codes.end()) {"""))
edit("src/atom/atom_matrix.cpp", lambda text: replace(
    replace(text, "      m->_i = ++j;", """      if (skipped == 0) throw ex_parse("Row span contains no data rows");
      m->_i = ++j;"""),
    "    m->_n = abs(n);", """    if (skipped == 0) throw ex_parse("Row span contains no data rows");
    m->_n = min(abs(n), rows - m->_i);"""))


def checked_long_division(text):
    text = replace(text, "  long quotient = _dividend / _divisor;", """  const long lower = std::numeric_limits<long>::min();
  const long upper = std::numeric_limits<long>::max();
  if (_divisor == 0 || (_dividend == lower && _divisor == -1))
    throw tex::SnowPreviewLimit();
  long quotient = _dividend / _divisor;""")
    text = replace(text, """    long b = (x[i] - '0') * pow(10, len - i - 1);
    long product = b * _divisor;
    remaining = remaining - product;""", """    long b = x[i] - '0';
    for (size_t place = i + 1; place < len; ++place) {
      if (b > upper / 10 || b < lower / 10) throw tex::SnowPreviewLimit();
      b *= 10;
    }
    if ((b > 0 && ((_divisor > 0 && b > upper / _divisor) ||
                  (_divisor < 0 && _divisor < lower / b))) ||
        (b < 0 && ((_divisor > 0 && b < lower / _divisor) ||
                  (_divisor < 0 && b < upper / _divisor))))
      throw tex::SnowPreviewLimit();
    const long product = b * _divisor;
    if ((product > 0 && remaining < lower + product) ||
        (product < 0 && remaining > upper + product))
      throw tex::SnowPreviewLimit();
    remaining -= product;""")
    return '#include "snow_preview.h"\n#include <limits>\n' + text


edit("src/atom/atom_impl.cpp", checked_long_division)
edit("src/core/core.cpp", lambda text: replace(
    replace(text, "  auto* cumWidth = new float[count + 1]();",
            "  std::unique_ptr<float[]> cumWidth(new float[count + 1]());"),
    "delete[] cumWidth;", "", 3))
edit("src/box/box_factory.cpp", lambda text: replace(
    replace(text, "    Extension* ext = tf.getExtension(c, style);",
            "    std::unique_ptr<Extension> ext(tf.getExtension(c, style));"),
    "    delete ext;\n", ""))
edit("src/atom/atom_impl.h", lambda text: replace(
    replace(replace(replace(replace(replace(
        text, "    Box* y;", "    sptr<Box> y;"),
        "    Box* y = nullptr;", "    sptr<Box> y;", 2),
        "    Box* cedilla = new CharBox(ch);", "    auto cedilla = sptrOf<CharBox>(ch);"),
        "    Box* ogonek = new CharBox(ch);", "    auto ogonek = sptrOf<CharBox>(ch);"),
        "    auto* T = new CharBox(t);", "    auto T = sptrOf<CharBox>(t);", 2),
        "    auto* B = new CharBox(ch);", "    auto B = sptrOf<CharBox>(ch);"))


# These locals are assembled across recursive calls and budget checkpoints. Share ownership
# immediately rather than only in the final return expression, so all unwind paths release.
for folder in ("atom", "box", "core"):
    for path in (root / "src" / folder).glob("*"):
        if path.suffix not in (".cpp", ".h"):
            continue
        original = path.read_text(encoding="utf-8")
        updated = re.sub(
            r"auto\s*\*?\s+(\w+) = new (HBox|VBox|VRowAtom|RowAtom)\(([^\n]*?)\);",
            r"auto \1 = sptrOf<\2>(\3);", original)
        if updated != original:
            path.write_text(updated, encoding="utf-8", newline="\n")


def budget_loops(text):
    # Check work/deadline/cancellation in both parsing and box-building loops. This includes
    # stretchy delimiters, matrix column expansion, and custom macro preprocessing.
    # Several upstream loops intentionally omit braces, including column padding and
    # literal scanning. Normalize their single-statement bodies before inserting checks.
    lines = []
    count = 0
    for line in text.splitlines(keepends=True):
        match = re.match(r"([ \t]*)(?:for|while)\s*\(", line)
        if match is None:
            lines.append(line)
            continue
        depth = 1
        quote = None
        escaped = False
        end = match.end()
        while end < len(line) and depth:
            char = line[end]
            if quote is not None:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
            elif char in ("'", '"'):
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            end += 1
        if depth:
            lines.append(line)
            continue
        indent = match.group(1)
        header = line[:end]
        body = line[end:].strip()
        check = indent + "  tex::SnowPreviewBudget::check();\n"
        if body.startswith("{"):
            lines.append(header + " {\n" + check + indent + body[1:].lstrip() + "\n")
        elif body.endswith(";") and "{" not in body and "}" not in body:
            lines.append(header + " {\n" + check + indent + "  " + body + "\n" + indent + "}\n")
        else:
            lines.append(line)
            continue
        count += 1
    text = "".join(lines)
    if count == 0:
        return text
    return '#include "snow_preview.h"\n' + text


for folder in ("atom", "box", "core"):
    for path in (root / "src" / folder).glob("*"):
        if path.suffix not in (".cpp", ".h"):
            continue
        original = path.read_text(encoding="utf-8")
        updated = budget_loops(original)
        if updated != original:
            path.write_text(updated, encoding="utf-8", newline="\n")


def parser_budget(text):
    text = replace(text,
                   '  for (int i = 1; i <= mac->_argc - 1; i++) expr += L"{" + optargs[i] + L"}";',
                   """  for (int i = 1; i <= mac->_argc - 1; i++) {
    tex::SnowPreviewBudget::check();
    tex::SnowPreviewBudget::replacement(expr.size(), 0, optargs[i].size() + 2);
    expr.push_back(L'{');
    expr.append(optargs[i]);
    expr.push_back(L'}');
  }""")
    text = replace(text, "  args.resize(argc + 10 + 1 + 1);", """  if (argc < 0 || argc > 200000) throw tex::SnowPreviewLimit();
  tex::SnowPreviewBudget::length(static_cast<size_t>(argc) + 12);
  args.resize(argc + 10 + 1 + 1);""")
    text = replace(text, "  auto getArg = [&](int i) { // NOLINT(misc-no-recursion)\n    skipWhiteSpace();",
                   "  auto getArg = [&](int i) { // NOLINT(misc-no-recursion)\n    skipWhiteSpace();\n    if (_pos >= _len) throw ex_parse(\"Missing command argument\");")
    text = replace(text, "void TeXParser::parse() {", """void TeXParser::parse() {
  tex::SnowPreviewBudget::Depth snowDepth;
  tex::SnowPreviewBudget::length(_latex.size());""")
    text = replace(text, "  _pos = _spos = _len = 0;", """  tex::SnowPreviewBudget::Depth snowDepth;
  tex::SnowPreviewBudget::length(latex.size());
  _pos = _spos = _len = 0;""")
    # Validate projected replacement length before std::wstring allocates expanded storage.
    replacements = (
        ("_latex.replace(beg, end - beg, formula);", "beg", "end - beg", "formula"),
        ("_latex.replace(_pos, 1, sup);", "_pos", "1", "sup"),
        ("_latex.replace(_pos, 1, sub);", "_pos", "1", "sub"),
        ("_latex.replace(pos, _pos - pos, args.back());", "pos", "_pos - pos", "args.back()"),
        ("_latex.replace(pos, _pos - pos, expr);", "pos", "_pos - pos", "expr"),
    )
    for before, _, removed, inserted in replacements:
        text = replace(text, before,
                       f"tex::SnowPreviewBudget::replacement(_latex.size(), {removed}, {inserted}.size());\n    {before}")
    return text


edit("src/core/parser.cpp", parser_budget)


def replacement_budget(text):
    text = replace(text, "src.replace(start, from.length(), to);", """tex::SnowPreviewBudget::replacement(src.size(), from.size(), to.size());
    src.replace(start, from.length(), to);""", 4)
    return '#include "snow_preview.h"\n' + text


edit("src/utils/string_utils.h", replacement_budget)


def render_dimensions(text):
    # The preview never requests diagnostic boxes. Keep this upstream debug branch and
    # its recursive graph wrapper out of every configuration, including Debug builds.
    text = replace(text, "  if (Box::DEBUG) {", "#ifdef GRAPHICS_DEBUG\n  if (Box::DEBUG) {")
    text = replace(text, "  }\n}\n\nsptr<BoxGroup> TeXRender::wrap", 
                   "  }\n#endif\n}\n\n#ifdef GRAPHICS_DEBUG\nsptr<BoxGroup> TeXRender::wrap")
    text = replace(text, "float TeXRender::getTextSize() const {", 
                   "#endif\n\nfloat TeXRender::getTextSize() const {")
    # Parser cancellation can unwind box creation. Own this builder's environment so
    # every abandoned live-preview request releases its temporary font and layout state.
    text = replace(text, "  Environment* env;", "  std::unique_ptr<Environment> env;")
    text = replace(text, "    env = new Environment(_style, tf, _widthUnit, _textWidth);",
                   "    env = std::make_unique<Environment>(_style, tf, _widthUnit, _textWidth);")
    text = replace(text, "    env = new Environment(_style, tf);",
                   "    env = std::make_unique<Environment>(_style, tf);")
    text = replace(text, "  delete env;\n", "")
    for name in ("getHeight", "getDepth", "getWidth"):
        start = text.index(f"int TeXRender::{name}() const {{")
        end = text.index("\n}", start)
        body = text[start:end]
        body = replace(body, "return (int) (", "return tex::snowCheckedDimension(")
        text = text[:start] + body + text[end:]
    return '#include "snow_preview.h"\n' + text


edit("src/render.cpp", render_dimensions)
