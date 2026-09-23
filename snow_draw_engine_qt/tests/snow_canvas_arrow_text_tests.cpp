#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_text_editor_session.h"
#include "snow_canvas_text.h"
#include "snow_canvas_type_conversions.h"

#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
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
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void key(SnowCanvasWidget& canvas, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(&canvas, &event);
}

QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const QJsonObject document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                     .object()
                                     .value(QStringLiteral("document"))
                                     .toObject();
    QJsonArray result;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto record = slot.toObject();
        if (record.value(QStringLiteral("data")).toObject().contains(kind)) {
            result.append(record);
        }
    }
    return result;
}

QJsonObject payload(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto found = records(runtime, kind);
    require(found.size() == 1, "expected one record of the requested kind");
    return found.first().toObject().value(QStringLiteral("data")).toObject().value(kind).toObject();
}

void createArrow(SnowCanvasWidget& canvas, SnowCanvasRuntime& runtime,
                 SnowCanvasArrowType type = SnowCanvasArrowType::Straight) {
    require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "activate arrow tool");
    SnowCanvasShapeStyle style;
    style.arrowType = type;
    require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyArrowType,
                                            SnowCanvasShapeKind::Arrow),
            "set arrow type");
    mouse(canvas, QEvent::MouseButtonPress, {70.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {490.0, 180.0}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {490.0, 180.0}, Qt::LeftButton, Qt::NoButton);
    require(records(runtime, QStringLiteral("Arrow")).size() == 1,
            "arrow gesture creates one arrow");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate selection tool");
}

void openLabel(SnowCanvasWidget& canvas) {
    mouse(canvas, QEvent::MouseButtonDblClick, {280.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    require(canvas.hasActiveTextEditing(), "double-click opens an attached text editor");
    require(canvas.canvasStyleToolbarState().source == SnowCanvasStyleToolbarSource::SelectedText,
            "arrow draft exposes text style controls");
}

void arrowRatioEditsRenderAndRoundTrip() {
    for (const auto shaft : {SnowCanvasArrowShaftType::Plain, SnowCanvasArrowShaftType::Tapered}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(600, 360);
        canvas.show();
        QApplication::processEvents();
        require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "activate ratio test arrow");
        SnowCanvasShapeStyle style;
        style.stroke = Qt::red;
        style.strokeWidth = 2.0;
        style.startArrowhead = SnowCanvasArrowhead::Triangle;
        style.endArrowhead = SnowCanvasArrowhead::Triangle;
        style.arrowShaftType = shaft;
        require(canvas.setCanvasShapeStylePatch(style,
                                                SnowCanvasShapeStylePropertyStrokeColor |
                                                    SnowCanvasShapeStylePropertyStrokeWidth |
                                                    SnowCanvasShapeStylePropertyStartArrowhead |
                                                    SnowCanvasShapeStylePropertyEndArrowhead |
                                                    SnowCanvasShapeStylePropertyArrowShaftType,
                                                SnowCanvasShapeKind::Arrow),
                "configure ratio test arrow");
        createArrow(canvas, runtime);
        const auto render = [&]() {
            return runtime.renderToImage(QRectF(-300, -180, 600, 360), QSize(600, 360), {});
        };
        const QImage original = render();
        mouse(canvas, QEvent::MouseButtonPress, {30.0, 120.0}, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, {550.0, 240.0}, Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, {550.0, 240.0}, Qt::LeftButton, Qt::NoButton);
        require(canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::SelectedArrow,
                "ratio test selects the arrow");
        style.arrowRatio = 3.0;
        require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyArrowRatio,
                                                SnowCanvasShapeKind::Arrow),
                "edit selected ratio");
        const auto arrow = payload(runtime, QStringLiteral("Arrow"));
        require(arrow.value(QStringLiteral("arrow_ratio")).toDouble() == 3.0 &&
                    arrow.value(QStringLiteral("stroke_width")).toDouble() == 2.0 &&
                    canvas.canvasStyleToolbarState().shapeStyle.arrowRatio == 3.0,
                "ratio crosses Qt and FFI without changing stroke width");
        const QImage enlarged = render();
        require(!original.isNull() && !enlarged.isNull() && original != enlarged,
                "ratio changes exported endpoint geometry");
        SnowCanvasRuntime restored;
        require(restored.restoreDocumentSession(runtime.serializeDocumentSession()) &&
                    payload(restored, QStringLiteral("Arrow")) == arrow,
                "ratio round-trips through the document session");
        require(canvas.undo() &&
                    payload(runtime, QStringLiteral("Arrow"))
                            .value(QStringLiteral("arrow_ratio"))
                            .toDouble() == 1.0 &&
                    render() == original,
                "undo restores ratio and original pixels");
        require(canvas.redo() && render() == enlarged, "redo restores enlarged endpoint pixels");
    }
    SnowCanvasShapeStyle style;
    for (double invalid : {0.0, 4.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        style.arrowRatio = invalid;
        const auto normalized =
            snow_canvas_types::toCanvasShapeStyle(snow_canvas_types::toEngineShapeStyle(style));
        require(normalized.arrowRatio == (invalid == 4.0 ? 3.0 : 1.0),
                "Qt/FFI normalizes invalid ratios");
    }
}

