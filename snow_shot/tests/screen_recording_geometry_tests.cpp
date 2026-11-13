#include "presentation/recording/screenrecordinggeometry.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#include <QGuiApplication>
#include "../src/presentation/resizegeometry.h"
#include <QRect>
#include <QScreen>
#include <QtMath>

#include <cstdlib>
#include <iostream>

namespace recording = snow_shot::presentation::recording;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void observedNativeGeometryFollowsDisplaysWithoutRescalingPhysicalCoordinates() {
    // Include negative origins, a display seam, odd extents, and the minimum-size window.
    for (const QRect client : {QRect(-1920, -200, 647, 367), QRect(-80, 40, 320, 240),
                               QRect(3000, 180, 1286, 726), QRect(40, 40, 8, 8)}) {
        for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            const auto observed = recording::screenRecordingObservedGeometry(client, scale);
            require(observed.recordingRegion == client.adjusted(3, 3, -3, -3),
                    "DPI and display origins must not rescale native capture coordinates");
            require(qAbs(observed.selectionRect.width() * scale -
                         observed.recordingRegion.width()) < 0.0001 &&
                        qAbs(observed.selectionRect.height() * scale -
                             observed.recordingRegion.height()) < 0.0001,
                    "fractional logical layout must cover exactly the physical capture pixels");
            const auto border = recording::screenRecordingAreaBorderGeometry(
                observed.frameRect, observed.selectionRect, observed.paddingWidth);
            require(qAbs(border.top.height() * scale - 2.0) < 0.0001 &&
                        qAbs(border.right.width() * scale - 2.0) < 0.0001,
                    "native resizing must retain the physical frame width");
            require(
                qAbs(observed.frameRect.width() * scale - client.width()) < 0.0001,
                "fractional-DPI borders must reach the native client edge without rounding loss");
        }
    }
    const auto rounded = recording::screenRecordingObservedGeometry(QRect(36, 36, 329, 249), 1.5,
                                                                    QMargins(4, 4, 4, 4));
    require(rounded.recordingRegion == QRect(40, 40, 321, 241),
            "Qt's fractional-DPI outer rounding must not change the initial screenshot selection");
    require(!recording::screenRecordingObservedGeometry(QRect(0, 0, 7, 8), 1.25)
                 .recordingRegion.isValid(),
            "undersized native geometry must not publish an invalid capture selection");
}

void recordingFrameStaysOutsideTheSelection(qreal scale) {
    const QRectF selection(300.4, 200.8, 641.2, 359.6);
    const recording::ScreenRecordingAreaFrameGeometry geometry =
        recording::screenRecordingAreaFrameGeometry(selection, scale);

    require(QRectF(geometry.windowGeometry).contains(selection),
            "recording frame window should contain the complete selection");
    const QRectF displayedSelection =
        geometry.selectionRect.translated(geometry.windowGeometry.topLeft());
    require(displayedSelection == selection,
            "recording frame interior should exactly match the screenshot selection");
    const QRectF displayedFrame = geometry.frameRect.translated(geometry.windowGeometry.topLeft());
    require(geometry.windowGeometry.contains(displayedFrame.toAlignedRect()),
            "recording border should expand only outside the screenshot selection");
    const qreal frameInset = geometry.borderWidth + geometry.paddingWidth;
    require(qAbs(selection.left() - displayedFrame.left() - frameInset) < 0.0001,
            "recording frame should leave the requested left padding");
    require(qAbs(selection.top() - displayedFrame.top() - frameInset) < 0.0001,
            "recording frame should leave the requested top padding");
    require(qAbs(displayedFrame.left() + displayedFrame.width() - selection.left() -
                 selection.width() - frameInset) < 0.0001,
            "recording frame should leave the requested right padding");
    require(qAbs(displayedFrame.top() + displayedFrame.height() - selection.top() -
                 selection.height() - frameInset) < 0.0001,
            "recording frame should leave the requested bottom padding");
    require(qAbs(geometry.borderWidth * scale - 2.0) < 0.0001,
            "recording border should remain two physical pixels on every side");
    require(qAbs(geometry.paddingWidth * scale - 1.0) < 0.0001,
            "recording frame padding should remain one physical pixel on every side");
}

