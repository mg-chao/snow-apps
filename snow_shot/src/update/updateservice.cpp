#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QTimer>

namespace snow_shot::update {
namespace {
constexpr int kProtocolVersion = 2;
constexpr qsizetype kMaximumFrameBytes = 64 * 1024;
constexpr qsizetype kMaximumDiagnosticBytes = 8 * 1024;

UpdateState stateFromName(const QString& name, bool* valid) {
    static const QHash<QString, UpdateState> states{
        {QStringLiteral("Unavailable"), UpdateState::Unavailable},
        {QStringLiteral("Idle"), UpdateState::Idle},
        {QStringLiteral("Checking"), UpdateState::Checking},
        {QStringLiteral("Available"), UpdateState::Available},
        {QStringLiteral("Downloading"), UpdateState::Downloading},
        {QStringLiteral("Verifying"), UpdateState::Verifying},
        {QStringLiteral("Ready"), UpdateState::Ready},
        {QStringLiteral("Applying"), UpdateState::Applying},
        {QStringLiteral("Failed"), UpdateState::Failed},
    };
    const auto found = states.constFind(name);
    *valid = found != states.cend();
    return *valid ? *found : UpdateState::Unavailable;
}

QString translatedError(const QJsonObject& object) {
    const QByteArray source = object.value(QStringLiteral("message")).toString().toUtf8();
    return source.isEmpty() ? QString()
                            : QCoreApplication::translate("UpdateErrors", source.constData());
}

enum class Operation { None, Probe, Check, Download, Apply };
enum class Trigger { Startup, Periodic, User, PolicyChange };
enum class Lifecycle { Stopped, Starting, Running, ExpectedExit };

QString operationName(Operation operation) {
    switch (operation) {
    case Operation::Probe:
        return QStringLiteral("probe");
    case Operation::Check:
        return QStringLiteral("check");
    case Operation::Download:
        return QStringLiteral("download");
    case Operation::Apply:
        return QStringLiteral("apply");
    case Operation::None:
        return {};
    }
    return {};
}

QString triggerName(Trigger trigger) {
    switch (trigger) {
    case Trigger::Startup:
        return QStringLiteral("startup");
    case Trigger::Periodic:
        return QStringLiteral("periodic");
    case Trigger::User:
        return QStringLiteral("user");
    case Trigger::PolicyChange:
        return QStringLiteral("policyChange");
    }
    return {};
}

int operationPriority(Operation operation) {
    switch (operation) {
    case Operation::Apply:
        return 4;
    case Operation::Download:
        return 3;
    case Operation::Check:
        return 2;
    case Operation::Probe:
        return 1;
    case Operation::None:
        return 0;
    }
    return 0;
}
} // namespace

struct UpdateService::Impl {
    Impl(UpdateService& owner, Options value)
        : q(owner), options(std::move(value)), process(&owner), handshakeTimeout(&owner),
          scheduleTimer(&owner) {
        process.setProcessChannelMode(QProcess::SeparateChannels);
        handshakeTimeout.setSingleShot(true);
        handshakeTimeout.setInterval(10000);
        scheduleTimer.setSingleShot(true);
        scheduleTimer.setTimerType(Qt::CoarseTimer);
        QObject::connect(&scheduleTimer, &QTimer::timeout, &q, [this] {
            automaticCheckDue = true;
            request(Operation::Check, Trigger::Periodic);
        });
        QObject::connect(&handshakeTimeout, &QTimer::timeout, &q, [this] {
            if (!handshakeComplete) {
                stopProcess();
                unavailable(QCoreApplication::translate(
                    "UpdateErrors", "Could not contact the update coordinator"));
            }
        });
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &q,
                         [this] { readProtocol(); });
        QObject::connect(&process, &QProcess::readyReadStandardError, &q,
                         [this] { readDiagnostics(); });
        QObject::connect(
            &process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &q,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                readDiagnostics();
                readProtocol();
                if (!stdoutBuffer.isEmpty() && !preserveStatusOnExit) {
                    protocolFailure();
                }
                const Operation finishedOperation = activeOperation;
                const Trigger finishedTrigger = activeTrigger;
                const bool intentional = stopping || handedOff || preserveStatusOnExit ||
                                         lifecycle == Lifecycle::ExpectedExit;
                handshakeTimeout.stop();
                handshakeComplete = false;
                stdoutBuffer.clear();
                activeOperation = Operation::None;
                lifecycle = Lifecycle::Stopped;
                if (intentional) {
                    launchPending();
                    return;
                }
                qWarning().noquote()
                    << "Snow Shot updater service exited unexpectedly" << exitCode << exitStatus;
                if (status.state == UpdateState::Checking ||
                    status.state == UpdateState::Downloading ||
                    status.state == UpdateState::Verifying ||
                    status.state == UpdateState::Applying) {
                    fail(QCoreApplication::translate("UpdateErrors",
                                                     "Application coordinator disconnected"));
                } else {
                    unavailable(QCoreApplication::translate(
                        "UpdateErrors", "Could not contact the update coordinator"));
                }
                if (finishedOperation == Operation::Probe) {
                    scheduleTimer.stop();
                    automaticCheckDue = false;
                    if (pendingTrigger != Trigger::User) {
                        pendingOperation = Operation::None;
                    }
                }
                if (finishedOperation == Operation::Check &&
                    (finishedTrigger == Trigger::Startup || finishedTrigger == Trigger::Periodic)) {
                    armAutomaticInterval();
                }
                queueDueAutomaticCheck();
                launchPending();
            });
        QObject::connect(&process, &QProcess::errorOccurred, &q, [this](QProcess::ProcessError) {
            if (!stopping && !preserveStatusOnExit && process.state() == QProcess::NotRunning &&
                !handshakeComplete) {
                const Operation failedOperation = activeOperation;
                const Trigger failedTrigger = activeTrigger;
                activeOperation = Operation::None;
                lifecycle = Lifecycle::Stopped;
                unavailable(QCoreApplication::translate(
                    "UpdateErrors", "Could not launch the application update helper"));
                if (failedOperation == Operation::Probe) {
                    scheduleTimer.stop();
                    automaticCheckDue = false;
                    if (pendingTrigger != Trigger::User) {
                        pendingOperation = Operation::None;
                    }
                }
                if (failedOperation == Operation::Check &&
                    (failedTrigger == Trigger::Startup || failedTrigger == Trigger::Periodic)) {
                    armAutomaticInterval();
                }
                queueDueAutomaticCheck();
                launchPending();
            }
        });
    }

    ~Impl() {
        stopping = true;
        if (process.state() == QProcess::NotRunning) {
            return;
        }
        if (handshakeComplete) {
            send(QStringLiteral("shutdown"));
            if (process.waitForFinished(250)) {
                return;
            }
        }
        process.terminate();
        if (!process.waitForFinished(250)) {
            process.kill();
            process.waitForFinished(250);
        }
    }

    QString executablePath() const {
#ifdef Q_OS_MACOS
        return QDir(options.applicationDirectory).filePath(QStringLiteral("snow-shot-updater"));
#else
        return QDir(options.applicationDirectory).filePath(QStringLiteral("snow-shot-updater.exe"));
#endif
    }

    bool automaticEnabled() const {
        return mode != u"manual";
    }

    void armStartupCheck() {
        if (started && automaticEnabled()) {
            automaticCheckDue = false;
            scheduleTimer.start(options.startupCheckDelay);
        }
    }

    void armAutomaticInterval() {
        if (started && automaticEnabled()) {
            automaticCheckDue = false;
            scheduleTimer.start(options.automaticCheckInterval);
        }
    }

    void queueDueAutomaticCheck() {
        if (automaticCheckDue && automaticEnabled() && pendingOperation == Operation::None) {
            pendingOperation = Operation::Check;
            pendingTrigger = Trigger::Periodic;
        }
    }

    void request(Operation operation, Trigger trigger) {
        if (operation == Operation::None) {
            return;
        }
        if (process.state() != QProcess::NotRunning || lifecycle != Lifecycle::Stopped) {
            if (operationPriority(operation) > operationPriority(pendingOperation) ||
                (operation == pendingOperation && trigger == Trigger::User)) {
                pendingOperation = operation;
                pendingTrigger = trigger;
            }
            return;
        }
        activeOperation = operation;
        activeTrigger = trigger;
        if (operation == Operation::Check && trigger == Trigger::Periodic) {
            automaticCheckDue = false;
        }
        lifecycle = Lifecycle::Starting;
        spawn();
    }

    void launchPending() {
        if (stopping || process.state() != QProcess::NotRunning ||
            pendingOperation == Operation::None) {
            return;
        }
        const Operation operation = pendingOperation;
        const Trigger trigger = pendingTrigger;
        pendingOperation = Operation::None;
        QTimer::singleShot(0, &q, [this, operation, trigger] { request(operation, trigger); });
    }

    void completeOperation(const QString& outcome) {
        if (outcome == u"success" && mode == u"check" &&
            (activeOperation == Operation::Probe ||
             (activeOperation == Operation::Check && activeTrigger != Trigger::User)) &&
            status.state == UpdateState::Available && !status.version.isEmpty() &&
            announcedVersion != status.version) {
            announcedVersion = status.version;
            emit q.automaticUpdateAvailable(status.version);
        }
        if (activeOperation == Operation::Probe && status.state == UpdateState::Unavailable) {
            scheduleTimer.stop();
            automaticCheckDue = false;
            if (pendingTrigger != Trigger::User) {
                pendingOperation = Operation::None;
            }
        }
        if (status.state == UpdateState::Ready && !status.version.isEmpty() &&
            notifiedVersion != status.version) {
            notifiedVersion = status.version;
            emit q.updateReady();
        }
        if (activeOperation == Operation::Check &&
            (activeTrigger == Trigger::Startup || activeTrigger == Trigger::Periodic)) {
            armAutomaticInterval();
        } else if (activeOperation == Operation::Check && activeTrigger == Trigger::User &&
                   outcome == u"success") {
            if (pendingOperation == Operation::Check &&
                (pendingTrigger == Trigger::Startup || pendingTrigger == Trigger::Periodic)) {
                pendingOperation = Operation::None;
            }
            armAutomaticInterval();
        } else if (activeOperation == Operation::Download &&
                   activeTrigger == Trigger::PolicyChange) {
            armAutomaticInterval();
        }
        if (activeOperation == Operation::Check && mode == u"download" &&
            status.state == UpdateState::Available) {
            pendingOperation = Operation::Download;
            pendingTrigger = Trigger::PolicyChange;
        }
        queueDueAutomaticCheck();
        lifecycle = Lifecycle::ExpectedExit;
    }

    void spawn() {
        if (process.state() != QProcess::NotRunning) {
            return;
        }
        stopping = false;
        handedOff = false;
        preserveStatusOnExit = false;
        lifecycle = Lifecycle::Starting;
        handshakeComplete = false;
        helloSeen = false;
        cancelWhenRunning = false;
        stdoutBuffer.clear();
        stderrBuffer.clear();
        QStringList arguments{
            QStringLiteral("--service"),
            QStringLiteral("--target"),
            options.root,
            QStringLiteral("--cache"),
            options.cacheDirectory,
            QStringLiteral("--base-url"),
            options.baseUrl.toString(QUrl::FullyEncoded),
            QStringLiteral("--parent"),
            QString::number(QCoreApplication::applicationPid()),
        };
        if (options.allowLocalHttp) {
            arguments.append(QStringLiteral("--allow-local-http"));
        }
        process.setProgram(executablePath());
        process.setArguments(arguments);
        process.setWorkingDirectory(options.root);
        process.start(QIODevice::ReadWrite);
        handshakeTimeout.start();
    }

    void stopProcess() {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(500);
        }
    }

    void send(const QString& command, QJsonObject payload = {}) {
        if (!handshakeComplete || process.state() != QProcess::Running) {
            return;
        }
        payload.insert(QStringLiteral("protocol"), kProtocolVersion);
        payload.insert(QStringLiteral("id"), static_cast<qint64>(nextRequestId++));
        payload.insert(QStringLiteral("command"), command);
        QByteArray frame = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        if (frame.size() > kMaximumFrameBytes) {
            protocolFailure();
            return;
        }
        frame.append('\n');
        if (process.write(frame) != frame.size()) {
            fail(QCoreApplication::translate("UpdateErrors", "Could not send updater status"));
        }
    }

    void readProtocol() {
        stdoutBuffer.append(process.readAllStandardOutput());
        if (stdoutBuffer.size() > kMaximumFrameBytes && !stdoutBuffer.contains('\n')) {
            protocolFailure(QCoreApplication::translate(
                "UpdateErrors", "The update service sent an oversized protocol message"));
            return;
        }
        qsizetype newline = -1;
        while ((newline = stdoutBuffer.indexOf('\n')) >= 0) {
            if (newline > kMaximumFrameBytes) {
                protocolFailure(QCoreApplication::translate(
                    "UpdateErrors", "The update service sent an oversized protocol message"));
                return;
            }
            QByteArray frame = stdoutBuffer.left(newline);
            stdoutBuffer.remove(0, newline + 1);
            if (frame.endsWith('\r')) {
                frame.chop(1);
            }
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(frame, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                protocolFailure();
                return;
            }
            handleEvent(document.object());
        }
        if (stdoutBuffer.size() > kMaximumFrameBytes) {
            protocolFailure(QCoreApplication::translate(
                "UpdateErrors", "The update service sent an oversized protocol message"));
        }
    }

    void readDiagnostics() {
        stderrBuffer.append(process.readAllStandardError());
        if (stderrBuffer.size() > kMaximumDiagnosticBytes && !stderrBuffer.contains('\n')) {
            qWarning().noquote() << "snow-shot-updater:"
                                 << QString::fromUtf8(stderrBuffer.left(kMaximumDiagnosticBytes));
            stderrBuffer.clear();
        }
        qsizetype newline = -1;
        while ((newline = stderrBuffer.indexOf('\n')) >= 0) {
            QByteArray line = stderrBuffer.left(newline);
            stderrBuffer.remove(0, newline + 1);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (!line.isEmpty()) {
                qWarning().noquote() << "snow-shot-updater:"
                                     << QString::fromUtf8(line.left(kMaximumDiagnosticBytes));
            }
        }
    }

    void handleEvent(const QJsonObject& event) {
        if (event.value(QStringLiteral("protocol")).toInt(-1) != kProtocolVersion) {
            protocolFailure(QCoreApplication::translate(
                "UpdateErrors", "The update service protocol version is unsupported"));
            return;
        }
        const QString type = event.value(QStringLiteral("type")).toString();
        if (!helloSeen) {
            if (type != u"hello" ||
                event.value(QStringLiteral("updaterVersion")).toString().isEmpty()) {
                protocolFailure();
                return;
            }
            helloSeen = true;
            handshakeComplete = true;
            lifecycle = Lifecycle::Running;
            handshakeTimeout.stop();
            send(QStringLiteral("execute"),
                 {{QStringLiteral("operation"), operationName(activeOperation)},
                  {QStringLiteral("trigger"), triggerName(activeTrigger)},
                  {QStringLiteral("mode"), mode},
                  {QStringLiteral("systemProxy"), systemProxy}});
            if (cancelWhenRunning) {
                cancelWhenRunning = false;
                send(QStringLiteral("cancel"));
            }
            return;
        }
        if (type == u"status") {
            applyStatus(event.value(QStringLiteral("status")).toObject());
        } else if (type == u"update_ready") {
            if (!status.version.isEmpty() && notifiedVersion != status.version) {
                notifiedVersion = status.version;
                emit q.updateReady();
            }
        } else if (type == u"operation_complete") {
            const QString outcome = event.value(QStringLiteral("outcome")).toString();
            if (lifecycle != Lifecycle::Running ||
                event.value(QStringLiteral("operation")).toString() !=
                    operationName(activeOperation) ||
                (outcome != u"success" && outcome != u"failed" && outcome != u"cancelled")) {
                protocolFailure();
                return;
            }
            applyStatus(event.value(QStringLiteral("status")).toObject());
            completeOperation(outcome);
        } else if (type == u"handoff_ready") {
            emit q.handoffReady();
            const bool proceed = status.state == UpdateState::Applying;
            if (proceed) {
                handedOff = true;
                send(QStringLiteral("handoff_decision"), {{QStringLiteral("proceed"), true}});
            }
        } else if (type == u"fatal") {
            const auto error = event.value(QStringLiteral("error")).toObject();
            fail(translatedError(error));
            if (activeOperation == Operation::Probe) {
                scheduleTimer.stop();
                automaticCheckDue = false;
                if (pendingTrigger != Trigger::User) {
                    pendingOperation = Operation::None;
                }
            }
            if (activeOperation == Operation::Check &&
                (activeTrigger == Trigger::Startup || activeTrigger == Trigger::Periodic)) {
                armAutomaticInterval();
            }
            queueDueAutomaticCheck();
            preserveStatusOnExit = true;
            stopProcess();
        } else if (type != u"command_result") {
            protocolFailure();
        } else if (!event.value(QStringLiteral("ok")).toBool(true)) {
            const auto error = event.value(QStringLiteral("error")).toObject();
            const QString detail = error.value(QStringLiteral("detail")).toString();
            if (!detail.isEmpty()) {
                qWarning().noquote() << "snow-shot-updater command failed:"
                                     << error.value(QStringLiteral("code")).toString()
                                     << detail.left(kMaximumDiagnosticBytes);
            }
        }
    }

    void applyStatus(const QJsonObject& object) {
        bool valid = false;
        const UpdateState state =
            stateFromName(object.value(QStringLiteral("state")).toString(), &valid);
        if (!valid) {
            protocolFailure();
            return;
        }
        status.state = state;
        status.version = object.value(QStringLiteral("version")).toString();
        status.received = object.value(QStringLiteral("received")).toVariant().toLongLong();
        status.total = object.value(QStringLiteral("total")).toVariant().toLongLong();
        const auto error = object.value(QStringLiteral("error")).toObject();
        errorSource = error.value(QStringLiteral("message")).toString().toUtf8();
        status.error = translatedError(error);
        const QString detail = error.value(QStringLiteral("detail")).toString();
        if (!detail.isEmpty()) {
            qWarning().noquote() << "snow-shot-updater:"
                                 << error.value(QStringLiteral("code")).toString()
                                 << detail.left(kMaximumDiagnosticBytes);
        }
        emit q.statusChanged();
    }

    void protocolFailure(const QString& error = QCoreApplication::translate(
                             "UpdateErrors", "The update service protocol message is invalid")) {
        if (handshakeComplete) {
            fail(error);
        } else {
            unavailable(error);
        }
        preserveStatusOnExit = true;
        if (activeOperation == Operation::Probe) {
            scheduleTimer.stop();
            automaticCheckDue = false;
            if (pendingTrigger != Trigger::User) {
                pendingOperation = Operation::None;
            }
        }
        if (activeOperation == Operation::Check &&
            (activeTrigger == Trigger::Startup || activeTrigger == Trigger::Periodic)) {
            armAutomaticInterval();
        }
        queueDueAutomaticCheck();
        stopProcess();
    }

    void fail(const QString& error) {
        errorSource.clear();
        status.state = UpdateState::Failed;
        status.error = error;
        emit q.statusChanged();
    }

    void unavailable(const QString& error) {
        errorSource.clear();
        status.state = UpdateState::Unavailable;
        status.error = error;
        emit q.statusChanged();
    }

    UpdateService& q;
    Options options;
    QProcess process;
    QTimer handshakeTimeout;
    QTimer scheduleTimer;
    UpdateStatus status;
    QByteArray stdoutBuffer;
    QByteArray stderrBuffer;
    QByteArray errorSource;
    QString mode = QStringLiteral("download");
    QString notifiedVersion;
    QString announcedVersion;
    quint64 nextRequestId = 1;
    Operation activeOperation = Operation::None;
    Trigger activeTrigger = Trigger::Startup;
    Operation pendingOperation = Operation::None;
    Trigger pendingTrigger = Trigger::Periodic;
    bool systemProxy = false;
    bool started = false;
    bool handshakeComplete = false;
    bool helloSeen = false;
    bool stopping = false;
    bool handedOff = false;
    bool preserveStatusOnExit = false;
    bool cancelWhenRunning = false;
    bool automaticCheckDue = false;
    Lifecycle lifecycle = Lifecycle::Stopped;
};

