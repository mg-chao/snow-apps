#include "snow_shot/presentation/windowcloseshortcut.h"
#include "snow_shot/presentation/screenrecordingtoolbarwindow.h"

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/applicationstorage.h"
#include "screenrecordinggeometry.h"
#include "screenrecordingperfinstrumentation.h"
#include "screenrecordingtoolbarautohide.h"
#include "../tools/screenshotfloatingtoolpalettenative.h"
#include "theme/theme_manager.h"
#include "widgets/detail/top_level_popup_window.h"
#include "widgets/popup_interaction_host.h"

#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#endif
#ifdef Q_OS_WIN
#include "../../platform/windows/captureexclusioncomposition.h"
#endif

#include <QScreen>
#include <QGuiApplication>
#include <QCloseEvent>
#include "widgets/color_picker.h"
#include "widgets/select.h"
#include <QScopedValueRollback>
#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QShowEvent>
#include <QScopeGuard>
#include <QTimer>
#include <QWindow>
#include <QHash>

namespace {
constexpr int kToolbarGap = 4;
namespace auto_hide = snow_shot::presentation::recording::toolbar_auto_hide;

class RecordingToolbarEdgeControl final : public QAbstractButton {
  public:
    RecordingToolbarEdgeControl() : QAbstractButton(nullptr) {
        setWindowFlags(screenshot_floating_palette_native::windowFlags());
        setObjectName(QStringLiteral("recordingToolbarAutoHideControl"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
#ifdef Q_OS_WIN
        snow_shot::platform::windows::detail::enableCaptureExclusionCompositionFallback(this);
#endif
#ifdef Q_OS_MACOS
        setAttribute(Qt::WA_MacAlwaysShowToolWindow);
        snow_shot::platform::configureScreenRecordingToolbarWindow(this);
#endif
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        retranslate();
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] { update(); });
    }

    void setSide(auto_hide::Side side) {
        if (m_side != side) {
            m_side = side;
            update();
        }
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::LanguageChange)
            retranslate();
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
            event->type() == QEvent::PaletteChange)
            update();
        // Hover reveals the toolbar before a tooltip can be useful. Avoid a
        // detached native tooltip surviving the control's disappearance.
        if (event->type() == QEvent::ToolTip)
            return true;
        return QAbstractButton::event(event);
    }

    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        adqt::widgets::detail::constrainTopLevelToolStackingToOwner(this, message);
        return QAbstractButton::nativeEvent(type, message, result);
    }

    void paintEvent(QPaintEvent*) override {
        const auto colors = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const qreal scale = width() / 20.0;
        const qreal radius = std::min<qreal>(
            adqt::widgets::scaleControlMetric(screenshot_action_toolbar::PanelRadius, scale),
            height() / 2.0);
        const QRectF body = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath surface;
        if (m_side == auto_hide::Side::Left) {
            surface.moveTo(body.topLeft());
            surface.lineTo(body.right() - radius, body.top());
            surface.quadTo(body.topRight(), QPointF(body.right(), body.top() + radius));
            surface.lineTo(body.right(), body.bottom() - radius);
            surface.quadTo(body.bottomRight(), QPointF(body.right() - radius, body.bottom()));
            surface.lineTo(body.bottomLeft());
        } else {
            surface.moveTo(body.topRight());
            surface.lineTo(body.left() + radius, body.top());
            surface.quadTo(body.topLeft(), QPointF(body.left(), body.top() + radius));
            surface.lineTo(body.left(), body.bottom() - radius);
            surface.quadTo(body.bottomLeft(), QPointF(body.left() + radius, body.bottom()));
            surface.lineTo(body.bottomRight());
        }
        surface.closeSubpath();
        painter.setBrush(isDown()       ? colors.colorFillSecondary
                         : underMouse() ? colors.colorPrimaryBg
                                        : colors.colorBgContainer);
        painter.setPen(QPen(underMouse() ? colors.colorPrimary : colors.colorBorderSecondary, 1));
        painter.drawPath(surface);
        const QPointF center(width() / 2.0, height() / 2.0);
        const qreal direction = m_side == auto_hide::Side::Left ? 1.0 : -1.0;
        QPainterPath chevron;
        chevron.moveTo(center + QPointF(-direction * 2 * scale, -4 * scale));
        chevron.lineTo(center + QPointF(direction * 2 * scale, 0));
        chevron.lineTo(center + QPointF(-direction * 2 * scale, 4 * scale));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(underMouse() ? colors.colorPrimary : colors.colorTextSecondary,
                            1.7 * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(chevron);
    }

  private:
    void retranslate() {
        const QString text =
            QCoreApplication::translate("ScreenRecordingToolbarWindow", "Show recording toolbar");
        setToolTip(text);
        setAccessibleName(text);
    }
    auto_hide::Side m_side = auto_hide::Side::Right;
};