void followFramePansWithoutResizingOrClipping() {
    for (qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
        for (QSizeF extent :
             {QSizeF(8.25, 6.5), QSizeF(641.2, 359.6), QSizeF(1920, 1080), QSizeF(3840, 2160)}) {
            QSize windowSize;
            for (int step = 0; step < 160; ++step) {
                const QRectF selection(
                    QPointF((-80 + step * 0.25) / scale, (40 + step * 0.125) / scale),
                    extent / scale);
                const auto geometry =
                    recording::screenRecordingFollowFrameGeometry(selection, scale);
                if (step == 0)
                    windowSize = geometry.windowGeometry.size();
                require(geometry.windowGeometry.size() == windowSize,
                        "panning a fixed viewport must retain a fixed backing-store extent");
                require(geometry.selectionRect.translated(geometry.windowGeometry.topLeft()) ==
                            selection,
                        "stable backing geometry must preserve the exact fractional viewport");
                // Model independent rounding of the native window origin and extent.
                const QRectF native(QPointF(qRound(geometry.windowGeometry.x() * scale) / scale,
                                            qRound(geometry.windowGeometry.y() * scale) / scale),
                                    QSizeF(qRound(windowSize.width() * scale) / scale,
                                           qRound(windowSize.height() * scale) / scale));
                const QRectF frame =
                    geometry.frameRect.translated(geometry.windowGeometry.topLeft());
                require(native.contains(frame),
                        "native rounding must not clip any side of the moving border");
            }
        }
    }
    require(!recording::screenRecordingFollowFrameGeometry({}, 1.25).windowGeometry.isValid(),
            "an invalid viewport must not produce a follow window");
}

void followGuideFitsThePaintedBorderInsideTheDisplay() {
    using recording::ScreenRecordingFollowGuideMotion;
    const QRectF bounds(-1200, -200, 600, 400);
    const auto normal =
        ScreenRecordingFollowGuideMotion({320, 240}, bounds, {-900, 0}, false, false).viewport();
    require(normal == QRectF(-1060, -120, 320, 240),
            "the guide centers on the cursor in native desktop coordinates");

    ScreenRecordingFollowGuideMotion motion({320, 240}, bounds, {-1200, -200}, false, false);
    const auto topLeft = motion.viewport();
    motion.track({50, 500});
    const auto bottomRight = motion.viewport();
    require(topLeft == QRectF(-1197, -197, 320, 240) && bottomRight == QRectF(-923, -43, 320, 240),
            "the guide clamps both axes for cursors at or beyond the display edges");

    const auto oversized =
        ScreenRecordingFollowGuideMotion({1600, 900}, bounds, {-1200, 500}, false, false)
            .viewport();
    require(qAbs(oversized.width() - 594.0) < 0.0001 &&
                qAbs(oversized.width() / oversized.height() - 1600.0 / 900.0) < 0.0001,
            "an oversized guide shrinks uniformly after reserving space for the border");
    for (const auto& guide : {topLeft, bottomRight, oversized}) {
        for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
            const auto frame = recording::screenRecordingFollowFrameGeometry(
                QRectF(guide.topLeft() / scale, guide.size() / scale), scale);
            const auto desktopFrame = frame.frameRect.translated(frame.windowGeometry.topLeft());
            const QRectF mappedFrame(desktopFrame.topLeft() * scale, desktopFrame.size() * scale);
            require(bounds.adjusted(-0.0001, -0.0001, 0.0001, 0.0001).contains(mappedFrame),
                    "the painted guide stays inside the display at every tested DPI");
        }
    }
}

