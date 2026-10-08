#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_state.h"
#include "snow_canvas_type_conversions.h"

#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_viewport.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}

void click(SnowCanvasWidget& canvas, QPointF point) {
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
}

QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    QJsonArray found;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(kind))
            found.append(data.value(kind));
    }
    return found;
}

QJsonObject only(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto found = records(runtime, kind);
    require(found.size() == 1, "expected exactly one owner or label");
    return found.first().toObject();
}

void prepare(SnowCanvasWidget& canvas) {
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Distance), "activate distance");
}

void drag(SnowCanvasWidget& canvas, QPointF start = {100, 180}, QPointF end = {300, 180}) {
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

void pixelCalibrationPreservesFocusAndPublishedPresentation() {
    SnowCanvasRuntime runtime;
    QWidget host;
    SnowCanvasWidget canvas(runtime, &host);
    QLineEdit input(&host);
    host.resize(600, 420);
    input.setGeometry(10, 370, 200, 30);
    host.show();
    host.activateWindow();
    prepare(canvas);
    drag(canvas);
    input.setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    require(QApplication::focusWidget() == &input, "focus the host's calibration input");

    const QByteArray before = runtime.serializeDocumentSession();
    const QByteArray history = runtime.serializeDocumentHistory();
    for (const auto& targets :
         {SnowCanvasSnapGuideTargets{{100.0}, {80.0}}, SnowCanvasSnapGuideTargets{{100.0}, {80.0}},
          SnowCanvasSnapGuideTargets{{200.0}, {180.0}}, SnowCanvasSnapGuideTargets{}}) {
        require(canvas.setCanvasSnapGuideTargets(targets), "update guides over distance controls");
        require(QApplication::focusWidget() == &input,
                "guide updates must preserve focus in the distance calibration input");
        require(runtime.serializeDocumentSession() == before &&
                    runtime.serializeDocumentHistory() == history,
                "guide updates must preserve the measured distance and undo history");
    }
    SnowCanvasViewport observer;
    const auto engine = snow_canvas_runtime::Access::handle(runtime);
    require(observer.create(engine, snow_canvas_viewport::defaultEngineConfig()),
            "create calibration observer");
    ScopedPatchHandle patch;
    require(snow_viewport_acquire_patch(engine, observer.get(), nullptr, patch.outParam()) ==
                SNOW_OK,
            "read presentation before calibration");
    SnowPatchInfo beforeInfo{};
    require(snow_patch_get_info(patch.get(), &beforeInfo) == SNOW_OK,
            "read calibration observer cursor");

    for (const QSizeF scale : {QSizeF(2, 3), QSizeF(2, 3), QSizeF(4, 5)}) {
        require(canvas.setDistanceCreationPixelScale(scale), "configure future distances");
        require(QApplication::focusWidget() == &input,
                "pixel calibration must preserve focus in the host input");
        require(runtime.serializeDocumentSession() == before,
                "pixel calibration must preserve committed annotations and history");
        require(snow_viewport_acquire_patch(engine, observer.get(), nullptr, patch.outParam()) ==
                    SNOW_OK,
                "read presentation after calibration");
        SnowPatchInfo afterInfo{};
        require(snow_patch_get_info(patch.get(), &afterInfo) == SNOW_OK &&
                    afterInfo.scene_revision == beforeInfo.scene_revision &&
                    afterInfo.decoration_revision == beforeInfo.decoration_revision &&
                    afterInfo.overlay_revision == beforeInfo.overlay_revision,
                "pixel calibration must not publish unrelated presentation changes");
    }
    require(canvas.deleteAllElements(), "clear the original distance");
    drag(canvas);
    require(only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                QStringLiteral("800 cm"),
            "new annotations use the latest pixel calibration");
}

void distanceMovesPublishOneIncrementalPatchForEveryViewport() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    SnowCanvasViewport observer;
    const auto engine = snow_canvas_runtime::Access::handle(runtime);
    require(observer.create(engine, snow_canvas_viewport::defaultEngineConfig()),
            "create observer viewport");
    require(snow_viewport_set_surface_size(engine, observer.get(), 600, 360) == SNOW_OK,
            "size observer viewport");
    mouse(canvas, QEvent::MouseButtonPress, {100, 180}, Qt::LeftButton, Qt::LeftButton);
    SnowPatchCursor cursor{};
    for (int x : {200, 250, 300, 350}) {
        ScopedPatchHandle patch;
        require(snow_viewport_acquire_patch(engine, observer.get(), nullptr, patch.outParam()) ==
                    SNOW_OK,
                "read current observer patch");
        SnowPatchInfo info{};
        require(snow_patch_get_info(patch.get(), &info) == SNOW_OK, "read observer cursor");
        cursor = {info.scene_revision, info.decoration_revision, info.overlay_revision};
        require(canvas.setCanvasSnapGuideTargets({{static_cast<double>(x - 300)}, {0.0}}),
                "update guides during a distance gesture");
        mouse(canvas, QEvent::MouseMove, {static_cast<double>(x), 180}, Qt::NoButton,
              Qt::LeftButton);
        require(snow_viewport_acquire_patch(engine, observer.get(), &cursor, patch.outParam()) ==
                    SNOW_OK,
                "read distance move patch");
        require(snow_patch_get_info(patch.get(), &info) == SNOW_OK, "read distance patch info");
        require(info.scene_reset == 0 && info.scene_revision == cursor.scene_revision + 1,
                "a distance move and host measurement publish one incremental scene revision");
    }
    mouse(canvas, QEvent::MouseButtonRelease, {400, 180}, Qt::LeftButton, Qt::NoButton);
    require(only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                QStringLiteral("300 cm"),
            "release measures the final distance before history commit");
}

