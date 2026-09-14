#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/platform/windows/autostartregistration.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <future>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <taskschd.h>
#include <shldisp.h>
#include <exdisp.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <comutil.h>
#endif

namespace snow_shot::platform::windows {
namespace {
bool operationPending = false;
std::function<bool()> restartGuard;
QString text(const char* source) {
    return QCoreApplication::translate("AdministratorLaunch", source);
}
AdministratorResult failure(const QString& detail) {
    return {false, false, detail};
}
#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
void check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error(QStringLiteral("Windows error 0x%1")
                                     .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'))
                                     .toStdString());
}
QString executablePath() {
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}
QString processPath(HANDLE process) {
    wchar_t path[32768];
    DWORD size = 32768;
    return QueryFullProcessImageNameW(process, 0, path, &size)
               ? QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(path, size)))
               : QString();
}
QString userSid(HANDLE process = GetCurrentProcess()) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value))
        return {};
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    QByteArray data(static_cast<qsizetype>(size), '\0');
    if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size))
        return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
        return {};
    const QString value = QString::fromWCharArray(sid);
    LocalFree(sid);
    return value;
}
bool sameProcessIdentity(HANDLE process) {
    const QString sid = userSid();
    return !sid.isEmpty() && userSid(process) == sid &&
           processPath(process).compare(processPath(GetCurrentProcess()), Qt::CaseInsensitive) == 0;
}
bool peerMatches(QLocalSocket& socket, DWORD expected, bool serverPeer) {
    ULONG pid = 0;
    const HANDLE pipe = reinterpret_cast<HANDLE>(socket.socketDescriptor());
    return (serverPeer ? GetNamedPipeServerProcessId(pipe, &pid)
                       : GetNamedPipeClientProcessId(pipe, &pid)) &&
           pid == expected;
}
QString taskName(const QString& executable, const QString& sid) {
    const QByteArray identity =
        QDir::cleanPath(QDir::fromNativeSeparators(executable)).toCaseFolded().toUtf8();
    return QStringLiteral("SnowShot-%1-%2")
        .arg(sid,
             QString::fromLatin1(
                 QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(24)));
}
struct Tasks {
    ComApartment apartment;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    QString executable = executablePath();
    QString sid = userSid();
    QString name = taskName(executable, sid);
    Tasks() {
        check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&service)));
        check(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()));
        check(service->GetFolder(_bstr_t(L"\\"), &folder));
    }
    ComPtr<IRegisteredTask> current() {
        ComPtr<IRegisteredTask> task;
        const HRESULT result = folder->GetTask(_bstr_t(name.toStdWString().c_str()), &task);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return {};
        check(result);
        validate(task.Get());
        return task;
    }
    void validate(IRegisteredTask* task) {
        ComPtr<ITaskDefinition> definition;
        check(task->get_Definition(&definition));
        ComPtr<IPrincipal> principal;
        check(definition->get_Principal(&principal));
        BSTR user = nullptr;
        check(principal->get_UserId(&user));
        const QString owner = QString::fromWCharArray(user);
        SysFreeString(user);
        ComPtr<IActionCollection> actions;
        check(definition->get_Actions(&actions));
        LONG count = 0;
        check(actions->get_Count(&count));
        if (owner != sid || count != 1)
            throw std::runtime_error(
                QT_TRANSLATE_NOOP("AdministratorLaunch", "Startup task ownership mismatch"));
        ComPtr<IAction> action;
        check(actions->get_Item(1, &action));
        ComPtr<IExecAction> exec;
        check(action.As(&exec));
        BSTR path = nullptr;
        check(exec->get_Path(&path));
        const QString target =
            QDir::cleanPath(QDir::fromNativeSeparators(QString::fromWCharArray(path)));
        SysFreeString(path);
        BSTR args = nullptr;
        check(exec->get_Arguments(&args));
        const QString arguments = QString::fromWCharArray(args);
        SysFreeString(args);
        if (target.compare(QDir::cleanPath(QDir::fromNativeSeparators(executable)),
                           Qt::CaseInsensitive) != 0 ||
            arguments != QStringLiteral("--autostart"))
            throw std::runtime_error(
                QT_TRANSLATE_NOOP("AdministratorLaunch", "Startup task target mismatch"));
    }
    void remove() {
        if (current())
            check(folder->DeleteTask(_bstr_t(name.toStdWString().c_str()), 0));
    }
    QString xml() {
        auto task = current();
        if (!task)
            return {};
        BSTR value = nullptr;
        check(task->get_Xml(&value));
        const QString result = QString::fromWCharArray(value);
        SysFreeString(value);
        return result;
    }
    QString securityDescriptor() {
        auto task = current();
        if (!task)
            return {};
        BSTR value = nullptr;
        check(task->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &value));
        const QString result = QString::fromWCharArray(value);
        SysFreeString(value);
        return result;
    }
    void restoreXml(const QString& xml, const QString& security) {
        ComPtr<IRegisteredTask> task;
        check(folder->RegisterTask(
            _bstr_t(name.toStdWString().c_str()), _bstr_t(xml.toStdWString().c_str()), TASK_CREATE,
            _variant_t(sid.toStdWString().c_str()), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(security.toStdWString().c_str()), &task));
    }
    void create() {
        ComPtr<ITaskDefinition> definition;
        check(service->NewTask(0, &definition));
        ComPtr<IPrincipal> principal;
        check(definition->get_Principal(&principal));
        check(principal->put_UserId(_bstr_t(sid.toStdWString().c_str())));
        check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
        check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
        ComPtr<ITaskSettings> settings;
        check(definition->get_Settings(&settings));
        check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
        check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
        check(settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S")));
        check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
        ComPtr<ITriggerCollection> triggers;
        check(definition->get_Triggers(&triggers));
        ComPtr<ITrigger> trigger;
        check(triggers->Create(TASK_TRIGGER_LOGON, &trigger));
        ComPtr<ILogonTrigger> logon;
        check(trigger.As(&logon));
        check(logon->put_UserId(_bstr_t(sid.toStdWString().c_str())));
        ComPtr<IActionCollection> actions;
        check(definition->get_Actions(&actions));
        ComPtr<IAction> action;
        check(actions->Create(TASK_ACTION_EXEC, &action));
        ComPtr<IExecAction> exec;
        check(action.As(&exec));
        check(exec->put_Path(_bstr_t(executable.toStdWString().c_str())));
        check(exec->put_Arguments(_bstr_t(L"--autostart")));
        check(exec->put_WorkingDirectory(
            _bstr_t(QFileInfo(executable).absolutePath().toStdWString().c_str())));
        ComPtr<IRegisteredTask> task;
        check(folder->RegisterTaskDefinition(
            _bstr_t(name.toStdWString().c_str()), definition.Get(), TASK_CREATE,
            _variant_t(sid.toStdWString().c_str()), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t((QStringLiteral("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;") + sid + u')')
                           .toStdWString()
                           .c_str()),
            &task));
    }
};
QString journalPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("startup-") + taskName(executablePath(), userSid()) +
                  QStringLiteral(".json"));
}
struct RegistrationTransaction {
    Tasks tasks;
    AutoStartRegistrationSnapshot registry = AutoStartRegistration::snapshot();
    QString taskXml;
    QString taskSecurity;
    bool active = false;
    RegistrationTransaction() {
        if (!registry.valid)
            throw std::runtime_error(registry.error.toStdString());
        taskXml = tasks.xml();
        taskSecurity = tasks.securityDescriptor();
    }
    void begin() {
        if (QFile::exists(journalPath()))
            throw std::runtime_error(QT_TRANSLATE_NOOP(
                "AdministratorLaunch", "An interrupted startup change needs recovery"));
        QDir().mkpath(QFileInfo(journalPath()).absolutePath());
        QSaveFile journal(journalPath());
        const QByteArray data =
            QJsonDocument(QJsonObject{{QStringLiteral("task"), !taskXml.isEmpty()},
                                      {QStringLiteral("registry"), registry.exists}})
                .toJson();
        if (!journal.open(QIODevice::WriteOnly) || journal.write(data) != data.size() ||
            !journal.commit())
            throw std::runtime_error(
                QT_TRANSLATE_NOOP("AdministratorLaunch", "Could not save startup recovery record"));
        active = true;
    }
    void apply(StartupMode mode) {
        tasks.remove();
        QString error;
        if (!AutoStartRegistration::setEnabled(false, &error))
            throw std::runtime_error(error.toStdString());
        if (mode == StartupMode::ElevatedTask)
            tasks.create();
        else if (mode == StartupMode::Registry && !AutoStartRegistration::setEnabled(true, &error))
            throw std::runtime_error(error.toStdString());
    }
    bool rollback(QString* error) {
        if (!active)
            return true;
        try {
            tasks.remove();
            if (!AutoStartRegistration::setEnabled(false, error))
                return false;
            if (!taskXml.isEmpty())
                tasks.restoreXml(taskXml, taskSecurity);
            // An existing conflicting registry entry must not be recreated beside a task.
            if (taskXml.isEmpty() && !AutoStartRegistration::restore(registry, error))
                return false;
            finish();
            return true;
        } catch (const std::exception& e) {
            *error = QString::fromUtf8(e.what());
            return false;
        }
    }
    void finish() {
        if (QFile::exists(journalPath()) && !QFile::remove(journalPath()))
            throw std::runtime_error(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                                       "Could not clear startup recovery record"));
        active = false;
    }
    ~RegistrationTransaction() {
        if (active) {
            QString error;
            rollback(&error);
        }
    }
};
QByteArray receive(QLocalSocket& socket) {
    QElapsedTimer timer;
    timer.start();
    while (!socket.canReadLine()) {
        if (socket.bytesAvailable() >= 4096 || timer.elapsed() >= 180000 ||
            !socket.waitForReadyRead(static_cast<int>(180000 - timer.elapsed())))
            throw std::runtime_error(QT_TRANSLATE_NOOP(
                "AdministratorLaunch", "Privilege handoff timed out or disconnected"));
    }
    const QByteArray line = socket.readLine(4096).trimmed();
    if (line.isEmpty())
        throw std::runtime_error(
            QT_TRANSLATE_NOOP("AdministratorLaunch", "Privilege handoff disconnected"));
    return line;
}
void send(QLocalSocket& socket, const QByteArray& line) {
    if (socket.write(line + '\n') < 0 ||
        (socket.bytesToWrite() && !socket.waitForBytesWritten(10000)))
        throw std::runtime_error(
            QT_TRANSLATE_NOOP("AdministratorLaunch", "Privilege handoff disconnected"));
}
struct LaunchResult {
    HANDLE process = nullptr;
    DWORD error = 0;
};
LaunchResult launchHelper(const QString& pipe) {
    const QString args = QStringLiteral("--administrator-helper %1 %2")
                             .arg(QString::number(QCoreApplication::applicationPid()), pipe);
    const std::wstring path = executablePath().toStdWString(), parameters = args.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    return ShellExecuteExW(&info) ? LaunchResult{info.hProcess, 0}
                                  : LaunchResult{nullptr, GetLastError()};
}
AdministratorResult withHelper(const QByteArray& operation, const std::function<bool()>& persist) {
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const QString pipe =
        QStringLiteral("snow-shot-admin-") + QUuid::createUuid().toString(QUuid::Id128);
    if (!server.listen(pipe))
        return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                              "Could not create the administrator handoff.")));
    auto future = std::async(std::launch::async, [pipe] {
        ComApartment apartment;
        return launchHelper(pipe);
    });
    // Keep painting and cancellation responsive while Windows owns the authorization dialog.
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            loop.quit();
    });
    poll.start(25);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    const auto launched = future.get();
    Handle child{launched.process};
    if (!child.value)
        return {false, launched.error == ERROR_CANCELLED,
                launched.error == ERROR_CANCELLED
                    ? text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                             "Administrator authorization was declined."))
                    : text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                             "Could not start the administrator helper."))};
    if (!server.hasPendingConnections() && !server.waitForNewConnection(30000))
        return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                              "The administrator helper did not become ready.")));
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    if (!socket || !peerMatches(*socket, GetProcessId(child.value), false) ||
        !sameProcessIdentity(child.value))
        return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                              "The administrator handoff could not be verified.")));
    try {
        send(*socket, operation);
        const auto ready = receive(*socket);
        if (ready != "ready")
            return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                                  "The administrator operation failed: %1"))
                               .arg(text(ready.constData())));
        const bool saved = persist();
        send(*socket, saved ? QByteArrayLiteral("commit") : QByteArrayLiteral("rollback"));
        const auto result = receive(*socket);
        if (!saved)
            return failure(
                text(QT_TRANSLATE_NOOP(
                    "AdministratorLaunch",
                    "Settings could not be saved. The administrator operation was cancelled.")) +
                (result == "done" ? QString() : u' ' + QString::fromUtf8(result)));
        if (result != "done")
            return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                                  "The administrator operation failed: %1"))
                               .arg(text(result.constData())));
        return {true, false, {}};
    } catch (const std::exception& e) {
        return failure(
            text(QT_TRANSLATE_NOOP("AdministratorLaunch", "The administrator operation failed: %1"))
                .arg(text(e.what())));
    }
}
#endif
} // namespace

