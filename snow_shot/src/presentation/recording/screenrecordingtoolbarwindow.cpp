#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/storage/settingsadapters.h"
#include "screenrecordinggeometry.h"

#include <QScreen>
#include <QScopedValueRollback>

namespace {
constexpr int kToolbarGap = 4;

ScreenshotToolPalette::Options recordingToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showSpotlightTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showHistoryActions = true;
    options.showRecordingControls = true;
    options.recordingDrawingMode = true;
    options.enableStyleToolbar = true;
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    options.styleDefaults = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
    return options;
}
} // namespace

ScreenRecordingToolbarWindow::ScreenRecordingToolbarWindow(QWidget* parent)
    : ScreenshotFloatingToolPaletteWindow(recordingToolbarOptions(), parent) {
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose, false);
    prepareForDisplay();
    // Secondary rows can grow without changing the fixed native frame or the
    // main-row anchor. Observe the committed host content, including those cases.
    connect(paletteHost(), &ScreenshotToolPaletteHost::visibleContentChanged, this, [this]() {
        if (!m_manuallyDragged) {
            placeForPhysicalRegion(m_physicalRegion);
        }
    });
    connect(paletteHost(), &ScreenshotToolPaletteHost::dragStarted, this,
            [this](const QPoint&) { m_manuallyDragged = true; });
}

void ScreenRecordingToolbarWindow::showAndActivate() {
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void ScreenRecordingToolbarWindow::placeForPhysicalRegion(const QRect& physicalRegion) {
    if (m_placing || !physicalRegion.isValid() || physicalRegion.isEmpty()) {
        return;
    }
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(physicalRegion);
    if (screen == nullptr) {
        return;
    }
    // Preparing the layout and changing the row arrangement can emit content
    // changes synchronously; the outer placement already accounts for them.
    const QScopedValueRollback<bool> placing(m_placing, true);
    m_physicalRegion = physicalRegion;
    m_manuallyDragged = false;
    const QRect logicalBounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRectF logicalRegion =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physicalRegion, screen);
    const QRect anchorRegion = snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(
                                   logicalRegion, screen->devicePixelRatio())
                                   .windowGeometry;
    setPlacementContext(screen, logicalBounds, physicalBounds);
    prepareForDisplay();

    const ScreenshotToolbarPlacementSnapshot toolbarGeometry = placementSnapshot();
    if (!toolbarGeometry.bottom.isValid()) {
        return;
    }
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(anchorRegion.left() + anchorRegion.width(),
                   anchorRegion.top() + anchorRegion.height()),
            QPoint(anchorRegion.left() + anchorRegion.width(), anchorRegion.top()),
            toolbarGeometry.bottom, toolbarGeometry.top, logicalBounds, kToolbarGap);
    setStyleToolbarAboveMain(placement.usesTopRightPlacement);
    resetPhysicalSizeInvariant();
    moveContentTo(placement.contentPosition);
}
