#include "snow_canvas_display_item.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_spotlight_renderer.h"
#include "snow_canvas_viewport.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QRegion>
#include <QThread>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class PaintObserver final : public QObject {
  public:
    void begin() {
        m_sawPaint = false;
        m_observing = true;
    }

    bool sawPaint() const {
        return m_sawPaint;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (m_observing && event != nullptr && event->type() == QEvent::Paint) {
            m_sawPaint = true;
        }
        return false;
    }

  private:
    bool m_observing = false;
    bool m_sawPaint = false;
};

SceneDisplayInfo sceneInfo() {
    SceneDisplayInfo info;
    info.surface_width = 100.0;
    info.surface_height = 100.0;
    info.camera_center_x = 50.0;
    info.camera_center_y = 50.0;
    info.camera_zoom = 1.0;
    return info;
}

SnowSpotlightCutout cutout(double centerX, double centerY, double width, double height,
                           double rotation = 0.0) {
    SnowSpotlightCutout item{};
    item.center_x = centerX;
    item.center_y = centerY;
    item.width = width;
    item.height = height;
    item.rotation = rotation;
    return item;
}

QImage render(const SnowSpotlightCutout* items, std::uint32_t itemCount,
              const QRectF& renderArea = QRectF(0.0, 0.0, 100.0, 100.0),
              const QRegion& exposed = QRegion(QRect(0, 0, 100, 100)), bool active = true) {
    QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setClipRegion(exposed);
    snow_canvas_spotlight_renderer::render(
        painter, sceneInfo(), SpotlightDisplayInfo{SnowColorRgba8{0, 0, 0, 255}, 0.64, active},
        items, itemCount, renderArea, exposed);
    painter.end();
    return image;
}

bool isDefaultMaskPixel(QRgb pixel) {
    return qAlpha(pixel) == 255 && qRed(pixel) == qGreen(pixel) && qGreen(pixel) == qBlue(pixel) &&
           qRed(pixel) >= 91 && qRed(pixel) <= 93;
}

void defaultMaskHasExactOpacityAndTransparentHole() {
    const SnowSpotlightCutout item = cutout(50.0, 50.0, 40.0, 30.0);
    const QImage image = render(&item, 1);
    require(isDefaultMaskPixel(image.pixel(5, 5)),
            "default spotlight mask must composite 64% black");
    require(image.pixelColor(50, 50) == QColor(Qt::white),
            "spotlight rectangle must reveal the unmasked canvas");
}

void overlappingAndRotatedCutoutsUseAPathUnion() {
    const SnowSpotlightCutout overlap[] = {
        cutout(40.0, 50.0, 30.0, 24.0),
        cutout(60.0, 50.0, 30.0, 24.0),
    };
    const QImage unionImage = render(overlap, 2);
    require(unionImage.pixelColor(50, 50) == QColor(Qt::white),
            "overlapping spotlight rectangles must form one transparent union");

    const SnowSpotlightCutout rotated = cutout(50.0, 50.0, 40.0, 20.0, std::acos(-1.0) / 4.0);
    const QImage rotatedImage = render(&rotated, 1);
    require(rotatedImage.pixelColor(50, 50) == QColor(Qt::white),
            "rotated spotlight must keep its center transparent");
    require(isDefaultMaskPixel(rotatedImage.pixel(68, 50)),
            "rotated spotlight must not use its axis-aligned bounding box as the hole");

    bool foundAntialiasedEdge = false;
    for (int y = 30; y <= 70 && !foundAntialiasedEdge; ++y) {
        for (int x = 30; x <= 70; ++x) {
            const int red = qRed(rotatedImage.pixel(x, y));
            if (red > 92 && red < 255) {
                foundAntialiasedEdge = true;
                break;
            }
        }
    }
    require(foundAntialiasedEdge, "rotated spotlight edge must be antialiased");
}

void renderAreaAndExposureLimitMaskWork() {
    const SnowSpotlightCutout item = cutout(50.0, 50.0, 16.0, 16.0);
    const QImage bounded = render(&item, 1, QRectF(20.0, 20.0, 60.0, 60.0));
    require(bounded.pixelColor(5, 5) == QColor(Qt::white),
            "pixels outside an explicit spotlight render area must remain untouched");
    require(isDefaultMaskPixel(bounded.pixel(25, 25)),
            "pixels inside the bounded render area must be masked");
    require(bounded.pixelColor(50, 50) == QColor(Qt::white),
            "cutouts must remain transparent inside a bounded render area");

    const QImage empty = render(&item, 1, QRectF());
    require(empty.pixelColor(25, 25) == QColor(Qt::white),
            "an explicitly empty render area must suppress the mask");

    QRegion exposed(QRect(0, 0, 20, 20));
    exposed += QRect(80, 80, 20, 20);
    const QImage clipped = render(nullptr, 0, QRectF(0, 0, 100, 100), exposed);
    require(isDefaultMaskPixel(clipped.pixel(5, 5)) && isDefaultMaskPixel(clipped.pixel(90, 90)),
            "every exposed region must receive the mask");
    require(clipped.pixelColor(50, 50) == QColor(Qt::white),
            "unexposed pixels must not be repainted by the mask pass");
}

