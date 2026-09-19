#include "snow_shot/platform/windows/administratorlaunch.h"
#include <QCoreApplication>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif
#include <QTranslator>
#include <QUuid>
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
#ifdef Q_OS_WIN
QString registryValue(const QString& path) {
    wchar_t command[2048];
    DWORD bytes = sizeof(command);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, path.toStdWString().c_str(), L"SnowShot",
                                        RRF_RT_REG_SZ, nullptr, command, &bytes);
    require(status == ERROR_SUCCESS, "test registration must be readable");
    return QString::fromWCharArray(command);
}
void setRegistryValue(const QString& path, const QString& command) {
    const std::wstring value = command.toStdWString();
    require(RegSetKeyValueW(
                HKEY_CURRENT_USER, path.toStdWString().c_str(), L"SnowShot", REG_SZ, value.c_str(),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
            "test registration must be writable");
}
QByteArray processSidBytes() {
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
            "test token must open");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray data(static_cast<qsizetype>(size), '\0');
    require(GetTokenInformation(token, TokenUser, data.data(), size, &size) != FALSE,
            "test token user must be readable");
    CloseHandle(token);
    const PSID sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
    return QByteArray(reinterpret_cast<const char*>(sid),
                      static_cast<qsizetype>(GetLengthSid(sid)));
}
// Temporarily denies the current user specific access to a registry key.
class DenyAccess {
  public:
    DenyAccess(const QString& path, REGSAM permissions)
        : object(QStringLiteral("CURRENT_USER\\") + path) {
        const std::wstring name = object.toStdWString();
        require(GetNamedSecurityInfoW(name.c_str(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, &previous, nullptr,
                                      &descriptor) == ERROR_SUCCESS,
                "test key DACL must be readable");
        const QByteArray sid = processSidBytes();
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = permissions;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(const_cast<char*>(sid.constData()));
        PACL merged = nullptr;
        require(SetEntriesInAclW(1, &deny, previous, &merged) == ERROR_SUCCESS,
                "test deny ACE must merge with the key DACL");
        const LSTATUS applied =
            SetNamedSecurityInfoW(const_cast<LPWSTR>(name.c_str()), SE_REGISTRY_KEY,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, merged, nullptr);
        LocalFree(merged);
        require(applied == ERROR_SUCCESS, "test deny ACE must apply");
    }
    ~DenyAccess() {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(object.toStdWString().c_str()), SE_REGISTRY_KEY,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr, previous, nullptr);
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
    }
    DenyAccess(const DenyAccess&) = delete;
    DenyAccess& operator=(const DenyAccess&) = delete;

  private:
    QString object;
    PACL previous = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
};
void startupRunValueReconciliation() {
    const QString sid = [] {
        const QByteArray bytes = processSidBytes();
        LPWSTR text = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<PSID>(const_cast<char*>(bytes.constData())),
                                       &text) != FALSE,
                "test SID must stringify");
        const QString result = QString::fromWCharArray(text);
        LocalFree(text);
        return result;
    }();
    const QString run = QStringLiteral("Software\\SnowShotTests\\") +
                        QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral("\\Run");
    const QString usersRunKey = sid + u'\\' + run;
    const QString expected = QStringLiteral("\"C:\\SnowShotTest\\bin\\snow_shot.exe\" --autostart");
    const QString migrated =
        QStringLiteral("\"C:\\SnowShotMoved\\bin\\snow_shot.exe\" --autostart");
    const QString foreign = QStringLiteral("\"C:\\Other\\app.exe\" --autostart");
    HKEY key = nullptr;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, run.toStdWString().c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &key,
                            nullptr) == ERROR_SUCCESS,
            "test Run key must be created");
    RegCloseKey(key);

    setRegistryValue(run, expected);
    reconcileStartupRunValue(usersRunKey, expected, QString());
    require(RegGetValueW(HKEY_CURRENT_USER, run.toStdWString().c_str(), L"SnowShot", RRF_RT_REG_SZ,
                         nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND,
            "uninstall must remove a matching registration");

    setRegistryValue(run, expected);
    reconcileStartupRunValue(usersRunKey, expected, migrated);
    require(registryValue(run) == migrated, "migration must rewrite a matching registration");

    setRegistryValue(run, foreign);
    reconcileStartupRunValue(usersRunKey, expected, QString());
    require(registryValue(run) == foreign, "foreign registrations must be preserved");

    setRegistryValue(run, expected);
    {
        DenyAccess deny(run, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY);
        reconcileStartupRunValue(usersRunKey, expected, QString());
    }
    require(registryValue(run) == expected,
            "registrations in unreadable keys must be skipped instead of failing cleanup");

    {
        DenyAccess deny(run, KEY_SET_VALUE);
        bool thrown = false;
        try {
            reconcileStartupRunValue(usersRunKey, expected, QString());
        } catch (const std::exception&) {
            thrown = true;
        }
        require(thrown, "a verified registration that cannot be modified must fail loudly");
    }
    require(registryValue(run) == expected, "a failed modification must leave the value intact");

    RegDeleteKeyValueW(HKEY_CURRENT_USER, run.toStdWString().c_str(), L"SnowShot");
    reconcileStartupRunValue(usersRunKey, expected, QString());
    reconcileStartupRunValue(sid + QStringLiteral("\\Software\\SnowShotTests\\missing\\Run"),
                             expected, QString());

    RegDeleteKeyW(HKEY_CURRENT_USER, run.toStdWString().c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, run.section(u'\\', 0, -2).toStdWString().c_str());
}
#else
void startupRunValueReconciliation() {}
#endif
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        presentationStates();
        transactions();
        restartFailures();
        accountIdentity();
        translations();
        startupRunValueReconciliation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
