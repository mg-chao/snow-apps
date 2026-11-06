#ifndef SNOW_SHOT_PLATFORM_MACOS_LOGINITEMSERVICE_H
#define SNOW_SHOT_PLATFORM_MACOS_LOGINITEMSERVICE_H

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <optional>

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

// Native queries, registration and initialization markers run serially off the GUI thread.
// Preference persistence and opening System Settings remain on the owning thread.
class LoginItemService final : public QObject {
    Q_OBJECT
  public:
    explicit LoginItemService(LoginItemOperations operations, QObject* parent = nullptr);
    ~LoginItemService() override;
    const LoginItemSnapshot& snapshot() const {
        return m_snapshot;
    }
    bool pending() const {
        return m_pending;
    }
    bool refreshing() const {
        return m_running || m_refreshTimer.isActive();
    }
    bool available() const {
        return m_snapshot.status != LoginItemStatus::Unavailable;
    }
    QString hint() const;
    const QString& error() const {
        return m_error;
    }
    void refresh();
    using Completion = std::function<void(LoginItemResult)>;
    // The return value reports admission only. For admitted requests, completion reports the
    // final native/persistence result on the owning thread; completionFuture() covers imports.
    LoginItemResult initialize(bool enabledByDefault, Completion completion = {});
    LoginItemResult setEnabled(bool enabled, Completion completion = {});
    std::shared_future<LoginItemResult> completionFuture() const {
        return m_completion;
    }
    void openSettings();

  signals:
    void changed();

  private:
    LoginItemOperations m_operations;
    LoginItemSnapshot m_snapshot;
    struct Request {
        bool automatic = false;
        bool enabled = false;
        Completion completion;
        std::shared_ptr<std::promise<LoginItemResult>> promise;
    };
    LoginItemResult enqueue(bool automatic, bool enabled, Completion completion);
    void start();
    QTimer m_refreshTimer;
    std::optional<Request> m_request;
    std::shared_future<LoginItemResult> m_completion;
    std::shared_ptr<std::promise<LoginItemResult>> m_pendingPromise;
    std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
    QString m_error;
    bool m_running = false;
    bool m_refreshAgain = false;
    bool m_pending = false;
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
