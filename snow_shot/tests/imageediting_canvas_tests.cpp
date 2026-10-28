// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "canvas_text_editing_preferences_test_support.h"

#include <QApplication>
#include <QEnterEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QtMath>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage renderWidget(QWidget& widget, qreal dpr) {
    QImage image(QSize(qCeil(widget.width() * dpr), qCeil(widget.height() * dpr)),
                 QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::magenta);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

void drawExportedPreview(QPainter& painter, const QRectF& imageRect, const QImage& exported) {
    painter.save();
    // The editor composites its complete scene over the host background. Source and
    // SourceOver can round differently during resampling, even for opaque source pixels.
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const auto deviceRect = painter.deviceTransform().mapRect(imageRect);
    const QSize deviceSize(qRound(deviceRect.width()), qRound(deviceRect.height()));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, deviceSize != exported.size());
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(imageRect, Qt::IntersectClip);
    painter.drawImage(imageRect, exported);
    painter.restore();
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, point, point, canvas.mapToGlobal(point), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
    QApplication::processEvents();
}

void drag(SnowCanvasWidget& canvas, QPointF start, QPointF end,
          QEvent::Type press = QEvent::MouseButtonPress) {
    mouse(canvas, press, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto elementSlots = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                  .object()
                                  .value(QStringLiteral("document"))
                                  .toObject()
                                  .value(QStringLiteral("slots"))
                                  .toArray();
    QJsonArray result;
    for (const auto& slot : elementSlots) {
        if (slot.toObject().value(QStringLiteral("data")).toObject().contains(kind))
            result.append(slot);
    }
    return result;
}

QJsonObject rectangle(const SnowCanvasRuntime& runtime) {
    const auto shapes = records(runtime, QStringLiteral("Rectangle"));
    require(shapes.size() == 1, "canvas fixture retains exactly one rectangle");
    return shapes.first()
        .toObject()
        .value(QStringLiteral("data"))
        .toObject()
        .value(QStringLiteral("Rectangle"))
        .toObject();
}

struct Fixture {
    ImageEditingDocument document;
    ImageEditingCanvas canvas{document};

    Fixture() {
        canvas.resize(480, 400);
        canvas.show();
        QApplication::processEvents();
        ImageEditingSource source;
        source.image = QImage(240, 160, QImage::Format_ARGB32_Premultiplied);
        source.image.fill(QColor(40, 50, 60));
        source.canvasRect = QRectF(0, 0, 240, 160);
        require(document.load(source), "load canvas fixture image");
        canvas.setMode(ImageEditingCanvas::Mode::Draw);
        require(canvas.setViewportCamera(120, 80, 1), "use a deterministic image camera");
        require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate canvas shape tool");
    }

    QPointF view(QPointF canvasPoint) const {
        return canvas.canvasToViewTransform().map(canvasPoint);
    }

    void addRectangle(const QRectF& bounds) {
        const QJsonObject operation{
            {QStringLiteral("type"), QStringLiteral("rectangle")},
            {QStringLiteral("bounds"),
             QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()}},
            {QStringLiteral("style"),
             QJsonObject{{QStringLiteral("fill"), QJsonArray{255, 0, 0, 255}},
                         {QStringLiteral("stroke"), QJsonArray{0, 0, 0, 0}}}}};
        require(!document.runtime()
                     .applyAnnotationTransaction(
                         QJsonDocument(
                             QJsonObject{{QStringLiteral("version"), 1},
                                         {QStringLiteral("operations"), QJsonArray{operation}}})
                             .toJson(QJsonDocument::Compact))
                     .isEmpty(),
                "add canvas fixture rectangle");
        require(canvas.resetEditingState(), "clear fixture selection");
        require(canvas.setCanvasTool(SnowCanvasTool::Shape), "restore shape tool");
    }

    void selectRectangle() {
        const auto id = records(document.runtime(), QStringLiteral("Rectangle"))
                            .first()
                            .toObject()
                            .value(QStringLiteral("id"))
                            .toObject();
        SnowCanvasRuntimeEditor editor(document.runtime());
        require(editor.select(static_cast<quint32>(id.value(QStringLiteral("index")).toInt()),
                              static_cast<quint32>(id.value(QStringLiteral("generation")).toInt())),
                "select canvas fixture rectangle");
        QApplication::processEvents();
    }

    QImage exported() {
        return document.runtime().renderToImage(document.bounds(), document.image().size(),
                                                {{document.image(), document.bounds()}});
    }
};

