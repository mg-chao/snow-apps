#include "snow_shot/platform/macos/loginitemservice.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <utility>

namespace snow_shot::platform::macos {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("LoginItemService", source);
}
} // namespace
LoginItemService::LoginItemService(LoginItemOperations operations, QObject* parent)
    : QObject(parent), m_operations(std::move(operations)) {
    m_refreshTimer.setSingleShot(true);
    connect(&m_refreshTimer, &QTimer::timeout, this, &LoginItemService::start);
    refresh();
}
LoginItemService::~LoginItemService() {
    // A hung system service must not hold up shutdown. Workers own their inputs/results and
    // their finished connections are automatically disconnected when this receiver is destroyed.
    m_cancelled->store(true);
    if (m_pendingPromise)
        m_pendingPromise->set_value({false, {}});
}
void LoginItemService::refresh() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_pending || m_running) {
        // Retain one invalidation (including language changes) that arrives after work starts.
        m_refreshAgain = true;
        return;
    }
    if (!m_refreshTimer.isActive())
        m_refreshTimer.start(0);
}
QString LoginItemService::hint() const {
    if (!m_snapshot.error.isEmpty())
        return m_snapshot.error;
    if (m_snapshot.status == LoginItemStatus::ApprovalRequired)
        return text(QT_TRANSLATE_NOOP(
            "LoginItemService",
            "Approval required. Allow Snow Shot in System Settings > General > Login Items"));
    return {};
}
LoginItemResult LoginItemService::initialize(bool enabledByDefault, Completion completion) {
    return enqueue(true, enabledByDefault, std::move(completion));
}
LoginItemResult LoginItemService::setEnabled(bool enabled, Completion completion) {
    return enqueue(false, enabled, std::move(completion));
}
LoginItemResult LoginItemService::enqueue(bool automatic, bool enabled, Completion completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_pending) {
        LoginItemResult result{
            false, text(QT_TRANSLATE_NOOP("LoginItemService",
                                          "A launch-at-login change is already in progress"))};
        return result;
    }
    auto promise = std::make_shared<std::promise<LoginItemResult>>();
    m_pendingPromise = promise;
    m_completion = promise->get_future().share();
    m_request = Request{automatic, enabled, std::move(completion), std::move(promise)};
    m_pending = true;
    m_error.clear();
    m_refreshAgain = false;
    if (!m_running)
        m_refreshTimer.start(0);
    emit changed();
    return {};
}
void LoginItemService::start() {
    if (m_running)
        return;
    struct Outcome {
        LoginItemSnapshot snapshot;
        LoginItemResult result;
        bool savePreference = false;
    };
    auto request = std::exchange(m_request, std::nullopt);
    auto outcome = std::make_shared<Outcome>();
    const bool mutation = request.has_value();
    const bool automatic = mutation && request->automatic;
    const bool enabled = mutation && request->enabled;
    m_running = true;
    m_refreshAgain = false;
    // Do not capture this or use the application-wide pool: its destructor waits for work,
    // including a potentially unbounded SMAppService XPC call.
    auto* worker = QThread::create([operations = m_operations, cancelled = m_cancelled, outcome,
                                    mutation, automatic, enabled] {
        if (cancelled->load())
            return;
        outcome->snapshot = operations.query();
        if (!mutation || cancelled->load())
            return;
        if (outcome->snapshot.status == LoginItemStatus::Unavailable) {
            if (!automatic)
                outcome->result = {false, outcome->snapshot.error};
            return;
        }
        if (automatic && operations.initialized())
            return;
        if (cancelled->load())
            return;
        if (!operations.markInitialized()) {
            outcome->result = {
                false, text(QT_TRANSLATE_NOOP("LoginItemService",
                                              "Could not save launch-at-login initialization. "
                                              "Registration was not changed"))};
            return;
        }
        // Existing registration, revoked consent and an explicit opt-out take precedence.
        if (automatic && (outcome->snapshot.requested() || !enabled))
            return;
        if (cancelled->load())
            return;
        if (enabled != outcome->snapshot.requested()) {
            outcome->result = operations.setEnabled(enabled);
            if (cancelled->load())
                return;
            outcome->snapshot = operations.query();
        }
        const bool available = outcome->snapshot.status != LoginItemStatus::Unavailable;
        if (outcome->result.success && (!available || enabled != outcome->snapshot.requested()))
            outcome->result = {
                false, text(QT_TRANSLATE_NOOP("LoginItemService",
                                              "macOS did not apply the launch-at-login change. "
                                              "Check Login Items in System Settings"))};
        outcome->savePreference = available;
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this,
            [this, outcome, request = std::move(request)]() mutable {
                const QPointer<LoginItemService> guard(this);
                const bool snapshotChanged = outcome->snapshot.status != m_snapshot.status ||
                                             outcome->snapshot.error != m_snapshot.error;
                m_snapshot = std::move(outcome->snapshot);
                if (outcome->savePreference) {
                    const auto savePreference = m_operations.savePreference;
                    const bool saved = savePreference(m_snapshot.requested());
                    if (!guard)
                        return;
                    if (!saved) {
                        if (!outcome->result.error.isEmpty())
                            outcome->result.error += QLatin1Char('\n');
                        outcome->result.success = false;
                        outcome->result.error += text(QT_TRANSLATE_NOOP(
                            "LoginItemService", "Could not save the launch-at-login preference. "
                                                "The displayed macOS status is still current"));
                    }
                }
                m_running = false;
                if (request) {
                    m_error = outcome->result.error;
                    m_pending = false;
                    request->promise->set_value(outcome->result);
                    m_pendingPromise.reset();
                }
                if (m_request || m_refreshAgain)
                    m_refreshTimer.start(0);
                if (snapshotChanged || request)
                    emit changed();
                if (!guard || !request)
                    return;
                if (request->completion)
                    request->completion(outcome->result);
            });
    worker->start();
}
void LoginItemService::openSettings() {
    if (!m_pending)
        m_operations.openSettings();
}
bool loginItemLocationAllowed(const QString& bundle, const QString& userApplications) {
    return bundle.endsWith(QStringLiteral(".app")) &&
           (bundle.startsWith(QStringLiteral("/Applications/")) ||
            (!userApplications.isEmpty() &&
             bundle.startsWith(userApplications + QLatin1Char('/'))));
}
QStringList loginItemLaunchArguments(QStringList arguments, bool nativeLoginLaunch) {
    if (nativeLoginLaunch && !arguments.contains(QStringLiteral("--autostart")))
        arguments.append(QStringLiteral("--autostart"));
    return arguments;
}
bool loginItemAutomaticRegistrationAllowed(const QStringList& arguments) {
    for (const auto& argument : arguments) {
        if (argument.startsWith(QStringLiteral("--e2e-")) ||
            (argument.startsWith(QStringLiteral("--")) &&
             argument.endsWith(QStringLiteral("-probe"))) ||
            argument == QStringLiteral("--self-test") ||
            argument == QStringLiteral("--administrator-helper"))
            return false;
    }
    return true;
}
} // namespace snow_shot::platform::macos
