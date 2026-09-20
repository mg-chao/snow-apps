#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "../src/presentation/toolbar/screenshottoolbarplacement.h"
#include "../src/presentation/capture/scrollingselectionmovement.h"

#include <QRectF>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {
constexpr qreal kMinimumSelectionSize = 10.0;
constexpr qreal kExpectedAspectRatio = 0.5;
constexpr qreal kComparisonTolerance = 0.0001;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void requireAspectRatio(const QRectF& selection, const char* message) {
    require(selection.width() >= kMinimumSelectionSize &&
                selection.height() >= kMinimumSelectionSize,
            "locked resize should respect the minimum selection size");
    require(std::abs(selection.height() / selection.width() - kExpectedAspectRatio) <
                kComparisonTolerance,
            message);
}

QRectF lockedDragResult(ScreenshotSelectionDragMode dragMode, const QPointF& originPosition,
                        const QPointF& position,
                        const QRectF& bounds = QRectF(0.0, 0.0, 800.0, 600.0)) {
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(100.0, 100.0, 200.0, 100.0));
    selection.toggleAspectRatioLock(kMinimumSelectionSize);
    require(selection.aspectRatioLocked(), "selection should enable its aspect lock");
    selection.beginMoveDrag(originPosition);
    return selection.selectionRectForDrag(dragMode, position, bounds, kMinimumSelectionSize);
}

void lockedAspectRatioAppliesToEveryResizeHandle() {
    struct DragCase {
        ScreenshotSelectionDragMode dragMode;
        QPointF originPosition;
        QPointF position;
    };
    const DragCase cases[] = {
        {ScreenshotSelectionDragMode::TopLeft, QPointF(100.0, 100.0), QPointF(40.0, 60.0)},
        {ScreenshotSelectionDragMode::Top, QPointF(200.0, 100.0), QPointF(200.0, 50.0)},
        {ScreenshotSelectionDragMode::TopRight, QPointF(300.0, 100.0), QPointF(360.0, 60.0)},
        {ScreenshotSelectionDragMode::Right, QPointF(300.0, 150.0), QPointF(350.0, 150.0)},
        {ScreenshotSelectionDragMode::BottomRight, QPointF(300.0, 200.0), QPointF(360.0, 260.0)},
        {ScreenshotSelectionDragMode::Bottom, QPointF(200.0, 200.0), QPointF(200.0, 250.0)},
        {ScreenshotSelectionDragMode::BottomLeft, QPointF(100.0, 200.0), QPointF(40.0, 240.0)},
        {ScreenshotSelectionDragMode::Left, QPointF(100.0, 150.0), QPointF(50.0, 150.0)},
    };

    for (const DragCase& drag : cases) {
        requireAspectRatio(lockedDragResult(drag.dragMode, drag.originPosition, drag.position),
                           "locked resize should retain the original aspect ratio");
    }
}

void lockedResizeStaysInsideBoundsWithoutDistorting() {
    const QRectF bounds(0.0, 0.0, 500.0, 500.0);
    const QRectF selection = lockedDragResult(ScreenshotSelectionDragMode::BottomRight,
                                              QPointF(300.0, 200.0), QPointF(450.0, 400.0), bounds);

    requireAspectRatio(selection, "bounds should not distort a locked aspect ratio during resize");
    require(selection.left() == 100.0 && selection.top() == 100.0,
            "corner resize should retain the opposite corner as its anchor");
    require(selection.right() <= bounds.right() && selection.bottom() <= bounds.bottom(),
            "locked resize should remain inside the canvas bounds");
    require(std::abs(selection.width() - 400.0) < kComparisonTolerance &&
                std::abs(selection.height() - 200.0) < kComparisonTolerance,
            "locked resize should use the largest proportionate size within bounds");
}

void lockedResizeAllowsFlippingAcrossOppositeEdges() {
    const QRectF horizontallyFlipped = lockedDragResult(
        ScreenshotSelectionDragMode::Right, QPointF(300.0, 150.0), QPointF(50.0, 150.0));
    requireAspectRatio(horizontallyFlipped,
                       "horizontal flip should retain the locked aspect ratio");
    require(std::abs(horizontallyFlipped.left() - 50.0) < kComparisonTolerance &&
                std::abs(horizontallyFlipped.right() - 100.0) < kComparisonTolerance,
            "crossing the left edge should flip a right-edge resize around its anchor");

    const QRectF verticallyFlipped = lockedDragResult(ScreenshotSelectionDragMode::Bottom,
                                                      QPointF(200.0, 200.0), QPointF(200.0, 50.0));
    requireAspectRatio(verticallyFlipped, "vertical flip should retain the locked aspect ratio");
    require(std::abs(verticallyFlipped.top() - 50.0) < kComparisonTolerance &&
                std::abs(verticallyFlipped.bottom() - 100.0) < kComparisonTolerance,
            "crossing the top edge should flip a bottom-edge resize around its anchor");
}