ImageEditingSource patternedSource() {
    ImageEditingSource source;
    source.image = QImage(640, 400, QImage::Format_ARGB32_Premultiplied);
    source.canvasRect = QRectF(-25.5, 17.25, 320, 200);
    for (int y = 0; y < source.image.height(); ++y) {
        for (int x = 0; x < source.image.width(); ++x) {
            source.image.setPixelColor(
                x, y,
                QColor((x * 17 + y * 3) % 256, (x * 5 + y * 19) % 256, (x * 11 + y * 7) % 256));
        }
    }
    return source;
}

void backgroundCallbacksPreserveTileProjection() {
    Fixture fixture;
    auto source = patternedSource();
    for (int y = 0; y < source.image.height(); ++y)
        for (int x = 0; x < source.image.width(); ++x) {
            auto color = source.image.pixelColor(x, y);
            color.setAlpha((x * 11 + y * 17) % 256);
            source.image.setPixelColor(x, y, color);
        }
    require(fixture.document.load(source), "load translucent tile projection fixture");
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        const SnowCanvasRenderContext context{QRect(0, 0, 480, 320),
                                              QRegion(QRect(111, 73, 97, 81)),
                                              QTransform(1.25, 0, 0, 1.25, 90.25, 31.5), dpr};
        for (const bool original : {false, true}) {
            QImage expected(QSize(qCeil(97 * dpr), qCeil(81 * dpr)),
                            QImage::Format_ARGB32_Premultiplied);
            expected.setDevicePixelRatio(dpr);
            expected.fill(Qt::transparent);
            auto actual = expected;
            QPainter reference(&expected);
            reference.translate(-111, -73);
            reference.setRenderHint(QPainter::SmoothPixmapTransform, true);
            reference.drawImage(context.canvasToViewTransform.mapRect(source.canvasRect),
                                source.image);
            reference.end();
            QPainter painter(&actual);
            painter.translate(-111, -73);
            if (original)
                fixture.canvas.customRenderer()->renderOriginalBackground(painter, context);
            else
                fixture.canvas.customRenderer()->renderBeforeCanvas(painter, context);
            painter.end();
            require(actual == expected,
                    "image backgrounds retain tile offsets, camera projection, DPR and alpha");
        }
    }
}

void filtersAndErasersMatchExportedImage() {
    const auto source = patternedSource();
    for (const auto type : {SnowCanvasFilterType::Inversion, SnowCanvasFilterType::GaussianBlur,
                            SnowCanvasFilterType::RestoreBackground}) {
        for (const bool pen : {false, true}) {
            Fixture fixture;
            require(fixture.document.load(source), "load patterned effect fixture");
            const auto center = source.canvasRect.center();
            require(fixture.canvas.setViewportCamera(center.x(), center.y(), 1),
                    "position effect fixture camera");
            if (type == SnowCanvasFilterType::RestoreBackground)
                fixture.addRectangle(source.canvasRect.adjusted(20, 20, -20, -20));
            const auto tool =
                type == SnowCanvasFilterType::RestoreBackground
                    ? (pen ? SnowCanvasTool::BrushEraser : SnowCanvasTool::RectangleEraser)
                    : (pen ? SnowCanvasTool::PenFilter : SnowCanvasTool::RectangleFilter);
            require(fixture.document.runtime().setQuickSelectionDisabledTools({tool}),
                    "effect gestures create new elements over existing annotations");
            if (type != SnowCanvasFilterType::RestoreBackground)
                require(fixture.canvas.setCanvasFilterStyle(
                            {type, 0.65, 1.0, 24}, SnowCanvasFilterStylePropertyType |
                                                       SnowCanvasFilterStylePropertyStrength |
                                                       SnowCanvasFilterStylePropertyOpacity |
                                                       SnowCanvasFilterStylePropertyStrokeWidth),
                        "configure effect fixture");
            require(fixture.canvas.setCanvasBrushEraserCreationStyle({24}),
                    "configure brush eraser fixture");
            for (int stroke = 0; stroke < 2; ++stroke) {
                require(fixture.canvas.setCanvasTool(tool), "activate effect fixture tool");
                drag(fixture.canvas,
                     fixture.view(source.canvasRect.topLeft() + QPointF(70 + stroke * 35, 45)),
                     fixture.view(source.canvasRect.topLeft() + QPointF(250, 155 - stroke * 25)));
                require(fixture.canvas.resetEditingState() &&
                            fixture.canvas.setCanvasTool(SnowCanvasTool::Select),
                        "hide effect selection and cursor overlays");
                // Warm the retained source raster before the next edit damages a subregion.
                static_cast<void>(fixture.canvas.grab());
            }
            const auto exported = fixture.exported();
            require(exported != source.image, "effect fixture changes document pixels");
            for (const qreal zoom : {1.0, 1.5}) {
                require(fixture.canvas.setViewportCamera(center.x() + 5.25, center.y() - 3.5, zoom),
                        "pan and zoom effect fixture");
                const auto imageRect =
                    fixture.canvas.canvasToViewTransform().mapRect(source.canvasRect);
                const auto verifyPreview = [&](const QImage& actual) {
                    const qreal dpr = actual.devicePixelRatio();
                    QImage expected(actual.size(), actual.format());
                    expected.setDevicePixelRatio(dpr);
                    expected.fill(Qt::transparent);
                    QPainter reference(&expected);
                    drawExportedPreview(reference, imageRect, exported);
                    reference.end();
                    // Exclude widget chrome and image-edge clipping; compare every document
                    // pixel across the effect boundary and the untouched surrounding image.
                    const auto interior = QTransform::fromScale(dpr, dpr)
                                              .mapRect(imageRect.adjusted(2, 2, -2, -2))
                                              .toAlignedRect()
                                              .intersected(actual.rect());
                    require(actual.copy(interior) == expected.copy(interior),
                            qPrintable(QStringLiteral("effect preview matches export (type %1, "
                                                      "pen %2, zoom %3, DPR %4)")
                                           .arg(static_cast<int>(type))
                                           .arg(pen)
                                           .arg(zoom)
                                           .arg(dpr)));
                };
                verifyPreview(fixture.canvas.grab().toImage());
                // Exercise fractional sampling on every run, regardless of the display DPR.
                for (const qreal dpr : {1.0, 1.25, 1.5, 2.0})
                    verifyPreview(renderWidget(fixture.canvas, dpr));
            }
        }
    }
}

