#include "snow_shot/app/singleinstancecoordinator.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <utility>

namespace snow_shot::app {
namespace {
constexpr int kForwardTimeoutMilliseconds = 1500;
constexpr int kMaximumRequestBytes = 1024 * 1024;

QString instanceIdentity() {
    const QByteArray identity =
        (QDir::homePath() + u'|' + QCoreApplication::organizationName() + u'|' +
         QCoreApplication::applicationName())
            .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(24));
}

QString runtimeDirectory() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (path.trimmed().isEmpty()) {
        path = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return QDir::cleanPath(path);
}

QByteArray requestPayload(const QStringList& arguments) {
    QJsonArray argumentArray;
    for (const QString& argument : arguments) {
        argumentArray.push_back(argument);
    }
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("version"), 1},
                             {QStringLiteral("arguments"), argumentArray},
                             {QStringLiteral("activate"), true},
                         })
        .toJson(QJsonDocument::Compact);
}

QByteArray framedPayload(const QByteArray& payload) {
    QByteArray frame;
    frame.resize(static_cast<qsizetype>(sizeof(quint32)) + payload.size());
    qToBigEndian(static_cast<quint32>(payload.size()), frame.data());
    std::copy(payload.cbegin(), payload.cend(), frame.begin() + sizeof(quint32));
    return frame;
}
} // namespace

SingleInstanceCoordinator::SingleInstanceCoordinator(QObject* parent) : QObject(parent) {
    const QString identity = instanceIdentity();
    m_serverName = QStringLiteral("snow-shot-") + identity;
    m_lockFilePath =
        QDir(runtimeDirectory()).filePath(QStringLiteral("snow-shot-") + identity +
                                          QStringLiteral(".lock"));
}

SingleInstanceCoordinator::~SingleInstanceCoordinator() {
    if (m_server != nullptr) {
        m_server->close();
    }
    if (m_primary && m_lock != nullptr) {
        m_lock->unlock();
    }
}

SingleInstanceResult
SingleInstanceCoordinator::acquireOrForward(const QStringList& arguments) {
    if (m_primary) {
        return {SingleInstanceOutcome::Primary, {}};
    }
    if (!QDir().mkpath(QFileInfo(m_lockFilePath).absolutePath())) {
        return {SingleInstanceOutcome::Failed,
                QStringLiteral("The single-instance runtime directory is unavailable")};
    }
    m_lock = std::make_unique<QLockFile>(m_lockFilePath);
    m_lock->setStaleLockTime(0);
    if (m_lock->tryLock(0)) {
        QString error;
        if (!startServer(&error)) {
            m_lock->unlock();
            return {SingleInstanceOutcome::Failed, error};
        }
        m_primary = true;
        return {SingleInstanceOutcome::Primary, {}};
    }

    QString forwardError;
    if (forwardRequest(arguments, kForwardTimeoutMilliseconds, &forwardError)) {
        return {SingleInstanceOutcome::Forwarded, {}};
    }

    if (m_lock->removeStaleLockFile() && m_lock->tryLock(0)) {
        QString error;
        if (!startServer(&error)) {
            m_lock->unlock();
            return {SingleInstanceOutcome::Failed, error};
        }
        m_primary = true;
        return {SingleInstanceOutcome::Primary, {}};
    }

    qint64 processId = 0;
    QString hostName;
    QString applicationName;
    static_cast<void>(m_lock->getLockInfo(&processId, &hostName, &applicationName));
    return {
        SingleInstanceOutcome::Failed,
        QStringLiteral("Another Snow Shot process owns the storage lock but could not be "
                       "contacted (PID %1 on %2, %3), %4")
            .arg(processId)
            .arg(hostName, applicationName, forwardError),
    };
}

bool SingleInstanceCoordinator::isPrimary() const {
    return m_primary;
}

QString SingleInstanceCoordinator::lockFilePath() const {
    return m_lockFilePath;
}

