#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>
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
        result.append({centerX - width / 2, centerX + width / 2, centerY - height / 2,
                       centerY + height / 2});
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
    std::cout << "Canvas align tests passed\n";
}