void lockedCornerResizeCanFlipBothAxes() {
    const QRectF flipped = lockedDragResult(ScreenshotSelectionDragMode::BottomRight,
                                            QPointF(300.0, 200.0), QPointF(50.0, 50.0));

    requireAspectRatio(flipped, "corner flip should retain the locked aspect ratio");
    require(std::abs(flipped.left()) < kComparisonTolerance &&
                std::abs(flipped.top() - 50.0) < kComparisonTolerance &&
                std::abs(flipped.right() - 100.0) < kComparisonTolerance &&
                std::abs(flipped.bottom() - 100.0) < kComparisonTolerance,
            "crossing both opposite edges should flip a corner resize on both axes");
}

void unlockedResizeCanChangeAspectRatio() {
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(100.0, 100.0, 200.0, 100.0));
    selection.beginMoveDrag(QPointF(300.0, 150.0));

    const QRectF resized =
        selection.selectionRectForDrag(ScreenshotSelectionDragMode::Right, QPointF(350.0, 150.0),
                                       QRectF(0.0, 0.0, 800.0, 600.0), kMinimumSelectionSize);
    require(resized.width() == 250.0 && resized.height() == 100.0,
            "unlocked resize should continue to change dimensions independently");
}

void persistedAspectRatioLockConstrainsNewMarquee() {
    ScreenshotSelectionModel selection;
    require(selection.setAspectRatioLockEnabled(true, kMinimumSelectionSize) &&
                selection.aspectRatioLocked(),
            "the aspect-ratio lock should be enabled before a selection exists");
    selection.setSelectionStartEnd(QPointF(40.0, 50.0), QPointF(40.0, 50.0));
    selection.beginMoveDrag(QPointF(40.0, 50.0));

    const QRectF marquee =
        selection.selectionRectForDrag(ScreenshotSelectionDragMode::Marquee, QPointF(160.0, 90.0),
                                       QRectF(0.0, 0.0, 800.0, 600.0), kMinimumSelectionSize);
    require(std::abs(marquee.width() - marquee.height()) < kComparisonTolerance,
            "a persisted aspect-ratio lock should constrain a new marquee to a square");

    selection.setSelectionRect(marquee);
    require(selection.setAspectRatioLockEnabled(true, kMinimumSelectionSize),
            "confirming the marquee should derive its concrete aspect ratio");
    selection.clearSelection();
    require(selection.aspectRatioLocked(),
            "clearing a selection should preserve the enabled aspect-ratio preference");
}

void grabAdjustmentSnapsOnlyTheDraggedEdgesToThePressPosition() {
    const QRectF selection(100.0, 100.0, 200.0, 100.0);
    const QRectF bounds(0.0, 0.0, 800.0, 600.0);
    struct GrabCase {
        ScreenshotSelectionDragMode dragMode;
        QPointF position;
        QRectF expected;
    };
    const GrabCase cases[] = {
        {ScreenshotSelectionDragMode::TopLeft, QPointF(97.0, 104.0),
         QRectF(97.0, 104.0, 203.0, 96.0)},
        {ScreenshotSelectionDragMode::Top, QPointF(200.0, 96.0), QRectF(100.0, 96.0, 200.0, 104.0)},
        {ScreenshotSelectionDragMode::TopRight, QPointF(303.0, 104.0),
         QRectF(100.0, 104.0, 204.0, 96.0)},
        {ScreenshotSelectionDragMode::Right, QPointF(304.0, 150.0),
         QRectF(100.0, 100.0, 205.0, 100.0)},
        {ScreenshotSelectionDragMode::BottomRight, QPointF(303.0, 196.0),
         QRectF(100.0, 100.0, 204.0, 97.0)},
        {ScreenshotSelectionDragMode::Bottom, QPointF(200.0, 204.0),
         QRectF(100.0, 100.0, 200.0, 105.0)},
        {ScreenshotSelectionDragMode::BottomLeft, QPointF(97.0, 196.0),
         QRectF(97.0, 100.0, 203.0, 97.0)},
        {ScreenshotSelectionDragMode::Left, QPointF(96.0, 150.0),
         QRectF(96.0, 100.0, 204.0, 100.0)},
    };

    for (const GrabCase& grab : cases) {
        require(grabAdjustedScreenshotSelectionRect(grab.dragMode, selection, grab.position, bounds,
                                                    kMinimumSelectionSize) == grab.expected,
                "the grab adjustment must move only the pressed edges onto the pointer cell");
    }
    for (const ScreenshotSelectionDragMode unchanged :
         {ScreenshotSelectionDragMode::All, ScreenshotSelectionDragMode::Marquee,
          ScreenshotSelectionDragMode::None}) {
        require(grabAdjustedScreenshotSelectionRect(unchanged, selection, QPointF(304.0, 150.0),
                                                    bounds, kMinimumSelectionSize) == selection,
                "whole-selection and marquee drags must not adjust the selection at press");
    }
}

