#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QKeyEvent>
#include "widgets/button.h"

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScopeGuard>
#include <limits>
#include <cstdlib>
#include <iostream>

namespace {
using namespace snow_shot::presentation;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}
ScreenshotToolPalette::Options options() {
    ScreenshotToolPalette::Options result;
    result.showTextTool = true;
    result.showSerialNumberTool = true;
    result.showFilterTool = true;
    result.showWatermarkTool = true;
    result.showSpotlightTool = true;
    result.styleDefaults = screenshotCanvasToolStyleDefaults();
    return result;
}
struct Editor {
    SnowCanvasWidget canvas;
    ScreenshotToolPalette palette{options()};
    ScreenshotStyleBinding binding{palette, canvas, &palette};
    Editor() {
        applyScreenshotCanvasToolStyles(canvas, palette.creationStyleDefaults());
        canvas.resize(400, 300);
        canvas.setInteractionEnabled(true);
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette, [this] {
            palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
        });
    }
    void text() {
        require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool");
        palette.setActiveTool(ScreenshotToolPalette::Tool::Text);
    }
};
void wheel(SnowCanvasWidget& canvas, int delta) {
    const QPointF point(100, 100);
    QWheelEvent event(point, canvas.mapToGlobal(point.toPoint()), QPoint(), QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &event);
}
void fontWheelRemembersDefaultsAndDraftChoice() {
    Editor editor;
    editor.text();
    editor.canvas.show();
    const double initial = editor.canvas.canvasStyleToolbarState().textStyle.fontSize;
    wheel(editor.canvas, 120);
    require(screenshotCanvasToolStyleDefaults().text.fontSize == initial + 1,
            "canvas text wheel persists without an active draft");
    require(editor.palette.creationStyleDefaults().text.fontSize == initial + 1,
            "canvas text wheel updates palette creation defaults");
    const QPointF point(100, 100);
    QMouseEvent press(QEvent::MouseButtonPress, point, editor.canvas.mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor.canvas, &press);
    require(editor.canvas.hasActiveTextEditing(), "start a text draft");
    wheel(editor.canvas, 120);
    require(editor.canvas.hasActiveTextEditing(), "style commit preserves the active draft");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == initial + 2,
            "active draft wheel persists");
    require(editor.canvas.cancelActiveTextEditing(), "cancel draft");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == initial + 2,
            "canceling a draft retains the explicit preference");
    require(editor.canvas.canvasStyleToolbarState().textStyle.fontSize == initial + 2,
            "canceling a draft preserves this canvas's creation font size");
    Editor fresh;
    require(fresh.palette.creationStyleDefaults().text.fontSize == initial + 2,
            "new editor loads the remembered font size");
}
void propertyPatchesPreserveOtherEditorsAndProgrammaticState() {
    Editor first;
    Editor second;
    first.text();
    second.text();
    const auto initial = screenshotCanvasToolStyleDefaults();
    auto style = initial.text;
    style.color = QColor(17, 31, 53);
    require(first.canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedColor}),
            "commit first editor color");
    style = initial.text;
    style.fontSize = 47;
    require(
        second.canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedFontSize}),
        "commit second editor size");
    const auto saved = screenshotCanvasToolStyleDefaults();
    require(saved.text.fontSize == 47 && saved.text.color == QColor(17, 31, 53),
            "stale editor must not overwrite another editor's changed property");
    require(second.binding.lastSaveSucceeded() == true, "binding exposes persistence success");
    style.color = Qt::blue;
    require(second.canvas.setCanvasTextStyle(style), "programmatic style update");
    second.palette.setStyleToolbarState(second.canvas.canvasStyleToolbarState());
    second.canvas.previewCanvasWatermarkConfig(initial.watermark);
    second.canvas.previewCanvasSpotlightConfig(initial.spotlight);
    static_cast<void>(second.canvas.undo());
    static_cast<void>(second.canvas.redo());
    require(screenshotCanvasToolStyleDefaults() == saved,
            "refresh, previews, programmatic setters and history do not persist preferences");
    style.fontSize = std::numeric_limits<double>::quiet_NaN();
    require(
        !second.canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedFontSize}),
        "invalid style edit fails");
    second.canvas.setInteractionEnabled(false);
    wheel(second.canvas, 120);
    require(!second.canvas.commitStyleEdit(
                SnowCanvasTextEdit{saved.text, SnowCanvasTextStyleMixedColor}),
            "disabled interaction rejects user edits");
    require(screenshotCanvasToolStyleDefaults() == saved, "failed edits never persist");
}
void allStyleFamiliesPersistOnlyTheirPatch() {
    Editor editor;
    auto expected = screenshotCanvasToolStyleDefaults();
    auto verify = [&](SnowCanvasTool tool, const SnowCanvasStyleEdit& edit) {
        require(editor.canvas.setCanvasTool(tool), "activate family");
        require(editor.canvas.commitStyleEdit(edit), "apply family patch");
        snowCanvasMergeStyleEdit(expected, edit);
        require(screenshotCanvasToolStyleDefaults() == expected,
                "each style family saves only explicitly edited properties");
    };
    for (const auto kind :
         {SnowCanvasShapeKind::Rectangle, SnowCanvasShapeKind::Arrow, SnowCanvasShapeKind::Line,
          SnowCanvasShapeKind::FreeDraw, SnowCanvasShapeKind::RectangleHighlight,
          SnowCanvasShapeKind::PenHighlight}) {
        auto style = expected.rectangle;
        style.strokeWidth = 11;
        verify(SnowCanvasTool::Shape,
               SnowCanvasShapeEdit{style, SnowCanvasShapeStylePropertyStrokeWidth, kind});
    }
    auto serial = expected.serialNumber;
    serial.fontSize = 73;
    serial.number = 999;
    serial.color = Qt::cyan;
    verify(SnowCanvasTool::SerialNumber,
           SnowCanvasSerialNumberEdit{serial, SnowCanvasSerialNumberStyleMixedFontSize});
    auto filter = expected.rectangleFilter;
    filter.strength = 0.37;
    verify(SnowCanvasTool::RectangleFilter,
           SnowCanvasFilterEdit{filter, SnowCanvasFilterStylePropertyStrength, false});
    filter.strokeWidth = 13;
    verify(SnowCanvasTool::PenFilter,
           SnowCanvasFilterEdit{filter, SnowCanvasFilterStylePropertyStrokeWidth, true});
    auto watermark = expected.watermark;
    watermark.fontSize = 41;
    watermark.color = Qt::blue;
    verify(SnowCanvasTool::Watermark,
           SnowCanvasWatermarkEdit{watermark, SnowCanvasWatermarkFontSize});
    auto spotlight = expected.spotlight;
    spotlight.opacity = 0.37;
    verify(SnowCanvasTool::Spotlight,
           SnowCanvasSpotlightEdit{spotlight, SnowCanvasSpotlightOpacity});
}
void sharedScreenshotRuntimeKeepsDraftEditsTransient() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    SnowCanvasWidget peer(runtime);
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette, [&](const SnowCanvasStyleEdit& edit) {
        replicateScreenshotStyleEdit(peer, edit);
    });
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate shared-runtime text");
    const QPointF point(100, 100);
    QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("alpha"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.hasActiveTextEditing(), "shared-runtime draft stays active");
    const auto history = canvas.canvasHistoryState();
    wheel(canvas, 120);
    require(canvas.hasActiveTextEditing() &&
                canvas.canvasHistoryState().canUndo == history.canUndo &&
                canvas.canvasHistoryState().canRedo == history.canRedo,
            "replication must not commit a draft or add a document history entry");
    const double size = canvas.canvasStyleToolbarState().textStyle.fontSize;
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "commit the draft by leaving Text");
    const QPointF start(1, 1), end(399, 299);
    QMouseEvent selectPress(QEvent::MouseButtonPress, start, canvas.mapToGlobal(start.toPoint()),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent selectMove(QEvent::MouseMove, end, canvas.mapToGlobal(end.toPoint()), Qt::NoButton,
                           Qt::LeftButton, Qt::NoModifier);
    QMouseEvent selectRelease(QEvent::MouseButtonRelease, end, canvas.mapToGlobal(end.toPoint()),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &selectPress);
    QApplication::sendEvent(&canvas, &selectMove);
    QApplication::sendEvent(&canvas, &selectRelease);
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedText,
            "select the committed text for a property edit");
    const auto saved = screenshotCanvasToolStyleDefaults();
    require(canvas.stepFontSize(1), "selected text font step");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == size + 1 &&
                screenshotCanvasToolStyleDefaults().text.color == saved.text.color,
            "selected text step remembers only its explicit property");
    require(canvas.undo(), "undo selected text style");
    require(screenshotCanvasToolStyleDefaults().text.fontSize == size + 1,
            "undo leaves the preference unchanged");
    require(canvas.redo(), "redo selected text style");
    require(peer.canvasStyleToolbarState().textStyle.fontSize == size + 1,
            "shared screenshot viewports synchronize without a second edit");
}

