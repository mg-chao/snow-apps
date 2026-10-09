#include "screenshotpinnedresizegeometry.h"

#include <QCoreApplication>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace resize_geometry = screenshot_pinned_resize_geometry;
using DragHandle = resize_geometry::DragHandle;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QPoint fixedAnchor(const QRect& rect, DragHandle handle) {
    switch (handle) {
    case DragHandle::TopLeft:
        return rect.bottomRight();
    case DragHandle::Top:
    case DragHandle::TopRight:
        return rect.bottomLeft();
    case DragHandle::Right:
    case DragHandle::BottomRight:
    case DragHandle::Bottom:
        return rect.topLeft();
    case DragHandle::BottomLeft:
    case DragHandle::Left:
        return rect.topRight();
    }
    return {};
}

void testEveryHandlePreservesItsFixedAnchor() {
    const QSize baseline(320, 180);
    const QRect reference(QPoint(100, 200), baseline);
    struct TestCase {
        DragHandle handle;
        QRect proposed;
    };
    const std::vector<TestCase> cases{
        {DragHandle::TopLeft, QRect(20, 110, 400, 270)},
        {DragHandle::Top, QRect(100, 110, 320, 270)},
        {DragHandle::TopRight, QRect(100, 110, 480, 270)},
        {DragHandle::Right, QRect(100, 200, 400, 180)},
        {DragHandle::BottomRight, QRect(100, 200, 480, 270)},
        {DragHandle::Bottom, QRect(100, 200, 320, 270)},
        {DragHandle::BottomLeft, QRect(20, 200, 400, 270)},
        {DragHandle::Left, QRect(20, 200, 400, 180)},
    };

    for (const TestCase& testCase : cases) {
        QRect result;
        require(resize_geometry::proportionalResizeRect(testCase.proposed, reference, baseline,
                                                        testCase.handle, 0.1, 5.0, &result),
                "a valid resize proposal should produce a result");
        require(fixedAnchor(result, testCase.handle) == fixedAnchor(reference, testCase.handle),
                "resizing should preserve the fixed opposite anchor");
        require(
            qAbs(result.height() - qRound(result.width() * static_cast<double>(baseline.height()) /
                                          baseline.width())) <= 1,
            "resizing should preserve the baseline aspect ratio");
    }
}

void testDraggedEdgeDeterminesScale() {
    const QSize baseline(320, 180);
    const QRect reference(QPoint(100, 200), baseline);
    QRect horizontal;
    QRect vertical;
    QRect corner;

    require(resize_geometry::proportionalResizeRect(QRect(100, 200, 400, 181), reference, baseline,
                                                    DragHandle::Right, 0.1, 5.0, &horizontal) &&
                horizontal.size() == QSize(400, 225),
            "a horizontal handle should derive scale from the proposed width");
    require(resize_geometry::proportionalResizeRect(QRect(100, 200, 321, 270), reference, baseline,
                                                    DragHandle::Bottom, 0.1, 5.0, &vertical) &&
                vertical.size() == QSize(480, 270),
            "a vertical handle should derive scale from the proposed height");
    require(resize_geometry::proportionalResizeRect(QRect(100, 200, 400, 270), reference, baseline,
                                                    DragHandle::BottomRight, 0.1, 5.0, &corner) &&
                corner.size() == QSize(480, 270),
            "a corner handle should contain both proposed dimensions");
}

void testScaleLimitsUseExactBaselineMultiples() {
    const QSize baseline(853, 479);
    const QRect reference(QPoint(10, 20), baseline);
    QRect minimum;
    QRect maximum;

    require(resize_geometry::proportionalResizeRect(QRect(10, 20, 1, 1), reference, baseline,
                                                    DragHandle::Right, 0.1, 5.0, &minimum) &&
                minimum.size() == resize_geometry::scaledSize(baseline, 0.1),
            "resizing below the minimum should clamp to the minimum scale");
    require(resize_geometry::proportionalResizeRect(QRect(10, 20, 9999, 9999), reference, baseline,
                                                    DragHandle::BottomRight, 0.1, 5.0, &maximum) &&
                maximum.size() == resize_geometry::scaledSize(baseline, 5.0),
            "resizing above the maximum should clamp to the maximum scale");
}

