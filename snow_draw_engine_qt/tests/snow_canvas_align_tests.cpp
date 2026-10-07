#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMouseEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const bool move = type == QEvent::MouseMove;
    QMouseEvent event(type, point, point, point, move ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      modifiers);
    QApplication::sendEvent(&canvas, &event);
}
QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    QJsonArray result;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(kind)) {
            result.append(data.value(kind));
        }
    }
    return result;
}
struct RectangleRecord {
    double left;
    double right;
    double top;
    double bottom;
};
bool operator==(const RectangleRecord& lhs, const RectangleRecord& rhs) {
    return lhs.left == rhs.left && lhs.right == rhs.right && lhs.top == rhs.top &&
           lhs.bottom == rhs.bottom;
}
QList<RectangleRecord> rectangleRecords(const SnowCanvasRuntime& runtime) {
    QList<RectangleRecord> result;
    for (const auto& record : records(runtime, QStringLiteral("Rectangle"))) {
        const auto object = record.toObject();
        const auto center = object.value(QStringLiteral("center")).toObject();
        const double centerX = center.value(QStringLiteral("x")).toDouble();
        const double centerY = center.value(QStringLiteral("y")).toDouble();
        const double width = object.value(QStringLiteral("width")).toDouble();
        const double height = object.value(QStringLiteral("height")).toDouble();
        result.append(
            {centerX - width / 2, centerX + width / 2, centerY - height / 2, centerY + height / 2});
    }
    return result;
}
std::unique_ptr<SnowCanvasWidget> canvasWithThreeRectangles(SnowCanvasRuntime& runtime) {
    auto canvas = std::make_unique<SnowCanvasWidget>(runtime);
    canvas->resize(800, 600);
    canvas->show();
    QApplication::processEvents();
    require(canvas->setViewportCamera(0, 0, 1), "set camera");
    require(canvas->setCanvasTool(SnowCanvasTool::Shape), "select shape tool");
    // Left rectangle spans x [100, 200], center (150, 140).
    mouse(*canvas, QEvent::MouseButtonPress, {100, 100});
    mouse(*canvas, QEvent::MouseMove, {200, 180});
    mouse(*canvas, QEvent::MouseButtonRelease, {200, 180});
    // Middle rectangle spans x [300, 500], center (400, 300).
    mouse(*canvas, QEvent::MouseButtonPress, {300, 200});
    mouse(*canvas, QEvent::MouseMove, {500, 400});
    mouse(*canvas, QEvent::MouseButtonRelease, {500, 400});
    // Right rectangle spans x [600, 700], center (650, 220).
    mouse(*canvas, QEvent::MouseButtonPress, {600, 200});
    mouse(*canvas, QEvent::MouseMove, {700, 240});
    mouse(*canvas, QEvent::MouseButtonRelease, {700, 240});
    require(canvas->setCanvasTool(SnowCanvasTool::Select), "select move tool");
    return canvas;
}
void alignSelection() {
    SnowCanvasRuntime runtime;
    auto canvas = canvasWithThreeRectangles(runtime);
    const auto before = rectangleRecords(runtime);

    mouse(*canvas, QEvent::MouseButtonPress, {80, 80});
    mouse(*canvas, QEvent::MouseMove, {720, 420});
    mouse(*canvas, QEvent::MouseButtonRelease, {720, 420});
    require(canvas->canvasStyleToolbarState().selectedElementCount == 3,
            "marquee selects all rectangles");

    static_cast<void>(canvas->alignSelected(SnowCanvasSelectionAlignment::AlignLeft));
    const auto aligned = rectangleRecords(runtime);
    require(aligned.size() == 3, "alignment keeps every rectangle");
    for (const auto& record : aligned) {
        require(record.left == before.first().left, "align left shares one left edge");
    }

    require(canvas->undo(), "undo alignment");
    require(rectangleRecords(runtime) == before, "one undo restores positions");
    require(canvas->redo(), "redo alignment");
    const auto redone = rectangleRecords(runtime);
    for (const auto& record : redone) {
        require(record.left == before.first().left, "redo reapplies alignment");
    }

    static_cast<void>(canvas->alignSelected(SnowCanvasSelectionAlignment::AlignCenterVertically));
    const auto centered = rectangleRecords(runtime);
    double selectionTop = centered.first().top;
    double selectionBottom = centered.first().bottom;
    for (const auto& record : centered) {
        selectionTop = std::min(selectionTop, record.top);
        selectionBottom = std::max(selectionBottom, record.bottom);
    }
    const double midY = (selectionTop + selectionBottom) / 2;
    for (const auto& record : centered) {
        require(std::abs((record.top + record.bottom) / 2 - midY) < 1e-9,
                "center vertically shares one midline");
    }
}
void alignSelectionRequiresMultipleElements() {
    SnowCanvasRuntime runtime;
    auto canvas = canvasWithThreeRectangles(runtime);
    const auto before = rectangleRecords(runtime);

    // The default rectangle has no fill, so only its stroke edge is hittable.
    mouse(*canvas, QEvent::MouseButtonPress, {150, 100});
    mouse(*canvas, QEvent::MouseButtonRelease, {150, 100});
    require(canvas->canvasStyleToolbarState().selectedElementCount == 1,
            "single click selects one rectangle");

    static_cast<void>(canvas->alignSelected(SnowCanvasSelectionAlignment::AlignLeft));
    require(rectangleRecords(runtime) == before, "single selection does not align");
}
void distributeSelection() {
    SnowCanvasRuntime runtime;
    auto canvas = canvasWithThreeRectangles(runtime);
    const auto before = rectangleRecords(runtime);

    mouse(*canvas, QEvent::MouseButtonPress, {80, 80});
    mouse(*canvas, QEvent::MouseMove, {720, 420});
    mouse(*canvas, QEvent::MouseButtonRelease, {720, 420});
    require(canvas->canvasStyleToolbarState().selectedElementCount == 3,
            "marquee selects all rectangles");

    static_cast<void>(canvas->alignSelected(SnowCanvasSelectionAlignment::DistributeHorizontally));
    const auto distributed = rectangleRecords(runtime);
    require(distributed.size() == 3, "distribution keeps every rectangle");
    const double gapOne = distributed[1].left - distributed[0].right;
    const double gapTwo = distributed[2].left - distributed[1].right;
    require(std::abs(gapOne - gapTwo) < 1e-9, "distribution equalizes gaps");
    require(distributed[0].left == before[0].left, "left rectangle stays pinned");
    require(distributed[2].right == before[2].right, "right rectangle stays pinned");
}