AdministratorResult runRestartTransaction(
    const std::function<AdministratorResult(const std::function<bool()>&)>& prepare,
    const std::function<bool()>& flush, const std::function<void()>& quit) {
    const auto result = prepare(flush);
    if (result.success)
        quit();
    return result;
}
AdministratorResult runStartupTransaction(const StartupTransactionOperations& operations) {
    QString error;
    try {
        if (operations.removePrevious(&error) && operations.installRequested(&error) &&
            operations.persist(&error))
            return {true, false, {}};
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
    QString rollback;
    bool restored = false;
    try {
        restored = operations.restore(&rollback);
    } catch (const std::exception& e) {
        rollback = QString::fromUtf8(e.what());
    }
    if (!restored)
        error +=
            text(QT_TRANSLATE_NOOP("AdministratorLaunch", " Recovery failed: %1")).arg(rollback);
    return failure(error);
}
AdministratorPresentation administratorPresentation(AdministratorState state, bool autoStart,
                                                    bool pending) {
    AdministratorPresentation result;
    const bool allowed = state.valid && state.member;
    result.launchEnabled = allowed && autoStart && !pending;
    result.restartEnabled = state.valid && (state.elevated || (state.member && !pending));
    result.elevated = state.valid && state.elevated;
    result.restartLabel = result.elevated
                              ? text(QT_TRANSLATE_NOOP("AdministratorLaunch", "Elevated"))
                              : text(QT_TRANSLATE_NOOP("AdministratorLaunch", "Restart"));
    if (!allowed)
        result.launchHint = text(
            QT_TRANSLATE_NOOP("AdministratorLaunch",
                              "This feature requires membership in the Administrators group."));
    else if (!autoStart)
        result.launchHint =
            text(QT_TRANSLATE_NOOP("AdministratorLaunch", "Turn on Auto start at boot first."));
    if (!allowed && !result.elevated)
        result.restartHint = result.launchHint;
    return result;
}
AdministratorState administratorState() {
#ifdef Q_OS_WIN
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return {};
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size))
        return {};
    GetTokenInformation(token.value, TokenGroups, nullptr, 0, &size);
    QByteArray buffer(static_cast<qsizetype>(size), '\0');
    if (!GetTokenInformation(token.value, TokenGroups, buffer.data(), size, &size))
        return {};
    BYTE admin[SECURITY_MAX_SID_SIZE];
    DWORD adminSize = sizeof(admin);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &adminSize))
        return {};
    const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buffer.data());
    bool member = false;
    for (DWORD i = 0; i < groups->GroupCount; ++i)
        member |= EqualSid(groups->Groups[i].Sid, admin) != FALSE;
    return {true, member, elevation.TokenIsElevated != 0};
