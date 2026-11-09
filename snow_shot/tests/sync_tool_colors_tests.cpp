#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "widgets/color_picker.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScopeGuard>

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace snow_shot::presentation;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ScreenshotToolPalette::Options options() {
    ScreenshotToolPalette::Options result;
    result.showTextTool = true;
    result.showSequenceNumberTool = true;
    result.styleDefaults = screenshotCanvasToolStyleDefaults();
    return result;
}

struct Editor {
    SnowCanvasRuntime runtime{SnowCanvasRuntimeConfig{screenshotCanvasToolStyleDefaults()}};
    SnowCanvasWidget canvas{runtime};
    ScreenshotToolPalette palette{options()};
    ScreenshotStyleBinding binding{palette, canvas, &palette};
    Editor() {
        canvas.resize(400, 300);
        canvas.setInteractionEnabled(true);
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette, [this] {
            palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
        });
    }
};

void assertColors(const SnowCanvasStyleDefaults& styles, const QColor& color) {
    for (const auto* shape : {&styles.rectangle, &styles.speechBubble, &styles.line,
                              &styles.freeDraw, &styles.rectangleHighlight, &styles.penHighlight})
        require(shape->stroke == color && shape->fill == color,
                "every shape creation stroke and fill must use the shared RGBA");
    for (const QColor& actual :
         {styles.arrow.stroke, styles.distance.stroke, styles.angle.stroke, styles.bracket.stroke,
          styles.magnifier.stroke, styles.text.color, styles.text.stroke, styles.text.fill,
          styles.serialNumber.color, styles.serialNumber.fill})
        require(actual == color, "text, numbers and specialized tools must share the color");
}