void testWheelScaleAnchorsUseHalfOpenEdges() {
    const QRect reference(101, 203, 319, 181);
    const QSize targetSize(477, 269);
    using ScaleAnchor = resize_geometry::ScaleAnchor;

    const QRect topLeft =
        resize_geometry::anchoredScaleRect(reference, targetSize, ScaleAnchor::TopLeft);
    const QRect topRight =
        resize_geometry::anchoredScaleRect(reference, targetSize, ScaleAnchor::TopRight);
    const QRect bottomLeft =
        resize_geometry::anchoredScaleRect(reference, targetSize, ScaleAnchor::BottomLeft);
    const QRect bottomRight =
        resize_geometry::anchoredScaleRect(reference, targetSize, ScaleAnchor::BottomRight);

    require(topLeft.topLeft() == reference.topLeft(),
            "top-left wheel scaling should preserve its half-open anchor");
    require(topRight.left() + topRight.width() == reference.left() + reference.width() &&
                topRight.top() == reference.top(),
            "top-right wheel scaling should preserve its half-open anchor");
    require(bottomLeft.left() == reference.left() &&
                bottomLeft.top() + bottomLeft.height() == reference.top() + reference.height(),
            "bottom-left wheel scaling should preserve its half-open anchor");
    require(bottomRight.left() + bottomRight.width() == reference.left() + reference.width() &&
                bottomRight.top() + bottomRight.height() == reference.top() + reference.height(),
            "bottom-right wheel scaling should preserve its half-open anchor");
}

void testWheelScalePreservesCenterAndMousePosition() {
    const QRect reference(100, 200, 320, 180);
    const QSize targetSize(480, 270);
    using ScaleAnchor = resize_geometry::ScaleAnchor;

    const QRect centered =
        resize_geometry::anchoredScaleRect(reference, targetSize, ScaleAnchor::Center);
    const QPointF referenceCenter(reference.left() + reference.width() / 2.0,
                                  reference.top() + reference.height() / 2.0);
    const QPointF centeredCenter(centered.left() + centered.width() / 2.0,
                                 centered.top() + centered.height() / 2.0);
    require(centeredCenter == referenceCenter,
            "center wheel scaling should preserve the geometric center");

    const QRect oddReference(101, 203, 319, 181);
    const QRect parityChanged =
        resize_geometry::anchoredScaleRect(oddReference, QSize(478, 270), ScaleAnchor::Center);
    const QRect roundTrip =
        resize_geometry::anchoredScaleRect(parityChanged, oddReference.size(), ScaleAnchor::Center);
    require(roundTrip == oddReference,
            "center wheel scaling should not drift across dimension parity changes");

    const QPointF mousePosition(180.0, 320.0);
    const QRect aroundMouse = resize_geometry::anchoredScaleRect(
        reference, targetSize, ScaleAnchor::MousePosition, mousePosition);
    const QPointF oldNormalized((mousePosition.x() - reference.left()) / reference.width(),
                                (mousePosition.y() - reference.top()) / reference.height());
    const QPointF mappedMouse(aroundMouse.left() + oldNormalized.x() * aroundMouse.width(),
                              aroundMouse.top() + oldNormalized.y() * aroundMouse.height());
    require((mappedMouse - mousePosition).manhattanLength() <= 1.0,
            "mouse-position wheel scaling should preserve the normalized cursor location");
}