void visibleGuideTargetsSnapWithoutElementSnapping() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "set guide test camera");
    require(!canvas.canvasSnapConfig().enabled, "element snapping starts disabled");
    require(canvas.setCanvasSnapGuideTargets({{100.0}, {80.0}}), "set guide targets");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "select shape tool for guide test");
    mouse(canvas, QEvent::MouseButtonPress, {450.0, 330.0});
    mouse(canvas, QEvent::MouseMove, {497.0, 377.0});
    mouse(canvas, QEvent::MouseButtonRelease, {497.0, 377.0});
    const auto snapped = rectangleRecords(runtime);
    require(snapped.size() == 1 && std::abs(snapped[0].right - 100.0) < 1e-9 &&
                std::abs(snapped[0].bottom - 80.0) < 1e-9,
            "shape edges snap to visible guides without enabling element snapping");
    require(canvas.setCanvasSnapGuideTargets({}), "clear guide targets");
    mouse(canvas, QEvent::MouseButtonPress, {550.0, 330.0});
    mouse(canvas, QEvent::MouseMove, {597.0, 377.0});
    mouse(canvas, QEvent::MouseButtonRelease, {597.0, 377.0});
    const auto unsnapped = rectangleRecords(runtime);
    require(unsnapped.size() == 2 && std::abs(unsnapped[1].right - 197.0) < 1e-9,
            "cleared guides must no longer affect creation");
}

