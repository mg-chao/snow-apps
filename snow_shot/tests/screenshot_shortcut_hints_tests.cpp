#include "snow_shot/presentation/screenshotshortcuthints.h"

#include <QCoreApplication>
#include <QKeySequence>
#include <QStringList>

#include <initializer_list>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        qFatal("%s", message);
    }
}

snow_shot::shortcuts::ShortcutBindingList
shortcutBindings(std::initializer_list<QString> portableText) {
    return snow_shot::shortcuts::bindingsFromPortableText(QStringList(portableText), true);
}

QString shortcutDisplay(std::initializer_list<QString> portableText) {
    return snow_shot::presentation::formatShortcutListDisplayText(shortcutBindings(portableText));
}

QString shortcutLine(const QString& label, std::initializer_list<QString> portableText) {
    return QStringLiteral("%1: %2").arg(label, shortcutDisplay(portableText));
}

QString modifierLine(const QString& label, Qt::KeyboardModifier modifier) {
    // Modifier-only rows render through the display service's modifier
    // labels; a bare "Alt"/"Ctrl" portable text does not parse as a key.
    return QStringLiteral("%1: %2").arg(
        label, snow_shot::shortcuts::ShortcutDisplayService::instance().modifierText(modifier));
}

QStringList hintLines(ScreenshotActiveTool tool,
                      std::initializer_list<SnowCanvasTool> disabled = {}) {
    ScreenshotShortcutHintContext context;
    context.activeTool = tool;
    context.captureMode = ScreenshotCaptureMode::Editing;
    for (const SnowCanvasTool disabledTool : disabled) {
        context.quickSelectionDisabledTools.insert(disabledTool);
    }
    return screenshotShortcutHintLines(context);
}

QStringList withDefaultCursorHints(std::initializer_list<QString> remaining) {
    QStringList lines{
        shortcutLine(QStringLiteral("Move cursor up"), {QStringLiteral("W"), QStringLiteral("Up")}),
        shortcutLine(QStringLiteral("Move cursor down"),
                     {QStringLiteral("S"), QStringLiteral("Down")}),
        shortcutLine(QStringLiteral("Move cursor left"),
                     {QStringLiteral("A"), QStringLiteral("Left")}),
        shortcutLine(QStringLiteral("Move cursor right"),
                     {QStringLiteral("D"), QStringLiteral("Right")}),
    };
    for (const QString& line : remaining) {
        lines.push_back(line);
    }
    return lines;
}

void enterHintsUseApplicationLabels() {
    for (const auto* alias : {"Return", "Enter", "Num+Return", "Num+Enter"}) {
        const snow_shot::shortcuts::ShortcutBindingMap configured{
            {QStringLiteral("next_selection_type"), {QString::fromLatin1(alias)}}};
        const auto row = screenshotConfiguredShortcutHintRow(
            configured, QStringLiteral("next_selection_type"), "Switch region type");
        require(row.shortcut == QStringLiteral("Enter"),
                "configured application hints must show both Enter keys uniformly");
    }
}