void effectsCompositeOverTheHostBackground() {
    class Checkerboard final : public QWidget {
      public:
        static void paint(QPainter& painter, const QRect& bounds) {
            painter.fillRect(bounds, Qt::white);
            constexpr int tile = 10;
            for (int y = 0; y < bounds.height(); y += tile)
                for (int x = 0; x < bounds.width(); x += tile)
                    if ((x / tile + y / tile) % 2)
                        painter.fillRect(QRect(x, y, tile, tile), QColor(240, 240, 240));
        }
        void paintEvent(QPaintEvent*) override {
            QPainter painter(this);
            paint(painter, rect());
        }
    };
    for (const auto tool : {SnowCanvasTool::RectangleFilter, SnowCanvasTool::PenFilter,
                            SnowCanvasTool::RectangleEraser, SnowCanvasTool::BrushEraser}) {
        for (const int alpha : {255, 96}) {
            for (const bool spotlight : {false, true}) {
                Checkerboard host;
                host.resize(480, 400);
                Fixture fixture;
                fixture.canvas.setParent(&host);
                fixture.canvas.setGeometry(host.rect());
                fixture.canvas.show();
                host.show();
                QApplication::processEvents();
                ImageEditingSource source;
                source.image = QImage(240, 160, QImage::Format_ARGB32_Premultiplied);
                source.image.fill(QColor(245, 245, 245, alpha));
                // Transparent pixels touch an image edge and the eraser's path.
                QPainter sourcePainter(&source.image);
                sourcePainter.setCompositionMode(QPainter::CompositionMode_Source);
                sourcePainter.fillRect(QRect(0, 60, 100, 50), Qt::transparent);
                sourcePainter.end();
                source.canvasRect = QRectF(-25.5, 17.25, 240, 160);
                require(fixture.document.load(source), "load host composition fixture");
                const auto center = source.canvasRect.center();
                require(fixture.canvas.setViewportCamera(center.x(), center.y(), 1),
                        "position host composition fixture");
                const bool eraser =
                    tool == SnowCanvasTool::RectangleEraser || tool == SnowCanvasTool::BrushEraser;
                if (eraser)
                    fixture.addRectangle(source.canvasRect.adjusted(20, 20, -20, -20));
                if (spotlight) {
                    require(
                        !fixture.document.runtime()
                             .applyAnnotationTransaction(QByteArrayLiteral(
                                 R"({"version":1,"operations":[{"type":"spotlight","bounds":[90,70,30,20]}]})"))
                             .isEmpty(),
                        "add a source-grid spotlight backdrop");
                    require(fixture.canvas.setCanvasSpotlightConfig({QColor(20, 40, 60), 0.4}),
                            "configure translucent spotlight backdrop");
                }
                require(fixture.document.runtime().setQuickSelectionDisabledTools({tool}),
                        "effect gestures create new elements");
                require(fixture.canvas.setCanvasFilterStyle(
                            {SnowCanvasFilterType::Inversion, 1, 1, 24},
                            SnowCanvasFilterStylePropertyType |
                                SnowCanvasFilterStylePropertyStrength |
                                SnowCanvasFilterStylePropertyOpacity |
                                SnowCanvasFilterStylePropertyStrokeWidth),
                        "configure host composition filter");
                require(fixture.canvas.setCanvasBrushEraserCreationStyle({24}),
                        "configure host composition eraser");
                require(fixture.canvas.setCanvasTool(tool), "activate host composition effect");
                drag(fixture.canvas, fixture.view(source.canvasRect.topLeft() + QPointF(35, 35)),
                     fixture.view(source.canvasRect.topLeft() + QPointF(150, 95)));
                require(fixture.canvas.resetEditingState() &&
                            fixture.canvas.setCanvasTool(SnowCanvasTool::Select),
                        "hide effect controls");
                const auto exported = fixture.exported();
                require(!exported.isNull() && exported != source.image,
                        "host composition fixture applies its effect");
                for (const qreal zoom : {0.65, 1.2}) {
                    require(fixture.canvas.setViewportCamera(center.x() + 0.25, center.y() - 0.35,
                                                             zoom),
                            "place image edges between physical pixels");
                    const auto imageRect =
                        fixture.canvas.canvasToViewTransform().mapRect(source.canvasRect);
                    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                        QImage expected(
                            QSize(qCeil(host.width() * dpr), qCeil(host.height() * dpr)),
                            QImage::Format_RGB32);
                        expected.setDevicePixelRatio(dpr);
                        QPainter reference(&expected);
                        Checkerboard::paint(reference, host.rect());
                        drawExportedPreview(reference, imageRect, exported);
                        reference.end();
                        auto actual = renderWidget(host, dpr);
                        require(actual == expected,
                                qPrintable(QStringLiteral("complete preview composites over host "
                                                          "(tool %1, alpha %2, spotlight %3, "
                                                          "zoom %4, DPR %5)")
                                               .arg(static_cast<int>(tool))
                                               .arg(alpha)
                                               .arg(spotlight)
                                               .arg(zoom)
                                               .arg(dpr)));
                        // Repainting disjoint edge strips must restore the parent before
                        // compositing alpha again, while retaining the unexposed center.
                        const QRegion damage =
                            QRegion(host.rect()) - QRegion(QRect(180, 150, 120, 100));
                        // Qt's clipped bilinear spans can round differently from full spans;
                        // independently compose the export with the same damage region.
                        reference.begin(&expected);
                        reference.setClipRegion(damage);
                        Checkerboard::paint(reference, host.rect());
                        drawExportedPreview(reference, imageRect, exported);
                        reference.end();
                        QPainter painter(&actual);
                        host.render(&painter, damage.boundingRect().topLeft(), damage);
                        painter.end();
                        require(actual == expected,
                                "partial edge repaints preserve alpha and the untouched center");
                    }
                }
            }
        }
    }
}

