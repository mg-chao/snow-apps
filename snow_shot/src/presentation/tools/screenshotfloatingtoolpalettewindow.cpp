#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"

#include "screenshotfloatingtoolpalettenative.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "screenshottoolbarperfinstrumentation.h"

#include "widgets/control_scale.h"
#include "widgets/dpi_stable_window_controller.h"
#include "widgets/button.h"
#include "widgets/select.h"
#include "icon_registry.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QJsonValue>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QPointF>
#include <QRegion>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace {
namespace native = screenshot_floating_palette_native;
}

ScreenshotFloatingToolPaletteWindow::ScreenshotFloatingToolPaletteWindow(
    const ScreenshotToolPalette::Options& options, QWidget* parent)
    : QWidget(parent, native::windowFlags()) {
    applyWindowAttributes();

    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* contentPanel = new QWidget(this);
    m_panel = contentPanel;
    contentPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    contentPanel->setAttribute(Qt::WA_NoSystemBackground, true);
    contentPanel->setAutoFillBackground(false);

    auto* contentLayout = new QHBoxLayout(contentPanel);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_paletteHost = new ScreenshotToolPaletteHost(options, contentPanel);
    contentLayout->addWidget(m_paletteHost);
    layout->addWidget(contentPanel);

    refreshGeometryForVisibleContent(false);
    const QList<adqt::widgets::AdButton*> buttons =
        m_paletteHost->findChildren<adqt::widgets::AdButton*>();

    m_scaleScope = new adqt::widgets::AdControlScaleScope(m_paletteHost, this);
    m_dpiController = new adqt::widgets::AdDpiStableWindowController(this, this);
    m_dpiController->captureBaseline();
    for (adqt::widgets::AdButton* button : buttons) {
        if (button->busyIndicatorSurface() != nullptr) {
            m_dpiController->registerAuxiliarySurface(button->busyIndicatorSurface());
        }
        connect(button, &adqt::widgets::AdButton::busyIndicatorSurfaceChanged, m_dpiController,
                [this](QWidget* surface) {
                    if (m_dpiController != nullptr) {
                        m_dpiController->registerAuxiliarySurface(surface);
                    }
                });
    }
    registerMaterializedScope(m_paletteHost);
    connect(m_paletteHost->palette(), &ScreenshotToolPalette::materializedScope, this,
            &ScreenshotFloatingToolPaletteWindow::registerMaterializedScope);
    connect(
        m_dpiController, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted, this,
        [this](const adqt::widgets::AdControlScaleContext& context,
               const QSize& logicalClientExtent) {
            m_processingNativeDpiChange = true;
            m_committedWindowDevicePixelRatio = context.currentDpr;
            ensureReferenceDevicePixelRatio();
            const adqt::widgets::AdControlScaleContext effectiveContext =
                adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(
                    m_referenceDevicePixelRatio, context.currentDpr, m_paletteScaleMultiplier,
                    context.revision);
            if (m_scaleScope != nullptr) {
                m_scaleScope->publishScale(effectiveContext, logicalClientExtent);
            }
            if (m_paletteHost != nullptr) {
                m_paletteHost->commitDpiScale(effectiveContext.logicalScale, logicalClientExtent,
                                              ScreenshotToolPaletteHost::defaultShadowMargins());
            }
            refreshGeometryForVisibleContent(true, true);
            m_processingNativeDpiChange = false;
            emit dpiScaleCommitCompleted();
        });

    connect(m_paletteHost, &ScreenshotToolPaletteHost::dragStarted, this,
            [this](const QPoint& pos) { beginPaletteDrag(pos); });
    connect(m_paletteHost, &ScreenshotToolPaletteHost::dragMoved, this,
            [this](const QPoint& pos) { updatePaletteDrag(pos); });
    connect(m_paletteHost, &ScreenshotToolPaletteHost::dragFinished, this,
            [this](const QPoint&) { finishPaletteDrag(true); });
    connect(m_paletteHost, &ScreenshotToolPaletteHost::visibleContentChanged, this,
            &ScreenshotFloatingToolPaletteWindow::handlePaletteContentChange);

    const auto applyToolbarSize = [this](const QString& size) {
        setPaletteScaleMultiplier(size == QStringLiteral("small") ? 0.8 : 1.0);
    };
    applyToolbarSize(snow_shot::storage::ScreenshotUiSettings().toolbarSize());
    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    connect(&configuration, &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this, applyToolbarSize](const QString& key, const QJsonValue&) {
                if (key == QStringLiteral("screenshot_ui/toolbar_size")) {
                    applyToolbarSize(snow_shot::storage::ScreenshotUiSettings().toolbarSize());
                }
            });
}

ScreenshotFloatingToolPaletteWindow::~ScreenshotFloatingToolPaletteWindow() {}

void ScreenshotFloatingToolPaletteWindow::setPaletteScaleMultiplier(qreal multiplier) {
    if (!std::isfinite(multiplier) || multiplier <= 0.0) {
        multiplier = 1.0;
    }
    multiplier = std::clamp<qreal>(multiplier, 0.25, 4.0);
    if (qFuzzyCompare(m_paletteScaleMultiplier + 1.0, multiplier + 1.0)) {
        return;
    }
    m_paletteScaleMultiplier = multiplier;
    resetPhysicalSizeInvariant();
    refreshGeometryForVisibleContent(m_lastRequestedContentPositionValid || isVisible(), true);
}

qreal ScreenshotFloatingToolPaletteWindow::paletteScaleMultiplier() const {
    return m_paletteScaleMultiplier;
}

