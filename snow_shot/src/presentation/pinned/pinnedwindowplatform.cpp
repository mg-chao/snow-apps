#include "pinnedwindowplatform.h"
#if defined(Q_OS_WIN)
#include "../../platform/windows/pinnedwindownative.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include <qt_windows.h>
#endif
#include <QCursor>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QTimer>
#include <QWindow>
#include <algorithm>

namespace snow_shot::presentation {
#if defined(Q_OS_MACOS)
QRect cocoaPinnedUsableGeometry(const QScreen& screen);
#endif
PinnedPlacement pinnedPlacement(const QRect& pixels, const QScreen& screen) {
    const qreal dpr = storage::pinnedGeometryScale(std::max(qreal(1), screen.devicePixelRatio()));
    return {screen.name(), screen.serialNumber(),
            QPointF(pixels.topLeft() - screen.geometry().topLeft()) / dpr, pixels.size()};
}
QRect pinnedWindowRect(const PinnedPlacement& placement, const QScreen& screen) {
    const qreal dpr = storage::pinnedGeometryScale(std::max(qreal(1), screen.devicePixelRatio()),
                                                   placement.units);
    return {screen.geometry().topLeft() + (placement.position * dpr).toPoint(),
            placement.windowSize};
}
PinnedDisplayGeometry pinnedDisplayGeometry(const QScreen& screen) {
    QRect usable = screen.availableGeometry();
#if defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        usable = cocoaPinnedUsableGeometry(screen);
#endif
    return {screen.name(), screen.serialNumber(), QRectF(screen.geometry()), QRectF(usable),
            std::max(qreal(1), screen.devicePixelRatio())};
}
QRectF pinnedDesktopRect(const PinnedPlacement& placement, const QScreen& screen) {
    return pinnedDesktopRect(placement, pinnedDisplayGeometry(screen));
}
QScreen* pinnedDisplay(const PinnedPlacement& placement, QScreen* fallback) {
    for (QScreen* screen : QGuiApplication::screens()) {
        if (!placement.displaySerial.isEmpty() && screen->serialNumber() == placement.displaySerial)
            return screen;
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->name() == placement.displayName)
            return screen;
    }
    return fallback != nullptr ? fallback : QGuiApplication::primaryScreen();
}
QScreen* pinnedDisplayAt(const QPointF& desktopPosition) {
    for (QScreen* screen : QGuiApplication::screens()) {
        if (pinnedDisplayContains(pinnedDisplayGeometry(*screen), desktopPosition))
            return screen;
    }
    return nullptr;
}
PinnedPlacement recoverPinnedPlacement(PinnedPlacement placement, const QScreen& screen) {
    return recoverPinnedPlacement(std::move(placement), pinnedDisplayGeometry(screen));
}

