#include "snow_shot/presentation/permissionguidecontroller.h"
#include "snow_shot/presentation/components/permissionguidewidget.h"
#include <QApplication>
#include <QPointer>
#include <algorithm>
#include <chrono>
#include <QtMath>

namespace snow_shot::presentation {
qint64 PermissionGuidePlatform::monotonicMilliseconds() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::optional<PermissionGuideWindow>
selectPermissionGuideWindow(const QVector<PermissionGuideWindow>& windows, quint32 previous) {
    std::optional<PermissionGuideWindow> first;
    for (const auto& window : windows) {
        if (!window.onScreen || window.layer != 0 || window.alpha <= 0.01 ||
            window.bounds.width() < 300 || window.bounds.height() < 200)
            continue;
        if (window.id == previous)
            return window;
        if (!first)
            first = window;
    }
    return first;
}
QRect permissionGuidePlacement(const std::optional<QRectF>& window, const QVector<QRect>& screens,
                               int height) {
    if (screens.isEmpty())
        return {};
    QRect screen = screens.first();
    if (window) {
        qreal largest = -1;
        for (const auto& candidate : screens) {
            const QRectF overlap = window->intersected(QRectF(candidate));
            const qreal area = overlap.width() * overlap.height();
            if (area > largest) {
                largest = area;
                screen = candidate;
            }
        }
    }
    const int width = std::max(1, std::min({500, screen.width() - 24,
                                            window ? qFloor(window->width() * 0.68) - 24 : 500}));
    height = std::clamp(height, 1, std::max(1, screen.height() - 24));
    const int right = window ? qFloor(window->x() + window->width()) : screen.x() + screen.width();
    const int bottom =
        window ? qFloor(window->y() + window->height()) : screen.y() + screen.height();
    return {std::clamp(right - width - 12, screen.x() + 12,
                       std::max(screen.x() + 12, screen.x() + screen.width() - width - 12)),
            std::clamp(bottom - height - 16, screen.y() + 12,
                       std::max(screen.y() + 12, screen.y() + screen.height() - height - 12)),
            width, height};
}
PermissionGuideController::PermissionGuideController(AppPermissionService& service, QObject* parent)
    : PermissionGuideController(service, createPermissionGuidePlatform(), parent) {}
PermissionGuideController::PermissionGuideController(
    AppPermissionService& service, std::unique_ptr<PermissionGuidePlatform> platform,
    QObject* parent)
    : QObject(parent), m_service(service), m_platform(std::move(platform)) {
    m_timer.setInterval(100);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PermissionGuideController::updatePlacement);
    connect(&service, &AppPermissionService::settingsOpened, this,
            &PermissionGuideController::showFor);
    connect(&service, &AppPermissionService::changed, this, [this] {
        if (!m_permission)
            return;
        if (m_service.snapshot().granted(*m_permission))
            dismiss();
        else
            updateContent();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, &PermissionGuideController::dismiss);
}
PermissionGuideController::~PermissionGuideController() {
    m_platform->stop();
    m_service.observe(this, false);
    if (m_widget) {
        disconnect(m_widget, nullptr, this, nullptr);
        // QDrag runs a nested event loop. Keep its source alive until it unwinds.
        if (m_widget->interacting()) {
            m_widget->hide();
            connect(m_widget, &PermissionGuideWidget::interactionFinished, m_widget,
                    &QObject::deleteLater);
        } else {
            delete m_widget;
        }
    }
}
void PermissionGuideController::showFor(AppPermission permission) {
    if (m_service.snapshot().granted(permission))
        return;
    if (!m_widget) {
        m_widget = new PermissionGuideWidget(m_platform->application());
        connect(m_widget, &PermissionGuideWidget::dismissed, this,
                &PermissionGuideController::dismiss);
        connect(m_widget, &PermissionGuideWidget::requestAccess, this, [this] {
            if (m_permission)
                m_service.request(*m_permission);
        });
        connect(m_widget, &PermissionGuideWidget::interactionFinished, this, [this] {
            if (m_dismissAfterInteraction)
                dismiss();
            else
                updatePlacement();
        });
        connect(m_widget, &PermissionGuideWidget::contentSizeChanged, this, [this] {
            QMetaObject::invokeMethod(this, &PermissionGuideController::updatePlacement,
                                      Qt::QueuedConnection);
        });
    }
    if (m_widget->interacting())
        return;
    setVisible(false);
    m_platform->stop();
    m_permission = permission;
    m_windowId = 0;
    m_foundWindow = false;
    m_sawSettings = false;
    m_fallback = false;
    m_dismissAfterInteraction = false;
    m_discoveryStarted = m_platform->monotonicMilliseconds();
    m_platform->start([guard = QPointer<PermissionGuideController>(this)] {
        if (guard)
            QMetaObject::invokeMethod(guard, &PermissionGuideController::updatePlacement,
                                      Qt::QueuedConnection);
    });
    updateContent();
    updatePlacement();
}
void PermissionGuideController::dismiss() {
    if (m_widget && m_widget->interacting()) {
        m_dismissAfterInteraction = true;
        return;
    }
    m_permission.reset();
    m_dismissAfterInteraction = false;
    m_timer.stop();
    m_platform->stop();
    setVisible(false);
}
void PermissionGuideController::setVisible(bool visible) {
    if (!m_widget)
        return;
    if (visible && !m_widget->isVisible()) {
        m_widget->winId();
        m_platform->prepareWindow(m_widget);
        m_widget->show();
    } else if (!visible) {
        m_widget->hide();
    }
    m_service.observe(this, visible);
}
void PermissionGuideController::updateContent() {
    if (m_widget && m_permission)
        m_widget->setPermission(*m_permission, m_service.snapshot().status(*m_permission),
                                m_service.requestPending(), m_fallback);
}
void PermissionGuideController::updatePlacement() {
    if (!m_permission || !m_widget || m_widget->interacting())
        return;
    const auto environment = m_platform->environment();
    if (environment.settingsRunning)
        m_sawSettings = true;
    if (!environment.settingsRunning &&
        (m_sawSettings || (m_platform->monotonicMilliseconds() - m_discoveryStarted) >= 5000)) {
        dismiss();
        return;
    }
    if (!environment.settingsActive && !environment.guideActive) {
        setVisible(false);
        if (environment.settingsRunning)
            m_timer.stop(); // Workspace activation will resume discovery.
        else
            m_timer.start(100); // Settings is still launching.
        return;
    }
    const auto window = selectPermissionGuideWindow(environment.windows, m_windowId);
    if (window) {
        m_windowId = window->id;
        m_foundWindow = true;
    }
    const bool hiddenSettingsWindow =
        std::any_of(environment.windows.begin(), environment.windows.end(),
                    [](const PermissionGuideWindow& candidate) {
                        return candidate.layer == 0 && candidate.alpha > 0.01 &&
                               candidate.bounds.width() >= 300 && candidate.bounds.height() >= 200;
                    });
    if (!window && (m_foundWindow || hiddenSettingsWindow)) {
        setVisible(false); // Minimized/off-space; never place a detached helper.
        m_timer.start(1000);
        return;
    }
    m_fallback = !window;
    if (m_fallback && (m_platform->monotonicMilliseconds() - m_discoveryStarted) < 5000) {
        setVisible(false);
        m_timer.start(100);
        return;
    }
    updateContent();
    const auto bounds = window ? std::optional<QRectF>(window->bounds) : std::nullopt;
    QRect placement = permissionGuidePlacement(bounds, environment.availableScreens, 100);
    if (!placement.isEmpty()) {
        placement = permissionGuidePlacement(bounds, environment.availableScreens,
                                             m_widget->heightForGuideWidth(placement.width()));
        if (placement != m_widget->geometry())
            m_widget->setGeometry(placement);
        setVisible(true);
    } else {
        setVisible(false);
    }
    m_timer.start(m_fallback ? 1000 : 100);
}
#ifndef Q_OS_MACOS
namespace {
class NoPermissionGuidePlatform final : public PermissionGuidePlatform {
    PermissionGuideApplication application() override {
        return {};
    }
    PermissionGuideEnvironment environment() override {
        return {};
    }
    void start(std::function<void()>) override {}
    void stop() override {}
    void prepareWindow(QWidget*) override {}
};
} // namespace
std::unique_ptr<PermissionGuidePlatform> createPermissionGuidePlatform() {
    return std::make_unique<NoPermissionGuidePlatform>();
}
#endif
} // namespace snow_shot::presentation
