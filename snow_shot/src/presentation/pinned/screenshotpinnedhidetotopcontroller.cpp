#include "screenshotpinnedhidetotopcontroller.h"
#include "screenshotpinnedpointerpresence.h"

#include "screenshotpinnedwindownative.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "theme/theme_color_utils.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScopedValueRollback>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace geometry = screenshot_pinned_hide_to_top;
namespace native = screenshot_pinned_window_native;

namespace {
constexpr int kHandleBorderDevicePixels = 2;

QVector<ScreenshotPinnedHideToTopController*>& reservations() {
    static QVector<ScreenshotPinnedHideToTopController*> entries;
    return entries;
}

class Handle final : public QWidget {
  public:
    Handle(QWidget* owner, ScreenshotPinnedHideToTopController* controller)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::WindowDoesNotAcceptFocus),
          m_owner(owner), m_controller(controller) {
        setObjectName(QStringLiteral("screenshotPinnedHideToTopHandle"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
    }
    QRect visualRect;

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(m_owner);
        const qreal dpr = devicePixelRatioF();
        const QRectF visual(visualRect.x() / dpr, visualRect.y() / dpr, visualRect.width() / dpr,
                            visualRect.height() / dpr);
        QPainter painter(this);
        // Layered Windows hit testing ignores fully transparent pixels. One alpha
        // step keeps the enlarged hover margin targetable without a visible panel.
        painter.fillRect(rect(), QColor(0, 0, 0, 1));
        // Paint the border in physical device pixels with the pinned window
        // border's convention: map the handle's logical rect through the
        // combined transform (device pixel ratio included) and round each edge,
        // so the left, bottom, and right sides stay exactly
        // kHandleBorderDevicePixels physical pixels wide at every scale factor.
        // The handle rests on the work-area top edge, so it omits the top side.
        const QTransform surfaceTransform = painter.combinedTransform();
        const QRectF mappedExtent = surfaceTransform.mapRect(visual);
        QRect deviceRect(
            qRound(mappedExtent.left()), qRound(mappedExtent.top()),
            qRound(mappedExtent.left() + mappedExtent.width()) - qRound(mappedExtent.left()),
            qRound(mappedExtent.top() + mappedExtent.height()) - qRound(mappedExtent.top()));
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (internalWinId() != 0) {
            const QRect client = native::currentClientGeometry(internalWinId());
            if (client.isValid() && !client.isEmpty()) {
                deviceRect = deviceRect.intersected(QRect(QPoint(), client.size()));
            }
        }
#endif
        if (deviceRect.isEmpty()) {
            return;
        }
        // Neutralize the whole combined transform (world transform plus the
        // paint device's scale, e.g. a backing store or QImage device pixel
        // ratio) so painter units become physical pixels of the paint device
        // and the integer fills below align exactly to the device grid.
        bool invertible = false;
        const QTransform toSurface = surfaceTransform.inverted(&invertible);
        if (!invertible) {
            return;
        }
        painter.setTransform(painter.transform() * toSurface);
        painter.fillRect(deviceRect, m_controller->handleColor());
        const QColor border = theme.colorText;
        const int left = deviceRect.left();
        const int top = deviceRect.top();
        const int right = deviceRect.right();
        const int bottom = deviceRect.bottom();
        const int sideHeight = bottom - top + 1;
        painter.fillRect(QRect(left, top, kHandleBorderDevicePixels, sideHeight), border);
        painter.fillRect(QRect(left, bottom - kHandleBorderDevicePixels + 1, right - left + 1,
                               kHandleBorderDevicePixels),
                         border);
        painter.fillRect(QRect(right - kHandleBorderDevicePixels + 1, top,
                               kHandleBorderDevicePixels, sideHeight),
                         border);
    }

  private:
    QPointer<QWidget> m_owner;
    ScreenshotPinnedHideToTopController* m_controller;
};
} // namespace