void inactiveMaskLeavesCanvasUnchanged() {
    const SnowSpotlightCutout item = cutout(50.0, 50.0, 20.0, 20.0);
    const QImage image =
        render(&item, 1, QRectF(0.0, 0.0, 100.0, 100.0), QRegion(QRect(0, 0, 100, 100)), false);
    require(image.pixelColor(5, 5) == QColor(Qt::white),
            "removing the final spotlight cutout must remove the global mask");
}

void zeroAlphaAndNonintersectingExposureSkipGeometry() {
    const SnowSpotlightCutout item = cutout(50.0, 50.0, 30.0, 20.0);
    QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    snow_canvas_spotlight_renderer::resetDiagnosticsForCurrentThread();
    snow_canvas_spotlight_renderer::render(
        painter, sceneInfo(), SpotlightDisplayInfo{SnowColorRgba8{0, 0, 0, 0}, 1.0, true}, &item, 1,
        QRectF(0.0, 0.0, 100.0, 100.0), QRegion(QRect(0, 0, 100, 100)));
    snow_canvas_spotlight_renderer::render(
        painter, sceneInfo(), SpotlightDisplayInfo{SnowColorRgba8{0, 0, 0, 255}, 1.0, true}, &item,
        1, QRectF(0.0, 0.0, 20.0, 20.0), QRegion(QRect(80, 80, 20, 20)));
    painter.end();
    const auto diagnostics = snow_canvas_spotlight_renderer::diagnosticsForCurrentThread();
    require(diagnostics.earlyExitCount == 2,
            "transparent and nonintersecting spotlight paints must be diagnosed as early exits");
}

void displayCachePatchesSpotlightIndependentlyFromStyle() {
    ScopedRuntimeHandle runtime;
    require(snow_runtime_create(runtime.outParam()) == SNOW_OK,
            "spotlight runtime creation must succeed");
    SnowCanvasViewport viewport;
    SnowEngineConfig config = snow_canvas_viewport::defaultEngineConfig();
    require(viewport.create(runtime.get(), config),
            "spotlight viewport creation must succeed");
    require(snow_viewport_set_surface_size(runtime.get(), viewport.get(), 100, 100) == SNOW_OK,
            "spotlight surface setup must succeed");
    require(snow_viewport_set_active_tool(runtime.get(), viewport.get(),
                                          SNOW_ACTIVE_TOOL_SPOTLIGHT) == SNOW_OK,
            "spotlight tool setup must succeed");

    SnowCanvasDisplayCache cache;
    require(cache.sync(runtime.get(), viewport.get()),
            "initial spotlight display cache sync must succeed");
    const std::uint64_t sceneRevision = cache.patchCursor().scene_revision;

    const auto pointer = [&](SnowPointerEventType type, double x, double y, std::uint8_t buttons) {
        SnowInputEvent event{};
        event.kind = SNOW_INPUT_EVENT_POINTER;
        event.pointer.pointer_id = 1;
        event.pointer.event_type = type;
        event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
        event.pointer.position_x = x;
        event.pointer.position_y = y;
        event.pointer.button = type == SNOW_POINTER_EVENT_MOVE ? SNOW_POINTER_BUTTON_NONE
                                                               : SNOW_POINTER_BUTTON_PRIMARY;
        event.pointer.buttons = buttons;
        SnowInteractionOutput output{};
        require(snow_viewport_process_input(runtime.get(), viewport.get(), &event, &output) ==
                    SNOW_OK,
                "spotlight pointer input must succeed");
    };
    pointer(SNOW_POINTER_EVENT_DOWN, 30.0, 30.0, 1);
    pointer(SNOW_POINTER_EVENT_MOVE, 70.0, 70.0, 1);
    require(cache.sync(runtime.get(), viewport.get()),
            "spotlight creation preview sync must succeed");
    require(cache.spotlightCutoutCount() == 1,
            "spotlight creation preview must populate dedicated cutout storage");
    require(cache.patchCursor().scene_revision == sceneRevision,
            "spotlight creation preview must not advance the scene revision");
    require(cache.sceneDirtyRectCount() == 0,
            "spotlight creation preview must not dirty retained scene content");

    pointer(SNOW_POINTER_EVENT_UP, 70.0, 70.0, 0);
    require(cache.sync(runtime.get(), viewport.get()),
            "committed spotlight sync must succeed");
    SnowSpotlightConfig spotlight{};
    require(snow_viewport_get_spotlight_config(runtime.get(), viewport.get(), &spotlight) ==
                SNOW_OK,
            "spotlight config query must succeed");
    spotlight.opacity = 0.35;
    require(snow_viewport_set_spotlight_config(runtime.get(), viewport.get(), &spotlight) ==
                SNOW_OK,
            "spotlight opacity update must succeed");
    require(cache.sync(runtime.get(), viewport.get()), "spotlight opacity sync must succeed");
    require(cache.patchCursor().scene_revision == sceneRevision,
            "spotlight color/opacity-only updates must preserve scene revision");
}