ScreenshotToolPalette* ScreenshotFloatingToolPaletteWindow::palette() const {
    return m_paletteHost != nullptr ? m_paletteHost->palette() : nullptr;
}

ScreenshotToolPaletteHost* ScreenshotFloatingToolPaletteWindow::paletteHost() const {
    return m_paletteHost;
}

void ScreenshotFloatingToolPaletteWindow::setOwnerWindow(QWidget* owner) {
    m_transientOwnerWindow.clear();
    if (parentWidget() == owner && (windowFlags() & Qt::WindowType_Mask) == Qt::Tool) {
        native::setNativePaletteOwner(winId(), owner);
        return;
    }

    cancelDrag();
    const bool wasVisible = isVisible();
    const QRect previousGeometry = geometry();
    if (wasVisible) {
        hide();
    }

    setParent(owner, native::windowFlags());
    applyWindowAttributes();
    if (previousGeometry.isValid() && !previousGeometry.isEmpty()) {
        setGeometry(previousGeometry);
    }
    native::setNativePaletteOwner(winId(), owner);
    if (m_placementScreen != nullptr) {
        applyPlacementScreen();
    }
    refreshGeometryForVisibleContent(true, true);

    if (wasVisible && (owner == nullptr || owner->isVisible())) {
        show();
        repaint();
        raise();
    }
}

void ScreenshotFloatingToolPaletteWindow::setTransientOwnerWindow(QWidget* owner) {
    m_transientOwnerWindow = owner;

    QWindow* ownerHandle = nullptr;
    if (owner != nullptr) {
        ownerHandle = owner->windowHandle();
        if (ownerHandle == nullptr) {
            static_cast<void>(owner->winId());
            ownerHandle = owner->windowHandle();
        }
    }

    const WId paletteWindowId = winId();
    if (QWindow* handle = windowHandle()) {
        handle->setTransientParent(ownerHandle);
    }
    native::setNativePaletteOwner(paletteWindowId, owner);
}

void ScreenshotFloatingToolPaletteWindow::setPlacementContext(QScreen* screen,
                                                              const QRect& logicalBounds,
                                                              const QRect& physicalBounds) {
    const bool placementScreenChangedByCaller = m_placementScreen != screen;
    const bool changed = placementScreenChangedByCaller ||
                         m_movementLogicalBounds != logicalBounds ||
                         m_movementPhysicalBounds != physicalBounds;
    if (placementScreenChangedByCaller) {
        resetPhysicalSizeInvariant();
    }
    m_placementScreen = screen;
    m_movementLogicalBounds = logicalBounds;
    m_movementPhysicalBounds = physicalBounds;
    const bool placementScreenChanged = applyPlacementScreen();
    if (!changed && !placementScreenChanged) {
        return;
    }

    refreshGeometryForVisibleContent(m_lastRequestedContentPositionValid || isVisible(), true);
}

void ScreenshotFloatingToolPaletteWindow::setStyleToolbarAboveMain(bool above) {
    if (m_paletteHost == nullptr) {
        return;
    }

    m_paletteHost->setStyleToolbarAboveMain(above);
    if (m_draggingPalette) {
        m_dragContentPosition = QPointF(contentPosition());
    }
}

void ScreenshotFloatingToolPaletteWindow::prepareForDisplay() {
    if (m_paletteHost != nullptr) {
        m_paletteHost->prepareForDisplay();
    }
    const qreal currentDpr = currentWindowDevicePixelRatio();
    if (m_lastAppliedWindowDevicePixelRatio <= 0.0 ||
        !qFuzzyCompare(m_lastAppliedWindowDevicePixelRatio + 1.0, currentDpr + 1.0) ||
        (m_paletteHost != nullptr && fixedWindowSizeHint() != size())) {
        refreshGeometryForVisibleContent(m_lastRequestedContentPositionValid || isVisible());
    }
}

void ScreenshotFloatingToolPaletteWindow::resetPhysicalSizeInvariant() {
    if (m_dpiController != nullptr) {
        m_dpiController->resetBaseline();
    }
    m_referenceDevicePixelRatio = 0.0;
    m_committedWindowDevicePixelRatio = 0.0;
    m_stablePhysicalWindowSize = QSize();
    m_lastAppliedWindowDevicePixelRatio = 0.0;
}

void ScreenshotFloatingToolPaletteWindow::moveContentTo(const QPoint& position) {
    moveContentTo(position, QSize());
}

void ScreenshotFloatingToolPaletteWindow::moveContentTo(const QPoint& position,
                                                        const QSize& windowSize) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.move_content");
    const qreal currentDpr = currentWindowDevicePixelRatio();
    if (m_lastAppliedWindowDevicePixelRatio <= 0.0 ||
        !qFuzzyCompare(m_lastAppliedWindowDevicePixelRatio + 1.0, currentDpr + 1.0)) {
        syncPalettePhysicalScale();
        updatePaletteGeometryForVisibleContent();
    }
    const QPoint targetPosition =
        m_movementClampingEnabled ? constrainedContentPosition(position) : position;
    const QSize hintSize = fixedWindowSizeHint();
    const QSize fallbackSize = hintSize.isValid() && !hintSize.isEmpty() ? hintSize : size();
    const QSize targetSize =
        windowSize.isValid() && !windowSize.isEmpty() ? windowSize : fallbackSize;
    const bool windowSizeChanged = targetSize != size();
    m_lastRequestedContentPosition = targetPosition;
    m_lastRequestedContentPositionValid = true;
    const QRect targetGeometry(targetPosition - contentOffset(), targetSize);
    if (geometry() != targetGeometry) {
        setGeometry(targetGeometry);
    }
    if (contentPosition() != targetPosition) {
        setGeometry(QRect(targetPosition - contentOffset(), targetSize));
    }
    m_lastRequestedContentPosition = contentPosition();
    m_lastRequestedContentPositionValid = true;
    if (windowSizeChanged) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("window.resize_reanchor");
        refreshPaletteWindow();
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_windowResizeOrReanchorCount;
#endif
    }
    if (m_draggingPalette) {
        m_dragContentPosition = QPointF(targetPosition);
    }
    updateMainToolbarPositionSnapshot();
    if (!m_processingNativeDpiChange && windowSizeChanged) {
        refreshStablePhysicalWindowSize();
    }
}

