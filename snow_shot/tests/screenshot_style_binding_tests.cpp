#include "snow_shot/presentation/screenshotstylebinding.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include <QKeyEvent>
#include "widgets/button.h"
#include "widgets/input_line_edit.h"
#include "widgets/select.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScopeGuard>
#include <QTimeZone>
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
void serialNumberAppearanceDefaultsPreserveSessionSequences() {
    const SnowCanvasSerialNumberNumericType types[] = {
        SnowCanvasSerialNumberNumericType::Arabic,
        SnowCanvasSerialNumberNumericType::Roman,
        SnowCanvasSerialNumberNumericType::LowercaseLetters,
        SnowCanvasSerialNumberNumericType::UppercaseLetters,
        SnowCanvasSerialNumberNumericType::Chinese,
    };
    for (const bool explicitStart : {false, true}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(800, 600);
        canvas.show();
        auto defaults = screenshotCanvasToolStyleDefaults();
        defaults.serialNumber.type = SnowCanvasSerialNumberType::OutlinedCircle;
        defaults.serialNumber.numericType = SnowCanvasSerialNumberNumericType::Arabic;
        defaults.serialNumber.number = 1;
        defaults.serialNumber.fontSize = 24;
        applyScreenshotCanvasToolStyles(canvas, defaults);
        require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "activate serial creation");
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
        for (int index = 0; index < 5; ++index) {
            auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
            style.numericType = types[index];
            style.number = 10 + index;
            require(canvas.applyStyleEdit(SnowCanvasSerialNumberEdit{
                        style, SnowCanvasSerialNumberStyleMixedNumericType |
                                   (explicitStart ? SnowCanvasSerialNumberStyleMixedNumber : 0)}),
                    "prepare independent automatic or explicit sequences");
            const qint64 start = explicitStart ? 10 + index : 1;
            require(create({80.0 + index * 140.0, 100}) == start &&
                        create({80.0 + index * 140.0, 250}) == start + 1,
                    "each numeric type creates two sequential badges");
        }
        for (int index = 0; index < 5; ++index) {
            const auto annotationsBeforeRefresh = documentSlots(runtime);
            const auto history = runtime.serializeDocumentHistory();
            const auto revision = runtime.documentRevision();
            defaults.serialNumber.numericType = types[index];
            defaults.serialNumber.color = QColor(19, 47, 89);
            defaults.serialNumber.fontSize = 32;
            applyScreenshotCanvasToolStyles(canvas, defaults);
            auto expected = defaults.serialNumber;
            expected.number = explicitStart ? 12 + index : 3;
            require(canvas.canvasTool() == SnowCanvasTool::SerialNumber &&
                        canvas.canvasStyleToolbarState().serialNumberStyle == expected,
                    "reopening creation appearance restores the format's session counter");
            require(documentSlots(runtime) == annotationsBeforeRefresh &&
                        runtime.serializeDocumentHistory() == history &&
                        runtime.documentRevision() == revision,
                    "appearance refresh preserves all existing annotations and history");
            require(create({80.0 + index * 140.0, 400}) == expected.number,
                    "creation continues after appearance refresh without duplicate numbers");
            require(runtime.undo(), "undo creation after reopening appearance");
            require(canvas.canvasStyleToolbarState().serialNumberStyle.number ==
                        expected.number + (explicitStart ? 1 : 0),
                    "appearance refresh preserves automatic and explicit history behavior");
            require(runtime.redo() && canvas.canvasStyleToolbarState().serialNumberStyle.number ==
                                          expected.number + 1,
                    "redo restores the next number without changing sequence ownership");
        }
    }
}
void newScreenshotDocumentRestartsSerialNumberSequences() {
    const SnowCanvasSerialNumberNumericType types[] = {
        SnowCanvasSerialNumberNumericType::Arabic,
        SnowCanvasSerialNumberNumericType::Roman,
        SnowCanvasSerialNumberNumericType::LowercaseLetters,
        SnowCanvasSerialNumberNumericType::UppercaseLetters,
        SnowCanvasSerialNumberNumericType::Chinese,
    };
    for (const bool explicitStart : {false, true}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        ScreenshotToolPalette palette(options());
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                         [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
        canvas.resize(800, 600);
        canvas.show();
        auto defaults = screenshotCanvasToolStyleDefaults();
        defaults.serialNumber.type = SnowCanvasSerialNumberType::OutlinedCircle;
        defaults.serialNumber.fontSize = 32;
        applyScreenshotCanvasToolStyles(canvas, defaults);
        require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                "activate screenshot numbering");
        palette.setActiveTool(ScreenshotToolPalette::Tool::SerialNumber);
        const auto viewport = canvas.viewportId();
        for (int capture = 0; capture < 3; ++capture) {
            if (capture > 0) {
                // Capture teardown reuses the runtime and its attached canvas.
                require(runtime.clearDocumentPreservingViewports(), "clear completed capture");
                applyScreenshotCanvasToolStyles(canvas, defaults);
                require(canvas.viewportId() == viewport && documentSlots(runtime).isEmpty() &&
                            !runtime.canUndo() && !runtime.canRedo(),
                        "new capture reuses its viewport with an empty document and history");
            }
            require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber),
                    "reactivate numbering in the new capture");
            for (int index = 0; index < 5; ++index) {
                auto style = canvas.canvasStyleToolbarState().serialNumberStyle;
                style.numericType = types[index];
                style.number = 10 + index;
                require(canvas.applyStyleEdit(SnowCanvasSerialNumberEdit{
                            style, SnowCanvasSerialNumberStyleMixedNumericType |
                                       (capture == 0 && explicitStart
                                            ? SnowCanvasSerialNumberStyleMixedNumber
                                            : 0)}),
                        "switch screenshot numbering format");
                auto expected = defaults.serialNumber;
                expected.numericType = types[index];
                expected.number = capture == 0 && explicitStart ? style.number : 1;
                require(
                    canvas.canvasStyleToolbarState().serialNumberStyle == expected &&
                        palette.creationStyleDefaults().serialNumber == expected,
                    "new capture resets every format and its toolbar while retaining appearance");
                const QPointF point(80.0 + index * 140.0, 100);
                mouse(canvas, QEvent::MouseButtonPress, point);
                mouse(canvas, QEvent::MouseButtonRelease, point);
                const qint64 created = documentSlots(runtime)
                                           .last()
                                           .toObject()
                                           .value(QStringLiteral("data"))
                                           .toObject()
                                           .value(QStringLiteral("SerialNumber"))
                                           .toObject()
                                           .value(QStringLiteral("number"))
                                           .toVariant()
                                           .toLongLong();
                require(created == expected.number,
                        "new capture creates a badge at its reset value");
                if (capture > 0) {
                    require(runtime.undo() &&
                                canvas.canvasStyleToolbarState().serialNumberStyle.number == 1,
                            "new capture discards the old manual start override");
                    require(runtime.redo() &&
                                canvas.canvasStyleToolbarState().serialNumberStyle.number == 2,
                            "new capture resumes automatic numbering after redo");
                }
            }
        }
    }
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
ScreenshotToolPalette::Options watermarkOptions(std::function<QDateTime()> clock) {
    auto result = options();
    result.watermarkTemplateClock = clock ? std::move(clock) : [] {
        return QDateTime(QDate(2026, 10, 9), QTime(12, 34, 56), QTimeZone::LocalTime);
    };
    return result;
}