geometry::Screen geometry::screenGeometry(QScreen* screen) {
    if (screen == nullptr) {
        return {};
    }
    const QRect physical = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect logical = screen->geometry();
    const QRect available = screen->availableGeometry();
    const qreal dpi = screen->devicePixelRatio() > 0 ? screen->devicePixelRatio() : 1.0;
    return {screen,
            QRect(physical.left() + qRound((available.left() - logical.left()) * dpi),
                  physical.top() + qRound((available.top() - logical.top()) * dpi),
                  qRound(available.width() * dpi), qRound(available.height() * dpi)),
            dpi};
}

QRect geometry::placeHandle(const QRect& workArea, int preferredLeft, qreal dpi,
                            const QVector<QRect>& occupied) {
    if (workArea.isEmpty() || !qIsFinite(dpi) || dpi <= 0) {
        return {};
    }
    const int width = std::min(workArea.width(), std::max(1, qRound(30 * dpi)));
    const int height = std::min(workArea.height(), std::max(1, qRound(6 * dpi)));
    const int gap = std::max(1, qRound(4 * dpi));
    const int lastLeft = workArea.right() - width + 1;
    const int preferred = qBound(workArea.left(), preferredLeft, lastLeft);
    const auto free = [&](int left) {
        return std::none_of(occupied.cbegin(), occupied.cend(), [&](const QRect& other) {
            return left < other.right() + 1 + gap && left + width + gap > other.left();
        });
    };
    QVector<int> right{preferred};
    QVector<int> left{preferred};
    for (const QRect& other : occupied) {
        right.push_back(other.right() + 1 + gap);
        left.push_back(other.left() - gap - width);
    }
    std::sort(right.begin(), right.end());
    std::sort(left.begin(), left.end(), std::greater<int>());
    for (int candidate : right) {
        if (candidate >= preferred && candidate <= lastLeft && free(candidate)) {
            return QRect(candidate, workArea.top(), width, height);
        }
    }
    for (int candidate : left) {
        if (candidate <= preferred && candidate >= workArea.left() && free(candidate)) {
            return QRect(candidate, workArea.top(), width, height);
        }
    }
    return QRect(preferred, workArea.top(), width, height);
}

QRect geometry::hitGeometry(const QRect& handle, const QRect& workArea, qreal dpi) {
    const int margin = std::max(1, qRound(2 * dpi));
    return handle.adjusted(-margin, 0, margin, margin).intersected(workArea);
}

ScreenshotPinnedHideToTopController::ScreenshotPinnedHideToTopController(QWidget* owner,
                                                                         Hooks hooks)
    : QObject(owner), m_owner(owner), m_hooks(std::move(hooks)),
      m_handle(std::make_unique<Handle>(owner, this)) {
    m_pointerPresence = std::make_unique<ScreenshotPinnedPointerPresence>(
        this,
        [this]() -> std::optional<bool> {
            if (m_hooks.cursor) {
                if (const auto cursor = m_hooks.cursor()) {
                    return pointerInside(*cursor);
                }
            }
            return std::nullopt;
        },
        [this](bool inside) {
            if (inside) {
                reveal();
            } else {
                hideWindow();
            }
        });
    m_handle->installEventFilter(this);
    retranslate();
    m_animation.setObjectName(QStringLiteral("screenshotPinnedHideToTopAnimation"));
    m_animation.setDuration(250);
    m_animation.setEasingCurve(QEasingCurve::InOutCubic);
    m_animation.setStartValue(0.0);
    m_animation.setEndValue(1.0);
    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        if (m_state != State::Entering || m_changing) {
            return;
        }
        const qreal progress = value.toReal();
        QRect target = m_hooks.geometry();
        target.moveTopLeft(QPoint(m_entryPosition.x(), qRound(m_entryPosition.y() * (1 - progress) +
                                                              m_screen.workArea.top() * progress)));
        if (!m_hooks.applyGeometry(target)) {
            exit(true);
            return;
        }
        refreshOpacity();
    });
    connect(&m_animation, &QVariantAnimation::finished, this, [this] {
        if (m_state == State::Entering) {
            hideWindow();
            showHandle();
            changed();
        }
    });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
            m_handle.get(), qOverload<>(&QWidget::update));
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
        if (active() && !m_shutdown && m_screen.screen == removed) {
            QScreen* target = ScreenshotGeometryMapper::screenForPhysicalRect(shownGeometry());
            if (target == removed) {
                target = QGuiApplication::primaryScreen();
            }
            recoverToScreen(geometry::screenGeometry(target));
        }
    });
}