void testThumbnailEntryPreservesCurrentMousePosition() {
    using ScaleAnchor = resize_geometry::ScaleAnchor;
    for (const QRect original : {QRect(100, 200, 600, 400), QRect(-1600, -300, 900, 600)}) {
        for (const int thumbnailSize : {83, 125, 166}) {
            const QPointF shrinkCursor(original.x() + original.width() / 3.0,
                                       original.y() + original.height() / 4.0);
            const QRect thumbnail =
                resize_geometry::anchoredScaleRect(original, QSize(thumbnailSize, thumbnailSize),
                                                   ScaleAnchor::MousePosition, shrinkCursor);
            require(qAbs(thumbnail.x() + thumbnail.width() / 3.0 - shrinkCursor.x()) <= 0.5 &&
                        qAbs(thumbnail.y() + thumbnail.height() / 4.0 - shrinkCursor.y()) <= 0.5,
                    "thumbnail shrink must retain the mouse anchor within pixel rounding");
        }
    }
}

void testWheelScaleSettingNames() {
    using ScaleAnchor = resize_geometry::ScaleAnchor;
    require(
        resize_geometry::scaleAnchorFromSetting(u"top_left") == ScaleAnchor::TopLeft &&
            resize_geometry::scaleAnchorFromSetting(u"top_right") == ScaleAnchor::TopRight &&
            resize_geometry::scaleAnchorFromSetting(u"bottom_left") == ScaleAnchor::BottomLeft &&
            resize_geometry::scaleAnchorFromSetting(u"bottom_right") == ScaleAnchor::BottomRight &&
            resize_geometry::scaleAnchorFromSetting(u"center") == ScaleAnchor::Center &&
            resize_geometry::scaleAnchorFromSetting(u"mouse_position") ==
                ScaleAnchor::MousePosition &&
            resize_geometry::scaleAnchorFromSetting(u"invalid") == ScaleAnchor::MousePosition,
        "wheel scale setting names should map to the documented anchors and default safely");
}

void testInvalidInputsAreRejected() {
    QRect result;
    require(!resize_geometry::proportionalResizeRect({}, QRect(0, 0, 100, 100), QSize(100, 100),
                                                     DragHandle::Right, 0.1, 5.0, &result),
            "an empty proposal should be rejected");
    require(!resize_geometry::proportionalResizeRect(QRect(0, 0, 100, 100), QRect(0, 0, 100, 100),
                                                     QSize(100, 100), DragHandle::Right, 2.0, 1.0,
                                                     &result),
            "an inverted scale range should be rejected");
    require(resize_geometry::scaledSize({}, 1.0).isEmpty(),
            "an invalid baseline should not produce a scaled size");
}