void indentedTriangleStyleRoundTrips() {
    static_assert(static_cast<int>(SnowCanvasArrowhead::Triangle) == 6);
    static_assert(static_cast<int>(SnowCanvasArrowhead::CrowfootOneOrMany) == 12);
    static_assert(SNOW_ARROWHEAD_TRIANGLE == 6);
    static_assert(SNOW_ARROWHEAD_INVERTED_TRIANGLE == 14);
    require(snow_canvas_types::toEngineArrowhead(SnowCanvasArrowhead::IndentedTriangle) ==
                    SNOW_ARROWHEAD_INDENTED_TRIANGLE &&
                snow_canvas_types::toCanvasArrowhead(SNOW_ARROWHEAD_INDENTED_TRIANGLE) ==
                    SnowCanvasArrowhead::IndentedTriangle,
            "indented triangle should round-trip through the Qt/C ABI boundary");
    for (int id = 0; id <= 12; ++id) {
        require(static_cast<int>(snow_canvas_types::toEngineArrowhead(
                    static_cast<SnowCanvasArrowhead>(id))) == id &&
                    static_cast<int>(
                        snow_canvas_types::toCanvasArrowhead(static_cast<SnowArrowhead>(id))) == id,
                "existing arrowhead numeric IDs must retain their meanings");
    }
    SnowCanvasStyleDefaults defaults;
    for (auto* shape : {&defaults.rectangle, &defaults.arrow, &defaults.line, &defaults.freeDraw,
                        &defaults.rectangleHighlight, &defaults.penHighlight}) {
        shape->fill = Qt::transparent;
        shape->stroke = Qt::red;
    }
    defaults.text.fill = Qt::transparent;
    defaults.serialNumber.fill = Qt::transparent;
    defaults.arrow.startArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    defaults.arrow.endArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    SnowStyleDefaults engineDefaults{};
    require(snow_canvas_types::toEngineStyleDefaults(defaults, engineDefaults),
            "indented endpoints should pass style defaults validation");
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "activate indented arrow tool");
    SnowCanvasShapeStyle style;
    style.startArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    style.endArrowhead = SnowCanvasArrowhead::IndentedTriangle;
    style.stroke = QColor(255, 40, 30);
    style.strokeWidth = 4.0;
    require(canvas.setCanvasShapeStylePatch(style,
                                            SnowCanvasShapeStylePropertyStartArrowhead |
                                                SnowCanvasShapeStylePropertyEndArrowhead |
                                                SnowCanvasShapeStylePropertyStrokeColor |
                                                SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::Arrow),
            "indented triangle style should be accepted");
    mouse(canvas, QEvent::MouseButtonPress, {150.0, 70.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {350.0, 290.0}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {350.0, 290.0}, Qt::LeftButton, Qt::NoButton);
    const auto arrow = payload(runtime, QStringLiteral("Arrow"));
    require(arrow.value(QStringLiteral("start_arrowhead")).toString() ==
                    QStringLiteral("indented_triangle") &&
                arrow.value(QStringLiteral("end_arrowhead")).toString() ==
                    QStringLiteral("indented_triangle"),
            "both indented endpoints should survive document serialization");
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(runtime.serializeDocumentSession()) &&
                payload(restored, QStringLiteral("Arrow")) == arrow,
            "indented endpoints should round-trip through saved documents");
    const QString previewDirectory = qEnvironmentVariable("SNOW_ARROW_TEXT_PREVIEW_DIR");
    if (!previewDirectory.isEmpty()) {
        require(canvas.grab().save(previewDirectory + QStringLiteral("/indented-triangle.png")),
                "save indented triangle rendering artifact");
    }
}

