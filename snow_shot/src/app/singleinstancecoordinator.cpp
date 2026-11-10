#include "snow_shot/app/singleinstancecoordinator.h"
#include "snow_shot/app/distribution.h"

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
#include <QUuid>

#include <algorithm>
#include <utility>

namespace snow_shot::app {
namespace {
constexpr int kMaximumRequestBytes = 1024 * 1024;
constexpr qsizetype kMaximumRememberedRequests = 4096;
constexpr int kLegacyProtocolVersion = 1;
constexpr int kReceiptProtocolVersion = 2;

QString instanceIdentity() {
    QString identityText = QDir::homePath() + u'|' + QCoreApplication::organizationName() + u'|' +
                           QCoreApplication::applicationName();
    if (distribution::isStoreManaged())
        identityText += u'|' + distribution::installationIdentity();
    const QByteArray identity = identityText.toUtf8();
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

QByteArray requestPayload(const QStringList& arguments, const QString& requestId,
                          int protocolVersion) {
    QJsonArray argumentArray;
    for (const QString& argument : arguments) {
        argumentArray.push_back(argument);
    }
    QJsonObject request{
        {QStringLiteral("version"), protocolVersion},
        {QStringLiteral("arguments"), argumentArray},
        {QStringLiteral("activate"), true},
    };
    if (protocolVersion == kReceiptProtocolVersion)
        request.insert(QStringLiteral("requestId"), requestId);
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

QByteArray framedPayload(const QByteArray& payload) {
    QByteArray frame;
    frame.resize(static_cast<qsizetype>(sizeof(quint32)) + payload.size());
    qToBigEndian(static_cast<quint32>(payload.size()), frame.data());
    std::copy(payload.cbegin(), payload.cend(), frame.begin() + sizeof(quint32));
    return frame;
}

bool writeFrame(QLocalSocket& socket, const QByteArray& frame,
                const std::function<int()>& remaining) {
    if (socket.write(frame) != frame.size())
        return false;
    while (socket.bytesToWrite() > 0) {
        const int timeout = remaining();
        if (timeout <= 0 || !socket.waitForBytesWritten(timeout))
            return false;
    }
    return true;
}
} // namespace

SingleInstanceCoordinator::SingleInstanceCoordinator(QObject* parent) : QObject(parent) {
    const QString identity = instanceIdentity();
    m_legacyServerName = QStringLiteral("snow-shot-") + identity;
    m_receiptServerName = m_legacyServerName + QStringLiteral("-v2");
    m_lockFilePath =
        QDir(runtimeDirectory())
            .filePath(QStringLiteral("snow-shot-") + identity + QStringLiteral(".lock"));
}

SingleInstanceCoordinator::~SingleInstanceCoordinator() {
    stopAcceptingLaunchRequests();
    if (m_primary && m_lock != nullptr) {
        m_lock->unlock();
    }
}

SingleInstanceResult
SingleInstanceCoordinator::acquireOrForward(const QStringList& arguments,
                                            int forwardingTimeoutMilliseconds) {
    if (m_primary) {
        return {SingleInstanceOutcome::Primary, {}};
    }
    if (!QDir().mkpath(QFileInfo(m_lockFilePath).absolutePath())) {
        return {SingleInstanceOutcome::Failed,
                QStringLiteral("The single-instance runtime directory is unavailable")};
    }
    m_lock = std::make_unique<QLockFile>(m_lockFilePath);
    m_lock->setStaleLockTime(0);
    const QString requestId = QUuid::createUuid().toString(QUuid::Id128);
    const int forwardTimeoutMilliseconds = std::max(1, forwardingTimeoutMilliseconds);
    QElapsedTimer timer;
    timer.start();
    QString forwardError;
    bool receiptRequired = false;
    do {
        if (m_lock->tryLock(0) || (m_lock->removeStaleLockFile() && m_lock->tryLock(0))) {
            QString error;
            if (!startServer(&error)) {
                m_lock->unlock();
                return {SingleInstanceOutcome::Failed, error};
            }
            m_primary = true;
            return {SingleInstanceOutcome::Primary, {}};
        }
        const int remaining = forwardTimeoutMilliseconds - static_cast<int>(timer.elapsed());
        if (remaining > 0) {
            const auto forwarded = forwardRequest(arguments, requestId, std::min(500, remaining),
                                                  receiptRequired, &forwardError);
            if (forwarded == ForwardResult::Forwarded)
                return {SingleInstanceOutcome::Forwarded, {}};
            if (forwarded == ForwardResult::Failed)
                break;
        }
        QThread::msleep(25);
    } while (timer.elapsed() < forwardTimeoutMilliseconds);

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

void SingleInstanceCoordinator::stopAcceptingLaunchRequests() {
    m_acceptingRequests = false;
    // Close the legacy endpoint first: a stopping modern primary must never look legacy-only.
    for (auto* server : {m_legacyServer.get(), m_receiptServer.get()}) {
        if (!server)
            continue;
        server->close();
        while (server->hasPendingConnections()) {
            if (auto* socket = server->nextPendingConnection()) {
                socket->abort();
                socket->deleteLater();
            }
        }
    }
    const auto clients = std::exchange(m_clients, {});
    for (auto* socket : clients) {
        QObject::disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        socket->deleteLater();
    }
}

bool SingleInstanceCoordinator::isPrimary() const {
    return m_primary;
}

QString SingleInstanceCoordinator::lockFilePath() const {
    return m_lockFilePath;
}

void SingleInstanceCoordinator::setLaunchRequestAdmission(
    std::function<bool(const QStringList&)> admission) {
    m_admission = std::move(admission);
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
    const auto listen = [this, error](const QString& name, int protocolVersion,
                                      std::unique_ptr<QLocalServer>& server) {
        QLocalServer::removeServer(name);
        server = std::make_unique<QLocalServer>(this);
        // A normal launch must be able to activate an elevated instance of the same
        // user. Windows' default pipe ACL can grant access only to administrators.
        server->setSocketOptions(QLocalServer::UserAccessOption);
        auto* endpoint = server.get();
        connect(endpoint, &QLocalServer::newConnection, this, [this, endpoint, protocolVersion] {
            acceptConnections(endpoint, protocolVersion);
        });
        if (!endpoint->listen(name)) {
            if (error)
                *error = QStringLiteral("Unable to start the single-instance IPC server: ") +
                         endpoint->errorString();
            return false;
        }
        return true;
    };
    // The endpoint itself advertises receipt support without dispatching a capability probe.
    // Publish it before the legacy endpoint so readiness cannot cause a protocol downgrade.
    if (!listen(m_receiptServerName, kReceiptProtocolVersion, m_receiptServer) ||
        !listen(m_legacyServerName, kLegacyProtocolVersion, m_legacyServer)) {
        stopAcceptingLaunchRequests();
        m_legacyServer.reset();
        m_receiptServer.reset();
        return false;
    }
    m_acceptingRequests = true;
    return true;
}

SingleInstanceCoordinator::ForwardResult
SingleInstanceCoordinator::forwardRequest(const QStringList& arguments, const QString& requestId,
                                          int timeoutMilliseconds, bool& receiptRequired,
                                          QString* error) const {
    QByteArray payload = requestPayload(arguments, requestId, kReceiptProtocolVersion);
    if (payload.size() > kMaximumRequestBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("The launch request is too large");
        }
        return ForwardResult::Failed;
    }
    QElapsedTimer timer;
    timer.start();
    const auto remaining = [&] {
        return std::max(0, timeoutMilliseconds - static_cast<int>(timer.elapsed()));
    };
    QLocalSocket receiptSocket;
    QLocalSocket legacySocket;
    QLocalSocket* selectedSocket = &receiptSocket;
    const auto connectReceipts = [&] {
        receiptSocket.connectToServer(m_receiptServerName, QIODevice::ReadWrite);
        return receiptSocket.waitForConnected(std::min(100, remaining()));
    };
    const auto receiptsUnavailable = [&] {
        return !receiptRequired && (receiptSocket.error() == QLocalSocket::ServerNotFoundError ||
                                    receiptSocket.error() == QLocalSocket::ConnectionRefusedError);
    };
    int protocolVersion = kReceiptProtocolVersion;
    if (!connectReceipts()) {
        // Only an absent endpoint permits legacy delivery. A busy, inaccessible, or
        // unresponsive modern endpoint retains its receipt contract on every retry.
        if (!receiptsUnavailable()) {
            if (error)
                *error = receiptSocket.errorString();
            return ForwardResult::Retry;
        }
        if (remaining() <= 0)
            return ForwardResult::Retry;
        legacySocket.connectToServer(m_legacyServerName, QIODevice::ReadWrite);
        if (!legacySocket.waitForConnected(std::min(100, remaining()))) {
            if (error)
                *error = legacySocket.errorString();
            return ForwardResult::Retry;
        }
        // A modern primary may have published both endpoints between our first probe and
        // the legacy connection. Recheck before sending any launch data on that connection.
        if (remaining() <= 0)
            return ForwardResult::Retry;
        receiptSocket.abort();
        if (!connectReceipts()) {
            if (!receiptsUnavailable()) {
                if (error)
                    *error = receiptSocket.errorString();
                return ForwardResult::Retry;
            }
            selectedSocket = &legacySocket;
            protocolVersion = kLegacyProtocolVersion;
            payload = requestPayload(arguments, {}, protocolVersion);
        }
    }
    // Keep this contract across attempts, even if the endpoint disappears after a write.
    if (protocolVersion == kReceiptProtocolVersion)
        receiptRequired = true;
    if (remaining() <= 0)
        return ForwardResult::Retry;
    auto& socket = *selectedSocket;
    const bool written = writeFrame(socket, framedPayload(payload), remaining);
    if (protocolVersion == kLegacyProtocolVersion) {
        // A legacy primary cannot deduplicate retries or acknowledge dispatch. Once a
        // write starts, never resend it, even if completion of that write is uncertain.
        if (error && !written)
            *error = socket.errorString();
        if (written)
            socket.disconnectFromServer();
        return written ? ForwardResult::Forwarded : ForwardResult::Failed;
    }
    if (written) {
        QByteArray receipt;
        while (timer.elapsed() < timeoutMilliseconds) {
            receipt.append(socket.readAll());
            if (receipt.size() >= static_cast<qsizetype>(sizeof(quint32))) {
                const auto size = qFromBigEndian<quint32>(receipt.constData());
                if (size == 0 || size > 1024)
                    break;
                if (receipt.size() >= static_cast<qsizetype>(sizeof(quint32)) + size) {
                    const auto response =
                        QJsonDocument::fromJson(receipt.mid(sizeof(quint32), size)).object();
                    if (response.value(QStringLiteral("version")).toInt() ==
                            kReceiptProtocolVersion &&
                        response.value(QStringLiteral("requestId")).toString() == requestId &&
                        response.value(QStringLiteral("accepted")).isBool()) {
                        if (response.value(QStringLiteral("accepted")).toBool())
                            return ForwardResult::Forwarded;
                        if (error)
                            *error = QStringLiteral(
                                "The primary application refused the launch request");
                        return ForwardResult::Failed;
                    }
                    break;
                }
            }
            if (!socket.waitForReadyRead(remaining()))
                break;
        }
    }
    if (error != nullptr) {
        *error =
            QStringLiteral("The primary application did not acknowledge the launch request: ") +
            socket.errorString();
    }
    return ForwardResult::Retry;
}

void SingleInstanceCoordinator::acceptConnections(QLocalServer* server, int protocolVersion) {
    while (m_acceptingRequests && server != nullptr && server->hasPendingConnections()) {
        QLocalSocket* socket = server->nextPendingConnection();
        if (socket == nullptr) {
            continue;
        }
        m_clients.insert(socket);
        connect(socket, &QLocalSocket::disconnected, this,
                [this, socket] { m_clients.remove(socket); });
        connect(socket, &QObject::destroyed, this, [this, socket] { m_clients.remove(socket); });
        socket->setProperty("snowShotLaunchBuffer", QByteArray());
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket, protocolVersion]() { readSocket(socket, protocolVersion); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        readSocket(socket, protocolVersion);
    }
}

void SingleInstanceCoordinator::readSocket(QLocalSocket* socket, int protocolVersion) {
    if (socket == nullptr || !m_acceptingRequests) {
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
            object.value(QStringLiteral("version")).toInt() != protocolVersion ||
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
        const auto requestIdValue = object.value(QStringLiteral("requestId"));
        const QString requestId = requestIdValue.toString();
        if ((protocolVersion == kReceiptProtocolVersion || !requestIdValue.isUndefined()) &&
            (!requestIdValue.isString() || requestId.isEmpty() || requestId.size() > 64))
            valid = false;
        if (valid) {
            const bool duplicate = !requestId.isEmpty() && m_receivedRequestIds.contains(requestId);
            bool accepted = duplicate;
            if (!duplicate) {
                try {
                    const auto admission = m_admission;
                    accepted = !admission || admission(arguments);
                } catch (...) {
                    accepted = false;
                }
            }
            if (accepted && !duplicate) {
                if (!requestId.isEmpty()) {
                    m_receivedRequestIds.insert(requestId);
                    m_requestOrder.enqueue(requestId);
                    if (m_requestOrder.size() > kMaximumRememberedRequests)
                        m_receivedRequestIds.remove(m_requestOrder.dequeue());
                }
                dispatchRequest(std::move(arguments));
            }
            if (!m_acceptingRequests)
                break;
            if (requestId.isEmpty())
                continue;
            const auto receipt =
                framedPayload(QJsonDocument(QJsonObject{
                                                {QStringLiteral("version"), protocolVersion},
                                                {QStringLiteral("accepted"), accepted},
                                                {QStringLiteral("requestId"), requestId},
                                            })
                                  .toJson(QJsonDocument::Compact));
            socket->write(receipt);
            socket->flush();
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