void followGuideRespectsMotionOptions() {
    using recording::ScreenRecordingFollowGuideMotion;
    using namespace std::chrono_literals;
    const QSizeF size(120, 84);
    const QRectF bounds(-200, -400, 1400, 1800);
    const QPointF center(210, 202);
    const QRectF initial(150, 160, 120, 84);

    ScreenRecordingFollowGuideMotion immediate(size, bounds, center, false, false);
    immediate.track(center + QPointF(1, 1));
    require(immediate.viewport() == initial.translated(1, 1) && !immediate.isMoving(),
            "with both options off the guide follows small movements immediately");

    ScreenRecordingFollowGuideMotion smooth(size, bounds, center, true, false);
    smooth.track(center + QPointF(10, 8));
    require(smooth.viewport() == initial && smooth.isMoving(),
            "smoothing alone accepts small movements and schedules easing");
    smooth.advance(50ms);
    require(smooth.viewport() == initial.translated(5, 4), "the guide eases halfway in 50 ms");

    for (const QPointF& delta :
         {QPointF(50, 0), QPointF(-50, 0), QPointF(0, 35), QPointF(0, -35)}) {
        ScreenRecordingFollowGuideMotion jitter(size, bounds, center, false, true);
        jitter.track(center + delta * 0.99);
        require(jitter.viewport() == initial, "the guide holds inside the central five-sixths");
        jitter.track(center + delta);
        require(jitter.viewport() == initial.translated(delta) && !jitter.isMoving(),
                "entering any outer twelfth recenters the guide with smoothing off");
    }

    ScreenRecordingFollowGuideMotion clamped(size, bounds, {bounds.left(), center.y()}, false,
                                             true);
    const QRectF edge = clamped.viewport();
    clamped.track({bounds.left(), center.y() + 10});
    require(clamped.viewport() == edge, "a clamped edge does not trigger recentering");

    ScreenRecordingFollowGuideMotion combined(size, bounds, center, true, true);
    combined.track(center + QPointF(49, 0));
    require(!combined.isMoving(), "smoothing leaves the dead zone in effect");
    combined.track(center + QPointF(50, 0));
    combined.advance(50ms);
    require(combined.viewport() == initial.translated(25, 0), "crossing the margin starts easing");
    combined.track(center + QPointF(84, 0));
    combined.advance(100ms);
    require(combined.viewport() == initial.translated(43.75, 0),
            "the jitter threshold follows the destination while the guide eases");
    combined.track(center + QPointF(100, 0));
    combined.advance(50ms);
    require(combined.viewport() == initial.translated(71.875, 0),
            "crossing the destination margin retargets the guide during easing");
    combined.advance(1s);
    require(combined.viewport() == initial.translated(100, 0) && !combined.isMoving(),
            "easing settles without any further pointer movement");
}

void smoothFollowGuideCatchesLargeJumpsImmediately() {
    using recording::ScreenRecordingFollowGuideMotion;
    using namespace std::chrono_literals;
    const QRectF bounds(-400, -300, 1400, 1000);
    for (bool reduceJitter : {false, true}) {
        ScreenRecordingFollowGuideMotion guide({100, 80}, bounds, {200, 200}, true, reduceJitter);
        guide.track({750, -200});
        require(guide.viewport() == QRectF(651, -200, 100, 80),
                "the guide catches a diagonal jump before the first animation tick");
        guide.track({760, -200});
        require(guide.viewport() == QRectF(661, -200, 100, 80),
                "the guide catches a jump inside the destination's dead zone");
        guide.advance(50ms);
        require(guide.viewport() == QRectF(reduceJitter ? 680.5 : 685.5, -220, 100, 80),
                "the guide still eases toward the cursor after catching up");

        guide.track({-400, 699});
        require(guide.viewport() == QRectF(-397, 617, 100, 80) && !guide.isMoving(),
                "the guide catches a reversal and keeps its border inside the display");
    }
}

