#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "screenrecordinggeometry.h"

#include <QScreen>

namespace {
constexpr int kToolbarGap = 4;

ScreenshotToolPalette::Options recordingToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showRecordingControls = true;
    options.enableStyleToolbar = false;
    return options;
}
} // namespace

ScreenRecordingToolbarWindow::ScreenRecordingToolbarWindow(QWidget* parent)
    : ScreenshotFloatingToolPaletteWindow(recordingToolbarOptions(), parent) {
    setObjectName(QStringLiteral("screenRecordingToolbar"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    prepareForDisplay();
}

void ScreenRecordingToolbarWindow::placeForPhysicalRegion(const QRect& physicalRegion) {
    if (!physicalRegion.isValid() || physicalRegion.isEmpty()) {
        return;
    }
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physicalRegion);
    if (screen == nullptr) {
        return;
    }
    const QRect logicalBounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRectF logicalRegion =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physicalRegion, screen);
    const QRect anchorRegion = snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(
                                   logicalRegion, screen->devicePixelRatio())
                                   .windowGeometry;
    setPlacementContext(screen, logicalBounds, physicalBounds);
    prepareForDisplay();

    const QRect toolbarRect = bottomPlacementContentRect();
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(anchorRegion.left() + anchorRegion.width(),
                   anchorRegion.top() + anchorRegion.height()),
            QPoint(anchorRegion.left() + anchorRegion.width(), anchorRegion.top()), toolbarRect,
            logicalBounds, kToolbarGap, topRightMainToolbarContentRect(),
            topPlacementContentRect());
    setStyleToolbarAboveMain(placement.usesTopRightPlacement);
    resetPhysicalSizeInvariant();
    moveContentTo(placement.contentPosition);
}
