#include "snow_shot/app/mcp/screenshotmcpserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#endif

namespace snow_shot::app::mcp {
namespace {
constexpr quint32 kMaximumFrameBytes = 64u * 1024u * 1024u;
constexpr quint32 kMaximumRequestBytes = 1024u * 1024u + 4096u;
const QString kProtocol = QStringLiteral("snow-shot-mcp/1");

bool privateDirectory(const QString& path) {
    if (QFileInfo(path).isSymLink() || !QDir().mkpath(path))
        return false;
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    QByteArray data(static_cast<qsizetype>(length), Qt::Uninitialized);
    const bool read = GetTokenInformation(token, TokenUser, data.data(), length, &length) != 0;
    CloseHandle(token);
    if (!read)
        return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
        return false;
    const QString sddl = QStringLiteral("D:P(A;OICI;FA;;;") + QString::fromWCharArray(sid) + u')';
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    const bool ok =
        SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()),
                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                         descriptor) != 0;
    LocalFree(descriptor);
    return ok;
#else
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
#endif
}
QString randomToken() {
    QByteArray bytes(32, Qt::Uninitialized);
    for (int offset = 0; offset < 32; offset += 4)
        qToBigEndian(QRandomGenerator::system()->generate(), bytes.data() + offset);
    return QString::fromLatin1(bytes.toHex());
}
QJsonObject responseObject(const ScreenshotMcpResponse& r) {
    QJsonObject o{{QStringLiteral("protocol"), kProtocol},
                  {QStringLiteral("request_id"), r.requestId},
                  {QStringLiteral("ok"), r.ok},
                  {QStringLiteral("result"), r.result},
                  {QStringLiteral("session_id"), r.sessionId},
                  {QStringLiteral("attachment_length"), r.attachment.size()}};
    if (r.revision)
        o.insert(QStringLiteral("revision"), static_cast<qint64>(*r.revision));
    if (!r.ok)
        o.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), r.errorCode},
                                                      {QStringLiteral("message"), r.errorMessage},
                                                      {QStringLiteral("details"), r.errorDetails}});
    if (!r.attachment.isEmpty())
        o.insert(QStringLiteral("attachment_mime"), r.attachmentMime);
    return o;
}
ScreenshotMcpResponse failure(const QString& id, const QString& code) {
    ScreenshotMcpResponse r;
    r.requestId = id;
    r.errorCode = code;
    r.errorMessage =
        QCoreApplication::translate("ScreenshotMcpServer", "MCP request failed (%1).").arg(code);
    return r;
}
QByteArray frame(const QJsonObject& o, const QByteArray& attachment) {
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
    const quint64 size = 4 + static_cast<quint64>(json.size()) + attachment.size();
    if (size > kMaximumFrameBytes)
        return {};
    QByteArray bytes(static_cast<qsizetype>(size + 4), Qt::Uninitialized);
    qToBigEndian(static_cast<quint32>(size), bytes.data());
    qToBigEndian(static_cast<quint32>(json.size()), bytes.data() + 4);
    std::copy(json.begin(), json.end(), bytes.begin() + 8);
    std::copy(attachment.begin(), attachment.end(), bytes.begin() + 8 + json.size());
    return bytes;
}
bool constantTimeEqual(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return difference == 0;
}
} // namespace

