#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSHORTCUTHINTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSHORTCUTHINTS_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/shortcuts/shortcutdisplayservice.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <optional>
#include <utility>

enum class ScreenshotShortcutHintMode {
    Hidden,
    SmartSelection,
    Selection,
    Tool,
    Scrolling,
};

enum class ScreenshotShortcutHintInput {
    Keyboard,
    Mouse,
};

struct ScreenshotShortcutHintRow {
    QString label;
    QString shortcut;
    ScreenshotShortcutHintInput input = ScreenshotShortcutHintInput::Keyboard;
    QStringList shortcutChips;
};

// These strings are translated through runtime-selected source text below, so
// they must be declared explicitly for lupdate.
[[maybe_unused]] inline constexpr const char* kScreenshotShortcutHintTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Vertical scroll"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Horizontal scroll"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Switch element level"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Switch color format"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Switch screenshot history"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Maintain aspect ratio"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Fixed-angle rotation"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Scale from center"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Auto-align"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Delete selected elements"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "Draw straight line"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "mouse wheel"),
    QT_TRANSLATE_NOOP("ScreenshotShortcutHintsWidget", "%1 + %2"),
};

[[nodiscard]] inline bool operator==(const ScreenshotShortcutHintRow& left,
                                     const ScreenshotShortcutHintRow& right) {
    return left.label == right.label && left.shortcut == right.shortcut &&
           left.input == right.input && left.shortcutChips == right.shortcutChips;
}

[[nodiscard]] inline bool operator!=(const ScreenshotShortcutHintRow& left,
                                     const ScreenshotShortcutHintRow& right) {
    return !(left == right);
}

// The hint list is a property of the active canvas workflow, rather than of the
// overlay widget itself. Keeping this context value-type makes the visibility
// matrix easy to exercise without constructing the screenshot UI.
struct ScreenshotShortcutHintContext {
    ScreenshotActiveTool activeTool = ScreenshotActiveTool::Move;
    ScreenshotCaptureMode captureMode = ScreenshotCaptureMode::Inactive;
    QSet<SnowCanvasTool> quickSelectionDisabledTools;
    std::optional<snow_shot::shortcuts::ShortcutBindingMap> configuredShortcuts = std::nullopt;
    bool smartSelectionEnabled = true;
};

[[nodiscard]] inline bool screenshotShortcutHintAreaIsObscured(const QRectF& hintArea,
                                                               const QRectF& selectionArea,
                                                               const QPointF& cursorPosition) {
    if (!hintArea.isValid() || hintArea.isEmpty()) {
        return false;
    }
    const bool selectionOverlaps =
        selectionArea.isValid() && !selectionArea.isEmpty() && hintArea.intersects(selectionArea);
    return selectionOverlaps || hintArea.contains(cursorPosition);
}

[[nodiscard]] inline ScreenshotShortcutHintMode
screenshotShortcutHintSelectionModeForContext(const ScreenshotShortcutHintContext& context) {
    if (context.captureMode == ScreenshotCaptureMode::IntelligentSelecting) {
        return ScreenshotShortcutHintMode::SmartSelection;
    }
    if (context.captureMode == ScreenshotCaptureMode::ManualSelecting ||
        (context.captureMode == ScreenshotCaptureMode::MovingSelection &&
         context.activeTool == ScreenshotActiveTool::Move)) {
        return ScreenshotShortcutHintMode::Selection;
    }
    return ScreenshotShortcutHintMode::Hidden;
}

[[nodiscard]] inline QString screenshotShortcutHintText(const char* source) {
    return QCoreApplication::translate("ScreenshotShortcutHintsWidget", source);
}

[[nodiscard]] inline qsizetype screenshotShortcutHintSeparatorIndex(const QString& text) {
    const qsizetype asciiSeparator = text.indexOf(QLatin1Char(':'));
    const qsizetype fullWidthSeparator = text.indexOf(QChar(0xFF1A));
    if (asciiSeparator < 0) {
        return fullWidthSeparator;
    }
    if (fullWidthSeparator < 0) {
        return asciiSeparator;
    }
    return std::min(asciiSeparator, fullWidthSeparator);
}