PinnedWindowPlatform::PinnedWindowPlatform(QWidget* window, Role role)
    : m_window(window), m_role(role) {
    window->installEventFilter(this);
    qApp->installEventFilter(this);
    const auto changed = [this](bool layoutChanged) {
        if (environmentChanged)
            environmentChanged(layoutChanged);
    };
    const auto watch = [this, changed](QScreen* screen) {
        connect(screen, &QScreen::geometryChanged, this, [changed] { changed(true); });
        connect(screen, &QScreen::availableGeometryChanged, this, [changed] { changed(true); });
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, [changed] { changed(false); });
    };
    for (QScreen* screen : QGuiApplication::screens())
        watch(screen);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [watch, changed](QScreen* screen) {
        watch(screen);
        changed(false);
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [changed] { changed(true); });
}
PinnedWindowPlatform::~PinnedWindowPlatform() = default;
void PinnedWindowPlatform::observeNativeSurface() {
    QWindow* surface = m_window ? m_window->windowHandle() : nullptr;
    if (surface == m_surface)
        return;
    if (m_surface)
        m_surface->removeEventFilter(this);
    m_surface = surface;
    if (m_surface)
        m_surface->installEventFilter(this);
}
bool PinnedWindowPlatform::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_surface && event->type() == QEvent::PlatformSurface) {
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            detach();
        return false;
    }
    if (watched != m_window) {
        if (m_window && event->type() == QEvent::Show) {
            if (auto* child = qobject_cast<QWidget*>(watched);
                child && child->isWindow() &&
                (m_window->isAncestorOf(child) ||
                 (child->windowHandle() &&
                  child->windowHandle()->transientParent() == m_window->windowHandle()))) {
                configurePinnedAuxiliary(child);
            }
        }
        return false;
    }
    if (event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
        detach();
    } else if (event->type() == QEvent::WinIdChange || event->type() == QEvent::Show) {
        if (m_window->internalWinId() != 0)
            static_cast<void>(attach());
    } else if (event->type() == QEvent::DevicePixelRatioChange ||
               event->type() == QEvent::ScreenChangeInternal) {
        if (environmentChanged)
            environmentChanged(false);
    }
    return false;
}
std::optional<QPointF> PinnedWindowPlatform::pointerPosition() const {
    return QPointF(QCursor::pos());
}
std::optional<bool> PinnedWindowPlatform::pointerInside() const {
    if (!m_window || !m_window->internalWinId())
        return std::nullopt;
    const auto current = placement();
    const auto pointer = pointerPosition();
    if (!current || !pointer || !m_window->screen())
        return std::nullopt;
    return pinnedDesktopRect(*current, *pinnedDisplay(*current, m_window->screen()))
        .contains(*pointer);
}
bool PinnedWindowPlatform::applyGeometry(const QRect& pixels, QScreen* screen,
                                         GeometryUpdate update) {
    if (!screen || !pixels.isValid())
        return false;
    return applyStablePlacement(pinnedPlacement(pixels, *screen), screen, update);
}
bool PinnedWindowPlatform::applyStablePlacement(PinnedPlacement requested, QScreen* screen,
                                                GeometryUpdate update) {
    if (!screen || !requested.isValid())
        return false;
    const QSize windowSize = requested.windowSize;
    const QPointF desktopAnchor = pinnedDesktopRect(requested, *screen).topLeft();
    const int attempts = requested.units == storage::PinnedGeometryUnits::LogicalPixels ? 1 : 3;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (!applyPlacement(requested, screen, update))
            return false;
        const auto actual = placement();
        if (!actual)
            return false;
        if (actual->windowSize == windowSize)
            return true;
        screen = pinnedDisplay(*actual, screen);
        requested.displayName = screen->name();
        requested.displaySerial = screen->serialNumber();
        requested.position = desktopAnchor - screen->geometry().topLeft();
    }
    return false;
}
QRect PinnedWindowPlatform::windowGeometry() const {
    const auto current = placement();
    return current && m_window && m_window->screen()
               ? pinnedWindowRect(*current, *pinnedDisplay(*current, m_window->screen()))
               : QRect();
}

class QtPinnedWindowPlatform final : public PinnedWindowPlatform {
  public:
    using PinnedWindowPlatform::PinnedWindowPlatform;
    bool attach() override {
        observeNativeSurface();
        return m_window && m_window->internalWinId() != 0;
    }
    void detach() override {}
    bool applyPlacement(const PinnedPlacement& placement, QScreen* screen,
                        GeometryUpdate) override {
        if (!m_window || !screen || !placement.isValid() ||
            placement.units != storage::kPinnedGeometryUnits)
            return false;
        m_placement = placement;
        m_window->setScreen(screen);
        m_window->setGeometry(pinnedDesktopRect(placement, *screen).toAlignedRect());
        return true;
    }
    std::optional<PinnedPlacement> placement() const override {
        return m_placement;
    }
    bool setInputTransparent(bool transparent) override {
        m_transparent = transparent;
        return true; // Offscreen state emulation, never selected for a native desktop.
    }
    bool setStaysOnTop(bool) override {
        return false; // Let the caller update Qt window flags.
    }
    bool activate() override {
        if (!m_window)
            return false;
        m_window->activateWindow();
        return true;
    }
    std::optional<QPointF> pointerPosition() const override {
        return std::nullopt;
    }
    bool usesControlledInteraction() const override {
        return true;
    }

  private:
    std::optional<PinnedPlacement> m_placement;
};
#if defined(Q_OS_MACOS)
std::unique_ptr<PinnedWindowPlatform> createCocoaPinnedWindowPlatform(QWidget*,
                                                                      PinnedWindowPlatform::Role);