void toolbarAndCanvasShareFontCommit() {
    Editor editor;
    editor.text();
    editor.palette.show();
    QApplication::processEvents();
    adqt::widgets::AdButton* preset = nullptr;
    for (auto* button : editor.palette.findChildren<adqt::widgets::AdButton*>()) {
        if (button->toolTip() == QStringLiteral("Text font size L (42px)"))
            preset = button;
    }
    require(preset != nullptr, "text size preset exists");
    preset->click();
    require(screenshotCanvasToolStyleDefaults().text.fontSize == 42 &&
                editor.canvas.canvasStyleToolbarState().textStyle.fontSize == 42,
            "toolbar preset applies and persists through the shared binding");
    wheel(editor.canvas, -120);
    require(screenshotCanvasToolStyleDefaults().text.fontSize == 41,
            "canvas wheel follows toolbar preset state");
    require(editor.canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number");
    auto serial = editor.canvas.canvasStyleToolbarState().serialNumberStyle;
    serial.fontSize = 511;
    require(editor.canvas.commitStyleEdit(
                SnowCanvasSerialNumberEdit{serial, SnowCanvasSerialNumberStyleMixedFontSize}),
            "set serial font near limit");
    wheel(editor.canvas, 120);
    wheel(editor.canvas, 120);
    wheel(editor.canvas, 0);
    require(screenshotCanvasToolStyleDefaults().serialNumber.fontSize == 512,
            "serial wheel uses the same upper limit as direct input");
}
} // namespace

void runScreenshotStyleBindingTests() {
    const auto original = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
    const auto restore = qScopeGuard([&] {
        static_cast<void>(snow_shot::presentation::persistScreenshotCanvasToolStyles(original));
    });
    fontWheelRemembersDefaultsAndDraftChoice();
    propertyPatchesPreserveOtherEditorsAndProgrammaticState();
    allStyleFamiliesPersistOnlyTheirPatch();
    toolbarAndCanvasShareFontCommit();
    sharedScreenshotRuntimeKeepsDraftEditsTransient();
}

void runScreenshotStylePersistenceFailureTest() {
    SnowCanvasWidget canvas;
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette, {},
                                   [](const SnowCanvasStyleEdit&) { return false; });
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text for failed save");
    auto style = canvas.canvasStyleToolbarState().textStyle;
    style.fontSize = 37;
    require(canvas.commitStyleEdit(SnowCanvasTextEdit{style, SnowCanvasTextStyleMixedFontSize}),
            "a storage failure does not invalidate a successful canvas edit");
    require(binding.lastSaveSucceeded() == false,
            "binding exposes the persistence dependency's failed save");
}
