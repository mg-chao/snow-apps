#pragma once

#include "snow_canvas_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <vector>

namespace {
inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class PatternRenderer final : public SnowCanvasCustomRenderer {
  public:
    PatternRenderer(const QSize& pixelSize, const QRectF& bounds) : reference{bounds, 1.0} {
        reference.pixelsPerCanvasUnit = pixelSize.width() / bounds.width();
        image = QImage(pixelSize, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixelColor(
                    x, y,
                    QColor((x * 17 + y * 3) % 256, (x * 5 + y * 19) % 256, (x * 11 + y * 7) % 256));
            }
        }
    }

    std::optional<SnowCanvasFilterRenderReference> filterRenderReference() const override {
        return enabled ? std::optional(reference) : std::nullopt;
    }
    std::uint64_t contentRevision() const override {
        return revision;
    }
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        ++beforeCalls;
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(context.viewportRect, Qt::transparent);
        painter.drawImage(context.canvasToViewTransform.mapRect(reference.canvasRect), image);
    }
    QImage image;
    SnowCanvasFilterRenderReference reference;
    bool enabled = true;
    std::uint64_t revision = 0;
    int beforeCalls = 0;
};

struct Fixture {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas{runtime};
    PatternRenderer renderer;

    explicit Fixture(QSize pixelSize = {160, 120}, QRectF bounds = {20.25, -10.5, 160, 120})
        : renderer(pixelSize, bounds) {
        canvas.resize(pixelSize);
        canvas.setClearBackgroundEnabled(false);
        canvas.setCustomRenderer(&renderer);
        canvas.show();
        QApplication::processEvents();
        const QPointF center = bounds.center();
        require(canvas.setViewportCamera(center.x(), center.y(),
                                         renderer.reference.pixelsPerCanvasUnit),
                "configure reference viewport");
        require(runtime.setQuickSelectionDisabledTools({SnowCanvasTool::RectangleFilter,
                                                        SnowCanvasTool::PenFilter,
                                                        SnowCanvasTool::Shape}),
                "disable quick selection in fixture");
    }
    ~Fixture() {
        canvas.setCustomRenderer(nullptr);
    }

    void draw(SnowCanvasTool tool, QPointF start, QPointF end) {
        require(canvas.resetEditingStatePreservingTool(), "reset drawing state");
        require(canvas.setCanvasTool(tool), "activate drawing tool");
        for (const auto& [type, position, button, buttons] :
             {std::tuple{QEvent::MouseButtonPress, start, Qt::LeftButton,
                         Qt::MouseButtons(Qt::LeftButton)},
              std::tuple{QEvent::MouseMove, end, Qt::NoButton, Qt::MouseButtons(Qt::LeftButton)},
              std::tuple{QEvent::MouseButtonRelease, end, Qt::LeftButton,
                         Qt::MouseButtons(Qt::NoButton)}}) {
            QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
            QApplication::sendEvent(&canvas, &event);
        }
        require(canvas.resetEditingStatePreservingTool(), "clear drawing selection");
        require(canvas.setCanvasTool(SnowCanvasTool::Select), "finish drawing");
    }

    void filter(SnowCanvasFilterType type, bool pen = false, double opacity = 0.7) {
        require(canvas.setCanvasFilterStyle({type, 0.65, opacity, 22},
                                            SnowCanvasFilterStylePropertyType |
                                                SnowCanvasFilterStylePropertyStrength |
                                                SnowCanvasFilterStylePropertyOpacity |
                                                SnowCanvasFilterStylePropertyStrokeWidth),
                "configure filter");
        const QSize size = canvas.size();
        draw(pen ? SnowCanvasTool::PenFilter : SnowCanvasTool::RectangleFilter,
             {size.width() * 0.15, size.height() * 0.2},
             {size.width() * 0.8, size.height() * 0.75});
    }

    void zoom(double relativeZoom, const QPointF& offset = {}) {
        const QRectF bounds = renderer.reference.canvasRect;
        const double scale = relativeZoom * renderer.reference.pixelsPerCanvasUnit;
        canvas.resize(qMax(1, qRound(bounds.width() * scale)),
                      qMax(1, qRound(bounds.height() * scale)));
        const QPointF center = bounds.center() + offset;
        require(canvas.setViewportCamera(center.x(), center.y(), scale), "zoom reference scene");
    }

    QImage render(double dpr = 1.0, QRegion exposed = {}) {
        QImage image(QSize(qCeil(canvas.width() * dpr), qCeil(canvas.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        canvas.render(&painter, {}, exposed, QWidget::DrawWindowBackground);
        return image;
    }
};

} // namespace