void toolMatrixMatchesRequestedVisibility() {
    require(hintLines(ScreenshotActiveTool::Move) ==
                QStringList{shortcutLine(QStringLiteral("Toggle cursor visibility"),
                                         {QStringLiteral("`")})},
            "Move must expose the cursor visibility shortcut");
    const QStringList transformHints = withDefaultCursorHints({
        shortcutLine(QStringLiteral("Maintain aspect ratio"), {QStringLiteral("Shift")}),
        shortcutLine(QStringLiteral("Fixed-angle rotation"), {QStringLiteral("Shift")}),
        modifierLine(QStringLiteral("Scale from center"), Qt::AltModifier),
        modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
        shortcutLine(QStringLiteral("Delete selected elements"), {QStringLiteral("Delete")}),
    });
    require(hintLines(ScreenshotActiveTool::Select) == transformHints,
            "selection tool hint matrix changed");
    require(hintLines(ScreenshotActiveTool::Shape) == transformHints,
            "shape tool hint matrix changed");
    require(hintLines(ScreenshotActiveTool::Arrow) == transformHints,
            "arrow tool hint matrix changed");
    require(hintLines(ScreenshotActiveTool::Line) == transformHints,
            "line tool hint matrix changed");
    require(hintLines(ScreenshotActiveTool::Angle) == transformHints,
            "angle hints should show only shared transform shortcuts");
    require(hintLines(ScreenshotActiveTool::RectangleHighlight) == transformHints,
            "rectangle-highlighter hint matrix changed");
    require(hintLines(ScreenshotActiveTool::RectangleFilter) == transformHints,
            "rectangle-filter hint matrix changed");

    QStringList penTransformHints = withDefaultCursorHints(
        {shortcutLine(QStringLiteral("Draw straight line"), {QStringLiteral("Shift")})});
    penTransformHints.append(transformHints.mid(4));
    require(hintLines(ScreenshotActiveTool::FreeDraw) == penTransformHints,
            "free-draw hint matrix changed");
    require(hintLines(ScreenshotActiveTool::PenFilter) == penTransformHints,
            "pen-filter hint matrix changed");
    require(hintLines(ScreenshotActiveTool::PenHighlight) ==
                withDefaultCursorHints({shortcutLine(QStringLiteral("Delete selected elements"),
                                                     {QStringLiteral("Delete")})}),
            "pen-highlighter hint matrix changed");

    const QStringList textTransformHints = withDefaultCursorHints({
        shortcutLine(QStringLiteral("Fixed-angle rotation"), {QStringLiteral("Shift")}),
        modifierLine(QStringLiteral("Scale from center"), Qt::AltModifier),
        modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
        shortcutLine(QStringLiteral("Delete selected elements"), {QStringLiteral("Delete")}),
    });
    require(hintLines(ScreenshotActiveTool::Text) == textTransformHints,
            "text hint matrix changed");
    require(hintLines(ScreenshotActiveTool::SerialNumber) == textTransformHints,
            "serial-number hint matrix changed");

    require(
        hintLines(ScreenshotActiveTool::Shape, {SnowCanvasTool::Shape}) ==
            withDefaultCursorHints({
                shortcutLine(QStringLiteral("Maintain aspect ratio"), {QStringLiteral("Shift")}),
                modifierLine(QStringLiteral("Scale from center"), Qt::AltModifier),
                modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
            }),
        "shape quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::Arrow, {SnowCanvasTool::Arrow}) ==
                withDefaultCursorHints({
                    shortcutLine(QStringLiteral("Fixed-angle rotation"), {QStringLiteral("Shift")}),
                    modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
                }),
            "arrow quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::Line, {SnowCanvasTool::Line}) ==
                withDefaultCursorHints({
                    shortcutLine(QStringLiteral("Fixed-angle rotation"), {QStringLiteral("Shift")}),
                    modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
                }),
            "line quick-selection suppression changed");
    require(
        hintLines(ScreenshotActiveTool::RectangleHighlight, {SnowCanvasTool::RectangleHighlight}) ==
            withDefaultCursorHints({
                shortcutLine(QStringLiteral("Maintain aspect ratio"), {QStringLiteral("Shift")}),
                modifierLine(QStringLiteral("Scale from center"), Qt::AltModifier),
                modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
            }),
        "rectangle-highlighter quick-selection suppression changed");
    require(
        hintLines(ScreenshotActiveTool::RectangleFilter, {SnowCanvasTool::RectangleFilter}) ==
            withDefaultCursorHints({
                shortcutLine(QStringLiteral("Maintain aspect ratio"), {QStringLiteral("Shift")}),
                modifierLine(QStringLiteral("Scale from center"), Qt::AltModifier),
                modifierLine(QStringLiteral("Auto-align"), Qt::ControlModifier),
            }),
        "rectangle-filter quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::FreeDraw, {SnowCanvasTool::FreeDraw}) ==
                withDefaultCursorHints({shortcutLine(QStringLiteral("Draw straight line"),
                                                     {QStringLiteral("Shift")})}),
            "free-draw quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::PenFilter, {SnowCanvasTool::PenFilter}) ==
                withDefaultCursorHints({shortcutLine(QStringLiteral("Draw straight line"),
                                                     {QStringLiteral("Shift")})}),
            "pen-filter quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::PenHighlight, {SnowCanvasTool::PenHighlight}) ==
                withDefaultCursorHints({}),
            "pen-highlighter delete hint should be suppressed");
    require(hintLines(ScreenshotActiveTool::Text, {SnowCanvasTool::Text}) ==
                withDefaultCursorHints({}),
            "text hints should be suppressed when quick selection is disabled");
    require(hintLines(ScreenshotActiveTool::SerialNumber, {SnowCanvasTool::SerialNumber}) ==
                withDefaultCursorHints({}),
            "serial-number hints should be suppressed when quick selection is disabled");

    require(
        hintLines(ScreenshotActiveTool::RectangleEraser) ==
                QStringList{
                    modifierLine(QStringLiteral("Maintain aspect ratio"), Qt::ShiftModifier),
                    QStringLiteral("Draw from center: Alt")} &&
            hintLines(ScreenshotActiveTool::BrushEraser) ==
                QStringList{modifierLine(QStringLiteral("Draw straight line"), Qt::ShiftModifier)},
        "immutable erasers show only their creation modifiers");
    require(hintLines(ScreenshotActiveTool::Eraser).isEmpty() &&
                hintLines(ScreenshotActiveTool::Ocr).isEmpty() &&
                hintLines(ScreenshotActiveTool::TextTranslation).isEmpty() &&
                hintLines(ScreenshotActiveTool::Table).isEmpty() &&
                hintLines(ScreenshotActiveTool::Qr).isEmpty() &&
                hintLines(ScreenshotActiveTool::Spotlight).isEmpty() &&
                hintLines(ScreenshotActiveTool::Watermark).isEmpty(),
            "tools without requested shortcuts must not expose hint rows");
}

