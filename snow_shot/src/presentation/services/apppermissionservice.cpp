#include "snow_shot/presentation/apppermissionservice.h"
#include <QCoreApplication>
#include <QPointer>
#include <algorithm>

namespace snow_shot::presentation {
namespace {
QString permissionText(const char* text) {
    return QCoreApplication::translate("AppPermissions", text);
}
} // namespace
QString appPermissionId(AppPermission permission) {
    switch (permission) {
    case AppPermission::ScreenRecording:
        return QStringLiteral("screen-recording");
    case AppPermission::Accessibility:
        return QStringLiteral("accessibility");
    case AppPermission::InputMonitoring:
        return QStringLiteral("input-monitoring");
    case AppPermission::Microphone:
        return QStringLiteral("microphone");
    }
    return {};
}
QString appPermissionName(AppPermission permission) {
    switch (permission) {
    case AppPermission::ScreenRecording:
        return permissionText(
            QT_TRANSLATE_NOOP("AppPermissions", "Screen & System Audio Recording"));
    case AppPermission::Accessibility:
        return permissionText(QT_TRANSLATE_NOOP("AppPermissions", "Accessibility"));
    case AppPermission::InputMonitoring:
        return permissionText(QT_TRANSLATE_NOOP("AppPermissions", "Input Monitoring"));
    case AppPermission::Microphone:
        return permissionText(QT_TRANSLATE_NOOP("AppPermissions", "Microphone"));
    }
    return {};
}
AppPermissions requiredPermissions(GlobalShortcutAction action, bool /*microphoneEnabled*/) {
    using Action = GlobalShortcutAction;
    using P = AppPermission;
    switch (action) {
    case Action::Screenshot:
    case Action::ScreenshotDelay:
    case Action::ScreenshotFixed:
    case Action::ScreenshotOcr:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotCopy:
    case Action::ScreenshotFullScreen:
    case Action::ScreenshotFocusedWindow:
        return {P::ScreenRecording};
    case Action::ScreenRecord:
    case Action::ScreenRecordCopy:
        // This action opens selection. The recording controller checks microphone
        // and effect permissions against the final toolbar settings at start.
        return {P::ScreenRecording};
    case Action::TranslateSelectedText:
        return {P::Accessibility};
    case Action::OpenScreenRecordingFolder:
    case Action::OpenCaptureHistory:
    case Action::OpenSettings:
    case Action::PinClipboardContent:
    case Action::PinSelectedFiles:
    case Action::ToggleGlobalHotkeys:
    case Action::ToggleDisableOnFocusedFullscreenWindow:
        return {};
    }
    return {};
}
AppPermissions requiredPermissions(settings::SettingsGlobalMouseAction /*action*/,
                                   bool /*microphoneEnabled*/) {
    AppPermissions result{AppPermission::ScreenRecording, AppPermission::Accessibility,
                          AppPermission::InputMonitoring};
    return result;
}
AppPermissions pagePermissions(bool mousePage, bool microphoneEnabled, bool selectedTextEnabled) {
    AppPermissions result{AppPermission::ScreenRecording};
    if (mousePage || selectedTextEnabled)
        result.append(AppPermission::Accessibility);
    if (mousePage)
        result.append(AppPermission::InputMonitoring);
    if (microphoneEnabled)
        result.append(AppPermission::Microphone);
    return result;
}
AppPermissionService::AppPermissionService(QObject* parent)
    : AppPermissionService(createAppPermissionBackend(), parent) {}
AppPermissionService::AppPermissionService(std::unique_ptr<AppPermissionBackend> backend,
                                           QObject* parent)
    : QObject(parent), m_backend(std::move(backend)) {
    m_refreshTimer.setSingleShot(true);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
        const auto next = m_backend->query();
        if (next != m_snapshot) {
            m_snapshot = next;
            emit changed();
        }
        emit refreshed();
    });
    m_pollTimer.setInterval(2000);
    m_pollTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &AppPermissionService::refresh);
}
AppPermissions AppPermissionService::missing(const AppPermissions& requirements) const {
    AppPermissions result;
    for (auto permission : requirements)
        if (!m_snapshot.granted(permission) && !result.contains(permission))
            result.append(permission);
    return result;
}
AppPermissions AppPermissionService::startupMissing() const {
    return missing(pagePermissions(true, m_microphoneEnabled, true));
}
AppPermissions AppPermissionService::takeStartupMissing() {
    if (m_startupHandled || std::find(m_snapshot.statuses.begin(), m_snapshot.statuses.end(),
                                      AppPermissionStatus::Checking) != m_snapshot.statuses.end())
        return {};
    m_startupHandled = true;
    return startupMissing();
}
bool AppPermissionService::allow(const AppPermissions& requirements,
                                 const std::function<void(const AppPermissions&)>& blocked) const {
    const auto unavailable = missing(requirements);
    if (unavailable.isEmpty())
        return true;
    if (blocked)
        blocked(unavailable);
    return false;
}
void AppPermissionService::setMicrophoneEnabled(bool enabled) {
    if (m_microphoneEnabled == enabled)
        return;
    m_microphoneEnabled = enabled;
    emit changed();
}
bool AppPermissionService::requestPending(AppPermission permission) const {
    return m_pending.contains(permission);
}
void AppPermissionService::refresh() {
    if (!m_refreshTimer.isActive())
        m_refreshTimer.start(0);
}
void AppPermissionService::request(AppPermission permission) {
    const auto status = m_snapshot.status(permission);
    if (requestPending() || m_snapshot.granted(permission) ||
        status == AppPermissionStatus::Checking || status == AppPermissionStatus::Restricted ||
        (permission == AppPermission::Microphone &&
         (status == AppPermissionStatus::Denied || status == AppPermissionStatus::Error)))
        return;
    m_pending.insert(permission);
    emit changed();
    m_backend->request(permission, [guard = QPointer<AppPermissionService>(this), permission] {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, permission] {
                if (!guard)
                    return;
                guard->m_pending.remove(permission);
                emit guard->changed();
                guard->refresh();
            },
            Qt::QueuedConnection);
    });
}
bool AppPermissionService::openSettings(AppPermission permission) {
    if (!m_backend->openSettings(permission))
        return false;
    emit settingsOpened(permission);
    return true;
}
void AppPermissionService::observe(QObject* owner, bool visible) {
    if (!owner)
        return;
    if (visible && !m_observers.contains(owner)) {
        m_observers.insert(owner, connect(owner, &QObject::destroyed, this,
                                          [this, owner] { observe(owner, false); }));
        if (!m_pollTimer.isActive()) {
            m_pollTimer.start();
            refresh();
        }
    } else if (!visible) {
        QObject::disconnect(m_observers.take(owner));
        if (m_observers.isEmpty())
            m_pollTimer.stop();
    }
}
#ifndef Q_OS_MACOS
namespace {
class NoPermissionsBackend final : public AppPermissionBackend {
    AppPermissionSnapshot query() override {
        AppPermissionSnapshot result;
        result.statuses.fill(AppPermissionStatus::Granted);
        return result;
    }
    void request(AppPermission, std::function<void()> completion) override {
        completion();
    }
    bool openSettings(AppPermission) override {
        return false;
    }
};
} // namespace
std::unique_ptr<AppPermissionBackend> createAppPermissionBackend() {
    return std::make_unique<NoPermissionsBackend>();
}
#endif
} // namespace snow_shot::presentation
