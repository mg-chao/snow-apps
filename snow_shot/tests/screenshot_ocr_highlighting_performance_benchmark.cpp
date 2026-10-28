#include "snow_shot/presentation/screenshotocrtextlayer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::shared_ptr<ScreenshotOcrPresentation> makePresentation(const QSize& size, int count) {
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(QPoint(), size);
    const int columns = count == 20 ? 5 : 20;
    const int rows = count / columns;
    const qreal cellWidth = qreal(size.width()) / columns;
    const qreal cellHeight = qreal(size.height()) / rows;
    for (int index = 0; index < count; ++index) {
        const QRectF rect((index % columns) * cellWidth + 4, (index / columns) * cellHeight + 4,
                          cellWidth - 8, cellHeight - 8);
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized text %1").arg(index);
        line.quad =
            QPolygonF({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()});
        presentation->lines.push_back(std::move(line));
    }
    return presentation;
}

void benchmarkLabels(const QSize& size, int count) {
    const auto presentation = makePresentation(size, count);
    ScreenshotOcrTextLayer layer;
    layer.setPresentation(presentation);
    layer.synchronize(QTransform(), QRect(QPoint(), size));
    const auto sceneItems = layer.scene()->items();
    auto items = sceneItems;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const auto* item) { return item->zValue() != 0; }),
                items.end());
    require(items.size() == count &&
                std::count_if(items.cbegin(), items.cend(),
                              [](const auto* item) { return item->isVisible(); }) == count,
            "benchmark prepares exactly one visible item per label");
    const auto geometryCount = layer.geometrySynchronizationCount();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    require(!image.isNull(), "allocate benchmark render surface");
    const QColor mask(0, 0, 0, 128);
    const auto render = [&] {
        QPainter painter(&image);
        layer.scene()->render(&painter, QRectF(image.rect()), layer.sceneRect());
    };
    image.fill(Qt::white);
    const QImage blank = image.copy();
    render();
    const QImage textOnly = image.copy();
    require(textOnly != blank, "benchmark labels paint visible text");
    layer.setTextHighlighting(true, mask);
    image.fill(Qt::white);
    render();
    require(image != textOnly, "benchmark highlighting changes the prepared label pixels");
    require(image.pixelColor(0, 0) == QColor(127, 127, 127),
            "benchmark masks non-text background exactly once");
    for (const auto* item : items) {
        const QPoint center = item->sceneBoundingRect().center().toPoint();
        require(image.pixel(center) == textOnly.pixel(center),
                "benchmark leaves the recognized text regions clear");
    }
    constexpr int warmups = 5;
    constexpr int samples = 30;
    std::array<std::vector<double>, 2> timings;
    for (auto& values : timings)
        values.reserve(samples);
    // Alternating order reduces bias from warming caches and changes in CPU frequency.
    for (int sample = -warmups; sample < samples; ++sample) {
        for (int pass = 0; pass < 2; ++pass) {
            const int enabled = (sample & 1) == 0 ? pass : 1 - pass;
            layer.setTextHighlighting(enabled != 0, mask);
            image.fill(Qt::white);
            QElapsedTimer timer;
            timer.start();
            render();
            if (sample >= 0)
                timings[static_cast<std::size_t>(enabled)].push_back(double(timer.nsecsElapsed()) /
                                                                     1'000'000.0);
        }
    }
    layer.synchronize(QTransform(), QRect(QPoint(), size));
    require(layer.geometrySynchronizationCount() == geometryCount,
            "highlighting toggles and paints do not synchronize label geometry");
    require(layer.scene()->items() == sceneItems,
            "highlighting toggles retain scene items and allocate no overlay items");
    for (int enabled = 0; enabled < 2; ++enabled) {
        auto& values = timings[static_cast<std::size_t>(enabled)];
        std::sort(values.begin(), values.end());
        std::cout << "ocr_highlighting width=" << size.width() << " height=" << size.height()
                  << " labels=" << count << " enabled=" << enabled
                  << " median_ms=" << values[values.size() / 2]
                  << " p95_ms=" << values[(values.size() * 95 + 99) / 100 - 1] << '\n';
    }
}

void benchmarkZoom(const QSize& size, int count) {
    const auto presentation = makePresentation(size, count);
    using Resolution = ScreenshotOcrTextLayer::HighlightingResolution;
    for (const auto resolution : {Resolution::Device, Resolution::Image}) {
        ScreenshotOcrTextLayer layer(nullptr, resolution);
        layer.setPresentation(presentation);
        layer.setTextHighlighting(true, QColor(0, 0, 0, 128));
        for (auto* item : layer.scene()->items())
            item->setOpacity(0); // Isolate the surrounding background from glyph painting.
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        std::vector<double> timings;
        for (int sample = -1; sample < 20; ++sample) {
            const qreal zoom = 0.5 + (sample + 1) * 0.05;
            image.fill(Qt::white);
            QElapsedTimer timer;
            timer.start();
            layer.synchronize(QTransform::fromScale(zoom, zoom), QRect(QPoint(), size));
            {
                QPainter painter(&image);
                layer.scene()->render(&painter, QRectF(image.rect()), layer.sceneRect());
            }
            if (sample >= 0)
                timings.push_back(double(timer.nsecsElapsed()) / 1'000'000.0);
        }
        if (resolution == Resolution::Image)
            require(layer.highlightingRasterBuildCount() == 1,
                    "zoom benchmark reuses one background raster at image resolution");
        std::sort(timings.begin(), timings.end());
        std::cout << "ocr_highlighting_zoom width=" << size.width() << " height=" << size.height()
                  << " labels=" << count << " cached_native=" << (resolution == Resolution::Image)
                  << " raster_builds=" << layer.highlightingRasterBuildCount()
                  << " median_ms=" << timings[timings.size() / 2]
                  << " p95_ms=" << timings[(timings.size() * 95 + 99) / 100 - 1] << '\n';
    }
}
} // namespace

void runScreenshotOcrHighlightingBenchmark() {
#ifdef Q_OS_WIN
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load offscreen benchmark font");
    QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
    for (const QSize& size : {QSize(1920, 1080), QSize(3840, 2160)}) {
        for (int count : {20, 500})
            benchmarkLabels(size, count);
        benchmarkZoom(size, 500);
    }
}