struct WatermarkEditor {
    SnowCanvasRuntime runtime{SnowCanvasRuntimeConfig{screenshotCanvasToolStyleDefaults()}};
    SnowCanvasWidget canvas{runtime};
    ScreenshotToolPalette palette;
    ScreenshotStyleBinding binding;

    explicit WatermarkEditor(std::function<QDateTime()> clock = {})
        : palette(watermarkOptions(std::move(clock))), binding(palette, canvas, &palette) {
        canvas.resize(400, 300);
        canvas.setInteractionEnabled(true);
        QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette, [this] {
            palette.setStyleToolbarState(canvas.canvasStyleToolbarState());
            palette.setWatermarkConfig(canvas.canvasWatermarkConfig());
        });
        QObject::connect(&palette, &ScreenshotToolPalette::watermarkRequested, &canvas, [this] {
            require(canvas.setCanvasTool(SnowCanvasTool::Watermark), "activate watermark canvas");
        });
        QObject::connect(&palette, &ScreenshotToolPalette::selectRequested, &canvas, [this] {
            require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate selection canvas");
        });
    }

    void activate() {
        require(palette.activateToolShortcut(ScreenshotToolPalette::Tool::Watermark),
                "watermark toolbar activation succeeds");
    }

    void reopen() {
        require(palette.activateToolShortcut(ScreenshotToolPalette::Tool::Select),
                "switch away from watermark");
        activate();
    }

    QLineEdit* text() {
        auto* editor = palette.findChild<QLineEdit*>(QStringLiteral("screenshotWatermarkTextEdit"));
        require(editor != nullptr, "watermark text editor exists");
        return editor;
    }

    QLineEdit* templateText() {
        auto* select = palette.findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("screenshotWatermarkTemplateSelect"));
        require(select != nullptr && select->lineEdit() != nullptr,
                "watermark template editor exists");
        return select->lineEdit();
    }

    void editTemplate(const QString& value) {
        auto* editor = templateText();
        editor->setText(value);
        emit editor->textEdited(value);
    }
};