void physicalSelectionMapsToItsLogicalSubregion() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "geometry test requires a primary screen");

    const QRect logicalBounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect physicalSelection(physicalBounds.left() + physicalBounds.width() / 4,
                                  physicalBounds.top() + physicalBounds.height() / 5,
                                  physicalBounds.width() / 2, physicalBounds.height() / 3);
    const QRectF mappedF =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physicalSelection, screen);
    const QRect mapped =
        ScreenshotGeometryMapper::logicalRectForPhysicalRect(physicalSelection, screen);
    const double scaleX =
        static_cast<double>(logicalBounds.width()) / static_cast<double>(physicalBounds.width());
    const double scaleY =
        static_cast<double>(logicalBounds.height()) / static_cast<double>(physicalBounds.height());
    const int expectedLeft =
        logicalBounds.left() +
        qFloor(static_cast<double>(physicalSelection.left() - physicalBounds.left()) * scaleX);
    const int expectedTop =
        logicalBounds.top() +
        qFloor(static_cast<double>(physicalSelection.top() - physicalBounds.top()) * scaleY);
    const int expectedRight =
        logicalBounds.left() +
        qCeil(static_cast<double>(physicalSelection.left() + physicalSelection.width() -
                                  physicalBounds.left()) *
              scaleX);
    const int expectedBottom =
        logicalBounds.top() +
        qCeil(static_cast<double>(physicalSelection.top() + physicalSelection.height() -
                                  physicalBounds.top()) *
              scaleY);
    const QRect expectedAligned(expectedLeft, expectedTop, expectedRight - expectedLeft,
                                expectedBottom - expectedTop);
    require(mapped == expectedAligned,
            "physical screenshot selection should map to the matching logical subregion");
    const QRectF expectedF(
        QPointF(logicalBounds.left() +
                    static_cast<double>(physicalSelection.left() - physicalBounds.left()) * scaleX,
                logicalBounds.top() +
                    static_cast<double>(physicalSelection.top() - physicalBounds.top()) * scaleY),
        QPointF(logicalBounds.left() +
                    static_cast<double>(physicalSelection.left() + physicalSelection.width() -
                                        physicalBounds.left()) *
                        scaleX,
                logicalBounds.top() +
                    static_cast<double>(physicalSelection.top() + physicalSelection.height() -
                                        physicalBounds.top()) *
                        scaleY));
    require(qAbs(mappedF.left() - expectedF.left()) < 0.0001 &&
                qAbs(mappedF.top() - expectedF.top()) < 0.0001 &&
                qAbs(mappedF.right() - expectedF.right()) < 0.0001 &&
                qAbs(mappedF.bottom() - expectedF.bottom()) < 0.0001,
            "physical selection mapping should preserve fractional high-DPI edges");
    require(mapped != logicalBounds,
            "partial physical selection must not expand to the entire screen");
}

void toolbarUsesBottomRightThenTopRight() {
    const QRect bounds(0, 0, 1920, 1080);
    const QRect toolbarRect(12, 8, 520, 48);
    const ScreenshotToolbarPlacementGeometry toolbarPlacement{toolbarRect, {}, toolbarRect};
    const QRect upperRegion(500, 200, 700, 400);
    const ScreenshotAnchoredToolbarPlacement bottomRightPlacement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(upperRegion.left() + upperRegion.width(),
                   upperRegion.top() + upperRegion.height()),
            QPoint(upperRegion.left() + upperRegion.width(), upperRegion.top()), toolbarPlacement,
            toolbarPlacement, bounds, 4);
    const QRect bottomRightToolbar = toolbarRect.translated(bottomRightPlacement.contentPosition);
    require(!bottomRightPlacement.usesTopRightPlacement,
            "toolbar should prefer the recording region bottom-right corner");
    require(bottomRightToolbar.right() == upperRegion.left() + upperRegion.width(),
            "bottom-right placement should use the recording region right anchor");
    require(bottomRightToolbar.top() == upperRegion.top() + upperRegion.height() + 5,
            "toolbar should keep the requested gap below the recording region");

    const QRect lowerRegion(500, 930, 700, 120);
    const ScreenshotAnchoredToolbarPlacement topRightPlacement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(lowerRegion.left() + lowerRegion.width(),
                   lowerRegion.top() + lowerRegion.height()),
            QPoint(lowerRegion.left() + lowerRegion.width(), lowerRegion.top()), toolbarPlacement,
            toolbarPlacement, bounds, 4);
    const QRect topRightToolbar = toolbarRect.translated(topRightPlacement.contentPosition);
    require(topRightPlacement.usesTopRightPlacement,
            "toolbar should use top-right when bottom-right lacks vertical space");
    require(topRightToolbar.right() == lowerRegion.left() + lowerRegion.width(),
            "top-right placement should keep the recording region right anchor");
    require(topRightToolbar.bottom() < lowerRegion.top(),
            "top-right placement should remain above the recording region");
}

