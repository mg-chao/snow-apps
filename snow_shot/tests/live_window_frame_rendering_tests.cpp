// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/presentation/pinned/livewindowframerenderer_p.h"

#include <QElapsedTimer>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QImage fixture(const QSize& size, QImage::Format format) {
    QImage image(size, format);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(
                x, y,
                QColor((x * 17) % 256, (y * 31) % 256, (x + y) % 256, (x * 7 + y * 11) % 256));
    return image;
}

void transformsPreservePixels() {
    using namespace snow_shot::presentation::live_window_detail;
    for (const auto format : {QImage::Format_RGBA8888, QImage::Format_ARGB32_Premultiplied}) {
        auto frame = fixture(QSize(37, 23), format);
        frame.setDevicePixelRatio(2);
        const auto* originalPixels = frame.constBits();
        for (int turn = 0; turn < 4; ++turn) {
            for (const bool flip : {false, true}) {
                const auto transform = QTransform().rotate(turn * 90).scale(flip ? -1 : 1, 1);
                const QSize rotatedSize = turn % 2 ? frame.size().transposed() : frame.size();
                QImage rotated(rotatedSize, format);
                // An exact pixel permutation is the reference. Smooth QImage::transformed
                // can round translucent premultiplied channels even for a reflection.
                const auto matrix = QImage::trueMatrix(transform, frame.width(), frame.height());
                for (int y = 0; y < frame.height(); ++y)
                    for (int x = 0; x < frame.width(); ++x) {
                        const auto mapped = matrix.map(QPointF(x + 0.5, y + 0.5));
                        rotated.setPixel(qFloor(mapped.x()), qFloor(mapped.y()), frame.pixel(x, y));
                    }
                require(transformedFrameSize(frame.size(), transform) == rotatedSize,
                        "presentation geometry matches the exported orientation");
                for (const qreal dpr : {1.0, 1.5, 2.0}) {
                    for (const qreal scale : {1.0, 0.6, 1.75}) {
                        const bool filtered = scale != 1.0;
                        const QSize output(180, 180);
                        QImage actual(output, QImage::Format_ARGB32_Premultiplied);
                        QImage expected(output, actual.format());
                        actual.setDevicePixelRatio(dpr);
                        expected.setDevicePixelRatio(dpr);
                        actual.fill(Qt::transparent);
                        expected.fill(Qt::transparent);
                        const QRectF target(QPointF(7 / dpr, 9 / dpr),
                                            QSizeF((QSizeF(rotated.size()) * scale).toSize()) /
                                                dpr);
                        const QRectF clip = target.adjusted(3 / dpr, 2 / dpr, -4 / dpr, -1 / dpr);
                        {
                            QPainter painter(&actual);
                            painter.setClipRect(clip);
                            paintLiveWindowFrame(painter, target, frame, transform, filtered);
                        }
                        {
                            QPainter painter(&expected);
                            painter.setClipRect(clip);
                            painter.setRenderHint(QPainter::SmoothPixmapTransform, filtered);
                            painter.drawImage(target, rotated, QRectF(rotated.rect()));
                        }
                        int maximumError = 0;
                        for (int y = 0; y < output.height(); ++y)
                            for (int x = 0; x < output.width(); ++x) {
                                const auto a = actual.pixel(x, y);
                                const auto b = expected.pixel(x, y);
                                for (const int shift : {0, 8, 16, 24})
                                    maximumError =
                                        qMax(maximumError, qAbs(int((a >> shift) & 255) -
                                                                int((b >> shift) & 255)));
                            }
                        // Qt's axis-aligned and general-transform bilinear paths differ
                        // in subpixel rounding. Native-size presentation must stay exact.
                        const int tolerance = filtered ? 3 : 0;
                        if (maximumError > tolerance) {
                            std::cerr << "format=" << format << " turn=" << turn << " flip=" << flip
                                      << " dpr=" << dpr << " scale=" << scale
                                      << " maximum_error=" << maximumError << '\n';
                            for (int y = 0; y < output.height(); ++y) {
                                for (int x = 0; x < output.width(); ++x) {
                                    if (actual.pixel(x, y) != expected.pixel(x, y)) {
                                        std::cerr << "first mismatch at " << x << ',' << y
                                                  << " actual=" << std::hex << actual.pixel(x, y)
                                                  << " expected=" << expected.pixel(x, y)
                                                  << std::dec << '\n';
                                        y = output.height();
                                        break;
                                    }
                                }
                            }
                        }
                        require(maximumError <= tolerance,
                                "paint-time rotation preserves pixels, alpha, clipping, and DPR");
                        require(frame.constBits() == originalPixels,
                                "presentation retains the immutable capture buffer");
                    }
                }
            }
        }
    }
}

#ifdef SNOW_SHOT_LIVE_FRAME_BENCHMARK
template <typename F> double measure(F operation) {
    for (int i = 0; i < 5; ++i)
        operation();
    QElapsedTimer timer;
    timer.start();
    constexpr int rounds = 60;
    for (int i = 0; i < rounds; ++i)
        operation();
    return double(timer.nsecsElapsed()) / (rounds * 1000.0);
}

void benchmark() {
    using namespace snow_shot::presentation::live_window_detail;
    for (const auto size : {QSize(1420, 815), QSize(1920, 1080), QSize(3840, 2160)}) {
        const auto frame = fixture(size, QImage::Format_RGBA8888);
        for (const int angle : {0, 90, 180, 270}) {
            const auto transform = QTransform().rotate(angle);
            const QSize unrotatedOutput = size.width() == 1420 ? size : QSize(640, 360);
            const auto output = angle % 180 ? unrotatedOutput.transposed() : unrotatedOutput;
            QImage raster(output, QImage::Format_ARGB32_Premultiplied);
            const auto oldUs = measure([&] {
                const auto rotated = frame.transformed(transform, Qt::SmoothTransformation);
                raster.fill(Qt::transparent);
                QPainter painter(&raster);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawImage(QRectF(raster.rect()), rotated, QRectF(rotated.rect()));
            });
            const auto newUs = measure([&] {
                raster.fill(Qt::transparent);
                QPainter painter(&raster);
                paintLiveWindowFrame(painter, QRectF(raster.rect()), frame, transform, true);
            });
            std::cout << "live_frame source=" << size.width() << 'x' << size.height()
                      << " viewport=" << output.width() << 'x' << output.height()
                      << " angle=" << angle << " materialize_paint_us=" << oldUs
                      << " direct_paint_us=" << newUs << " speedup=" << oldUs / newUs << '\n';
        }
    }
}
#endif
} // namespace

int main() {
    transformsPreservePixels();
#ifdef SNOW_SHOT_LIVE_FRAME_BENCHMARK
    benchmark();
#endif
    return EXIT_SUCCESS;
}