ScreenshotPinnedHideToTopController::~ScreenshotPinnedHideToTopController() {
    shutdown();
}

QRect ScreenshotPinnedHideToTopController::shownGeometry() const {
    QRect result = m_hooks.geometry();
    if (active()) {
        result.moveTopLeft(
            QPoint(m_handleGeometry.left(), m_handleGeometry.top() + m_handleGeometry.height()));
    }
    return result;
}

void ScreenshotPinnedHideToTopController::setAccentIndex(int index) {
    m_accentIndex = index >= 0 && index < 13 ? index : -1;
}

QColor ScreenshotPinnedHideToTopController::handleColor() const {
    const auto resolved = adqt::theme::ThemeManager::instance().resolve(m_owner);
    const auto& accents = resolved.theme.accents;
    const QColor colors[] = {accents.blue,    accents.purple,  accents.cyan,     accents.green,
                             accents.magenta, accents.pink,    accents.red,      accents.orange,
                             accents.yellow,  accents.volcano, accents.geekblue, accents.gold,
                             accents.lime};
    const auto palette = adqt::theme::generateMappedPalette(
        colors[qBound(0, m_accentIndex, 12)],
        resolved.theme.scheme == adqt::theme::ThemeScheme::Dark, resolved.values.colorBgContainer);
    return palette.value(4, colors[qBound(0, m_accentIndex, 12)]);
}

bool ScreenshotPinnedHideToTopController::reserve(const geometry::Screen& screen,
                                                  int preferredLeft) {
    release();
    QVector<QRect> occupied;
    for (const auto* entry : reservations()) {
        if (screen.screen != nullptr ? entry->m_screen.screen == screen.screen
                                     : (entry->m_screen.screen == nullptr &&
                                        entry->m_screen.workArea == screen.workArea)) {
            occupied.push_back(entry->handleGeometry());
        }
    }
    const QRect placement =
        geometry::placeHandle(screen.workArea, preferredLeft, screen.dpi, occupied);
    if (placement.isEmpty()) {
        return false;
    }
    m_screen = screen;
    m_handleGeometry = placement;
    reservations().push_back(this);
    watchScreen();
    return true;
}

void ScreenshotPinnedHideToTopController::release() {
    reservations().removeAll(this);
    for (const auto& connection : m_screenConnections) {
        disconnect(connection);
    }
    m_screenConnections.clear();
}

void ScreenshotPinnedHideToTopController::watchScreen() {
    if (m_screen.screen == nullptr) {
        return;
    }
    const auto reconcile = [this] {
        if (active()) {
            reconcileScreen(geometry::screenGeometry(m_screen.screen));
        }
    };
    m_screenConnections.push_back(
        connect(m_screen.screen, &QScreen::availableGeometryChanged, this, reconcile));
    m_screenConnections.push_back(
        connect(m_screen.screen, &QScreen::geometryChanged, this, reconcile));
    m_screenConnections.push_back(
        connect(m_screen.screen, &QScreen::logicalDotsPerInchChanged, this, reconcile));
}