void watermarkContentEditsPersistOnlyCommittedFields() {
    const snow_shot::storage::WatermarkContentSettings settings;
    require(settings.setContent({}), "start watermark edits without saved content");
    WatermarkEditor editor;
    editor.activate();
    editor.text()->setText(QStringLiteral("  版权所有 © 雪  "));
    const QString text = QStringLiteral("版权所有 © 雪");
    const QString templateValue = QStringLiteral("  {text} / {YYYY-MM-DD_HH-mm-ss}  ");
    editor.editTemplate(templateValue);
    require(editor.binding.lastSaveSucceeded() == true &&
                settings.content() == snow_shot::storage::WatermarkContent{text, templateValue} &&
                editor.canvas.canvasWatermarkConfig().text == text &&
                editor.canvas.canvasWatermarkConfig().templateValue == templateValue,
            "successful editor commits save trimmed text and exact manual template whitespace");
    const auto stored = snow_shot::storage::ApplicationStorage::instance()
                            .configuration()
                            .value(QStringLiteral("drawing/watermark_content"))
                            .toObject();
    require(stored.size() == 2 && !stored.contains(QStringLiteral("template_application_time")),
            "content preferences never save application timestamps");
    auto stale = editor.canvas.canvasWatermarkConfig();
    stale.text = QStringLiteral("stale text");
    stale.templateValue = QStringLiteral("stale template");
    stale.fontSize = 37;
    require(editor.canvas.commitStyleEdit(
                SnowCanvasWatermarkEdit{stale, SnowCanvasWatermarkFontSize}) &&
                settings.content() == snow_shot::storage::WatermarkContent{text, templateValue},
            "appearance-only commits preserve saved text and template");
    stale.text = QStringLiteral("Updated");
    require(
        editor.canvas.commitStyleEdit(SnowCanvasWatermarkEdit{stale, SnowCanvasWatermarkText}) &&
            settings.content() == snow_shot::storage::WatermarkContent{stale.text, templateValue},
        "text-only patches preserve the latest template rather than a stale editor value");
    editor.editTemplate(QString());
    require(settings.content() == snow_shot::storage::WatermarkContent{stale.text, QString()},
            "clearing the template preserves saved text");
    WatermarkEditor plain;
    plain.activate();
    require(plain.canvas.canvasWatermarkConfig().text == stale.text &&
                plain.canvas.canvasWatermarkConfig().templateValue.isEmpty() &&
                !plain.canvas.canvasWatermarkConfig().templateApplicationTime.has_value(),
            "plain text restores without a template or application time");
    plain.text()->clear();
    require(settings.content() == snow_shot::storage::WatermarkContent{},
            "clearing text persists empty content");
    plain.reopen();
    require(plain.canvas.canvasWatermarkConfig().text.isEmpty(),
            "reopening a cleared watermark does not resurrect previous text");
    WatermarkEditor empty;
    empty.activate();
    require(!empty.runtime.hasDocumentContent() && !empty.runtime.canUndo(),
            "empty saved content creates no watermark or history entry");
    empty.editTemplate(QStringLiteral("{YYYY}"));
    WatermarkEditor templateOnly;
    templateOnly.activate();
    require(templateOnly.canvas.canvasWatermarkConfig().text.isEmpty() &&
                templateOnly.canvas.canvasWatermarkConfig().templateValue ==
                    QStringLiteral("{YYYY}") &&
                templateOnly.canvas.canvasWatermarkConfig().templateApplicationTime ==
                    std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
                        {2026, 10, 9, 12, 34, 56}},
            "a timestamp-only template restores even with empty watermark text");
}