void grabAdjustmentRespectsBoundsAndMinimumSize() {
    const QRectF selection(10.0, 10.0, 20.0, 20.0);
    require(grabAdjustedScreenshotSelectionRect(ScreenshotSelectionDragMode::Right, selection,
                                                QPointF(35.0, 20.0), QRectF(0.0, 0.0, 32.0, 32.0),
                                                kMinimumSelectionSize) ==
                QRectF(10.0, 10.0, 22.0, 20.0),
            "the grab adjustment must clamp the snapped border to the canvas bounds");
    require(grabAdjustedScreenshotSelectionRect(ScreenshotSelectionDragMode::Bottom, selection,
                                                QPointF(20.0, 12.0), QRectF(0.0, 0.0, 800.0, 600.0),
                                                kMinimumSelectionSize)
                    .height() == kMinimumSelectionSize,
            "the grab adjustment must keep the selection at its minimum size");
}

void positionFollowDragTracksThePointerAfterGrabAdjustment() {
    const QRectF bounds(0.0, 0.0, 800.0, 600.0);
    const QPointF press(304.0, 150.0); // 4 px right of the border, inside the 8 px hit tolerance

    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(100.0, 100.0, 200.0, 100.0));
    const QRectF adjusted = grabAdjustedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Right, selection.normalizedSelection(), press, bounds,
        kMinimumSelectionSize);
    require(adjusted == QRectF(100.0, 100.0, 205.0, 100.0),
            "the grab adjustment must move the pressed border onto the pointer cell at press");
    selection.setSelectionRect(adjusted);
    selection.beginMoveDrag(press);
    require(selection.selectionRectForDrag(ScreenshotSelectionDragMode::Right, press, bounds,
                                           kMinimumSelectionSize) == adjusted,
            "pressing must not move the selection beyond the grab adjustment");
    const QRectF dragged = selection.selectionRectForDrag(
        ScreenshotSelectionDragMode::Right, QPointF(340.0, 150.0), bounds, kMinimumSelectionSize);
    require(dragged == QRectF(100.0, 100.0, 241.0, 100.0),
            "the dragged border must sit on the pointer cell while the opposite border stays "
            "anchored");

    ScreenshotSelectionModel locked;
    locked.setSelectionRect(QRectF(100.0, 100.0, 200.0, 100.0));
    locked.toggleAspectRatioLock(kMinimumSelectionSize);
    locked.setSelectionRect(adjusted);
    locked.beginMoveDrag(press);
    const QRectF lockedDragged = locked.selectionRectForDrag(
        ScreenshotSelectionDragMode::Right, QPointF(340.0, 150.0), bounds, kMinimumSelectionSize);
    require(lockedDragged.left() == 100.0 &&
                std::abs(lockedDragged.right() - 341.0) < kComparisonTolerance,
            "locked position-follow resize must anchor the opposite border on the pointer cell");
    requireAspectRatio(lockedDragged,
                       "locked position-follow resize should retain the original aspect ratio");
}

void movementFollowDragKeepsThePressTimeGrabOffset() {
    const QRectF bounds(0.0, 0.0, 800.0, 600.0);
    const QPointF press(304.0, 150.0);

    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(100.0, 100.0, 200.0, 100.0));
    selection.beginMoveDrag(press);
    require(selection.selectionRectForDrag(ScreenshotSelectionDragMode::Right, press, bounds,
                                           kMinimumSelectionSize) ==
                QRectF(100.0, 100.0, 200.0, 100.0),
            "movement-follow drags must not adjust the selection at press");
    require(selection.selectionRectForDrag(ScreenshotSelectionDragMode::Right,
                                           QPointF(340.0, 150.0), bounds, kMinimumSelectionSize) ==
                QRectF(100.0, 100.0, 236.0, 100.0),
            "movement-follow drags must keep the press-time grab offset on the dragged border");
}

void selectionShadowDefaultsToRequestedColor() {
    const QColor expected(0x33, 0x33, 0x33);

    ScreenshotSelectionModel selection;
    require(selection.shadowColor() == expected,
            "new selections should default to #333333 shadow color");

    ScreenshotSelectionParams params;
    require(params.shadowColor == expected,
            "selection params should default to #333333 shadow color");

    selection.setShadowColor(QColor());
    require(selection.shadowColor() == expected,
            "invalid selection shadow colors should fall back to #333333");
    selection.reset();
    require(selection.shadowColor() == expected,
            "reset selections should restore the #333333 shadow color");
}