QPoint ScreenshotFloatingToolPaletteWindow::contentPosition() const {
    return pos() + contentOffset();
}

QRect ScreenshotFloatingToolPaletteWindow::occupiedContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->occupiedContentRect()
                                    : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotFloatingToolPaletteWindow::visualContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->visualContentRect()
                                    : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotFloatingToolPaletteWindow::fullContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->fullContentRect()
                                    : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotFloatingToolPaletteWindow::bottomPlacementContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->bottomPlacementContentRect()
                                    : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotFloatingToolPaletteWindow::topPlacementContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->topPlacementContentRect()
                                    : QRect(QPoint(0, 0), contentSizeHint());
}

QRect ScreenshotFloatingToolPaletteWindow::topRightMainToolbarContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->topRightMainToolbarContentRect()
                                    : QRect(QPoint(0, 0), contentSizeHint());
}

QSize ScreenshotFloatingToolPaletteWindow::contentSizeHint() const {
    if (m_paletteHost != nullptr) {
        return m_paletteHost->contentSizeHint();
    }

    return sizeHint();
}

QSize ScreenshotFloatingToolPaletteWindow::windowSizeHint() const {
    return fixedWindowSizeHint();
}

bool ScreenshotFloatingToolPaletteWindow::containsInteractiveGlobalPoint(
    const QPoint& globalPosition) const {
    return isPointInInteractiveContent(mapFromGlobal(globalPosition));
}

bool ScreenshotFloatingToolPaletteWindow::stepStrokeWidth(int direction) {
    return m_paletteHost != nullptr && m_paletteHost->stepStrokeWidth(direction);
}

bool ScreenshotFloatingToolPaletteWindow::stepSelectionOpacity(int direction) {
    return m_paletteHost != nullptr && m_paletteHost->stepSelectionOpacity(direction);
}

bool ScreenshotFloatingToolPaletteWindow::stepSpotlightOpacity(int direction) {
    return m_paletteHost != nullptr && m_paletteHost->stepSpotlightOpacity(direction);
}

bool ScreenshotFloatingToolPaletteWindow::stepFilterIntensity(int direction) {
    return m_paletteHost != nullptr && m_paletteHost->stepFilterIntensity(direction);
}

bool ScreenshotFloatingToolPaletteWindow::stepPenFilterStrokeWidth(int direction) {
    return m_paletteHost != nullptr && m_paletteHost->stepPenFilterStrokeWidth(direction);
}

bool ScreenshotFloatingToolPaletteWindow::stepWatermarkFontSize(int direction) {
    return m_paletteHost != nullptr && m_paletteHost->stepWatermarkFontSize(direction);
}

void ScreenshotFloatingToolPaletteWindow::setMovementClampingEnabled(bool enabled) {
    m_movementClampingEnabled = enabled;
}

bool ScreenshotFloatingToolPaletteWindow::movementClampingEnabled() const {
    return m_movementClampingEnabled;
}

void ScreenshotFloatingToolPaletteWindow::cancelDrag() {
    finishPaletteDrag(false);
}

bool ScreenshotFloatingToolPaletteWindow::physicalDragActive() const {
    return m_dpiController != nullptr && m_dpiController->physicalDragActive();
}

adqt::widgets::AdDpiStableWindowDiagnostics
ScreenshotFloatingToolPaletteWindow::dpiTransitionDiagnostics() const {
    return m_dpiController != nullptr ? m_dpiController->diagnostics()
                                      : adqt::widgets::AdDpiStableWindowDiagnostics{};
}

bool ScreenshotFloatingToolPaletteWindow::event(QEvent* event) {
    const bool devicePixelRatioChanged =
        event != nullptr && event->type() == QEvent::DevicePixelRatioChange;
    if (event != nullptr) {
        if (event->type() == QEvent::WinIdChange) {
            applyPlacementScreen();
        }
    }

    const bool handled = QWidget::event(event);
    if (event != nullptr && event->type() == QEvent::Move && !m_geometryCommitActive) {
        m_lastRequestedContentPosition = contentPosition();
        m_lastRequestedContentPositionValid = true;
        if (m_draggingPalette) {
            m_dragContentPosition = QPointF(m_lastRequestedContentPosition);
        }
        updateMainToolbarPositionSnapshot();
    }
    if (devicePixelRatioChanged && m_dpiController == nullptr) {
        refreshGeometryForVisibleContent(true, true);
    }
    return handled;
}

bool ScreenshotFloatingToolPaletteWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_watermarkTextEditor && event != nullptr) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) {
            beginKeyboardFocusInteraction(m_watermarkTextEditor);
        } else if (event->type() == QEvent::FocusOut) {
            const QPointer<QWidget> editor = m_watermarkTextEditor;
            QTimer::singleShot(0, this, [this, editor]() {
                if (editor == nullptr || !editor->hasFocus()) {
                    endKeyboardFocusInteraction(editor);
                }
            });
        }
    }

    if (event != nullptr && event->type() == QEvent::Wheel &&
        handleToolbarWheel(static_cast<QWheelEvent*>(event))) {
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

bool ScreenshotFloatingToolPaletteWindow::nativeEvent(const QByteArray& eventType, void* message,
                                                      qintptr* result) {
    Q_UNUSED(eventType);
    if (handleNativeHitTest(message, result)) {
        return true;
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void ScreenshotFloatingToolPaletteWindow::hideEvent(QHideEvent* event) {
    endKeyboardFocusInteraction();
    cancelDrag();
    QWidget::hideEvent(event);
}

void ScreenshotFloatingToolPaletteWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyPlacementScreen();
    const qreal currentDpr = currentWindowDevicePixelRatio();
    if (m_lastAppliedWindowDevicePixelRatio <= 0.0 ||
        !qFuzzyCompare(m_lastAppliedWindowDevicePixelRatio + 1.0, currentDpr + 1.0)) {
        refreshGeometryForVisibleContent(true, true);
    } else {
        refreshPaletteWindow(true);
    }
}

void ScreenshotFloatingToolPaletteWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
}

void ScreenshotFloatingToolPaletteWindow::wheelEvent(QWheelEvent* event) {
    if (handleToolbarWheel(event)) {
        return;
    }

    QWidget::wheelEvent(event);
}

QPoint
ScreenshotFloatingToolPaletteWindow::constrainedContentPosition(const QPoint& position) const {
    return constrainedContentPosition(QPointF(position)).toPoint();
}

QPointF
ScreenshotFloatingToolPaletteWindow::constrainedContentPosition(const QPointF& position) const {
    if (!m_movementLogicalBounds.isValid()) {
        return position;
    }

    const QRect paletteRect = occupiedContentRect();
    if (paletteRect.isEmpty()) {
        return position;
    }

    QPointF constrained = position;
    const double minX = static_cast<double>(m_movementLogicalBounds.left() - paletteRect.left());
    const double minY = static_cast<double>(m_movementLogicalBounds.top() - paletteRect.top());
    const double maxX =
        std::max(minX, static_cast<double>(m_movementLogicalBounds.right() - paletteRect.right()));
    const double maxY = std::max(
        minY, static_cast<double>(m_movementLogicalBounds.bottom() - paletteRect.bottom()));
    constrained.setX(std::clamp(constrained.x(), minX, maxX));
    constrained.setY(std::clamp(constrained.y(), minY, maxY));
    return constrained;
}

void ScreenshotFloatingToolPaletteWindow::updatePaletteGeometryForVisibleContent() {
    if (m_panel == nullptr || m_paletteHost == nullptr) {
        return;
    }

#if defined(SNOW_SHOT_TEST_HOOKS)
    const QSize previousHostSize = m_paletteHost->size();
#endif
    m_paletteHost->prepareForDisplay();
    const QSize windowSize = fixedWindowSizeHint();
    if (m_panel->size() != windowSize) {
        m_panel->setFixedSize(windowSize);
    }
#if defined(SNOW_SHOT_TEST_HOOKS)
    if (previousHostSize != windowSize) {
        ++m_paletteGeometryRefreshCount;
    }
#endif
}

void ScreenshotFloatingToolPaletteWindow::refreshPaletteWindow(bool forceRepaint) {
    if (!isVisible()) {
        return;
    }

    if (forceRepaint) {
        repaint();
    } else {
        update();
    }
}

bool ScreenshotFloatingToolPaletteWindow::handleNativeHitTest(void* message,
                                                              qintptr* result) const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (message == nullptr || result == nullptr) {
        return false;
    }

    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_NCHITTEST) {
        return false;
    }

    RECT nativeWindowRect{};
    if (msg->hwnd == nullptr || GetWindowRect(msg->hwnd, &nativeWindowRect) == 0) {
        return false;
    }

    const qreal devicePixelRatio = currentWindowDevicePixelRatio();
    const qreal scale = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const QPoint localPosition(
        static_cast<int>(std::floor(
            static_cast<qreal>(GET_X_LPARAM(msg->lParam) - nativeWindowRect.left) / scale)),
        static_cast<int>(std::floor(
            static_cast<qreal>(GET_Y_LPARAM(msg->lParam) - nativeWindowRect.top) / scale)));
    if (isPointInInteractiveContent(localPosition)) {
        return false;
    }

    *result = HTTRANSPARENT;
    return true;
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}

bool ScreenshotFloatingToolPaletteWindow::isPointInInteractiveContent(
    const QPoint& localPosition) const {
    if (!rect().contains(localPosition)) {
        return false;
    }
    if (m_paletteHost == nullptr) {
        return true;
    }

    const QRegion interactiveRegion =
        m_paletteHost->interactiveHostRegion().translated(m_paletteHost->pos());
    return interactiveRegion.contains(localPosition);
}