[[nodiscard]] inline ScreenshotShortcutHintRow screenshotFixedShortcutHintRow(
    const char* source, ScreenshotShortcutHintInput input = ScreenshotShortcutHintInput::Keyboard) {
    const QString sourceText = QString::fromLatin1(source);
    const auto& display = snow_shot::shortcuts::ShortcutDisplayService::instance();
    const auto row = [input](const char* label, const QString& shortcut) {
        return ScreenshotShortcutHintRow{screenshotShortcutHintText(label), shortcut, input, {}};
    };
    if (sourceText == QStringLiteral("Vertical scroll: mouse wheel")) {
        return row("Vertical scroll", screenshotShortcutHintText("mouse wheel"));
    }
    if (sourceText == QStringLiteral("Horizontal scroll: Shift + mouse wheel")) {
        return row("Horizontal scroll", screenshotShortcutHintText("%1 + %2").arg(
                                            display.modifierText(Qt::ShiftModifier),
                                            screenshotShortcutHintText("mouse wheel")));
    }
    if (sourceText == QStringLiteral("Switch element level: mouse wheel")) {
        return row("Switch element level", screenshotShortcutHintText("mouse wheel"));
    }
    if (sourceText == QStringLiteral("Switch color format: Shift")) {
        return row("Switch color format", display.modifierText(Qt::ShiftModifier));
    }
    if (sourceText == QStringLiteral("Maintain aspect ratio: Shift")) {
        return row("Maintain aspect ratio", display.modifierText(Qt::ShiftModifier));
    }
    if (sourceText == QStringLiteral("Fixed-angle rotation: Shift")) {
        return row("Fixed-angle rotation", display.modifierText(Qt::ShiftModifier));
    }
    if (sourceText == QStringLiteral("Scale from center: Alt")) {
        return row("Scale from center", display.modifierText(Qt::AltModifier));
    }
    if (sourceText == QStringLiteral("Auto-align: Ctrl")) {
        return row("Auto-align", display.modifierText(Qt::ControlModifier));
    }
    if (sourceText == QStringLiteral("Delete selected elements: Delete")) {
        return row("Delete selected elements",
                   snow_shot::shortcuts::formatShortcutDisplayText(
                       snow_shot::shortcuts::bindingFromPortableText(QStringLiteral("Delete"))));
    }
    if (sourceText == QStringLiteral("Draw straight line: Shift")) {
        return row("Draw straight line", display.modifierText(Qt::ShiftModifier));
    }
    const QString text = screenshotShortcutHintText(source);
    const qsizetype separator = screenshotShortcutHintSeparatorIndex(text);
    if (separator < 0) {
        return {text, {}, input, {}};
    }
    return {text.left(separator), text.mid(separator + 1).trimmed(), input, {}};
}

[[nodiscard]] inline snow_shot::shortcuts::ShortcutBindingList
screenshotShortcutSchemaDefaultKeys(const QString& actionId) {
    const QJsonValue defaultValue = snow_shot::storage::ConfigurationSchema::defaultValue(
        QStringLiteral("screenshot_shortcuts/") + actionId);
    return snow_shot::shortcuts::shortcutBindingsFromJson(defaultValue, true, 2);
}

[[nodiscard]] inline snow_shot::shortcuts::ShortcutBindingList screenshotConfiguredShortcutHintKeys(
    const std::optional<snow_shot::shortcuts::ShortcutBindingMap>& configuredShortcuts,
    const QString& actionId) {
    if (configuredShortcuts.has_value()) {
        const auto configured = configuredShortcuts->find(actionId);
        if (configured != configuredShortcuts->cend()) {
            return *configured;
        }
    }
    return screenshotShortcutSchemaDefaultKeys(actionId);
}

[[nodiscard]] inline ScreenshotShortcutHintRow screenshotConfiguredShortcutHintRow(
    const std::optional<snow_shot::shortcuts::ShortcutBindingMap>& configuredShortcuts,
    const QString& actionId, const char* label) {
    return {
        QCoreApplication::translate("SettingsCatalog", label),
        snow_shot::shortcuts::formatShortcutListDisplayText(
            screenshotConfiguredShortcutHintKeys(configuredShortcuts, actionId)),
        ScreenshotShortcutHintInput::Keyboard,
        {},
    };
}

inline void appendScreenshotConfiguredShortcutHintRow(
    QVector<ScreenshotShortcutHintRow>& rows,
    const std::optional<snow_shot::shortcuts::ShortcutBindingMap>& configuredShortcuts,
    const QString& actionId, const char* label) {
    ScreenshotShortcutHintRow row =
        screenshotConfiguredShortcutHintRow(configuredShortcuts, actionId, label);
    if (!row.shortcut.isEmpty()) {
        rows.push_back(std::move(row));
    }
}