void marqueeDragUsesTheSharedGeometryTransactionWithoutMinimumInflation() {
    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd(QPointF(40.0, 50.0), QPointF(40.0, 50.0));
    selection.beginMoveDrag(QPointF(40.0, 50.0));

    const QRectF marquee =
        selection.selectionRectForDrag(ScreenshotSelectionDragMode::Marquee, QPointF(43.0, 54.0),
                                       QRectF(0.0, 0.0, 100.0, 100.0), kMinimumSelectionSize);
    require(marquee == QRectF(40.0, 50.0, 4.0, 5.0),
            "marquee drags must span the pointer cells inclusively without resize minimums");
    require(screenshotPixelRectForSelection(marquee) == QRect(40, 50, 4, 5),
            "the inclusive marquee span must convert to its exact pixel rectangle");
    require(selection
                .selectionRectForDrag(ScreenshotSelectionDragMode::Marquee, QPointF(40.0, 50.0),
                                      QRectF(0.0, 0.0, 100.0, 100.0), kMinimumSelectionSize)
                .isEmpty(),
            "pressing and releasing on the same pointer cell must not create a selection");
    require(!screenshotSelectionDragAnchor(marquee, ScreenshotSelectionDragMode::Marquee,
                                           QPointF(43.0, 54.0), kMinimumSelectionSize)
                 .has_value(),
            "marquee drags must sample the color picker at the pointer, not a resize handle");
    require(screenshotSelectionDragModeForPoint(QRectF(10.0, 10.0, 20.0, 20.0), QPointF(50.0, 50.0),
                                                false, 8.0, kMinimumSelectionSize) ==
                ScreenshotSelectionDragMode::BottomRight,
            "Move must classify a point outside the selection as a directional resize");
}

void marqueeDragSelectsSinglePixelStrips() {
    const QRectF bounds(0.0, 0.0, 200.0, 200.0);

    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd(QPointF(40.0, 50.0), QPointF(40.0, 50.0));
    selection.beginMoveDrag(QPointF(40.0, 50.0));
    selection.setSelectionRect(selection.selectionRectForDrag(
        ScreenshotSelectionDragMode::Marquee, QPointF(43.0, 50.0), bounds, kMinimumSelectionSize));
    require(selection.pixelSelection() == QRect(40, 50, 4, 1) && selection.hasPixelSelection(),
            "a marquee kept on one pointer row must select that single pixel row");

    selection.setSelectionRect(selection.selectionRectForDrag(
        ScreenshotSelectionDragMode::Marquee, QPointF(36.0, 50.0), bounds, kMinimumSelectionSize));
    require(selection.pixelSelection() == QRect(36, 50, 5, 1),
            "a reverse marquee kept on one pointer row must still select that single pixel row");

    selection.setSelectionRect(selection.selectionRectForDrag(
        ScreenshotSelectionDragMode::Marquee, QPointF(40.0, 54.0), bounds, kMinimumSelectionSize));
    require(selection.pixelSelection() == QRect(40, 50, 1, 5),
            "a marquee kept on one pointer column must select that single pixel column");

    ScreenshotSelectionModel seeded;
    seeded.setSelectionStartEnd(QPointF(10.4, 70.6), QPointF(60.6, 70.6));
    require(seeded.pixelSelection() == QRect(10, 71, 52, 1),
            "pointer-seeded horizontal strips must select the shared pointer row");

    const QRectF lockedStrip = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(40.0, 50.0), QPointF(70.0, 50.0),
        bounds, kMinimumSelectionSize, 0.01);
    require(lockedStrip == QRectF(40.0, 50.0, 100.0, 1.0),
            "a locked marquee kept on one pointer row must grow until the pressed cell's row "
            "satisfies the ratio");
}

void marqueeDragCanMaintainAnAspectRatio() {
    const QRectF bounds(0.0, 0.0, 200.0, 200.0);
    const QRectF square = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(40.0, 50.0), QPointF(70.0, 100.0),
        bounds, kMinimumSelectionSize, 1.0);
    require(square == QRectF(40.0, 50.0, 51.0, 51.0),
            "locked marquee drags should expand the shorter pointer span proportionally");

    const QRectF flipped = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(100.0, 100.0), QPointF(40.0, 50.0),
        bounds, kMinimumSelectionSize, 0.5);
    require(std::abs(flipped.width() - 101.0) < kComparisonTolerance &&
                std::abs(flipped.height() - 50.5) < kComparisonTolerance && flipped.left() == 0.0 &&
                std::abs(flipped.top() - 50.5) < kComparisonTolerance,
            "locked marquee drags must keep the pressed cell inside when crossing both axes");

    const QRectF clipped = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(180.0, 180.0),
        QPointF(240.0, 230.0), bounds, kMinimumSelectionSize, 1.0);
    require(clipped == QRectF(180.0, 180.0, 20.0, 20.0),
            "locked marquee drags should remain inside the canvas bounds");
}