void ScreenshotFloatingToolPaletteWindow::handlePaletteContentChange() {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.palette_content_changed");
    if (m_paletteHost == nullptr) {
        return;
    }

    if (!m_processingNativeDpiChange && !m_draggingPalette) {
        m_paletteHost->setLogicalClientExtent(QSize());
    }

    const QSize newHostSize = fixedWindowSizeHint();
    const QPoint newContentOffset = contentOffset();
    const QRect newMainRect = mainToolbarContentRect();
    const bool sizeChanged = newHostSize != m_lastAppliedHostSize;
    const bool offsetChanged = newContentOffset != m_lastAppliedContentOffset;
    const bool mainAnchorChanged = newMainRect != m_lastAppliedMainToolbarContentRect;
    if (!sizeChanged && !offsetChanged && !mainAnchorChanged) {
        return;
    }

    const bool restoreMainToolbarPosition =
        mainAnchorChanged && m_lastMainToolbarGlobalTopLeftValid && !m_draggingPalette;
    const QPoint previousMainToolbarGlobalTopLeft = m_lastMainToolbarGlobalTopLeft;
    refreshGeometryForVisibleContent(true);
    if (restoreMainToolbarPosition && !newMainRect.isEmpty()) {
        moveContentTo(previousMainToolbarGlobalTopLeft - newMainRect.topLeft(), size());
    }
    emit visibleContentChanged();
}

bool ScreenshotFloatingToolPaletteWindow::applyPlacementScreen() {
    if (m_placementScreen == nullptr) {
        return false;
    }

    QWindow* handle = windowHandle();
    if (handle == nullptr) {
        return false;
    }
    if (handle->screen() == m_placementScreen) {
        return false;
    }

    handle->setScreen(m_placementScreen);
    return true;
}

void ScreenshotFloatingToolPaletteWindow::refreshGeometryForVisibleContent(
    bool preserveContentPosition, bool forceRepaint) {
    if (m_geometryCommitActive) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("window.geometry_request_coalesced");
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_coalescedGeometryRequestCount;
#endif
        m_geometryUpdatePending = true;
        m_pendingPreserveContentPosition =
            m_pendingPreserveContentPosition || preserveContentPosition;
        m_pendingForceRepaint = m_pendingForceRepaint || forceRepaint;
        return;
    }

    const bool updatesWereEnabled = updatesEnabled();
    if (updatesWereEnabled) {
        setUpdatesEnabled(false);
    }
    bool repaintRequested = forceRepaint;
    bool committed = false;
    bool preserve = preserveContentPosition;
    do {
        m_geometryCommitActive = true;
        m_geometryUpdatePending = false;
        m_pendingPreserveContentPosition = false;
        m_pendingForceRepaint = false;
        committed = commitGeometryUpdate(preserve) || committed;
        m_geometryCommitActive = false;
        if (m_geometryUpdatePending) {
            preserve = m_pendingPreserveContentPosition;
            repaintRequested = repaintRequested || m_pendingForceRepaint;
        }
    } while (m_geometryUpdatePending);

    if (updatesWereEnabled) {
        setUpdatesEnabled(true);
        if ((committed || repaintRequested) && isVisible()) {
            if (repaintRequested) {
                repaint();
            } else {
                update();
            }
        }
    }
}

bool ScreenshotFloatingToolPaletteWindow::commitGeometryUpdate(bool preserveContentPosition) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.refresh_geometry");
    const bool hasExistingGeometry = geometry().isValid() && !geometry().isEmpty();
    const bool hasContentAnchor =
        preserveContentPosition && (m_lastRequestedContentPositionValid || hasExistingGeometry);
    const QPoint contentAnchor =
        m_lastRequestedContentPositionValid ? m_lastRequestedContentPosition : contentPosition();

    syncPalettePhysicalScale();
    updatePaletteGeometryForVisibleContent();
    const QSize windowSize = fixedWindowSizeHint();
    if (!windowSize.isValid() || windowSize.isEmpty()) {
        return false;
    }

    const qreal currentDpr = m_committedWindowDevicePixelRatio > 0.0
                                 ? m_committedWindowDevicePixelRatio
                                 : currentWindowDevicePixelRatio();
    const QPoint newContentOffset = contentOffset();
    const QRect newMainToolbarRect = mainToolbarContentRect();
    const bool changed =
        m_lastAppliedWindowDevicePixelRatio <= 0.0 ||
        !qFuzzyCompare(m_lastAppliedWindowDevicePixelRatio + 1.0, currentDpr + 1.0) ||
        m_lastAppliedHostSize != windowSize || m_lastAppliedContentOffset != newContentOffset ||
        m_lastAppliedMainToolbarContentRect != newMainToolbarRect;
    if (!changed) {
        return false;
    }

    SNOW_SHOT_TOOLBAR_PERF_COUNTER("window.geometry_committed");
#if defined(SNOW_SHOT_TEST_HOOKS)
    ++m_committedGeometryPassCount;
#endif

    if (hasContentAnchor && !m_processingNativeDpiChange) {
        m_lastRequestedContentPosition = contentAnchor;
        m_lastRequestedContentPositionValid = true;
        setGeometry(QRect(contentAnchor - contentOffset(), windowSize));
    } else if (!m_processingNativeDpiChange) {
        resize(windowSize);
    }
    if (m_processingNativeDpiChange) {
        m_lastRequestedContentPosition =
            m_draggingPalette && m_dragPhysicalAnchorValid
                ? m_dragContentPosition.toPoint()
                : contentPosition();
        m_lastRequestedContentPositionValid = true;
    }
    if (m_draggingPalette && hasContentAnchor) {
        m_dragContentPosition =
            QPointF(m_processingNativeDpiChange ? m_lastRequestedContentPosition : contentAnchor);
    }
    updateMainToolbarPositionSnapshot();
    m_lastAppliedWindowDevicePixelRatio = currentDpr;
    m_lastAppliedHostSize = windowSize;
    m_lastAppliedContentOffset = newContentOffset;
    m_lastAppliedMainToolbarContentRect = newMainToolbarRect;
#if defined(SNOW_SHOT_TEST_HOOKS)
    ++m_windowResizeOrReanchorCount;
