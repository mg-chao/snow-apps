#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/platform/windows/privilegedlocalserver.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif
#include <QTranslator>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace snow_shot::platform::windows;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void presentationStates() {
    for (const bool start : {false, true}) {
        const auto standard = administratorPresentation({true, false, false}, start, false);
        require(!standard.launchEnabled && !standard.restartEnabled &&
                    !standard.launchHint.isEmpty() && !standard.restartHint.isEmpty(),
                "standard users need disabled controls and explanations");
        const auto admin = administratorPresentation({true, true, false}, start, false);
        require(admin.launchEnabled == start && admin.restartEnabled && !admin.elevated &&
                    admin.restartLabel == u"Restart",
                "filtered administrator token must remain eligible for elevation");
        require(start == admin.launchHint.isEmpty(),
                "auto-start dependency must have a visible hint");
        const auto elevated = administratorPresentation({true, true, true}, start, false);
        require(elevated.restartEnabled && elevated.elevated &&
                    elevated.restartLabel == u"Elevated",
                "elevated button must stay enabled");
        require(administratorPresentation({true, true, true}, start, true).restartEnabled,
                "the elevated no-op button must remain clickable even during another operation");
        const auto pending = administratorPresentation({true, true, false}, start, true);
        require(!pending.launchEnabled && !pending.restartEnabled,
                "pending operation must exclude overlapping changes");
    }
    const auto unknown = administratorPresentation({}, true, false);
    require(!unknown.launchEnabled && !unknown.restartEnabled,
            "token query failures must fail closed");
}
void transactions() {
    for (const auto original :
         {StartupMode::Off, StartupMode::Registry, StartupMode::ElevatedTask}) {
        for (const auto target :
             {StartupMode::Off, StartupMode::Registry, StartupMode::ElevatedTask}) {
            for (int failedStep = -1; failedStep < 3; ++failedStep) {
                bool registry = original == StartupMode::Registry,
                     task = original == StartupMode::ElevatedTask;
                auto configured = original;
                bool restored = false;
                const auto invariant = [&] {
                    require(!(registry && task), "startup registrations must never coexist");
                };
                const auto result =
                    runStartupTransaction({[&](QString* error) {
                                               if (failedStep == 0) {
                                                   *error = QStringLiteral("delete denied");
                                                   return false;
                                               }
                                               registry = task = false;
                                               invariant();
                                               return true;
                                           },
                                           [&](QString* error) {
                                               if (failedStep == 1) {
                                                   *error = QStringLiteral("registration failed");
                                                   return false;
                                               }
                                               registry = target == StartupMode::Registry;
                                               task = target == StartupMode::ElevatedTask;
                                               invariant();
                                               return true;
                                           },
                                           [&](QString* error) {
                                               if (failedStep == 2) {
                                                   *error = QStringLiteral("disk full");
                                                   return false;
                                               }
                                               configured = target;
                                               return true;
                                           },
                                           [&](QString*) {
                                               registry = task = false;
                                               invariant();
                                               registry = original == StartupMode::Registry;
                                               task = original == StartupMode::ElevatedTask;
                                               restored = true;
                                               invariant();
                                               return true;
                                           }});
                require(result.success == (failedStep == -1),
                        "transaction outcome must reflect OS and persistence results");
                require(configured == (result.success ? target : original),
                        "failed transaction must not commit configuration");
                require(restored != result.success,
                        "every failure must restore the previous registration");
                require(
                    registry == ((result.success ? target : original) == StartupMode::Registry) &&
                        task == ((result.success ? target : original) == StartupMode::ElevatedTask),
                    "rollback must restore the original mode");
            }
        }
    }
    const auto broken =
        runStartupTransaction({[](QString*) {
                                   throw std::runtime_error("registration failure");
                                   return false;
                               },
                               [](QString*) { return true; }, [](QString*) { return true; },
                               [](QString* error) {
                                   *error = QStringLiteral("restore denied");
                                   return false;
                               }});
    require(!broken.success && broken.error.contains(u"registration failure") &&
                broken.error.contains(u"restore denied"),
            "rollback failure must not hide the initiating failure");
}
class Translator : public QTranslator {
    QString translate(const char* context, const char* source, const char*, int) const override {
        return QByteArray(context) == "AdministratorLaunch" && QByteArray(source) == "Elevated"
                   ? QStringLiteral("Localized elevated")
                   : QString();
    }
};
void restartFailures() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        bool saved = false, quit = false;
        const auto result = runRestartTransaction(
            [&](const std::function<bool()>& flush) {
                if (scenario == 0)
                    return AdministratorResult{false, true, QStringLiteral("UAC declined")};
                if (scenario == 1)
                    return AdministratorResult{false, false, QStringLiteral("invalid child")};
                if (!flush())
                    return AdministratorResult{false, false, QStringLiteral("disk full")};
                if (scenario == 3)
                    return AdministratorResult{false, false, QStringLiteral("child disconnected")};
                return AdministratorResult{true, false, {}};
            },
            [&] {
                saved = true;
                return scenario != 2;
            },
            [&] { quit = true; });
        require(
            quit == (scenario == 4) && result.success == quit,
            "restart must quit only after readiness, settings save and handoff acknowledgement");
        require(saved == (scenario >= 2),
                "failed authorization must not reach settings persistence");
        require(result.cancelled == (scenario == 0),
                "UAC cancellation must remain distinguishable for warning messages");
    }
}
void pipeIdentity() {
#ifdef Q_OS_WIN
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const QString name =
        QStringLiteral("snow-shot-admin-test-") + QUuid::createUuid().toString(QUuid::Id128);
    require(server.listen(name), "test pipe must listen");
    QLocalSocket client;
    client.connectToServer(name);
    require(client.waitForConnected(1000), "test client must connect");
    require(server.hasPendingConnections() || server.waitForNewConnection(1000),
            "test server must accept");
    std::unique_ptr<QLocalSocket> peer(server.nextPendingConnection());
    const auto pid = static_cast<quint32>(QCoreApplication::applicationPid());
    require(verifyLocalPeer(client, true, QCoreApplication::applicationFilePath(), false, pid),
            "client must verify actual pipe server PID and executable");
    require(verifyLocalPeer(*peer, false, QCoreApplication::applicationFilePath(), false, pid),
            "server must verify actual client PID and executable");
    require(!verifyLocalPeer(*peer, false, QCoreApplication::applicationFilePath(), false, pid + 1),
            "mismatching PID must be rejected");
    QTemporaryDir directory;
    QFile fake(directory.filePath(QStringLiteral("fake.exe")));
    require(fake.open(QIODevice::WriteOnly), "fake peer fixture must open");
    fake.write("not an executable");
    fake.close();
    require(!verifyLocalPeer(*peer, false, fake.fileName(), true),
            "unverified executable copies must be rejected");
    QFile executable(QCoreApplication::applicationFilePath());
    require(executable.open(QIODevice::ReadOnly), "current image must be readable");
    QCryptographicHash trusted(QCryptographicHash::Sha256);
    require(trusted.addData(&executable), "current image digest must be readable");
    require(verifyLocalPeer(*peer, false, fake.fileName(), true, pid, trusted.result()),
            "worker verification must survive replacement of the installed reference image");
#endif
}
void privilegedPipe() {
#ifdef Q_OS_WIN
    PrivilegedLocalServer server;
    const QString name =
        QStringLiteral("snow-shot-privileged-test-") + QUuid::createUuid().toString(QUuid::Id128);
    require(server.listen(name), "privileged worker pipe must listen without elevation");
    for (int index = 0; index < 3; ++index) {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&server, &PrivilegedLocalServer::newConnection, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QLocalSocket client;
        client.connectToServer(name);
        require(client.waitForConnected(1000), "privileged worker client must connect");
        timeout.start(2000);
        loop.exec();
        std::unique_ptr<QLocalSocket> accepted(server.nextPendingConnection());
        require(accepted != nullptr, "native server must accept repeated worker connections");
        require(verifyLocalPeer(*accepted, false, QCoreApplication::applicationFilePath()),
                "native pipe must preserve kernel peer identity");
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(GetSecurityInfo(reinterpret_cast<HANDLE>(accepted->socketDescriptor()),
                                SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                                nullptr, &descriptor) == ERROR_SUCCESS,
                "native pipe ACL must be readable");
        bool administrators = false, everyone = false;
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void* entry = nullptr;
            require(GetAce(acl, i, &entry) != FALSE, "ACL entries must be readable");
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
            if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE) {
                PSID sid = const_cast<DWORD*>(&ace->SidStart);
                administrators |= IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != FALSE;
                everyone |= IsWellKnownSid(sid, WinWorldSid) != FALSE;
            }
        }
        LocalFree(descriptor);
        require(administrators && !everyone,
                "worker IPC must admit administrators without granting world access");
    }
    server.close();
