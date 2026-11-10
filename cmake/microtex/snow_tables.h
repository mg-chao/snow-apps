// Primitive table support for Snow Shot's pinned MicroTeX integration.
// The original MicroTeX sources and font resources retain their upstream licenses.
#ifndef SNOW_MICROTEX_TABLES_H
#define SNOW_MICROTEX_TABLES_H

#include "utils/enums.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER)
#define SNOW_MICROTEX_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define SNOW_MICROTEX_NOINLINE __attribute__((noinline))
#else
#define SNOW_MICROTEX_NOINLINE
#endif

namespace tex {
class Atom;
class SymbolAtom;
class MacroInfo;
class TeXParser;
struct CharFont;

namespace snow_tables {

template <typename Key, typename Value> struct Pair {
    Key key;
    Value value;
};

// Runtime count avoids a separate instantiation/unrolled constructor for every
// array length. All retained string/node construction lives in this one loop.
template <typename Map, typename Row>
SNOW_MICROTEX_NOINLINE Map makeMap(const Row* rows, std::size_t count) {
    static_assert(std::is_trivially_copyable_v<Row> && std::is_standard_layout_v<Row>);
    Map result;
    for (std::size_t index = 0; index < count; ++index) {
        result.emplace(rows[index].key, rows[index].value);
    }
    return result;
}

template <typename Map, typename Row>
SNOW_MICROTEX_NOINLINE Map makeIndirectMap(const Row* rows, std::size_t count) {
    static_assert(std::is_trivially_copyable_v<Row> && std::is_standard_layout_v<Row>);
    Map result;
    for (std::size_t index = 0; index < count; ++index) {
        result.emplace(rows[index].key, *rows[index].value);
    }
    return result;
}

template <typename Set, typename Row>
SNOW_MICROTEX_NOINLINE Set makeSet(const Row* rows, std::size_t count) {
    static_assert(std::is_trivially_copyable_v<Row>);
    Set result;
    for (std::size_t index = 0; index < count; ++index) {
        result.emplace(rows[index]);
    }
    return result;
}

struct SymbolRow {
    const char* name;
    AtomType type;
    bool delimiter;
};

using CommandDelegate = std::shared_ptr<Atom> (*)(TeXParser&, std::vector<std::wstring>&);
struct CommandRow {
    const wchar_t* name;
    int arguments;
    int optionalPosition;
    CommandDelegate delegate;
};

struct ColorRow {
    const char* name;
    std::uint32_t direct;
    float cyan;
    float magenta;
    float yellow;
    float black;
    bool isCmyk;
};

struct SettingRow {
    const char* name;
    float value;
    const char* fontName;
};

struct CharacterRow {
    int code;
    const char* fontName;
};

struct TextStyleRow {
    const char* name;
    CharacterRow characters[4];
};

struct GlyphRow {
    int font;
    int code;
    const char* name;
};

struct MacroTemplateRow {
    int arguments;
    const wchar_t* name;
    const wchar_t* code;
    // A null end marks a command; an empty literal remains a valid environment.
    const wchar_t* end;
};

using Symbols = std::map<std::string, std::shared_ptr<SymbolAtom>>;
using Commands = std::map<std::wstring, MacroInfo*>;
using Colors = std::map<std::string, std::uint32_t>;
using Settings = std::map<std::string, float>;
using TextStyles = std::map<std::string, std::vector<CharFont*>>;

SNOW_MICROTEX_NOINLINE Symbols makeSymbols(const SymbolRow* rows, std::size_t count);
SNOW_MICROTEX_NOINLINE Commands makeCommands(const CommandRow* rows, std::size_t count);
SNOW_MICROTEX_NOINLINE Colors makeColors(const ColorRow* rows, std::size_t count);
SNOW_MICROTEX_NOINLINE Settings makeSettings(const SettingRow* rows, std::size_t count);
SNOW_MICROTEX_NOINLINE TextStyles makeTextStyles(const TextStyleRow* rows, std::size_t count);
SNOW_MICROTEX_NOINLINE void initializeMacroTemplates(const MacroTemplateRow* rows,
                                                     std::size_t count);

static_assert(std::is_trivially_copyable_v<SymbolRow> && std::is_standard_layout_v<SymbolRow>);
static_assert(std::is_trivially_copyable_v<CommandRow> && std::is_standard_layout_v<CommandRow>);
static_assert(std::is_trivially_copyable_v<ColorRow> && std::is_standard_layout_v<ColorRow>);
static_assert(std::is_trivially_copyable_v<SettingRow> && std::is_standard_layout_v<SettingRow>);
static_assert(std::is_trivially_copyable_v<TextStyleRow> &&
              std::is_standard_layout_v<TextStyleRow>);
static_assert(std::is_trivially_copyable_v<GlyphRow> && std::is_standard_layout_v<GlyphRow>);
static_assert(std::is_trivially_copyable_v<MacroTemplateRow> &&
              std::is_standard_layout_v<MacroTemplateRow>);

} // namespace snow_tables
} // namespace tex

#endif
