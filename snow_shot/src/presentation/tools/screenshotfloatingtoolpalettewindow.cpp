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
#include <QCoreApplication>
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

ScreenshotFloatingToolPaletteWindow::GeometryUpdateTransaction::GeometryUpdateTransaction(
    ScreenshotFloatingToolPaletteWindow& owner)
    : m_owner(owner) {
    m_owner.beginGeometryUpdate();
}

ScreenshotFloatingToolPaletteWindow::GeometryUpdateTransaction::~GeometryUpdateTransaction() {
    m_owner.endGeometryUpdate();
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

    bindDynamicKeyboardEditors();

    refreshGeometryForVisibleContent(false);

    QList<adqt::icons::IconPixmapRequest> iconWarmupRequests;
    const QList<adqt::widgets::AdButton*> buttons =
        m_paletteHost->findChildren<adqt::widgets::AdButton*>();
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (const adqt::widgets::AdButton* button : buttons) {
        if (button == nullptr || !button->iconRef().isValid()) {
            continue;
        }
        for (const QScreen* screen : screens) {
            adqt::icons::IconPixmapRequest request;
            request.ref = button->iconRef();
            request.render.logicalSize = button->iconSize();
            request.render.devicePixelRatio = screen != nullptr ? screen->devicePixelRatio() : 1.0;
            iconWarmupRequests.append(request);
        }
    }
    adqt::icons::prewarm(iconWarmupRequests);

    m_scaleScope = new adqt::widgets::AdControlScaleScope(m_paletteHost, this);
    m_dpiController = new adqt::widgets::AdDpiStableWindowController(this, this);
    m_dpiController->setScaleScope(m_scaleScope);
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
    connect(
        m_dpiController, &adqt::widgets::AdDpiStableWindowController::scaleCommitCompleted, this,
        [this](const adqt::widgets::AdControlScaleContext& context,
               const QSize& logicalClientExtent) {
            m_processingNativeDpiChange = true;
            if (m_paletteHost != nullptr) {
                m_paletteHost->commitDpiScale(context.logicalScale * m_paletteScaleMultiplier,
                                              logicalClientExtent,
                                              ScreenshotToolPaletteHost::defaultShadowMargins());
            }
            // requestGeometryUpdate() synchronously prepares the host once. A
            // second preparation and explicit activation of this top-level
            // layout used to make Qt reapply its pre-transition logical
            // geometry while the native controller was preserving the new
            // physical frame, producing a resize/move fight on mixed-DPI
            // monitor returns.
            requestGeometryUpdate(true, true);
            const QList<QWidget*> scaledWidgets = findChildren<QWidget*>();
            for (QWidget* widget : scaledWidgets) {
                QCoreApplication::removePostedEvents(widget, QEvent::LayoutRequest);
            }
            QCoreApplication::removePostedEvents(this, QEvent::LayoutRequest);
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

void ScreenshotFloatingToolPaletteWindow::bindDynamicKeyboardEditors() {
    if (m_paletteHost == nullptr) {
        return;
    }

    QLineEdit* watermarkTextEditor =
        m_paletteHost->findChild<QLineEdit*>(QStringLiteral("screenshotWatermarkTextEdit"));
    if (watermarkTextEditor != nullptr && m_watermarkTextEditor != watermarkTextEditor) {
        m_watermarkTextEditor = watermarkTextEditor;
        watermarkTextEditor->installEventFilter(this);
        connect(watermarkTextEditor, &QLineEdit::editingFinished, this, [this]() {
            const QPointer<QWidget> editor = m_watermarkTextEditor;
            QTimer::singleShot(0, this, [this, editor]() { endKeyboardFocusInteraction(editor); });
        });
    }

    const QList<adqt::widgets::AdSelect*> selects =
        m_paletteHost->findChildren<adqt::widgets::AdSelect*>();
    for (adqt::widgets::AdSelect* select : selects) {
        if (select == nullptr || !select->searchEnabled() || select->lineEdit() == nullptr ||
            select->property("snowShotKeyboardBindingInstalled").toBool()) {
            continue;
        }
        select->setProperty("snowShotKeyboardBindingInstalled", true);
        connect(select, &adqt::widgets::AdSelect::popupOpening, this,
                [this, select]() { beginKeyboardFocusInteraction(select->lineEdit()); });
        connect(select, &adqt::widgets::AdSelect::popupVisibleChanged, this,
                [this, select](bool visible) {
                    if (!visible) {
                        endKeyboardFocusInteraction(select->lineEdit());
                    }
                });
    }
}

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

void ScreenshotFloatingToolPaletteWindow::releaseNativeSurface() {
    cancelDrag();
    hide();
    setUpdatesEnabled(false);
    destroy(true, true);
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
    GeometryUpdateTransaction transaction(*this);
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
    bindDynamicKeyboardEditors();
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
    m_stablePhysicalWindowSize = QSize();
    m_geometrySnapshotValid = false;
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

    // Secondary editors are materialized on demand. Rebind keyboard-capable
    // controls whenever the palette publishes a new content tree so editors
    // created after construction receive the same native focus hand-off as
    // controls present in the initial palette.
    bindDynamicKeyboardEditors();

    if (!m_processingNativeDpiChange) {
        const QSize constrainedSize = fixedWindowSizeHint();
        const QSize naturalSize = m_paletteHost->unconstrainedHostSizeHint();
        const bool naturalContentFits =
            constrainedSize.isValid() && naturalSize.isValid() &&
            naturalSize.width() <= constrainedSize.width() &&
            naturalSize.height() <= constrainedSize.height();
        const ScreenshotToolPalette* palette = m_paletteHost->palette();
        const bool reusableSecondaryRowsExist =
            palette != nullptr && palette->stylePanel() != nullptr;
        if (!reusableSecondaryRowsExist || !naturalContentFits) {
            m_paletteHost->setLogicalClientExtent(QSize());
        }
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
    GeometryUpdateTransaction transaction(*this);
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

    GeometryUpdateTransaction transaction(*this);
    handle->setScreen(m_placementScreen);
    return true;
}

void ScreenshotFloatingToolPaletteWindow::refreshGeometryForVisibleContent(
    bool preserveContentPosition, bool forceRepaint) {
    requestGeometryUpdate(preserveContentPosition, forceRepaint);
}

void ScreenshotFloatingToolPaletteWindow::beginGeometryUpdate() {
    ++m_geometryUpdateDepth;
}

void ScreenshotFloatingToolPaletteWindow::endGeometryUpdate() {
    Q_ASSERT(m_geometryUpdateDepth > 0);
    --m_geometryUpdateDepth;
    if (m_geometryUpdateDepth == 0) {
        drainGeometryUpdates();
    }
}

void ScreenshotFloatingToolPaletteWindow::requestGeometryUpdate(bool preserveContentPosition,
                                                                bool forceRepaint) {
    if (m_geometryUpdatePending || m_geometryUpdateDepth > 0 || m_drainingGeometryUpdates) {
        SNOW_SHOT_TOOLBAR_PERF_COUNTER("window.geometry_request_coalesced");
#if defined(SNOW_SHOT_TEST_HOOKS)
        ++m_coalescedGeometryRequestCount;
#endif
    }
    m_geometryUpdatePending = true;
    m_pendingPreserveContentPosition = m_pendingPreserveContentPosition || preserveContentPosition;
    m_pendingForceRepaint = m_pendingForceRepaint || forceRepaint;
    if (m_geometryUpdateDepth == 0 && !m_drainingGeometryUpdates) {
        drainGeometryUpdates();
    }
}

ScreenshotFloatingToolPaletteWindow::GeometrySnapshot
ScreenshotFloatingToolPaletteWindow::geometrySnapshot() const {
    GeometrySnapshot snapshot;
    QWindow* handle = const_cast<ScreenshotFloatingToolPaletteWindow*>(this)->windowHandle();
    snapshot.screen = handle != nullptr ? handle->screen() : m_placementScreen.data();
    snapshot.devicePixelRatio = currentWindowDevicePixelRatio();
    snapshot.paletteRevision = m_paletteHost != nullptr ? m_paletteHost->layoutRevision() : 0;
    snapshot.hostSize = fixedWindowSizeHint();
    snapshot.windowSize = size();
    snapshot.contentOffset = contentOffset();
    snapshot.mainToolbarRect = mainToolbarContentRect();
    return snapshot;
}

bool ScreenshotFloatingToolPaletteWindow::geometrySnapshotsEqual(const GeometrySnapshot& lhs,
                                                                 const GeometrySnapshot& rhs) {
    return lhs.screen == rhs.screen && lhs.devicePixelRatio == rhs.devicePixelRatio &&
           lhs.paletteRevision == rhs.paletteRevision && lhs.hostSize == rhs.hostSize &&
           lhs.windowSize == rhs.windowSize && lhs.contentOffset == rhs.contentOffset &&
           lhs.mainToolbarRect == rhs.mainToolbarRect;
}

void ScreenshotFloatingToolPaletteWindow::drainGeometryUpdates() {
    if (m_drainingGeometryUpdates || m_geometryUpdateDepth > 0) {
        return;
    }

    m_drainingGeometryUpdates = true;
    const bool updatesWereEnabled = updatesEnabled();
    if (updatesWereEnabled) {
        setUpdatesEnabled(false);
    }
    bool forceRepaint = false;
    bool committed = false;
    while (m_geometryUpdatePending) {
        const bool preserveContentPosition = m_pendingPreserveContentPosition;
        forceRepaint = forceRepaint || m_pendingForceRepaint;
        m_geometryUpdatePending = false;
        m_pendingPreserveContentPosition = false;
        m_pendingForceRepaint = false;
        committed = commitGeometryUpdate(preserveContentPosition) || committed;
    }
    m_drainingGeometryUpdates = false;
    if (updatesWereEnabled) {
        setUpdatesEnabled(true);
        if ((committed || forceRepaint) && isVisible()) {
            if (forceRepaint) {
                repaint();
            } else {
                update();
            }
        }
    }
}

bool ScreenshotFloatingToolPaletteWindow::commitGeometryUpdate(bool preserveContentPosition) {
    SNOW_SHOT_TOOLBAR_PERF_SCOPE("window.refresh_geometry");
    const GeometrySnapshot input = geometrySnapshot();
    if (m_geometrySnapshotValid && geometrySnapshotsEqual(input, m_lastGeometrySnapshot)) {
        return false;
    }

    SNOW_SHOT_TOOLBAR_PERF_COUNTER("window.geometry_committed");
#if defined(SNOW_SHOT_TEST_HOOKS)
    ++m_committedGeometryPassCount;
#endif
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

    if (hasContentAnchor && !m_processingNativeDpiChange) {
        m_lastRequestedContentPosition = contentAnchor;
        m_lastRequestedContentPositionValid = true;
        setGeometry(QRect(contentAnchor - contentOffset(), windowSize));
    } else if (!m_processingNativeDpiChange) {
        resize(windowSize);
    }
    if (m_processingNativeDpiChange && !m_draggingPalette) {
        m_lastRequestedContentPosition = contentPosition();
        m_lastRequestedContentPositionValid = true;
    }
    if (m_draggingPalette && hasContentAnchor) {
        m_dragContentPosition =
            QPointF(m_processingNativeDpiChange ? m_lastRequestedContentPosition : contentAnchor);
    }
    updateMainToolbarPositionSnapshot();
    m_lastAppliedWindowDevicePixelRatio = currentWindowDevicePixelRatio();
    m_lastAppliedHostSize = windowSize;
    m_lastAppliedContentOffset = contentOffset();
    m_lastAppliedMainToolbarContentRect = mainToolbarContentRect();
#if defined(SNOW_SHOT_TEST_HOOKS)
    ++m_windowResizeOrReanchorCount;
#endif
    if (!m_processingNativeDpiChange) {
        refreshStablePhysicalWindowSize();
    }
    m_lastGeometrySnapshot = geometrySnapshot();
    m_geometrySnapshotValid = true;
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
    if (m_dpiController != nullptr && m_dpiController->hasBaseline()) {
        m_referenceDevicePixelRatio = m_dpiController->referenceDpr();
        return;
    }
    if (m_referenceDevicePixelRatio > 0.0) {
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
    const qreal currentDpr = currentWindowDevicePixelRatio();
    const qreal dpiScale = currentDpr > 0.0 ? m_referenceDevicePixelRatio / currentDpr : 1.0;
    const qreal scale = dpiScale * m_paletteScaleMultiplier;
    m_paletteHost->setPhysicalScale(scale);
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
    updateMainToolbarPositionSnapshot(contentPosition());
}

void ScreenshotFloatingToolPaletteWindow::updateMainToolbarPositionSnapshot(
    const QPoint& contentPosition) {
    const QRect mainRect = mainToolbarContentRect();
    if (mainRect.isEmpty()) {
        m_lastMainToolbarGlobalTopLeftValid = false;
        return;
    }

    m_lastMainToolbarGlobalTopLeft = contentPosition + mainRect.topLeft();
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

    m_draggingPalette = true;
    m_lastDragPosition = dragPositionForEvent(globalPosition);
    if (m_dpiController != nullptr && m_dpiController->beginPhysicalDrag()) {
        m_dragPhysicalAnchorValid = true;
        m_stablePhysicalWindowSize = m_dpiController->stablePhysicalFrameSize();
        m_dragPhysicalCursorToWindowOffset = m_dpiController->physicalDragAnchor();
        const bool cursorCaptured =
            native::currentPhysicalCursorPosition(&m_lastPhysicalDragCursorPosition);
        Q_UNUSED(cursorCaptured);
        m_dragContentPosition = QPointF(contentPosition());
        raise();
        return;
    }
    QPointF physicalPosition;
    QRect nativeWindowGeometry;
    m_dragPhysicalAnchorValid = native::currentPhysicalCursorPosition(&physicalPosition) &&
                                native::currentWindowGeometry(winId(), &nativeWindowGeometry);
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
    if (m_dragPhysicalAnchorValid && !m_movementClampingEnabled &&
        native::currentPhysicalCursorPosition(&physicalPosition)) {
        m_lastPhysicalDragCursorPosition = physicalPosition;
        if (m_dpiController != nullptr &&
            m_dpiController->moveForPhysicalCursor(physicalPosition)) {
            const QPointF dragPosition = dragPositionForEvent(globalPosition);
            m_dragContentPosition += dragPosition - m_lastDragPosition;
            m_lastDragPosition = dragPosition;
            m_lastRequestedContentPosition = m_dragContentPosition.toPoint();
            m_lastRequestedContentPositionValid = true;
            updateMainToolbarPositionSnapshot(m_lastRequestedContentPosition);
            return;
        }
        const QRect targetNativeGeometry = nativeWindowGeometryForPhysicalDrag(
            physicalPosition, m_dragPhysicalCursorToWindowOffset, m_stablePhysicalWindowSize);
        if (native::moveWindowTo(winId(), targetNativeGeometry.topLeft())) {
            const QPointF dragPosition = dragPositionForEvent(globalPosition);
            m_dragContentPosition += dragPosition - m_lastDragPosition;
            m_lastDragPosition = dragPosition;
            m_lastRequestedContentPosition = m_dragContentPosition.toPoint();
            m_lastRequestedContentPositionValid = true;
            updateMainToolbarPositionSnapshot(m_lastRequestedContentPosition);
            return;
        }
    }
    if (m_dragPhysicalAnchorValid) {
        m_dragPhysicalAnchorValid = false;
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
    if (native::currentPhysicalCursorPosition(&physicalPosition) &&
        m_movementLogicalBounds.isValid() && m_movementPhysicalBounds.isValid()) {
        return ScreenshotGeometryMapper::logicalDragPositionForPhysicalPoint(
            QPointF(globalPosition), physicalPosition, m_movementLogicalBounds,
            m_movementPhysicalBounds);
    }

    return QPointF(globalPosition);
}