void configuredShortcutRowsUseActualValues() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::Move;
    context.captureMode = ScreenshotCaptureMode::ManualSelecting;
    context.configuredShortcuts = snow_shot::shortcuts::ShortcutBindingMap{
        {QStringLiteral("move_cursor_up"),
         shortcutBindings({QStringLiteral("Ctrl+Alt+I"), QStringLiteral("Up")})},
        {QStringLiteral("move_cursor_down"), shortcutBindings({QStringLiteral("Ctrl+Alt+K")})},
        {QStringLiteral("move_cursor_left"), shortcutBindings({QStringLiteral("Ctrl+Alt+J")})},
        {QStringLiteral("move_cursor_right"), shortcutBindings({QStringLiteral("Ctrl+Alt+L")})},
        {QStringLiteral("move_entire_selection"), shortcutBindings({QStringLiteral("Ctrl+M")})},
        {QStringLiteral("keep_selection_width_and_height_consistent"),
         shortcutBindings({QStringLiteral("Alt+R")})},
        {QStringLiteral("selection_aspect_ratio_snap"),
         shortcutBindings({QStringLiteral("G"), QStringLiteral("Ctrl+Alt+Q")})},
        {QStringLiteral("select_previously_selected_area"),
         shortcutBindings({QStringLiteral("P")})},
        {QStringLiteral("select_next_previously_selected_area"),
         shortcutBindings({QStringLiteral("Shift+P")})},
        {QStringLiteral("print"), shortcutBindings({QStringLiteral("Ctrl+Alt+P")})},
        {QStringLiteral("copy_color"), shortcutBindings({QStringLiteral("Alt+C")})},
        {QStringLiteral("toggle_coordinate_mode"), shortcutBindings({QStringLiteral("Alt+P")})},
        {QStringLiteral("previous_screenshot_history"),
         shortcutBindings({QStringLiteral("PgUp"), QStringLiteral("[")})},
        {QStringLiteral("next_screenshot_history"),
         shortcutBindings({QStringLiteral("PgDown"), QStringLiteral("]")})},
    };

    const QVector<ScreenshotShortcutHintRow> rows = screenshotShortcutHintRows(context);
    require(rows.size() == 12, "manual-selection configured hint row count changed");
    require(rows.at(0).label == QStringLiteral("Move cursor up") &&
                rows.at(0).shortcut ==
                    shortcutDisplay({QStringLiteral("Ctrl+Alt+I"), QStringLiteral("Up")}) &&
                rows.at(1).label == QStringLiteral("Move cursor down") &&
                rows.at(1).shortcut == shortcutDisplay({QStringLiteral("Ctrl+Alt+K")}) &&
                rows.at(2).label == QStringLiteral("Move cursor left") &&
                rows.at(2).shortcut == shortcutDisplay({QStringLiteral("Ctrl+Alt+J")}) &&
                rows.at(3).label == QStringLiteral("Move cursor right") &&
                rows.at(3).shortcut == shortcutDisplay({QStringLiteral("Ctrl+Alt+L")}),
            "cursor directions must use four independent configured rows");
    require(rows.at(4).shortcut == shortcutDisplay({QStringLiteral("Ctrl+M")}) &&
                rows.at(5).shortcut == shortcutDisplay({QStringLiteral("Alt+R")}) &&
                rows.at(6).label == QStringLiteral("Selection aspect ratio snap") &&
                rows.at(6).shortcut ==
                    shortcutDisplay({QStringLiteral("G"), QStringLiteral("Ctrl+Alt+Q")}) &&
                rows.at(7).shortcut == shortcutDisplay({QStringLiteral("Alt+C")}),
            "selection action hints must use configured shortcuts");
    require(rows.at(8).label == QStringLiteral("Toggle global/relative coordinates") &&
                rows.at(8).shortcut == shortcutDisplay({QStringLiteral("Alt+P")}),
            "coordinate toggle must follow Copy color and show the configured binding");
    require(rows.at(9).label == QStringLiteral("Toggle cursor visibility") &&
                rows.at(9).shortcut == shortcutDisplay({QStringLiteral("`")}),
            "cursor visibility must show its configurable backtick default");
    require(rows.at(10).label == QStringLiteral("Switch color format") &&
                rows.at(10).shortcut == shortcutDisplay({QStringLiteral("Shift")}),
            "the fixed color-format shortcut must remain visible");
    require(rows.at(11).label == QStringLiteral("Switch screenshot history") &&
                rows.at(11).shortcut ==
                    shortcutDisplay({QStringLiteral("PgUp"), QStringLiteral("["),
                                     QStringLiteral("PgDown"), QStringLiteral("]")}) &&
                rows.at(11).shortcutChips ==
                    QStringList{shortcutDisplay({QStringLiteral("PgUp"), QStringLiteral("[")}),
                                shortcutDisplay({QStringLiteral("PgDown"), QStringLiteral("]")})},
            "history hint must split the previous and next shortcuts into separate chips");
}

