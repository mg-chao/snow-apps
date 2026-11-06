#include "snow_tables.h"

#include "atom/atom_char.h"
#include "core/macro.h"
#include "fonts/fonts.h"
#include "graphic/graphic_basic.h"

namespace tex::snow_tables {

Symbols makeSymbols(const SymbolRow* rows, std::size_t count) {
    Symbols result;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& row = rows[index];
        auto [entry, inserted] = result.try_emplace(row.name);
        if (inserted) {
            entry->second = std::make_shared<SymbolAtom>(row.name, row.type, row.delimiter);
        }
    }
    return result;
}

Commands makeCommands(const CommandRow* rows, std::size_t count) {
    Commands result;
    try {
        for (std::size_t index = 0; index < count; ++index) {
            const auto& row = rows[index];
            auto command =
                std::make_unique<PreDefMacro>(row.arguments, row.optionalPosition, row.delegate);
            const auto inserted = result.emplace(row.name, command.get()).second;
            if (inserted) {
                command.release();
            }
        }
    } catch (...) {
        for (const auto& entry : result) {
            delete entry.second;
        }
        throw;
    }
    return result;
}

Colors makeColors(const ColorRow* rows, std::size_t count) {
    Colors result;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& row = rows[index];
        const auto value =
            row.isCmyk ? cmyk(row.cyan, row.magenta, row.yellow, row.black) : row.direct;
        result.emplace(row.name, value);
    }
    return result;
}

Settings makeSettings(const SettingRow* rows, std::size_t count) {
    Settings result;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& row = rows[index];
        const auto value =
            row.fontName ? static_cast<float>(FontInfo::__id(row.fontName)) : row.value;
        result.emplace(row.name, value);
    }
    return result;
}

TextStyles makeTextStyles(const TextStyleRow* rows, std::size_t count) {
    TextStyles result;
    try {
        for (std::size_t index = 0; index < count; ++index) {
            const auto& row = rows[index];
            auto [entry, inserted] = result.emplace(row.name, std::vector<CharFont*>(4, nullptr));
            if (!inserted) {
                continue;
            }
            for (std::size_t slot = 0; slot < 4; ++slot) {
                const auto& character = row.characters[slot];
                if (character.fontName) {
                    entry->second[slot] = new CharFont(static_cast<wchar_t>(character.code),
                                                       FontInfo::__id(character.fontName));
                }
            }
        }
    } catch (...) {
        for (const auto& entry : result) {
            for (const auto* character : entry.second) {
                delete character;
            }
        }
        throw;
    }
    return result;
}

void initializeMacroTemplates(const MacroTemplateRow* rows, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        const auto& row = rows[index];
        if (row.end) {
            NewEnvironmentMacro::addNewEnvironment(row.name, row.code, row.end, row.arguments);
        } else {
            NewCommandMacro::addNewCommand(row.name, row.code, row.arguments);
        }
    }
}

} // namespace tex::snow_tables
