#include "snow_preview.h"

#include "atom/atom_basic.h"
#include "atom/atom_impl.h"
#include "atom/atom_matrix.h"
#include "atom/atom_row.h"
#include "box/box_single.h"
#include "core/formula.h"
#include "core/macro.h"
#include "core/parser.h"
#include "fonts/fonts.h"
#include "render.h"

#include <cmath>

namespace tex {

thread_local SnowPreviewBudget* SnowPreviewBudget::current = nullptr;

SnowPreviewBudget::SnowPreviewBudget(const std::atomic_bool& canceledValue)
    : canceled(canceledValue),
      deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(500)) {
    current = this;
}

SnowPreviewBudget::~SnowPreviewBudget() {
    current = nullptr;
}

void SnowPreviewBudget::check() {
    if (current == nullptr)
        return;
    if (current->canceled.load(std::memory_order_relaxed))
        throw SnowPreviewCanceled();
    if (++current->operations > 200000)
        throw SnowPreviewLimit();
    if ((current->operations & 127U) == 0 && std::chrono::steady_clock::now() > current->deadline)
        throw SnowPreviewLimit();
}

void SnowPreviewBudget::checkpoint() {
    check();
    if (current != nullptr && std::chrono::steady_clock::now() > current->deadline)
        throw SnowPreviewLimit();
}

void SnowPreviewBudget::length(std::size_t value) {
    check();
    if (current != nullptr && value > 1000000)
        throw SnowPreviewLimit();
}

void SnowPreviewBudget::replacement(std::size_t size, std::size_t removed, std::size_t inserted) {
    if (removed > size)
        throw SnowPreviewLimit();
    if (inserted > 1000000 || size - removed > 1000000 - inserted)
        throw SnowPreviewLimit();
    length(size - removed + inserted);
}

void SnowPreviewBudget::matrix(int rows, int columns) {
    check();
    // Bound layout storage before ragged input is padded or temporary box arrays allocate.
    constexpr int maximumCells = 200000;
    if (rows < 0 || columns < 0 || rows > maximumCells || columns > maximumCells ||
        (columns != 0 && rows > maximumCells / columns))
        throw SnowPreviewLimit();
}

SnowPreviewBudget::Depth::Depth() {
    check();
    if (current != nullptr && current->depth >= 128)
        throw SnowPreviewLimit();
    if (current != nullptr)
        ++current->depth;
}

SnowPreviewBudget::Depth::~Depth() {
    if (current != nullptr)
        --current->depth;
}

int snowCheckedDimension(double value) {
    SnowPreviewBudget::check();
    if (!std::isfinite(value) || value < 0 || value > 32768)
        throw SnowPreviewLimit();
    return static_cast<int>(std::ceil(value));
}

void SnowSessionState::reset() {
    // Capture defaults before any formula can redefine colors or sizing. Font metrics and
    // alphabet registrations deliberately remain cached; formula-local mutations do not.
    static const auto defaultColors = ColorAtom::_colors;
    static const auto defaultSettings = DefaultTeXFont::_generalSettings;
    NewCommandMacro::_codes.clear();
    NewCommandMacro::_replacements.clear();
    snowRebuildCommands();
    NewCommandMacro::_errIfConflict = true;
    NewCommandMacro::_init_();
    ColorAtom::_colors = defaultColors;
    MatrixAtom::_colspeReplacement.clear();
    MatrixAtom::LINE_COLOR = transparent;
    RowAtom::_breakEveywhere = false;
    OvalAtom::_multiplier = 0.5f;
    OvalAtom::_diameter = 0;
    TeXRender::_defaultSize = -1;
    TeXRender::_magFactor = 0;
    DefaultTeXFont::_generalSettings = defaultSettings;
    DefaultTeXFont::_magnificationEnable = true;
    TextRenderingBox::_init_();
    Formula::PIXELS_PER_POINT = 1;
    Formula::_predefinedTeXFormulas.clear();
    for (const auto& entry : Formula::_externalFontMap)
        delete entry.second;
    Formula::_externalFontMap.clear();
    TeXParser::_isLoading = false;
}

} // namespace tex
