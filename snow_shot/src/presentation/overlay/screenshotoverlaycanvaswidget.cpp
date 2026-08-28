#include "screenshotoverlaycanvaswidget.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"

#include <QPaintEvent>
#include <QRegion>

namespace {
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
qint64 paintRegionArea(const QRegion& region) {
    qint64 area = 0;
    for (const QRect& rect : region) {
        area += static_cast<qint64>(rect.width()) * static_cast<qint64>(rect.height());
    }
    return area;
}
#endif
} // namespace

ScreenshotOverlayCanvasWidget::ScreenshotOverlayCanvasWidget(SnowCanvasRuntime& runtime,
                                                             QWidget* parent)
    : SnowCanvasWidget(runtime, parent) {}

void ScreenshotOverlayCanvasWidget::paintEvent(QPaintEvent* event) {
    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.canvas.paint_event");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.paint_events", 1);
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION)
    const QRegion region = event != nullptr ? event->region().intersected(rect()) : QRegion(rect());
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.paint_rects", region.rectCount());
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.paint_logical_pixels",
                                   paintRegionArea(region));
    if (region.contains(rect())) {
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.canvas.full_paint_events", 1);
    }
#endif
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.canvas.paint_begin");
    SnowCanvasWidget::paintEvent(event);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.canvas.paint_end");
}