void guideTargetMetadataPreservesFocusAndDocumentState() {
    SnowCanvasRuntime runtime;
    QWidget window;
    SnowCanvasWidget canvas(runtime, &window);
    QLineEdit editor(&window);
    window.resize(800, 650);
    canvas.setGeometry(0, 0, 800, 600);
    editor.setGeometry(0, 600, 800, 40);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "set metadata test camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "set metadata test tool");
    mouse(canvas, QEvent::MouseButtonPress, {450.0, 330.0});
    mouse(canvas, QEvent::MouseMove, {480.0, 360.0});
    mouse(canvas, QEvent::MouseButtonRelease, {480.0, 360.0});
    const auto document = runtime.serializeDocumentSession();
    const auto history = runtime.serializeDocumentHistory();
    const auto revision = runtime.documentRevision();
    editor.setFocus(Qt::OtherFocusReason);
    require(QApplication::focusWidget() == &editor, "metadata test editor must own focus");

    for (const auto& targets :
         {SnowCanvasSnapGuideTargets{{100.0}, {80.0}}, SnowCanvasSnapGuideTargets{{100.0}, {80.0}},
          SnowCanvasSnapGuideTargets{{200.0}, {180.0}}, SnowCanvasSnapGuideTargets{}}) {
        require(canvas.setCanvasSnapGuideTargets(targets), "guide metadata update must succeed");
        require(QApplication::focusWidget() == &editor,
                "guide metadata must preserve focus for changed, unchanged, and cleared targets");
    }
    require(runtime.documentRevision() == revision &&
                runtime.serializeDocumentSession() == document &&
                runtime.serializeDocumentHistory() == history,
            "guide metadata must preserve document and undo history");
}

void guideTargetValidationAndViewportLifecycle() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(800, 600);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "set validated guide camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "set validated guide tool");
    const SnowCanvasSnapGuideTargets firstTargets{{100.0}, {80.0}};
    const SnowCanvasSnapGuideTargets changedTargets{{200.0}, {180.0}};
    require(canvas.setCanvasSnapGuideTargets(firstTargets), "initialize validated guides");
    require(canvas.setCanvasSnapGuideTargets(changedTargets), "change validated guides");
    for (const auto& invalid :
         {SnowCanvasSnapGuideTargets{{1.0, 2.0, 3.0}, {}},
          SnowCanvasSnapGuideTargets{{}, {1.0, 2.0, 3.0}},
          SnowCanvasSnapGuideTargets{{std::numeric_limits<qreal>::quiet_NaN()}, {}},
          SnowCanvasSnapGuideTargets{{}, {std::numeric_limits<qreal>::infinity()}}}) {
        require(!canvas.setCanvasSnapGuideTargets(invalid), "invalid guides must be rejected");
    }
    const auto session = runtime.serializeDocumentSession();
    mouse(canvas, QEvent::MouseButtonPress, {550.0, 430.0});
    mouse(canvas, QEvent::MouseMove, {597.0, 477.0});
    mouse(canvas, QEvent::MouseButtonRelease, {597.0, 477.0});
    const auto changed = rectangleRecords(runtime);
    require(changed.size() == 1 && changed[0].right == 200.0 && changed[0].bottom == 180.0,
            "changed guides must snap and rejected updates must preserve valid targets");

    for (const bool restore : {false, true}) {
        require(restore ? runtime.restoreDocumentSession(session) : runtime.reset(),
                "replace guide viewport runtime");
        require(canvas.setViewportCamera(0.0, 0.0, 1.0), "restore guide camera");
        require(canvas.setCanvasTool(SnowCanvasTool::Shape), "restore guide tool");
        require(canvas.setCanvasSnapGuideTargets(changedTargets),
                "same targets must apply to a replacement viewport");
        mouse(canvas, QEvent::MouseButtonPress, {550.0, 430.0});
        mouse(canvas, QEvent::MouseMove, {597.0, 477.0});
        mouse(canvas, QEvent::MouseButtonRelease, {597.0, 477.0});
        const auto restored = rectangleRecords(runtime);
        require(!restored.isEmpty() && restored.last().right == 200.0 &&
                    restored.last().bottom == 180.0,
                "runtime replacement must not leave the same targets falsely cached");
    }
    require(runtime.clearDocumentPreservingViewports(), "clear guide document");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate shape after document clearing");
    require(canvas.setCanvasSnapGuideTargets(changedTargets), "refresh guides after clear");
    mouse(canvas, QEvent::MouseButtonPress, {550.0, 430.0});
    mouse(canvas, QEvent::MouseMove, {597.0, 477.0});
    mouse(canvas, QEvent::MouseButtonRelease, {597.0, 477.0});
    const auto cleared = rectangleRecords(runtime);
    require(cleared.size() == 1 && cleared[0].right == 200.0 && cleared[0].bottom == 180.0,
            "document clearing must preserve working guide targets");
    require(canvas.setCanvasSnapGuideTargets({}), "clear validated guides");
    // Start inside the unfilled rectangle so quick selection cannot capture its stroke.
    mouse(canvas, QEvent::MouseButtonPress, {570.0, 450.0});
    mouse(canvas, QEvent::MouseMove, {597.0, 477.0});
    mouse(canvas, QEvent::MouseButtonRelease, {597.0, 477.0});
    const auto noGuides = rectangleRecords(runtime);
    require(noGuides.size() == 2 && noGuides.last().right == 197.0 &&
                noGuides.last().bottom == 177.0,
            "cleared guides must not keep cached snapping behavior");
    runtime.destroyAsync();
    require(!canvas.setCanvasSnapGuideTargets({}),
            "a detached viewport must reject even an unchanged guide update");
}