void rememberedWatermarkAppliesOncePerCaptureWithFreshTime() {
    const snow_shot::storage::WatermarkContentSettings settings;
    const snow_shot::storage::WatermarkContent saved{
        QStringLiteral("CONFIDENTIAL"), QStringLiteral("  {text} {YYYY-MM-DD_HH-mm-ss}  ")};
    require(settings.setContent(saved), "prepare remembered watermark content");
    int clockCalls = 0;
    WatermarkEditor editor([&clockCalls] {
        return QDateTime(QDate(2026, 10, 9), QTime(12, 34, 10 + clockCalls++),
                         QTimeZone::LocalTime);
    });
    const QRectF bounds(0, 0, 400, 300);
    QImage base(400, 300, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::white);
    const QList<CanvasExportSource> sources{{base, bounds}};
    const QImage before = editor.runtime.renderToImage(bounds, base.size(), sources);
    require(!before.isNull() && !editor.runtime.hasDocumentContent() && !editor.runtime.canUndo() &&
                editor.canvas.canvasWatermarkConfig().text.isEmpty() && clockCalls == 0,
            "remembered content does not apply during canvas construction");
    editor.activate();
    const auto first = editor.canvas.canvasWatermarkConfig();
    require(
        first.text == saved.text && first.templateValue == saved.templateValue &&
            first.templateApplicationTime ==
                std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
                    {2026, 10, 9, 12, 34, 10}} &&
            editor.text()->text() == saved.text &&
            editor.templateText()->text() == saved.templateValue && clockCalls == 1,
        "activation populates both editors and applies remembered content with the current time");
    const QImage after = editor.runtime.renderToImage(bounds, base.size(), sources);
    require(!after.isNull() && after != before && editor.runtime.hasDocumentContent(),
            "the remembered watermark is rendered on the capture after activation");
    editor.reopen();
    require(editor.canvas.canvasWatermarkConfig() == first && clockCalls == 1 &&
                editor.templateText()->text() == saved.templateValue,
            "tool switching preserves watermark content and its application time");
    require(editor.canvas.undo() && !editor.runtime.hasDocumentContent() &&
                !editor.runtime.canUndo(),
            "restored text and template undo together as one edit");
    editor.reopen();
    require(!editor.runtime.hasDocumentContent() && editor.runtime.canRedo() && clockCalls == 1 &&
                settings.content() == saved,
            "undo and reopening do not reapply saved content or overwrite preferences");
    require(editor.canvas.redo() && editor.canvas.canvasWatermarkConfig() == first,
            "redo restores the original application timestamp");
    require(editor.canvas.clearDocument(), "start a new capture using the same canvas");
    editor.palette.resetStyleState();
    editor.palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    editor.activate();
    require(clockCalls == 2 && editor.canvas.canvasWatermarkConfig().text == saved.text &&
                editor.canvas.canvasWatermarkConfig().templateApplicationTime ==
                    std::optional<SnowCanvasWatermarkTemplateApplicationTime>{
                        {2026, 10, 9, 12, 34, 11}},
            "a new capture rearms restoration and takes a fresh timestamp");
    WatermarkEditor existing([&clockCalls] {
        ++clockCalls;
        return QDateTime(QDate(2026, 10, 9), QTime(12, 34, 56), QTimeZone::LocalTime);
    });
    auto document = first;
    document.text = QStringLiteral("Existing document");
    document.templateValue = QStringLiteral("{text} {YYYY}");
    document.templateApplicationTime =
        SnowCanvasWatermarkTemplateApplicationTime{2024, 2, 29, 1, 2, 3};
    require(existing.canvas.setCanvasWatermarkConfig(document),
            "load an existing document watermark");
    existing.activate();
    require(
        existing.canvas.canvasWatermarkConfig() == document &&
            existing.text()->text() == document.text &&
            existing.templateText()->text() == document.templateValue && clockCalls == 2 &&
            settings.content() == saved,
        "existing document content and timestamps take precedence without changing preferences");
    require(existing.canvas.undo(), "undo the loaded document watermark");
    existing.reopen();
    require(!existing.runtime.hasDocumentContent() && clockCalls == 2,
            "an existing document's empty undo state does not trigger preference restoration");
}

