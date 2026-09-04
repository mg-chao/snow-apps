#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLEPRESETS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLEPRESETS_H

#include "icon_core.h"
#include "screenshottoolpalettebuttons.h"

#include <QColor>
#include <QStringList>
#include <QVector>

namespace snow_shot::presentation::style_presets {

// Stroke color palette shared by the shape, line, free-draw and arrow stroke
// editors. The first entry tracks the compiled-in creation default.
[[nodiscard]] const QVector<QColor>& strokeColors();

// Fill color palette shared by the shape fill editor (no transparent entry).
[[nodiscard]] const QVector<QColor>& shapeFillColors();

// Color palette shared by the text, serial number, watermark, highlight,
// pen-highlight and spotlight editors.
[[nodiscard]] const QVector<QColor>& textColors();

// Fill color palette shared by the text and serial number fill editors. The
// first entry is fully transparent.
[[nodiscard]] const QVector<QColor>& textFillColors();

// Stroke width presets for the shape/line/free-draw editors.
[[nodiscard]] const QVector<double>& shapeStrokeWidths();

// Stroke width presets shared by the arrow stroke editor and the text stroke
// editor.
[[nodiscard]] const QVector<double>& strokePresetWidths();

// S/M/L/XL size preset values shared by the pen-highlight and pen-filter
// width editors. They intentionally coincide with the font size presets.
[[nodiscard]] const QVector<double>& sizePresetValues();

// S/M/L/XL size preset labels shared by every editor that renders
// sizePresetValues().
[[nodiscard]] const QStringList& sizePresetLabels();

// S/M/L/XL size preset icon shared by every editor that renders
// sizePresetValues() or the font size presets.
[[nodiscard]] adqt::icons::IconRef sizePresetIcon(int index);

// Font size presets shared by the text and serial number font editors.
[[nodiscard]] const QVector<double>& fontSizes();

// Font size presets for the watermark font editor.
[[nodiscard]] const QVector<double>& watermarkFontSizes();

// Shared tooltip pattern for an S/M/L/XL preset: "<pattern> S (30px)".
[[nodiscard]] ScreenshotToolPaletteTranslationText sizePresetTooltip(const char* pattern, int index,
                                                                     double value);

} // namespace snow_shot::presentation::style_presets

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTESTYLEPRESETS_H
