#ifndef SNOW_SHOT_PLATFORM_WINDOWS_ADMINISTRATORLAUNCH_H
#define SNOW_SHOT_PLATFORM_WINDOWS_ADMINISTRATORLAUNCH_H

#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
class QLocalSocket;

namespace snow_shot::platform::windows {
enum class StartupMode { Off, Registry, ElevatedTask };
struct AdministratorState {
    bool valid = false;
    bool member = false;
    bool elevated = false;
};
struct AdministratorPresentation {
    bool launchEnabled = false;
    bool restartEnabled = false;
    bool elevated = false;
    QString launchHint;
    QString restartHint;
    QString restartLabel;
};
AdministratorPresentation administratorPresentation(AdministratorState state, bool autoStart,
                                                    bool pending);
struct AdministratorResult {
    bool success = false;
    bool cancelled = false;
    QString error;
};

// Injectable transaction boundary: configuration is committed only after OS state succeeds.
struct StartupTransactionOperations {
    std::function<bool(QString*)> removePrevious;
    std::function<bool(QString*)> installRequested;
    std::function<bool(QString*)> persist;
    std::function<bool(QString*)> restore;
};
AdministratorResult runRestartTransaction(
    const std::function<AdministratorResult(const std::function<bool()>&)>& prepare,
    const std::function<bool()>& flush, const std::function<void()>& quit);
AdministratorResult runStartupTransaction(const StartupTransactionOperations& operations);
AdministratorState administratorState();
std::optional<StartupMode> observedStartupMode();
bool verifyLocalPeer(QLocalSocket& socket, bool serverPeer, const QString& expectedExecutable,
                     bool allowVerifiedCopy = false, quint32 expectedPid = 0,
                     const QByteArray& expectedDigest = {});
// Task Scheduler UserId may be a SID, DOMAIN\user, UPN, or SAM account name.
QString canonicalAccountSid(const QString& accountOrSid);
bool sameAccountSid(const QString& left, const QString& right);
bool administratorOperationPending();
void setAdministratorRestartGuard(std::function<bool()> guard);
AdministratorResult changeStartupMode(StartupMode mode, const std::function<bool()>& persist);
AdministratorResult restartAsAdministrator(const std::function<bool()>& flush);
// Returns -1 for an ordinary launch, 0 for a completed helper, or a failure exit code.
// A successful restart handoff returns -1 after the parent has exited.
int dispatchAdministratorHelper(const QStringList& arguments);
AdministratorResult reconcileStartupMode(StartupMode mode);
// Reconciles the auto-start Run value under an HKEY_USERS-relative Run key path (for
// example "S-1-5-21-...\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"). Keys that are
// missing or deny read access cannot contain a verifiable Snow Shot registration and are
// skipped; throws only when a matching registration cannot be removed or rewritten.
void reconcileStartupRunValue(const QString& usersRunKey, const QString& expectedCommand,
                              const QString& replacementCommand);
// Installer-only operations. Cleanup is restricted to registrations targeting this root.
AdministratorResult removeInstallationStartup(const QString& root);
AdministratorResult migrateInstallationStartup(const QString& previousRoot, const QString& root);
bool launchOnInteractiveDesktop(const QString& executable);
} // namespace snow_shot::platform::windows
#endif