void unchangedRenderAreaDoesNotScheduleRepaint() {
    SnowCanvasWidget canvas;
    canvas.resize(1920, 1080);
    canvas.show();
    QApplication::processEvents();

    const QRectF renderArea(-800.0, -450.0, 1600.0, 900.0);
    canvas.setSpotlightRenderArea(renderArea);
    QApplication::processEvents();

    PaintObserver observer;
    canvas.installEventFilter(&observer);
    observer.begin();
    canvas.setSpotlightRenderArea(QRectF(renderArea.bottomRight(), renderArea.topLeft()));
    QApplication::processEvents();
    canvas.removeEventFilter(&observer);

    require(!observer.sawPaint(), "an unchanged spotlight render area must not schedule a repaint");
}

void spotlightPreviewCoalescesAndCommitCancelsQueuedValue() {
    SnowCanvasWidget canvas;
    int appliedCount = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::spotlightPreviewApplied,
                     [&appliedCount]() { ++appliedCount; });

    SnowCanvasSpotlightConfig first;
    first.color = QColor(32, 96, 180, 255);
    first.opacity = 0.35;
    SnowCanvasSpotlightConfig latest = first;
    latest.color = QColor(210, 64, 48, 220);
    latest.opacity = 0.72;

    canvas.previewCanvasSpotlightConfig(first);
    require(appliedCount == 1, "the first spotlight preview should apply synchronously");
    canvas.previewCanvasSpotlightConfig(first);
    require(appliedCount == 1,
            "an unchanged spotlight preview should not emit another application");
    canvas.previewCanvasSpotlightConfig(latest);
    require(appliedCount == 1,
            "later spotlight preview writes should coalesce until the refresh callback");

    QElapsedTimer previewWait;
    previewWait.start();
    while (appliedCount < 2 && previewWait.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(appliedCount == 2,
            "the refresh-paced spotlight callback should deliver the latest value once");

    SnowCanvasWidget cancellationCanvas;
    int cancellationCount = 0;
    QObject::connect(&cancellationCanvas, &SnowCanvasWidget::spotlightPreviewApplied,
                     [&cancellationCount]() { ++cancellationCount; });
    cancellationCanvas.previewCanvasSpotlightConfig(first);
    SnowCanvasSpotlightConfig stale = first;
    stale.opacity = 0.91;
    SnowCanvasSpotlightConfig committed = latest;
    committed.color = QColor(24, 144, 88, 255);
    committed.opacity = 0.48;
    cancellationCanvas.previewCanvasSpotlightConfig(stale);
    require(cancellationCanvas.setCanvasSpotlightConfig(committed),
            "a spotlight commit should succeed while a preview is queued");
    QCoreApplication::processEvents();
    require(cancellationCount == 1,
            "a persistent spotlight commit should cancel its queued transient preview");
    require(cancellationCanvas.canvasSpotlightConfig() == committed,
            "the persistent spotlight commit should remain authoritative");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    defaultMaskHasExactOpacityAndTransparentHole();
    overlappingAndRotatedCutoutsUseAPathUnion();
    renderAreaAndExposureLimitMaskWork();
    inactiveMaskLeavesCanvasUnchanged();
    zeroAlphaAndNonintersectingExposureSkipGeometry();
    displayCachePatchesSpotlightIndependentlyFromStyle();
    unchangedRenderAreaDoesNotScheduleRepaint();
    spotlightPreviewCoalescesAndCommitCancelsQueuedValue();
    return 0;
}