void taperedShaftsRenderAndRoundTrip() {
    {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(600, 360);
        canvas.show();
        QApplication::processEvents();
        require(canvas.setCanvasTool(SnowCanvasTool::Arrow),
                "activate double-headed tapered arrow tool");
        SnowCanvasShapeStyle style;
        style.arrowShaftType = SnowCanvasArrowShaftType::Tapered;
        style.startArrowhead = SnowCanvasArrowhead::Arrow;
        style.endArrowhead = SnowCanvasArrowhead::Arrow;
        style.stroke = QColor(255, 0, 0, 128);
        style.strokeWidth = 4.0;
        require(canvas.setCanvasShapeStylePatch(style,
                                                SnowCanvasShapeStylePropertyArrowShaftType |
                                                    SnowCanvasShapeStylePropertyStartArrowhead |
                                                    SnowCanvasShapeStylePropertyEndArrowhead |
                                                    SnowCanvasShapeStylePropertyStrokeColor |
                                                    SnowCanvasShapeStylePropertyStrokeWidth,
                                                SnowCanvasShapeKind::Arrow),
                "set double-headed tapered arrow defaults");
        createArrow(canvas, runtime);
        const QImage image =
            runtime.renderToImage(QRectF(-300, -180, 600, 360), QSize(600, 360), {});
        const int startInteriorAlpha = image.pixelColor(90, 180).alpha();
        require(startInteriorAlpha >= 120 && startInteriorAlpha <= 130,
                "both open-arrow heads must use the filled tapered rendering");
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                require(image.pixelColor(x, y).alpha() <= 130,
                        "double-headed taper must not double-paint either head join");
    }

    for (const auto head :
         {SnowCanvasArrowhead::Arrow, SnowCanvasArrowhead::Triangle,
          SnowCanvasArrowhead::TriangleOutline, SnowCanvasArrowhead::IndentedTriangle}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(600, 360);
        canvas.show();
        QApplication::processEvents();
        require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "activate tapered arrow tool");
        SnowCanvasShapeStyle style;
        style.arrowType = SnowCanvasArrowType::Straight;
        style.arrowShaftType = SnowCanvasArrowShaftType::Tapered;
        style.endArrowhead = head;
        style.stroke = QColor(255, 0, 0, 128);
        style.strokeWidth = 4.0;
        const quint32 properties =
            SnowCanvasShapeStylePropertyArrowType | SnowCanvasShapeStylePropertyArrowShaftType |
            SnowCanvasShapeStylePropertyEndArrowhead | SnowCanvasShapeStylePropertyStrokeColor |
            SnowCanvasShapeStylePropertyStrokeWidth;
        require(canvas.setCanvasShapeStylePatch(style, properties, SnowCanvasShapeKind::Arrow),
                "set tapered arrow defaults");
        createArrow(canvas, runtime);
        require(payload(runtime, QStringLiteral("Arrow"))
                        .value(QStringLiteral("arrow_shaft_type"))
                        .toString() == QStringLiteral("tapered"),
                "new arrows should preserve the shaft preference");
        const auto exportImage = [&]() {
            return runtime.renderToImage(QRectF(-300, -180, 600, 360), QSize(600, 360), {});
        };
        QImage image = exportImage();
        require(!image.isNull(), "tapered arrow export must render");
        const QString directory = qEnvironmentVariable("SNOW_ARROW_TEXT_PREVIEW_DIR");
        if (!directory.isEmpty())
            image.save(directory + QStringLiteral("/shaft-%1.png").arg(static_cast<int>(head)));
        if (head == SnowCanvasArrowhead::TriangleOutline) {
            require(image.pixelColor(400, 180).alpha() == 0,
                    "hollow shaft interior must stay transparent");
            require(image.pixelColor(400, 176).alpha() > 0, "hollow shaft contour must be visible");
        } else {
            require(image.pixelColor(400, 180).alpha() >= 120 &&
                        image.pixelColor(400, 180).alpha() <= 130,
                    "filled taper must retain uniform alpha");
            require(image.pixelColor(400, 178).alpha() > 0,
                    "filled taper must widen near its head");
        }
        SnowCanvasRuntime restored;
        require(restored.restoreDocumentSession(runtime.serializeDocumentSession()),
                "restore tapered document");
        require(payload(restored, QStringLiteral("Arrow")) ==
                    payload(runtime, QStringLiteral("Arrow")),
                "shaft must round-trip");
        mouse(canvas, QEvent::MouseButtonPress, {40.0, 140.0}, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, {530.0, 220.0}, Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, {530.0, 220.0}, Qt::LeftButton, Qt::NoButton);
        require(canvas.canvasStyleToolbarState().source ==
                    SnowCanvasStyleToolbarSource::SelectedArrow,
                "select arrow before editing shaft");
        style.arrowShaftType = SnowCanvasArrowShaftType::Plain;
        require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyArrowShaftType,
                                                SnowCanvasShapeKind::Arrow),
                "edit shaft on selected arrow");
        require(payload(runtime, QStringLiteral("Arrow"))
                        .value(QStringLiteral("arrow_shaft_type"))
                        .toString() == QStringLiteral("plain"),
                "selected shaft should change");
        require(canvas.undo(), "undo shaft style edit");
        require(payload(runtime, QStringLiteral("Arrow"))
                        .value(QStringLiteral("arrow_shaft_type"))
                        .toString() == QStringLiteral("tapered"),
                "undo must restore shaft");
        require(canvas.redo(), "redo shaft style edit");
        require(payload(runtime, QStringLiteral("Arrow"))
                        .value(QStringLiteral("arrow_shaft_type"))
                        .toString() == QStringLiteral("plain"),
                "redo restores plain shaft");
        require(canvas.undo(), "restore taper for fallback check");
        style.endArrowhead = SnowCanvasArrowhead::Circle;
        require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyEndArrowhead,
                                                SnowCanvasShapeKind::Arrow),
                "switch to unsupported head");
        const QImage fallback = exportImage();
        require(payload(runtime, QStringLiteral("Arrow"))
                        .value(QStringLiteral("arrow_shaft_type"))
                        .toString() == QStringLiteral("tapered"),
                "fallback retains preference");
        require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyArrowShaftType,
                                                SnowCanvasShapeKind::Arrow),
                "set explicit plain fallback");
        require(exportImage() == fallback, "unsupported head must render exactly like plain shaft");
        for (const QString& pathType :
             {QStringLiteral("straight"), QStringLiteral("curve"), QStringLiteral("elbow")}) {
            for (const QString& strokeStyle :
                 {QStringLiteral("solid"), QStringLiteral("dashed"), QStringLiteral("dotted")}) {
                for (const bool reverse : {false, true}) {
                    auto session =
                        QJsonDocument::fromJson(restored.serializeDocumentSession()).object();
                    auto document = session.value(QStringLiteral("document")).toObject();
                    auto documentSlots = document.value(QStringLiteral("slots")).toArray();
                    for (int i = 0; i < documentSlots.size(); ++i) {
                        auto slot = documentSlots.at(i).toObject();
                        auto data = slot.value(QStringLiteral("data")).toObject();
                        if (!data.contains(QStringLiteral("Arrow")))
                            continue;
                        auto arrow = data.value(QStringLiteral("Arrow")).toObject();
                        arrow.insert(QStringLiteral("x"), -230.0);
                        arrow.insert(QStringLiteral("y"), 0.0);
                        arrow.insert(QStringLiteral("width"), 440.0);
                        arrow.insert(QStringLiteral("height"), 180.0);
                        arrow.insert(QStringLiteral("arrow_type"), pathType);
                        arrow.insert(QStringLiteral("stroke_style"), strokeStyle);
                        arrow.insert(QStringLiteral("points"),
                                     pathType == QStringLiteral("elbow")
                                         ? QJsonArray{QJsonArray{0, 0}, QJsonArray{300, 0},
                                                      QJsonArray{300, -80}, QJsonArray{130, -80}}
                                         : QJsonArray{QJsonArray{0, 0}, QJsonArray{300, 100},
                                                      QJsonArray{440, 0}, QJsonArray{130, -80}});
                        if (reverse) {
                            arrow.insert(QStringLiteral("start_arrowhead"),
                                         arrow.value(QStringLiteral("end_arrowhead")));
                            arrow.insert(QStringLiteral("end_arrowhead"), QJsonValue::Null);
                        }
                        data.insert(QStringLiteral("Arrow"), arrow);
                        slot.insert(QStringLiteral("data"), data);
                        documentSlots.replace(i, slot);
                    }
                    document.insert(QStringLiteral("slots"), documentSlots);
                    session.insert(QStringLiteral("document"), document);
                    SnowCanvasRuntime empty;
                    const auto emptySession =
                        QJsonDocument::fromJson(empty.serializeDocumentSession()).object();
                    session.insert(QStringLiteral("history"),
                                   emptySession.value(QStringLiteral("history")));
                    SnowCanvasRuntime variant;
                    require(variant.restoreDocumentSession(QJsonDocument(session).toJson()),
                            "restore routed taper fixture");
                    const QImage routed =
                        variant.renderToImage(QRectF(-300, -180, 600, 360), QSize(1200, 720), {});
                    int visible = 0;
                    for (int y = 0; y < routed.height(); ++y)
                        for (int x = 0; x < routed.width(); ++x) {
                            const int alpha = routed.pixelColor(x, y).alpha();
                            visible += alpha > 0 ? 1 : 0;
                            require(alpha <= 130,
                                    "taper must not double-paint translucent bends or head joins");
                        }
                    require(visible > 100, "every route, dash style, and direction must render");
                    if (!directory.isEmpty() && head == SnowCanvasArrowhead::TriangleOutline &&
                        !reverse)
                        routed.save(directory +
                                    QStringLiteral("/shaft-%1-%2.png").arg(pathType, strokeStyle));
                }
            }
        }
    }
}