void SingleInstanceCoordinator::setLaunchRequestHandler(
    std::function<void(const QStringList&)> handler) {
    m_handler = std::move(handler);
    if (!m_handler) {
        return;
    }
    const QVector<QStringList> pending = std::move(m_pendingRequests);
    m_pendingRequests.clear();
    for (const QStringList& arguments : pending) {
        m_handler(arguments);
    }
}

bool SingleInstanceCoordinator::startServer(QString* error) {
    QLocalServer::removeServer(m_serverName);
    m_server = std::make_unique<QLocalServer>(this);
    connect(m_server.get(), &QLocalServer::newConnection, this,
            &SingleInstanceCoordinator::acceptConnections);
    if (!m_server->listen(m_serverName)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to start the single-instance IPC server: ") +
                     m_server->errorString();
        }
        m_server.reset();
        return false;
    }
    return true;
}

bool SingleInstanceCoordinator::forwardRequest(const QStringList& arguments,
                                               int timeoutMilliseconds,
                                               QString* error) const {
    const QByteArray payload = requestPayload(arguments);
    if (payload.size() > kMaximumRequestBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("The launch request is too large");
        }
        return false;
    }
    const QByteArray frame = framedPayload(payload);
    QElapsedTimer timer;
    timer.start();
    QString lastError;
    while (timer.elapsed() < timeoutMilliseconds) {
        QLocalSocket socket;
        socket.connectToServer(m_serverName, QIODevice::WriteOnly);
        const int remaining = timeoutMilliseconds - static_cast<int>(timer.elapsed());
        if (socket.waitForConnected(std::min(100, std::max(1, remaining)))) {
            if (socket.write(frame) == frame.size() &&
                socket.waitForBytesWritten(std::min(500, std::max(1, remaining)))) {
                socket.disconnectFromServer();
                return true;
            }
            lastError = socket.errorString();
        } else {
            lastError = socket.errorString();
        }
        QThread::msleep(25);
    }
    if (error != nullptr) {
        *error = lastError.isEmpty() ? QStringLiteral("The IPC request timed out") : lastError;
    }
    return false;
}

void SingleInstanceCoordinator::acceptConnections() {
    while (m_server != nullptr && m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        if (socket == nullptr) {
            continue;
        }
        socket->setProperty("snowShotLaunchBuffer", QByteArray());
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket]() { readSocket(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        readSocket(socket);
    }
}

void SingleInstanceCoordinator::readSocket(QLocalSocket* socket) {
    if (socket == nullptr) {
        return;
    }
    QByteArray buffer = socket->property("snowShotLaunchBuffer").toByteArray();
    buffer += socket->readAll();
    for (;;) {
        if (buffer.size() < static_cast<qsizetype>(sizeof(quint32))) {
            break;
        }
        const quint32 payloadSize = qFromBigEndian<quint32>(buffer.constData());
        if (payloadSize == 0 || payloadSize > kMaximumRequestBytes) {
            socket->disconnectFromServer();
            buffer.clear();
            break;
        }
        const qsizetype frameSize = static_cast<qsizetype>(sizeof(quint32)) + payloadSize;
        if (buffer.size() < frameSize) {
            break;
        }
        const QByteArray payload = buffer.mid(sizeof(quint32), payloadSize);
        buffer.remove(0, frameSize);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        const QJsonObject object = document.object();
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            object.value(QStringLiteral("version")).toInt() != 1 ||
            !object.value(QStringLiteral("arguments")).isArray() ||
            !object.value(QStringLiteral("activate")).toBool()) {
            continue;
        }
        QStringList arguments;
        bool valid = true;
        for (const QJsonValue& value : object.value(QStringLiteral("arguments")).toArray()) {
            if (!value.isString()) {
                valid = false;
                break;
            }
            arguments.push_back(value.toString());
        }
        if (valid) {
            dispatchRequest(std::move(arguments));
        }
    }
    socket->setProperty("snowShotLaunchBuffer", buffer);
}

void SingleInstanceCoordinator::dispatchRequest(QStringList arguments) {
    emit launchRequestReceived(arguments);
    if (m_handler) {
        m_handler(arguments);
    } else {
        m_pendingRequests.push_back(std::move(arguments));
    }
}
} // namespace snow_shot::app