void assertRuntimeColors(SnowCanvasWidget& canvas, const QColor& color) {
    for (const auto tool :
         {SnowCanvasTool::Shape, SnowCanvasTool::SpeechBubble, SnowCanvasTool::Arrow,
          SnowCanvasTool::Line, SnowCanvasTool::FreeDraw, SnowCanvasTool::RectangleHighlight,
          SnowCanvasTool::PenHighlight, SnowCanvasTool::Distance, SnowCanvasTool::Angle,
          SnowCanvasTool::Bracket, SnowCanvasTool::Magnifier, SnowCanvasTool::Text,
          SnowCanvasTool::SerialNumber}) {
        require(canvas.setCanvasTool(tool), "switch drawing tools");
        const auto state = canvas.canvasStyleToolbarState();
        QColor actual;
        switch (tool) {
        case SnowCanvasTool::Text:
            actual = state.textStyle.color;
            require(state.textStyle.fill == color && state.textStyle.stroke == color,
                    "text fill and stroke follow the foreground");
            break;
        case SnowCanvasTool::SerialNumber:
            actual = state.serialNumberStyle.color;
            require(state.serialNumberStyle.fill == color, "number fills are shared");
            break;
        case SnowCanvasTool::Distance:
            actual = state.distanceStyle.stroke;
            break;
        case SnowCanvasTool::Angle:
            actual = state.angleStyle.stroke;
            break;
        case SnowCanvasTool::Bracket:
            actual = state.bracketStyle.stroke;
            break;
        case SnowCanvasTool::Magnifier:
            actual = state.magnifierStyle.stroke;
            break;
        default:
            actual = state.shapeStyle.stroke;
            break;
        }
        require(actual == color,
                "the runtime must create with the shared color after tool switching");
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, const QPointF& point) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void committedColorsReachEveryEditor() {
    const storage::DrawingSettings settings;
    Editor first;
    Editor second;
    SnowCanvasWidget peer(first.runtime);
    require(settings.setSyncToolColors(true), "enable sync in live editors");
    const auto olderSession = first.runtime.serializeDocumentSession();
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto commit = [&](const SnowCanvasStyleEdit& edit, const QColor& color) {
        require(first.canvas.setCanvasTool(SnowCanvasTool::Select), "clear creation tool");
        require(first.canvas.commitStyleEdit(edit), "commit an eligible color edit");
        require(first.binding.lastSaveSucceeded() == true && settings.sharedToolColor() == color,
                "successful color commits persist the shared value");
        assertColors(first.palette.creationStyleDefaults(), color);
        assertColors(second.palette.creationStyleDefaults(), color);
        assertColors(screenshotCanvasToolStyleDefaults(), color);
        const auto revision = storage::ApplicationStorage::instance().configuration().revision();
        assertRuntimeColors(peer, color);
        assertRuntimeColors(second.canvas, color);
        require(storage::ApplicationStorage::instance().configuration().revision() == revision,
                "switching tools must not write creation preferences");
    };
    auto shape = original.rectangle;
    shape.stroke = QColor(23, 76, 129, 117);
    commit(SnowCanvasShapeEdit{shape, SnowCanvasShapeStylePropertyStrokeColor}, shape.stroke);
    shape.fill = QColor(65, 43, 21, 0);
    commit(SnowCanvasShapeEdit{shape, SnowCanvasShapeStylePropertyFillColor}, shape.fill);
    shape.fill = QColor(9, 71, 37, 200);
    commit(SnowCanvasShapeEdit{shape, SnowCanvasShapeStylePropertyFillColor,
                               SnowCanvasShapeKind::RectangleHighlight},
           shape.fill);
    shape.stroke = QColor(11, 22, 33, 44);
    commit(SnowCanvasShapeEdit{shape, SnowCanvasShapeStylePropertyStrokeColor,
                               SnowCanvasShapeKind::PenHighlight},
           shape.stroke);
    auto text = original.text;
    text.color = QColor(42, 84, 126, 168);
    commit(SnowCanvasTextEdit{text, SnowCanvasTextStyleMixedColor}, text.color);
    text.stroke = QColor(12, 34, 56, 78);
    commit(SnowCanvasTextEdit{text, SnowCanvasTextStyleMixedStroke}, text.stroke);
    text.fill = QColor(21, 43, 65, 87);
    commit(SnowCanvasTextEdit{text, SnowCanvasTextStyleMixedFill}, text.fill);
    auto number = original.serialNumber;
    number.color = QColor(13, 26, 39, 0);
    commit(SnowCanvasSerialNumberEdit{number, SnowCanvasSerialNumberStyleMixedColor}, number.color);
    number.fill = QColor(19, 38, 57, 76);
    commit(SnowCanvasSerialNumberEdit{number, SnowCanvasSerialNumberStyleMixedFill}, number.fill);
    const auto shared = settings.sharedToolColor();
    const auto before = screenshotCanvasToolStyleDefaults();
    require(before.rectangle.strokeWidth == original.rectangle.strokeWidth &&
                before.rectangle.fillStyle == original.rectangle.fillStyle &&
                before.rectangleHighlight.opacity == original.rectangleHighlight.opacity &&
                before.watermark == original.watermark && before.spotlight == original.spotlight &&
                before.arrow.arrowOutlineColor == original.arrow.arrowOutlineColor,
            "color synchronization preserves non-color properties and excluded colors");
    shape.arrowOutlineColor = QColor(40, 80, 120);
    require(first.canvas.commitStyleEdit(SnowCanvasShapeEdit{
                shape, SnowCanvasShapeStylePropertyArrowOutlineColor, SnowCanvasShapeKind::Arrow}),
            "arrow outline edits remain independent");
    text.fontSize = 41;
    require(
        first.canvas.commitStyleEdit(SnowCanvasTextEdit{text, SnowCanvasTextStyleMixedFontSize}) &&
            settings.sharedToolColor() == shared,
        "non-color edits must ignore stale colors carried by the edit payload");
    text.fontSize = std::numeric_limits<double>::quiet_NaN();
    text.color = Qt::blue;
    const auto revision = storage::ApplicationStorage::instance().configuration().revision();
    require(
        !first.canvas.commitStyleEdit(SnowCanvasTextEdit{text, SnowCanvasTextStyleMixedColor}) &&
            settings.sharedToolColor() == shared &&
            storage::ApplicationStorage::instance().configuration().revision() == revision,
        "rejected edits must not change shared preferences");
    require(first.runtime.restoreDocumentSession(olderSession),
            "restore a session with older creation colors");
    assertRuntimeColors(first.canvas, shared);
    SnowCanvasRuntime independentClone;
    require(independentClone.cloneDocumentSessionFrom(first.runtime), "clone a synced session");
    SnowCanvasWidget cloneCanvas(independentClone);
    require(cloneCanvas.setCanvasTool(SnowCanvasTool::Shape), "inspect clone's independent policy");
    auto cloneStyle = cloneCanvas.canvasStyleToolbarState().shapeStyle;
    cloneStyle.stroke = Qt::green;
    require(cloneCanvas.applyStyleEdit(
                SnowCanvasShapeEdit{cloneStyle, SnowCanvasShapeStylePropertyStrokeColor}) &&
                cloneCanvas.canvasStyleToolbarState().shapeStyle.stroke == cloneStyle.stroke,
            "a cloned runtime must honor its destination color policy");
    require(settings.setSyncToolColors(false), "disable synchronization");
    shape.stroke = QColor(200, 40, 20);
    require(first.canvas.commitStyleEdit(
                SnowCanvasShapeEdit{shape, SnowCanvasShapeStylePropertyStrokeColor}),
            "edit independent shape color");
    const auto independent = screenshotCanvasToolStyleDefaults();
    require(independent.rectangle.stroke == shape.stroke && independent.text.color == shared,
            "disabled tools diverge from their last synchronized colors");
    require(settings.setSyncToolColors(true), "re-enable with remembered color");
    assertColors(screenshotCanvasToolStyleDefaults(), shared);
    require(first.canvas.setCanvasTool(SnowCanvasTool::Shape), "activate shape color controls");
    first.palette.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    auto pickers = first.palette.findChildren<adqt::widgets::AdColorPicker*>();
    adqt::widgets::AdColorPicker* strokePicker = nullptr;
    for (auto* picker : pickers) {
        if (picker->accessibleName() == QStringLiteral("Stroke color"))
            strokePicker = picker;
    }
    require(strokePicker, "shape toolbar exposes its stroke color picker");
    const auto beforePreview = storage::ApplicationStorage::instance().configuration().revision();
    const auto originalStyle = first.canvas.canvasStyleToolbarState().shapeStyle;
    auto previewStyle = originalStyle;
    previewStyle.stroke = QColor(201, 102, 3, 45);
    require(first.canvas.applyStyleEdit(
                SnowCanvasShapeEdit{previewStyle, SnowCanvasShapeStylePropertyStrokeColor}) &&
                first.canvas.applyStyleEdit(
                    SnowCanvasShapeEdit{originalStyle, SnowCanvasShapeStylePropertyStrokeColor}) &&
                settings.sharedToolColor() == shared &&
                storage::ApplicationStorage::instance().configuration().revision() == beforePreview,
            "uncommitted color previews and their cancellation must not update preferences");
    const QColor picked(18, 54, 90, 126);
    strokePicker->commitValue(adqt::widgets::AdColorValue::solid(picked));
    require(settings.sharedToolColor() == picked,
            "color picker commits update the shared preference");
    assertColors(second.palette.creationStyleDefaults(), picked);
    require(first.canvas.canvasStyleToolbarState().shapeStyle.fill == picked,
            "the active tool's other creation color follows the picker immediately");
}

void selectedEditsAndActiveDraftsStayLocal() {
    const storage::DrawingSettings settings;
    Editor editor;
    editor.canvas.show();
    QApplication::processEvents();
    require(editor.canvas.setViewportCamera(200, 150, 1), "set drawing camera");
    require(editor.canvas.setCanvasTool(SnowCanvasTool::Shape), "draw original rectangle");
    mouse(editor.canvas, QEvent::MouseButtonPress, {40, 40});
    mouse(editor.canvas, QEvent::MouseMove, {160, 100});
    mouse(editor.canvas, QEvent::MouseButtonRelease, {160, 100});
    require(editor.canvas.setCanvasTool(SnowCanvasTool::Select), "select original rectangle");
    mouse(editor.canvas, QEvent::MouseButtonPress, {40, 60});
    mouse(editor.canvas, QEvent::MouseButtonRelease, {40, 60});
    const auto selected = editor.canvas.canvasStyleToolbarState();
    require(selected.source == SnowCanvasStyleToolbarSource::SelectedRectangle,
            "fixture must have a selected rectangle");
    const auto checkpoint = editor.runtime.documentCheckpoint();
    const auto history = editor.runtime.serializeDocumentHistory();
    const auto selection = editor.runtime.selectedElementIds();
    const QColor color(5, 90, 140, 160);
    require(storage::setDrawingStyleValues(
                {{QStringLiteral("drawing/shared_tool_color"), storage::colorToRgbaString(color)}}),
            "change shared preference");
    require(editor.runtime.documentCheckpoint() == checkpoint,
            "external color changes must preserve document content");
    require(editor.runtime.serializeDocumentHistory() == history,
            "external color changes must preserve history");
    require(editor.runtime.selectedElementIds() == selection &&
                editor.canvas.canvasStyleToolbarState().source == selected.source &&
                editor.canvas.canvasStyleToolbarState().shapeStyle == selected.shapeStyle,
            "external color changes must preserve the selected element and its displayed style");
    auto edited = selected.shapeStyle;
    edited.stroke = QColor(170, 60, 30, 80);
    require(editor.canvas.commitStyleEdit(
                SnowCanvasShapeEdit{edited, SnowCanvasShapeStylePropertyStrokeColor}),
            "edit selected stroke");
    require(editor.canvas.canvasStyleToolbarState().shapeStyle.fill == selected.shapeStyle.fill &&
                settings.sharedToolColor() == edited.stroke,
            "selected stroke changes only that property and remembers the shared creation color");
    require(editor.runtime.undo() && editor.runtime.documentCheckpoint() == checkpoint,
            "one undo reverts the selected edit without extra synchronization history");
    require(settings.sharedToolColor() == edited.stroke,
            "undo does not change creation preferences");
    require(editor.canvas.setCanvasTool(SnowCanvasTool::Text), "start text draft");
    mouse(editor.canvas, QEvent::MouseButtonPress, {240, 180});
    mouse(editor.canvas, QEvent::MouseButtonRelease, {240, 180});
    require(editor.canvas.hasActiveTextEditing(), "text fixture has an active draft");
    const auto draftState = editor.canvas.canvasStyleToolbarState();
    require(storage::setDrawingStyleValues(
                {{QStringLiteral("drawing/shared_tool_color"), storage::colorToRgbaString(color)}}),
            "update while typing");
    require(editor.canvas.hasActiveTextEditing() &&
                editor.canvas.canvasStyleToolbarState().textStyle == draftState.textStyle &&
                editor.canvas.canvasStyleToolbarState().source == draftState.source,
            "creation synchronization must not interrupt or recolor an active text draft");
    QKeyEvent letter(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"));
    QApplication::sendEvent(&editor.canvas, &letter);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&editor.canvas, &escape);
    assertRuntimeColors(editor.canvas, color);
}

void failedPersistenceRemainsReported() {
    SnowCanvasRuntime runtime{SnowCanvasRuntimeConfig{screenshotCanvasToolStyleDefaults()}};
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette, {},
                                   [](const SnowCanvasStyleEdit&) { return false; });
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto revision = configuration.revision();
    const auto saved = storage::DrawingSettings().sharedToolColor();
    auto shape = screenshotCanvasToolStyleDefaults().rectangle;
    shape.stroke = QColor(197, 103, 47, 91);
    require(
        canvas.commitStyleEdit(
            SnowCanvasShapeEdit{shape, SnowCanvasShapeStylePropertyStrokeColor}) &&
            binding.lastSaveSucceeded() == false && configuration.revision() == revision &&
            storage::DrawingSettings().sharedToolColor() == saved,
        "a failed save remains observable without claiming the shared preference was persisted");
    assertColors(palette.creationStyleDefaults(), shape.stroke);
    assertRuntimeColors(canvas, shape.stroke);
}