class ScreenshotMcpServer::SocketWorker final : public QObject {
    Q_OBJECT
  public:
    SocketWorker(QString name, QString token)
        : m_name(std::move(name)), m_token(std::move(token)) {}
    QString start() {
        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        m_server->setMaxPendingConnections(8);
        if (!m_server->listen(m_name))
            return m_server->errorString();
        connect(m_server, &QLocalServer::newConnection, this, [this] { accept(); });
        auto* deadlines = new QTimer(this);
        connect(deadlines, &QTimer::timeout, this, [this] {
            for (auto& client : m_clients)
                if (client.partialTimer.isValid() && client.partialTimer.elapsed() > 5000)
                    client.socket->abort();
        });
        deadlines->start(1000);
        return {};
    }
    void stop() {
        if (m_server)
            m_server->close();
        const auto clients = m_clients;
        m_clients.clear();
        for (const auto& c : clients) {
            c.socket->disconnect(this);
            c.socket->abort();
            delete c.socket;
        }
    }
    void complete(quint64 id, ScreenshotMcpResponse response) {
        auto it = m_clients.find(id);
        if (it == m_clients.end())
            return;
        auto& c = it.value();
        if (c.controlIds.remove(response.requestId)) {
            write(c, response);
            return;
        }
        if (c.activeId != response.requestId)
            return; // Completion from a canceled/timed out request.
        write(c, response);
        c.activeId.clear();
        dispatch(id);
    }
  signals:
    void requestReceived(quint64 connectionId, const QJsonObject& request,
                         const QByteArray& attachment);
    void clientDisconnected(quint64 connectionId);
    void connectionCountChanged(int count);