void marqueeDragReachesTheFullCanvasAtTheExtremePointerPosition() {
    const QRectF bounds(0.0, 0.0, 3840.0, 2160.0);

    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd(QPointF(0.0, 0.0), QPointF(0.0, 0.0));
    selection.beginMoveDrag(QPointF(0.0, 0.0));
    selection.setSelectionRect(selection.selectionRectForDrag(ScreenshotSelectionDragMode::Marquee,
                                                              QPointF(3839.0, 2159.0), bounds,
                                                              kMinimumSelectionSize));
    require(selection.pixelSelection() == QRect(0, 0, 3840, 2160),
            "a marquee dragged onto the last pointer cell must cover the whole canvas");

    ScreenshotSelectionModel reversed;
    reversed.setSelectionStartEnd(QPointF(3839.0, 2159.0), QPointF(3839.0, 2159.0));
    reversed.beginMoveDrag(QPointF(3839.0, 2159.0));
    reversed.setSelectionRect(reversed.selectionRectForDrag(
        ScreenshotSelectionDragMode::Marquee, QPointF(0.0, 0.0), bounds, kMinimumSelectionSize));
    require(reversed.pixelSelection() == QRect(0, 0, 3840, 2160),
            "a marquee dragged from the last pointer cell must cover the whole canvas");
}

void lockedMarqueeDragReachesTheFullCanvasAtTheExtremePointerPosition() {
    const QRectF bounds(0.0, 0.0, 3840.0, 2160.0);
    constexpr qreal kCanvasAspectRatio = 2160.0 / 3840.0;

    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd(QPointF(0.0, 0.0), QPointF(0.0, 0.0));
    selection.beginMoveDrag(QPointF(0.0, 0.0));
    selection.setSelectionRect(selection.selectionRectForDrag(
        ScreenshotSelectionDragMode::Marquee, QPointF(3839.0, 2159.0), bounds,
        kMinimumSelectionSize, kCanvasAspectRatio));
    require(selection.pixelSelection() == QRect(0, 0, 3840, 2160),
            "a locked marquee dragged onto the last pointer cell must cover the whole canvas");

    ScreenshotSelectionModel reversed;
    reversed.setSelectionStartEnd(QPointF(3839.0, 2159.0), QPointF(3839.0, 2159.0));
    reversed.beginMoveDrag(QPointF(3839.0, 2159.0));
    reversed.setSelectionRect(
        reversed.selectionRectForDrag(ScreenshotSelectionDragMode::Marquee, QPointF(0.0, 0.0),
                                      bounds, kMinimumSelectionSize, kCanvasAspectRatio));
    require(reversed.pixelSelection() == QRect(0, 0, 3840, 2160),
            "a locked marquee dragged from the last pointer cell must cover the whole canvas");
}

void lockedMarqueeDragStaysInsideTheCanvasFromExclusiveEdgePointerCells() {
    const QRectF bounds(0.0, 0.0, 3840.0, 2160.0);
    constexpr qreal kCanvasAspectRatio = 2160.0 / 3840.0;

    // Pointer positions at the canvas edge round onto the exclusive boundary
    // cell (3840/2160), where no real pixel exists; the locked marquee must
    // anchor on the last real pixel instead of growing past the canvas.
    const QRectF reversed = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(3839.5, 2159.5), QPointF(0.0, 0.0),
        bounds, kMinimumSelectionSize, kCanvasAspectRatio);
    require(reversed == QRectF(0.0, 0.0, 3840.0, 2160.0),
            "a locked reverse marquee anchored on the exclusive edge cell must stay the whole "
            "canvas");

    const QRectF singleAxis = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(3840.0, 1079.4),
        QPointF(0.0, 500.0), bounds, kMinimumSelectionSize, kCanvasAspectRatio);
    require(singleAxis == QRectF(1920.0, 0.0, 1920.0, 1080.0),
            "a locked marquee with one anchor on the exclusive edge must keep the pressed real "
            "pixel inside the canvas");
}

