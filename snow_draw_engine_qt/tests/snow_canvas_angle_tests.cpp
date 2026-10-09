#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_wheel_input.h"
#include "snow_canvas_state.h"
#include "snow_canvas_type_conversions.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
constexpr double radiansPerDegree = 0.017453292519943295;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}

void click(SnowCanvasWidget& canvas, QPointF point) {
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
}

void wheel(SnowCanvasWidget& canvas, int delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event({300, 60}, {300, 60}, {}, {0, delta}, Qt::NoButton, modifiers,
                      Qt::NoScrollPhase, false);
    event.setTimestamp(100);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
    require(event.isAccepted(), "angle wheel is consumed");
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
    require(found.size() == 1, "one angle owner and one generated label");
    return found.first().toObject();
}

QString label(const SnowCanvasRuntime& runtime) {
    return only(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString();
}

double sweep(const QJsonObject& owner) {
    const auto points = owner.value(QStringLiteral("points")).toArray();
    require(points.size() == 3, "an angle always has three points");
    const auto a = points.at(0).toArray();
    const auto v = points.at(1).toArray();
    const auto b = points.at(2).toArray();
    const double ax = a.at(0).toDouble() - v.at(0).toDouble();
    const double ay = a.at(1).toDouble() - v.at(1).toDouble();
    const double bx = b.at(0).toDouble() - v.at(0).toDouble();
    const double by = b.at(1).toDouble() - v.at(1).toDouble();
    double value = std::atan2(-(ax * by - ay * bx), ax * bx + ay * by);
    if (value < 0)
        value += 360 * radiansPerDegree;
    if (owner.value(QStringLiteral("angle")).toObject().value(QStringLiteral("full_turn")).toBool())
        value = 360 * radiansPerDegree;
    return value / radiansPerDegree;
}

void prepare(SnowCanvasWidget& canvas) {
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Angle), "activate angle tool");
}

void angle(SnowCanvasWidget& canvas, QPointF end = {300, 60}) {
    click(canvas, {500, 180});
    click(canvas, {300, 180});
    click(canvas, end);
}

void creationPrecisionAndHistory() {
    for (const auto unit : {SnowCanvasAngleUnit::Degrees, SnowCanvasAngleUnit::Radians}) {
        for (quint32 precision = 0; precision <= 3; ++precision) {
            SnowCanvasRuntime runtime;
            SnowCanvasWidget canvas(runtime);
            prepare(canvas);
            auto style = canvas.canvasAngleStyle();
            require(style.strokeWidth == 2 && style.decimalPlaces == 0 &&
                        style.unit == SnowCanvasAngleUnit::Degrees,
                    "default angle style");
            style.unit = unit;
            style.decimalPlaces = precision;
            require(canvas.setCanvasAngleStyle(style), "set angle format");
            click(canvas, {500, 180});
            click(canvas, {300, 180});
            require(!runtime.hasDocumentContent() && !runtime.canUndo(),
                    "two clicks leave an uncommitted preview");
            click(canvas, {300, 60});
            const QString expected =
                unit == SnowCanvasAngleUnit::Degrees
                    ? QString::number(90.0, 'f', int(precision)) + QChar(0x00b0)
                    : QString::number(90 * radiansPerDegree, 'f', int(precision)) +
                          QStringLiteral(" rad");
            require(label(runtime) == expected, "unit and fixed precision label");
            const auto owner = only(runtime, QStringLiteral("Arrow"));
            const auto text = only(runtime, QStringLiteral("Text"));
            require(owner.value(QStringLiteral("linear_kind")).toString() ==
                            QStringLiteral("Angle") &&
                        std::abs(sweep(owner) - 90) < 1e-8,
                    "counterclockwise angle has distinct identity");
            require(text.value(QStringLiteral("rotation")).toDouble() == 0 &&
                        text.value(QStringLiteral("font_size")).toDouble() == 20 &&
                        text.value(QStringLiteral("width")).toDouble() > 1,
                    "generated label is measured before final commit");
            require(canvas.undo() && !runtime.hasDocumentContent() && !runtime.canUndo(),
                    "one undo removes geometry and label");
            require(canvas.redo() && label(runtime) == expected, "redo restores measured label");
            SnowCanvasRuntime restored;
            require(restored.restoreDocumentSession(runtime.serializeDocumentSession()) &&
                        label(restored) == expected && restored.undo() &&
                        !restored.hasDocumentContent(),
                    "session preserves angle and atomic history");
        }
    }
}