void testTrackLimitsAllowEveryTransitionFrame() {
    const QSize minimum(400, 200);
    const QSize maximum(20000, 10000);
    for (const QSize enlarged : {QSize(8000, 4000), QSize(24000, 12000)}) {
        const QSize thumbnail(120, 120);
        for (const bool shrinking : {false, true}) {
            const QSize start = shrinking ? enlarged : thumbnail;
            const QSize end = shrinking ? thumbnail : enlarged;
            const auto limits = resize_geometry::trackSizeLimits(minimum, maximum, start, end);
            for (int step = 0; step <= 100; ++step) {
                const QSize frame = start + (end - start) * (step / 100.0);
                require(frame.expandedTo(limits.minimum) == frame &&
                            frame.boundedTo(limits.maximum) == frame,
                        "native limits must allow every thumbnail and restoration frame");
            }
        }
    }
    const auto unchanged = resize_geometry::trackSizeLimits(minimum, maximum, {}, {});
    require(unchanged.minimum == minimum && unchanged.maximum == maximum,
            "missing transition geometry must preserve normal scale limits");
}
void testCrossingResizeKeepsProportionsAndAnchor() {
    using H = resize_geometry::DragHandle;
    const QRect origin(100, 200, 200, 100);
    const QPoint deltas[] = {{300, 150},   {0, 150},  {-300, 150}, {-300, 0},
                             {-300, -150}, {0, -150}, {300, -150}, {300, 0}};
    const QPoint anchors[] = {{300, 300}, {100, 300}, {100, 300}, {100, 200},
                              {100, 200}, {100, 200}, {300, 200}, {300, 200}};
    const H opposites[] = {H::BottomRight, H::Bottom, H::BottomLeft, H::Left,
                           H::TopLeft,     H::Top,    H::TopRight,   H::Right};
    for (int index = 0; index < 8; ++index) {
        H effective = H(index);
        QRect result;
        require(resize_geometry::dragResizeRect(origin, deltas[index], origin.size(), H(index), .1,
                                                5., &effective, &result),
                "crossing resize failed");
        require(result.size() == QSize(100, 50) && effective == opposites[index],
                "crossing must switch handles and retain aspect ratio");
        const bool left =
            effective == H::Left || effective == H::TopLeft || effective == H::BottomLeft;
        const bool top = effective == H::Top || effective == H::TopLeft || effective == H::TopRight;
        require(result.topLeft() + QPoint(left ? result.width() : 0, top ? result.height() : 0) ==
                    anchors[index],
                "crossing must preserve the original opposite anchor");
        require(resize_geometry::dragResizeRect(origin, {}, origin.size(), H(index), .1, 5.,
                                                &effective, &result) &&
                    result == origin && effective == H(index),
                "crossing back must restore the original rectangle");
    }
    H effective = H::TopLeft;
    QRect result;
    require(resize_geometry::dragResizeRect(origin, QPoint(-201, -101), origin.size(),
                                            H::BottomRight, .1, 5., &effective, &result) &&
                result == QRect(80, 190, 20, 10),
            "minimum scale must apply on the crossed side");
    require(resize_geometry::dragResizeRect(origin, QPoint(-200, -100), origin.size(),
                                            H::BottomRight, .1, 5., &effective, &result) &&
                effective == H::TopLeft,
            "zero distance must retain the previous corner");
    require(resize_geometry::dragResizeRect(origin, QPoint(-3000, 0), origin.size(), H::BottomRight,
                                            .1, 5., &effective, &result) &&
                result == QRect(-900, 200, 1000, 500),
            "single-axis crossing must preserve maximum scale and the other axis direction");
}

void testReservedFrameHitZones() {
    const QRectF outer(0, 0, 132, 92);
    const QRectF content(16, 16, 100, 60);
    const QSizeF edgeBand(6, 6);
    const QPointF points[] = {{8, 8},    {40, 8},  {124, 8}, {124, 40},
                              {124, 84}, {40, 84}, {8, 84},  {8, 40}};
    for (int index = 0; index < 8; ++index)
        require(resize_geometry::hitHandle(points[index], outer, content, edgeBand) ==
                    DragHandle(index),
                "each reserved side/corner must select its resize handle");
    require(resize_geometry::hitHandle({17, 17}, outer, content, edgeBand) == DragHandle::TopLeft &&
                resize_geometry::hitHandle({17, 8}, outer, content, edgeBand) == DragHandle::Top,
            "content edge bands and outside-content side strips must remain distinct");
    require(!resize_geometry::hitHandle({60, 40}, outer, content, edgeBand) &&
                !resize_geometry::hitHandle({132, 40}, outer, content, edgeBand) &&
                !resize_geometry::hitHandle({60, 92}, outer, content, edgeBand),
            "interior and exclusive outer edges must not resize");
    const QRectF tinyOuter(0, 0, 33, 33);
    const QRectF tinyContent(16, 16, 1, 1);
    require(resize_geometry::hitHandle({16.5, 16.5}, tinyOuter, tinyContent, edgeBand) ==
                    DragHandle::TopLeft &&
                resize_geometry::hitHandle({16.9, 16.9}, tinyOuter, tinyContent, edgeBand) ==
                    DragHandle::BottomRight,
            "overlapping tiny-window edge bands must choose the nearest corner");
    require(!resize_geometry::hitHandle({16, 16}, outer, content, QSizeF(-1, 6)) &&
                !resize_geometry::hitHandle({16, 16}, outer, content,
                                            QSizeF(std::numeric_limits<qreal>::quiet_NaN(), 6)),
            "invalid edge bands must be rejected");
}

