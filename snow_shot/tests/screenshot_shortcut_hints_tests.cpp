#include "snow_shot/presentation/screenshotshortcuthints.h"

#include <QCoreApplication>
#include <QStringList>

#include <initializer_list>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        qFatal("%s", message);
    }
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
        QStringLiteral("Move cursor up: W / Up"),
        QStringLiteral("Move cursor down: S / Down"),
        QStringLiteral("Move cursor left: A / Left"),
        QStringLiteral("Move cursor right: D / Right"),
    };
    for (const QString& line : remaining) {
        lines.push_back(line);
    }
    return lines;
}

void toolMatrixMatchesRequestedVisibility() {
    const QStringList transformHints = withDefaultCursorHints({
        QStringLiteral("Maintain aspect ratio: Shift"),
        QStringLiteral("Fixed-angle rotation: Shift"),
        QStringLiteral("Scale from center: Alt"),
        QStringLiteral("Auto-align: Ctrl"),
        QStringLiteral("Delete selected elements: Delete"),
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

    QStringList penTransformHints =
        withDefaultCursorHints({QStringLiteral("Draw straight line: Shift")});
    penTransformHints.append(transformHints.mid(4));
    require(hintLines(ScreenshotActiveTool::FreeDraw) == penTransformHints,
            "free-draw hint matrix changed");
    require(hintLines(ScreenshotActiveTool::PenFilter) == penTransformHints,
            "pen-filter hint matrix changed");
    require(hintLines(ScreenshotActiveTool::PenHighlight) ==
                withDefaultCursorHints(
                    {QStringLiteral("Delete selected elements: Delete")}),
            "pen-highlighter hint matrix changed");

    const QStringList textTransformHints = withDefaultCursorHints({
        QStringLiteral("Fixed-angle rotation: Shift"),
        QStringLiteral("Scale from center: Alt"),
        QStringLiteral("Auto-align: Ctrl"),
        QStringLiteral("Delete selected elements: Delete"),
    });
    require(hintLines(ScreenshotActiveTool::Text) == textTransformHints,
            "text hint matrix changed");
    require(hintLines(ScreenshotActiveTool::SerialNumber) == textTransformHints,
            "serial-number hint matrix changed");

    require(hintLines(ScreenshotActiveTool::Shape, {SnowCanvasTool::Shape}) ==
                withDefaultCursorHints({
                    QStringLiteral("Maintain aspect ratio: Shift"),
                    QStringLiteral("Scale from center: Alt"),
                    QStringLiteral("Auto-align: Ctrl"),
                }),
            "shape quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::Arrow, {SnowCanvasTool::Arrow}) ==
                withDefaultCursorHints({
                    QStringLiteral("Fixed-angle rotation: Shift"),
                    QStringLiteral("Auto-align: Ctrl"),
                }),
            "arrow quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::Line, {SnowCanvasTool::Line}) ==
                withDefaultCursorHints({
                    QStringLiteral("Fixed-angle rotation: Shift"),
                    QStringLiteral("Auto-align: Ctrl"),
                }),
            "line quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::RectangleHighlight,
                      {SnowCanvasTool::RectangleHighlight}) ==
                withDefaultCursorHints({
                    QStringLiteral("Maintain aspect ratio: Shift"),
                    QStringLiteral("Scale from center: Alt"),
                    QStringLiteral("Auto-align: Ctrl"),
                }),
            "rectangle-highlighter quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::RectangleFilter,
                      {SnowCanvasTool::RectangleFilter}) ==
                withDefaultCursorHints({
                    QStringLiteral("Maintain aspect ratio: Shift"),
                    QStringLiteral("Scale from center: Alt"),
                    QStringLiteral("Auto-align: Ctrl"),
                }),
            "rectangle-filter quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::FreeDraw, {SnowCanvasTool::FreeDraw}) ==
                withDefaultCursorHints({QStringLiteral("Draw straight line: Shift")}),
            "free-draw quick-selection suppression changed");
    require(hintLines(ScreenshotActiveTool::PenFilter, {SnowCanvasTool::PenFilter}) ==
                withDefaultCursorHints({QStringLiteral("Draw straight line: Shift")}),
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
    context.configuredShortcuts = QMap<QString, QStringList>{
        {QStringLiteral("move_cursor_up"),
         {QStringLiteral("Ctrl+Alt+I"), QStringLiteral("Up")}},
        {QStringLiteral("move_cursor_down"), {QStringLiteral("Ctrl+Alt+K")}},
        {QStringLiteral("move_cursor_left"), {QStringLiteral("Ctrl+Alt+J")}},
        {QStringLiteral("move_cursor_right"), {QStringLiteral("Ctrl+Alt+L")}},
        {QStringLiteral("move_entire_selection"), {QStringLiteral("Ctrl+M")}},
        {QStringLiteral("keep_selection_width_and_height_consistent"),
         {QStringLiteral("Alt+R")}},
        {QStringLiteral("select_previously_selected_area"), {QStringLiteral("P")}},
        {QStringLiteral("copy_color"), {QStringLiteral("Alt+C")}},
        {QStringLiteral("previous_screenshot_history"),
         {QStringLiteral("PgUp"), QStringLiteral("[")}},
        {QStringLiteral("next_screenshot_history"),
         {QStringLiteral("PgDown"), QStringLiteral("]")}},
    };

    const QVector<ScreenshotShortcutHintRow> rows = screenshotShortcutHintRows(context);
    require(rows.size() == 10, "manual-selection configured hint row count changed");
    require(rows.at(0).label == QStringLiteral("Move cursor up") &&
                rows.at(0).shortcut == QStringLiteral("Ctrl+Alt+I / Up") &&
                rows.at(1).label == QStringLiteral("Move cursor down") &&
                rows.at(1).shortcut == QStringLiteral("Ctrl+Alt+K") &&
                rows.at(2).label == QStringLiteral("Move cursor left") &&
                rows.at(2).shortcut == QStringLiteral("Ctrl+Alt+J") &&
                rows.at(3).label == QStringLiteral("Move cursor right") &&
                rows.at(3).shortcut == QStringLiteral("Ctrl+Alt+L"),
            "cursor directions must use four independent configured rows");
    require(rows.at(4).shortcut == QStringLiteral("Ctrl+M") &&
                rows.at(5).shortcut == QStringLiteral("Alt+R") &&
                rows.at(6).shortcut == QStringLiteral("P") &&
                rows.at(7).shortcut == QStringLiteral("Alt+C"),
            "selection action hints must use configured shortcuts");
    require(rows.at(8).label == QStringLiteral("Switch Color Format") &&
                rows.at(8).shortcut == QStringLiteral("Shift"),
            "the fixed color-format shortcut must remain visible");
    require(rows.at(9).label == QStringLiteral("Switch Screenshot History") &&
                rows.at(9).shortcut == QStringLiteral("PgUp / [ / PgDown / ]") &&
                rows.at(9).shortcutChips == QStringList{QStringLiteral("PgUp / ["),
                                                       QStringLiteral("PgDown / ]")},
            "history hint must split the previous and next shortcuts into separate chips");
}

void defaultHistoryShortcutUsesSeparateChips() {
    const QVector<ScreenshotShortcutHintRow> rows =
        screenshotShortcutHintRows(ScreenshotShortcutHintMode::Selection);
    const ScreenshotShortcutHintRow& historyRow = rows.constLast();
    require(historyRow.label == QStringLiteral("Switch Screenshot History") &&
                historyRow.shortcut == QStringLiteral(", / .") &&
                historyRow.shortcutChips ==
                    QStringList{QStringLiteral(","), QStringLiteral(".")},
            "default history keys must render as separate comma and period chips");
}

void unassignedConfiguredShortcutIsNotHinted() {
    ScreenshotShortcutHintContext context;
    context.activeTool = ScreenshotActiveTool::PenHighlight;
    context.captureMode = ScreenshotCaptureMode::Editing;
    context.configuredShortcuts = QMap<QString, QStringList>{
        {QStringLiteral("move_cursor_up"), {QStringLiteral("I")}},
        {QStringLiteral("move_cursor_down"), {}},
        {QStringLiteral("move_cursor_left"), {QStringLiteral("J")}},
        {QStringLiteral("move_cursor_right"), {QStringLiteral("L")}},
    };

    const QVector<ScreenshotShortcutHintRow> rows = screenshotShortcutHintRows(context);
    require(rows.size() == 4 && rows.at(0).label == QStringLiteral("Move cursor up") &&
                rows.at(1).label == QStringLiteral("Move cursor left") &&
                rows.at(2).label == QStringLiteral("Move cursor right") &&
                rows.at(3).label == QStringLiteral("Delete selected elements"),
            "an unassigned shortcut action must not leave a stale default hint");
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
                    QStringLiteral("Horizontal scroll: Shift + mouse wheel"),
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
                !lines.contains(QStringLiteral("Select Window/Window Sub-element: Tab")),
            "disabled Smart Selection must hide only the Tab target-switch hint");
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
    require(!screenshotShortcutHintAreaIsObscured(
                hintArea, QRectF(300.0, 100.0, 200.0, 150.0), QPointF(500.0, 500.0)),
            "a separate selection and cursor must leave shortcut hints visible");
    require(screenshotShortcutHintAreaIsObscured(
                hintArea, QRectF(200.0, 250.0, 100.0, 100.0), QPointF(500.0, 500.0)),
            "a selection overlapping the shortcut hint area must hide it");
    require(screenshotShortcutHintAreaIsObscured(
                hintArea, QRectF(), QPointF(100.0, 350.0)),
            "a cursor over the shortcut hint area must hide it");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    toolMatrixMatchesRequestedVisibility();
    configuredShortcutRowsUseActualValues();
    defaultHistoryShortcutUsesSeparateChips();
    unassignedConfiguredShortcutIsNotHinted();
    scrollingHintsUseMouseWheelLabels();
    selectionStageContextsRetainShortcutHints();
    disabledSmartSelectionHidesTheTargetSwitchHint();
    emptyContextsUseHiddenMode();
    hintAreaHidesForSelectionOverlapOrCursorHover();
    return 0;
}