void coordinateHintFollowsCopyColor() {
    for (const auto mode :
         {ScreenshotShortcutHintMode::Selection, ScreenshotShortcutHintMode::SmartSelection}) {
        const auto rows = screenshotShortcutHintRows(mode);
        const auto snap = std::find_if(rows.cbegin(), rows.cend(), [](const auto& row) {
            return row.label == QStringLiteral("Selection aspect ratio snap");
        });
        require(snap != rows.cend() && snap->shortcut == shortcutDisplay({QStringLiteral("Q")}),
                "both selection stages must show the configurable Q snap default");
        const auto copy = std::find_if(rows.cbegin(), rows.cend(), [](const auto& row) {
            return row.label == QStringLiteral("Copy color");
        });
        require(copy != rows.cend() && copy + 1 != rows.cend() &&
                    (copy + 1)->label == QStringLiteral("Toggle global/relative coordinates") &&
                    (copy + 1)->shortcut == shortcutDisplay({QStringLiteral("Shift+P")}),
                "coordinate toggle must follow Copy color in both selection modes");
        const snow_shot::shortcuts::ShortcutBindingMap disabled{
            {QStringLiteral("toggle_coordinate_mode"), {}}};
        const auto disabledRows = screenshotShortcutHintRows(mode, disabled);
        require(disabledRows.size() == rows.size() - 1 &&
                    std::none_of(disabledRows.cbegin(), disabledRows.cend(),
                                 [](const auto& row) {
                                     return row.label ==
                                            QStringLiteral("Toggle global/relative coordinates");
                                 }),
                "disabled coordinate shortcut must not leave a stale hint");
    }
}

void printShortcutIsNotHinted() {
    const snow_shot::shortcuts::ShortcutBindingMap configured{
        {QStringLiteral("print"), shortcutBindings({QStringLiteral("Ctrl+Alt+P")})}};
    for (const auto mode :
         {ScreenshotShortcutHintMode::Selection, ScreenshotShortcutHintMode::SmartSelection}) {
        for (const auto& rows :
             {screenshotShortcutHintRows(mode), screenshotShortcutHintRows(mode, configured)}) {
            require(
                std::none_of(rows.cbegin(), rows.cend(),
                             [](const auto& row) { return row.label == QStringLiteral("Print"); }),
                "selection hints must omit Print for default and configured shortcuts");
        }
    }
}