void invalidClicksAndCancellation() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    click(canvas, {500, 180});
    click(canvas, {500, 180});
    click(canvas, {300, 180});
    click(canvas, {300, 180});
    require(!runtime.hasDocumentContent(), "zero-length arms keep the draft alive");
    click(canvas, {300, 60});
    require(label(runtime) == QStringLiteral("90°"), "valid click completes retained draft");
    require(canvas.deleteAllElements(), "clear completed angle");
    click(canvas, {500, 180});
    mouse(canvas, QEvent::MouseButtonDblClick, {499, 180}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {499, 180}, Qt::LeftButton);
    click(canvas, {499, 60});
    require(label(runtime) == QStringLiteral("90°"),
            "double-click press still counts as one click for a short nonzero first arm");
    require(canvas.deleteAllElements(), "clear short angle");
    click(canvas, {500, 180});
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    require(!runtime.hasDocumentContent(), "escape cancels draft");
    require(canvas.setCanvasTool(SnowCanvasTool::Angle), "reactivate angle");
    click(canvas, {500, 180});
    mouse(canvas, QEvent::MouseButtonPress, {300, 180}, Qt::RightButton, Qt::RightButton);
    mouse(canvas, QEvent::MouseButtonRelease, {300, 180}, Qt::RightButton);
    require(!runtime.hasDocumentContent(), "secondary click cancels draft");
    click(canvas, {500, 180});
    require(canvas.setCanvasTool(SnowCanvasTool::Line) && !runtime.hasDocumentContent(),
            "tool change cancels draft");
    auto invalid = canvas.canvasAngleStyle();
    invalid.decimalPlaces = 4;
    require(!canvas.setCanvasAngleStyle(invalid), "reject unsupported precision");
    invalid.decimalPlaces = 0;
    invalid.strokeWidth = std::numeric_limits<double>::quiet_NaN();
    require(!canvas.setCanvasAngleStyle(invalid) &&
                !canvas.adjustAngleValue(std::numeric_limits<double>::infinity()),
            "reject nonfinite style and wheel values");
}

void wheelDraftAndSelectedAngle() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    const auto width = canvas.canvasAngleStyle().strokeWidth;
    wheel(canvas, 120);
    require(!runtime.hasDocumentContent() && canvas.canvasAngleStyle().strokeWidth == width,
            "idle angle wheel changes neither width nor document");
    auto style = canvas.canvasAngleStyle();
    style.decimalPlaces = 1;
    require(canvas.setCanvasAngleStyle(style), "configure wheel precision");
    click(canvas, {500, 180});
    click(canvas, {300, 180});
    mouse(canvas, QEvent::MouseMove, {300, 60});
    wheel(canvas, 240);
    wheel(canvas, 240, Qt::ShiftModifier);
    mouse(canvas, QEvent::MouseMove, {300, 60});
    click(canvas, {300, 60});
    require(label(runtime) == QStringLiteral("92.2°"),
            "coalesced and fine wheel steps survive synthesized release movement");
    const auto owner = only(runtime, QStringLiteral("Arrow"));
    require(std::abs(sweep(owner) - 92.2) < 1e-8 && canvas.canvasAngleStyle().strokeWidth == width,
            "wheel changes geometry without changing stroke width");
    const auto points = owner.value(QStringLiteral("points")).toArray();
    const auto v = points.at(1).toArray();
    const auto b = points.at(2).toArray();
    require(std::abs(std::hypot(b.at(0).toDouble() - v.at(0).toDouble(),
                                b.at(1).toDouble() - v.at(1).toDouble()) -
                     120) < 1e-8,
            "wheel preserves second-arm length");
    require(canvas.undo() && !runtime.hasDocumentContent() && !runtime.canUndo(),
            "draft wheel has no separate undo entries");
    require(canvas.redo() && canvas.setCanvasTool(SnowCanvasTool::Select),
            "select completed angle");
    click(canvas, {440, 180});
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedAngle,
            "selected angle exposes angle controls");
    wheel(canvas, 120);
    require(label(runtime) == QStringLiteral("93.2°"), "selected angle wheel under Select");
    require(canvas.undo() && label(runtime) == QStringLiteral("92.2°"),
            "selected adjustment undoes geometry and label");
    require(canvas.duplicateSelected() && records(runtime, QStringLiteral("Arrow")).size() == 2 &&
                records(runtime, QStringLiteral("Text")).size() == 2,
            "duplicate remaps generated label ownership");
    require(canvas.deleteSelected() && records(runtime, QStringLiteral("Arrow")).size() == 1 &&
                records(runtime, QStringLiteral("Text")).size() == 1,
            "delete removes selected owner and label together");
}