void rejectedWatermarkRestorationCanRetryWithoutSaving() {
    const snow_shot::storage::WatermarkContentSettings settings;
    const snow_shot::storage::WatermarkContent saved{QStringLiteral("Saved"),
                                                     QStringLiteral("{text} {YYYY}")};
    require(settings.setContent(saved), "prepare content for rejected restoration");
    WatermarkEditor editor;
    editor.palette.setStyleEditHandler([](const SnowCanvasStyleEdit&) { return false; });
    editor.activate();
    require(!editor.runtime.hasDocumentContent() &&
                !editor.binding.lastSaveSucceeded().has_value() && settings.content() == saved,
            "a rejected restoration neither changes the document nor persists content");
    editor.palette.setStyleEditHandler(
        [&editor](const SnowCanvasStyleEdit& edit) { return editor.canvas.commitStyleEdit(edit); });
    editor.reopen();
    require(editor.canvas.canvasWatermarkConfig().text == saved.text &&
                editor.canvas.canvasWatermarkConfig().templateValue == saved.templateValue &&
                editor.binding.lastSaveSucceeded() == true,
            "rejected restoration remains pending and succeeds on the next activation");
    const auto applied = editor.canvas.canvasWatermarkConfig();
    editor.palette.setStyleEditHandler([&editor](const SnowCanvasStyleEdit&) {
        editor.palette.setWatermarkConfig(editor.canvas.canvasWatermarkConfig());
        return false;
    });
    editor.text()->setText(QStringLiteral("Rejected"));
    require(editor.canvas.canvasWatermarkConfig() == applied && settings.content() == saved &&
                editor.text()->text() == applied.text,
            "rejected text edits preserve the document, saved content, and synchronized editor");
}
} // namespace

void runWatermarkPersistenceTests() {
    const snow_shot::storage::WatermarkContentSettings settings;
    const auto originalContent = settings.content();
    const auto originalStyles = screenshotCanvasToolStyleDefaults();
    const auto restore = qScopeGuard([&] {
        static_cast<void>(settings.setContent(originalContent));
        static_cast<void>(persistScreenshotCanvasToolStyles(originalStyles));
    });
    watermarkContentEditsPersistOnlyCommittedFields();
    rememberedWatermarkAppliesOncePerCaptureWithFreshTime();
    rejectedWatermarkRestorationCanRetryWithoutSaving();
}

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
    runWatermarkPersistenceTests();
}