void previousSelectionShortcutsAreNotHinted() {
    const snow_shot::shortcuts::ShortcutBindingMap configured{
        {QStringLiteral("select_previously_selected_area"),
         shortcutBindings({QStringLiteral("Alt+F9")})},
        {QStringLiteral("select_next_previously_selected_area"),
         shortcutBindings({QStringLiteral("Alt+F10")})},
    };
    for (const auto mode :
         {ScreenshotShortcutHintMode::Selection, ScreenshotShortcutHintMode::SmartSelection}) {
        for (const auto& rows :
             {screenshotShortcutHintRows(mode), screenshotShortcutHintRows(mode, configured)}) {
            require(std::none_of(rows.cbegin(), rows.cend(),
                                 [](const auto& row) {
                                     return row.label ==
                                                QStringLiteral("Previous selection area") ||
                                            row.label == QStringLiteral("Next selection area") ||
                                            row.label ==
                                                QStringLiteral("Select previously selected area");
                                 }),
                    "selection hints must omit both previous-area navigation shortcuts");
        }
    }
}

void defaultHistoryShortcutUsesSeparateChips() {
    const QVector<ScreenshotShortcutHintRow> rows =
        screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection);
    const ScreenshotShortcutHintRow& historyRow = rows.constLast();
    require(historyRow.label == QStringLiteral("Switch screenshot history") &&
                historyRow.shortcut ==
                    shortcutDisplay({QStringLiteral(","), QStringLiteral(".")}) &&
                historyRow.shortcutChips == QStringList{shortcutDisplay({QStringLiteral(",")}),
                                                        shortcutDisplay({QStringLiteral(".")})},
            "default history keys must render as separate comma and period chips");
}

void unassignedConfiguredShortcutIsNotHinted() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::PenHighlight;
    context.captureMode = ScreenshotCaptureMode::Editing;
    context.configuredShortcuts = snow_shot::shortcuts::ShortcutBindingMap{
        {QStringLiteral("move_cursor_up"), shortcutBindings({QStringLiteral("I")})},
        {QStringLiteral("move_cursor_down"), {}},
        {QStringLiteral("move_cursor_left"), shortcutBindings({QStringLiteral("J")})},
        {QStringLiteral("move_cursor_right"), shortcutBindings({QStringLiteral("L")})},
    };

    const QVector<ScreenshotShortcutHintRow> rows = screenshotShortcutHintRows(context);
    require(rows.size() == 4 && rows.at(0).label == QStringLiteral("Move cursor up") &&
                rows.at(1).label == QStringLiteral("Move cursor left") &&
                rows.at(2).label == QStringLiteral("Move cursor right") &&
                rows.at(3).label == QStringLiteral("Delete selected elements"),
            "an unassigned shortcut action must not leave a stale default hint");
}

void unconfiguredRowsFallBackToSchemaDefaults() {
    const QStringList hintActionIds{
        QStringLiteral("move_cursor_up"),
        QStringLiteral("move_cursor_down"),
        QStringLiteral("move_cursor_left"),
        QStringLiteral("move_cursor_right"),
        QStringLiteral("move_entire_selection"),
        QStringLiteral("keep_selection_width_and_height_consistent"),
        QStringLiteral("switch_selection_between_window_and_window_sub_element"),
        QStringLiteral("previous_screenshot_history"),
        QStringLiteral("next_screenshot_history"),
        QStringLiteral("copy_color"),
        QStringLiteral("toggle_coordinate_mode"),
    };

    snow_shot::shortcuts::ShortcutBindingMap schemaDefaults;
    for (const QString& actionId : hintActionIds) {
        const snow_shot::shortcuts::ShortcutBindingList defaults =
            screenshotShortcutSchemaDefaultKeys(actionId);
        require(!defaults.isEmpty(), "every hinted action must declare a schema default");
        for (const snow_shot::shortcuts::ShortcutBinding& shortcut : defaults) {
            require(!QKeySequence::fromString(shortcut.portableText, QKeySequence::PortableText)
                         .isEmpty(),
                    "every schema default must parse as a portable key sequence");
        }
        schemaDefaults.insert(actionId, defaults);
    }

    require(
        screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection, std::nullopt, true) ==
            screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection, schemaDefaults, true),
        "unconfigured selection rows must render the schema defaults");
    require(screenshotShortcutHintRows(ScreenshotShortcutHintMode::SmartSelection, std::nullopt,
                                       true) ==
                screenshotShortcutHintRows(ScreenshotShortcutHintMode::SmartSelection,
                                           schemaDefaults, true),
            "unconfigured smart-selection rows must render the schema defaults");

    snow_shot::shortcuts::ShortcutBindingMap partialMap = schemaDefaults;
    partialMap.remove(QStringLiteral("copy_color"));
    require(
        screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection, partialMap, true) ==
            screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection, schemaDefaults, true),
        "an action missing from the configured snapshot must fall back to the schema default");
}