#else
    return {};
#endif
}
std::optional<StartupMode> observedStartupMode() {
#ifdef Q_OS_WIN
    try {
        Tasks tasks;
        if (tasks.current())
            return StartupMode::ElevatedTask;
        const auto registry = AutoStartRegistration::snapshot();
        if (!registry.valid)
            return std::nullopt;
        return registry.exists ? StartupMode::Registry : StartupMode::Off;
    } catch (...) {
        return std::nullopt;
    }
#else
    return std::nullopt;
#endif
}
bool verifyLocalPeer(QLocalSocket& socket, bool serverPeer, const QString& expectedExecutable,
                     bool allowVerifiedCopy, quint32 expectedPid,
                     const QByteArray& expectedDigest) {
#ifdef Q_OS_WIN
    ULONG pid = 0;
    const HANDLE pipe = reinterpret_cast<HANDLE>(socket.socketDescriptor());
    if (!(serverPeer ? GetNamedPipeServerProcessId(pipe, &pid)
                     : GetNamedPipeClientProcessId(pipe, &pid)) ||
        (expectedPid && pid != expectedPid))
        return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value)
        return false;
    // An elevated worker may belong to the alternate administrator used for UAC.
    // Executable validation is required in either direction; same-user endpoints are additionally
    // ACL protected.
    const QString path = processPath(process.value);
    if (path.isEmpty())
        return false;
    const QString expected = QDir::cleanPath(QDir::fromNativeSeparators(expectedExecutable));
    if (path.compare(expected, Qt::CaseInsensitive) == 0)
        return true;
    if (!allowVerifiedCopy)
        return false;
    QFile actual(path);
    if (!actual.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash a(QCryptographicHash::Sha256);
    if (!a.addData(&actual))
        return false;
    if (!expectedDigest.isEmpty())
        return a.result() == expectedDigest;
    QFile reference(expected);
    if (!reference.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash b(QCryptographicHash::Sha256);
    return b.addData(&reference) && a.result() == b.result();
#else
    Q_UNUSED(socket);
    Q_UNUSED(serverPeer);
    Q_UNUSED(expectedExecutable);
    Q_UNUSED(allowVerifiedCopy);
    Q_UNUSED(expectedPid);
    Q_UNUSED(expectedDigest);
    return false;
#endif
}
bool administratorOperationPending() {
    return operationPending;
}
void setAdministratorRestartGuard(std::function<bool()> guard) {
    restartGuard = std::move(guard);
}
AdministratorResult changeStartupMode(StartupMode mode, const std::function<bool()>& persist) {
    if (operationPending)
        return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                              "Another administrator operation is in progress.")));
    QScopedValueRollback<bool> pending(operationPending, true);