void ctrlTogglesElementSnappingIndependentlyOfVisibleGuides() {
    for (const bool withGuides : {false, true}) {
        for (const bool persistentSnapping : {false, true}) {
            for (const bool ctrl : {false, true}) {
                for (const bool ctrlDuringDrag : {false, true}) {
                    SnowCanvasRuntime runtime;
                    SnowCanvasWidget canvas(runtime);
                    canvas.resize(800, 600);
                    canvas.show();
                    QApplication::processEvents();
                    require(canvas.setViewportCamera(0.0, 0.0, 1.0), "set Ctrl snap camera");
                    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "select Ctrl snap shape");
                    mouse(canvas, QEvent::MouseButtonPress, {300.0, 200.0});
                    mouse(canvas, QEvent::MouseMove, {500.0, 400.0});
                    mouse(canvas, QEvent::MouseButtonRelease, {500.0, 400.0});
                    auto config = canvas.canvasSnapConfig();
                    config.enabled = persistentSnapping;
                    require(canvas.setCanvasSnapConfig(config), "configure persistent snapping");
                    if (withGuides) {
                        require(canvas.setCanvasSnapGuideTargets({{400.0}, {400.0}}),
                                "set visible guides away from element snap targets");
                    }
                    const auto modifiers = ctrl ? Qt::ControlModifier : Qt::NoModifier;
                    mouse(canvas, QEvent::MouseButtonPress, {650.0, 120.0},
                          ctrlDuringDrag ? Qt::NoModifier : modifiers);
                    mouse(canvas, QEvent::MouseMove, {503.0, 197.0}, modifiers);
                    mouse(canvas, QEvent::MouseButtonRelease, {503.0, 197.0}, modifiers);
                    const auto result = rectangleRecords(runtime);
                    const bool shouldSnap = persistentSnapping != ctrl;
                    require(result.size() == 2 && result[1].left == (shouldSnap ? 100.0 : 103.0) &&
                                result[1].bottom == (shouldSnap ? -100.0 : -103.0),
                            "Ctrl must toggle element snapping before and during drawing, with "
                            "or without visible guides");
                    require(canvas.canvasSnapConfig().enabled == persistentSnapping,
                            "Ctrl snapping must not change the persistent setting");
                    require(canvas.undo() && rectangleRecords(runtime).size() == 1,
                            "snapped drawing is one undoable edit");
                    require(canvas.redo() && rectangleRecords(runtime) == result,
                            "redo restores snapped geometry");
                }
            }
        }
    }
}
} // namespace
int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
    }
#endif
    QApplication app(argc, argv);
    alignSelection();
    alignSelectionRequiresMultipleElements();
    distributeSelection();
    visibleGuideTargetsSnapWithoutElementSnapping();
    guideTargetMetadataPreservesFocusAndDocumentState();
    guideTargetValidationAndViewportLifecycle();
    ctrlTogglesElementSnappingIndependentlyOfVisibleGuides();
    std::cout << "Canvas align tests passed\n";
}