void scrollingHintsUseMouseWheelLabels() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::Move;
    context.captureMode = ScreenshotCaptureMode::ScrollingCapture;
    require(screenshotShortcutHintModeForContext(context) == ScreenshotShortcutHintMode::Scrolling,
            "scrolling capture should use the scrolling hint mode");
    require(screenshotShortcutHintLines(context) ==
                QStringList{
                    QStringLiteral("Vertical scroll: Mouse wheel"),
                    QStringLiteral("Horizontal scroll: %1 + Mouse wheel")
                        .arg(snow_shot::shortcuts::ShortcutDisplayService::instance().modifierText(
                            Qt::ShiftModifier)),
                },
            "scrolling capture hint labels changed");
}

void selectionStageContextsRetainShortcutHints() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::Move;

    context.captureMode = ScreenshotCaptureMode::IntelligentSelecting;
    require(screenshotShortcutHintSelectionModeForContext(context) ==
                    ScreenshotShortcutHintMode::SmartSelection &&
                screenshotShortcutHintModeForContext(context) ==
                    ScreenshotShortcutHintMode::SmartSelection &&
                screenshotShortcutHintLines(context) ==
                    screenshotShortcutHintLines(ScreenshotShortcutHintMode::SmartSelection),
            "intelligent selection must retain its shortcut hints through the context resolver");

    context.captureMode = ScreenshotCaptureMode::ManualSelecting;
    require(screenshotShortcutHintSelectionModeForContext(context) ==
                    ScreenshotShortcutHintMode::Selection &&
                screenshotShortcutHintModeForContext(context) ==
                    ScreenshotShortcutHintMode::Selection &&
                screenshotShortcutHintLines(context) ==
                    screenshotShortcutHintLines(ScreenshotShortcutHintMode::Selection),
            "manual selection must retain its shortcut hints through the context resolver");

    context.captureMode = ScreenshotCaptureMode::MovingSelection;
    require(screenshotShortcutHintSelectionModeForContext(context) ==
                    ScreenshotShortcutHintMode::Selection &&
                screenshotShortcutHintModeForContext(context) ==
                    ScreenshotShortcutHintMode::Selection &&
                screenshotShortcutHintLines(context) ==
                    screenshotShortcutHintLines(ScreenshotShortcutHintMode::Selection),
            "the Move tool must retain selection shortcut hints after confirmation");

    context.activeTool = ScreenshotActiveTool::Select;
    require(screenshotShortcutHintSelectionModeForContext(context) ==
                    ScreenshotShortcutHintMode::Hidden &&
                screenshotShortcutHintModeForContext(context) ==
                    ScreenshotShortcutHintMode::Hidden &&
                screenshotShortcutHintLines(context).isEmpty(),
            "moving-selection hints must remain exclusive to the Move tool");
}

void disabledSmartSelectionHidesTheTargetSwitchHint() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::Move;
    context.captureMode = ScreenshotCaptureMode::IntelligentSelecting;
    context.smartSelectionEnabled = false;

    const QStringList lines = screenshotShortcutHintLines(context);
    require(lines.contains(QStringLiteral("Switch element level: Mouse wheel")) &&
                !lines.contains(shortcutLine(QStringLiteral("Select window/window sub-element"),
                                             {QStringLiteral("Tab")})),
            "disabled Smart selection must hide only the Tab target-switch hint");
}

void emptyContextsUseHiddenMode() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::Select;
    context.captureMode = ScreenshotCaptureMode::Editing;
    require(screenshotShortcutHintModeForContext(context) == ScreenshotShortcutHintMode::Tool,
            "a populated drawing-tool context should use tool hint mode");

    context.activeTool = ScreenshotActiveTool::PenHighlight;
    context.quickSelectionDisabledTools.insert(SnowCanvasTool::PenHighlight);
    require(screenshotShortcutHintModeForContext(context) == ScreenshotShortcutHintMode::Tool,
            "cursor movement should keep a conditionally empty tool context visible");

    context.activeTool = ScreenshotActiveTool::Select;
    context.captureMode = ScreenshotCaptureMode::Inactive;
    context.quickSelectionDisabledTools.clear();
    require(screenshotShortcutHintModeForContext(context) == ScreenshotShortcutHintMode::Hidden,
            "non-editing drawing contexts should not leave stale tool hints visible");
}

