#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/storage/settingsadapters.h"
#include "screenrecordinggeometry.h"
#include "screenrecordingperfinstrumentation.h"

#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#endif

#include <QScreen>
#include <QGuiApplication>
#include <QCloseEvent>
#include "widgets/color_picker.h"
#include "widgets/select.h"
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
#ifdef Q_OS_MACOS
    snow_shot::platform::configureScreenshotToolbarWindow(this);
#endif
    // Secondary rows can grow without changing the fixed native frame or the
    // main-row anchor. Observe the committed host content, including those cases.
    connect(paletteHost(), &ScreenshotToolPaletteHost::visibleContentChanged, this, [this]() {
        if (!m_manuallyDragged && !m_regionInteractionActive) {
            placeForRecordingRegion(m_recordingRegion);
        }
    });
    connect(paletteHost(), &ScreenshotToolPaletteHost::dragStarted, this,
            [this](const QPoint&) { m_manuallyDragged = true; });
}

void ScreenRecordingToolbarWindow::showAndActivate() {
    if (m_regionInteractionActive) {
        return;
    }
    prepareForDisplay();
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    SNOW_SHOT_RECORDING_PERF_MILESTONE("toolbar.show_and_activate_returned");
}

void ScreenRecordingToolbarWindow::placeForRecordingRegion(const QRect& recordingRegion) {
    SNOW_SHOT_RECORDING_PERF_SCOPE("toolbar.place_for_region");
    if (m_regionInteractionActive || m_placing || !recordingRegion.isValid() ||
        recordingRegion.isEmpty()) {
        return;
    }
#ifdef Q_OS_MACOS
    QScreen* screen = QGuiApplication::screenAt(recordingRegion.center());
    if (screen == nullptr)
        screen = QGuiApplication::primaryScreen();
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(recordingRegion);
#endif
    if (screen == nullptr) {
        return;
    }
    // Preparing the layout and changing the row arrangement can emit content
    // changes synchronously; the outer placement already accounts for them.
    const QScopedValueRollback<bool> placing(m_placing, true);
    m_recordingRegion = recordingRegion;
    m_manuallyDragged = false;
    const QRect logicalBounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
#ifdef Q_OS_MACOS
    const QRectF logicalRegion(recordingRegion);
    const qreal regionScale = 1.0;
#else
    const QRectF logicalRegion =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(recordingRegion, screen);
    const qreal regionScale = screen->devicePixelRatio();
#endif
    const QRect anchorRegion = snow_shot::presentation::recording::screenRecordingAreaFrameGeometry(
                                   logicalRegion, regionScale)
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
    moveContentTo(placement.contentPosition);
    // As in screenshot toolbar presentation, reconcile the actual native frame
    // after moving across displays before it can paint the prepared content.
    prepareForDisplay();
}

void ScreenRecordingToolbarWindow::beginRegionInteraction() {
    if (m_regionInteractionActive) {
        return;
    }
    m_regionInteractionActive = true;
    for (auto* picker : palette()->findChildren<adqt::widgets::AdColorPicker*>()) {
        picker->setPopupVisible(false);
    }
    for (auto* select : palette()->findChildren<adqt::widgets::AdSelect*>()) {
        select->setPopupVisible(false);
    }
    hide();
    for (QWidget* child : findChildren<QWidget*>()) {
        if (child->isWindow()) {
            child->hide();
        }
    }
}

void ScreenRecordingToolbarWindow::endRegionInteraction(const QRect& recordingRegion) {
    if (!m_regionInteractionActive) {
        return;
    }
    m_regionInteractionActive = false;
    placeForRecordingRegion(recordingRegion);
    prepareForDisplay();
    show();
    raise();
}

void ScreenRecordingToolbarWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit closeRequested();
}