void testFrameMarginDoesNotParticipateInResizeScale() {
    const QSize baseline(320, 180);
    const QRect reference(84, 184, 352, 212);
    for (int index = 0; index < 8; ++index) {
        const auto handle = DragHandle(index);
        QRect result;
        require(resize_geometry::proportionalResizeRect(QRect(50, 100, 432, 257), reference,
                                                        baseline, handle, .1, 5., &result, 16) &&
                    result.size() == QSize(432, 257) &&
                    fixedAnchor(result, handle) == fixedAnchor(reference, handle),
                "proportional resizing must scale content and preserve the fixed outer anchor");
    }
    auto effective = DragHandle::Right;
    QRect result;
    require(resize_geometry::proportionalResizeRect(QRect(84, 184, 1, 1), reference, baseline,
                                                    DragHandle::BottomRight, .1, 5., &result, 16) &&
                result == QRect(84, 184, 64, 50),
            "a native proposal smaller than the margin must clamp content at its minimum scale");
    require(resize_geometry::dragResizeRect(reference, {80, 0}, baseline, DragHandle::Right, .1, 5.,
                                            &effective, &result, 16) &&
                result == QRect(84, 184, 432, 257),
            "drag resizing must reserve an unchanged margin around proportional content");
    require(resize_geometry::dragResizeRect(reference, {-1000, 0}, baseline, DragHandle::Right, .1,
                                            5., &effective, &result, 16) &&
                effective == DragHandle::Left && result.width() == 712 && result.height() == 415 &&
                result.adjusted(16, 16, -16, -16).right() + 1 == 100,
            "crossing a content edge must retain the content anchor and constant margin");
}

void testDpiScalingPreservesPhysicalFrameMargin() {
    require(resize_geometry::dpiScaledOuterSize({132, 92}, 96, 144, 16) == QSize(182, 122) &&
                resize_geometry::dpiScaledOuterSize({182, 122}, 144, 96, 16) == QSize(132, 92),
            "DPI scaling must scale content alone and keep all four physical margins fixed");
    require(resize_geometry::dpiScaledOuterSize({33, 33}, 192, 96, 16) == QSize(33, 33),
            "one-pixel content must remain visible during downscaling");
    require(resize_geometry::dpiScaledOuterSize({133, 93}, 96, 192, 16) == QSize(234, 154),
            "DPI negotiation must use the pending outer extent and preserve its odd content size");
    require(
        resize_geometry::dpiScaledOuterSize({32, 33}, 96, 144, 16).isEmpty() &&
            resize_geometry::dpiScaledOuterSize({132, 92}, 0, 144, 16).isEmpty() &&
            resize_geometry::dpiScaledOuterSize({132, 92}, 1, std::numeric_limits<int>::max(), 16)
                .isEmpty(),
        "invalid and overflowing DPI geometry must be rejected");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    try {
        testCrossingResizeKeepsProportionsAndAnchor();
        testReservedFrameHitZones();
        testFrameMarginDoesNotParticipateInResizeScale();
        testDpiScalingPreservesPhysicalFrameMargin();
        testEveryHandlePreservesItsFixedAnchor();
        testDraggedEdgeDeterminesScale();
        testScaleLimitsUseExactBaselineMultiples();
        testWheelScaleAnchorsUseHalfOpenEdges();
        testWheelScalePreservesCenterAndMousePosition();
        testWheelScaleSettingNames();
        testThumbnailEntryPreservesCurrentMousePosition();
        testInvalidInputsAreRejected();
        testTrackLimitsAllowEveryTransitionFrame();
    } catch (const std::exception& error) {
        std::cerr << "screenshot pinned resize geometry test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "screenshot pinned resize geometry tests passed\n";
    return 0;
}