inline void appendScreenshotCursorMovementShortcutHintRows(
    QVector<ScreenshotShortcutHintRow>& rows,
    const std::optional<snow_shot::shortcuts::ShortcutBindingMap>& configuredShortcuts) {
    appendScreenshotConfiguredShortcutHintRow(rows, configuredShortcuts,
                                              QStringLiteral("move_cursor_up"), "Move cursor up");
    appendScreenshotConfiguredShortcutHintRow(
        rows, configuredShortcuts, QStringLiteral("move_cursor_down"), "Move cursor down");
    appendScreenshotConfiguredShortcutHintRow(
        rows, configuredShortcuts, QStringLiteral("move_cursor_left"), "Move cursor left");
    appendScreenshotConfiguredShortcutHintRow(
        rows, configuredShortcuts, QStringLiteral("move_cursor_right"), "Move cursor right");
}

[[nodiscard]] inline QVector<ScreenshotShortcutHintRow> screenshotShortcutHintRows(
    ScreenshotShortcutHintMode mode,
    const std::optional<snow_shot::shortcuts::ShortcutBindingMap>& configuredShortcuts =
        std::nullopt,
    bool smartSelectionEnabled = true) {
    if (mode == ScreenshotShortcutHintMode::Hidden || mode == ScreenshotShortcutHintMode::Tool) {
        return {};
    }
    if (mode == ScreenshotShortcutHintMode::Scrolling) {
        return {
            screenshotFixedShortcutHintRow("Vertical scroll: mouse wheel",
                                           ScreenshotShortcutHintInput::Mouse),
            screenshotFixedShortcutHintRow("Horizontal scroll: Shift + mouse wheel",
                                           ScreenshotShortcutHintInput::Mouse),
        };
    }

    QVector<ScreenshotShortcutHintRow> rows;
    appendScreenshotCursorMovementShortcutHintRows(rows, configuredShortcuts);
    if (mode == ScreenshotShortcutHintMode::SmartSelection) {
        rows.push_back(screenshotFixedShortcutHintRow("Switch element level: mouse wheel",
                                                      ScreenshotShortcutHintInput::Mouse));
        if (smartSelectionEnabled) {
            appendScreenshotConfiguredShortcutHintRow(
                rows, configuredShortcuts,
                QStringLiteral("switch_selection_between_window_and_window_sub_element"),
                "Select window/window sub-element");
        }
    } else {
        appendScreenshotConfiguredShortcutHintRow(rows, configuredShortcuts,
                                                  QStringLiteral("move_entire_selection"),
                                                  "Move entire selection");
        appendScreenshotConfiguredShortcutHintRow(
            rows, configuredShortcuts, QStringLiteral("keep_selection_width_and_height_consistent"),
            "Keep selection width and height consistent");
    }
    appendScreenshotConfiguredShortcutHintRow(rows, configuredShortcuts,
                                              QStringLiteral("select_previously_selected_area"),
                                              "Select previously selected area");
    appendScreenshotConfiguredShortcutHintRow(rows, configuredShortcuts,
                                              QStringLiteral("copy_color"), "Copy color");
    rows.push_back(screenshotFixedShortcutHintRow("Switch color format: Shift"));

    const snow_shot::shortcuts::ShortcutBindingList previousHistoryShortcuts =
        screenshotConfiguredShortcutHintKeys(configuredShortcuts,
                                             QStringLiteral("previous_screenshot_history"));
    const snow_shot::shortcuts::ShortcutBindingList nextHistoryShortcuts =
        screenshotConfiguredShortcutHintKeys(configuredShortcuts,
                                             QStringLiteral("next_screenshot_history"));
    snow_shot::shortcuts::ShortcutBindingList historyShortcuts = previousHistoryShortcuts;
    historyShortcuts.append(nextHistoryShortcuts);
    if (!historyShortcuts.isEmpty()) {
        QStringList historyShortcutChips;
        if (!previousHistoryShortcuts.isEmpty()) {
            historyShortcutChips.push_back(
                snow_shot::shortcuts::formatShortcutListDisplayText(previousHistoryShortcuts));
        }
        if (!nextHistoryShortcuts.isEmpty()) {
            historyShortcutChips.push_back(
                snow_shot::shortcuts::formatShortcutListDisplayText(nextHistoryShortcuts));
        }
        rows.push_back({screenshotShortcutHintText("Switch screenshot history"),
                        snow_shot::shortcuts::formatShortcutListDisplayText(historyShortcuts),
                        ScreenshotShortcutHintInput::Keyboard, std::move(historyShortcutChips)});
    }
    return rows;
}