#endif
    if (!m_processingNativeDpiChange) {
        refreshStablePhysicalWindowSize();
    }
    return true;
}

QRect ScreenshotFloatingToolPaletteWindow::nativeWindowGeometryForPhysicalDrag(
    const QPointF& physicalCursorPosition, const QPointF& physicalCursorToWindowOffset,
    const QSize& stablePhysicalWindowSize) {
    const QPoint topLeft(qRound(physicalCursorPosition.x() - physicalCursorToWindowOffset.x()),
                         qRound(physicalCursorPosition.y() - physicalCursorToWindowOffset.y()));
    return QRect(topLeft, stablePhysicalWindowSize);
}

void ScreenshotFloatingToolPaletteWindow::ensureReferenceDevicePixelRatio() {
    if (m_referenceDevicePixelRatio > 0.0) {
        return;
    }
    if (m_placementScreen != nullptr && m_placementScreen->devicePixelRatio() > 0.0) {
        m_referenceDevicePixelRatio = m_placementScreen->devicePixelRatio();
        return;
    }
    if (m_dpiController != nullptr && m_dpiController->hasBaseline()) {
        m_referenceDevicePixelRatio = m_dpiController->referenceDpr();
        return;
    }

    const qreal devicePixelRatio = targetDevicePixelRatio();
    m_referenceDevicePixelRatio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
}

void ScreenshotFloatingToolPaletteWindow::syncPalettePhysicalScale() {
    if (m_paletteHost == nullptr) {
        return;
    }

    ensureReferenceDevicePixelRatio();
    const qreal currentDpr = m_committedWindowDevicePixelRatio > 0.0
                                 ? m_committedWindowDevicePixelRatio
                                 : currentWindowDevicePixelRatio();
    const adqt::widgets::AdControlScaleContext context =
        adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(
            m_referenceDevicePixelRatio, currentDpr, m_paletteScaleMultiplier);
    if (m_scaleScope != nullptr) {
        m_scaleScope->publishScale(context);
    }
    m_paletteHost->setPhysicalScale(context.logicalScale);
    m_paletteHost->setShadowMargins(ScreenshotToolPaletteHost::defaultShadowMargins());
}

void ScreenshotFloatingToolPaletteWindow::refreshStablePhysicalWindowSize() {
    if (m_dpiController != nullptr) {
        m_dpiController->captureBaseline(targetDevicePixelRatio());
        m_stablePhysicalWindowSize = m_dpiController->stablePhysicalFrameSize();
        return;
    }
    QRect nativeGeometry;
    if (native::currentWindowGeometry(winId(), &nativeGeometry)) {
        m_stablePhysicalWindowSize = nativeGeometry.size();
        return;
    }

    const qreal devicePixelRatio = currentWindowDevicePixelRatio();
    const qreal scale = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    m_stablePhysicalWindowSize = QSize(std::max(1, qRound(size().width() * scale)),
                                       std::max(1, qRound(size().height() * scale)));
}

QRect ScreenshotFloatingToolPaletteWindow::mainToolbarContentRect() const {
    return m_paletteHost != nullptr ? m_paletteHost->mainToolbarContentRect() : QRect();
}

void ScreenshotFloatingToolPaletteWindow::updateMainToolbarPositionSnapshot() {
    const QRect mainRect = mainToolbarContentRect();
    if (mainRect.isEmpty()) {
        m_lastMainToolbarGlobalTopLeftValid = false;
        return;
    }

    const QPoint currentContentPosition =
        m_draggingPalette && m_dragPhysicalAnchorValid ? m_dragContentPosition.toPoint()
                                                       : contentPosition();
    m_lastMainToolbarGlobalTopLeft = currentContentPosition + mainRect.topLeft();
    m_lastMainToolbarGlobalTopLeftValid = true;
}

qreal ScreenshotFloatingToolPaletteWindow::currentWindowDevicePixelRatio() const {
    if (QWindow* handle = const_cast<ScreenshotFloatingToolPaletteWindow*>(this)->windowHandle()) {
        const qreal windowDpr = handle->devicePixelRatio();
        if (windowDpr > 0.0) {
            return windowDpr;
        }
    }

    const qreal widgetDpr = devicePixelRatioF();
    if (widgetDpr > 0.0) {
        return widgetDpr;
    }

    return 1.0;
}

qreal ScreenshotFloatingToolPaletteWindow::targetDevicePixelRatio() const {
    if (m_placementScreen != nullptr && m_placementScreen->devicePixelRatio() > 0.0) {
        return m_placementScreen->devicePixelRatio();
    }

    return currentWindowDevicePixelRatio();
}

void ScreenshotFloatingToolPaletteWindow::beginPaletteDrag(const QPoint& globalPosition) {
    if (m_paletteHost == nullptr) {
        return;
    }

    QPointF physicalPosition;
    if (native::currentPhysicalCursorPosition(&physicalPosition)) {
        beginPaletteDragAtPhysicalPosition(globalPosition, physicalPosition);
        return;
    }

    m_draggingPalette = true;
    m_lastDragPosition = dragPositionForEvent(globalPosition);
    m_dragPhysicalAnchorValid = false;
    m_dragContentPosition = QPointF(contentPosition());
    raise();
}