void toolbarRemainsInsideTheAvailableScreen() {
    const QRect bounds(0, 0, 800, 600);
    const QRect toolbarRect(10, 6, 1000, 48);
    const ScreenshotToolbarPlacementGeometry toolbarPlacement{toolbarRect, {}, toolbarRect};
    const QRect region(5, 570, 100, 20);
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(region.left() + region.width(), region.top() + region.height()),
            QPoint(region.left() + region.width(), region.top()), toolbarPlacement,
            toolbarPlacement, bounds, 4);
    const QRect toolbar = toolbarRect.translated(placement.contentPosition);

    require(toolbar.left() == bounds.left() && toolbar.top() >= bounds.top(),
            "oversized toolbar placement should clamp to the screen origin");
    require(toolbar.bottom() <= bounds.bottom(),
            "toolbar placement should clamp vertically to the screen");
}

void toolbarChoosesItsVerticalAnchorAfterHorizontalClamping() {
    // The secondary row extends left of the main row. Placement must constrain
    // all visible content, including on displays with nonzero desktop origins.
    const ScreenshotToolbarPlacementGeometry bottom{QRect(120, 0, 620, 40), QRect(-40, 44, 780, 44),
                                                    QRect(-40, 0, 780, 88)};
    const ScreenshotToolbarPlacementGeometry top{QRect(120, 48, 620, 40), QRect(-40, 0, 780, 44),
                                                 QRect(-40, 0, 780, 88)};
    for (const QPoint origin : {QPoint(0, 0), QPoint(1600, 120), QPoint(-1600, -120)}) {
        const QRect bounds(origin, QSize(1600, 900));
        for (const int right : {200, 1800}) {
            // 807 fits exactly below; one point lower must choose the top.
            for (const int lowerEdge : {320, 807, 808, 880}) {
                const QPoint bottomAnchor = origin + QPoint(right, lowerEdge);
                const QPoint topAnchor = bottomAnchor - QPoint(0, 120);
                const auto placement = ScreenshotGeometryMapper::anchoredToolbarPlacement(
                    bottomAnchor, topAnchor, bottom, top, bounds, 4);
                const bool above = lowerEdge > 807;
                require(placement.usesTopRightPlacement == above,
                        "horizontal overflow must not reject a vertically fitting toolbar anchor");
                const auto& layout = above ? top : bottom;
                const QRect occupied =
                    layout.occupiedContentRect.translated(placement.contentPosition);
                const QRect main =
                    layout.mainToolbarContentRect.translated(placement.contentPosition);
                require(bounds.contains(occupied),
                        "horizontal adjustment must keep every toolbar row on its display");
                require(right == 200 ? occupied.left() == bounds.left()
                                     : occupied.right() == bounds.right(),
                        "horizontal clamping must use the occupied extent and desktop origin");
                require(above ? main.bottom() == topAnchor.y() - 5
                              : main.top() == bottomAnchor.y() + 5,
                        "horizontal adjustment must preserve the vertical anchor and gap");
            }
        }
    }
}