UpdateService::UpdateService(Options options, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(options))) {
    setObjectName(QStringLiteral("snowShotUpdateService"));
}

UpdateService::~UpdateService() = default;

bool UpdateService::event(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && !m_impl->errorSource.isEmpty()) {
        m_impl->status.error =
            QCoreApplication::translate("UpdateErrors", m_impl->errorSource.constData());
        emit statusChanged();
    }
    return QObject::event(event);
}

const UpdateStatus& UpdateService::status() const {
    return m_impl->status;
}

void UpdateService::start() {
    if (m_impl->started) {
        return;
    }
    m_impl->started = true;
    m_impl->request(Operation::Probe, Trigger::Startup);
    m_impl->armStartupCheck();
}

void UpdateService::setMode(const QString& mode) {
    if (mode != u"manual" && mode != u"check" && mode != u"download") {
        return;
    }
    const QString previous = m_impl->mode;
    m_impl->mode = mode;
    if (!m_impl->started) {
        return;
    }
    if (mode == u"manual") {
        m_impl->scheduleTimer.stop();
        m_impl->automaticCheckDue = false;
        if (m_impl->pendingTrigger != Trigger::User) {
            m_impl->pendingOperation = Operation::None;
        }
        if (m_impl->activeOperation == Operation::Download &&
            m_impl->activeTrigger != Trigger::User) {
            if (m_impl->handshakeComplete) {
                m_impl->send(QStringLiteral("cancel"));
            } else {
                m_impl->cancelWhenRunning = true;
            }
        }
        return;
    }
    if (mode != u"download" && m_impl->activeOperation == Operation::Download &&
        m_impl->activeTrigger != Trigger::User) {
        if (m_impl->handshakeComplete) {
            m_impl->send(QStringLiteral("cancel"));
        } else {
            m_impl->cancelWhenRunning = true;
        }
    } else if (mode == u"download") {
        m_impl->cancelWhenRunning = false;
    }
    if (mode != u"download" && m_impl->pendingOperation == Operation::Download &&
        m_impl->pendingTrigger != Trigger::User) {
        m_impl->pendingOperation = Operation::None;
    }
    if (mode == u"download" && m_impl->status.state == UpdateState::Available) {
        m_impl->scheduleTimer.stop();
        m_impl->request(Operation::Download, Trigger::PolicyChange);
    } else if (previous == u"manual") {
        m_impl->armStartupCheck();
    }
}

