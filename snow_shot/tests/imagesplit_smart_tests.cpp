// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "imagesplit_guarded_image.h"
#include "imagesplit_smart_reference.h"

#include <QCoreApplication>
#include <QTransform>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace snow_shot::presentation;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QImage textFixture(QColor background = Qt::white, QColor foreground = Qt::black, int gapFirst = 91,
                   int gapLast = 96) {
    QImage image(127, 160, QImage::Format_ARGB32);
    image.fill(background);
    // Repeated H-like stems, CJK-like square radicals, thin accents and crossbars.
    // The annotated text bands cover every candidate except the known blank gap.
    for (int y = 0; y < image.height(); ++y) {
        if (y >= gapFirst && y <= gapLast)
            continue;
        for (int x = 8; x < 120; ++x) {
            const int cell = (x - 8) % 14;
            if (cell < 2 || cell == 8 || (y % 19 < 2 && cell <= 8))
                image.setPixelColor(x, y, foreground);
        }
    }
    return image;
}
ImageSplitSettings horizontal(int count = 2) {
    ImageSplitSettings settings;
    settings.direction = ImageSplitDirection::Horizontal;
    settings.count = count;
    return settings;
}
void compare(const QImage& image, ImageSplitSettings settings) {
    const auto actual = prepareImageSplitLayout(image, settings);
    const auto reference = image_split_test::referenceSmart(image, settings);
    require(actual && actual->layout.isValid() == reference.isValid() &&
                actual->layout.verticalLines == reference.verticalLines &&
                actual->layout.horizontalLines == reference.horizontalLines,
            "bounded optimized analysis agrees with whole-image scalar oracle");
}
void knownTextGaps() {
    const auto settings = horizontal();
    for (const auto colors : {std::pair{QColor(Qt::white), QColor(Qt::black)},
                              std::pair{QColor(16, 24, 32), QColor(230, 235, 240)},
                              std::pair{QColor(200, 0, 0), QColor(0, 102, 0)},
                              std::pair{QColor(0, 0, 0, 0), QColor(30, 90, 180, 90)}}) {
        auto image = textFixture(colors.first, colors.second);
        const auto prepared = prepareImageSplitLayout(image, settings);
        require(prepared && prepared->layout.horizontalLines.first() == 93,
                "Smart selects the nearest wide clear boundary outside annotated text");
        const auto transposed = image.transformed(QTransform(0, 1, 1, 0, 0, 0));
        auto vertical = settings;
        vertical.direction = ImageSplitDirection::Vertical;
        require(prepareImageSplitLayout(transposed, vertical)->layout.verticalLines.first() == 93,
                "transposed source selects the identical vertical boundary");
        compare(image, settings);
    }
    QImage panels = textFixture(QColor(128, 128, 128), QColor(100, 100, 100));
    for (int y = 91; y <= 96; ++y)
        for (int x = 0; x < panels.width(); ++x)
            panels.setPixel(x, y, x < 63 ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
    const auto variance = [&](int row) {
        qint64 sum = 0;
        qint64 squares = 0;
        for (int x = 0; x < panels.width(); ++x) {
            const int value = qRed(panels.pixel(x, row));
            sum += value;
            squares += value * value;
        }
        return std::tuple{panels.width() * squares - sum * sum, std::abs(row - 80), row};
    };
    int varianceCut = 80;
    for (int candidate = 59; candidate <= 101; ++candidate)
        if (variance(candidate) < variance(varianceCut))
            varianceCut = candidate;
    require((varianceCut < 91 || varianceCut > 97) &&
                prepareImageSplitLayout(panels, settings)->layout.horizontalLines.first() == 93,
            "local contrast finds the clear panel boundary that global variance rejects");
    auto narrow = textFixture(Qt::white, Qt::black, 91, 91);
    const int boundary = prepareImageSplitLayout(narrow, settings)->layout.horizontalLines.first();
    require(boundary == 91 || boundary == 92, "a one-pixel clear row is a usable boundary");
    auto dotted = textFixture();
    for (int x = 0; x < dotted.width(); ++x)
        dotted.setPixel(x, 80, qRgb(255, 255, 255));
    require(prepareImageSplitLayout(dotted, settings)->layout.horizontalLines.first() == 93,
            "surrounding band prefers line spacing over an internal dot/stem gap");
    for (int sign : {-1, 1}) {
        const auto edge = sign < 0 ? textFixture(Qt::white, Qt::black, 55, 58)
                                   : textFixture(Qt::white, Qt::black, 100, 106);
        require(prepareImageSplitLayout(edge, settings)->layout.horizontalLines.first() ==
                    80 + sign * 21,
                "search includes both exact radius endpoints");
    }
    auto noisy = textFixture();
    for (int y = 0; y < noisy.height(); ++y)
        for (int x = 0; x < noisy.width(); ++x) {
            if (qRed(noisy.pixel(x, y)) == 255) {
                const int value = 220 + x / 8 + (x * 17 + y * 13) % 3;
                noisy.setPixel(x, y, qRgb(value, value, value));
            }
        }
    const int noisyCut = prepareImageSplitLayout(noisy, settings)->layout.horizontalLines.first();
    require(noisyCut >= 91 && noisyCut <= 97,
            "gradient and mild noise retain the annotated clear gap");
    auto antialiased = textFixture();
    const auto hardEdges = antialiased;
    for (int y = 0; y < antialiased.height(); ++y) {
        for (int x = 1; x + 1 < antialiased.width(); ++x) {
            const int coverage =
                (qRed(hardEdges.pixel(x - 1, y)) + 2 * qRed(hardEdges.pixel(x, y)) +
                 qRed(hardEdges.pixel(x + 1, y))) /
                4;
            antialiased.setPixel(x, y, qRgb(coverage, coverage, coverage));
        }
    }
    require(prepareImageSplitLayout(antialiased, settings)->layout.horizontalLines.first() == 93,
            "antialiased thin strokes retain the annotated clear gap");
    compare(antialiased, settings);
    // Adjacent-row difference is exactly zero inside the repeated stems at the nominal cut.
    const auto repeated = textFixture();
    const auto rowDifference = [&](int boundary) {
        qint64 difference = 0;
        for (int x = 0; x < repeated.width(); ++x)
            difference +=
                std::abs(qRed(repeated.pixel(x, boundary - 1)) - qRed(repeated.pixel(x, boundary)));
        return std::tuple{difference, std::abs(boundary - 80), boundary};
    };
    int differenceCut = 80;
    for (int candidate = 59; candidate <= 101; ++candidate)
        if (rowDifference(candidate) < rowDifference(differenceCut))
            differenceCut = candidate;
    require(differenceCut == 80 && repeated.pixel(8, differenceCut) == qRgb(0, 0, 0) &&
                prepareImageSplitLayout(repeated, settings)->layout.horizontalLines.first() == 93,
            "local contrast avoids the continuous stroke chosen by adjacent-row difference");
}
void geometryAndDenseAxes() {
    for (int dimension = 1; dimension <= 96; ++dimension) {
        QImage image(7, dimension, QImage::Format_RGB32);
        for (int y = 0; y < dimension; ++y)
            for (int x = 0; x < 7; ++x)
                image.setPixel(x, y, qRgb((x * 31 + y * 13) % 256, x * 17, y));
        for (int count = 1; count <= dimension; ++count) {
            const auto settings = horizontal(count);
            const auto actual = prepareImageSplitLayout(image, settings)->layout;
            const auto nominal = calculateImageSplitLayout(image.size(), settings);
            require(actual.isValid() && actual.outputCount() == count,
                    "every valid density preserves output count and ordered nonempty cells");
            int total = 0;
            for (int part = 0; part < count; ++part) {
                const auto region = actual.regionAt(part);
                require(region.y() == total, "regions have no gaps or overlaps");
                total += region.height();
            }
            require(total == dimension, "regions cover exactly the original source");
            for (qsizetype i = 0; i < actual.horizontalLines.size(); ++i)
                require(std::abs(actual.horizontalLines.at(i) - nominal.horizontalLines.at(i)) <=
                            21,
                        "all adjustments stay within physical-pixel radius");
            compare(image, settings);
        }
    }
    QImage flat(5000, 150, QImage::Format_RGB32);
    flat.fill(Qt::gray);
    for (auto direction : {ImageSplitDirection::Horizontal, ImageSplitDirection::Vertical}) {
        auto settings = horizontal(100);
        settings.direction = direction;
        const auto actual = prepareImageSplitLayout(flat, settings)->layout;
        const auto nominal = calculateImageSplitLayout(flat.size(), settings);
        require(actual.verticalLines == nominal.verticalLines &&
                    actual.horizontalLines == nominal.horizontalLines,
                "uniform images retain nominal boundaries across multiple batches");
    }
    QImage thin(1, 1'000'000, QImage::Format_RGB32);
    thin.fill(Qt::white);
    const auto dense = prepareImageSplitLayout(thin, horizontal(thin.height()))->layout;
    require(dense.horizontalLines.runCount() == 1 && dense.horizontalLines.storageBytes() < 1024,
            "millions of fixed cuts retain compact arithmetic storage");
    QImage nearDense(3, 10000, QImage::Format_RGB32);
    nearDense.fill(Qt::white);
    require(
        prepareImageSplitLayout(nearDense, horizontal(9999))->layout.horizontalLines.runCount() <=
            2,
        "one-pixel arithmetic tails are appended in bulk");
}
void formatsAndMemory() {
    const auto source = textFixture();
    for (auto format :
         {QImage::Format_RGB32, QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied,
          QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBX8888,
          QImage::Format_RGB888, QImage::Format_Grayscale8, QImage::Format_Grayscale16,
          QImage::Format_Indexed8, QImage::Format_Mono, QImage::Format_MonoLSB,
          QImage::Format_RGBA64, QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4}) {
        const auto image = source.convertToFormat(format);
        image_split_test::GuardedImage guarded(image);
        compare(guarded.image(), horizontal());
        compare(guarded.image(), {});
        const auto alphaSource = textFixture(QColor(9, 20, 31, 54), QColor(230, 15, 90, 137));
        const auto alphaImage = alphaSource.convertToFormat(format);
        image_split_test::GuardedImage guardedAlpha(alphaImage);
        compare(guardedAlpha.image(), horizontal());
        compare(guardedAlpha.image(), {});
    }
    QImage transparent(127, 160, QImage::Format_ARGB32);
    for (int y = 0; y < transparent.height(); ++y)
        for (int x = 0; x < transparent.width(); ++x)
            transparent.setPixel(x, y, qRgba(x, y, (x * y) % 256, 0));
    require(prepareImageSplitLayout(transparent, horizontal())->layout.horizontalLines.first() ==
                80,
            "invisible RGB data cannot move cuts");
    auto sourceWithMetadata = source;
    sourceWithMetadata.setDevicePixelRatio(2.5);
    sourceWithMetadata.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const auto* bits = sourceWithMetadata.constBits();
    const auto key = sourceWithMetadata.cacheKey();
    ImageSplitDocument document;
    QImage preview(3, 3, QImage::Format_RGB32);
    preview.fill(Qt::red);
    document.setImage(sourceWithMetadata, preview, horizontal());
    require(document.layout().horizontalLines.first() == 93 &&
                document.image().constBits() == bits && document.image().cacheKey() == key &&
                document.image().devicePixelRatio() == 2.5 &&
                document.image().colorSpace() == sourceWithMetadata.colorSpace(),
            "full-resolution analysis ignores preview/DPR and preserves native source "
            "metadata/storage");
    // Both tile directions and batch boundaries, with a high-precision conversion path.
    QImage tiled(4201, 193, QImage::Format_RGB32);
    for (int y = 0; y < tiled.height(); ++y)
        for (int x = 0; x < tiled.width(); ++x)
            tiled.setPixel(x, y,
                           qRgb((x * 13 + y * 7) % 256, (x * 3 + y * 17) % 256, (x + y) % 256));
    for (auto direction : {ImageSplitDirection::Horizontal, ImageSplitDirection::Vertical}) {
        auto settings = horizontal(70);
        settings.direction = direction;
        compare(tiled, settings);
        compare(tiled.convertToFormat(QImage::Format_RGBA64), settings);
    }
}
void historyCancellationAndCache() {
    ImageSplitDocument document;
    const auto source = textFixture();
    document.setImage(source, {}, horizontal());
    require(document.settings().type == ImageSplitType::Smart && !document.canUndo(),
            "Smart is the default initial undo baseline");
    const auto generated = document.state();
    require(document.createLine(false, 20), "manual guide can be added in Smart mode");
    auto overlap = document.settings();
    overlap.overlapWidth = 4;
    const auto manual = document.state();
    require(document.setSettings(overlap) &&
                document.state().horizontalLines == manual.horizontalLines,
            "overlap preserves manually edited Smart guides");
    document.undo();
    document.undo();
    require(document.state() == generated, "undo restores the generated Smart geometry exactly");
    auto equal = document.settings();
    equal.type = ImageSplitType::Equal;
    document.setSettings(equal);
    const auto cached = document.cachedLayout(horizontal());
    require(cached && cached->layout.horizontalLines == generated.horizontalLines,
            "switching away retains the latest generated Smart cache");
    const auto revision = document.revision();
    document.createLine(false, 10);
    require(!document.setPreparedSettings(*cached, revision),
            "changed document rejects stale result");
    auto changed = horizontal(3);
    const auto prepared = prepareImageSplitLayout(source, changed);
    document.setImage(source.copy(), {}, equal);
    require(!document.setPreparedSettings(*prepared, document.revision()),
            "new source rejects old pixel identity");
    require(!prepareImageSplitLayout(source, horizontal(), [] { return true; }),
            "early cancellation returns no layout");
    int checks = 0;
    require(!prepareImageSplitLayout(source, horizontal(), [&] { return ++checks > 2; }),
            "in-flight cancellation discards partial geometry");
    auto grid = horizontal();
    grid.direction = ImageSplitDirection::Grid;
    const auto layout = prepareImageSplitLayout(source, grid)->layout;
    require(layout.verticalLines == calculateImageSplitLayout(source.size(), grid).verticalLines &&
                layout.horizontalLines ==
                    calculateImageSplitLayout(source.size(), grid).horizontalLines,
            "grid remains equal even with Smart selected");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        knownTextGaps();
        geometryAndDenseAxes();
        formatsAndMemory();
        historyCancellationAndCache();
        std::cout << "Smart split tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