void toolbarAnchorsTheMainRowAndUsesTheVisibleSecondaryRow() {
    const QRect bounds(0, 0, 1000, 800);
    const ScreenshotToolbarPlacementGeometry bottom{
        QRect(100, 0, 200, 40),
        QRect(180, 44, 120, 20),
        QRect(100, 0, 200, 64),
    };
    const ScreenshotToolbarPlacementGeometry top{
        QRect(100, 60, 200, 40),
        QRect(180, 16, 120, 40),
        QRect(100, 16, 200, 84),
    };
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(QPoint(500, 200), QPoint(500, 500),
                                                           bottom, top, bounds, 4);
    require(!placement.usesTopRightPlacement,
            "visible bottom content should remain at the bottom-right anchor");

    const QRect main = bottom.mainToolbarContentRect.translated(placement.contentPosition);
    const QRect secondary =
        bottom.secondaryToolbarContentRect.translated(placement.contentPosition);
    require(main.right() == 500 && main.top() == 205,
            "bottom placement should anchor the main row's top-right corner");
    require(secondary.right() == 500 && secondary.top() == 249 &&
                secondary.top() - main.bottom() - 1 == 4,
            "bottom placement should right-align and space the active secondary row");

    const ScreenshotToolbarPlacementGeometry crampedBottom{
        bottom.mainToolbarContentRect,
        bottom.secondaryToolbarContentRect,
        QRect(100, 0, 200, 700),
    };
    const ScreenshotAnchoredToolbarPlacement topPlacement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(QPoint(500, 750), QPoint(500, 500),
                                                           crampedBottom, top, bounds, 4);
    require(topPlacement.usesTopRightPlacement,
            "placement should fall back to the top-right anchor when bottom content does not fit");
    const QRect topMain = top.mainToolbarContentRect.translated(topPlacement.contentPosition);
    const QRect topSecondary =
        top.secondaryToolbarContentRect.translated(topPlacement.contentPosition);
    require(topMain.right() == 500 && topMain.bottom() == 495,
            "top placement should anchor the main row's bottom-right corner");
    require(topSecondary.right() == 500 && topMain.top() - topSecondary.bottom() - 1 == 4,
            "top placement should right-align and space the active secondary row");
}

void encoderCompatibilityNeverShrinksTheSelection() {
    const QRect bounds(0, 0, 1920, 1080);
    const QRect oddSelection(100, 80, 641, 359);
    const QRect expanded = recording::screenRecordingCompatibleCaptureRegion(oddSelection, bounds);
    require(expanded.contains(oddSelection),
            "encoder-compatible capture region should retain every selected pixel");
    require(expanded.width() == 642 && expanded.height() == 360,
            "odd capture dimensions should expand to the next even dimensions");
    require(expanded.topLeft() == oddSelection.topLeft(),
            "capture expansion should prefer the right and bottom edges");

    const QRect edgeSelection(1279, 721, 641, 359);
    const QRect edgeExpanded =
        recording::screenRecordingCompatibleCaptureRegion(edgeSelection, bounds);
    require(edgeExpanded.contains(edgeSelection),
            "screen-edge capture expansion should retain every selected pixel");
    require(edgeExpanded.right() == bounds.right() && edgeExpanded.bottom() == bounds.bottom(),
            "screen-edge capture expansion should stay within physical bounds");
    require(edgeExpanded.width() == 642 && edgeExpanded.height() == 360,
            "screen-edge capture dimensions should remain encoder compatible");
}

void recordingBorderStaysOutsideTheEncoderPaddingForSelection(const QRect& selection,
                                                              const QRect& bounds) {
    const QRect captureRegion =
        recording::screenRecordingCompatibleCaptureRegion(selection, bounds);
    const recording::ScreenRecordingAreaFrameGeometry geometry =
        recording::screenRecordingAreaFrameGeometry(QRectF(selection), 1.0);
    const QRectF displayedFrame = geometry.frameRect.translated(geometry.windowGeometry.topLeft());
    const qreal minimumClearance = geometry.borderWidth - 0.0001;
    const qreal captureRight = captureRegion.left() + captureRegion.width();
    const qreal captureBottom = captureRegion.top() + captureRegion.height();
    const qreal frameRight = displayedFrame.left() + displayedFrame.width();
    const qreal frameBottom = displayedFrame.top() + displayedFrame.height();

    require(captureRegion.left() - displayedFrame.left() >= minimumClearance,
            "left recording border should remain outside the encoder padding");
    require(captureRegion.top() - displayedFrame.top() >= minimumClearance,
            "top recording border should remain outside the encoder padding");
    require(frameRight - captureRight >= minimumClearance,
            "right recording border should remain outside the encoder padding");
    require(frameBottom - captureBottom >= minimumClearance,
            "bottom recording border should remain outside the encoder padding");
}

