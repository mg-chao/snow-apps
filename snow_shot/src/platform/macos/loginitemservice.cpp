#include "snow_shot/platform/macos/loginitemservice.h"

#include <QCoreApplication>
#include <QScopedValueRollback>
#include <QtConcurrentRun>
#include <utility>

namespace snow_shot::platform::macos {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("LoginItemService", source);
}
} // namespace
LoginItemService::LoginItemService(LoginItemOperations operations, QObject* parent)
    : QObject(parent), m_operations(std::move(operations)) {
    connect(&m_queryWatcher, &QFutureWatcher<LoginItemSnapshot>::finished, this,
            &LoginItemService::queryFinished);
    refresh();
}
void LoginItemService::refresh() {
    if (m_pending)
        return;
    if (m_queryInFlight) {
        // Coalesce bursts (page show, activation, language change) into one follow-up
        // query instead of starving the worker pool with concurrent XPC calls.
        m_queryQueued = true;
        return;
    }
    startQuery();
}
void LoginItemService::refreshSynchronously() {
    if (m_pending)
        return;
    // A routine query may still be running. Its snapshot predates the one read below, so
    // forget it instead of letting it roll the result of an explicit operation back.
    m_queryQueued = false;
    if (m_queryInFlight)
        m_discardQueryResult = true;
    applySnapshot(m_operations.query());
}
void LoginItemService::startQuery() {
    m_queryInFlight = true;
    m_queryQueued = false;
    m_queryWatcher.setFuture(QtConcurrent::run(m_operations.query));
}
void LoginItemService::queryFinished() {
    LoginItemSnapshot snapshot = m_queryWatcher.result();
    const bool discard = m_discardQueryResult;
    const bool queued = m_queryQueued;
    m_queryInFlight = false;
    m_queryQueued = false;
    m_discardQueryResult = false;
    if (!discard && !m_pending)
        applySnapshot(std::move(snapshot));
    if (queued && !m_queryInFlight)
        startQuery();
}
void LoginItemService::applySnapshot(LoginItemSnapshot snapshot) {
    if (snapshot.status == m_snapshot.status && snapshot.error == m_snapshot.error)
        return;
    m_snapshot = std::move(snapshot);
    emit changed();
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
LoginItemResult LoginItemService::initialize(bool enabledByDefault) {
    refreshSynchronously();
    if (!available() || m_operations.initialized())
        return {};
    if (!m_operations.markInitialized())
        return {
            false,
            text(QT_TRANSLATE_NOOP(
                "LoginItemService",
                "Could not save launch-at-login initialization. Registration was not changed"))};
    // An existing native registration (including revoked consent) takes precedence.
    if (m_snapshot.requested() || !enabledByDefault)
        return {};
    return setEnabled(true);
}
LoginItemResult LoginItemService::setEnabled(bool enabled) {
    if (m_pending)
        return {false, text(QT_TRANSLATE_NOOP("LoginItemService",
                                              "A launch-at-login change is already in progress"))};
    refreshSynchronously();
    if (!available())
        return {false, hint()};
    if (!m_operations.markInitialized())
        return {
            false,
            text(QT_TRANSLATE_NOOP(
                "LoginItemService",
                "Could not save launch-at-login initialization. Registration was not changed"))};
    LoginItemResult result;
    {
        QScopedValueRollback<bool> pending(m_pending, true);
        emit changed();
        if (enabled != m_snapshot.requested())
            result = m_operations.setEnabled(enabled);
        m_snapshot = m_operations.query();
        if (result.success && (!available() || enabled != m_snapshot.requested()))
            result = {false,
                      text(QT_TRANSLATE_NOOP("LoginItemService",
                                             "macOS did not apply the launch-at-login change. "
                                             "Check Login Items in System Settings"))};
        if (available() && !m_operations.savePreference(m_snapshot.requested())) {
            if (!result.error.isEmpty())
                result.error += QLatin1Char('\n');
            result.success = false;
            result.error += text(QT_TRANSLATE_NOOP("LoginItemService",
                                                   "Could not save the launch-at-login preference. "
                                                   "The displayed macOS status is still current"));
        }
    }
    emit changed();
    return result;
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