  private:
    struct Client {
        QLocalSocket* socket = nullptr;
        QByteArray buffer;
        bool authenticated = false;
        QString activeId;
        QSet<QString> controlIds;
        QElapsedTimer partialTimer;
        struct Queued {
            QJsonObject request;
            QElapsedTimer timer;
        };
        QQueue<Queued> queue;
        QElapsedTimer connected;
    };
    void write(Client& c, ScreenshotMcpResponse r) {
        QByteArray bytes = frame(responseObject(r), r.attachment);
        if (bytes.isEmpty())
            bytes =
                frame(responseObject(failure(r.requestId, QStringLiteral("output_too_large"))), {});
        if (c.socket->bytesToWrite() + bytes.size() > kMaximumFrameBytes + 4) {
            c.socket->abort();
            return;
        }
        c.socket->write(bytes);
    }
    void accept() {
        while (m_server->hasPendingConnections()) {
            auto* socket = m_server->nextPendingConnection();
            if (m_clients.size() >= 8) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            const quint64 id = ++m_nextId;
            Client c;
            c.socket = socket;
            c.connected.start();
            m_clients.insert(id, c);
            socket->setReadBufferSize(kMaximumRequestBytes + 8);
            connect(socket, &QLocalSocket::readyRead, this, [this, id] { read(id); });
            connect(
                socket, &QLocalSocket::disconnected, this,
                [this, id, socket] {
                    if (!m_clients.remove(id))
                        return;
                    publishConnectionCount();
                    emit clientDisconnected(id);
                    socket->deleteLater();
                },
                Qt::QueuedConnection);
            QTimer::singleShot(5000, socket, [this, id] {
                const auto it = m_clients.find(id);
                if (it != m_clients.end() && !it->authenticated)
                    it->socket->abort();
            });
        }
    }
    void publishConnectionCount() {
        int count = 0;
        for (const auto& client : std::as_const(m_clients))
            if (client.authenticated)
                ++count;
        emit connectionCountChanged(count);
    }
    void dispatch(quint64 id) {
        auto it = m_clients.find(id);
        if (it == m_clients.end() || !it->activeId.isEmpty() || it->queue.isEmpty())
            return;
        const auto queued = it->queue.dequeue();
        auto request = queued.request;
        request.insert(QStringLiteral("_queue_wait_ms"), queued.timer.elapsed());
        it->activeId = request.value(QStringLiteral("request_id")).toString();
        emit requestReceived(id, request, {});
    }
    void read(quint64 id) {
        auto it = m_clients.find(id);
        if (it == m_clients.end())
            return;
        auto& c = it.value();
        int budget = 32;
        while (c.socket->bytesAvailable() > 0 && budget-- > 0) {
            if (c.buffer.isEmpty())
                c.partialTimer.start();
            // Read only enough for one frame. Never readAll an untrusted stream into memory.
            qsizetype required = 8;
            if (c.buffer.size() >= 8) {
                const auto size = qFromBigEndian<quint32>(c.buffer.constData());
                const auto jsonSize = qFromBigEndian<quint32>(c.buffer.constData() + 4);
                if (size < 6 || size > kMaximumRequestBytes || jsonSize != size - 4) {
                    c.socket->abort();
                    return; // Requests never carry binary or document sessions.
                }
                required = static_cast<qsizetype>(size) + 4;
            }
            c.buffer.append(c.socket->read(required - c.buffer.size()));
            if (c.buffer.size() < 8)
                return;
            const auto size = qFromBigEndian<quint32>(c.buffer.constData());
            const auto jsonSize = qFromBigEndian<quint32>(c.buffer.constData() + 4);
            if (size < 6 || size > kMaximumRequestBytes || jsonSize != size - 4) {
                c.socket->abort();
                return;
            }
            if (c.buffer.size() < static_cast<qsizetype>(size) + 4)
                continue;
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(c.buffer.mid(8, jsonSize), &error);
            c.buffer.clear();
            c.partialTimer.invalidate();
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                c.socket->abort();
                return;
            }
            const auto request = doc.object();
            const QString requestId = request.value(QStringLiteral("request_id")).toString();
            const QString method = request.value(QStringLiteral("method")).toString();
            if (requestId.isEmpty() || requestId.size() > 128 || method.size() > 128 ||
                request.value(QStringLiteral("protocol")).toString() != kProtocol) {
                write(c, failure(requestId, QStringLiteral("protocol_error")));
                c.socket->disconnectFromServer();
                return;
            }
            if (!c.authenticated) {
                const auto params = request.value(QStringLiteral("params")).toObject();
                if (method != QStringLiteral("handshake") ||
                    params.value(QStringLiteral("client_protocol")).toString() != kProtocol ||
                    !constantTimeEqual(params.value(QStringLiteral("token")).toString().toLatin1(),
                                       m_token.toLatin1())) {
                    write(c, failure(requestId, QStringLiteral("unauthorized")));
                    c.socket->disconnectFromServer();
                    return;
                }
                c.authenticated = true;
                publishConnectionCount();
                ScreenshotMcpResponse reply;
                reply.requestId = requestId;
                reply.ok = true;
                reply.result = {{QStringLiteral("protocol"), kProtocol},
                                {QStringLiteral("handshake_ms"), c.connected.elapsed()}};
                write(c, reply);
                continue;
            }
            bool duplicate = c.activeId == requestId || c.controlIds.contains(requestId);
            for (const auto& queued : std::as_const(c.queue))
                duplicate |=
                    queued.request.value(QStringLiteral("request_id")).toString() == requestId;
            if (duplicate) {
                c.socket->abort();
                return;
            }
            if ((method == QStringLiteral("screenshot_cancel") ||
                 method == QStringLiteral("screenshot_state") ||
                 method == QStringLiteral("snow_shot_status")) &&
                !c.activeId.isEmpty()) {
                // Cancellation must not sit behind the operation it cancels. The session adapter
                // completes the active request before completing this cancellation request.
                if (c.controlIds.size() >= 8) {
                    write(c, failure(requestId, QStringLiteral("queue_full")));
                    continue;
                }
                c.controlIds.insert(requestId);
                emit requestReceived(id, request, {});
                continue;
            }
            if (c.queue.size() >= 8) {
                write(c, failure(requestId, QStringLiteral("queue_full")));
                continue;
            }
            Client::Queued queued{request, {}};
            queued.timer.start();
            c.queue.enqueue(std::move(queued));
            dispatch(id);
        }
        if (c.socket->bytesAvailable() > 0)
            QTimer::singleShot(0, this, [this, id] { read(id); });
    }
    QLocalServer* m_server = nullptr;
    QString m_name;
    QString m_token;
    QHash<quint64, Client> m_clients;
    quint64 m_nextId = 0;
};