#ifdef Q_OS_WIN
    try {
        Tasks tasks;
        if (mode == StartupMode::ElevatedTask || tasks.current() || QFile::exists(journalPath())) {
            const auto state = administratorState();
            if (!state.valid || !state.member)
                return failure(text(QT_TRANSLATE_NOOP(
                    "AdministratorLaunch",
                    "This feature requires membership in the Administrators group.")));
            // Even elevated callers use the same isolated transaction lifetime and protocol.
            return withHelper(QByteArray::number(static_cast<int>(mode)), persist);
        }
        const auto previous = AutoStartRegistration::snapshot();
        if (!previous.valid)
            return failure(previous.error);
        return runStartupTransaction(
            {[](QString* error) { return AutoStartRegistration::setEnabled(false, error); },
             [mode](QString* error) {
                 return mode != StartupMode::Registry ||
                        AutoStartRegistration::setEnabled(true, error);
             },
             [&persist](QString* error) {
                 if (persist())
                     return true;
                 *error =
                     text(QT_TRANSLATE_NOOP("AdministratorLaunch", "Settings could not be saved."));
                 return false;
             },
             [&previous](QString* error) {
                 return AutoStartRegistration::restore(previous, error);
             }});
    } catch (const std::exception& e) {
        return failure(
            text(QT_TRANSLATE_NOOP("AdministratorLaunch", "The administrator operation failed: %1"))
                .arg(text(e.what())));
    }