ScreenshotToolPalette::Options recordingToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showSpeechBubbleTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showDistanceTool = true;
    options.showAngleTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showPenHighlightTool = true;
    options.showSpotlightTool = true;
    // Magnifiers sample captured images; recording annotations have no image source.
    options.showMagnifierTool = false;
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
    options.actionToolsLayoutKind =
        snow_shot::storage::ScreenshotToolbarLayoutKind::RecordingActionTools;
    options.actionToolsLayout = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::RecordingActionTools);
    options.styleDefaults = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
    return options;
}
} // namespace

class ScreenRecordingToolbarWindow::AutoHide final : public QObject {
  public:
    explicit AutoHide(ScreenRecordingToolbarWindow& toolbar) : QObject(&toolbar), toolbar(toolbar) {
        reveal.setObjectName(QStringLiteral("recordingToolbarRevealTimer"));
        retreat.setObjectName(QStringLiteral("recordingToolbarRetreatTimer"));
        reveal.setSingleShot(true);
        reveal.setInterval(100);
        retreat.setSingleShot(true);
        retreat.setInterval(200);
        watch.setInterval(50);
        connect(&reveal, &QTimer::timeout, this, [this] {
            if (collapsed && control && control->geometry().contains(cursor()))
                revealToolbar();
        });
        connect(&retreat, &QTimer::timeout, this, [this] { collapseToolbar(true); });
        connect(&watch, &QTimer::timeout, this, [this] { reconcile(); });
        // Timers belong to this state object and are discoverable in offscreen
        // fixtures without sleeping or depending on the desktop cursor.
        reveal.setParent(this);
        retreat.setParent(this);
        watch.setParent(this);
        qApp->installEventFilter(this);
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen* addedScreen) {
            observeScreen(addedScreen);
            queueReconcile();
        });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] { queueReconcile(); });
        for (QScreen* observedScreen : QGuiApplication::screens())
            observeScreen(observedScreen);
        enabled = snow_shot::storage::RecordingSettings().autoHideToolbar();
    }

    ~AutoHide() override {
        qApp->removeEventFilter(this);
    }

    QWidget* materialize() {
        if (!control) {
            control = std::make_unique<RecordingToolbarEdgeControl>();
            connect(control.get(), &QAbstractButton::clicked, this, [this] { revealToolbar(); });
            setOwner(controlOwner);
        }
        // Keep the native identity for the whole recording UI session, including
        // when auto-hide is initially disabled and enabled after capture starts.
        static_cast<void>(control->winId());
        return control.get();
    }

    void setOwner(QWidget* owner) {
        controlOwner = owner;
        if (!control)
            return;
        if (owner)
            static_cast<void>(owner->winId());
        static_cast<void>(control->winId());
        toolbar.applyTransientOwnerWindow(control.get(), owner);
    }

    void shown() {
        presented = true;
        if (!changingVisibility) {
            collapsed = false;
            pendingCollapse = false;
            if (control)
                control->hide();
        }
        queueReconcile();
    }

    void clearPresentation() {
        presented = false;
        collapsed = false;
        pendingCollapse = false;
        transfer = {};
        side = auto_hide::Side::None;
        screen.clear();
        reveal.stop();
        retreat.stop();
        watch.stop();
        if (control)
            control->hide();
    }

    void setEnabled(bool value) {
        enabled = value;
        if (!value) {
            pendingCollapse = false;
            reveal.stop();
            retreat.stop();
            watch.stop();
            if (collapsed)
                revealToolbar();
            transfer = {};
        }
        queueReconcile();
    }

    void setBlocked(QObject* owner, bool blocked) {
        if (!owner)
            return;
        if (blocked && !blockers.contains(owner)) {
            blockers.insert(owner, connect(owner, &QObject::destroyed, this, [this, owner] {
                                blockers.remove(owner);
                                queueReconcile();
                            }));
        } else if (!blocked && blockers.contains(owner)) {
            disconnect(blockers.take(owner));
        }
        if (blocked && collapsed)
            revealToolbar();
        reconcile();
    }

    void queueReconcile() {
        if (queued || !presented)
            return;
        queued = true;
        QTimer::singleShot(0, this, [this] {
            queued = false;
            reconcile();
        });
    }

    void reconcile(bool retryPending = false) {
        if (evaluating || toolbar.m_placing)
            return;
        const QPointer<AutoHide> alive(this);
        evaluating = true;
        const auto evaluation = qScopeGuard([alive] {
            if (alive)
                alive->evaluating = false;
        });
        if (!presented || toolbar.m_regionInteractionActive || !enabled) {
            reveal.stop();
            retreat.stop();
            watch.stop();
            return;
        }
        if (dragging) {
            pendingCollapse = false;
            reveal.stop();
            retreat.stop();
            watch.stop();
            return;
        }
        if (!updateDock()) {
            pendingCollapse = false;
            retreat.stop();
            watch.stop();
            if (collapsed)
                revealToolbar();
            transfer = {};
            return;
        }
        if (collapsed) {
            if (retryPending && showGuard) {
                const auto guard = showGuard;
                const bool allowed = guard(control.get());
                if (!alive || !presented || !enabled || !collapsed)
                    return;
                if (!allowed) {
                    revealToolbar();
                    if (!alive)
                        return;
                    pendingCollapse = true;
                    watch.start();
                    return;
                }
            }
            watch.stop();
            if (control->geometry().contains(cursor())) {
                if (!reveal.isActive())
                    reveal.start();
            } else {
                reveal.stop();
            }
            return;
        }
        // Only watch while an expanded toolbar is docked. This also catches
        // pointer movement into another application and popup completion there.
        if (!watch.isActive())
            watch.start();
        if (protectedInteraction()) {
            pendingCollapse = false;
            retreat.stop();
        } else if (pendingCollapse) {
            if (retryPending)
                collapseToolbar(false);
        } else if (!retreat.isActive()) {
            retreat.start();
        }
    }

    void collapseToolbar(bool newRequest) {
        if (!enabled || !presented || collapsed || toolbar.m_regionInteractionActive ||
            toolbar.m_placing || !updateDock() || protectedInteraction()) {
            pendingCollapse = false;
            return;
        }
        syncPlacementContext();
        if (!updateDock() || protectedInteraction())
            return;
        QWidget* surface = materialize();
        control->setSide(side);
        if (control->windowHandle()->screen() != screen)
            control->windowHandle()->setScreen(screen);
        control->setGeometry(tabRect);
        const WId preparedIdentity = control->winId();
        pendingCollapse = true;
        const QPointer<AutoHide> alive(this);
        if (newRequest)
            emit toolbar.autoHideControlRequested(surface);
        if (!alive)
            return;
        const auto guard = showGuard;
        if (guard && !guard(surface))
            return;
        if (!alive)
            return;
        // A synchronous guard/request handler may dismiss or reconfigure us.
        if (!presented || !enabled || !pendingCollapse || protectedInteraction())
            return;
        const bool wasChangingVisibility = changingVisibility;
        changingVisibility = true;
        const auto transition = qScopeGuard([alive, wasChangingVisibility] {
            if (alive)
                alive->changingVisibility = wasChangingVisibility;
        });
        control->show();
        if (!alive)
            return;
        if (guard && control->winId() != preparedIdentity) {
            const bool allowed = guard(surface);
            if (!alive || !presented || !enabled || !pendingCollapse)
                return;
            if (!allowed) {
                control->hide();
                return;
            }
        }
        control->raise();
        if (!control->isVisible())
            return;
        transfer = auto_hide::transferRegion(rowRect, tabRect, side);
        collapsed = true;
        pendingCollapse = false;
        retreat.stop();
        watch.stop();
        toolbar.ScreenshotFloatingToolPaletteWindow::hide();
        reconcile();
    }

    void revealToolbar() {
        if (!collapsed || !presented || toolbar.m_regionInteractionActive)
            return;
        const QScopedValueRollback<bool> transition(changingVisibility, true);
        reveal.stop();
        pendingCollapse = false;
        collapsed = false;
        control->hide();
        syncPlacementContext();
        toolbar.showWithoutActivating();
        queueReconcile();
    }

    bool protectedInteraction() {
        const QPoint point = cursor();
        if (toolbar.containsInteractiveGlobalPoint(point)) {
            transfer = {};
            return true;
        }
        if (transfer.contains(point))
            return true;
        transfer = {};
        QWidget* focus = QApplication::focusWidget();
        return dragging || !blockers.isEmpty() || QWidget::mouseGrabber() ||
               QApplication::activeModalWidget() ||
               QGuiApplication::mouseButtons() != Qt::NoButton ||
               (focus && (focus == &toolbar || toolbar.isAncestorOf(focus)) &&
                focus->testAttribute(Qt::WA_InputMethodEnabled)) ||
               adqt::widgets::detail::popupInteractionActive(&toolbar);
    }

    bool updateDock() {
        rowRect =
            toolbar.paletteHost()->mainToolbarContentRect().translated(toolbar.contentPosition());
        QScreen* selected = nullptr;
        qint64 bestArea = 0;
        for (QScreen* candidate : QGuiApplication::screens()) {
            const QRect intersection = rowRect.intersected(candidate->geometry());
            const qint64 area = qint64(intersection.width()) * intersection.height();
            if (area > bestArea || (area > 0 && area == bestArea && candidate == screen)) {
                selected = candidate;
                bestArea = area;
            }
        }
        if (!selected) {
            selected = screen ? screen.data() : QGuiApplication::primaryScreen();
            if (!selected)
                return false;
            // A removed/rearranged monitor can leave both presentations outside
            // the desktop. Recover the complete toolbar on the surviving screen.
            toolbar.setPlacementContext(selected, selected->geometry(),
                                        ScreenshotGeometryMapper::physicalRectForScreen(*selected));
            toolbar.moveContentTo(toolbar.constrainedContentPosition(toolbar.contentPosition()));
            rowRect = toolbar.paletteHost()->mainToolbarContentRect().translated(
                toolbar.contentPosition());
        }
        screen = selected;
        side = auto_hide::sideForRow(rowRect, screen->geometry(), side);
        tabRect = auto_hide::tabGeometry(rowRect, screen->geometry(), side,
                                         toolbar.paletteHost()->physicalScale());
        if (collapsed && control && side != auto_hide::Side::None) {
            control->setSide(side);
            control->setGeometry(tabRect);
            transfer = auto_hide::transferRegion(rowRect, tabRect, side);
        }
        return side != auto_hide::Side::None;
    }

    void syncPlacementContext() {
        if (!screen)
            return;
        // Native drag commits retain the capture display's physical baseline.
        // Adopt the new display without resetting that baseline on hover reveal.
        const QScopedValueRollback<bool> placing(toolbar.m_placing, true);
        toolbar.setPlacementContext(screen, screen->geometry(),
                                    ScreenshotGeometryMapper::physicalRectForScreen(*screen), true);
        toolbar.prepareForDisplay();
    }

    QPoint cursor() const {
#if defined(SNOW_SHOT_TEST_HOOKS)
        if (toolbar.m_autoHideCursorPositionOverride)
            return *toolbar.m_autoHideCursorPositionOverride;
#endif
        return QCursor::pos();
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == control.get() && event->type() == QEvent::WinIdChange)
            toolbar.applyTransientOwnerWindow(control.get(), controlOwner);
        if (watched == control.get() && collapsed && !changingVisibility &&
            (event->type() == QEvent::WinIdChange || event->type() == QEvent::Hide)) {
            // A replaced native surface has no inherited capture approval.
            // Retire it immediately; the next collapse must pass the guard again.
            revealToolbar();
        }
        if (enabled && presented) {
            switch (event->type()) {
            case QEvent::Enter:
            case QEvent::Leave:
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::FocusIn:
            case QEvent::FocusOut:
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::DevicePixelRatioChange:
                queueReconcile();
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(watched, event);
    }

    void observeScreen(QScreen* observed) {
        connect(observed, &QScreen::geometryChanged, this, [this] { queueReconcile(); });
        connect(observed, &QScreen::logicalDotsPerInchChanged, this, [this] { queueReconcile(); });
    }

    ScreenRecordingToolbarWindow& toolbar;
    std::unique_ptr<RecordingToolbarEdgeControl> control;
    QPointer<QWidget> controlOwner;
    std::function<bool(QWidget*)> showGuard;
    QHash<QObject*, QMetaObject::Connection> blockers;
    QPointer<QScreen> screen;
    QTimer reveal;
    QTimer retreat;
    QTimer watch;
    QRect rowRect;
    QRect tabRect;
    QRegion transfer;
    auto_hide::Side side = auto_hide::Side::None;
    bool enabled = false;
    bool presented = false;
    bool collapsed = false;
    bool pendingCollapse = false;
    bool dragging = false;
    bool changingVisibility = false;
    bool evaluating = false;
    bool queued = false;
};