void gesturesAndDerivedLabels() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    auto style = canvas.canvasDistanceStyle();
    require(style.strokeWidth == 2 && style.factor == 1 &&
                style.unit == SnowCanvasDistanceUnit::Cm && style.decimalPlaces == 0 &&
                style.endpointStyle == SnowCanvasArrowhead::Bar,
            "distance defaults");
    style.unit = SnowCanvasDistanceUnit::Mm;
    style.decimalPlaces = 2;
    require(canvas.setCanvasDistanceStyle(style), "set distance creation style");
    require(canvas.setDistanceCreationPixelScale({2, 3}), "set source pixel metric");
    drag(canvas);
    const auto arrow = only(runtime, QStringLiteral("Arrow"));
    const auto text = only(runtime, QStringLiteral("Text"));
    require(arrow.value(QStringLiteral("linear_kind")).toString() == QStringLiteral("Distance"),
            "distance uses arrow storage with distinct subtype");
    require(text.value(QStringLiteral("text")).toString() == QStringLiteral("400.00 mm"),
            "label uses source pixels and fixed precision");
    require(text.value(QStringLiteral("font_size")).toDouble() == 20 &&
                text.value(QStringLiteral("rotation")).toDouble() == 0,
            "label keeps default font size and horizontal rotation");
    require(arrow.value(QStringLiteral("points")).toArray().size() == 2 &&
                arrow.value(QStringLiteral("start_arrowhead")) ==
                    arrow.value(QStringLiteral("end_arrowhead")),
            "two endpoints share the marker style");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate distance selection");
    click(canvas, {130, 180});
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedDistance,
            "new distance exposes its own selected settings");
    style = canvas.canvasDistanceStyle();
    style.stroke = QColor(20, 40, 80);
    style.strokeWidth = 4;
    require(canvas.commitStyleEdit(
                SnowCanvasDistanceEdit{style, SnowCanvasDistanceStylePropertyStrokeColor |
                                                  SnowCanvasDistanceStylePropertyStrokeWidth}),
            "patch distance stroke");
    require(
        std::abs(
            only(runtime, QStringLiteral("Text")).value(QStringLiteral("font_size")).toDouble() -
            28.284271247461902) < 1e-9,
        "stroke changes resize generated label");
    require(only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                text.value(QStringLiteral("text")),
            "stroke patch preserves measurement settings");
    require(
        canvas.undo() &&
            only(runtime, QStringLiteral("Text")).value(QStringLiteral("font_size")).toDouble() ==
                20,
        "undo restores owner and label styling together");
    require(canvas.redo(), "redo distance style");
    require(!canvas.editSelectedArrowText(), "generated distance label cannot be edited");
    mouse(canvas, QEvent::MouseButtonDblClick, {200, 180}, Qt::LeftButton, Qt::LeftButton);
    require(!canvas.hasActiveTextEditing(), "double click keeps the generated label read-only");
    const auto before = runtime.serializeDocumentSession();
    require(canvas.setDistanceCreationPixelScale({5, 5}), "update future source metric");
    require(only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                text.value(QStringLiteral("text")),
            "changing viewport metric preserves existing distance");
    require(canvas.deleteSelected() && records(runtime, QStringLiteral("Text")).isEmpty(),
            "delete removes owned label");
    require(canvas.undo(), "undo distance delete");
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(before), "restore distance session");
    require(only(restored, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                text.value(QStringLiteral("text")),
            "restoration retains distance and metric");
    const auto image = runtime.renderToImage(QRectF(-300, -180, 600, 360), {600, 360}, {});
    require(!image.isNull(), "distance exports through arrow renderer");

    SnowCanvasRuntime clicks;
    SnowCanvasWidget second(clicks);
    prepare(second);
    click(second, {100, 180});
    require(!clicks.hasDocumentContent(), "first click keeps an uncommitted draft");
    mouse(second, QEvent::MouseMove, {300, 180}, Qt::NoButton, Qt::NoButton);
    click(second, {300, 180});
    require(records(clicks, QStringLiteral("Arrow")).size() == 1 &&
                only(clicks, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                    QStringLiteral("200 cm"),
            "second click commits distance and owned label");
    require(second.undo() && !clicks.hasDocumentContent() && second.redo(),
            "creation is one history step");
}

void releaseMeasuresTheFinalDistancePreview() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    mouse(canvas, QEvent::MouseButtonPress, {100, 180}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {110, 180}, Qt::NoButton, Qt::LeftButton);
    require(!runtime.hasDocumentContent(), "a measured preview has no persistent elements");
    mouse(canvas, QEvent::MouseButtonRelease, {300, 180}, Qt::LeftButton, Qt::NoButton);
    const auto text = only(runtime, QStringLiteral("Text"));
    const double width = text.value(QStringLiteral("width")).toDouble();
    require(text.value(QStringLiteral("text")).toString() == QStringLiteral("200 cm") && width > 1,
            "release measures final generated content before its history transaction");

    SnowCanvasRuntime reference;
    SnowCanvasWidget expected(reference);
    prepare(expected);
    drag(expected);
    require(std::abs(
                width -
                only(reference, QStringLiteral("Text")).value(QStringLiteral("width")).toDouble()) <
                0.01,
            "release at a new position has the same persisted layout as a final move");
    require(canvas.undo() && canvas.redo(), "final release remains one history step");
    require(
        std::abs(width -
                 only(runtime, QStringLiteral("Text")).value(QStringLiteral("width")).toDouble()) <
            0.01,
        "redo restores the measured final layout");
}