#else
    Q_UNUSED(mode);
    Q_UNUSED(persist);
    return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                          "Administrator launch is only supported on Windows.")));
#endif
}
AdministratorResult restartAsAdministrator(const std::function<bool()>& flush) {
    const auto state = administratorState();
    if (state.elevated)
        return {true, false, {}};
    if (!state.valid || !state.member)
        return failure(text(
            QT_TRANSLATE_NOOP("AdministratorLaunch",
                              "This feature requires membership in the Administrators group.")));
    if (operationPending)
        return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                              "Another administrator operation is in progress.")));
    QScopedValueRollback<bool> pending(operationPending, true);
#ifdef Q_OS_WIN
    if (restartGuard && !restartGuard())
        return failure(text(QT_TRANSLATE_NOOP(
            "AdministratorLaunch",
            "Finish capturing, recording, exporting, or updating before restarting.")));
    return runRestartTransaction(
        [](const std::function<bool()>& save) {
            return withHelper(QByteArrayLiteral("restart"), save);
        },
        [&] { return (!restartGuard || restartGuard()) && flush(); },
        [] { QCoreApplication::quit(); });
#else
    Q_UNUSED(flush);
    return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                          "Administrator launch is only supported on Windows.")));
#endif
}
int dispatchAdministratorHelper(const QStringList& arguments) {
    if (arguments.size() < 2 || arguments[1] != u"--administrator-helper")
        return -1;
#ifdef Q_OS_WIN
    if (arguments.size() != 4 || !administratorState().elevated)
        return 20;
    bool valid = false;
    const DWORD pid = arguments[2].toUInt(&valid);
    Handle parent{valid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)
                        : nullptr};
    if (!parent.value || !sameProcessIdentity(parent.value) ||
        !arguments[3].startsWith(u"snow-shot-admin-"))
        return 21;
    QLocalSocket socket;
    socket.connectToServer(arguments[3]);
    if (!socket.waitForConnected(10000) || !peerMatches(socket, pid, true))
        return 22;
    try {
        const QByteArray operation = receive(socket);
        if (operation == "restart") {
            send(socket, QByteArrayLiteral("ready"));
            if (receive(socket) != "commit") {
                send(socket, QByteArrayLiteral("done"));
                return 0;
            }
            send(socket, QByteArrayLiteral("done"));
            return WaitForSingleObject(parent.value, 45000) == WAIT_OBJECT_0 ? -1 : 23;
        }
        if (operation != "0" && operation != "1" && operation != "2")
            return 24;
        // A previous interrupted operation is reconciled to the parent's persisted selection.
        // The journal contains no executable, command line, or task XML to trust across elevation.
        if (QFile::exists(journalPath()) && !QFile::remove(journalPath()))
            return 25;
        RegistrationTransaction transaction;
        transaction.begin();
        bool cancelled = false;
        const auto result =
            runStartupTransaction({[&](QString*) {
                                       transaction.apply(StartupMode::Off);
                                       return true;
                                   },
                                   [&](QString* error) {
                                       const auto mode =
                                           static_cast<StartupMode>(operation.toInt());
                                       if (mode == StartupMode::ElevatedTask)
                                           transaction.tasks.create();
                                       else if (mode == StartupMode::Registry)
                                           return AutoStartRegistration::setEnabled(true, error);
                                       return true;
                                   },
                                   [&](QString*) {
                                       send(socket, QByteArrayLiteral("ready"));
                                       cancelled = receive(socket) != "commit";
                                       if (!cancelled)
                                           transaction.finish();
                                       return !cancelled;
                                   },
                                   [&](QString* error) { return transaction.rollback(error); }});
        if (!result.success && !(cancelled && result.error.isEmpty())) {
            send(socket, result.error.toUtf8());
            return 26;
        }
        send(socket, QByteArrayLiteral("done"));
        return 0;
    } catch (const std::exception& e) {
        try {
            send(socket, QByteArray(e.what()));
        } catch (...) {
        }
        return 27;
    }
