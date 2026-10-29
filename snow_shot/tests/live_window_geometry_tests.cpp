#include "../src/presentation/pinned/livewindownative_p.h"

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
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using snow_shot::presentation::live_window_detail::cropMapping;
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
