#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QKeyEvent>
#include "widgets/button.h"
#include "widgets/input_line_edit.h"

#include <QApplication>
#include <QCursor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
QJsonArray documentSlots(const SnowCanvasRuntime& runtime) {
    return QJsonDocument::fromJson(runtime.serializeDocumentSession())
        .object()
        .value(QStringLiteral("document"))
        .toObject()
        .value(QStringLiteral("slots"))
        .toArray();
}
void arrowCreationDefaultsRestoreShaftAndRatio() {
    SnowCanvasWidget canvas;
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.arrow.arrowRatio = 2.7;
    defaults.arrow.startArrowhead = SnowCanvasArrowhead::Circle;
    defaults.arrow.endArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    for (const auto arrowType :
         {SnowCanvasArrowType::Straight, SnowCanvasArrowType::Curve, SnowCanvasArrowType::Elbow}) {
        defaults.arrow.arrowType = arrowType;
        for (const auto shaftType :
             {SnowCanvasArrowShaftType::Tapered, SnowCanvasArrowShaftType::Plain}) {
            defaults.arrow.arrowShaftType = shaftType;
            applyScreenshotCanvasToolStyles(canvas, defaults);
            require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "inspect restored arrow defaults");
            const auto style = canvas.canvasStyleToolbarState().shapeStyle;
            require(style.arrowShaftType == shaftType &&
                        style.arrowRatio == defaults.arrow.arrowRatio &&
                        style.arrowType == arrowType &&
                        style.startArrowhead == defaults.arrow.startArrowhead &&
                        style.endArrowhead == defaults.arrow.endArrowhead,
                    "arrow creation defaults must restore shaft and ratio independently of type "
                    "and arrowhead compatibility");
        }
    }
}
void creationDefaultsDoNotActivateFilterTools() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "prepare screenshot selection tool");
    const QCursor cursor = canvas.cursor();
    const auto viewport = canvas.viewportId();
    const auto revision = runtime.documentRevision();
    int activeToolChanges = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::activeToolChanged, &canvas,
                     [&] { ++activeToolChanges; });
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.rectangleFilter = {SnowCanvasFilterType::GaussianBlur, 0.37, 0.61, 3};
    defaults.penFilter = {SnowCanvasFilterType::Brightness, 0.37, 0.83, 21};
    for (int capture = 0; capture < 8; ++capture) {
        applyScreenshotCanvasToolStyles(canvas, defaults);
        require(activeToolChanges == 0 && canvas.canvasTool() == SnowCanvasTool::Select &&
                    canvas.cursor() == cursor,
                "repeated screenshot defaults must not activate tools or allocate brush cursors");
        require(canvas.viewportId() == viewport && runtime.documentRevision() == revision &&
                    !runtime.canUndo(),
                "creation defaults must preserve the viewport and empty document history");
    }
    require(canvas.setCanvasTool(SnowCanvasTool::RectangleFilter), "inspect rectangle defaults");
    require(canvas.canvasStyleToolbarState().filterStyle == defaults.rectangleFilter,
            "rectangle filter creation style must apply without activating it during setup");
    require(canvas.setCanvasTool(SnowCanvasTool::PenFilter), "inspect pen defaults");
    require(canvas.canvasStyleToolbarState().filterStyle == defaults.penFilter,
            "pen filter creation style must apply independently of rectangle defaults");
}
void creationDefaultsPreserveDocumentAndEditingCleanup() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(160, 120, 1), "set style restoration fixture camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "draw style restoration fixture");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(40, 40));
    mouse(canvas, QEvent::MouseMove, QPointF(140, 100));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(140, 100));
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select style restoration fixture");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(40, 60));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(40, 60));
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "fixture rectangle must be selected before defaults refresh");
    const auto originalSlots = documentSlots(runtime);
    const auto history = runtime.serializeDocumentHistory();
    const auto revision = runtime.documentRevision();
    require(!originalSlots.isEmpty(), "style restoration fixture must contain a document element");
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.rectangle.stroke = QColor(19, 47, 89);
    defaults.rectangle.strokeWidth = 12;
    applyScreenshotCanvasToolStyles(canvas, defaults);
    require(documentSlots(runtime) == originalSlots &&
                runtime.serializeDocumentHistory() == history &&
                runtime.documentRevision() == revision,
            "refreshing creation styles must never rewrite selected elements or their history");
    require(canvas.canvasStyleToolbarState().source !=
                SnowCanvasStyleToolbarSource::SelectedRectangle,
            "creation style restoration retains its existing selection cleanup");
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "start pending text before style refresh");
    mouse(canvas, QEvent::MouseButtonPress, QPointF(180, 150));
    QKeyEvent type(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("pending"));
    QApplication::sendEvent(&canvas, &type);
    require(canvas.hasActiveTextEditing(), "style restoration fixture must have pending text");
    QList<SnowCanvasTool> observedTools;
    QObject::connect(&canvas, &SnowCanvasWidget::activeToolChanged, &canvas,
                     [&] { observedTools.append(canvas.canvasTool()); });
    applyScreenshotCanvasToolStyles(canvas, defaults);
    require(!canvas.hasActiveTextEditing() && canvas.canvasTool() == SnowCanvasTool::Text &&
                canvas.cursor().shape() == Qt::IBeamCursor &&
                documentSlots(runtime) != originalSlots,
            "creation style restoration must commit pending text and restore its active tool");
    require(!observedTools.contains(SnowCanvasTool::RectangleFilter) &&
                !observedTools.contains(SnowCanvasTool::PenFilter),
            "restoring a non-filter tool must not transiently activate either filter");
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
    serial.numericType = SnowCanvasSerialNumberNumericType::Roman;
    verify(SnowCanvasTool::SerialNumber,
           SnowCanvasSerialNumberEdit{serial, SnowCanvasSerialNumberStyleMixedNumericType});
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
    auto spotlightStyle = expected.rectangle;
    spotlightStyle.shape = SnowCanvasRectangleShape::Diamond;
    verify(SnowCanvasTool::Spotlight,
           SnowCanvasShapeEdit{spotlightStyle, SnowCanvasShapeStylePropertyShape,
                               SnowCanvasShapeKind::Spotlight});
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
    arrowCreationDefaultsRestoreShaftAndRatio();
    creationDefaultsDoNotActivateFilterTools();
    creationDefaultsPreserveDocumentAndEditingCleanup();
    fontWheelRemembersDefaultsAndDraftChoice();
    propertyPatchesPreserveOtherEditorsAndProgrammaticState();
    allStyleFamiliesPersistOnlyTheirPatch();
    toolbarAndCanvasShareFontCommit();
    sharedScreenshotRuntimeKeepsDraftEditsTransient();
}