void draftLatchUsesScreenPixels() {
    for (const int movement : {3, 4}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        prepare(canvas);
        require(canvas.setViewportCamera(0, 0, 2), "zoom draft latch fixture");
        auto style = canvas.canvasAngleStyle();
        style.decimalPlaces = 2;
        require(canvas.setCanvasAngleStyle(style), "show latch precision");
        click(canvas, {500, 180});
        click(canvas, {300, 180});
        mouse(canvas, QEvent::MouseMove, {300, 60});
        wheel(canvas, 120);
        mouse(canvas, QEvent::MouseMove, {300.0 + movement, 60});
        click(canvas, {300.0 + movement, 60});
        require(label(runtime) ==
                    (movement == 3 ? QStringLiteral("91.00°") : QStringLiteral("88.09°")),
                "latch retains three screen pixels and resumes beyond them at nonunit zoom");
    }
}

bool inkNear(const QImage& image, QPoint center, int radius = 3) {
    for (int y = center.y() - radius; y <= center.y() + radius; ++y) {
        for (int x = center.x() - radius; x <= center.x() + radius; ++x) {
            if (image.rect().contains(x, y) && image.pixelColor(x, y).alpha() > 100)
                return true;
        }
    }
    return false;
}

void directedArcExportAndFullTurn() {
    for (const auto end : {QPointF(300, 60), QPointF(300, 300)}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        prepare(canvas);
        angle(canvas, end);
        require(label(runtime) == (end.y() < 180 ? QStringLiteral("90°") : QStringLiteral("270°")),
                "directed sweep supports reflex angles");
        const auto image = runtime.renderToImage({-300, -180, 600, 360}, {600, 360}, {});
        require(!image.isNull() && inkNear(image, {420, 180}), "export renders first arm");
        require(inkNear(image, end.y() < 180 ? QPoint(319, 161) : QPoint(273, 180)),
                "export renders directed inner arc away from either arm");
        require(image.pixelColor(20, 20).alpha() == 0, "export keeps transparent background");
    }
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    angle(canvas, {440, 180});
    require(label(runtime) == QStringLiteral("0°"), "coincident nonzero rays create zero angle");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select zero angle");
    click(canvas, {420, 180});
    require(canvas.adjustAngleValue(400 * radiansPerDegree) &&
                label(runtime) == QStringLiteral("360°"),
            "wheel clamps at full turn and preserves its identity");
    const auto image = runtime.renderToImage({-300, -180, 600, 360}, {600, 360}, {});
    require(inkNear(image, {273, 180}), "full-turn export renders a circle");
    require(canvas.adjustAngleValue(-500 * radiansPerDegree) &&
                label(runtime) == QStringLiteral("0°"),
            "wheel clamps at zero");
}

void abiAndStyleNotifications() {
    SnowCanvasAngleStyle style;
    style.unit = SnowCanvasAngleUnit::Radians;
    style.decimalPlaces = 3;
    style.strokeWidth = 72;
    const auto abi = snow_canvas_types::toEngineAngleStyle(style);
    require(snow_canvas_types::toCanvasAngleStyle(abi) == style, "angle ABI round trip");
    SnowStyleToolbarState before{};
    auto after = before;
    after.angle_style = abi;
    require(!snow_canvas_state::styleToolbarStatesEqual(before, after),
            "angle style notifies controls");
    after = before;
    after.angle_style_mixed = 4;
    require(!snow_canvas_state::styleToolbarStatesEqual(before, after),
            "angle mixed state notifies controls");
}

void selectedStyleKeepsCreationDefaults() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    const auto defaults = canvas.canvasAngleStyle();
    angle(canvas);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select for angle style edit");
    click(canvas, {440, 180});
    auto selected = canvas.canvasAngleStyle();
    selected.unit = SnowCanvasAngleUnit::Radians;
    selected.decimalPlaces = 2;
    const auto properties = static_cast<quint32>(SnowCanvasAngleStyleProperty::Unit) |
                            static_cast<quint32>(SnowCanvasAngleStyleProperty::DecimalPlaces);
    require(!canvas.commitStyleEdit(SnowCanvasAngleStyleEdit{selected, properties, true}) &&
                label(runtime) == QStringLiteral("90°"),
            "stale creation-default editor cannot mutate a newly selected angle");
    require(canvas.commitStyleEdit(SnowCanvasAngleStyleEdit{selected, properties, false}) &&
                label(runtime) == QStringLiteral("1.57 rad"),
            "edit selected angle format");
    click(canvas, {30, 30});
    require(canvas.setCanvasTool(SnowCanvasTool::Angle) && canvas.canvasAngleStyle() == defaults,
            "selected style edits preserve future angle defaults");
}