#endif
#if defined(Q_OS_WIN)
class WindowsPinnedWindowPlatform final : public PinnedWindowPlatform {
  public:
    WindowsPinnedWindowPlatform(QWidget* window, Role role)
        : PinnedWindowPlatform(window, role), m_keyboard(window) {}
    ~WindowsPinnedWindowPlatform() override {
        detach();
    }
    bool attach() override {
        if (!m_window || !m_window->internalWinId())
            return false;
        observeNativeSurface();
        const WId id = m_window->internalWinId();
        if (m_attached && m_attached != id)
            detach();
        if (m_role == Role::Image && !screenshot_pinned_window_native::applySystemResizeStyle(id))
            return false;
        if (m_resizeState &&
            !screenshot_pinned_window_native::installSynchronizedResize(id, m_resizeState))
            return false;
        if (m_transparent && m_attached != id &&
            !screenshot_pinned_window_native::setInputTransparent(id, true))
            return false;
        m_attached = id;
        return true;
    }
    void detach() override {
        m_keyboard.stop();
        if (m_attached)
            screenshot_pinned_window_native::removeSynchronizedResize(m_attached);
        m_attached = 0;
    }
    void setResizeInteractionState(const bool* state) override {
        m_resizeState = state;
    }
    void setMoveKeyCombinations(const QList<QKeyCombination>& keys) override {
        m_keyboard.setKeyCombinations(keys);
    }
    void setSystemMoveActive(bool active) override {
        if (active)
            static_cast<void>(m_keyboard.start());
        else
            m_keyboard.stop();
    }
    bool startSystemMove() override {
        return m_window && m_window->windowHandle() && m_window->windowHandle()->startSystemMove();
    }
    bool handleNativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        auto* pin = qobject_cast<ScreenshotPinnedWindow*>(m_window.data());
        return pin && PinnedWindowWindowsEvents::handle(*pin, type, message, result);
    }
    bool systemInteractionReleased() const override {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;
    }
    QRect frameGeometry() const override {
        return m_window ? screenshot_pinned_window_native::currentWindowGeometry(
                              m_window->internalWinId())
                        : QRect();
    }
    bool synchronizePaint(bool invalidate) override {
        using namespace screenshot_pinned_window_native;
        return m_window &&
               synchronizeClientPaint(m_window->internalWinId(),
                                      invalidate ? PaintSynchronization::InvalidateAndUpdate
                                                 : PaintSynchronization::FlushAlreadyPainted);
    }
    bool applyGeometry(const QRect& pixels, QScreen*, GeometryUpdate update) override {
        return m_window && m_window->internalWinId() &&
               screenshot_pinned_window_native::applyClientGeometry(
                   m_window->internalWinId(), pixels,
                   update == GeometryUpdate::DiscardContents
                       ? screenshot_pinned_window_native::GeometryUpdate::DiscardClientPixels
                       : screenshot_pinned_window_native::GeometryUpdate::PreserveClientPixels);
    }
    QRect windowGeometry() const override {
        return m_window && m_window->internalWinId()
                   ? screenshot_pinned_window_native::currentClientGeometry(
                         m_window->internalWinId())
                   : QRect();
    }
    bool applyPlacement(const PinnedPlacement& placement, QScreen* screen,
                        GeometryUpdate update) override {
        return screen && placement.isValid() &&
               applyGeometry(pinnedWindowRect(placement, *screen), screen, update);
    }
    std::optional<PinnedPlacement> placement() const override {
        if (!m_window || !m_window->screen())
            return std::nullopt;
        const QRect rect = windowGeometry();
        return rect.isValid() ? std::optional(pinnedPlacement(rect, *m_window->screen()))
                              : std::nullopt;
    }
    std::optional<bool> pointerInside() const override {
        return m_window
                   ? screenshot_pinned_window_native::pointerInsideWindow(m_window->internalWinId())
                   : std::nullopt;
    }
    bool setInputTransparent(bool transparent) override {
        if (!m_window || !screenshot_pinned_window_native::setInputTransparent(
                             m_window->internalWinId(), transparent))
            return false;
        m_transparent = transparent;
        return true;
    }
    bool setStaysOnTop(bool staysOnTop) override {
        return m_window && m_window->internalWinId() != 0 &&
               screenshot_pinned_window_native::setStaysOnTop(m_window->internalWinId(),
                                                              staysOnTop);
    }
    bool activate() override {
        return m_window &&
               screenshot_pinned_window_native::activateWindow(m_window->internalWinId());
    }

  private:
    screenshot_pinned_window_native::SystemMoveKeyboard m_keyboard;
    WId m_attached = 0;
    const bool* m_resizeState = nullptr;
};
#endif
std::unique_ptr<PinnedWindowPlatform> createPinnedWindowPlatform(QWidget* window,
                                                                 PinnedWindowPlatform::Role role) {
#if defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        return createCocoaPinnedWindowPlatform(window, role);
#elif defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows"))
        return std::make_unique<WindowsPinnedWindowPlatform>(window, role);
#endif
    return std::make_unique<QtPinnedWindowPlatform>(window, role);
}
PinnedWindowPlatform* configurePinnedAuxiliary(QWidget* window) {
    if (!window)
        return nullptr;
    if (auto* existing = dynamic_cast<PinnedWindowPlatform*>(window->findChild<QObject*>(
            QStringLiteral("snowPinnedWindowPlatform"), Qt::FindDirectChildrenOnly)))
        return existing;
    auto platform = createPinnedWindowPlatform(window, PinnedWindowPlatform::Role::Auxiliary);
    platform->setObjectName(QStringLiteral("snowPinnedWindowPlatform"));
    platform->setParent(window);
    if (window->internalWinId() != 0)
        static_cast<void>(platform->attach());
    return platform.release();
}
} // namespace snow_shot::presentation