void resizeControlsRemainVisibleAndUsableAtEveryImageEdge() {
    struct Edge {
        QRectF shape;
        QPointF handle;
        QPointF dragOffset;
        QRectF externalArea;
    };
    // Selection frames sit four view pixels outside the shape. Each sampled
    // area lies wholly beyond the image, where clipping used to hide the handle.
    const std::array edges{
        Edge{{1, 50, 50, 40}, {-3, 46}, {-16, -10}, {-7, 41, 7, 11}},
        Edge{{189, 50, 50, 40}, {243, 46}, {16, -10}, {241, 41, 7, 11}},
        Edge{{90, 1, 50, 40}, {86, -3}, {-10, -16}, {81, -7, 11, 7}},
        Edge{{90, 119, 50, 40}, {144, 163}, {10, 16}, {139, 161, 11, 7}},
    };
    for (const auto& edge : edges) {
        Fixture fixture;
        fixture.addRectangle(edge.shape);
        const auto area =
            fixture.canvas.canvasToViewTransform().mapRect(edge.externalArea).toAlignedRect();
        const auto withoutControl = fixture.canvas.grab(area).toImage();
        const auto exported = fixture.exported();
        fixture.selectRectangle();
        require(fixture.canvas.grab(area).toImage() != withoutControl,
                "selection resize controls remain visible beyond every image edge");
        require(fixture.exported() == exported, "editor controls do not enter exported pixels");
        const auto before = rectangle(fixture.document.runtime());
        drag(fixture.canvas, fixture.view(edge.handle),
             fixture.view(edge.handle + edge.dragOffset));
        require(rectangle(fixture.document.runtime()) != before &&
                    records(fixture.document.runtime(), QStringLiteral("Rectangle")).size() == 1,
                "an outside-image resize control modifies the existing shape");
    }
}