ScreenRecordingToolbarWindow::ScreenRecordingToolbarWindow(QWidget* parent)
    : ScreenshotFloatingToolPaletteWindow(recordingToolbarOptions(), parent) {
#ifdef Q_OS_WIN
    snow_shot::platform::windows::detail::enableCaptureExclusionCompositionFallback(this);
#endif
    snow_shot::presentation::installWindowCloseShortcut(this, [this] { close(); });
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_DeleteOnClose, false);
    prepareForDisplay();
#ifdef Q_OS_MACOS
    snow_shot::platform::configureScreenRecordingToolbarWindow(this);
#endif
    m_autoHide = std::make_unique<AutoHide>(*this);
    // Secondary rows can grow without changing the fixed native frame or the
    // main-row anchor. Observe the committed host content, including those cases.
    connect(paletteHost(), &ScreenshotToolPaletteHost::visibleContentChanged, this, [this]() {
        if (!m_manuallyDragged && !m_regionInteractionActive) {
            placeForRecordingRegion(m_recordingRegion);
        }
        m_autoHide->queueReconcile();
    });
    connect(paletteHost(), &ScreenshotToolPaletteHost::dragStarted, this, [this](const QPoint&) {
        m_manuallyDragged = true;
        m_autoHide->dragging = true;
        m_autoHide->reconcile();
    });
    connect(this, &ScreenshotFloatingToolPaletteWindow::dragFinished, this, [this] {
        m_autoHide->dragging = false;
        m_autoHide->queueReconcile();
    });
    connect(paletteHost(), &ScreenshotToolPaletteHost::dragCancelled, this, [this] {
        m_autoHide->dragging = false;
        m_autoHide->queueReconcile();
    });
    connect(this, &ScreenshotFloatingToolPaletteWindow::dpiScaleCommitCompleted, this,
            [this] { m_autoHide->queueReconcile(); });
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        connect(
            &storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
                if (key == QStringLiteral("screen_recording/action_tools_layout")) {
                    palette()->setActionToolsLayout(
                        snow_shot::storage::ScreenshotToolbarSettings().layout(
                            snow_shot::storage::ScreenshotToolbarLayoutKind::RecordingActionTools));
                }
                if (key == QStringLiteral("screen_recording/auto_hide_toolbar")) {
                    m_autoHide->setEnabled(
                        snow_shot::storage::RecordingSettings().autoHideToolbar());
                }
            });
    }
}

