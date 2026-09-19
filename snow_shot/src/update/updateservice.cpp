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
constexpr int kProtocolVersion = 1;
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
} // namespace

struct UpdateService::Impl {
    Impl(UpdateService& owner, Options value)
        : q(owner), options(std::move(value)), process(&owner), handshakeTimeout(&owner) {
        process.setProcessChannelMode(QProcess::SeparateChannels);
        handshakeTimeout.setSingleShot(true);
        handshakeTimeout.setInterval(10000);
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
        QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &q,
                         [this](int exitCode, QProcess::ExitStatus exitStatus) {
                             readDiagnostics();
                             readProtocol();
                             if (!stdoutBuffer.isEmpty() && !preserveStatusOnExit) {
                                 protocolFailure();
                             }
                             const bool intentional = stopping || handedOff || preserveStatusOnExit;
                             handshakeTimeout.stop();
                             handshakeComplete = false;
                             stdoutBuffer.clear();
                             if (intentional) {
                                 return;
                             }
                             qWarning().noquote() << "Snow Shot updater service exited unexpectedly"
                                                  << exitCode << exitStatus;
                             manualRespawnAvailable = true;
                             if (status.state == UpdateState::Checking ||
                                 status.state == UpdateState::Downloading ||
                                 status.state == UpdateState::Verifying ||
                                 status.state == UpdateState::Applying) {
                                 fail(QCoreApplication::translate(
                                     "UpdateErrors", "Application coordinator disconnected"));
                             } else {
                                 unavailable(QCoreApplication::translate(
                                     "UpdateErrors", "Could not contact the update coordinator"));
                             }
                         });
        QObject::connect(&process, &QProcess::errorOccurred, &q, [this](QProcess::ProcessError) {
            if (!stopping && !preserveStatusOnExit && process.state() == QProcess::NotRunning &&
                !handshakeComplete) {
                manualRespawnAvailable = true;
                unavailable(QCoreApplication::translate(
                    "UpdateErrors", "Could not launch the application update helper"));
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
            if (process.waitForFinished(750)) {
                return;
            }
        }
        process.terminate();
        if (!process.waitForFinished(500)) {
            process.kill();
            process.waitForFinished(500);
        }
    }

    QString executablePath() const {
#ifdef Q_OS_MACOS
        return QDir(options.applicationDirectory).filePath(QStringLiteral("snow-shot-updater"));
#else
        return QDir(options.applicationDirectory).filePath(QStringLiteral("snow-shot-updater.exe"));
#endif
    }

    void spawn() {
        if (process.state() != QProcess::NotRunning) {
            return;
        }
        stopping = false;
        handedOff = false;
        preserveStatusOnExit = false;
        handshakeComplete = false;
        helloSeen = false;
        stdoutBuffer.clear();
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
            handshakeTimeout.stop();
            send(QStringLiteral("set_mode"), {{QStringLiteral("mode"), mode}});
            send(QStringLiteral("set_system_proxy"), {{QStringLiteral("enabled"), systemProxy}});
            if (startRequested) {
                send(QStringLiteral("start"));
            }
            if (pendingManualCheck) {
                pendingManualCheck = false;
                send(QStringLiteral("check"), {{QStringLiteral("manual"), true}});
            }
            return;
        }
        if (type == u"status") {
            applyStatus(event.value(QStringLiteral("status")).toObject());
        } else if (type == u"update_ready") {
            emit q.updateReady();
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
            preserveStatusOnExit = true;
            manualRespawnAvailable = true;
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
        manualRespawnAvailable = true;
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
    UpdateStatus status;
    QByteArray stdoutBuffer;
    QByteArray stderrBuffer;
    QByteArray errorSource;
    QString mode = QStringLiteral("download");
    quint64 nextRequestId = 1;
    bool systemProxy = false;
    bool startRequested = false;
    bool pendingManualCheck = false;
    bool handshakeComplete = false;
    bool helloSeen = false;
    bool manualRespawnAvailable = false;
    bool stopping = false;
    bool handedOff = false;
    bool preserveStatusOnExit = false;
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
    m_impl->startRequested = true;
    m_impl->spawn();
    if (m_impl->handshakeComplete) {
        m_impl->send(QStringLiteral("start"));
    }
}

void UpdateService::setMode(const QString& mode) {
    if (mode != u"manual" && mode != u"check" && mode != u"download") {
        return;
    }
    m_impl->mode = mode;
    m_impl->send(QStringLiteral("set_mode"), {{QStringLiteral("mode"), mode}});
}

void UpdateService::setSystemProxy(bool enabled) {
    m_impl->systemProxy = enabled;
    m_impl->send(QStringLiteral("set_system_proxy"), {{QStringLiteral("enabled"), enabled}});
}

void UpdateService::check(bool manual) {
    if (m_impl->process.state() == QProcess::NotRunning && manual &&
        m_impl->manualRespawnAvailable) {
        m_impl->manualRespawnAvailable = false;
        m_impl->pendingManualCheck = true;
        m_impl->spawn();
        return;
    }
    m_impl->send(QStringLiteral("check"), {{QStringLiteral("manual"), manual}});
}

void UpdateService::download() {
    m_impl->send(QStringLiteral("download"));
}

void UpdateService::cancel() {
    m_impl->send(QStringLiteral("cancel"));
}

void UpdateService::requestRestart() {
    emit restartRequested();
}

void UpdateService::beginApply() {
    m_impl->send(QStringLiteral("begin_apply"));
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
