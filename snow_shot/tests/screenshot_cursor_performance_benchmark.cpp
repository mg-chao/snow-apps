#include "snow_shot/presentation/screenshotcursorimagesource.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#endif

namespace {
quint64 privateBytes() {
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)))
        return counters.PrivateUsage;
#endif
    return 0;
}
void requireCursor(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

CapturedDisplayModel cursorDisplay(const QSize& size) {
    CapturedDisplayModel display;
    display.active = true;
    display.physicalRect = QRect(QPoint(), size);
    display.canvasRect = display.physicalRect;
    display.image = QImage(size, QImage::Format_RGBA8888);
    display.image.fill(QColor(20, 40, 60));
    display.cursorPixelRect = QRect(size.width() / 2, size.height() / 2, 16, 24);
    display.cursorPatch = QImage(display.cursorPixelRect.size(), QImage::Format_RGBA8888);
    display.cursorPatch.fill(QColor(200, 100, 50));
    return display;
}
} // namespace

void runScreenshotCursorBenchmark() {
    QJsonArray scenarios;
    for (const QSize size : {QSize(1920, 1080), QSize(3840, 2160)}) {
        const auto display = cursorDisplay(size);
        SnowCanvasWidget canvas;
        canvas.resize(size);
        canvas.setClearBackgroundEnabled(false);
        static_cast<void>(canvas.setViewportCamera(size.width() / 2.0, size.height() / 2.0, 1.0));
        ScreenshotCanvasRenderer renderer(canvas);
        canvas.setCustomRenderer(&renderer);
        renderer.setImageSource(screenshotDisplayImageSource(display, false));
        const QRectF damage = screenshotCursorCanvasRect(display);
        QImage surface(size, QImage::Format_ARGB32_Premultiplied);
        const auto cleanPointer = display.image.constBits();
        const auto patchPointer = display.cursorPatch.constBits();
        quint64 baselineMemory = 0, peakMemory = 0;
        std::vector<double> timings;
        timings.reserve(500);
        for (int i = 0; i < 520; ++i) {
            QElapsedTimer elapsed;
            elapsed.start();
            renderer.setImageSource(screenshotDisplayImageSource(display, (i % 2) != 0), damage);
            QPainter painter(&surface);
            const QRect paintRect = display.cursorPixelRect.adjusted(-1, -1, 1, 1);
            canvas.render(&painter, paintRect.topLeft(), QRegion(paintRect));
            painter.end();
            const double milliseconds = elapsed.nsecsElapsed() / 1000000.0;
            requireCursor(surface.pixelColor(display.cursorPixelRect.center()) ==
                              ((i % 2) != 0 ? QColor(200, 100, 50) : QColor(20, 40, 60)),
                          "measured repaint must present the requested cursor pixels");
            if (i == 19)
                baselineMemory = privateBytes();
            if (i >= 20) {
                timings.push_back(milliseconds);
                peakMemory = std::max(peakMemory, privateBytes());
            }
            requireCursor(display.image.constBits() == cleanPointer &&
                              display.cursorPatch.constBits() == patchPointer,
                          "benchmark visibility updates must retain the original frame allocation");
        }
        std::sort(timings.begin(), timings.end());
        const double p95 = timings[static_cast<size_t>(timings.size() * .95)];
        requireCursor(p95 < 1000.0 / 60.0, "cursor repaint must complete within one 60 Hz frame");
        requireCursor(peakMemory <= baselineMemory + 1024 * 1024,
                      "repeated cursor toggles must retain stable memory after warmup");
        scenarios.append(QJsonObject{
            {QStringLiteral("width"), size.width()},
            {QStringLiteral("height"), size.height()},
            {QStringLiteral("p50_ms"), timings[timings.size() / 2]},
            {QStringLiteral("p95_ms"), p95},
            {QStringLiteral("retained_cursor_bytes"), display.cursorPatch.sizeInBytes()},
            {QStringLiteral("retained_image_bytes"),
             display.image.sizeInBytes() + display.cursorPatch.sizeInBytes()},
            {QStringLiteral("private_bytes_after_warmup"), static_cast<qint64>(baselineMemory)},
            {QStringLiteral("private_bytes_peak"), static_cast<qint64>(peakMemory)}});
        canvas.setCustomRenderer(nullptr);
    }
    std::cout << QJsonDocument(scenarios).toJson().constData();
}
