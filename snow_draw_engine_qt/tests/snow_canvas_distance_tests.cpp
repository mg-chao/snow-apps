#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_state.h"
#include "snow_canvas_type_conversions.h"

#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_viewport.h"

#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
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

void key(SnowCanvasWidget& canvas, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}

void replaceValue(SnowCanvasWidget& canvas, const QString& value) {
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    QApplication::clipboard()->setText(value);
    key(canvas, Qt::Key_V, Qt::ControlModifier);
}

void inlineDistanceOpeningDragSelectsDigits() {
    for (const auto tool : {SnowCanvasTool::Distance, SnowCanvasTool::Select}) {
        for (const double zoom : {0.5, 1.0, 2.0}) {
            for (const bool reverse : {false, true}) {
                SnowCanvasRuntime runtime;
                SnowCanvasWidget canvas(runtime);
                prepare(canvas);
                auto style = canvas.canvasDistanceStyle();
                style.decimalPlaces = 3;
                require(canvas.setCanvasDistanceStyle(style), "set drag-selection precision");
                drag(canvas);
                require(canvas.setCanvasTool(tool), "set numeric drag-selection tool");
                require(canvas.setViewportCamera(-100, 0, zoom), "zoom numeric drag selection");
                const auto document = runtime.serializeDocumentSession();
                const auto history = runtime.serializeDocumentHistory();
                const QPointF left(300 - 47 * zoom, 180);
                const QPointF right(300 + 5 * zoom, 180);
                const QPointF start = reverse ? right : left;
                const QPointF end = reverse ? left : right;
                mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
                require(canvas.hasActiveTextEditing(), "opening press enters numeric editing");
                mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
                mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
                const auto selected = static_cast<QWidget&>(canvas)
                                          .inputMethodQuery(Qt::ImCurrentSelection)
                                          .toString();
                require(!selected.isEmpty() && !selected.contains(QLatin1Char(' ')),
                        "the opening drag selects digits without the unit");
                mouse(canvas, QEvent::MouseMove, {450, 250}, Qt::NoButton, Qt::NoButton);
                require(static_cast<QWidget&>(canvas)
                                .inputMethodQuery(Qt::ImCurrentSelection)
                                .toString() == selected,
                        "releasing the opening drag ends selection tracking");
                key(canvas, Qt::Key_C, Qt::ControlModifier);
                require(QApplication::clipboard()->text() == selected,
                        "opening-drag selection can be copied");
                key(canvas, Qt::Key_5, Qt::NoModifier, QStringLiteral("5"));
                const auto edited = static_cast<QWidget&>(canvas)
                                        .inputMethodQuery(Qt::ImSurroundingText)
                                        .toString();
                require(edited.contains(QLatin1Char('5')) &&
                            edited.size() == QStringLiteral("200.000").size() - selected.size() + 1,
                        "typing replaces the opening-drag selection");
                key(canvas, Qt::Key_Escape);
                require(!canvas.hasActiveTextEditing() &&
                            runtime.serializeDocumentSession() == document &&
                            runtime.serializeDocumentHistory() == history,
                        "canceling a selected numeric edit preserves the owner and history");
            }
        }
    }
}