void rotationAndTextControlsRemainUsableOutsideImage() {
    Fixture rotation;
    rotation.addRectangle({90, 1, 50, 40});
    const auto rotationPoint = rotation.view({115, -23});
    const QRect rotationArea(rotationPoint.toPoint() - QPoint(6, 6), QSize(13, 13));
    const auto withoutRotation = rotation.canvas.grab(rotationArea).toImage();
    rotation.selectRectangle();
    require(rotation.canvas.grab(rotationArea).toImage() != withoutRotation,
            "the rotation control remains visible above the image");
    drag(rotation.canvas, rotationPoint, rotationPoint + QPointF(25, 10));
    require(
        std::abs(
            rectangle(rotation.document.runtime()).value(QStringLiteral("rotation")).toDouble()) >
            0.1,
        "the outside-image rotation control rotates the existing shape");

    for (const auto press : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
        Fixture text;
        text.addRectangle({90, 119, 50, 40});
        const auto textPoint = text.view({115, 183});
        const QRect textArea(textPoint.toPoint() - QPoint(6, 6), QSize(13, 13));
        const auto withoutText = text.canvas.grab(textArea).toImage();
        text.selectRectangle();
        require(text.canvas.grab(textArea).toImage() != withoutText,
                "the attached-text control remains visible below the image");
        drag(text.canvas, textPoint, textPoint, press);
        require(text.canvas.hasActiveTextEditing(),
                "single and double clicks on external controls open attached text editing");
        QKeyEvent input(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("A"));
        QApplication::sendEvent(&text.canvas, &input);
        mouse(text.canvas, QEvent::MouseButtonPress, textPoint, Qt::LeftButton, Qt::LeftButton);
        mouse(text.canvas, QEvent::MouseButtonRelease, textPoint, Qt::LeftButton, Qt::NoButton);
        require(text.canvas.hasActiveTextEditing(),
                "an active inline editor remains interactive outside the image");
        QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
        QApplication::sendEvent(&text.canvas, &commit);
        require(!text.canvas.hasActiveTextEditing() &&
                    records(text.document.runtime(), QStringLiteral("Text")).size() == 1,
                "attached text commits as a single existing-shape editing operation");
    }
}

void documentPixelsRemainClippedWhileControlsAreVisible() {
    Fixture fixture;
    const QRect external =
        fixture.canvas.canvasToViewTransform().mapRect(QRectF(-12, 56, 8, 28)).toAlignedRect();
    const auto emptyPixels = fixture.canvas.grab(external).toImage();
    fixture.addRectangle({-15, 50, 30, 40});
    require(fixture.canvas.grab(external).toImage() == emptyPixels,
            "document pixels extending past the image remain clipped in the editor");
    const auto exported = fixture.exported();
    require(exported.size() == fixture.document.image().size() &&
                exported.pixelColor(0, 70) == QColor(Qt::red),
            "export keeps image dimensions and includes the visible part of the annotation");
    fixture.selectRectangle();
    require(fixture.exported() == exported,
            "export excludes selection overlays beyond image edges");
}