void runScreenshotSerialNumberRestartTests() {
    const auto original = screenshotCanvasToolStyleDefaults();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(persistScreenshotCanvasToolStyles(original)); });
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    ScreenshotToolPalette palette(options());
    ScreenshotStyleBinding binding(palette, canvas, &palette);
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    canvas.resize(400, 300);
    canvas.show();
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial number tool");
    auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
    style.type = SnowCanvasSerialNumberType::OutlinedCircle;
    style.numericType = SnowCanvasSerialNumberNumericType::Arabic;
    style.fontSize = 24;
    style.number = 50;
    require(canvas.setCanvasSerialNumberStyle(style), "start with a high canvas number");
    const auto create = [&](QPointF point) {
        mouse(canvas, QEvent::MouseButtonPress, point);
        mouse(canvas, QEvent::MouseButtonRelease, point);
        return documentSlots(runtime)
            .last()
            .toObject()
            .value(QStringLiteral("data"))
            .toObject()
            .value(QStringLiteral("SerialNumber"))
            .toObject()
            .value(QStringLiteral("number"))
            .toVariant()
            .toLongLong();
    };
    require(create({40, 150}) == 50, "create the existing high sequence number");
    palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
    palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
    palette.show();
    QApplication::processEvents();
    adqt::widgets::AdLineEdit* input = nullptr;
    for (auto* candidate : palette.findChildren<adqt::widgets::AdLineEdit*>()) {
        if (candidate->toolTip() == QStringLiteral("Sequence number (scroll to adjust)")) {
            input = candidate;
            break;
        }
    }
    require(input != nullptr && input->text() == QStringLiteral("51"),
            "style editor displays the next number after the high element");
    for (const int start : {2, 0}) {
        input->setText(QString::number(start));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(input, &enter);
        require(canvas.canvasStyleToolbarState().serialNumberStyle.number == start,
                "style editor commits a value below the maximum canvas number");
        for (int offset = 0; offset < 3; ++offset) {
            require(create({120.0 + offset * 100.0, 80.0 + start * 50.0}) == start + offset,
                    "canvas creates the entered smaller number and increments from it");
            require(input->text() == QString::number(start + offset + 1),
                    "style editor follows the restarted sequence");
        }
    }
    require(documentSlots(runtime)
                    .first()
                    .toObject()
                    .value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("SerialNumber"))
                    .toObject()
                    .value(QStringLiteral("number"))
                    .toInt() == 50,
            "restarting numbering preserves the existing high element");
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