QString ScreenshotMcpServer::defaultRuntimeDirectory() {
#ifdef Q_OS_WIN
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("SnowShot/mcp"));
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("SnowShot/mcp"));
#endif
}
ScreenshotMcpServer::ScreenshotMcpServer(QObject* parent, QString runtimeDirectory)
    : QObject(parent) {
    if (!runtimeDirectory.isEmpty()) {
        m_runtimeDirectory = std::move(runtimeDirectory);
        m_descriptorPath = QDir(m_runtimeDirectory).filePath(QStringLiteral("snow-shot-mcp.json"));
    } else if (qEnvironmentVariableIsSet("SNOW_SHOT_MCP_DESCRIPTOR")) {
        m_descriptorPath = qEnvironmentVariable("SNOW_SHOT_MCP_DESCRIPTOR");
        if (!m_descriptorPath.isEmpty())
            m_runtimeDirectory = QFileInfo(m_descriptorPath).absolutePath();
    } else {
        m_runtimeDirectory = defaultRuntimeDirectory();
        m_descriptorPath = QDir(m_runtimeDirectory).filePath(QStringLiteral("snow-shot-mcp.json"));
    }
}
ScreenshotMcpServer::~ScreenshotMcpServer() {
    stop();
}
bool ScreenshotMcpServer::start(QString* error) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_running)
        return true;
    if (m_runtimeDirectory.isEmpty() || !QFileInfo(m_descriptorPath).isAbsolute()) {
        if (error)
            *error = tr("Could not secure the MCP runtime directory.");
        return false;
    }
    if (!privateDirectory(m_runtimeDirectory)) {
        if (error)
            *error = tr("Could not secure the MCP runtime directory.");
        return false;
    }
    m_lock = std::make_unique<QLockFile>(
        QDir(m_runtimeDirectory).filePath(QStringLiteral("endpoint.lock")));
    if (!m_lock->tryLock(0)) {
        if (error)
            *error = tr("Another Snow Shot instance owns the MCP endpoint.");
        m_lock.reset();
        return false;
    }
    ++m_epoch;
    m_generation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_token = randomToken();
#ifdef Q_OS_WIN
    m_socketName = QStringLiteral("snow-shot-mcp-") + m_generation;
#else
    m_socketName =
        QDir(m_runtimeDirectory).filePath(QStringLiteral("socket-")) + m_generation.left(8);