void multiRegionHintsExposeMovementWithoutResizeControls() {
    ScreenshotShortcutHintContext context;
    context.captureMode = ScreenshotCaptureMode::MovingSelection;
    context.multiRegionEditing = true;
    require(
        screenshotShortcutHintLines(context) ==
            withDefaultCursorHints({
                shortcutLine(QStringLiteral("Move entire selection"), {QStringLiteral("Space")}),
                shortcutLine(QStringLiteral("Copy color"), {QStringLiteral("C")}),
                shortcutLine(QStringLiteral("Toggle global/relative coordinates"),
                             {QStringLiteral("Shift+P")}),
                shortcutLine(QStringLiteral("Toggle cursor visibility"), {QStringLiteral("`")}),
                modifierLine(QStringLiteral("Switch color format"), Qt::ShiftModifier),
                shortcutLine(QStringLiteral("Switch screenshot history"),
                             {QStringLiteral(","), QStringLiteral(".")}),
                shortcutLine(QStringLiteral("Duplicate latest region"), {QStringLiteral("R")}),
            }),
        "settled multi-region hints must show whole-selection movement without resize controls");

    context.multiRegionEditing = false;
    require(screenshotShortcutHintLines(context) ==
                screenshotShortcutHintLines(ScreenshotShortcutHintMode::Selection),
            "replacing the multi-region selection must restore single-selection hints");
}

void regionOperationHintsMatchAvailableInputs() {
    ScreenshotShortcutHintContext context;
    context.multiRegionEditing = true;
    context.regionOperationActive = true;
    for (const auto mode :
         {ScreenshotCaptureMode::ManualSelecting, ScreenshotCaptureMode::IntelligentSelecting}) {
        context.captureMode = mode;
        QStringList expected = withDefaultCursorHints({});
        if (mode == ScreenshotCaptureMode::IntelligentSelecting) {
            expected.push_back(QStringLiteral("Switch element level: Mouse wheel"));
        }
        expected.push_back(modifierLine(QStringLiteral("Switch color format"), Qt::ShiftModifier));
        expected.push_back(
            shortcutLine(QStringLiteral("Duplicate latest region"), {QStringLiteral("R")}));
        require(screenshotShortcutHintLines(context) == expected,
                "region operations must omit shortcuts blocked by the input controller");
        context.smartSelectionEnabled = false;
        require(screenshotShortcutHintLines(context) == expected,
                "region operations never advertise the blocked window/sub-element shortcut");
        context.smartSelectionEnabled = true;
    }

    context.captureMode = ScreenshotCaptureMode::ManualSelecting;
    context.regionConstructionActive = true;
    const QStringList constructionHints = withDefaultCursorHints(
        {modifierLine(QStringLiteral("Switch color format"), Qt::ShiftModifier)});
    require(screenshotShortcutHintLines(context) == constructionHints,
            "custom region construction must hide duplication until the draft is finished");
    context.multiRegionEditing = false;
    context.regionOperationActive = false;
    require(screenshotShortcutHintLines(context) == constructionHints,
            "standalone custom region construction shares the same blocked configured commands");
}

void multiRegionHintsRespectConfiguredAndDisabledShortcuts() {
    ScreenshotShortcutHintContext context;
    context.captureMode = ScreenshotCaptureMode::ManualSelecting;
    context.multiRegionEditing = true;
    context.regionOperationActive = true;
    context.configuredShortcuts = snow_shot::shortcuts::ShortcutBindingMap{
        {QStringLiteral("move_cursor_up"), shortcutBindings({QStringLiteral("Alt+I")})},
        {QStringLiteral("move_cursor_down"), {}},
        {QStringLiteral("move_cursor_left"), {}},
        {QStringLiteral("move_cursor_right"), {}},
        {QStringLiteral("duplicate_latest_region"), shortcutBindings({QStringLiteral("K")})},
        {QStringLiteral("move_entire_selection"), shortcutBindings({QStringLiteral("Alt+M")})},
        {QStringLiteral("copy_color"), shortcutBindings({QStringLiteral("Alt+C")})},
        {QStringLiteral("selection_aspect_ratio_snap"), shortcutBindings({QStringLiteral("G")})},
    };
    require(screenshotShortcutHintLines(context) ==
                QStringList{
                    shortcutLine(QStringLiteral("Move cursor up"), {QStringLiteral("Alt+I")}),
                    modifierLine(QStringLiteral("Switch color format"), Qt::ShiftModifier),
                    shortcutLine(QStringLiteral("Duplicate latest region"), {QStringLiteral("K")}),
                },
            "multi-region hints must follow remapping and disabled bindings without reviving "
            "blocked actions");
    context.configuredShortcuts->insert(QStringLiteral("duplicate_latest_region"), {});
    require(screenshotShortcutHintRows(context).size() == 2,
            "disabling duplication must remove its region-operation hint");
    context.regionOperationActive = false;
    for (const auto mode :
         {ScreenshotCaptureMode::ManualSelecting, ScreenshotCaptureMode::MovingSelection}) {
        context.captureMode = mode;
        require(screenshotShortcutHintLines(context).contains(shortcutLine(
                    QStringLiteral("Move entire selection"), {QStringLiteral("Alt+M")})),
                "settled multi-region hints must show the configured whole-selection shortcut");
    }
    context.configuredShortcuts->insert(QStringLiteral("move_entire_selection"), {});
    const auto rows = screenshotShortcutHintRows(context);
    require(std::none_of(rows.cbegin(), rows.cend(),
                         [](const auto& row) {
                             return row.label == QStringLiteral("Move entire selection");
                         }),
            "disabling whole-selection movement must remove its multi-region hint");
}

