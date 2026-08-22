#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"

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

    const QRectF marquee = selection.selectionRectForDrag(
        ScreenshotSelectionDragMode::Marquee, QPointF(43.0, 54.0),
        QRectF(0.0, 0.0, 100.0, 100.0), kMinimumSelectionSize);
    require(marquee == QRectF(40.0, 50.0, 3.0, 4.0),
            "marquee drags must use their actual pointer span without resize minimums");
    require(!screenshotSelectionDragAnchor(marquee, ScreenshotSelectionDragMode::Marquee,
                                           QPointF(43.0, 54.0), kMinimumSelectionSize)
                 .has_value(),
            "marquee drags must sample the color picker at the pointer, not a resize handle");
    require(screenshotSelectionDragModeForPoint(QRectF(10.0, 10.0, 20.0, 20.0),
                                                QPointF(50.0, 50.0), false, 8.0,
                                                kMinimumSelectionSize) ==
                ScreenshotSelectionDragMode::BottomRight,
            "Move must classify a point outside the selection as a directional resize");
}

void marqueeDragCanMaintainAnAspectRatio() {
    const QRectF bounds(0.0, 0.0, 200.0, 200.0);
    const QRectF square = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(40.0, 50.0),
        QPointF(70.0, 100.0), bounds, kMinimumSelectionSize, 1.0);
    require(square == QRectF(40.0, 50.0, 50.0, 50.0),
            "locked marquee drags should expand the shorter pointer span proportionally");

    const QRectF flipped = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(100.0, 100.0),
        QPointF(40.0, 50.0), bounds, kMinimumSelectionSize, 0.5);
    require(std::abs(flipped.width() - 100.0) < kComparisonTolerance &&
                std::abs(flipped.height() - 50.0) < kComparisonTolerance &&
                flipped.left() == 0.0 && flipped.top() == 50.0,
            "locked marquee drags should retain their ratio when crossing both axes");

    const QRectF clipped = draggedScreenshotSelectionRect(
        ScreenshotSelectionDragMode::Marquee, QRectF(), QPointF(180.0, 180.0),
        QPointF(240.0, 230.0), bounds, kMinimumSelectionSize, 1.0);
    require(clipped == QRectF(180.0, 180.0, 20.0, 20.0),
            "locked marquee drags should remain inside the canvas bounds");
}

CapturedDisplayModel syntheticDisplay(const QRect& physicalRect, const QRect& canvasRect) {
    CapturedDisplayModel display;
    display.physicalRect = physicalRect;
    display.canvasRect = canvasRect;
    display.logicalRect = physicalRect;
    display.active = true;
    return display;
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
} // namespace

int main() {
    lockedAspectRatioAppliesToEveryResizeHandle();
    lockedResizeStaysInsideBoundsWithoutDistorting();
    lockedResizeAllowsFlippingAcrossOppositeEdges();
    lockedCornerResizeCanFlipBothAxes();
    unlockedResizeCanChangeAspectRatio();
    marqueeDragUsesTheSharedGeometryTransactionWithoutMinimumInflation();
    marqueeDragCanMaintainAnAspectRatio();
    selectionShadowDefaultsToRequestedColor();
    physicalPointMappingUsesHalfOpenMonitorBounds();
    physicalWindowRectIsClippedAndMappedAcrossMonitors();
    dragAnchorDoesNotReplaceTheActualCursorPosition();
    return 0;
}