void deleteKeyRemovesEditedText() {
    for (const bool attached : {false, true}) {
        for (const bool existing : {false, true}) {
            for (const bool selected : {false, true}) {
                SnowCanvasRuntime runtime;
                SnowCanvasWidget canvas(runtime);
                canvas.resize(600, 360);
                canvas.show();
                QApplication::processEvents();
                if (attached) {
                    createArrow(canvas, runtime);
                    openLabel(canvas);
                } else {
                    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool");
                    mouse(canvas, QEvent::MouseButtonPress, {280.0, 180.0}, Qt::LeftButton,
                          Qt::LeftButton);
                    mouse(canvas, QEvent::MouseButtonRelease, {280.0, 180.0}, Qt::LeftButton,
                          Qt::NoButton);
                }
                key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("abc"));
                if (existing) {
                    key(canvas, Qt::Key_Return, Qt::ControlModifier);
                    require(records(runtime, QStringLiteral("Text")).size() == 1,
                            "commit creates text before reopening");
                    if (attached) {
                        openLabel(canvas);
                    } else {
                        mouse(canvas, QEvent::MouseButtonPress, {280.0, 180.0}, Qt::LeftButton,
                              Qt::LeftButton);
                        mouse(canvas, QEvent::MouseButtonRelease, {280.0, 180.0}, Qt::LeftButton,
                              Qt::NoButton);
                    }
                }
                require(canvas.hasActiveTextEditing(), "text editor is active before Delete");
                key(canvas, Qt::Key_Home);
                key(canvas, Qt::Key_Right);
                if (selected) {
                    key(canvas, Qt::Key_A, Qt::ControlModifier);
                }
                QInputMethodEvent preedit(QStringLiteral("pending"), {});
                QApplication::sendEvent(&canvas, &preedit);
                key(canvas, Qt::Key_Delete);
                require(!canvas.hasActiveTextEditing(), "Delete ends text editing");
                require(!canvas.testAttribute(Qt::WA_InputMethodEnabled),
                        "Delete disables text input methods");
                require(records(runtime, QStringLiteral("Text")).isEmpty(),
                        "Delete removes the entire text regardless of caret or selection");
                if (attached) {
                    require(records(runtime, QStringLiteral("Arrow")).size() == 1,
                            "deleting label preserves its arrow");
                }
                if (existing) {
                    require(canvas.undo(), "text deletion is undoable");
                    require(payload(runtime, QStringLiteral("Text"))
                                    .value(QStringLiteral("text"))
                                    .toString() == QStringLiteral("abc"),
                            "one undo restores the original text");
                    require(canvas.redo(), "text deletion is redoable");
                    require(records(runtime, QStringLiteral("Text")).isEmpty(),
                            "redo removes text again");
                } else if (!attached) {
                    require(!canvas.canvasHistoryState().canUndo,
                            "deleting a new draft does not create a history entry");
                }
            }
        }
    }
}