void runScreenshotSerialNumberRestartTests() {
    serialNumberAppearanceDefaultsPreserveSessionSequences();
    newScreenshotDocumentRestartsSerialNumberSequences();
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

    const SnowCanvasSerialNumberNumericType types[] = {
        SnowCanvasSerialNumberNumericType::Arabic,
        SnowCanvasSerialNumberNumericType::Roman,
        SnowCanvasSerialNumberNumericType::Chinese,
        SnowCanvasSerialNumberNumericType::LowercaseLetters,
        SnowCanvasSerialNumberNumericType::UppercaseLetters,
    };
    qint64 nextNumbers[] = {11, 12, 13, 14, 15};
    const auto numericEditorText = [&] {
        for (auto* candidate : palette.findChildren<adqt::widgets::AdLineEdit*>()) {
            if (candidate->toolTip() == QStringLiteral("Sequence number (scroll to adjust)")) {
                return candidate->text();
            }
        }
        require(false, "the active serial number editor exists after a canvas state change");
        return QString();
    };
    const auto activateNumericType = [&](int index, bool setNumber) {
        auto nextStyle = canvas.canvasStyleToolbarState().serialNumberStyle;
        nextStyle.numericType = types[index];
        nextStyle.number = nextNumbers[index];
        require(canvas.commitStyleEdit(SnowCanvasSerialNumberEdit{
                    nextStyle, SnowCanvasSerialNumberStyleMixedNumericType |
                                   (setNumber ? SnowCanvasSerialNumberStyleMixedNumber : 0)}),
                "activate the numeric type's independent creation sequence");
    };
    const auto rightClick = [&](QPointF point) {
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas.mapToGlobal(point.toPoint()),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, canvas.mapToGlobal(point.toPoint()),
                            Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &press);
        QApplication::sendEvent(&canvas, &release);
    };
    for (int index = 0; index < 5; ++index) {
        activateNumericType(index, true);
    }
    for (int index = 0; index < 5; ++index) {
        activateNumericType(index, false);
        auto expectedStyle = canvas.canvasStyleToolbarState().serialNumberStyle;
        expectedStyle.number = 1;
        const auto elementsBeforeReset = documentSlots(runtime);
        const auto history = runtime.serializeDocumentHistory();
        const auto revision = runtime.documentRevision();
        if (index == 0) {
            rightClick({40, 150});
            require(canvas.canvasStyleToolbarState().source ==
                            SnowCanvasStyleToolbarSource::SelectedSerialNumber &&
                        numericEditorText() == QStringLiteral("50"),
                    "right clicking an existing serial number selects it without resetting it");
        }
        const QPointF blank(390, 290);
        rightClick(blank);
        require(canvas.canvasStyleToolbarState().source ==
                        SnowCanvasStyleToolbarSource::DefaultSerialNumber &&
                    canvas.canvasStyleToolbarState().serialNumberStyle == expectedStyle &&
                    palette.creationStyleDefaults().serialNumber == expectedStyle &&
                    numericEditorText() == QStringLiteral("1"),
                "blank canvas right click resets the active numeric editor and creation value");
        require(documentSlots(runtime) == elementsBeforeReset &&
                    runtime.serializeDocumentHistory() == history &&
                    runtime.documentRevision() == revision,
                "right click reset leaves existing annotations and undo history unchanged");
        QContextMenuEvent menu(QContextMenuEvent::Mouse, blank.toPoint(),
                               canvas.mapToGlobal(blank.toPoint()));
        menu.setAccepted(false);
        QApplication::sendEvent(&canvas, &menu);
        require(menu.isAccepted(), "serial number reset consumes the mouse context menu");
        rightClick(blank);
        require(numericEditorText() == QStringLiteral("1"),
                "repeated blank right click stays at one");
        require(create({40.0 + index * 75.0, 260}) == 1 &&
                    numericEditorText() == QStringLiteral("2"),
                "the next annotation starts at one and advances the numeric editor");
        nextNumbers[index] = 2;
        for (int other = 0; other < 5; ++other) {
            activateNumericType(other, false);
            require(numericEditorText() == QString::number(nextNumbers[other]),
                    "right click reset preserves the other numeric types' next values");
        }
    }
    activateNumericType(0, true);
    style = canvas.canvasStyleToolbarState().serialNumberStyle;
    style.number = 9;
    require(canvas.commitStyleEdit(
                SnowCanvasSerialNumberEdit{style, SnowCanvasSerialNumberStyleMixedNumber}),
            "prepare a serial number before disabling interaction");
    canvas.setInteractionEnabled(false);
    rightClick({390, 290});
    require(numericEditorText() == QStringLiteral("9"),
            "disabled canvas interaction does not reset the numeric editor");
    canvas.setInteractionEnabled(true);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "leave the serial number tool");
    rightClick({390, 290});
    require(canvas.setCanvasTool(SnowCanvasTool::SerialNumber), "return to the serial number tool");
    require(numericEditorText() == QStringLiteral("9"),
            "blank right click with another active tool preserves the serial number");
    rightClick({40, 260});
    require(canvas.createSerialNumberText() && canvas.hasActiveTextEditing(),
            "edit a serial number's attached text before resetting its sequence");
    QKeyEvent label(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("Kept label"));
    QApplication::sendEvent(&canvas, &label);
    rightClick({390, 290});
    require(!canvas.hasActiveTextEditing() && numericEditorText() == QStringLiteral("1") &&
                runtime.serializeDocumentSession().contains("Kept label"),
            "blank right click commits an active serial number label and resets the same press");
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