void recordingBorderStaysOutsideTheEncoderPadding() {
    const QRect bounds(0, 0, 1920, 1080);
    recordingBorderStaysOutsideTheEncoderPaddingForSelection(QRect(100, 80, 641, 359), bounds);
    recordingBorderStaysOutsideTheEncoderPaddingForSelection(QRect(1279, 721, 641, 359), bounds);
}

void recordingBorderSidesMeetAtEveryPaddedCorner() {
    const auto geometry =
        recording::screenRecordingAreaFrameGeometry(QRectF(100, 80, 641, 359), 1.0);
    const auto border = recording::screenRecordingAreaBorderGeometry(
        geometry.frameRect, geometry.selectionRect, geometry.paddingWidth);

    require(qAbs(border.left.top() - geometry.frameRect.top()) < 0.0001 &&
                qAbs(border.left.bottom() - geometry.frameRect.bottom()) < 0.0001 &&
                qAbs(border.right.top() - geometry.frameRect.top()) < 0.0001 &&
                qAbs(border.right.bottom() - geometry.frameRect.bottom()) < 0.0001,
            "vertical recording border sides should span the complete frame height");
    require(border.top.intersects(border.left),
            "top and left recording border sides should meet across the padded corner");
    require(border.top.intersects(border.right),
            "top and right recording border sides should meet across the padded corner");
    require(border.bottom.intersects(border.left),
            "bottom and left recording border sides should meet across the padded corner");
    require(border.bottom.intersects(border.right),
            "bottom and right recording border sides should meet across the padded corner");
}

void claritySettingsMapToMaximumOutputSizes() {
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("4k")) ==
                QSize(3840, 2160),
            "4K clarity should cap output at 3840x2160");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("2k")) ==
                QSize(2560, 1440),
            "2K clarity should cap output at 2560x1440");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("1080p")) ==
                QSize(1920, 1080),
            "1080p clarity should cap output at 1920x1080");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("720p")) ==
                QSize(1280, 720),
            "720p clarity should cap output at 1280x720");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("480p")) ==
                QSize(854, 480),
            "480p clarity should use an encoder-compatible 16:9 size");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("invalid")) ==
                QSize(1920, 1080),
            "invalid clarity should use the persisted setting's 1080p default");
}