ScreenRecordingToolbarWindow::~ScreenRecordingToolbarWindow() {
    dismissAutoHide();
    // State owns a parentless native window; destroy it before QWidget's children
    // and remove its application event filter while the toolbar is still intact.
    m_autoHide.reset();
}

QWidget* ScreenRecordingToolbarWindow::autoHideControlWindow() {
    return m_autoHide->materialize();
}

void ScreenRecordingToolbarWindow::setAutoHideControlOwner(QWidget* owner) {
    m_autoHide->setOwner(owner);
}

void ScreenRecordingToolbarWindow::setAutoHideControlShowGuard(
    std::function<bool(QWidget*)> guard) {
    m_autoHide->showGuard = std::move(guard);
}

void ScreenRecordingToolbarWindow::refreshAutoHideControl() {
    m_autoHide->reconcile(true);
}

void ScreenRecordingToolbarWindow::dismissAutoHide() {
    if (m_autoHide)
        m_autoHide->clearPresentation();
    ScreenshotFloatingToolPaletteWindow::hide();
}

void ScreenRecordingToolbarWindow::hide() {
    dismissAutoHide();
}

bool ScreenRecordingToolbarWindow::recordingUiPresented() const {
    return m_autoHide && m_autoHide->presented && !m_regionInteractionActive;
}