#else
    return 20;
#endif
}
AdministratorResult reconcileStartupMode(StartupMode mode) {
#ifdef Q_OS_WIN
    try {
        Tasks tasks;
        const bool task = static_cast<bool>(tasks.current());
        if (QFile::exists(journalPath()))
            return failure(text(QT_TRANSLATE_NOOP(
                "AdministratorLaunch", "An interrupted startup change needs administrator "
                                       "authorization. Reapply your startup setting.")));
        if (mode == StartupMode::ElevatedTask) {
            QString error;
            if (!AutoStartRegistration::setEnabled(false, &error))
                return failure(error);
            return task ? AdministratorResult{true, false, {}}
                        : failure(text(QT_TRANSLATE_NOOP(
                              "AdministratorLaunch", "Elevated auto-start needs repair. Turn "
                                                     "Launch as administrator off and on again.")));
        }
        if (task)
            return failure(text(QT_TRANSLATE_NOOP("AdministratorLaunch",
                                                  "Elevated auto-start needs repair. Turn Launch "
                                                  "as administrator off and on again.")));
        QString error;
        if (mode == StartupMode::Registry && AutoStartRegistration::matchesExpectedCommand())
            return {true, false, {}};
        return AutoStartRegistration::setEnabled(mode == StartupMode::Registry, &error)
                   ? AdministratorResult{true, false, {}}
                   : failure(error);
    } catch (const std::exception& e) {
        return failure(
            text(QT_TRANSLATE_NOOP("AdministratorLaunch", "The administrator operation failed: %1"))
                .arg(text(e.what())));
    }
#else
    Q_UNUSED(mode);
    return {true, false, {}};
#endif
}