void arcAndLabelSelectAndEraseTheOwnedPair() {
    for (const bool useLabel : {false, true}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        prepare(canvas);
        angle(canvas);
        QPointF target(319, 161);
        if (useLabel) {
            const auto center =
                only(runtime, QStringLiteral("Text")).value(QStringLiteral("center")).toObject();
            target = canvas.canvasToViewTransform().map(
                QPointF(center.value(QStringLiteral("x")).toDouble(),
                        center.value(QStringLiteral("y")).toDouble()));
        }
        require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate arc and label picking");
        click(canvas, target);
        require(canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::SelectedAngle,
                "picking arc or generated label selects the angle");
        mouse(canvas, QEvent::MouseButtonDblClick, target, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, target, Qt::LeftButton);
        require(!canvas.hasActiveTextEditing(), "angle labels remain read-only");
        require(canvas.setCanvasTool(SnowCanvasTool::Eraser), "activate object eraser");
        click(canvas, target);
        require(!runtime.hasDocumentContent(), "arc and label erasure remove the owned pair");
        require(canvas.undo() && label(runtime) == QStringLiteral("90°"),
                "erasure undo restores angle and label together");
    }
}

void heterogeneousStyleEditChangesOnlyCreationDefaults() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas);
    angle(canvas);
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "create a mixed-selection rectangle");
    mouse(canvas, QEvent::MouseButtonPress, {60, 230}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {140, 290}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {140, 290}, Qt::LeftButton);
    const auto owner = only(runtime, QStringLiteral("Arrow"));
    const auto rectangles = records(runtime, QStringLiteral("Rectangle"));
    require(rectangles.size() == 1, "mixed-selection fixture contains one rectangle");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select heterogeneous objects");
    mouse(canvas, QEvent::MouseButtonPress, {20, 20}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {550, 320}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {550, 320}, Qt::LeftButton);
    require(runtime.selectedElementIds().size() >= 2 &&
                canvas.canvasStyleToolbarState().source !=
                    SnowCanvasStyleToolbarSource::SelectedAngle,
            "angle and rectangle expose a heterogeneous selection");
    auto style = canvas.canvasAngleStyle();
    style.decimalPlaces = 3;
    require(canvas.setCanvasAngleStylePatch(
                style, static_cast<quint32>(SnowCanvasAngleStyleProperty::DecimalPlaces)),
            "update angle defaults in heterogeneous selection");
    require(only(runtime, QStringLiteral("Arrow")) == owner &&
                records(runtime, QStringLiteral("Rectangle")) == rectangles &&
                label(runtime) == QStringLiteral("90°") &&
                canvas.canvasAngleStyle().decimalPlaces == 3,
            "heterogeneous style edits preserve selected records and change only future defaults");
}

void savePreview(const QString& path) {
    QImage montage(1200, 720, QImage::Format_ARGB32_Premultiplied);
    montage.fill(QColor(QStringLiteral("#fafafa")));
    QPainter painter(&montage);
    const QList<double> angles{0, 90, 180, 270, 360, 135};
    for (int i = 0; i < angles.size(); ++i) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        prepare(canvas);
        auto style = canvas.canvasAngleStyle();
        if (i == 5) {
            style.unit = SnowCanvasAngleUnit::Radians;
            style.decimalPlaces = 3;
            style.strokeWidth = 8;
        }
        require(canvas.setCanvasAngleStyle(style), "configure preview");
        const double value = angles.at(i) == 360 ? 0 : angles.at(i);
        angle(canvas, QPointF(300 + 120 * std::cos(value * radiansPerDegree),
                              180 - 120 * std::sin(value * radiansPerDegree)));
        if (angles.at(i) == 360) {
            require(canvas.setCanvasTool(SnowCanvasTool::Select), "select full turn preview");
            click(canvas, {420, 180});
            require(canvas.adjustAngleValue(360 * radiansPerDegree), "make full turn preview");
        }
        const auto image = runtime.renderToImage({-300, -180, 600, 360}, {600, 360}, {});
        const QRect tile((i % 3) * 400, (i / 3) * 360, 400, 360);
        painter.fillRect(tile.adjusted(1, 1, -1, -1), Qt::white);
        painter.drawImage(tile.adjusted(0, 60, 0, -60), image);
    }
    painter.end();
    require(montage.save(path), "save angle render preview");
}
} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QApplication application(argc, argv);
    creationPrecisionAndHistory();
    invalidClicksAndCancellation();
    wheelDraftAndSelectedAngle();
    draftLatchUsesScreenPixels();
    directedArcExportAndFullTurn();
    abiAndStyleNotifications();
    selectedStyleKeepsCreationDefaults();
    heterogeneousStyleEditChangesOnlyCreationDefaults();
    arcAndLabelSelectAndEraseTheOwnedPair();
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--preview"))
        savePreview(QString::fromLocal8Bit(argv[2]));
    std::cout << "Angle canvas tests passed\n";
    return EXIT_SUCCESS;
}
