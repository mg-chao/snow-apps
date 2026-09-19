#ifndef SNOW_SHOT_PRESENTATION_APPPERMISSIONSERVICE_H
#define SNOW_SHOT_PRESENTATION_APPPERMISSIONSERVICE_H

#include "snow_shot/presentation/globalshortcuttypes.h"
#include "snow_shot/presentation/globalmousetypes.h"
#include <QObject>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <array>
#include <functional>
#include <memory>

namespace snow_shot::presentation {
enum class AppPermission { ScreenRecording, Accessibility, InputMonitoring, Microphone };
enum class AppPermissionStatus {
    Checking,
    Granted,
    Missing,
    NotDetermined,
    Denied,
    Restricted,
    Error
};
using AppPermissions = QVector<AppPermission>;
struct AppPermissionSnapshot {
    std::array<AppPermissionStatus, 4> statuses{
        AppPermissionStatus::Checking, AppPermissionStatus::Checking, AppPermissionStatus::Checking,
        AppPermissionStatus::Checking};
    AppPermissionStatus status(AppPermission permission) const {
        return statuses.at(static_cast<size_t>(permission));
    }
    bool granted(AppPermission permission) const {
        return status(permission) == AppPermissionStatus::Granted;
    }
    friend bool operator==(const AppPermissionSnapshot&, const AppPermissionSnapshot&) = default;
};
class AppPermissionBackend {
  public:
    virtual ~AppPermissionBackend() = default;
    virtual AppPermissionSnapshot query() = 0;
    // Completion may arrive asynchronously, including after service destruction.
    virtual void request(AppPermission permission, std::function<void()> completion) = 0;
    virtual bool openSettings(AppPermission permission) = 0;
};
std::unique_ptr<AppPermissionBackend> createAppPermissionBackend();
QString appPermissionId(AppPermission permission);
QString appPermissionName(AppPermission permission);
AppPermissions requiredPermissions(GlobalShortcutAction action, bool microphoneEnabled);
AppPermissions requiredPermissions(settings::SettingsGlobalMouseAction action,
                                   bool microphoneEnabled);
AppPermissions pagePermissions(bool mousePage, bool microphoneEnabled, bool selectedTextEnabled);

class AppPermissionService final : public QObject {
    Q_OBJECT
  public:
    explicit AppPermissionService(QObject* parent = nullptr);
    explicit AppPermissionService(std::unique_ptr<AppPermissionBackend> backend,
                                  QObject* parent = nullptr);
    const AppPermissionSnapshot& snapshot() const {
        return m_snapshot;
    }
    AppPermissions missing(const AppPermissions& requirements) const;
    AppPermissions startupMissing() const;
    AppPermissions takeStartupMissing();
    bool microphoneEnabled() const {
        return m_microphoneEnabled;
    }
    void setMicrophoneEnabled(bool enabled);
    bool requestPending(AppPermission permission) const;
    bool requestPending() const {
        return !m_pending.isEmpty();
    }
    bool allow(const AppPermissions& requirements,
               const std::function<void(const AppPermissions&)>& blocked) const;
    void refresh();
    void request(AppPermission permission);
    bool openSettings(AppPermission permission);
    void observe(QObject* owner, bool visible);
    bool polling() const {
        return m_pollTimer.isActive();
    }
  signals:
    void changed();
    void refreshed();

  private:
    std::unique_ptr<AppPermissionBackend> m_backend;
    AppPermissionSnapshot m_snapshot;
    QSet<AppPermission> m_pending;
    QHash<QObject*, QMetaObject::Connection> m_observers;
    QTimer m_refreshTimer;
    QTimer m_pollTimer;
    bool m_microphoneEnabled = false;
    bool m_startupHandled = false;
};
} // namespace snow_shot::presentation
#endif