void runAngleStyleBindingTests() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    auto defaults = screenshotCanvasToolStyleDefaults();
    defaults.angle.decimalPlaces = 1;
    auto settings = options();
    settings.showAngleTool = true;
    settings.styleDefaults = defaults;
    ScreenshotToolPalette palette(settings);
    int saves = 0;
    ScreenshotStyleBinding binding(palette, canvas, &palette, {},
                                   [&saves](const SnowCanvasStyleEdit&) {
                                       ++saves;
                                       return true;
                                   });
    applyScreenshotCanvasToolStyles(canvas, defaults);
    canvas.resize(400, 300);
    canvas.setInteractionEnabled(true);
    canvas.show();
    QApplication::processEvents();
    QObject::connect(&canvas, &SnowCanvasWidget::styleToolbarStateChanged, &palette,
                     [&] { palette.setStyleToolbarState(canvas.canvasStyleToolbarState()); });
    require(canvas.setCanvasTool(SnowCanvasTool::Angle), "activate angle style binding fixture");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Angle);
    const auto click = [&](QPointF point) {
        mouse(canvas, QEvent::MouseButtonPress, point);
        mouse(canvas, QEvent::MouseButtonRelease, point);
    };
    const auto label = [&] {
        for (const auto& slot : documentSlots(runtime)) {
            const auto text = slot.toObject()
                                  .value(QStringLiteral("data"))
                                  .toObject()
                                  .value(QStringLiteral("Text"))
                                  .toObject();
            if (!text.isEmpty())
                return text.value(QStringLiteral("text")).toString();
        }
        return QString();
    };
    const auto sourceWidth = [&] {
        for (const auto& slot : documentSlots(runtime)) {
            const auto arrow = slot.toObject()
                                   .value(QStringLiteral("data"))
                                   .toObject()
                                   .value(QStringLiteral("Arrow"))
                                   .toObject();
            if (!arrow.isEmpty())
                return arrow.value(QStringLiteral("stroke_width")).toDouble();
        }
        return 0.0;
    };
    click({300, 150});
    click({180, 150});
    click({180, 40});
    require(label() == QStringLiteral("90.0°"), "angle fixture has a generated measured label");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select angle through selection tool");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    click({260, 150});
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedAngle,
            "selected angle exposes its dedicated style editor");
    WheelStepAccumulator accumulator;
    const auto hostWheel = [&](int delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                               Qt::ScrollPhase phase = Qt::NoScrollPhase) {
        QWheelEvent event({260, 150}, {260, 150}, {}, {0, delta}, Qt::NoButton, modifiers, phase,
                          false);
        event.setTimestamp(100);
        return handleScreenshotStyleWheel(palette, canvas, event, accumulator);
    };
    require(hostWheel(240) && label() == QStringLiteral("92.0°") && sourceWidth() == 2,
            "host wheel applies all coalesced angle steps ahead of selection opacity");
    require(hostWheel(120, Qt::ShiftModifier) && label() == QStringLiteral("92.1°"),
            "shift wheel uses a tenth-degree increment");
    const auto beforeControl = runtime.serializeDocumentSession();
    require(!hostWheel(120, Qt::ControlModifier) &&
                runtime.serializeDocumentSession() == beforeControl,
            "control wheel retains host navigation without editing the angle");
    require(hostWheel(120, Qt::NoModifier, Qt::ScrollMomentum) &&
                runtime.serializeDocumentSession() == beforeControl,
            "momentum consumes without changing discrete angle values");
    palette.angleValueAdjustmentRequested(-1, true);
    require(label() == QStringLiteral("92.0°"), "palette angle wheel signal reaches the canvas");
    auto selected = canvas.canvasAngleStyle();
    selected.strokeWidth = 7;
    selected.unit = SnowCanvasAngleUnit::Radians;
    require(canvas.commitStyleEdit(SnowCanvasAngleStyleEdit{
                selected, static_cast<quint32>(SnowCanvasAngleStyleProperty::All), false}) &&
                sourceWidth() == 7 && saves == 0 &&
                palette.creationStyleDefaults().angle == defaults.angle,
            "selected angle styling changes only the document and never saves creation defaults");
    require(canvas.resetEditingState() && canvas.setCanvasTool(SnowCanvasTool::Angle) &&
                canvas.canvasAngleStyle() == defaults.angle,
            "selected styling preserves future angle defaults");
    selected = defaults.angle;
    selected.strokeWidth = 4;
    require(canvas.commitStyleEdit(SnowCanvasAngleStyleEdit{
                selected, static_cast<quint32>(SnowCanvasAngleStyleProperty::StrokeWidth)}) &&
                saves == 1 && palette.creationStyleDefaults().angle.strokeWidth == 4,
            "creation style commits are remembered and saved once");

    click({150, 270});
    click({30, 270});
    click({30, 180});
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select among multiple angles");
    palette.setActiveTool(ScreenshotToolPalette::Tool::Select);
    click({260, 150});
    const auto palettePixelWheel = [&] {
        QWheelEvent event({100, 30}, {100, 30}, {0, 1}, {}, Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
        QApplication::sendEvent(&palette, &event);
    };
    const auto firstSelection = runtime.serializeDocumentHistory();
    palettePixelWheel();
    const auto firstStep = runtime.serializeDocumentHistory();
    require(firstStep != firstSelection,
            "selected angle responds to the first precise wheel delta");
    palettePixelWheel();
    require(runtime.serializeDocumentHistory() == firstStep,
            "subsequent precise deltas accumulate without extra angle steps");
    click({90, 270});
    const auto secondSelection = runtime.serializeDocumentHistory();
    palettePixelWheel();
    require(runtime.serializeDocumentHistory() != secondSelection,
            "changing selected angle resets the palette's fractional wheel remainder");
}