void widgetLifecycle() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    createArrow(canvas, runtime);
    const QByteArray before = runtime.serializeDocumentHistory();
    openLabel(canvas);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("cancelled"));
    key(canvas, Qt::Key_Escape);
    require(!canvas.hasActiveTextEditing(), "Escape closes the draft");
    require(runtime.serializeDocumentHistory() == before,
            "cancel leaves document and history untouched");

    openLabel(canvas);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Request"));
    key(canvas, Qt::Key_Return);
    QInputMethodEvent preedit(QString::fromUtf8("连接"), {});
    QApplication::sendEvent(&canvas, &preedit);
    require(records(runtime, QStringLiteral("Text")).isEmpty(),
            "IME draft remains outside document");
    QInputMethodEvent committed;
    committed.setCommitString(QString::fromUtf8("连接 → response"));
    QApplication::sendEvent(&canvas, &committed);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    const auto text = payload(runtime, QStringLiteral("Text"));
    require(text.value(QStringLiteral("text")).toString() ==
                QString::fromUtf8("Request\n连接 → response"),
            "multiline IME text commits without content changes");
    require(text.value(QStringLiteral("rotation")).toDouble() == 0.0, "label stays horizontal");
    require(payload(runtime, QStringLiteral("Arrow"))
                .value(QStringLiteral("text_element_id"))
                .isObject(),
            "committed label has persisted ownership");
    require(canvas.canvasStyleToolbarState().canEditArrowText,
            "selected arrow exposes edit action");
    require(canvas.undo(), "undo label creation");
    require(records(runtime, QStringLiteral("Text")).isEmpty(), "one undo removes the label");
    require(records(runtime, QStringLiteral("Arrow")).size() == 1, "undo preserves arrow");
    require(canvas.redo(), "redo label creation");

    key(canvas, Qt::Key_Return);
    require(canvas.hasActiveTextEditing(), "Enter reopens the selected arrow label");
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("replacement"));
    key(canvas, Qt::Key_Escape);
    require(payload(runtime, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                text.value(QStringLiteral("text")),
            "cancelling existing label restores original text");
    const auto ownerStyle = payload(runtime, QStringLiteral("Arrow"));
    require(canvas.editSelectedArrowText(), "open label style editing");
    auto style = canvas.canvasStyleToolbarState().textStyle;
    style.color = QColor(194, 36, 62);
    style.fontSize = 36.0;
    require(canvas.setCanvasTextStyle(style), "change label font and color");
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(
        payload(runtime, QStringLiteral("Text")).value(QStringLiteral("font_size")).toDouble() ==
            36.0,
        "label style commits with text layout");
    require(payload(runtime, QStringLiteral("Arrow")) == ownerStyle,
            "label style preserves arrow style and geometry");
    require(canvas.undo(), "undo label style and measured layout together");
    require(payload(runtime, QStringLiteral("Text")) == text,
            "style undo restores exact original label");
    require(canvas.editSelectedArrowText(), "public action opens existing label");
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    key(canvas, Qt::Key_Backspace);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(records(runtime, QStringLiteral("Text")).isEmpty(), "empty commit removes label");
    require(records(runtime, QStringLiteral("Arrow")).size() == 1, "empty commit keeps arrow");
    require(canvas.undo(), "undo restores cleared label");
    require(canvas.duplicateSelected(), "duplicate arrow and label");
    require(records(runtime, QStringLiteral("Text")).size() == 2, "duplicate includes text");
    require(canvas.deleteSelected(), "delete duplicated pair");
    require(records(runtime, QStringLiteral("Text")).size() == 1, "delete removes owned label");
    const QByteArray saved = runtime.serializeDocumentSession();
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(saved), "saved arrow text and history restore");
    require(payload(restored, QStringLiteral("Text")).value(QStringLiteral("text")) ==
                text.value(QStringLiteral("text")),
            "restored text matches original");
}

void arrowLabelWheelChangesFontSizeWhileSelecting() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    createArrow(canvas, runtime);
    openLabel(canvas);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Label"));

    const double initialFontSize = canvas.canvasStyleToolbarState().textStyle.fontSize;
    const QTransform initialTransform = canvas.canvasToViewTransform();
    const QPointF position(280.0, 180.0);
    const auto wheel = [&](int delta) {
        QWheelEvent event(position, canvas.mapToGlobal(position.toPoint()), QPoint(),
                          QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&canvas, &event);
    };
    wheel(120);
    require(canvas.canvasStyleToolbarState().textStyle.fontSize == initialFontSize + 1.0,
            "wheel increases an arrow label draft's font size with Select active");
    require(canvas.canvasToViewTransform() == initialTransform,
            "font-size wheel does not zoom while editing an arrow label");
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(
        payload(runtime, QStringLiteral("Text")).value(QStringLiteral("font_size")).toDouble() ==
            initialFontSize + 1.0,
        "wheel-adjusted arrow label font size commits");

    require(canvas.editSelectedArrowText(), "reopen the committed arrow label");
    wheel(-120);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(
        payload(runtime, QStringLiteral("Text")).value(QStringLiteral("font_size")).toDouble() ==
            initialFontSize,
        "wheel decreases an existing arrow label's font size");
}