#endif
    m_thread = std::make_unique<QThread>();
    m_thread->setObjectName(QStringLiteral("ScreenshotMcpSocket"));
    m_worker = new SocketWorker(m_socketName, m_token);
    m_worker->moveToThread(m_thread.get());
    connect(m_thread.get(), &QThread::finished, m_worker, &QObject::deleteLater);
    const quint64 epoch = m_epoch;
    connect(
        m_worker, &SocketWorker::requestReceived, this,
        [this, epoch](quint64 connection, const QJsonObject& request, const QByteArray& bytes) {
            if (epoch == m_epoch)
                handleRequest(connection, request, bytes);
        },
        Qt::QueuedConnection);
    connect(
        m_worker, &SocketWorker::clientDisconnected, this,
        [this, epoch](quint64 connection) {
            if (epoch == m_epoch)
                handleClientDisconnected(connection);
        },
        Qt::QueuedConnection);
    connect(
        m_worker, &SocketWorker::connectionCountChanged, this,
        [this, epoch](int count) {
            if (epoch == m_epoch)
                QCoreApplication::instance()->setProperty("snowShotMcpConnections", count);
        },
        Qt::QueuedConnection);
    m_thread->start();
    QString listenError;
    QMetaObject::invokeMethod(
        m_worker, [this, &listenError] { listenError = m_worker->start(); },
        Qt::BlockingQueuedConnection);
    if (!listenError.isEmpty() || !writeDescriptor(error)) {
        if (error && !listenError.isEmpty())
            *error = tr("Could not open the local MCP endpoint.");
        stop();
        return false;
    }
    m_running = true;
    QCoreApplication::instance()->setProperty("snowShotMcpRunning", true);
    emit runningChanged(true);
    return true;
}
void ScreenshotMcpServer::stop() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_epoch;
    removeDescriptor();
    if (m_thread) {
        disconnect(m_worker, nullptr, this, nullptr);
        QMetaObject::invokeMethod(m_worker, &SocketWorker::stop, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
        m_worker = nullptr;
        m_thread.reset();
    }
    m_lock.reset();
    m_token.clear();
    QCoreApplication::instance()->setProperty("snowShotMcpRunning", false);
    QCoreApplication::instance()->setProperty("snowShotMcpConnections", 0);
    if (std::exchange(m_running, false))
        emit runningChanged(false);
}
bool ScreenshotMcpServer::isRunning() const {
    return m_running;
}
QString ScreenshotMcpServer::descriptorPath() const {
    return m_descriptorPath;
}
QString ScreenshotMcpServer::socketName() const {
    return m_socketName;
}
void ScreenshotMcpServer::setRequestHandler(RequestHandler h) {
    m_requestHandler = std::move(h);
}
void ScreenshotMcpServer::setClientDisconnectedHandler(ClientDisconnectedHandler h) {
    m_clientDisconnectedHandler = std::move(h);
}
void ScreenshotMcpServer::handleRequest(quint64 connection, const QJsonObject& object,
                                        const QByteArray&) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_running)
        return;
    ScreenshotMcpRequest request;
    request.connectionId = connection;
    request.requestId = object.value(QStringLiteral("request_id")).toString();
    request.method = object.value(QStringLiteral("method")).toString();
    request.sessionId = object.value(QStringLiteral("session_id")).toString();
    request.idempotencyKey = object.value(QStringLiteral("idempotency_key")).toString();
    request.params = object.value(QStringLiteral("params")).toObject();
    const auto revision = object.value(QStringLiteral("expected_revision"));
    if (!revision.isUndefined()) {
        const double number = revision.toDouble(-1);
        if (number < 0 || number > 9007199254740991.0 || std::floor(number) != number) {
            sendResponse(connection,
                         failure(request.requestId, QStringLiteral("invalid_parameters")));
            return;
        }
        request.expectedRevision = static_cast<quint64>(number);
    }
    if (!m_requestHandler) {
        sendResponse(connection, failure(request.requestId, QStringLiteral("unavailable")));
        return;
    }
    QPointer<ScreenshotMcpServer> guard(this);
    const quint64 epoch = m_epoch;
    const qint64 queueWait = object.value(QStringLiteral("_queue_wait_ms")).toInteger();
    m_requestHandler(request, [guard, epoch, connection, queueWait,
                               id = request.requestId](ScreenshotMcpResponse response) mutable {
        if (!guard || epoch != guard->m_epoch)
            return;
        response.requestId = id;
        auto timings = response.result.value(QStringLiteral("timings_ms")).toObject();
        timings.insert(QStringLiteral("queue_wait"), queueWait);
        response.result.insert(QStringLiteral("timings_ms"), timings);
        guard->sendResponse(connection, std::move(response));
    });
}
void ScreenshotMcpServer::handleClientDisconnected(quint64 id) {
    if (m_running && m_clientDisconnectedHandler)
        m_clientDisconnectedHandler(id);
}
void ScreenshotMcpServer::sendResponse(quint64 id, ScreenshotMcpResponse response) {
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, id, response = std::move(response)]() mutable {
            worker->complete(id, std::move(response));
        },
        Qt::QueuedConnection);
}
bool ScreenshotMcpServer::writeDescriptor(QString* error) {
    QSaveFile file(m_descriptorPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error)
            *error = tr("Could not write the private MCP descriptor.");
        return false;
    }
    const QJsonObject object{
        {QStringLiteral("protocol"), kProtocol},
        {QStringLiteral("socket"), m_socketName},
        {QStringLiteral("token"), m_token},
        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
        {QStringLiteral("generation"), m_generation},
        {QStringLiteral("max_frame_bytes"), static_cast<qint64>(kMaximumFrameBytes)}};
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = tr("Could not write the private MCP descriptor.");
        return false;
    }
    return true;
}
void ScreenshotMcpServer::removeDescriptor() {
    // Never remove a replacement descriptor owned by another generation.
    QFile file(m_descriptorPath);
    if (!m_descriptorPath.isEmpty() && file.open(QIODevice::ReadOnly) && file.size() < 16384) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        if (object.value(QStringLiteral("generation")).toString() == m_generation)
            QFile::remove(m_descriptorPath);
    }
}
} // namespace snow_shot::app::mcp
#include "screenshotmcpserver.moc"