#endif
}
void accountIdentity() {
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
            "process token must be queryable");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray data(static_cast<qsizetype>(size), '\0');
    require(GetTokenInformation(token, TokenUser, data.data(), size, &size) != FALSE,
            "TokenUser must be readable");
    CloseHandle(token);
    PSID processSid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    LPWSTR sidText = nullptr;
    require(ConvertSidToStringSidW(processSid, &sidText) != FALSE, "process SID must stringify");
    const QString sid = QString::fromWCharArray(sidText);
    LocalFree(sidText);
    wchar_t name[256];
    wchar_t domain[256];
    DWORD nameSize = 256;
    DWORD domainSize = 256;
    SID_NAME_USE use = SidTypeInvalid;
    require(LookupAccountSidW(nullptr, processSid, name, &nameSize, domain, &domainSize, &use) !=
                FALSE,
            "process SID must resolve to an account name");
    const QString account = QString::fromWCharArray(name);
    const QString qualified = QString::fromWCharArray(domain) + QLatin1Char('\\') + account;
    require(!account.isEmpty() && account.compare(sid, Qt::CaseInsensitive) != 0,
            "Windows account names are not SID strings");
    require(sameAccountSid(sid, sid), "identical SIDs must match");
    require(sameAccountSid(sid.toLower(), sid), "SID string comparison must ignore case");
    require(sameAccountSid(account, sid),
            "Task Scheduler UserId account names must match the process SID");
    require(sameAccountSid(qualified, sid), "DOMAIN\\user UserId must match the process SID");
    require(sameAccountSid(sid, account) && sameAccountSid(qualified, account),
            "account identity comparison must be commutative");
    require(canonicalAccountSid(account) == sid && canonicalAccountSid(qualified) == sid &&
                canonicalAccountSid(sid) == sid,
            "canonical account SID must match ConvertSidToStringSid of TokenUser");
    require(!sameAccountSid(sid, QStringLiteral("S-1-5-18")) &&
                !sameAccountSid(sid, QStringLiteral("NT AUTHORITY\\SYSTEM")),
            "Local System must not match the current user");
    require(!sameAccountSid(sid, {}) && !sameAccountSid({}, sid) &&
                !sameAccountSid(QStringLiteral("missing-snow-shot-user"), sid),
            "unknown or empty principals must fail closed");
#endif
}
void translations() {
    Translator translator;
    QCoreApplication::installTranslator(&translator);
    require(administratorPresentation({true, true, true}, true, false).restartLabel ==
                u"Localized elevated",
            "dynamic labels must use the active translator");
    QCoreApplication::removeTranslator(&translator);
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        presentationStates();
        transactions();
        restartFailures();
        pipeIdentity();
        privilegedPipe();
        accountIdentity();
        translations();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