void blankOutsideGesturesCannotCreateAnnotations() {
    for (const auto tool :
         {SnowCanvasTool::Shape, SnowCanvasTool::Arrow, SnowCanvasTool::Text,
          SnowCanvasTool::FreeDraw, SnowCanvasTool::PenFilter, SnowCanvasTool::BrushEraser}) {
        for (const auto press : {QEvent::MouseButtonPress, QEvent::MouseButtonDblClick}) {
            Fixture fixture;
            require(fixture.canvas.setCanvasTool(tool), "activate outside-gesture fixture tool");
            const auto before = fixture.document.checkpoint();
            drag(fixture.canvas, fixture.view({-30, 80}), fixture.view({100, 80}), press);
            require(fixture.document.checkpoint() == before &&
                        !fixture.canvas.hasActiveTextEditing(),
                    "blank outside-image mouse gestures cannot create annotations or drafts");
        }
    }
    Fixture fixture;
    require(fixture.canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "activate native tablet tool");
    const auto before = fixture.document.checkpoint();
    const QPointingDevice pen(
        QStringLiteral("image editor test pen"), 54, QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
    const auto tablet = [&](QEvent::Type type, QPointF position, quint64 timestamp) {
        QTabletEvent event(type, &pen, position, fixture.canvas.mapToGlobal(position), 0.5, 0, 0, 0,
                           0, 0, Qt::NoModifier,
                           type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                           type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
        event.setTimestamp(timestamp);
        QApplication::sendEvent(&fixture.canvas, &event);
    };
    tablet(QEvent::TabletPress, fixture.view({-30, 80}), 100);
    tablet(QEvent::TabletMove, fixture.view({100, 80}), 110);
    tablet(QEvent::TabletRelease, fixture.view({100, 80}), 120);
    require(fixture.document.checkpoint() == before,
            "blank outside-image native tablet gestures cannot create annotations");

    fixture.canvas.setMode(ImageEditingCanvas::Mode::CropRows);
    int crops = 0;
    fixture.canvas.cropRequested = [&](ImageEditingDocument::CropAxis, int, int) { ++crops; };
    drag(fixture.canvas, fixture.view({-30, 80}), fixture.view({100, 120}));
    require(crops == 0, "crop gestures still require an image starting point");
    drag(fixture.canvas, fixture.view({100, 80}), fixture.view({100, 120}));
    require(crops == 1, "inside-image crop gestures remain functional");
}

QRect changedPixels(const QImage& before, const QImage& after) {
    QRect changed;
    for (int y = 0; y < before.height(); ++y)
        for (int x = 0; x < before.width(); ++x)
            if (before.pixel(x, y) != after.pixel(x, y))
                changed |= QRect(x, y, 1, 1);
    return changed;
}

void requireCropGuide(const QImage& image, QRectF guide) {
    const qreal dpr = image.devicePixelRatio();
    const auto pixels = QTransform::fromScale(dpr, dpr)
                            .mapRect(guide.adjusted(-2, -2, 2, 2))
                            .toAlignedRect()
                            .intersected(image.rect());
    int blue = 0;
    for (int y = pixels.top(); y <= pixels.bottom(); ++y)
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const auto color = image.pixelColor(x, y);
            blue += color.blue() > color.green() + 25 && color.green() > color.red() + 10;
        }
    const auto minimum = std::max(guide.width(), guide.height()) * dpr / 8;
    require(blue > minimum, "crop boundaries have image-splitting blue markers across the image");
}

void cropGuidesTrackRoundedImagePixels() {
    for (const auto mode :
         {ImageEditingCanvas::Mode::CropRows, ImageEditingCanvas::Mode::CropColumns}) {
        Fixture fixture;
        auto source = patternedSource();
        source.image.fill(QColor(40, 50, 60));
        require(fixture.document.load(source), "load crop guide image with offset scaled bounds");
        fixture.canvas.setMode(mode);
        const bool rows = mode == ImageEditingCanvas::Mode::CropRows;
        const auto bounds = fixture.document.bounds();
        // Both image axes use two source pixels per canvas unit.
        const auto hover = bounds.topLeft() + QPointF(63.2, 52.7);
        const auto snapped = bounds.topLeft() + QPointF(63, 52.5);
        for (const qreal zoom : {0.75, 1.0, 1.25}) {
            require(fixture.canvas.setViewportCamera(bounds.center().x() + 8,
                                                     bounds.center().y() - 7, zoom),
                    "zoom and pan the crop guide fixture");
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&fixture.canvas, &leave);
            std::array<QImage, 3> before;
            const std::array<qreal, 3> dprs{1.0, 1.25, 2.0};
            for (size_t i = 0; i < dprs.size(); ++i)
                before[i] = renderWidget(fixture.canvas, dprs[i]);
            mouse(fixture.canvas, QEvent::MouseMove, fixture.view(hover));
            const QRectF guide = fixture.canvas.canvasToViewTransform().mapRect(
                rows ? QRectF(bounds.x(), snapped.y(), bounds.width(), 0)
                     : QRectF(snapped.x(), bounds.y(), 0, bounds.height()));
            for (size_t i = 0; i < dprs.size(); ++i) {
                const auto after = renderWidget(fixture.canvas, dprs[i]);
                requireCropGuide(after, guide);
                const auto expected = QTransform::fromScale(dprs[i], dprs[i])
                                          .mapRect(guide.adjusted(-2, -2, 2, 2))
                                          .toAlignedRect();
                const auto changed = changedPixels(before[i], after);
                require(!changed.isEmpty() && expected.contains(changed),
                        "hover paints only the guide at the rounded source-pixel boundary");
                const auto imageRect =
                    QTransform::fromScale(dprs[i], dprs[i])
                        .mapRect(fixture.canvas.canvasToViewTransform().mapRect(bounds))
                        .toAlignedRect();
                require(imageRect.contains(changed), "crop guides stay within the image");
            }
            mouse(fixture.canvas, QEvent::MouseMove,
                  fixture.view(bounds.topLeft() - QPointF(5, 5)));
            require(renderWidget(fixture.canvas, 1) == before[0],
                    "moving outside the image removes the crop guide");
        }
        require(fixture.document.image() == source.image && !fixture.document.dirty() &&
                    fixture.exported() == source.image,
                "hover guides leave document pixels, history, and exported images unchanged");
    }
}