void clarityBoundsFollowCaptureOrientation() {
    require(recording::screenRecordingOrientedMaximumSize(QSize(1920, 1080), QSize(1080, 1920)) ==
                QSize(1080, 1920),
            "portrait captures should transpose landscape clarity bounds");
    require(recording::screenRecordingOrientedMaximumSize(QSize(1920, 1080), QSize(1920, 1080)) ==
                QSize(1920, 1080),
            "landscape captures should retain landscape clarity bounds");
    require(recording::screenRecordingOrientedMaximumSize(QSize(1080, 1920), QSize(1920, 1080)) ==
                QSize(1920, 1080),
            "landscape captures should transpose portrait bounds");
    require(recording::screenRecordingOrientedMaximumSize(QSize(1920, 1080), QSize(1000, 1000)) ==
                QSize(1920, 1080),
            "square captures should retain the configured orientation");
}
void minimumRegionsAndCrossingEdges() {
    const QRect bounds(-100, -50, 200, 150);
    for (const QSize size :
         {QSize(1, 1), QSize(1, 100), QSize(9, 9), QSize(10, 10), QSize(40, 60)}) {
        const QRect input(-30, -20, size.width(), size.height());
        const QRect output = recording::screenRecordingNormalizedRegion(input, bounds);
        require(output == QRect(input.topLeft(), size.expandedTo(QSize(10, 10))),
                "small regions expand from the top-left without shrinking either axis");
    }
    require(recording::screenRecordingNormalizedRegion(QRect(99, 99, 1, 1), bounds) ==
                QRect(90, 90, 10, 10),
            "expansion must shift inward at display boundaries");
    require(recording::screenRecordingNormalizedRegion(QRect(), bounds).isEmpty(),
            "empty selections must stay invalid");
    require(recording::screenRecordingMinimumExtent(2.0) == 5 &&
                recording::screenRecordingMinimumExtent(1.5) == 7,
            "physical minimum must round up in logical coordinates");
    const QRect origin(-50, -20, 100, 80);
    namespace geometry = snow_shot::presentation::resize_geometry;
    const Qt::Edges handles[] = {Qt::LeftEdge,
                                 Qt::RightEdge,
                                 Qt::TopEdge,
                                 Qt::BottomEdge,
                                 Qt::LeftEdge | Qt::TopEdge,
                                 Qt::RightEdge | Qt::TopEdge,
                                 Qt::LeftEdge | Qt::BottomEdge,
                                 Qt::RightEdge | Qt::BottomEdge};
    for (const auto pressed : handles) {
        Qt::Edges previous = pressed;
        for (const int distance : {20, 1, 0, -1, -20, 0, 20}) {
            const QPoint delta(pressed.testFlag(Qt::LeftEdge)    ? 100 - distance
                               : pressed.testFlag(Qt::RightEdge) ? distance - 100
                                                                 : 0,
                               pressed.testFlag(Qt::TopEdge)      ? 80 - distance
                               : pressed.testFlag(Qt::BottomEdge) ? distance - 80
                                                                  : 0);
            const auto drag = geometry::dragGeometry(origin, pressed, delta, previous);
            const QRect resized = geometry::anchoredRect(
                origin, pressed, drag.edges, drag.requestedSize.expandedTo(QSize(10, 10)));
            require(resized.width() >= 10 && resized.height() >= 10,
                    "crossing must always retain the recording minimum");
            if (distance == 0)
                require(drag.edges == previous, "equality retains the last side");
            if (pressed.testFlag(Qt::LeftEdge) || pressed.testFlag(Qt::RightEdge)) {
                const int anchor = pressed.testFlag(Qt::LeftEdge) ? 50 : -50;
                require((drag.edges.testFlag(Qt::LeftEdge) ? resized.x() + resized.width()
                                                           : resized.x()) == anchor,
                        "horizontal crossing retains the opposite outer boundary");
            }
            if (pressed.testFlag(Qt::TopEdge) || pressed.testFlag(Qt::BottomEdge)) {
                const int anchor = pressed.testFlag(Qt::TopEdge) ? 60 : -20;
                require((drag.edges.testFlag(Qt::TopEdge) ? resized.y() + resized.height()
                                                          : resized.y()) == anchor,
                        "vertical crossing retains the opposite outer boundary");
            }
            previous = drag.edges;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    minimumRegionsAndCrossingEdges();
    followFramePansWithoutResizingOrClipping();
    followGuideFitsThePaintedBorderInsideTheDisplay();
    followGuideRespectsMotionOptions();
    smoothFollowGuideCatchesLargeJumpsImmediately();
    observedNativeGeometryFollowsDisplaysWithoutRescalingPhysicalCoordinates();
    physicalSelectionMapsToItsLogicalSubregion();
    recordingFrameStaysOutsideTheSelection(1.0);
    recordingFrameStaysOutsideTheSelection(1.5);
    recordingFrameStaysOutsideTheSelection(2.0);
    toolbarUsesBottomRightThenTopRight();
    toolbarRemainsInsideTheAvailableScreen();
    toolbarChoosesItsVerticalAnchorAfterHorizontalClamping();
    toolbarAnchorsTheMainRowAndUsesTheVisibleSecondaryRow();
    encoderCompatibilityNeverShrinksTheSelection();
    recordingBorderStaysOutsideTheEncoderPadding();
    recordingBorderSidesMeetAtEveryPaddedCorner();
    claritySettingsMapToMaximumOutputSizes();
    clarityBoundsFollowCaptureOrientation();
    return 0;
}
