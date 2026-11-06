#ifndef SNOW_SHOT_PLATFORM_MACOS_LOGINITEMSERVICE_H
#define SNOW_SHOT_PLATFORM_MACOS_LOGINITEMSERVICE_H

#include <QFutureWatcher>
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

// The service lives on the owning GUI thread. Routine status queries are dispatched to a
// worker thread because reading the native login item status is a synchronous XPC round
// trip to the smd daemon that can stall the caller for an unbounded time. Registration
// changes stay on the GUI thread. Tests inject operations without touching macOS.
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
    // Requests a status refresh without blocking the caller. The updated snapshot is
    // announced through changed() once the worker finished reading the native status.
    void refresh();
    // True while a routine refresh is queued or reading the native status.
    bool refreshing() const {
        return m_queryInFlight || m_queryQueued;
    }
    LoginItemResult initialize(bool enabledByDefault);
    LoginItemResult setEnabled(bool enabled);
    void openSettings();

  signals:
    void changed();

  private:
    // Blocking query for explicit operations that must act on and report the observed
    // status in the same call. Never use this for routine refreshes.
    void refreshSynchronously();
    void startQuery();
    void queryFinished();
    void applySnapshot(LoginItemSnapshot snapshot);

    LoginItemOperations m_operations;
    LoginItemSnapshot m_snapshot;
    QFutureWatcher<LoginItemSnapshot> m_queryWatcher;
    bool m_pending = false;
    bool m_queryInFlight = false;
    bool m_queryQueued = false;
    bool m_discardQueryResult = false;
};

// Canonical paths only: callers resolve symlinks before checking containment.
bool loginItemLocationAllowed(const QString& bundle, const QString& userApplications);
QStringList loginItemLaunchArguments(QStringList arguments, bool nativeLoginLaunch);
bool loginItemAutomaticRegistrationAllowed(const QStringList& arguments);
#ifdef Q_OS_MACOS
LoginItemOperations nativeLoginItemOperations();
LoginItemService& loginItemService();
void observeNativeLoginItemLaunch();
bool initialNativeLoginItemLaunch();
bool isNativeLoginItemLaunch();
#endif
} // namespace snow_shot::platform::macos
#endif