void inlineDistanceValuesKeepTheirOwnerAndControls() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    auto style = canvas.canvasDistanceStyle();
    style.decimalPlaces = 3;
    require(canvas.setCanvasDistanceStyle(style), "set distance precision");
    drag(canvas);
    const auto points = only(runtime, QStringLiteral("Arrow")).value(QStringLiteral("points"));
    const auto initialHistory = runtime.serializeDocumentHistory();
    const auto valuePoint = QPointF(170, 180);
    mouse(canvas, QEvent::MouseMove, valuePoint, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() == Qt::IBeamCursor, "numeric distance hover uses I-beam");
    click(canvas, valuePoint);
    require(canvas.hasActiveTextEditing(), "one click edits an unselected distance value");
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedDistance,
            "inline value editing preserves distance toolbar");
    mouse(canvas, QEvent::MouseMove, {240, 180}, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() != Qt::IBeamCursor,
            "unit hover stays noneditable during numeric editing");
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    key(canvas, Qt::Key_C, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == QStringLiteral("200.000"),
            "selection excludes the unit");
    key(canvas, Qt::Key_Return);
    require(!canvas.hasActiveTextEditing() && runtime.serializeDocumentHistory() == initialHistory,
            "unchanged value does not create an undo step");
    click(canvas, valuePoint);
    replaceValue(canvas, QStringLiteral("25.125"));
    require(only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                QStringLiteral("200.000 cm"),
            "draft remains temporary until committed");
    key(canvas, Qt::Key_Return);
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                    QStringLiteral("25.125 cm") &&
                only(runtime, QStringLiteral("Arrow")).value(QStringLiteral("points")) == points,
            "Enter recalibrates the value without moving endpoints");
    require(canvas.undo() && only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                                 QStringLiteral("200.000 cm"),
            "one undo restores the original value");
    require(canvas.redo(), "redo inline distance value");
    click(canvas, {180, 180});
    require(canvas.hasActiveTextEditing(), "click edits a selected distance");
    replaceValue(canvas, QStringLiteral("50"));
    key(canvas, Qt::Key_Escape);
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                    QStringLiteral("25.125 cm"),
            "Escape cancels the draft");
    click(canvas, {180, 180});
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    key(canvas, Qt::Key_Delete);
    key(canvas, Qt::Key_Return);
    require(canvas.hasActiveTextEditing() && records(runtime, QStringLiteral("Arrow")).size() == 1,
            "empty input stays editable and Delete never removes the annotation");
    click(canvas, {450, 250});
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                    QStringLiteral("25.125 cm"),
            "outside click discards invalid input");
    click(canvas, {180, 180});
    replaceValue(canvas, QStringLiteral("30.500"));
    click(canvas, {450, 250});
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                    QStringLiteral("30.500 cm"),
            "outside click saves valid input");
    mouse(canvas, QEvent::MouseMove, {230, 180}, Qt::NoButton, Qt::NoButton);
    require(canvas.cursor().shape() != Qt::IBeamCursor, "unit suffix is not editable");
    click(canvas, {230, 180});
    require(!canvas.hasActiveTextEditing(), "unit click retains annotation selection behavior");
    click(canvas, {180, 180});
    replaceValue(canvas, QStringLiteral("40"));
    style = canvas.canvasDistanceStyle();
    style.unit = SnowCanvasDistanceUnit::Mm;
    require(canvas.setCanvasDistanceStyle(style, SnowCanvasDistanceStylePropertyUnit),
            "change unit from distance controls");
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                    QStringLiteral("40.000 mm"),
            "toolbar edit commits the pending value before changing unit");
}

void inlineDistanceZoomInputAndLifecycle() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    drag(canvas);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "use select for numeric editing");
    for (double zoom : {0.5, 1.0, 2.0}) {
        require(canvas.setViewportCamera(-100, 0, zoom), "zoom the distance canvas");
        const QPointF point(300 - 15 * zoom, 180);
        mouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        require(canvas.cursor().shape() == Qt::IBeamCursor, "numeric hover follows zoom");
        click(canvas, point);
        require(canvas.hasActiveTextEditing(), "numeric click follows zoom");
        key(canvas, Qt::Key_Escape);
        mouse(canvas, QEvent::MouseMove, {300 + 25 * zoom, 180}, Qt::NoButton, Qt::NoButton);
        require(canvas.cursor().shape() != Qt::IBeamCursor,
                "unit hover stays outside numeric hit area at each zoom");
    }
    click(canvas, {270, 180});
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent ime;
    ime.setCommitString(QStringLiteral("12.5"));
    QApplication::sendEvent(&canvas, &ime);
    require(static_cast<QWidget&>(canvas).inputMethodQuery(Qt::ImSurroundingText).toString() ==
                QStringLiteral("12.5"),
            "IME edits only the numeric buffer");
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(static_cast<QWidget&>(canvas).inputMethodQuery(Qt::ImSurroundingText).toString() ==
                QStringLiteral("200"),
            "local undo restores the numeric buffer");
    replaceValue(canvas, QStringLiteral("12.3456"));
    require(static_cast<QWidget&>(canvas).inputMethodQuery(Qt::ImSurroundingText).toString() ==
                QStringLiteral("200"),
            "four decimal places are rejected");
    replaceValue(canvas, QStringLiteral("12 mm"));
    require(static_cast<QWidget&>(canvas).inputMethodQuery(Qt::ImSurroundingText).toString() ==
                QStringLiteral("200"),
            "pasted unit text is rejected");
    replaceValue(canvas, QStringLiteral("12\n"));
    require(static_cast<QWidget&>(canvas).inputMethodQuery(Qt::ImSurroundingText).toString() ==
                QStringLiteral("200"),
            "pasted trailing newlines are rejected");
    replaceValue(canvas, QStringLiteral("50"));
    canvas.setInteractionEnabled(false);
    require(!canvas.hasActiveTextEditing() && canvas.canvasDistanceStyle().factor == 1.0,
            "disabling interaction cancels the numeric draft");
    canvas.setInteractionEnabled(true);
    click(canvas, {270, 180});
    const auto session = runtime.serializeDocumentSession();
    require(runtime.restoreDocumentSession(session), "restore during numeric editing");
    require(!canvas.hasActiveTextEditing() && !canvas.testAttribute(Qt::WA_InputMethodEnabled),
            "document restoration releases the numeric editor and input method");
    require(canvas.setViewportCamera(-100, 0, 2), "restore zoom after session replacement");
    require(canvas.setCanvasTool(SnowCanvasTool::Distance),
            "activate distance after session replacement");
    click(canvas, {270, 180});
    require(canvas.hasActiveTextEditing(), "reopen editor before external deletion");
    SnowCanvasWidget other(runtime);
    require(other.deleteSelected(), "delete owner through another viewport");
    require(!canvas.hasActiveTextEditing() && records(runtime, QStringLiteral("Arrow")).isEmpty(),
            "owner deletion releases the active numeric editor");
}