void ScreenRecordingToolbarWindow::setAutoHideBlocked(QObject* owner, bool blocked) {
    m_autoHide->setBlocked(owner, blocked);
}

void ScreenRecordingToolbarWindow::hideEvent(QHideEvent* event) {
    ScreenshotFloatingToolPaletteWindow::hideEvent(event);
    if (m_autoHide && !m_autoHide->changingVisibility)
        m_autoHide->clearPresentation();
}

void ScreenRecordingToolbarWindow::showEvent(QShowEvent* event) {
    ScreenshotFloatingToolPaletteWindow::showEvent(event);
    if (m_autoHide)
        m_autoHide->shown();
}

void ScreenRecordingToolbarWindow::showAndActivate() {
    if (m_regionInteractionActive) {
        return;
    }
    showWithoutActivating();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    SNOW_SHOT_RECORDING_PERF_MILESTONE("toolbar.show_and_activate_returned");
}

void ScreenRecordingToolbarWindow::showWithoutActivating() {
    if (m_regionInteractionActive) {
        return;
    }
    prepareForDisplay();
    // Showing/re-aligning the companion toolbar must not take focus from its area.
    // Keep keyboard activation available for explicit controls and text editors.
    const bool wasNonActivating = testAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    show();
    raise();
    setAttribute(Qt::WA_ShowWithoutActivating, wasNonActivating);
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
    m_autoHide->queueReconcile();
}

void ScreenRecordingToolbarWindow::beginRegionInteraction() {
    if (m_regionInteractionActive) {
        return;
    }
    m_regionInteractionActive = true;
    palette()->closeRecordingAudioGainPopovers();
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
    showWithoutActivating();
}

void ScreenRecordingToolbarWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit closeRequested();
}
