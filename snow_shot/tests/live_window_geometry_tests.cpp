#include "../src/presentation/pinned/livewindownative_p.h"
#include "../src/presentation/pinned/livewindowframecropper_p.h"
#include "../src/presentation/pinned/livewindownativemapping_p.h"

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
bool same(const QRectF& actual, const QRectF& expected) {
    constexpr qreal tolerance = 0.00001;
    return qAbs(actual.x() - expected.x()) < tolerance &&
           qAbs(actual.y() - expected.y()) < tolerance &&
           qAbs(actual.width() - expected.width()) < tolerance &&
           qAbs(actual.height() - expected.height()) < tolerance;
}

void frameCropsPreservePixelsAndReuseReleasedStorage() {
    using namespace snow_shot::presentation::live_window_detail;
    QImage image(32, 24, QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(x * 7, y * 9, 90, 128 + x));
    const QRectF bounds(100, 200, 32, 24);
    FrameCropper cropper;
    const auto whole = cropper.crop(image, bounds, QRectF(0, 0, 32, 24), image.size());
    require(whole.constBits() == image.constBits(), "A complete unscaled frame shares its pixels.");
    const QRect region(3, 4, 8, 6);
    auto first = cropper.crop(image, bounds, region, region.size());
    auto second = cropper.crop(image, bounds, region.translated(1, 1), region.size());
    require(first == image.copy(region) && second == image.copy(region.translated(1, 1)),
            "Integral crops preserve exact source pixels, including alpha and row offsets.");
    const auto* released = first.constBits();
    first = {};
    auto reused = cropper.crop(image, bounds, region.translated(2, 2), region.size());
    require(reused.constBits() == released && reused == image.copy(region.translated(2, 2)),
            "A released crop allocation is reused without detaching a retained reader.");
    require(second == image.copy(region.translated(1, 1)),
            "Reusing a crop never overwrites a frame still held by a reader.");
    const auto padded = cropper.crop(image, bounds, QRectF(29, 21, 8, 6), region.size());
    require(padded.size() == region.size() && padded.pixelColor(0, 0) == image.pixelColor(29, 21) &&
                padded.pixelColor(3, 0).alpha() == 0 && padded.pixelColor(0, 3).alpha() == 0,
            "A shrinking source leaves the missing crop edges transparent.");
    const auto blank = cropper.crop(image, bounds, QRectF(40, 40, 8, 6), region.size());
    require(blank.pixelColor(0, 0).alpha() == 0 && blank.pixelColor(7, 5).alpha() == 0,
            "A crop outside the source clears every reused pixel.");
    const QRectF fractional(1.25, 2.75, 10.5, 8.5);
    const QSize scaled(21, 17);
    const auto actual = cropper.crop(image, bounds, fractional, scaled);
    QImage reference(scaled, QImage::Format_RGBA8888);
    reference.fill(Qt::transparent);
    reference.setColorSpace(image.colorSpace());
    {
        QPainter painter(&reference);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(QPointF(), scaled), image, fractional);
    }
    require(actual == reference, "Fractional and scaled crops retain smooth raster sampling.");
    cropper.clear();
    require(second == image.copy(region.translated(1, 1)),
            "Releasing the crop pool preserves outstanding immutable frames.");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    frameCropsPreservePixelsAndReuseReleasedStorage();
    using snow_shot::presentation::live_window_detail::nativeFramePlacement;
    for (int turn = 0; turn < 4; ++turn) {
        for (bool flip : {false, true}) {
            const QRectF crop(13.5, 24.5, 60, 40);
            const QTransform transform = QTransform().rotate(turn * 90).scale(flip ? -1 : 1, 1);
            const QSizeF output = turn % 2 ? QSizeF(80, 120) : QSizeF(120, 80);
            const auto placement = nativeFramePlacement(crop, transform, output);
            require(placement && same(placement->mapRect(crop), QRectF(QPointF(), output)),
                    "Native transforms map the fixed crop onto the entire destination.");
            const auto reference = QImage::trueMatrix(transform, 60, 40);
            for (const QPointF& point : {QPointF(0.5, 0.5), QPointF(59.5, 39.5)}) {
                const QPointF actual = placement->map(point + crop.topLeft());
                const QPointF expected = reference.map(point) * 2;
                require(QLineF(actual, expected).length() < 0.00001,
                        "Native crop orientation agrees with raster rotation and reflection.");
            }
        }
    }
    require(
        !nativeFramePlacement({}, {}, QSizeF(100, 100)) &&
            !nativeFramePlacement(QRectF(0, 0, 10, 10), QTransform().scale(0, 1), QSizeF(10, 10)),
        "Empty or singular native mappings are rejected.");
    using snow_shot::presentation::live_window_detail::cropMapping;
    using snow_shot::presentation::live_window_detail::windowRelativeCrop;
    for (const QPoint& origin : {QPoint(118, 120), QPoint(-1800, -400)}) {
        const QRectF selected(238, 200, 128, 80);
        require(same(windowRelativeCrop(selected, origin, origin - QPoint(9, 0)),
                     QRectF(247, 200, 128, 80)),
                "A native source crop includes invisible frame margins without scaling pixels.");
        require(same(windowRelativeCrop(selected, origin, origin), selected),
                "Frameless source crops retain their original coordinates and size.");
        require(same(windowRelativeCrop(QRectF(0, 0, 40, 20), origin, origin - QPoint(12, 8)),
                     QRectF(12, 8, 40, 20)),
                "Clipped source crops use the same outer-window origin on both axes.");
    }
    const QRectF crop(30, 40, 100, 80);
    const QRectF destination(5, 7, 200, 160);
    const auto initial = cropMapping(crop, QRectF(0, 0, 300, 200), destination);
    require(same(initial.source, crop) && same(initial.destination, destination),
            "The initial crop occupies the complete displayed content extent.");

    const auto smaller = cropMapping(crop, QRectF(0, 0, 70, 60), destination);
    require(same(smaller.source, QRectF(30, 40, 40, 20)),
            "Source resizing clips only the available source-local pixels.");
    require(same(smaller.destination, QRectF(5, 7, 80, 40)),
            "Clipped source content occupies its corresponding destination subsection.");

    const auto missing = cropMapping(crop, QRectF(0, 0, 20, 20), destination);
    require(missing.source.isEmpty() && missing.destination.isEmpty(),
            "A crop outside current source bounds remains entirely blank.");

    const auto restored = cropMapping(crop, QRectF(0, 0, 300, 200), destination);
    require(same(restored.source, initial.source) &&
                same(restored.destination, initial.destination),
            "Growing the source restores the original crop and extent.");

    const auto offset =
        cropMapping(QRectF(-10, -20, 50, 60), QRectF(0, 0, 100, 100), QRectF(0, 0, 100, 120));
    require(same(offset.source, QRectF(0, 0, 40, 40)) &&
                same(offset.destination, QRectF(20, 40, 80, 80)),
            "Clipping the leading edges preserves blank offsets instead of recentering.");

    const auto fractional =
        cropMapping(QRectF(1.5, 2.5, 30, 20), QRectF(0, 0, 21.5, 12.5), QRectF(0, 0, 45, 30));
    require(same(fractional.source, QRectF(1.5, 2.5, 20, 10)) &&
                same(fractional.destination, QRectF(0, 0, 30, 15)),
            "Fractional desktop points retain their backing-pixel scale.");
    require(cropMapping({}, QRectF(0, 0, 100, 100), destination).source.isEmpty(),
            "An empty crop never divides by zero.");
    require(cropMapping(crop, QRectF(0, 0, 100, 100), {}).destination.isEmpty(),
            "An empty destination has no visible content.");
    return EXIT_SUCCESS;
}