void wrappingAndFinalPointerPosition() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    createArrow(canvas, runtime);
    require(canvas.setCanvasTool(SnowCanvasTool::Text), "activate text tool");
    mouse(canvas, QEvent::MouseButtonPress, {280.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    require(canvas.hasActiveTextEditing(), "Text tool creates an attached label");
    const QString original = QStringLiteral("A long label with several words and emoji ") +
                             QString::fromUtf8("🙂 中文 ").repeated(32);
    key(canvas, Qt::Key_A, Qt::NoModifier, original);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    auto text = payload(runtime, QStringLiteral("Text"));
    require(text.value(QStringLiteral("text")).toString() == original,
            "wrapping preserves original text");
    const double font = text.value(QStringLiteral("font_size")).toDouble();
    require(text.value(QStringLiteral("width")).toDouble() <= qMax(420.0 * 0.7, font * 11.0) + 0.01,
            "label obeys reference maximum width");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "select arrow for endpoint drag");
    mouse(canvas, QEvent::MouseButtonPress, {80.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {80.0, 180.0}, Qt::LeftButton, Qt::NoButton);
    mouse(canvas, QEvent::MouseButtonPress, {490.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {390.0, 180.0}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {360.0, 180.0}, Qt::LeftButton, Qt::NoButton);
    const auto arrow = payload(runtime, QStringLiteral("Arrow"));
    text = payload(runtime, QStringLiteral("Text"));
    require(std::abs(arrow.value(QStringLiteral("width")).toDouble() - 290.0) < 0.01,
            "endpoint uses the final release coordinate");
    require(text.value(QStringLiteral("width")).toDouble() <= qMax(290.0 * 0.7, font * 11.0) + 0.01,
            "final arrow geometry and label wrapping commit together");
    const auto center = text.value(QStringLiteral("center")).toObject();
    const QPointF view =
        canvas.canvasToViewTransform().map(QPointF(center.value(QStringLiteral("x")).toDouble(),
                                                   center.value(QStringLiteral("y")).toDouble()));
    require(std::abs(view.x() - 215.0) < 0.01, "label follows final midpoint");
    require(canvas.undo(), "undo endpoint edit");
    require(
        std::abs(
            payload(runtime, QStringLiteral("Arrow")).value(QStringLiteral("width")).toDouble() -
            420.0) < 0.01,
        "one undo restores arrow and measured label");
}

void gapPreservesBackground() {
    for (const SnowArrowType type :
         {SNOW_ARROW_TYPE_STRAIGHT, SNOW_ARROW_TYPE_CURVE, SNOW_ARROW_TYPE_ELBOW}) {
        for (const SnowStrokeStyle style :
             {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
            for (int head = SNOW_ARROWHEAD_NONE; head <= SNOW_ARROWHEAD_INDENTED_TRIANGLE; ++head) {
                for (const QColor background : {QColor(19, 103, 157), QColor(Qt::transparent)}) {
                    QImage image(200, 100, QImage::Format_ARGB32_Premultiplied);
                    image.fill(background);
                    SceneDisplayInfo info{};
                    info.surface_width = 200;
                    info.surface_height = 100;
                    info.camera_zoom = 1.0;
                    SnowArrowPoint points[] = {{-80.0, 0.0}, {80.0, 0.0}};
                    SnowSceneDisplayItem raw{};
                    raw.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
                    raw.stroke = {255, 0, 0, 255};
                    raw.stroke_width = 4.0;
                    raw.opacity = 1.0;
                    raw.arrow_points = points;
                    raw.arrow_point_count = 2;
                    raw.arrow_type = type;
                    raw.arrow_stroke_style = style;
                    raw.arrow_start_head = static_cast<SnowArrowhead>(head);
                    raw.arrow_end_head = static_cast<SnowArrowhead>(head);
                    raw.has_bound_text_element = 1;
                    raw.arrow_text_bounds[0] = -25.0;
                    raw.arrow_text_bounds[1] = -10.0;
                    raw.arrow_text_bounds[2] = 25.0;
                    raw.arrow_text_bounds[3] = 10.0;
                    SnowCanvasSceneItem item(raw);
                    QPainter painter(&image);
                    snow_canvas_renderer::SceneRenderRequest request;
                    request.painter = &painter;
                    request.displayInfo = &info;
                    request.sceneItems = &item;
                    request.sceneItemCount = 1;
                    request.exposedRegion = QRegion(image.rect());
                    request.clearBackgroundEnabled = false;
                    snow_canvas_renderer::renderSceneItems(request);
                    painter.end();
                    require(image.pixelColor(100, 50) == background,
                            "arrow gap preserves original background and alpha");
                    bool visible = false;
                    for (int x = 35; x < 65; ++x) {
                        visible = visible || image.pixelColor(x, 50).red() > 230;
                    }
                    require(visible, "arrow remains visible outside label gap");
                }
            }
        }
    }
}

void arrowTypesDragLabelAlongPathAndEraseAsPair() {
    for (const auto type :
         {SnowCanvasArrowType::Straight, SnowCanvasArrowType::Curve, SnowCanvasArrowType::Elbow}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(600, 360);
        canvas.show();
        QApplication::processEvents();
        createArrow(canvas, runtime, type);
        openLabel(canvas);
        key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("first\nsecond\nthird"));
        // Clicking outside commits without needing a keyboard shortcut.
        mouse(canvas, QEvent::MouseButtonPress, {550.0, 310.0}, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, {550.0, 310.0}, Qt::LeftButton, Qt::NoButton);
        require(!canvas.hasActiveTextEditing(), "outside click commits arrow label");
        const auto before = payload(runtime, QStringLiteral("Text"));
        const auto beforeArrow = payload(runtime, QStringLiteral("Arrow"));
        const QString previewDirectory = qEnvironmentVariable("SNOW_ARROW_TEXT_PREVIEW_DIR");
        if (!previewDirectory.isEmpty()) {
            require(canvas.grab().save(
                        previewDirectory +
                        QStringLiteral("/arrow-text-%1.png").arg(static_cast<int>(type))),
                    "save requested rendering artifact");
        }
        const auto center = before.value(QStringLiteral("center")).toObject();
        const QPointF point = canvas.canvasToViewTransform().map(
                                  QPointF(center.value(QStringLiteral("x")).toDouble(),
                                          center.value(QStringLiteral("y")).toDouble())) +
                              QPointF(0.0, 20.0);
        mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, point + QPointF(30.0, 40.0), Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, point + QPointF(30.0, 40.0), Qt::LeftButton,
              Qt::NoButton);
        const auto after = payload(runtime, QStringLiteral("Text"));
        const auto moved = after.value(QStringLiteral("center")).toObject();
        require(std::abs(moved.value(QStringLiteral("x")).toDouble() -
                         center.value(QStringLiteral("x")).toDouble() - 30.0) < 0.01,
                "dragging label moves it along the arrow");
        require(std::abs(moved.value(QStringLiteral("y")).toDouble() -
                         center.value(QStringLiteral("y")).toDouble()) < 0.01,
                "dragging off the path keeps the label on the arrow");
        const auto afterArrow = payload(runtime, QStringLiteral("Arrow"));
        require(afterArrow.value(QStringLiteral("x")) == beforeArrow.value(QStringLiteral("x")) &&
                    afterArrow.value(QStringLiteral("y")) ==
                        beforeArrow.value(QStringLiteral("y")) &&
                    afterArrow.value(QStringLiteral("points")) ==
                        beforeArrow.value(QStringLiteral("points")) &&
                    afterArrow.contains(QStringLiteral("text_path_fraction")),
                "dragging the label preserves arrow geometry and stores its path position");
        require(after.value(QStringLiteral("font_size")) ==
                    before.value(QStringLiteral("font_size")),
                "dragging the label preserves font size");
        require(canvas.setCanvasTool(SnowCanvasTool::Eraser), "activate eraser");
        const QPointF erasePoint = canvas.canvasToViewTransform().map(
                                       QPointF(moved.value(QStringLiteral("x")).toDouble(),
                                               moved.value(QStringLiteral("y")).toDouble())) +
                                   QPointF(0.0, 20.0);
        mouse(canvas, QEvent::MouseButtonPress, erasePoint, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, erasePoint, Qt::LeftButton, Qt::NoButton);
        require(records(runtime, QStringLiteral("Arrow")).isEmpty() &&
                    records(runtime, QStringLiteral("Text")).isEmpty(),
                "erasing label deletes the pair");
        require(canvas.undo(), "undo pair erasure");
        require(records(runtime, QStringLiteral("Text")).size() == 1, "undo restores owned text");
    }
}

void deleteAllElementsClearsDocumentAsOneUndoEntry() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(600, 360);
    canvas.show();
    QApplication::processEvents();
    createArrow(canvas, runtime);
    openLabel(canvas);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("label"));
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate shape tool");
    mouse(canvas, QEvent::MouseButtonPress, {20.0, 20.0}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {120.0, 90.0}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {120.0, 90.0}, Qt::LeftButton, Qt::NoButton);
    require(records(runtime, QStringLiteral("Arrow")).size() == 1 &&
                records(runtime, QStringLiteral("Text")).size() == 1 &&
                records(runtime, QStringLiteral("Rectangle")).size() == 1,
            "arrow, label, and rectangle exist before deleting all");

    require(canvas.deleteAllElements(), "delete all elements should succeed");
    require(records(runtime, QStringLiteral("Arrow")).isEmpty() &&
                records(runtime, QStringLiteral("Text")).isEmpty() &&
                records(runtime, QStringLiteral("Rectangle")).isEmpty(),
            "delete all removes every element kind");
    require(canvas.canvasHistoryState().canUndo, "delete all keeps history for undo");

    require(canvas.undo(), "undo delete all");
    require(records(runtime, QStringLiteral("Arrow")).size() == 1 &&
                records(runtime, QStringLiteral("Text")).size() == 1 &&
                records(runtime, QStringLiteral("Rectangle")).size() == 1,
            "one undo restores every element");

    require(canvas.redo(), "redo delete all");
    require(records(runtime, QStringLiteral("Arrow")).isEmpty() &&
                records(runtime, QStringLiteral("Text")).isEmpty() &&
                records(runtime, QStringLiteral("Rectangle")).isEmpty(),
            "redo clears the document again");
}