void cancellationAndValidation() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    click(canvas, {100, 180});
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    require(!runtime.hasDocumentContent(), "cancel leaves no owner or provisional label");
    require(canvas.setCanvasTool(SnowCanvasTool::Distance), "reactivate after cancellation");
    click(canvas, {100, 180});
    mouse(canvas, QEvent::MouseButtonPress, {200, 180}, Qt::RightButton, Qt::RightButton);
    mouse(canvas, QEvent::MouseButtonRelease, {200, 180}, Qt::RightButton, Qt::NoButton);
    require(!runtime.hasDocumentContent(), "right click cancels creation");
    click(canvas, {300, 180});
    require(!runtime.hasDocumentContent(), "a click after cancellation starts a fresh draft");
    QApplication::sendEvent(&canvas, &escape);
    require(!canvas.setDistanceCreationPixelScale({0, 1}) &&
                !canvas.setDistanceCreationPixelScale({std::numeric_limits<double>::infinity(), 1}),
            "invalid pixel scales fail");
    auto style = canvas.canvasDistanceStyle();
    style.factor = 0;
    require(!canvas.setCanvasDistanceStyle(style), "invalid distance factor fails");
    style.factor = 0.01;
    style.decimalPlaces = 4;
    require(!canvas.setCanvasDistanceStyle(style), "invalid decimal count fails");
    style.decimalPlaces = 3;
    require(canvas.setCanvasDistanceStyle(style), "minimum factor is accepted");
}

