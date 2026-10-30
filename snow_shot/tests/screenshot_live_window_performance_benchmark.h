// SPDX-License-Identifier: GPL-3.0-or-later
// Included by the pinned shell fixture only in benchmark-enabled presets.
#include "../src/presentation/pinned/livewindowframecropper_p.h"

namespace live_pinned_benchmark {
template <typename Operation> double measure(Operation operation, int rounds = 40) {
    for (int index = 0; index < 3; ++index)
        operation();
    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < rounds; ++index)
        operation();
    return static_cast<double>(timer.nsecsElapsed()) / (1000.0 * rounds);
}

QImage legacyCrop(const QImage& image, const QRectF& bounds, const QRectF& crop,
                  const QSize& pixels) {
    QImage result(pixels, QImage::Format_RGBA8888);
    result.fill(Qt::transparent);
    result.setColorSpace(image.colorSpace());
    const auto mapping = snow_shot::presentation::live_window_detail::cropMapping(
        crop, QRectF(QPointF(), bounds.size()), QRectF(QPointF(), pixels));
    const qreal scaleX = image.width() / bounds.width();
    const qreal scaleY = image.height() / bounds.height();
    const QRectF source(mapping.source.x() * scaleX, mapping.source.y() * scaleY,
                        mapping.source.width() * scaleX, mapping.source.height() * scaleY);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(mapping.destination, image, source);
    return result;
}

void run() {
    using namespace live_pinned_tests;
    const QSize pixels(3840, 2160);
    std::array<QImage, 3> frames;
    for (size_t index = 0; index < frames.size(); ++index) {
        frames[index] = QImage(pixels, QImage::Format_RGBA8888);
        frames[index].fill(QColor(40 + static_cast<int>(index), 120, 200));
    }
    const QRectF bounds{QPointF(), QSizeF(pixels)};
    for (const QRectF crop : {bounds, QRectF(640, 360, 2560, 1440)}) {
        snow_shot::presentation::live_window_detail::FrameCropper cropper;
        QImage output;
        const double oldUs =
            measure([&] { output = legacyCrop(frames[0], bounds, crop, crop.size().toSize()); });
        const double newUs =
            measure([&] { output = cropper.crop(frames[0], bounds, crop, crop.size().toSize()); });
        require(output.pixelColor(0, 0) == frames[0].pixelColor(0, 0),
                "crop benchmark consumes correct pixels");
        std::cout << "live_crop size=" << crop.width() << 'x' << crop.height()
                  << " legacy_us=" << oldUs << " optimized_us=" << newUs
                  << " shares_input=" << (output.constBits() == frames[0].constBits()) << '\n';
    }

    auto source = std::make_shared<FakeSource>();
    source->frame = frames[0];
    source->region = QRect(20, 20, 640, 360);
    ScreenshotPinnedWindow window;
    present(window, config(source));
    auto& renderer = ScreenshotPinnedWindowTestAccess::renderer(window);
    const QRectF canvasRect = renderer.imageSourceSnapshot().materializedCanvasRect;
    size_t next = 0;
    const auto nextSource = [&] {
        return ScreenshotImageSource::fromImage(frames[next++ % frames.size()], canvasRect);
    };
    const double oldInstallUs = measure([&] { renderer.setImageSource(nextSource()); });
    const double newInstallUs = measure([&] { renderer.setPinnedLiveFrame(nextSource()); });
    const double shellInstallUs = measure([&] { emit source->frameChanged(frames[next++ % 3]); });
    require(Access::original(window).size() == pixels,
            "shell updates retain capture-sized image storage");
    QImage raster(window.size(), QImage::Format_ARGB32_Premultiplied);
    const double oldPaintUs = measure([&] {
        renderer.setImageSource(nextSource());
        raster.fill(Qt::transparent);
        window.render(&raster);
    });
    const double newPaintUs = measure([&] {
        renderer.setPinnedLiveFrame(nextSource());
        raster.fill(Qt::transparent);
        window.render(&raster);
    });
    std::cout << "live_renderer source=3840x2160 viewport=" << raster.width() << 'x'
              << raster.height() << " legacy_install_us=" << oldInstallUs
              << " optimized_install_us=" << newInstallUs << " shell_install_us=" << shellInstallUs
              << " legacy_install_paint_us=" << oldPaintUs
              << " optimized_install_paint_us=" << newPaintUs << '\n';
    for (const int angle : {0, 90, 180, 270}) {
        if (angle)
            Access::command(window, QStringLiteral("rotate_clockwise"));
        raster = QImage(window.size(), QImage::Format_ARGB32_Premultiplied);
        const auto materializedUs = measure(
            [&] {
                const auto transformed =
                    snowCanvasTransformImage(frames[next++ % frames.size()],
                                             Access::transform(window), Qt::SmoothTransformation);
                renderer.setPinnedLiveFrame(ScreenshotImageSource::fromImage(
                    transformed, renderer.imageSourceSnapshot().materializedCanvasRect));
                raster.fill(Qt::transparent);
                window.render(&raster);
            },
            30);
        const double frameUs = measure(
            [&] {
                emit source->frameChanged(frames[next++ % frames.size()]);
                raster.fill(Qt::transparent);
                window.render(&raster);
            },
            30);
        require(raster.pixelColor(raster.rect().center()).blue() == 200,
                "live presentation benchmark renders the captured frame");
        std::cout << "live_presentation source=3840x2160 angle=" << angle
                  << " viewport=" << raster.width() << 'x' << raster.height()
                  << " materialize_install_paint_us=" << materializedUs
                  << " install_paint_us=" << frameUs << '\n';
    }
    window.close();
}
} // namespace live_pinned_benchmark