void sharedViewsAndLongOffscreenText() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    SnowCanvasWidget second(runtime);
    canvas.resize(600, 360);
    second.resize(600, 360);
    canvas.show();
    second.show();
    QApplication::processEvents();
    require(second.setViewportCamera(0.0, 0.0, 0.65), "shared viewport uses an independent zoom");
    createArrow(canvas, runtime);
    openLabel(canvas);
    key(canvas, Qt::Key_Escape);
    const QImage before = second.grab().toImage();
    const QByteArray history = runtime.serializeDocumentHistory();
    openLabel(canvas);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Shared draft"));
    const QImage draft = second.grab().toImage();
    require(draft != before, "draft text and arrow gap refresh other viewports");
    key(canvas, Qt::Key_Escape);
    require(second.grab().toImage() == before, "cancellation removes shared draft and gap");
    require(runtime.serializeDocumentHistory() == history, "shared draft adds no undo records");
    openLabel(canvas);
    const QString original = QString::fromUtf8("长文本🙂 with words\n").repeated(100);
    key(canvas, Qt::Key_A, Qt::NoModifier, original);
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(canvas.setViewportCamera(10000.0, 10000.0, 0.7), "pan label outside scene cache");
    require(canvas.editSelectedArrowText(), "reopen complete offscreen label");
    key(canvas, Qt::Key_End, Qt::ControlModifier);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("END"));
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    require(payload(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                original + QStringLiteral("END"),
            "offscreen reopening preserves text beyond metadata buffer");
}