void ScreenshotFloatingToolPaletteWindow::beginPaletteDragAtPhysicalPosition(
    const QPoint& globalPosition, const QPointF& physicalPosition) {
    if (m_paletteHost == nullptr) {
        return;
    }

    m_draggingPalette = true;
    m_lastDragPosition = dragPositionForEvent(globalPosition, physicalPosition);
    if (m_dpiController != nullptr &&
        m_dpiController->captureBaseline(targetDevicePixelRatio())) {
        m_referenceDevicePixelRatio = m_dpiController->referenceDpr();
    }
    if (m_dpiController != nullptr && m_dpiController->beginPhysicalDrag(physicalPosition)) {
        m_dragPhysicalAnchorValid = true;
        m_stablePhysicalWindowSize = m_dpiController->stablePhysicalFrameSize();
        m_dragPhysicalCursorToWindowOffset = m_dpiController->physicalDragAnchor();
        m_lastPhysicalDragCursorPosition = physicalPosition;
        m_dragContentPosition = QPointF(contentPosition());
        raise();
        return;
    }
    QRect nativeWindowGeometry;
    m_dragPhysicalAnchorValid = native::currentWindowGeometry(winId(), &nativeWindowGeometry);
    if (m_dragPhysicalAnchorValid) {
        m_stablePhysicalWindowSize = nativeWindowGeometry.size();
        m_dragPhysicalCursorToWindowOffset =
            QPointF(physicalPosition.x() - nativeWindowGeometry.left(),
                    physicalPosition.y() - nativeWindowGeometry.top());
        m_lastPhysicalDragCursorPosition = physicalPosition;
    }
    m_dragContentPosition = QPointF(contentPosition());
    raise();
}

void ScreenshotFloatingToolPaletteWindow::updatePaletteDrag(const QPoint& globalPosition) {
    if (!m_draggingPalette) {
        return;
    }

    QPointF physicalPosition;
    if (native::currentPhysicalCursorPosition(&physicalPosition) &&
        updatePaletteDragAtPhysicalPosition(globalPosition, physicalPosition)) {
        return;
    }

    const QPointF dragPosition = dragPositionForEvent(globalPosition);
    const QPointF delta = dragPosition - m_lastDragPosition;
    m_lastDragPosition = dragPosition;
    if (qFuzzyIsNull(delta.x()) && qFuzzyIsNull(delta.y())) {
        return;
    }

    m_dragContentPosition += delta;
    if (m_movementClampingEnabled) {
        m_dragContentPosition = constrainedContentPosition(m_dragContentPosition);
    }
    moveContentDuringDrag(m_dragContentPosition.toPoint());
}

bool ScreenshotFloatingToolPaletteWindow::updatePaletteDragAtPhysicalPosition(
    const QPoint& globalPosition, const QPointF& physicalPosition) {
    if (!m_draggingPalette || !m_dragPhysicalAnchorValid || m_movementClampingEnabled) {
        return false;
    }

    const QPointF dragPosition = dragPositionForEvent(globalPosition, physicalPosition);
    m_dragContentPosition += dragPosition - m_lastDragPosition;
    m_lastDragPosition = dragPosition;
    m_lastPhysicalDragCursorPosition = physicalPosition;
    bool moved = m_dpiController != nullptr &&
                 m_dpiController->moveForPhysicalCursor(physicalPosition);
    if (!moved) {
        const QRect targetNativeGeometry = nativeWindowGeometryForPhysicalDrag(
            physicalPosition, m_dragPhysicalCursorToWindowOffset, m_stablePhysicalWindowSize);
        moved = native::moveWindowTo(winId(), targetNativeGeometry.topLeft());
    }
    if (!moved) {
        m_dragPhysicalAnchorValid = false;
        moveContentDuringDrag(m_dragContentPosition.toPoint());
        return true;
    }

    m_lastRequestedContentPosition = m_dragContentPosition.toPoint();
    m_lastRequestedContentPositionValid = true;
    updateMainToolbarPositionSnapshot();
    return true;
}

void ScreenshotFloatingToolPaletteWindow::moveContentDuringDrag(const QPoint& position) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.drag_move");
    const QPoint targetPosition =
        m_movementClampingEnabled ? constrainedContentPosition(position) : position;
    m_lastRequestedContentPosition = targetPosition;
    m_lastRequestedContentPositionValid = true;

    const QPoint targetWindowPosition = targetPosition - contentOffset();
    if (pos() != targetWindowPosition) {
        move(targetWindowPosition);
    }

    m_lastRequestedContentPosition = contentPosition();
    m_dragContentPosition = QPointF(m_lastRequestedContentPosition);
    updateMainToolbarPositionSnapshot();
}

void ScreenshotFloatingToolPaletteWindow::finishPaletteDrag(bool emitFinished) {
    if (!m_draggingPalette) {
        return;
    }

    m_draggingPalette = false;
    m_dragPhysicalAnchorValid = false;
    if (m_dpiController != nullptr) {
        m_dpiController->endPhysicalDrag();
    }
    if (m_paletteHost != nullptr) {
        m_paletteHost->cancelDrag();
    }
    if (emitFinished) {
        emit dragFinished();
    }
}

bool ScreenshotFloatingToolPaletteWindow::handleToolbarWheel(QWheelEvent* event) {
    return m_paletteHost != nullptr && m_paletteHost->handleToolbarWheel(event);
}

void ScreenshotFloatingToolPaletteWindow::applyWindowAttributes() {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    // This no-activate tool window must still dispatch tooltip events.
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
}