void inlineDistanceImePreeditIsPublishedWithoutCommitting() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    auto style = canvas.canvasDistanceStyle();
    style.decimalPlaces = 3;
    require(canvas.setCanvasDistanceStyle(style), "set IME distance precision");
    drag(canvas);
    const auto originalArrow = only(runtime, QStringLiteral("Arrow"));
    const auto originalText = only(runtime, QStringLiteral("Text"));
    const auto history = runtime.serializeDocumentHistory();
    const auto engine = snow_canvas_runtime::Access::handle(runtime);
    SnowCanvasViewport observer;
    require(observer.create(engine, snow_canvas_viewport::defaultEngineConfig()),
            "create IME presentation observer");
    require(snow_viewport_set_surface_size(engine, observer.get(), 600, 360) == SNOW_OK,
            "set IME observer surface");
    const auto requirePresentation = [&](const QString& value) {
        require(static_cast<QWidget&>(canvas).inputMethodQuery(Qt::ImSurroundingText).toString() ==
                    value,
                "IME surrounding text contains the numeric composition without the unit");
        ScopedPatchHandle patch;
        require(snow_viewport_acquire_patch(engine, observer.get(), nullptr, patch.outParam()) ==
                    SNOW_OK,
                "read composed distance scene");
        const SnowSceneDisplayItem* items = nullptr;
        std::uint32_t count = 0;
        require(snow_patch_get_scene_items(patch.get(), &items, &count) == SNOW_OK,
                "read composed distance scene items");
        bool found = false;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (items[index].kind == SNOW_SCENE_DISPLAY_ITEM_TEXT) {
                require(QString::fromUtf8(items[index].text_utf8, items[index].text_utf8_len) ==
                            value + QStringLiteral(" cm"),
                        "rendered distance label includes the current IME composition");
                found = true;
            }
        }
        require(found, "composed distance label remains in the scene");
        require(only(runtime, QStringLiteral("Arrow")) == originalArrow &&
                    only(runtime, QStringLiteral("Text")) == originalText &&
                    runtime.serializeDocumentHistory() == history,
                "IME composition preserves the document and undo history");
    };

    click(canvas, {170, 180});
    require(canvas.hasActiveTextEditing(), "open distance editor for IME composition");
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    for (const auto& value : {QStringLiteral("12.5"), QStringLiteral("12.3456"),
                              QStringLiteral("pending"), QString()}) {
        QInputMethodEvent preedit(value, {});
        QApplication::sendEvent(&canvas, &preedit);
        requirePresentation(value.isEmpty() ? QStringLiteral("200.000") : value);
    }
    for (const auto& rejected : {QStringLiteral("12.3456"), QStringLiteral("pending")}) {
        key(canvas, Qt::Key_A, Qt::ControlModifier);
        QInputMethodEvent rejectedPreedit(rejected, {});
        QApplication::sendEvent(&canvas, &rejectedPreedit);
        requirePresentation(rejected);
        QInputMethodEvent rejectedCommit;
        rejectedCommit.setCommitString(rejected);
        QApplication::sendEvent(&canvas, &rejectedCommit);
        requirePresentation(QStringLiteral("200.000"));
    }
    key(canvas, Qt::Key_End);
    QInputMethodEvent oldComposition(QStringLiteral("pending"), {});
    QApplication::sendEvent(&canvas, &oldComposition);
    requirePresentation(QStringLiteral("200.000pending"));
    QInputMethodEvent nextComposition(QStringLiteral("next"), {});
    nextComposition.setCommitString(QStringLiteral("invalid"));
    QApplication::sendEvent(&canvas, &nextComposition);
    requirePresentation(QStringLiteral("200.000next"));
    QInputMethodEvent clearComposition;
    QApplication::sendEvent(&canvas, &clearComposition);
    requirePresentation(QStringLiteral("200.000"));

    key(canvas, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent beforeRejectedKey(QStringLiteral("pending"), {});
    QApplication::sendEvent(&canvas, &beforeRejectedKey);
    key(canvas, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    requirePresentation(QStringLiteral("200.000"));
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent preedit(QStringLiteral("12.5"), {});
    QApplication::sendEvent(&canvas, &preedit);
    key(canvas, Qt::Key_Escape);
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")) == originalText &&
                runtime.serializeDocumentHistory() == history,
            "Escape discards numeric composition without committing it");

    click(canvas, {170, 180});
    require(canvas.hasActiveTextEditing(), "reopen distance editor for IME acceptance");
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent acceptedPreedit(QStringLiteral("12.5"), {});
    QApplication::sendEvent(&canvas, &acceptedPreedit);
    QInputMethodEvent committed;
    committed.setCommitString(QStringLiteral("12.5"));
    QApplication::sendEvent(&canvas, &committed);
    requirePresentation(QStringLiteral("12.5"));
    key(canvas, Qt::Key_Return);
    require(!canvas.hasActiveTextEditing() &&
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                    QStringLiteral("12.500 cm") &&
                only(runtime, QStringLiteral("Arrow")).value(QStringLiteral("points")) ==
                    originalArrow.value(QStringLiteral("points")),
            "accepted IME number recalibrates the distance on Enter");
    require(canvas.undo() && only(runtime, QStringLiteral("Arrow")) == originalArrow &&
                only(runtime, QStringLiteral("Text")) == originalText,
            "one undo restores the distance before the accepted IME edit");
}