bool ScreenshotPinnedHideToTopController::enter(const geometry::Screen& screen) {
    if (active() || m_owner == nullptr || !reserve(screen, m_hooks.geometry().left())) {
        return false;
    }
    if (m_accentIndex < 0) {
        setAccentIndex(m_hooks.allocateAccent());
    }
    m_entryPosition = m_hooks.geometry().topLeft();
    m_suppressed = false;
    m_shutdown = false;
    m_restoring = false;
    m_state = State::Entering;
    m_animation.start();
    changed();
    return true;
}

bool ScreenshotPinnedHideToTopController::prepareRestore(const geometry::Screen& screen,
                                                         const QRect& savedHandle) {
    if (active() || savedHandle.isEmpty() || m_accentIndex < 0 ||
        !reserve(screen, savedHandle.left())) {
        return false;
    }
    m_state = State::Hidden;
    m_shutdown = false;
    m_restoring = true;
    m_owner->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_owner->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_owner->setWindowOpacity(0.0);
    return true;
}

void ScreenshotPinnedHideToTopController::finishRestore() {
    if (!m_restoring || !active()) {
        return;
    }
    m_restoring = false;
    hideWindow();
    m_owner->setAttribute(Qt::WA_ShowWithoutActivating, false);
    m_owner->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    showHandle();
    changed();
}

void ScreenshotPinnedHideToTopController::exit(bool cancelEntry) {
    if (!active() || m_owner == nullptr) {
        return;
    }
    const State previous = m_state;
    QRect target = previous == State::Hidden ? shownGeometry() : m_hooks.geometry();
    if (previous == State::Entering && cancelEntry) {
        target.moveTopLeft(m_entryPosition);
    }
    m_state = State::Normal;
    m_pointerPresence->reset();
    m_animation.stop();
    m_restoring = false;
    m_suppressed = false;
    release();
    m_handle->hide();
    QScopedValueRollback guard(m_changing, true);
    if (target != m_hooks.geometry()) {
        static_cast<void>(m_hooks.applyGeometry(target));
    }
    m_owner->setAttribute(Qt::WA_ShowWithoutActivating, false);
    m_owner->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    refreshOpacity();
    m_owner->show();
    changed();
}

void ScreenshotPinnedHideToTopController::shutdown() {
    m_pointerPresence->reset();
    m_shutdown = true;
    m_animation.stop();
    m_restoring = false;
    m_suppressed = true;
    release();
    m_handle->hide();
}

void ScreenshotPinnedHideToTopController::setSuppressed(bool suppressed) {
    if (!active()) {
        return;
    }
    m_suppressed = suppressed;
    if (suppressed) {
        m_pointerPresence->reset();
        m_animation.stop();
        hideWindow();
        m_handle->hide();
        release();
    } else if (reserve(m_screen, m_handleGeometry.left())) {
        showHandle();
    } else {
        exit();
    }
}

void ScreenshotPinnedHideToTopController::refreshOpacity() {
    if (m_owner != nullptr) {
        const qreal factor =
            m_restoring || m_state == State::Hidden
                ? 0.0
                : (m_state == State::Entering ? 1.0 - m_animation.currentValue().toReal() : 1.0);
        m_owner->setWindowOpacity(qBound(25, m_hooks.opacity(), 100) / 100.0 * factor);
    }
}

void ScreenshotPinnedHideToTopController::showHandle() {
    if (m_suppressed || m_restoring || !active()) {
        return;
    }
    const QRect hit = geometry::hitGeometry(m_handleGeometry, m_screen.workArea, m_screen.dpi);
    auto* handle = static_cast<Handle*>(m_handle.get());
    handle->visualRect = m_handleGeometry.translated(-hit.topLeft());
    if (m_screen.screen != nullptr) {
        handle->setScreen(m_screen.screen);
    }
    handle->resize(std::max(1, qRound(hit.width() / m_screen.dpi)),
                   std::max(1, qRound(hit.height() / m_screen.dpi)));
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        if (!native::applyClientGeometry(handle->winId(), hit)) {
            exit();
            return;
        }
    } else {
        handle->move(hit.topLeft());
    }
    handle->show();
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        if (!native::applyClientGeometry(handle->winId(), hit)) {
            exit();
            return;
        }
    }
    handle->raise();
    handle->update();
}

