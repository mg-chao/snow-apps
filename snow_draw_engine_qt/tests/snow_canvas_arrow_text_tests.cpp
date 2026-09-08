#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_text_editor_session.h"
#include "snow_canvas_text.h"

#include <QApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <cstdlib>
#include <iostream>

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
            for (int head = SNOW_ARROWHEAD_NONE; head <= SNOW_ARROWHEAD_INVERTED_TRIANGLE; ++head) {
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

void arrowTypesMoveAndEraseAsPair() {
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
                "dragging label moves its owner");
        require(after.value(QStringLiteral("font_size")) ==
                    before.value(QStringLiteral("font_size")),
                "arrow transforms preserve label font size");
        require(canvas.setCanvasTool(SnowCanvasTool::Eraser), "activate eraser");
        const QPointF erasePoint = point + QPointF(30.0, 40.0);
        mouse(canvas, QEvent::MouseButtonPress, erasePoint, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, erasePoint, Qt::LeftButton, Qt::NoButton);
        require(records(runtime, QStringLiteral("Arrow")).isEmpty() &&
                    records(runtime, QStringLiteral("Text")).isEmpty(),
                "erasing label deletes the pair");
        require(canvas.undo(), "undo pair erasure");
        require(records(runtime, QStringLiteral("Text")).size() == 1, "undo restores owned text");
    }
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
    widgetLifecycle();
    wrappingAndFinalPointerPosition();
    gapPreservesBackground();
    arrowTypesMoveAndEraseAsPair();
    sharedViewsAndLongOffscreenText();
    boundShapeReroutesAndMeasuresLabel();
    std::cout << "Arrow text tests passed\n";
}