void conversionsAndStateChanges() {
    SnowCanvasDistanceStyle style;
    style.factor = 2.31;
    style.unit = SnowCanvasDistanceUnit::Km;
    style.endpointStyle = SnowCanvasArrowhead::IndentedTriangle;
    style.endpointScale = 2.5;
    const auto abi = snow_canvas_types::toEngineDistanceStyle(style);
    require(snow_canvas_types::toCanvasDistanceStyle(abi) == style, "distance ABI round trip");
    auto millimeters = style;
    millimeters.unit = SnowCanvasDistanceUnit::Mm;
    const auto millimeterAbi = snow_canvas_types::toEngineDistanceStyle(millimeters);
    require(snow_canvas_types::validDistanceStyle(millimeters) &&
                millimeterAbi.unit == SNOW_DISTANCE_UNIT_MM &&
                snow_canvas_types::toCanvasDistanceStyle(millimeterAbi) == millimeters,
            "millimeter styles validate and round trip through the ABI");
    SnowStyleToolbarState original{};
    SnowStyleToolbarState changed = original;
    changed.distance_style = abi;
    require(!snow_canvas_state::styleToolbarStatesEqual(original, changed),
            "distance style updates notify the toolbar");
    changed = original;
    changed.distance_style_mixed = SnowCanvasDistanceStyleMixedFactor;
    require(!snow_canvas_state::styleToolbarStatesEqual(original, changed),
            "mixed distance state updates notify the toolbar");
    changed = original;
    changed.distance_measured_length = 125.0;
    require(!snow_canvas_state::styleToolbarStatesEqual(original, changed),
            "geometry-only distance changes notify the toolbar");
    const auto converted = snow_canvas_types::toCanvasStyleToolbarState(changed);
    require(converted.distanceMeasuredLength == 125.0 &&
                converted != snow_canvas_types::toCanvasStyleToolbarState(original),
            "calibrated length passes through the ABI and participates in toolbar equality");
}

void labelsRenderAtTheCenterWithoutWrapping() {
    const QList<QPair<QPointF, QPointF>> segments = {{{100, 180}, {500, 180}},
                                                     {{300, 60}, {300, 300}},
                                                     {{100, 60}, {500, 300}},
                                                     {{298, 180}, {302, 180}}};
    for (const auto& segment : segments) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        prepare(canvas);
        auto style = canvas.canvasDistanceStyle();
        style.stroke = QColor(245, 34, 45);
        style.decimalPlaces = 3;
        require(canvas.setCanvasDistanceStyle(style), "configure rendered distance label");
        drag(canvas, segment.first, segment.second);
        const auto text = only(runtime, QStringLiteral("Text"));
        const auto center = text.value(QStringLiteral("center")).toObject();
        const auto viewCenter = canvas.canvasToViewTransform().map(
            QPointF(center.value(QStringLiteral("x")).toDouble(),
                    center.value(QStringLiteral("y")).toDouble()));
        const auto expected = (segment.first + segment.second) / 2;
        require(std::abs(viewCenter.x() - expected.x()) < 0.01 &&
                    std::abs(viewCenter.y() - expected.y()) < 0.01,
                "horizontal, vertical, diagonal and short labels stay centered");
        require(text.value(QStringLiteral("rotation")).toDouble() == 0 &&
                    text.value(QStringLiteral("height")).toDouble() < 32 &&
                    text.value(QStringLiteral("width")).toDouble() > 1,
                "distance text has a measured horizontal single-line layout");
        const auto image = runtime.renderToImage(QRectF(-300, -180, 600, 360), {600, 360}, {});
        require(!image.isNull(), "export the generated distance label");
        bool hasGlyph = false;
        const auto halfWidth = qCeil(text.value(QStringLiteral("width")).toDouble() / 2);
        for (int y = 169; y <= 191; ++y) {
            for (int x = 300 - halfWidth; x <= 300 + halfWidth; ++x) {
                const auto color = image.pixelColor(x, y);
                hasGlyph = hasGlyph || (color.alpha() > 100 && color.red() > 200);
            }
        }
        require(hasGlyph, "export renders distance glyphs even on a short annotation");
        require(image.pixelColor(300 - halfWidth - 4, 168).alpha() == 0,
                "generated label preserves the transparent background");
    }
}
} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
    }
#endif
    QApplication application(argc, argv);
    pixelCalibrationPreservesFocusAndPublishedPresentation();
    distanceMovesPublishOneIncrementalPatchForEveryViewport();
    gesturesAndDerivedLabels();
    releaseMeasuresTheFinalDistancePreview();
    cancellationAndValidation();
    conversionsAndStateChanges();
    labelsRenderAtTheCenterWithoutWrapping();
    std::cout << "Distance canvas tests passed\n";
    return EXIT_SUCCESS;
}
