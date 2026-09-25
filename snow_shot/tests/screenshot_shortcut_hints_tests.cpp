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
    return snow_shot::shortcuts::formatShortcutListDisplayText(shortcutBindings(portableText));
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

void toolMatrixMatchesRequestedVisibility() {
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

    require(hintLines(ScreenshotActiveTool::Eraser).isEmpty() &&
                hintLines(ScreenshotActiveTool::Ocr).isEmpty() &&
                hintLines(ScreenshotActiveTool::Table).isEmpty() &&
                hintLines(ScreenshotActiveTool::Qr).isEmpty() &&
                hintLines(ScreenshotActiveTool::Move).isEmpty() &&
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
        {QStringLiteral("select_previously_selected_area"),
         shortcutBindings({QStringLiteral("P")})},
        {QStringLiteral("copy_color"), shortcutBindings({QStringLiteral("Alt+C")})},
        {QStringLiteral("toggle_coordinate_mode"), shortcutBindings({QStringLiteral("Alt+P")})},
        {QStringLiteral("previous_screenshot_history"),
         shortcutBindings({QStringLiteral("PgUp"), QStringLiteral("[")})},
        {QStringLiteral("next_screenshot_history"),
         shortcutBindings({QStringLiteral("PgDown"), QStringLiteral("]")})},
    };

    const QVector<ScreenshotShortcutHintRow> rows = screenshotShortcutHintRows(context);
    require(rows.size() == 11, "manual-selection configured hint row count changed");
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
                rows.at(6).shortcut == shortcutDisplay({QStringLiteral("P")}) &&
                rows.at(7).shortcut == shortcutDisplay({QStringLiteral("Alt+C")}),
            "selection action hints must use configured shortcuts");
    require(rows.at(8).label == QStringLiteral("Toggle Global/Relative Coordinates") &&
                rows.at(8).shortcut == shortcutDisplay({QStringLiteral("Alt+P")}),
            "coordinate toggle must follow Copy color and show the configured binding");
    require(rows.at(9).label == QStringLiteral("Switch color format") &&
                rows.at(9).shortcut == shortcutDisplay({QStringLiteral("Shift")}),
            "the fixed color-format shortcut must remain visible");
    require(rows.at(10).label == QStringLiteral("Switch screenshot history") &&
                rows.at(10).shortcut ==
                    shortcutDisplay({QStringLiteral("PgUp"), QStringLiteral("["),
                                     QStringLiteral("PgDown"), QStringLiteral("]")}) &&
                rows.at(10).shortcutChips ==
                    QStringList{shortcutDisplay({QStringLiteral("PgUp"), QStringLiteral("[")}),
                                shortcutDisplay({QStringLiteral("PgDown"), QStringLiteral("]")})},
            "history hint must split the previous and next shortcuts into separate chips");
}

void coordinateHintFollowsCopyColor() {
    for (const auto mode :
         {ScreenshotShortcutHintMode::Selection, ScreenshotShortcutHintMode::SmartSelection}) {
        const auto rows = screenshotShortcutHintRows(mode);
        const auto copy = std::find_if(rows.cbegin(), rows.cend(), [](const auto& row) {
            return row.label == QStringLiteral("Copy color");
        });
        require(copy != rows.cend() && copy + 1 != rows.cend() &&
                    (copy + 1)->label == QStringLiteral("Toggle Global/Relative Coordinates") &&
                    (copy + 1)->shortcut == shortcutDisplay({QStringLiteral("Shift+P")}),
                "coordinate toggle must follow Copy color in both selection modes");
        const snow_shot::shortcuts::ShortcutBindingMap disabled{
            {QStringLiteral("toggle_coordinate_mode"), {}}};
        const auto disabledRows = screenshotShortcutHintRows(mode, disabled);
        require(disabledRows.size() == rows.size() - 1 &&
                    std::none_of(disabledRows.cbegin(), disabledRows.cend(),
                                 [](const auto& row) {
                                     return row.label ==
                                            QStringLiteral("Toggle Global/Relative Coordinates");
                                 }),
                "disabled coordinate shortcut must not leave a stale hint");
    }
}

void defaultHistoryShortcutUsesSeparateChips() {
    const QVector<ScreenshotShortcutHintRow> rows =
        screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection);
    const ScreenshotShortcutHintRow& historyRow = rows.constLast();
    require(historyRow.label == QStringLiteral("Switch screenshot history") &&
                historyRow.shortcut == QStringLiteral(", / .") &&
                historyRow.shortcutChips == QStringList{QStringLiteral(","), QStringLiteral(".")},
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
        QStringLiteral("select_previously_selected_area"),
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
                    QStringLiteral("Vertical scroll: mouse wheel"),
                    QStringLiteral("Horizontal scroll: %1 + mouse wheel")
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
    require(lines.contains(QStringLiteral("Switch element level: mouse wheel")) &&
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
    toolMatrixMatchesRequestedVisibility();
    configuredShortcutRowsUseActualValues();
    coordinateHintFollowsCopyColor();
    defaultHistoryShortcutUsesSeparateChips();
    unassignedConfiguredShortcutIsNotHinted();
    unconfiguredRowsFallBackToSchemaDefaults();
    scrollingHintsUseMouseWheelLabels();
    selectionStageContextsRetainShortcutHints();
    disabledSmartSelectionHidesTheTargetSwitchHint();
    emptyContextsUseHiddenMode();
    hintAreaHidesForSelectionOverlapOrCursorHover();
    return 0;
}