void ScreenshotPinnedHideToTopController::hideWindow() {
    QScopedValueRollback guard(m_changing, true);
    m_state = State::Hidden;
    m_owner->clearFocus();
    m_owner->hide();
    refreshOpacity();
}

void ScreenshotPinnedHideToTopController::reveal() {
    QScopedValueRollback guard(m_changing, true);
    if (!m_hooks.applyGeometry(shownGeometry())) {
        exit();
        return;
    }
    m_state = State::Revealed;
    refreshOpacity();
    m_owner->show();
    m_owner->raise();
    m_handle->raise();
    m_hooks.activate();
}

void ScreenshotPinnedHideToTopController::refreshPointer() {
    if (active() && m_hooks.cursor) {
        if (const auto cursor = m_hooks.cursor()) {
            updatePointer(*cursor);
        }
    }
}

void ScreenshotPinnedHideToTopController::updatePointer(const QPoint& nativeCursor) {
    if (m_suppressed || m_restoring || m_changing || m_owner == nullptr ||
        (m_state != State::Hidden && m_state != State::Revealed)) {
        return;
    }
    m_pointerPresence->update(pointerInside(nativeCursor));
}

bool ScreenshotPinnedHideToTopController::pointerInside(const QPoint& nativeCursor) const {
    return geometry::hitGeometry(m_handleGeometry, m_screen.workArea, m_screen.dpi)
               .contains(nativeCursor) ||
           (m_state == State::Revealed && m_hooks.frameGeometry().contains(nativeCursor));
}

void ScreenshotPinnedHideToTopController::reconcileScreen(const geometry::Screen& screen) {
    if (!active() || m_suppressed) {
        return;
    }
    if (!reserve(screen, m_handleGeometry.left())) {
        recoverToScreen(screen);
        return;
    }
    if (m_state == State::Revealed && !m_hooks.applyGeometry(shownGeometry())) {
        exit();
        return;
    }
    if (m_state != State::Entering) {
        showHandle();
    }
    changed();
}

void ScreenshotPinnedHideToTopController::recoverToScreen(const geometry::Screen& screen) {
    QRect target = shownGeometry();
    if (!screen.workArea.isEmpty()) {
        target.moveLeft(
            qBound(screen.workArea.left(), target.left(),
                   std::max(screen.workArea.left(), screen.workArea.right() - target.width() + 1)));
        target.moveTop(qBound(
            screen.workArea.top(), target.top(),
            std::max(screen.workArea.top(), screen.workArea.bottom() - target.height() + 1)));
    }
    exit();
    static_cast<void>(m_hooks.applyGeometry(target));
    changed();
}

void ScreenshotPinnedHideToTopController::changed() {
    if (m_hooks.changed) {
        m_hooks.changed();
    }
}

void ScreenshotPinnedHideToTopController::retranslate() {
    m_handle->setAccessibleName(
        QCoreApplication::translate("ScreenshotPinnedWindow", "Hide to Top"));
}

bool ScreenshotPinnedHideToTopController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_handle.get()) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
            event->type() == QEvent::MouseMove) {
            refreshPointer();
        } else if (event->type() == QEvent::ContextMenu) {
            m_hooks.contextMenu(static_cast<QContextMenuEvent*>(event)->globalPos());
            return true;
        } else if (event->type() == QEvent::LanguageChange) {
            retranslate();
        } else if (event->type() == QEvent::MouseButtonPress ||
                   event->type() == QEvent::MouseButtonDblClick) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton ||
                (mouse->button() == Qt::LeftButton &&
                 event->type() == QEvent::MouseButtonDblClick)) {
                m_hooks.mouseAction(mouse->button() == Qt::LeftButton);
                return true;
            }
        }
    }
    return QObject::eventFilter(watched, event);
}