void duplicateHintFollowsEditingContextAndConfiguration() {
    ScreenshotShortcutHintContext context;
    context.captureMode = ScreenshotCaptureMode::MovingSelection;
    const auto hasDuplicate = [](const auto& rows) {
        return std::any_of(rows.begin(), rows.end(), [](const auto& row) {
            return row.label == QStringLiteral("Duplicate latest region");
        });
    };
    require(!hasDuplicate(screenshotShortcutHintRows(context)),
            "single selection has no copy hint");
    context.multiRegionEditing = true;
    auto rows = screenshotShortcutHintRows(context);
    require(hasDuplicate(rows) && rows.last().shortcut == shortcutDisplay({QStringLiteral("R")}),
            "multi-region editing advertises the configured duplicate command");
    context.configuredShortcuts = snow_shot::shortcuts::ShortcutBindingMap{
        {QStringLiteral("duplicate_latest_region"), {QStringLiteral("K")}}};
    rows = screenshotShortcutHintRows(context);
    require(rows.last().shortcut == shortcutDisplay({QStringLiteral("K")}),
            "duplicate hint follows remapping");
    context.configuredShortcuts->insert(QStringLiteral("duplicate_latest_region"), {});
    require(!hasDuplicate(screenshotShortcutHintRows(context)),
            "disabled duplicate hides its hint");
}

void hintAreaHidesForSelectionOverlapOrCursorHover() {
    const QRectF hintArea(16.0, 300.0, 240.0, 180.0);
    require(!screenshotShortcutHintAreaIsObscured(hintArea, QRectF(300.0, 100.0, 200.0, 150.0),
                                                  QPointF(500.0, 500.0)),
            "a separate selection and cursor must leave shortcut hints visible");
    require(screenshotShortcutHintAreaIsObscured(hintArea, QRectF(200.0, 250.0, 100.0, 100.0),
                                                 QPointF(500.0, 500.0)),
            "a selection overlapping the shortcut hint area must hide it");
    require(screenshotShortcutHintAreaIsObscured(hintArea, QRectF(), QPointF(100.0, 350.0)),
            "a cursor over the shortcut hint area must hide it");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    enterHintsUseApplicationLabels();
    toolMatrixMatchesRequestedVisibility();
    configuredShortcutRowsUseActualValues();
    coordinateHintFollowsCopyColor();
    printShortcutIsNotHinted();
    previousSelectionShortcutsAreNotHinted();
    defaultHistoryShortcutUsesSeparateChips();
    unassignedConfiguredShortcutIsNotHinted();
    unconfiguredRowsFallBackToSchemaDefaults();
    scrollingHintsUseMouseWheelLabels();
    selectionStageContextsRetainShortcutHints();
    disabledSmartSelectionHidesTheTargetSwitchHint();
    emptyContextsUseHiddenMode();
    hintAreaHidesForSelectionOverlapOrCursorHover();
    multiRegionHintsExposeMovementWithoutResizeControls();
    regionOperationHintsMatchAvailableInputs();
    multiRegionHintsRespectConfiguredAndDisabledShortcuts();
    duplicateHintFollowsEditingContextAndConfiguration();
    return 0;
}
