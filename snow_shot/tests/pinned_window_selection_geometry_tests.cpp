#include "pinnedwindowselectiongeometry.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QCoreApplication>
#include <QRect>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace geometry = pinned_window_selection_geometry;
using Alignment = SnowCanvasSelectionAlignment;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-9, message);
}

void everyAlignmentUsesSelectionBounds() {
    const QList<QRectF> rectangles{{100, 200, 100, 70}, {300, -20, 200, 40}, {-100, 80, 80, 120}};
    struct Case {
        Alignment alignment;
        bool horizontal;
        int edge;
        double target;
    };
    const std::array cases{Case{Alignment::AlignLeft, true, 0, -100},
                           Case{Alignment::AlignCenterHorizontally, true, 1, 200},
                           Case{Alignment::AlignRight, true, 2, 500},
                           Case{Alignment::AlignTop, false, 0, -20},
                           Case{Alignment::AlignCenterVertically, false, 1, 125},
                           Case{Alignment::AlignBottom, false, 2, 270}};
    for (const auto& test : cases) {
        const auto result = geometry::alignmentTargets(rectangles, test.alignment);
        require(result.has_value() && result->size() == rectangles.size(),
                "every alignment must retain every window");
        for (qsizetype index = 0; index < rectangles.size(); ++index) {
            const auto& target = (*result)[index];
            require(target.size() == rectangles[index].size(), "alignment must retain each size");
            requireNear(test.horizontal ? target.y() : target.x(),
                        test.horizontal ? rectangles[index].y() : rectangles[index].x(),
                        "alignment must preserve the other axis");
            const double start = test.horizontal ? target.x() : target.y();
            const double extent = test.horizontal ? target.width() : target.height();
            requireNear(start + extent * test.edge / 2.0, test.target,
                        "alignment must use the half-open selection edge or center");
        }
    }
}

void distributionPreservesOrderAndEqualizesGaps() {
    const QList<QRectF> rectangles{{600, 0, 100, 10}, {100, 20, 100, 40}, {220, -10, 200, 30}};
    const auto horizontal =
        geometry::alignmentTargets(rectangles, Alignment::DistributeHorizontally);
    require(horizontal.has_value(), "horizontal distribution must succeed");
    require((*horizontal)[0] == rectangles[0] && (*horizontal)[1] == rectangles[1],
            "distribution must keep the two outer edges in place");
    require((*horizontal)[2] == QRectF(300, -10, 200, 30),
            "unequal widths must produce equal gaps, retaining input order");
    QList<QRectF> verticalInput;
    for (const auto& rectangle : rectangles)
        verticalInput.append({rectangle.y(), rectangle.x(), rectangle.height(), rectangle.width()});
    const auto vertical =
        geometry::alignmentTargets(verticalInput, Alignment::DistributeVertically);
    require(vertical.has_value(), "vertical distribution must succeed");
    for (qsizetype index = 0; index < rectangles.size(); ++index)
        require((*vertical)[index] == QRectF((*horizontal)[index].y(), (*horizontal)[index].x(),
                                             (*horizontal)[index].height(),
                                             (*horizontal)[index].width()),
                "vertical distribution must mirror horizontal distribution");
}

void overlapDistributionMatchesEngineFallback() {
    const QList<QRectF> rectangles{{0, 0, 100, 10}, {90, 1, 100, 12}, {30, 2, 100, 14}};
    const auto result = geometry::alignmentTargets(rectangles, Alignment::DistributeHorizontally);
    require(result.has_value() && (*result)[0] == rectangles[0] && (*result)[1] == rectangles[1] &&
                (*result)[2] == QRectF(45, 2, 100, 14),
            "overlapping windows must distribute centers while preserving outer units");

    const QList<QRectF> equalCenters{
        {0, 0, 20, 20}, {20, 0, 120, 40}, {70, 0, 20, 40}, {140, 0, 20, 20}};
    const auto stable = geometry::alignmentTargets(equalCenters, Alignment::DistributeHorizontally);
    require(stable.has_value(), "equal-center distribution must succeed");
    requireNear((*stable)[1].center().x(), 10.0 + 140.0 / 3.0,
                "the first equal-center window must retain its stable order");
    requireNear((*stable)[2].center().x(), 10.0 + 280.0 / 3.0,
                "the second equal-center window must retain its stable order");

    const QList<QRectF> nested{{0, 0, 200, 10}, {10, 0, 20, 10}, {170, 0, 20, 10}};
    const auto nestedResult = geometry::alignmentTargets(nested, Alignment::DistributeHorizontally);
    require(nestedResult.has_value() && (*nestedResult)[0] == nested[0],
            "one nested outer window must remain the engine reference unit");
    requireNear((*nestedResult)[1].center().x(), 100.0,
                "nested overlap must match the engine's shared-outer-unit fallback");
    requireNear((*nestedResult)[2].center().x(), 100.0,
                "nested overlap must retain the engine's zero center step");
}

void fractionalEdgesRemainHalfOpen() {
    const QList<QRectF> rectangles{{-120.25, 31.5, 101.5, 39.25}, {140.75, -60.25, 77.25, 82.5}};
    const auto right = geometry::alignmentTargets(rectangles, Alignment::AlignRight);
    const auto bottom = geometry::alignmentTargets(rectangles, Alignment::AlignBottom);
    require(right.has_value() && bottom.has_value(), "fractional alignment must succeed");
    for (qsizetype index = 0; index < rectangles.size(); ++index) {
        requireNear((*right)[index].x() + (*right)[index].width(), 218.0,
                    "right alignment must preserve fractional half-open edges");
        requireNear((*bottom)[index].y() + (*bottom)[index].height(), 70.75,
                    "bottom alignment must preserve fractional half-open edges");
    }
    require(geometry::alignmentTargets(*right, Alignment::AlignRight) == right,
            "repeated alignment must be idempotent");
}