void marqueeDragIgnoresCoordinateRoundTripNoise() {
    const QRectF bounds(0.0, 0.0, 2880.0, 1620.0);

    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd(QPointF(0.0, 0.0), QPointF(0.0, 0.0));
    selection.beginMoveDrag(QPointF(0.0, 0.0));
    selection.setSelectionRect(selection.selectionRectForDrag(ScreenshotSelectionDragMode::Marquee,
                                                              QPointF(2879.0 - 1e-9, 1619.0 + 1e-9),
                                                              bounds, kMinimumSelectionSize));
    require(selection.pixelSelection() == QRect(0, 0, 2880, 1620),
            "logical-to-physical round-trip noise must not shrink the pointer cell span");
}

void followModeGrabReachesTheFullCanvasAtTheExtremePointerPosition() {
    const QRectF selection(0.0, 0.0, 3838.0, 2158.0);
    const QRectF bounds(0.0, 0.0, 3840.0, 2160.0);
    const QRectF grabbed =
        grabAdjustedScreenshotSelectionRect(ScreenshotSelectionDragMode::BottomRight, selection,
                                            QPointF(3839.0, 2159.0), bounds, kMinimumSelectionSize);
    require(grabbed == QRectF(0.0, 0.0, 3840.0, 2160.0),
            "a follow-mode grab onto the last pointer cell must keep that cell inside the "
            "selection");
}

void pointerSeededSelectionsAddressWholePointerCells() {
    ScreenshotSelectionModel selection;
    selection.setSelectionStartEnd(QPointF(10.4, 20.4), QPointF(30.6, 40.6));
    require(selection.normalizedSelection() == QRectF(10.0, 20.0, 22.0, 22.0),
            "pointer-seeded selections must span both pointer cells inclusively");
    require(selection.pixelSelection() == QRect(10, 20, 22, 22),
            "pointer-seeded selections must convert to their exact pixel rectangle");

    selection.setSelectionStartEnd(QPointF(15.7, 25.7), QPointF(15.7, 25.7));
    require(!selection.hasPixelSelection(),
            "a click on a single pointer cell must not create a selection");
}

void lockedMovementFollowResizeKeepsTheExactRatioOnWholePixels() {
    const QRectF bounds(0.0, 0.0, 800.0, 600.0);

    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(100.0, 100.0, 220.0, 100.0));
    selection.toggleAspectRatioLock(kMinimumSelectionSize);
    // Press exactly on the bottom-right border: the grab offset is zero, so the
    // dominant dragged border must keep tracking the pointer delta while the
    // ratio-exact width of this drag stays fractional.
    selection.beginMoveDrag(QPointF(320.0, 200.0));
    const QRectF dragged =
        selection.selectionRectForDrag(ScreenshotSelectionDragMode::BottomRight,
                                       QPointF(350.0, 237.0), bounds, kMinimumSelectionSize);
    require(dragged.topLeft() == QPointF(100.0, 100.0) &&
                std::abs(dragged.bottom() - 237.0) < kComparisonTolerance,
            "movement-follow locked resize must preserve the press-time grab offset");
    require(std::abs(dragged.right() - 401.4) < kComparisonTolerance,
            "locked resize must keep the exact ratio on the non-dominant axis");
    require(std::abs(dragged.height() / dragged.width() - 100.0 / 220.0) < kComparisonTolerance,
            "locked resize should retain the original aspect ratio");
    const QRect pixels = screenshotPixelRectForSelection(dragged);
    require(pixels == QRect(100, 100, 302, 137),
            "fractional locked edges must capture through the last covered pixel");
}

CapturedDisplayModel syntheticDisplay(const QRect& physicalRect, const QRect& canvasRect) {
    CapturedDisplayModel display;
    display.physicalRect = physicalRect;
    display.canvasRect = canvasRect;
    display.logicalRect = physicalRect;
    display.active = true;
    return display;
}