void UpdateService::setSystemProxy(bool enabled) {
    m_impl->systemProxy = enabled;
}

void UpdateService::check(bool manual) {
    m_impl->request(Operation::Check, manual ? Trigger::User : Trigger::Periodic);
}

void UpdateService::download() {
    m_impl->request(Operation::Download, Trigger::User);
}

void UpdateService::cancel() {
    if (m_impl->activeOperation == Operation::None) {
        return;
    }
    if (m_impl->handshakeComplete) {
        m_impl->send(QStringLiteral("cancel"));
    } else {
        m_impl->cancelWhenRunning = true;
    }
}

void UpdateService::requestRestart() {
    emit restartRequested();
}

void UpdateService::beginApply() {
    m_impl->request(Operation::Apply, Trigger::User);
}

void UpdateService::reportBlocked(const QString& reason) {
    m_impl->errorSource.clear();
    if (m_impl->status.state == UpdateState::Applying) {
        m_impl->status.state = UpdateState::Ready;
        m_impl->status.error = reason;
        emit statusChanged();
        m_impl->send(QStringLiteral("handoff_decision"),
                     {{QStringLiteral("proceed"), false}, {QStringLiteral("reason"), reason}});
    } else {
        m_impl->status.error = reason;
        emit statusChanged();
    }
}
} // namespace snow_shot::update