static AdministratorResult updateInstallationStartup(const QString& root,
                                                     const QString& replacementRoot) {
#ifdef Q_OS_WIN
    try {
        Tasks tasks;
        tasks.executable = QDir(root).filePath(QStringLiteral("bin/snow_shot.exe"));
        ComPtr<IRegisteredTaskCollection> collection;
        check(tasks.folder->GetTasks(TASK_ENUM_HIDDEN, &collection));
        LONG count = 0;
        check(collection->get_Count(&count));
        for (LONG i = count; i > 0; --i) {
            ComPtr<IRegisteredTask> task;
            check(collection->get_Item(_variant_t(i), &task));
            BSTR name = nullptr;
            check(task->get_Name(&name));
            const QString candidate = QString::fromWCharArray(name);
            SysFreeString(name);
            if (!candidate.startsWith(u"SnowShot-"))
                continue;
            ComPtr<ITaskDefinition> definition;
            check(task->get_Definition(&definition));
            ComPtr<IPrincipal> principal;
            check(definition->get_Principal(&principal));
            BSTR sid = nullptr;
            check(principal->get_UserId(&sid));
            tasks.sid = QString::fromWCharArray(sid);
            SysFreeString(sid);
            if (candidate != taskName(tasks.executable, tasks.sid))
                continue;
            tasks.name = candidate;
            tasks.validate(task.Get());
            const QString previousXml = tasks.xml();
            const QString previousSecurity = tasks.securityDescriptor();
            const QString previousExecutable = tasks.executable;
            if (!replacementRoot.isEmpty()) {
                tasks.executable =
                    QDir(replacementRoot).filePath(QStringLiteral("bin/snow_shot.exe"));
                tasks.name = taskName(tasks.executable, tasks.sid);
                if (tasks.current())
                    throw std::runtime_error(QT_TRANSLATE_NOOP(
                        "AdministratorLaunch", "The destination already has a startup task"));
                tasks.executable = previousExecutable;
                tasks.name = candidate;
            }
            tasks.remove();
            if (!replacementRoot.isEmpty()) {
                try {
                    tasks.executable =
                        QDir(replacementRoot).filePath(QStringLiteral("bin/snow_shot.exe"));
                    tasks.name = taskName(tasks.executable, tasks.sid);
                    tasks.create();
                } catch (...) {
                    tasks.executable = previousExecutable;
                    tasks.name = candidate;
                    tasks.restoreXml(previousXml, previousSecurity);
                    throw;
                }
                tasks.executable = previousExecutable;
                tasks.name = candidate;
            }
        }
        // Enumerate loaded user hives, never confuse the installer account with the app owner.
        for (DWORD index = 0;; ++index) {
            wchar_t sid[256];
            DWORD size = 256;
            const LSTATUS status =
                RegEnumKeyExW(HKEY_USERS, index, sid, &size, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS)
                check(HRESULT_FROM_WIN32(status));
            const QString key =
                QString::fromWCharArray(sid, size) +
                QStringLiteral("\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
            HKEY run = nullptr;
            const LSTATUS opened = RegOpenKeyExW(HKEY_USERS, key.toStdWString().c_str(), 0,
                                                 KEY_QUERY_VALUE | KEY_SET_VALUE, &run);
            if (opened == ERROR_FILE_NOT_FOUND)
                continue;
            check(HRESULT_FROM_WIN32(opened));
            wchar_t command[32768];
            DWORD bytes = sizeof(command);
            const LSTATUS read =
                RegGetValueW(run, nullptr, L"SnowShot", RRF_RT_REG_SZ, nullptr, command, &bytes);
            LSTATUS removed = ERROR_SUCCESS;
            const QString expected = QStringLiteral("\"%1\" --autostart")
                                         .arg(QDir::toNativeSeparators(tasks.executable));
            if (read == ERROR_SUCCESS &&
                QString::fromWCharArray(command).compare(expected, Qt::CaseInsensitive) == 0)
                if (replacementRoot.isEmpty())
                    removed = RegDeleteValueW(run, L"SnowShot");
                else {
                    const std::wstring value =
                        QStringLiteral("\"%1\" --autostart")
                            .arg(QDir::toNativeSeparators(
                                QDir(replacementRoot)
                                    .filePath(QStringLiteral("bin/snow_shot.exe"))))
                            .toStdWString();
                    removed = RegSetValueExW(
                        run, L"SnowShot", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
                }
            RegCloseKey(run);
            check(HRESULT_FROM_WIN32(removed));
        }
        // Users who are signed out have no HKEY_USERS hive. Open their application hive
        // privately rather than mounting it globally or writing the installer's HKCU.
        HKEY profiles = nullptr;
        check(HRESULT_FROM_WIN32(RegOpenKeyExW(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0,
            KEY_READ, &profiles)));
        struct RegistryHandle {
            HKEY value;
            ~RegistryHandle() {
                if (value)
                    RegCloseKey(value);
            }
        } profilesGuard{profiles};
        for (DWORD index = 0;; ++index) {
            wchar_t sid[256];
            DWORD length = 256;
            const LSTATUS status =
                RegEnumKeyExW(profiles, index, sid, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            check(HRESULT_FROM_WIN32(status));
            const QString owner = QString::fromWCharArray(sid, length);
            if (!owner.startsWith(u"S-1-5-21-") && !owner.startsWith(u"S-1-12-1-"))
                continue;
            HKEY loaded = nullptr;
            if (RegOpenKeyExW(HKEY_USERS, sid, 0, KEY_READ, &loaded) == ERROR_SUCCESS) {
                RegCloseKey(loaded);
                continue;
            }
            wchar_t directory[32768];
            DWORD bytes = sizeof(directory);
            check(HRESULT_FROM_WIN32(RegGetValueW(profiles, sid, L"ProfileImagePath",
                                                  RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                                                  directory, &bytes)));
            const QString hivePath =
                QDir(QString::fromWCharArray(directory)).filePath(QStringLiteral("NTUSER.DAT"));
            if (!QFileInfo::exists(hivePath))
                continue;
            HKEY hive = nullptr;
            check(HRESULT_FROM_WIN32(RegLoadAppKeyW(hivePath.toStdWString().c_str(), &hive,
                                                    KEY_READ | KEY_WRITE, REG_PROCESS_APPKEY, 0)));
            RegistryHandle hiveGuard{hive};
            wchar_t command[32768];
            bytes = sizeof(command);
            const LSTATUS read =
                RegGetValueW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                             L"SnowShot", RRF_RT_REG_SZ, nullptr, command, &bytes);
            if (read == ERROR_FILE_NOT_FOUND)
                continue;
            check(HRESULT_FROM_WIN32(read));
            const QString expected = QStringLiteral("\"%1\" --autostart")
                                         .arg(QDir::toNativeSeparators(tasks.executable));
            if (QString::fromWCharArray(command).compare(expected, Qt::CaseInsensitive) == 0) {
                if (replacementRoot.isEmpty()) {
                    check(HRESULT_FROM_WIN32(RegDeleteKeyValueW(
                        hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"SnowShot")));
                } else {
                    const std::wstring value =
                        QStringLiteral("\"%1\" --autostart")
                            .arg(QDir::toNativeSeparators(
                                QDir(replacementRoot)
                                    .filePath(QStringLiteral("bin/snow_shot.exe"))))
                            .toStdWString();
                    check(HRESULT_FROM_WIN32(
                        RegSetKeyValueW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                        L"SnowShot", REG_SZ, value.c_str(),
                                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))));
                }
                check(HRESULT_FROM_WIN32(RegFlushKey(hive)));
            }
        }
        return {true, false, {}};
    } catch (const std::exception& e) {
        return failure(QString::fromUtf8(e.what()));
    }
#else
    Q_UNUSED(root);
    Q_UNUSED(replacementRoot);
    return {true, false, {}};
#endif
}
AdministratorResult removeInstallationStartup(const QString& root) {
    return updateInstallationStartup(root, {});
}
AdministratorResult migrateInstallationStartup(const QString& previousRoot, const QString& root) {
    if (QDir::cleanPath(previousRoot).compare(QDir::cleanPath(root), Qt::CaseInsensitive) == 0)
        return {true, false, {}};
    return updateInstallationStartup(previousRoot, root);
}
bool launchOnInteractiveDesktop(const QString& executable) {
#ifdef Q_OS_WIN
    ComApartment apartment;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&windows))))
        return false;
    _variant_t empty;
    long handle = 0;
    ComPtr<IDispatch> desktop;
    if (FAILED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH,
                                     &desktop)))
        return false;
    ComPtr<IServiceProvider> provider;
    if (FAILED(desktop.As(&provider)))
        return false;
    ComPtr<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))))
        return false;
    ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(&view)))
        return false;
    ComPtr<IDispatch> background;
    if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))))
        return false;
    ComPtr<IShellFolderViewDual> folder;
    if (FAILED(background.As(&folder)))
        return false;
    ComPtr<IDispatch> application;
    if (FAILED(folder->get_Application(&application)))
        return false;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(application.As(&shell)))
        return false;
    return SUCCEEDED(shell->ShellExecute(
        _bstr_t(executable.toStdWString().c_str()), _variant_t(L"--show-main-window"),
        _variant_t(QFileInfo(executable).absolutePath().toStdWString().c_str()),
        _variant_t(L"open"), _variant_t(SW_SHOWNORMAL)));
#else
    Q_UNUSED(executable);
    return false;
#endif
}
} // namespace snow_shot::platform::windows