void globalMouseDesktopPointsMapAcrossMixedScaleDisplays() {
    ScreenshotDisplaySession displays;
    auto left = syntheticDisplay(QRect(-2400, -400, 2400, 1800), QRect(0, 0, 2400, 1800));
    left.logicalRect = QRect(-1200, -200, 1200, 900);
    displays.appendDisplay(left);
    auto right = syntheticDisplay(QRect(0, 0, 1920, 1080), QRect(2400, 400, 1920, 1080));
    right.logicalRect = QRect(0, 0, 1920, 1080);
    displays.appendDisplay(right);
    ScreenshotGeometryMapper geometry;
    const auto begin = geometry.physicalPositionForLogicalPoint(displays, QPointF(-1100.5, -100.5));
    const auto finish = geometry.physicalPositionForLogicalPoint(displays, QPoint(300, 200));
    require(begin == QPoint(-2201, -201) && finish == QPoint(300, 200),
            "Quartz desktop points must map independently on Retina and non-Retina displays");
    require(
        geometry.canvasPositionForPhysicalPoint(displays, begin) == QPointF(199, 199) &&
            geometry.canvasPositionForPhysicalPoint(displays, finish) == QPointF(2700, 600),
        "cross-display global drags must preserve capture pixel endpoints and negative origins");
    require(geometry.physicalPositionForLogicalPoint(displays, QPoint(0, 50)) == QPoint(0, 50),
            "the logical shared edge belongs to the adjacent display without double scaling");
    snow_shot::capture_detail::ScrollingSelectionMovement movement;
    require(movement.begin(ScreenshotScrollingRecognitionMode::Horizontal,
                           ScreenshotScrollingRecognitionMode::Horizontal,
                           QRect(199, 199, 600, 400), begin),
            "begin mixed-DPI scrolling movement");
    require(movement.update(finish, QRect(0, 0, 4320, 1800)) == QRect(2700, 199, 600, 400),
            "scrolling movement must cross mixed-DPI displays without scaling or off-axis drift");
    ScreenshotDisplaySession empty;
    require(geometry.physicalPositionForLogicalPoint(empty, QPoint(-10, 20)) == QPoint(-10, 20),
            "coordinate conversion must have a stable fallback before display capture is ready");
}

void physicalPointMappingUsesHalfOpenMonitorBounds() {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(syntheticDisplay(QRect(0, 0, 100, 100), QRect(0, 0, 100, 100)));
    displays.appendDisplay(syntheticDisplay(QRect(100, 0, 200, 100), QRect(100, 0, 200, 100)));

    ScreenshotGeometryMapper geometry;
    require(geometry.displayForPhysicalPoint(displays, QPointF(50, 50)) == &displays.displayAt(0),
            "physical pointer inside the first monitor selected the wrong display");
    require(geometry.displayForPhysicalPoint(displays, QPointF(100, 50)) == &displays.displayAt(1),
            "the shared monitor edge must belong to the next half-open display");
    require(geometry.displayForPhysicalPoint(displays, QPointF(300, 50)) == nullptr,
            "a pointer on the exclusive right edge must not select a monitor");
}

void selectorDisplayIdentityPreventsMixedScaleCrossMapping() {
    ScreenshotDisplaySession displays;
    auto first = syntheticDisplay(QRect(0, 0, 200, 200), QRect(0, 0, 100, 100));
    first.stableId = QStringLiteral("display:1");
    first.canvasRect = QRect(0, 0, 200, 200);
    auto second = syntheticDisplay(QRect(100, 0, 100, 100), QRect(100, 0, 100, 100));
    second.stableId = QStringLiteral("display:2");
    second.canvasRect = QRect(200, 0, 100, 100);
    displays.appendDisplay(first);
    displays.appendDisplay(second);
    ScreenshotGeometryMapper geometry;
    const QRectF rect(120, 20, 20, 20);
    require(geometry.canvasRectForPhysicalRect(displays, rect, QStringLiteral("display:1")) == rect,
            "Retina selector output must remain on the queried display");
    require(geometry.canvasRectForPhysicalRect(displays, rect, QStringLiteral("display:2")) ==
                QRectF(220, 20, 20, 20),
            "secondary display output must map using its own canvas origin");
    require(
        geometry.canvasRectForPhysicalRect(displays, rect, QStringLiteral("display:3")).isEmpty(),
        "obsolete display output must not select a different monitor");
}

void physicalWindowRectIsClippedAndMappedAcrossMonitors() {
    ScreenshotDisplaySession displays;
    displays.appendDisplay(syntheticDisplay(QRect(0, 0, 100, 100), QRect(0, 0, 100, 100)));
    displays.appendDisplay(syntheticDisplay(QRect(100, 0, 200, 100), QRect(100, 0, 200, 100)));

    ScreenshotGeometryMapper geometry;
    const QRectF windowRect(-25.0, 20.0, 350.0, 60.0);
    const QRectF canvasRect = geometry.canvasRectForPhysicalRect(displays, windowRect);
    require(canvasRect == QRectF(0.0, 20.0, 300.0, 60.0),
            "a cross-monitor window must be clipped to visible desktop geometry");

    const QRectF offDesktopRect(-80.0, -80.0, 40.0, 40.0);
    require(geometry.canvasRectForPhysicalRect(displays, offDesktopRect).isEmpty(),
            "a fully off-screen window must produce an empty selection geometry");
}

void dragAnchorDoesNotReplaceTheActualCursorPosition() {
    const QRectF selection(10.0, 10.0, 40.0, 40.0);
    // QRectF selections use half-open bounds, so the bottom-right screenshot
    // pixel is one unit inside the geometric edge returned by bottomRight().
    const QPointF cursorPosition(selection.right() - 1.0, selection.bottom() - 1.0);
    const std::optional<QPointF> anchor = screenshotSelectionDragAnchor(
        selection, ScreenshotSelectionDragMode::BottomRight, cursorPosition, 10.0);

    require(anchor.has_value() && anchor.value() == selection.bottomRight(),
            "bottom-right drags should keep the picker sample anchored to the handle");
    require(cursorPosition != anchor.value(),
            "a handle anchor must remain distinct from the cursor position used for navigation");
}