void invalidSelectionsProduceNoPartialTargets() {
    const QList<QRectF> valid{{0, 0, 20, 10}, {30, 0, 20, 10}};
    require(!geometry::alignmentTargets({}, Alignment::AlignLeft) &&
                !geometry::alignmentTargets({valid.first()}, Alignment::AlignLeft) &&
                !geometry::alignmentTargets(valid, Alignment::DistributeHorizontally) &&
                !geometry::alignmentTargets(valid, static_cast<Alignment>(99)),
            "invalid operations and insufficient selections must be rejected");
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const QRectF invalid :
         {QRectF(0, 0, 0, 10), QRectF(0, 0, -1, 10), QRectF(infinity, 0, 20, 10),
          QRectF(0, nan, 20, 10), QRectF(0, 0, infinity, 10)}) {
        require(!geometry::alignmentTargets({valid.first(), invalid}, Alignment::AlignLeft),
                "one invalid member must reject the complete alignment");
        require(!geometry::scaledTargets({valid.first(), invalid}, 1.2),
                "one invalid member must reject the complete scaling");
    }
    require(!geometry::alignmentTargets({QRectF(-1e308, 0, 1e307, 10), QRectF(1e308, 0, 1e307, 10)},
                                        Alignment::AlignLeft),
            "overflowing selection bounds must be rejected");
    require(!geometry::scaledTargets({QRectF(0, 0, 1e308, 10)}, 2.0),
            "overflowing scale targets must be rejected");
}

void commonScaleFactorHonorsEveryWindowLimit() {
    const std::array percents{20.0, 400.0};
    requireNear(*geometry::sharedScaleFactor(percents, 0.1), 0.5,
                "the smallest-scale member must constrain shrinking");
    requireNear(*geometry::sharedScaleFactor(percents, 4.0), 1.25,
                "the largest-scale member must constrain growing");
    requireNear(*geometry::sharedScaleFactor(percents, 1.1), 1.1,
                "a valid shared factor must remain unchanged");
    const std::array atLimits{10.0, 500.0};
    requireNear(*geometry::sharedScaleFactor(atLimits, 0.5), 1.0,
                "mixed members at opposite limits must block shrinking");
    requireNear(*geometry::sharedScaleFactor(atLimits, 2.0), 1.0,
                "mixed members at opposite limits must block growing");
    const std::array custom{25.0, 150.0};
    requireNear(*geometry::sharedScaleFactor(custom, 4.0, 20.0, 200.0), 4.0 / 3.0,
                "custom scale limits must apply to every member");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::array invalid{100.0, nan};
    const std::array zero{100.0, 0.0};
    const std::array incompatible{1.0, 1000.0};
    require(!geometry::sharedScaleFactor({}, 1.0) && !geometry::sharedScaleFactor(invalid, 1.0) &&
                !geometry::sharedScaleFactor(zero, 1.0) &&
                !geometry::sharedScaleFactor(incompatible, 1.0) &&
                !geometry::sharedScaleFactor(percents, 0.0) &&
                !geometry::sharedScaleFactor(percents, nan) &&
                !geometry::sharedScaleFactor(percents, 1.0, 0.0, 500.0) &&
                !geometry::sharedScaleFactor(percents, 1.0, 500.0, 10.0),
            "invalid or incompatible scale ranges must produce no shared factor");
}

void scalingRetainsEachTopLeftWithoutRoundingDrift() {
    const QList<QRectF> originals{{-41.5, 62.25, 101, 79}, {172.75, -60.5, 301, 199}};
    constexpr double factor = 1.37;
    const auto grown = geometry::scaledTargets(originals, factor);
    require(grown.has_value(), "shared scaling must succeed");
    for (qsizetype index = 0; index < originals.size(); ++index) {
        require((*grown)[index].topLeft() == originals[index].topLeft(),
                "each selected window must retain its own top-left anchor");
        requireNear((*grown)[index].width() / originals[index].width(), factor,
                    "every width must receive the same factor");
        requireNear((*grown)[index].height() / originals[index].height(), factor,
                    "every height must receive the same factor");
    }
    const auto shrunk = geometry::scaledTargets(*grown, 1.0 / factor);
    require(shrunk.has_value(), "inverse shared scaling must succeed");
    for (qsizetype index = 0; index < originals.size(); ++index) {
        requireNear((*shrunk)[index].width(), originals[index].width(),
                    "inverse scaling must restore odd widths before final rounding");
        requireNear((*shrunk)[index].height(), originals[index].height(),
                    "inverse scaling must restore odd heights before final rounding");
    }
    const auto restored = geometry::scaledTargets(originals, 1.0);
    require(restored == originals, "gesture snapshots must restore original rectangles exactly");
    for (qsizetype index = 0; index < originals.size(); ++index)
        require((*shrunk)[index].size().toSize() == originals[index].size().toSize(),
                "inverse factors must restore odd native sizes after final rounding");
    require(!geometry::scaledTargets({}, factor) && !geometry::scaledTargets(originals, -1.0),
            "empty selections and nonpositive factors must be rejected");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        everyAlignmentUsesSelectionBounds();
        distributionPreservesOrderAndEqualizesGaps();
        overlapDistributionMatchesEngineFallback();
        fractionalEdgesRemainHalfOpen();
        invalidSelectionsProduceNoPartialTargets();
        commonScaleFactorHonorsEveryWindowLimit();
        scalingRetainsEachTopLeftWithoutRoundingDrift();
        std::cout << "Pinned window selection geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
