#include "snow_shot/platform/macos/loginitemservice.h"

#include <QCoreApplication>
#include <QScopedValueRollback>
#include <utility>

namespace snow_shot::platform::macos {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("LoginItemService", source);
}
} // namespace
LoginItemService::LoginItemService(LoginItemOperations operations, QObject* parent)
    : QObject(parent), m_operations(std::move(operations)) {
    refresh();
}
void LoginItemService::refresh() {
    if (m_pending)
        return;
    m_snapshot = m_operations.query();
    emit changed();
}
QString LoginItemService::hint() const {
    if (!m_snapshot.error.isEmpty())
        return m_snapshot.error;
    if (m_snapshot.status == LoginItemStatus::ApprovalRequired)
        return text(QT_TRANSLATE_NOOP(
            "LoginItemService",
            "Approval required. Allow Snow Shot in System Settings > General > Login Items."));
    return {};
}
LoginItemResult LoginItemService::initialize(bool enabledByDefault) {
    refresh();
    if (!available() || m_operations.initialized())
        return {};
    if (!m_operations.markInitialized())
        return {
            false,
            text(QT_TRANSLATE_NOOP(
                "LoginItemService",
                "Could not save launch-at-login initialization. Registration was not changed."))};
    // An existing native registration (including revoked consent) takes precedence.
    if (m_snapshot.requested() || !enabledByDefault)
        return {};
    return setEnabled(true);
}
LoginItemResult LoginItemService::setEnabled(bool enabled) {
    if (m_pending)
        return {false, text(QT_TRANSLATE_NOOP("LoginItemService",
                                              "A launch-at-login change is already in progress."))};
    refresh();
    if (!available())
        return {false, hint()};
    if (!m_operations.markInitialized())
        return {
            false,
            text(QT_TRANSLATE_NOOP(
                "LoginItemService",
                "Could not save launch-at-login initialization. Registration was not changed."))};
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
                                             "Check Login Items in System Settings."))};
        if (available() && !m_operations.savePreference(m_snapshot.requested())) {
            if (!result.error.isEmpty())
                result.error += QLatin1Char('\n');
            result.success = false;
            result.error += text(QT_TRANSLATE_NOOP("LoginItemService",
                                                   "Could not save the launch-at-login preference. "
                                                   "The displayed macOS status is still current."));
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