void activeShapeGesturesKeepTheirColors() {
    const storage::DrawingSettings settings;
    require(settings.setSyncToolColors(true), "enable sync for gesture isolation");
    const QColor original(180, 70, 30, 117);
    require(storage::setDrawingStyleValues({{QStringLiteral("drawing/shared_tool_color"),
                                             storage::colorToRgbaString(original)}}),
            "set the initial shared color");
    Editor drawing;
    Editor peer;
    drawing.canvas.show();
    QApplication::processEvents();
    require(drawing.canvas.setViewportCamera(200, 150, 1) &&
                drawing.canvas.setCanvasTool(SnowCanvasTool::Shape),
            "prepare a rectangle gesture");
    mouse(drawing.canvas, QEvent::MouseButtonPress, {40, 40});
    mouse(drawing.canvas, QEvent::MouseMove, {100, 80});
    const auto document = drawing.runtime.documentCheckpoint();
    const auto history = drawing.runtime.serializeDocumentHistory();
    const QColor updated(20, 80, 140, 168);
    auto style = peer.canvas.canvasStyleToolbarState().shapeStyle;
    style.stroke = updated;
    require(peer.canvas.commitStyleEdit(
                SnowCanvasShapeEdit{style, SnowCanvasShapeStylePropertyStrokeColor}),
            "another editor changes the shared color during the gesture");
    require(settings.sharedToolColor() == updated &&
                drawing.runtime.documentCheckpoint() == document &&
                drawing.runtime.serializeDocumentHistory() == history,
            "cross-editor synchronization changes only creation preferences");
    mouse(drawing.canvas, QEvent::MouseMove, {160, 100});
    mouse(drawing.canvas, QEvent::MouseButtonRelease, {160, 100});
    require(drawing.canvas.setCanvasTool(SnowCanvasTool::Select), "inspect the committed gesture");
    mouse(drawing.canvas, QEvent::MouseButtonPress, {40, 60});
    mouse(drawing.canvas, QEvent::MouseButtonRelease, {40, 60});
    const auto first = drawing.canvas.canvasStyleToolbarState();
    require(first.source == SnowCanvasStyleToolbarSource::SelectedRectangle &&
                first.shapeStyle.stroke == original && first.shapeStyle.fill == original,
            "an in-progress rectangle retains its stroke and fill across shared color changes");
    mouse(drawing.canvas, QEvent::MouseButtonPress, {360, 260});
    mouse(drawing.canvas, QEvent::MouseButtonRelease, {360, 260});
    require(drawing.canvas.setCanvasTool(SnowCanvasTool::Shape), "begin the next rectangle");
    mouse(drawing.canvas, QEvent::MouseButtonPress, {220, 160});
    mouse(drawing.canvas, QEvent::MouseButtonRelease, {320, 220});
    require(drawing.canvas.setCanvasTool(SnowCanvasTool::Select), "inspect the next rectangle");
    mouse(drawing.canvas, QEvent::MouseButtonPress, {220, 180});
    mouse(drawing.canvas, QEvent::MouseButtonRelease, {220, 180});
    const auto next = drawing.canvas.canvasStyleToolbarState();
    require(next.source == SnowCanvasStyleToolbarSource::SelectedRectangle &&
                next.shapeStyle.stroke == updated && next.shapeStyle.fill == updated,
            "the next gesture uses the latest shared RGBA");
}
} // namespace

void runSyncToolColorTests() {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto snapshot = configuration.snapshot();
    const auto restore =
        qScopeGuard([&] { require(configuration.applySnapshot(snapshot), "restore settings"); });
    committedColorsReachEveryEditor();
    selectedEditsAndActiveDraftsStayLocal();
    failedPersistenceRemainsReported();
    activeShapeGesturesKeepTheirColors();
}
