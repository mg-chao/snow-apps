#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <cstdlib>
#include <iostream>
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF p) {
    const bool move = type == QEvent::MouseMove;
    QMouseEvent event(type, p, p, p, move ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}
void drag(SnowCanvasWidget& canvas, QPointF a, QPointF b) {
    mouse(canvas, QEvent::MouseButtonPress, a);
    mouse(canvas, QEvent::MouseMove, b);
    mouse(canvas, QEvent::MouseButtonRelease, b);
}
QJsonObject owner(const SnowCanvasRuntime& runtime) {
    const auto doc = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                         .object()
                         .value("document")
                         .toObject();
    QJsonObject result;
    int count = 0;
    for (const auto& slot : doc.value("slots").toArray()) {
        const auto data = slot.toObject().value("data").toObject();
        if (data.contains("Arrow")) {
            result = data.value("Arrow").toObject();
            ++count;
        }
        require(!data.contains("Text"), "brackets never allocate label elements");
    }
    require(count == 1, "exactly one bracket");
    return result;
}
QPointF point(const QJsonObject& a, int index) {
    const auto p = a.value("points").toArray().at(index).toArray();
    return {a.value("x").toDouble() + p.at(0).toDouble(),
            a.value("y").toDouble() + p.at(1).toDouble()};
}
void controlsAndExport() {
    SnowCanvasRuntime runtime;
    require(runtime.setAutomaticallySelectNewlyCreatedElements(true), "enable automatic selection");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1) && canvas.setCanvasTool(SnowCanvasTool::Bracket),
            "activate bracket");
    drag(canvas, {200, 80}, {300, 280});
    if (const auto path = qEnvironmentVariable("SNOW_BRACKET_CANVAS_PREVIEW"); !path.isEmpty())
        require(canvas.grab().save(path), "save selected bracket preview");
    const auto initial = owner(runtime);
    require(initial.value("linear_kind") == "Bracket" &&
                initial.value("points").toArray().size() == 3,
            "three-point bracket identity");
    require(canvas.canvasStyleToolbarState().source ==
                SnowCanvasStyleToolbarSource::SelectedBracket,
            "selected bracket style source");
    const auto exported = runtime.renderToImage({-300, -180, 600, 360}, {600, 360}, {});
    require(!exported.isNull() && exported.pixelColor(200, 180).alpha() == 0,
            "endpoint guide is absent from export");
    require(exported.pixelColor(300, 180).alpha() > 0, "apex is painted");
    drag(canvas, {200, 180}, {220, 190});
    const auto guided = owner(runtime);
    require(point(guided, 0) == point(initial, 0) + QPointF(20, 10) &&
                point(guided, 2) == point(initial, 2) + QPointF(20, 10) &&
                point(guided, 1) == point(initial, 1),
            "guide moves only endpoints");
    require(canvas.undo() && owner(runtime) == initial, "guide drag undoes atomically");
    drag(canvas, {300, 180}, {340, 150});
    const auto tip = owner(runtime);
    require(point(tip, 0) == point(initial, 0) && point(tip, 2) == point(initial, 2) &&
                point(tip, 1) == point(initial, 1) + QPointF(40, -30),
            "apex moves independently");
    auto style = canvas.canvasBracketStyle();
    style.stroke = QColor(20, 130, 210, 128);
    style.strokeWidth = 7;
    require(canvas.commitStyleEdit(
                SnowCanvasBracketStyleEdit{style, 3, SnowCanvasStyleEditTarget::Selection}),
            "selected style patch");
    require(canvas.canvasBracketStyle() == style, "stroke color alpha and width round trip");
    require(!canvas.commitStyleEdit(
                SnowCanvasBracketStyleEdit{style, 3, SnowCanvasStyleEditTarget::Creation}),
            "stale creation patch cannot edit selected bracket");
    const auto checkpoint = runtime.documentCheckpoint();
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(runtime.serializeDocumentSession()) &&
                restored.documentCheckpoint() == checkpoint,
            "session round trip");
    require(canvas.undo() && canvas.undo() && canvas.undo() && !runtime.hasDocumentContent(),
            "one history operation per creation or edit");
}

void otherElementsSnapToBracketControls() {
    SnowCanvasRuntime runtime;
    const auto result = QJsonDocument::fromJson(runtime.applyAnnotationTransaction(
        R"({"version":1,"operations":[{"type":"bracket","points":[[-100,-100],[0,0],[-80,100]]}]})"));
    require(result.object().value("created_element_ids").toArray().size() == 1,
            "insert skewed bracket snap reference");
    const auto bracket = owner(runtime);
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1) && canvas.setCanvasTool(SnowCanvasTool::Shape),
            "activate shape for bracket snapping");
    auto snap = canvas.canvasSnapConfig();
    snap.enabled = true;
    require(canvas.setCanvasSnapConfig(snap), "enable bracket control snapping");
    drag(canvas, {400, 40}, {303, 183});
    const auto session = runtime.serializeDocumentSession();
    const auto checkpoint = runtime.documentCheckpoint();
    const auto elementSlots = QJsonDocument::fromJson(session)
                                  .object()
                                  .value("document")
                                  .toObject()
                                  .value("slots")
                                  .toArray();
    int rectangles = 0;
    for (const auto& slot : elementSlots) {
        const auto data = slot.toObject().value("data").toObject();
        if (!data.contains("Rectangle"))
            continue;
        ++rectangles;
        const auto rect = data.value("Rectangle").toObject();
        const auto center = rect.value("center").toObject();
        require(center.value("x").toDouble() == 50 && center.value("y").toDouble() == -70 &&
                    rect.value("width").toDouble() == 100 && rect.value("height").toDouble() == 140,
                "rectangle edges snap independently to the bracket tip coordinates");
    }
    require(rectangles == 1 && owner(runtime) == bracket,
            "snapping creates one rectangle and preserves the bracket");
    require(canvas.undo() && owner(runtime) == bracket, "snapped creation undoes atomically");
    require(canvas.redo() && runtime.documentCheckpoint() == checkpoint,
            "redo restores snapped geometry");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    controlsAndExport();
    otherElementsSnapToBracketControls();
    return 0;
}