[[nodiscard]] inline bool
screenshotShortcutHintToolIsQuickSelectionDisabled(const ScreenshotShortcutHintContext& context,
                                                   SnowCanvasTool tool) {
    return context.quickSelectionDisabledTools.contains(tool);
}

[[nodiscard]] inline QStringList
screenshotShortcutHintLines(const QVector<ScreenshotShortcutHintRow>& rows) {
    QStringList lines;
    lines.reserve(rows.size());
    for (const ScreenshotShortcutHintRow& row : rows) {
        lines.push_back(QStringLiteral("%1: %2").arg(row.label, row.shortcut));
    }
    return lines;
}

[[nodiscard]] inline QStringList screenshotShortcutHintLines(ScreenshotShortcutHintMode mode) {
    return screenshotShortcutHintLines(screenshotShortcutHintRows(mode));
}

[[nodiscard]] inline QVector<ScreenshotShortcutHintRow>
screenshotShortcutHintRows(const ScreenshotShortcutHintContext& context) {
    if (context.captureMode == ScreenshotCaptureMode::ScrollingCapture) {
        return screenshotShortcutHintRows(ScreenshotShortcutHintMode::Scrolling,
                                          context.configuredShortcuts,
                                          context.smartSelectionEnabled);
    }

    // Selection-stage hints are independent of the currently selected canvas
    // tool. Keep these stages mapped to the dedicated selection modes so intelligent and
    // manual selection retain their context-specific shortcuts.
    const ScreenshotShortcutHintMode selectionMode =
        screenshotShortcutHintSelectionModeForContext(context);
    if (selectionMode != ScreenshotShortcutHintMode::Hidden) {
        return screenshotShortcutHintRows(selectionMode, context.configuredShortcuts,
                                          context.smartSelectionEnabled);
    }

    // Hints are intentionally limited to the canvas editing tools after the
    // selection stages above. Recognition workflows have their own transient
    // UI and should not leave stale drawing instructions over the capture.
    if (context.captureMode != ScreenshotCaptureMode::Editing) {
        return {};
    }

    const auto disabled = [&context](SnowCanvasTool tool) {
        return screenshotShortcutHintToolIsQuickSelectionDisabled(context, tool);
    };
    const auto append = [](QVector<ScreenshotShortcutHintRow>& rows, const char* source,
                           bool enabled = true) {
        if (enabled) {
            rows.push_back(screenshotFixedShortcutHintRow(source));
        }
    };

    QVector<ScreenshotShortcutHintRow> rows;
    if (context.activeTool != ScreenshotActiveTool::Eraser &&
        context.activeTool != ScreenshotActiveTool::Ocr &&
        context.activeTool != ScreenshotActiveTool::Table &&
        context.activeTool != ScreenshotActiveTool::Qr &&
        context.activeTool != ScreenshotActiveTool::Markdown &&
        context.activeTool != ScreenshotActiveTool::Html &&
        context.activeTool != ScreenshotActiveTool::Move &&
        context.activeTool != ScreenshotActiveTool::Spotlight &&
        context.activeTool != ScreenshotActiveTool::Watermark &&
        context.activeTool != ScreenshotActiveTool::AutoFilter) {
        appendScreenshotCursorMovementShortcutHintRows(rows, context.configuredShortcuts);
    }
    switch (context.activeTool) {
    case ScreenshotActiveTool::Select:
        append(rows, "Maintain aspect ratio: Shift");
        append(rows, "Fixed-angle rotation: Shift");
        append(rows, "Scale from center: Alt");
        append(rows, "Auto-align: Ctrl");
        append(rows, "Delete selected elements: Delete");
        break;
    case ScreenshotActiveTool::Shape:
        append(rows, "Maintain aspect ratio: Shift");
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::Shape));
        append(rows, "Scale from center: Alt");
        append(rows, "Auto-align: Ctrl");
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::Shape));
        break;
    case ScreenshotActiveTool::Arrow:
        append(rows, "Maintain aspect ratio: Shift", !disabled(SnowCanvasTool::Arrow));
        append(rows, "Fixed-angle rotation: Shift");
        append(rows, "Scale from center: Alt", !disabled(SnowCanvasTool::Arrow));
        append(rows, "Auto-align: Ctrl");
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::Arrow));
        break;
    case ScreenshotActiveTool::Line:
        append(rows, "Maintain aspect ratio: Shift", !disabled(SnowCanvasTool::Line));
        append(rows, "Fixed-angle rotation: Shift");
        append(rows, "Scale from center: Alt", !disabled(SnowCanvasTool::Line));
        append(rows, "Auto-align: Ctrl");
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::Line));
        break;
    case ScreenshotActiveTool::FreeDraw:
        append(rows, "Draw straight line: Shift");
        append(rows, "Maintain aspect ratio: Shift", !disabled(SnowCanvasTool::FreeDraw));
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::FreeDraw));
        append(rows, "Scale from center: Alt", !disabled(SnowCanvasTool::FreeDraw));
        append(rows, "Auto-align: Ctrl", !disabled(SnowCanvasTool::FreeDraw));
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::FreeDraw));
        break;
    case ScreenshotActiveTool::RectangleHighlight:
        append(rows, "Maintain aspect ratio: Shift");
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::RectangleHighlight));
        append(rows, "Scale from center: Alt");
        append(rows, "Auto-align: Ctrl");
        append(rows, "Delete selected elements: Delete",
               !disabled(SnowCanvasTool::RectangleHighlight));
        break;
    case ScreenshotActiveTool::PenHighlight:
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::PenHighlight));
        break;
    case ScreenshotActiveTool::Text:
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::Text));
        append(rows, "Scale from center: Alt", !disabled(SnowCanvasTool::Text));
        append(rows, "Auto-align: Ctrl", !disabled(SnowCanvasTool::Text));
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::Text));
        break;
    case ScreenshotActiveTool::SerialNumber:
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::SerialNumber));
        append(rows, "Scale from center: Alt", !disabled(SnowCanvasTool::SerialNumber));
        append(rows, "Auto-align: Ctrl", !disabled(SnowCanvasTool::SerialNumber));
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::SerialNumber));
        break;
    case ScreenshotActiveTool::PenFilter:
        append(rows, "Draw straight line: Shift");
        append(rows, "Maintain aspect ratio: Shift", !disabled(SnowCanvasTool::PenFilter));
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::PenFilter));
        append(rows, "Scale from center: Alt", !disabled(SnowCanvasTool::PenFilter));
        append(rows, "Auto-align: Ctrl", !disabled(SnowCanvasTool::PenFilter));
        append(rows, "Delete selected elements: Delete", !disabled(SnowCanvasTool::PenFilter));
        break;
    case ScreenshotActiveTool::RectangleFilter:
        append(rows, "Maintain aspect ratio: Shift");
        append(rows, "Fixed-angle rotation: Shift", !disabled(SnowCanvasTool::RectangleFilter));
        append(rows, "Scale from center: Alt");
        append(rows, "Auto-align: Ctrl");
        append(rows, "Delete selected elements: Delete",
               !disabled(SnowCanvasTool::RectangleFilter));
        break;
    case ScreenshotActiveTool::Eraser:
    case ScreenshotActiveTool::Ocr:
    case ScreenshotActiveTool::Table:
    case ScreenshotActiveTool::Qr:
    case ScreenshotActiveTool::Markdown:
    case ScreenshotActiveTool::Html:
    case ScreenshotActiveTool::Move:
    case ScreenshotActiveTool::Spotlight:
    case ScreenshotActiveTool::Watermark:
    case ScreenshotActiveTool::AutoFilter:
        break;
    }
    return rows;
}

[[nodiscard]] inline QStringList
screenshotShortcutHintLines(const ScreenshotShortcutHintContext& context) {
    return screenshotShortcutHintLines(screenshotShortcutHintRows(context));
}

[[nodiscard]] inline ScreenshotShortcutHintMode
screenshotShortcutHintModeForContext(const ScreenshotShortcutHintContext& context) {
    if (context.captureMode == ScreenshotCaptureMode::ScrollingCapture) {
        return ScreenshotShortcutHintMode::Scrolling;
    }

    const ScreenshotShortcutHintMode selectionMode =
        screenshotShortcutHintSelectionModeForContext(context);
    if (selectionMode != ScreenshotShortcutHintMode::Hidden) {
        return selectionMode;
    }

    return screenshotShortcutHintRows(context).isEmpty() ? ScreenshotShortcutHintMode::Hidden
                                                         : ScreenshotShortcutHintMode::Tool;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSHORTCUTHINTS_H