void cropGuidesRemainVisibleAtImageEdges() {
    for (const auto mode :
         {ImageEditingCanvas::Mode::CropRows, ImageEditingCanvas::Mode::CropColumns}) {
        for (const auto color : {Qt::black, Qt::white}) {
            Fixture fixture;
            ImageEditingSource source;
            source.image = QImage(240, 160, QImage::Format_ARGB32_Premultiplied);
            source.image.fill(color);
            source.canvasRect = QRectF(0, 0, 240, 160);
            require(fixture.document.load(source), "load crop edge contrast fixture");
            require(fixture.canvas.setViewportCamera(120, 80, 1), "set crop edge camera");
            const auto before = renderWidget(fixture.canvas, 1);
            fixture.canvas.setMode(mode);
            for (const auto point : {QPointF(0.1, 0.1), QPointF(239.9, 159.9)}) {
                mouse(fixture.canvas, QEvent::MouseMove, fixture.view(point));
                const auto changed = changedPixels(before, renderWidget(fixture.canvas, 1));
                require(!changed.isEmpty(),
                        "crop guides remain visible at either image edge on light and dark images");
            }
        }
    }
}

void cropGuidesFollowPointerAndModeChanges() {
    Fixture fixture;
    fixture.canvas.actualSize();
    const auto before = renderWidget(fixture.canvas, 1);
    const auto point = fixture.view({83.3, 68.2});
    mouse(fixture.canvas, QEvent::MouseMove, point);
    fixture.canvas.setMode(ImageEditingCanvas::Mode::CropRows);
    require(renderWidget(fixture.canvas, 1) != before,
            "activating crop shows a guide at the existing pointer position");
    fixture.canvas.setMode(ImageEditingCanvas::Mode::Pan);
    require(renderWidget(fixture.canvas, 1) == before, "pan mode removes the crop guide");
    fixture.canvas.setMode(ImageEditingCanvas::Mode::CropColumns);
    const auto columns = renderWidget(fixture.canvas, 1);
    fixture.canvas.setMode(ImageEditingCanvas::Mode::Draw);
    require(renderWidget(fixture.canvas, 1) == before, "draw mode removes the crop guide");
    fixture.canvas.setMode(ImageEditingCanvas::Mode::CropColumns);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&fixture.canvas, &leave);
    require(renderWidget(fixture.canvas, 1) == before, "leaving the canvas removes the crop guide");
    QEnterEvent enter(point, point, fixture.canvas.mapToGlobal(point));
    QApplication::sendEvent(&fixture.canvas, &enter);
    require(renderWidget(fixture.canvas, 1) == columns,
            "entering the canvas restores the guide without requiring mouse motion");

    mouse(fixture.canvas, QEvent::MouseButtonPress, point, Qt::MiddleButton, Qt::MiddleButton);
    require(renderWidget(fixture.canvas, 1) == before, "panning hides the hover guide");
    const auto moved = point + QPointF(21, 13);
    mouse(fixture.canvas, QEvent::MouseMove, moved, Qt::NoButton, Qt::MiddleButton);
    mouse(fixture.canvas, QEvent::MouseButtonRelease, moved, Qt::MiddleButton);
    const auto verifyAtPointer = [&] {
        const auto bounds = fixture.document.bounds();
        const auto canvasPoint = fixture.canvas.canvasToViewTransform().inverted().map(moved);
        const auto pixel = fixture.document.imagePixelAt(canvasPoint);
        const qreal x =
            bounds.x() + qRound(pixel.x()) * bounds.width() / fixture.document.image().width();
        requireCropGuide(renderWidget(fixture.canvas, 1),
                         fixture.canvas.canvasToViewTransform().mapRect(
                             QRectF(x, bounds.y(), 0, bounds.height())));
    };
    verifyAtPointer();
    QWheelEvent wheel(moved, fixture.canvas.mapToGlobal(moved), {}, {0, 120}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&fixture.canvas, &wheel);
    verifyAtPointer();
    fixture.canvas.fit();
    verifyAtPointer();
}