void unchangedRoundedDistancePreservesCalibration() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    require(canvas.setDistanceCreationPixelScale({2, 3}), "use anisotropic pixel calibration");
    drag(canvas, {100, 180}, {130, 220});
    const auto history = runtime.serializeDocumentHistory();
    click(canvas, {105, 200});
    require(canvas.hasActiveTextEditing(), "edit a rounded calibrated value");
    key(canvas, Qt::Key_Return);
    require(runtime.serializeDocumentHistory() == history &&
                only(runtime, QStringLiteral("Arrow"))
                        .value(QStringLiteral("distance"))
                        .toObject()
                        .value(QStringLiteral("factor"))
                        .toDouble() == 1.0,
            "unchanged rounded value preserves exact calibration and history");
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

void calibrationFollowsRuntimeReplacementAndCaptureReset() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    const auto emptySession = runtime.serializeDocumentSession();
    for (int capture = 0; capture < 3; ++capture) {
        if (capture == 1) {
            require(runtime.restoreDocumentSession(emptySession), "replace the distance runtime");
            prepare(canvas);
        } else if (capture == 2) {
            require(runtime.clearDocumentPreservingViewports(), "reset the capture document");
            prepare(canvas);
        }
        require(canvas.setDistanceCreationPixelScale({2, 3}), "restore capture pixel calibration");
        require(canvas.setDistanceCreationPixelScale({2, 3}), "reuse unchanged pixel calibration");
        require(!canvas.setDistanceCreationPixelScale({0, 3}),
                "invalid calibration must never replace the last valid scale");
        require(canvas.setDistanceCreationPixelScale({2, 3}), "reuse valid scale after rejection");
        drag(canvas);
        require(only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                    QStringLiteral("400 cm"),
                "reused calibration must survive runtime replacement and capture reset");
    }
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
    for (auto unit :
         {SnowCanvasDistanceUnit::Px, SnowCanvasDistanceUnit::Cm, SnowCanvasDistanceUnit::M,
          SnowCanvasDistanceUnit::Km, SnowCanvasDistanceUnit::Mm, SnowCanvasDistanceUnit::Pt,
          SnowCanvasDistanceUnit::In, SnowCanvasDistanceUnit::Ft, SnowCanvasDistanceUnit::Um}) {
        auto candidate = style;
        candidate.unit = unit;
        require(snow_canvas_types::validDistanceStyle(candidate) &&
                    snow_canvas_types::toCanvasDistanceStyle(
                        snow_canvas_types::toEngineDistanceStyle(candidate)) == candidate,
                "every distance unit validates and round trips without changing calibration");
    }
    for (int invalid : {-1, 9}) {
        auto candidate = style;
        candidate.unit = static_cast<SnowCanvasDistanceUnit>(invalid);
        require(!snow_canvas_types::validDistanceStyle(candidate), "unknown units must fail");
    }
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
    inlineDistanceOpeningDragSelectsDigits();
    inlineDistanceValuesKeepTheirOwnerAndControls();
    inlineDistanceZoomInputAndLifecycle();
    inlineDistanceImePreeditIsPublishedWithoutCommitting();
    unchangedRoundedDistancePreservesCalibration();
    pixelCalibrationPreservesFocusAndPublishedPresentation();
    calibrationFollowsRuntimeReplacementAndCaptureReset();
    distanceMovesPublishOneIncrementalPatchForEveryViewport();
    gesturesAndDerivedLabels();
    releaseMeasuresTheFinalDistancePreview();
    cancellationAndValidation();
    conversionsAndStateChanges();
    labelsRenderAtTheCenterWithoutWrapping();
    std::cout << "Distance canvas tests passed\n";
    return EXIT_SUCCESS;
}