void shadowWidthPreservesSelectionAndToolbarPlacement() {
    const ScreenshotGeometryMapper geometry;
    ScreenshotToolbarPlacementSnapshot toolbar;
    toolbar.bottom.mainToolbarContentRect = QRect(0, 0, 300, 40);
    toolbar.bottom.occupiedContentRect = QRect(0, 0, 300, 80);
    toolbar.top.mainToolbarContentRect = QRect(0, 40, 300, 40);
    toolbar.top.occupiedContentRect = QRect(0, 0, 300, 80);
    for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        CapturedDisplayModel display;
        display.active = true;
        display.logicalRect = QRect(-1200, 0, 1200, 900);
        display.physicalRect =
            QRect(-qRound(1200 * scale), 0, qRound(1200 * scale), qRound(900 * scale));
        display.canvasRect = display.physicalRect;
        for (const int bottom : {400, 800, 880}) {
            ScreenshotSelectionModel selection;
            selection.setSelectionRect(
                QRectF(-900 * scale, 100 * scale, 600 * scale, (bottom - 100) * scale));
            const QRectF originalSelection = selection.normalizedSelection();
            const QRect originalPixels = selection.pixelSelection();
            ScreenshotToolbarPresentationState state;
            state.selectionPixels = originalPixels;
            state.selectionCanvas = originalSelection;
            const auto baseline = snow_shot::presentation::screenshotToolbarPlacement(
                state, geometry, &display, toolbar, display.logicalRect, 4);
            require(baseline.usesTopRightPlacement == (bottom == 880),
                    "fixture must cover both bottom and top toolbar placement");
            for (const int width : {1, 8, 32, 64, 0}) {
                static_cast<void>(selection.setShadowWidth(width));
                require(selection.normalizedSelection() == originalSelection &&
                            selection.pixelSelection() == originalPixels,
                        "shadow width must preserve the selection area and capture pixels");
                state.shadowWidth = selection.shadowWidth();
                const auto placement = snow_shot::presentation::screenshotToolbarPlacement(
                    state, geometry, &display, toolbar, display.logicalRect, 4);
                require(placement.contentPosition == baseline.contentPosition &&
                            placement.usesTopRightPlacement == baseline.usesTopRightPlacement,
                        "shadow width must not move the toolbar or change its placement side");
            }
        }
    }
}
} // namespace

int main() {
    shadowWidthPreservesSelectionAndToolbarPlacement();
    lockedAspectRatioAppliesToEveryResizeHandle();
    lockedResizeStaysInsideBoundsWithoutDistorting();
    lockedResizeAllowsFlippingAcrossOppositeEdges();
    lockedCornerResizeCanFlipBothAxes();
    unlockedResizeCanChangeAspectRatio();
    persistedAspectRatioLockConstrainsNewMarquee();
    grabAdjustmentSnapsOnlyTheDraggedEdgesToThePressPosition();
    grabAdjustmentRespectsBoundsAndMinimumSize();
    positionFollowDragTracksThePointerAfterGrabAdjustment();
    movementFollowDragKeepsThePressTimeGrabOffset();
    marqueeDragUsesTheSharedGeometryTransactionWithoutMinimumInflation();
    marqueeDragSelectsSinglePixelStrips();
    marqueeDragCanMaintainAnAspectRatio();
    marqueeDragReachesTheFullCanvasAtTheExtremePointerPosition();
    lockedMarqueeDragReachesTheFullCanvasAtTheExtremePointerPosition();
    lockedMarqueeDragStaysInsideTheCanvasFromExclusiveEdgePointerCells();
    marqueeDragIgnoresCoordinateRoundTripNoise();
    followModeGrabReachesTheFullCanvasAtTheExtremePointerPosition();
    pointerSeededSelectionsAddressWholePointerCells();
    lockedMovementFollowResizeKeepsTheExactRatioOnWholePixels();
    selectionShadowDefaultsToRequestedColor();
    globalMouseDesktopPointsMapAcrossMixedScaleDisplays();
    physicalPointMappingUsesHalfOpenMonitorBounds();
    physicalWindowRectIsClippedAndMappedAcrossMonitors();
    selectorDisplayIdentityPreventsMixedScaleCrossMapping();
    dragAnchorDoesNotReplaceTheActualCursorPosition();
    return 0;
}
