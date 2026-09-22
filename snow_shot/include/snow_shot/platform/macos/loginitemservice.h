#ifndef SNOW_SHOT_PLATFORM_MACOS_LOGINITEMSERVICE_H
#define SNOW_SHOT_PLATFORM_MACOS_LOGINITEMSERVICE_H

#include <QObject>
#include <QStringList>
#include <functional>

namespace snow_shot::platform::macos {
enum class LoginItemStatus { Unregistered, Enabled, ApprovalRequired, Unavailable };
struct LoginItemSnapshot {
    LoginItemStatus status = LoginItemStatus::Unavailable;
    QString error;
    bool requested() const {
        return status == LoginItemStatus::Enabled || status == LoginItemStatus::ApprovalRequired;
    }
};
struct LoginItemResult {
    bool success = true;
    QString error;
};
struct LoginItemOperations {
    std::function<LoginItemSnapshot()> query;
    std::function<LoginItemResult(bool)> setEnabled;
    std::function<bool()> initialized;
    std::function<bool()> markInitialized;
    std::function<bool(bool)> savePreference;
    std::function<void()> openSettings;
};

// All operations run on the owning GUI thread. Tests inject operations without touching macOS.
class LoginItemService final : public QObject {
    Q_OBJECT
  public:
    explicit LoginItemService(LoginItemOperations operations, QObject* parent = nullptr);
    const LoginItemSnapshot& snapshot() const {
        return m_snapshot;
    }
    bool pending() const {
        return m_pending;
    }
    bool available() const {
        return m_snapshot.status != LoginItemStatus::Unavailable;
    }
    QString hint() const;
    void refresh();
    LoginItemResult initialize(bool enabledByDefault);
    LoginItemResult setEnabled(bool enabled);
    void openSettings();

  signals:
    void changed();

  private:
    LoginItemOperations m_operations;
    LoginItemSnapshot m_snapshot;
    bool m_pending = false;
};

// Canonical paths only: callers resolve symlinks before checking containment.
bool loginItemLocationAllowed(const QString& bundle, const QString& userApplications);
QStringList loginItemLaunchArguments(QStringList arguments, bool nativeLoginLaunch);
bool loginItemAutomaticRegistrationAllowed(const QStringList& arguments);
#ifdef Q_OS_MACOS
LoginItemService& loginItemService();
void observeNativeLoginItemLaunch();
bool initialNativeLoginItemLaunch();
bool isNativeLoginItemLaunch();
#endif
} // namespace snow_shot::platform::macos
#endif
