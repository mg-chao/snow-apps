#include "snow_shot/presentation/screenshotoriginalimagepreviewwindow.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <iostream>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#endif

class ScreenshotOriginalImagePreviewWindowTestAccess final {
  public:
    static quint64 generation(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_rasterGeneration;
    }
    static const QImage& source(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_sourceImage;
    }
    static const QImage& raster(const ScreenshotOriginalImagePreviewWindow& window) {
        return window.m_viewportImage;
    }
};

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const bool nativeBenchmark = application.arguments().contains(QStringLiteral("--native-only"));
    if (nativeBenchmark != ScreenshotOriginalImagePreviewWindow::usesPhysicalGeometry()) {
        std::cerr
            << "Use --native-only -platform windows for native geometry, or -platform offscreen\n";
        return 2;
    }
    QRect viewport(500, 100, 640, 360);
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (nativeBenchmark) {
        QScreen* screen = QGuiApplication::primaryScreen();
        const auto* native =
            screen ? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!native || GetMonitorInfoW(native->handle(), &info) == FALSE)
            return 2;
        viewport.moveTopLeft(QPoint(info.rcWork.left + 500, info.rcWork.top + 100));
    }
#endif
    const auto finishPresentation =
        [nativeBenchmark](ScreenshotOriginalImagePreviewWindow& preview) {
            if (nativeBenchmark) {
                preview.repaint();
                QApplication::processEvents();
            }
        };
    std::cout
        << "platform,source_width,source_height,viewport_width,viewport_height,render_median_ms,"
           "render_p95_ms,move_median_us,move_p95_us,source_bytes,cache_bytes\n";
    for (const QSize sourceSize : {QSize(3840, 2160), QSize(40000, 240)}) {
        ScreenshotOriginalImagePreviewState state;
        state.image = QImage(sourceSize, QImage::Format_ARGB32_Premultiplied);
        state.image.fill(QColor(48, 96, 160));
        state.resultRect = viewport;
        state.imageRectInViewport =
            sourceSize.width() > 32768 ? QRectF(-35000, 40, sourceSize.width(), sourceSize.height())
                                       : QRectF(QPointF(), QSizeF(state.resultRect.size()));
        ScreenshotOriginalImagePreviewWindow preview;
        if (!preview.present(state))
            return 1;
        QApplication::processEvents();
        finishPresentation(preview);
        std::vector<double> renders;
        std::vector<double> movements;
        for (int run = 0; run < 22; ++run) {
            state.imageRectInViewport.translate(0.25, 0);
            QElapsedTimer timer;
            timer.start();
            if (!preview.present(state))
                return 1;
            finishPresentation(preview);
            const double renderMs = timer.nsecsElapsed() / 1000000.0;
            if (ScreenshotOriginalImagePreviewWindowTestAccess::source(preview).constBits() !=
                state.image.constBits())
                return 3;
            const quint64 generation =
                ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview);
            timer.restart();
            constexpr int kMovementCount = 100;
            for (int step = 0; step < kMovementCount; ++step) {
                state.resultRect.translate(step % 2 == 0 ? 1 : -1, 0);
                if (!preview.present(state))
                    return 1;
            }
            finishPresentation(preview);
            const double moveUs = timer.nsecsElapsed() / (1000.0 * kMovementCount);
            if (ScreenshotOriginalImagePreviewWindowTestAccess::generation(preview) != generation)
                return 4;
            if (nativeBenchmark &&
                (ScreenshotOriginalImagePreviewWindow::nativeClientRect(&preview).size() !=
                     state.resultRect.size() ||
                 ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).size() !=
                     state.resultRect.size()))
                return 5;
            if (run >= 2) {
                renders.push_back(renderMs);
                movements.push_back(moveUs);
            }
        }
        std::sort(renders.begin(), renders.end());
        std::sort(movements.begin(), movements.end());
        std::cout << (nativeBenchmark ? "native_windows" : "logical") << ',' << sourceSize.width()
                  << ',' << sourceSize.height() << ',' << state.resultRect.width() << ','
                  << state.resultRect.height() << ',' << renders[10] << ',' << renders[18] << ','
                  << movements[10] << ',' << movements[18] << ',' << state.image.sizeInBytes()
                  << ','
                  << ScreenshotOriginalImagePreviewWindowTestAccess::raster(preview).sizeInBytes()
                  << '\n';
    }
    return 0;
}