void boundShapeReroutesAndMeasuresLabel() {
    for (const auto type :
         {SnowCanvasArrowType::Straight, SnowCanvasArrowType::Curve, SnowCanvasArrowType::Elbow}) {
        SnowCanvasRuntime runtime;
        SnowCanvasWidget canvas(runtime);
        canvas.resize(600, 360);
        canvas.show();
        QApplication::processEvents();
        for (const QRectF rect :
             {QRectF(20.0, 145.0, 45.0, 70.0), QRectF(495.0, 145.0, 65.0, 70.0)}) {
            require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate bound shape tool");
            mouse(canvas, QEvent::MouseButtonPress, rect.topLeft(), Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, rect.bottomRight(), Qt::NoButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, rect.bottomRight(), Qt::LeftButton,
                  Qt::NoButton);
        }
        createArrow(canvas, runtime, type);
        require(payload(runtime, QStringLiteral("Arrow"))
                    .value(QStringLiteral("end_binding"))
                    .isObject(),
                "arrow endpoint binds to target shape");
        openLabel(canvas);
        key(canvas, Qt::Key_A, Qt::NoModifier,
            QStringLiteral("Bound arrow label with wrapping and preserved words ").repeated(5));
        key(canvas, Qt::Key_Return, Qt::ControlModifier);
        const auto original = payload(runtime, QStringLiteral("Text"));
        mouse(canvas, QEvent::MouseButtonPress, {558.0, 180.0}, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, {390.0, 270.0}, Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, {360.0, 280.0}, Qt::LeftButton, Qt::NoButton);
        const auto arrow = payload(runtime, QStringLiteral("Arrow"));
        const auto text = payload(runtime, QStringLiteral("Text"));
        require(text.value(QStringLiteral("center")) != original.value(QStringLiteral("center")),
                "label follows arrow when bound shape moves");
        require(text.value(QStringLiteral("width")).toDouble() <=
                    qMax(arrow.value(QStringLiteral("width")).toDouble() * 0.7,
                         text.value(QStringLiteral("font_size")).toDouble() * 11.0) +
                        0.01,
                "bound shape release includes final label wrapping");
        require(text.value(QStringLiteral("text")) == original.value(QStringLiteral("text")),
                "rerouting preserves label content");
        require(canvas.undo(), "undo bound shape move");
        require(payload(runtime, QStringLiteral("Text")) == original,
                "one undo restores exact label before rerouting");
    }
}
} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // The static offscreen plugin uses FreeType, so point it at installed fonts.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
    }
#endif
    QApplication app(argc, argv);
    arrowRatioEditsRenderAndRoundTrip();
    taperedShaftsRenderAndRoundTrip();
    if (app.arguments().contains(QStringLiteral("--shafts-only")))
        return 0;
    indentedTriangleStyleRoundTrips();
    deleteKeyRemovesEditedText();
    widgetLifecycle();
    arrowLabelWheelChangesFontSizeWhileSelecting();
    wrappingAndFinalPointerPosition();
    gapPreservesBackground();
    arrowTypesDragLabelAlongPathAndEraseAsPair();
    deleteAllElementsClearsDocumentAsOneUndoEntry();
    sharedViewsAndLongOffscreenText();
    boundShapeReroutesAndMeasuresLabel();
    std::cout << "Arrow text tests passed\n";
}