void ScreenshotFloatingToolPaletteWindow::prewarmScopeIcons(QWidget* scope) {
    if (scope == nullptr) {
        return;
    }
    QList<adqt::icons::IconPixmapRequest> requests;
    const qreal dpr = m_placementScreen != nullptr
                          ? m_placementScreen->devicePixelRatio()
                          : (screen() != nullptr ? screen()->devicePixelRatio() : 1.0);
    const QList<adqt::widgets::AdButton*> buttons =
        scope->findChildren<adqt::widgets::AdButton*>();
    for (const auto* button : buttons) {
        if (button == nullptr || !button->iconRef().isValid()) {
            continue;
        }
        adqt::icons::IconPixmapRequest request;
        request.ref = button->iconRef();
        request.render.logicalSize = button->iconSize();
        request.render.devicePixelRatio = dpr;
        requests.append(request);
    }
    adqt::icons::prewarm(requests);
}

void ScreenshotFloatingToolPaletteWindow::registerMaterializedScope(QWidget* scope) {
    if (scope == nullptr) {
        return;
    }
    if (auto* editor = scope->findChild<QLineEdit*>(
            QStringLiteral("screenshotWatermarkTextEdit"))) {
        if (m_watermarkTextEditor != editor) {
            m_watermarkTextEditor = editor;
            editor->installEventFilter(this);
            connect(editor, &QLineEdit::editingFinished, this, [this, editor]() {
                QTimer::singleShot(0, this,
                                   [this, editor]() { endKeyboardFocusInteraction(editor); });
            });
        }
    }
    for (adqt::widgets::AdSelect* select : scope->findChildren<adqt::widgets::AdSelect*>()) {
        if (select == nullptr || !select->searchEnabled() || select->lineEdit() == nullptr ||
            select->property("snowShotFloatingFocusRegistered").toBool()) {
            continue;
        }
        select->setProperty("snowShotFloatingFocusRegistered", true);
        connect(select, &adqt::widgets::AdSelect::popupOpening, this,
                [this, select]() { beginKeyboardFocusInteraction(select->lineEdit()); });
        connect(select, &adqt::widgets::AdSelect::popupVisibleChanged, this,
                [this, select](bool visible) {
                    if (!visible) {
                        endKeyboardFocusInteraction(select->lineEdit());
                    }
                });
    }
    for (adqt::widgets::AdButton* button : scope->findChildren<adqt::widgets::AdButton*>()) {
        if (button == nullptr ||
            button->property("snowShotDpiSurfaceRegistered").toBool()) {
            continue;
        }
        button->setProperty("snowShotDpiSurfaceRegistered", true);
        if (m_dpiController != nullptr && button->busyIndicatorSurface() != nullptr) {
            m_dpiController->registerAuxiliarySurface(button->busyIndicatorSurface());
        }
        connect(button, &adqt::widgets::AdButton::busyIndicatorSurfaceChanged, this,
                [this](QWidget* surface) {
                    if (m_dpiController != nullptr) {
                        m_dpiController->registerAuxiliarySurface(surface);
                    }
                });
    }
    prewarmScopeIcons(scope);
}

void ScreenshotFloatingToolPaletteWindow::beginKeyboardFocusInteraction(QWidget* editor) {
    if (editor == nullptr) {
        return;
    }

    m_keyboardFocusEditor = editor;
    m_keyboardFocusInteractionActive = true;
#if defined(Q_OS_WIN) || defined(_WIN32)
    static_cast<void>(native::setKeyboardFocusEnabled(winId(), true));
    static_cast<void>(native::activateWindow(winId()));
#else
    if (QWindow* handle = windowHandle()) {
        handle->setFlag(Qt::WindowDoesNotAcceptFocus, false);
        handle->requestActivate();
    }
#endif
    editor->setFocus(Qt::MouseFocusReason);
}

void ScreenshotFloatingToolPaletteWindow::endKeyboardFocusInteraction(QWidget* editor) {
    if (editor != nullptr && m_keyboardFocusEditor != editor) {
        return;
    }
    m_keyboardFocusEditor.clear();
    if (!m_keyboardFocusInteractionActive) {
        return;
    }

    m_keyboardFocusInteractionActive = false;
#if defined(Q_OS_WIN) || defined(_WIN32)
    static_cast<void>(native::setKeyboardFocusEnabled(winId(), false));
#else
    if (QWindow* handle = windowHandle()) {
        handle->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    }
#endif

    QWidget* owner =
        m_transientOwnerWindow != nullptr ? m_transientOwnerWindow.data() : parentWidget();
    if (owner != nullptr && owner->windowHandle() != nullptr && owner->isVisible()) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        static_cast<void>(native::activateWindow(owner->winId()));
#else
        owner->windowHandle()->requestActivate();
#endif
    }
}

QSize ScreenshotFloatingToolPaletteWindow::fixedWindowSizeHint() const {
    return m_paletteHost != nullptr ? m_paletteHost->hostSizeHint() : QSize();
}

QPoint ScreenshotFloatingToolPaletteWindow::contentOffset() const {
    return (m_panel != nullptr ? m_panel->pos() : QPoint()) +
           (m_paletteHost != nullptr ? m_paletteHost->contentOffset() : QPoint());
}

QPointF
ScreenshotFloatingToolPaletteWindow::dragPositionForEvent(const QPoint& globalPosition) const {
    QPointF physicalPosition;
    if (native::currentPhysicalCursorPosition(&physicalPosition)) {
        return dragPositionForEvent(globalPosition, physicalPosition);
    }

    return QPointF(globalPosition);
}

QPointF ScreenshotFloatingToolPaletteWindow::dragPositionForEvent(
    const QPoint& globalPosition, const QPointF& physicalPosition) const {
    return ScreenshotGeometryMapper::logicalDragPositionForPhysicalPoint(
        QPointF(globalPosition), physicalPosition, m_movementLogicalBounds,
        m_movementPhysicalBounds);
}