void cropPreviewsRepaintOnlyTheirDamageAndCancelCleanly() {
    for (const auto mode :
         {ImageEditingCanvas::Mode::CropRows, ImageEditingCanvas::Mode::CropColumns}) {
        Fixture fixture;
        fixture.canvas.setMode(mode);
        mouse(fixture.canvas, QEvent::MouseMove, fixture.view({26, 26}));
        QApplication::processEvents();
        const auto before = fixture.canvas.grab().toImage();
        class PaintRegions final : public QObject {
          public:
            QRegion painted;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::Paint)
                    painted += static_cast<QPaintEvent*>(event)->region();
                return false;
            }
        } events;
        fixture.canvas.installEventFilter(&events);
        mouse(fixture.canvas, QEvent::MouseButtonPress, fixture.view({20, 20}), Qt::LeftButton,
              Qt::LeftButton);
        const bool rows = mode == ImageEditingCanvas::Mode::CropRows;
        const auto guideAt = [&](qreal position) {
            const auto bounds = fixture.document.bounds();
            return fixture.canvas.canvasToViewTransform().mapRect(
                rows ? QRectF(bounds.x(), position, bounds.width(), 0)
                     : QRectF(position, bounds.y(), 0, bounds.height()));
        };
        requireCropGuide(renderWidget(fixture.canvas, 1), guideAt(20));
        events.painted = {};
        mouse(fixture.canvas, QEvent::MouseMove, fixture.view({26, 26}), Qt::NoButton,
              Qt::LeftButton);
        const auto damage = events.painted.boundingRect();
        require(!damage.isEmpty() && (mode == ImageEditingCanvas::Mode::CropRows
                                          ? damage.height() < fixture.canvas.height() / 2
                                          : damage.width() < fixture.canvas.width() / 2),
                "crop motion repaints the preview strip without repainting the full canvas");
        const auto dragging = renderWidget(fixture.canvas, 1);
        requireCropGuide(dragging, guideAt(20));
        requireCropGuide(dragging, guideAt(26));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&fixture.canvas, &escape);
        QApplication::processEvents();
        require(fixture.canvas.grab().toImage() == before && !fixture.document.dirty(),
                "cancelling the preview restores the hover guide without editing the document");
        int crops = 0;
        fixture.canvas.cropRequested = [&](ImageEditingDocument::CropAxis axis, int first,
                                           int last) {
            require(axis == (rows ? ImageEditingDocument::CropAxis::Rows
                                  : ImageEditingDocument::CropAxis::Columns) &&
                        first == 80 && last == 26,
                    "guides preserve the reverse drag's requested crop pixels");
            ++crops;
        };
        mouse(fixture.canvas, QEvent::MouseButtonPress, fixture.view({80, 80}), Qt::LeftButton,
              Qt::LeftButton);
        mouse(fixture.canvas, QEvent::MouseMove, fixture.view({26, 26}), Qt::NoButton,
              Qt::LeftButton);
        requireCropGuide(renderWidget(fixture.canvas, 1), guideAt(80));
        requireCropGuide(renderWidget(fixture.canvas, 1), guideAt(26));
        mouse(fixture.canvas, QEvent::MouseButtonRelease, fixture.view({26, 26}), Qt::LeftButton);
        require(
            crops == 1 && fixture.canvas.grab().toImage() == before,
            "releasing a reverse crop drag removes both boundaries and restores the hover guide");
    }
}
} // namespace

void runImageEditingCanvasTests() {
    effectsCompositeOverTheHostBackground();
    filtersAndErasersMatchExportedImage();
    backgroundCallbacksPreserveTileProjection();
    resizeControlsRemainVisibleAndUsableAtEveryImageEdge();
    rotationAndTextControlsRemainUsableOutsideImage();
    documentPixelsRemainClippedWhileControlsAreVisible();
    blankOutsideGesturesCannotCreateAnnotations();
    cropGuidesTrackRoundedImagePixels();
    cropGuidesRemainVisibleAtImageEdges();
    cropGuidesFollowPointerAndModeChanges();
    cropPreviewsRepaintOnlyTheirDamageAndCancelCleanly();
}

void runImageEditingTextPreferencesTests() {
    canvas_text_editing_preferences_test::exercise([](auto verify) {
        snow_shot::presentation::ImageEditingDocument document;
        snow_shot::presentation::ImageEditingCanvas canvas(document);
        verify(canvas);
    });
}
