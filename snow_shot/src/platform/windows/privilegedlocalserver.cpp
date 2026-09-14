#include "snow_shot/platform/windows/privilegedlocalserver.h"

#include <QLocalSocket>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <sddl.h>
#include <QWinEventNotifier>
#endif

namespace snow_shot::platform::windows {
struct PrivilegedLocalServer::Impl {
    explicit Impl(PrivilegedLocalServer& owner) : q(owner) {}
    PrivilegedLocalServer& q;
    QList<QLocalSocket*> pending;
#ifndef Q_OS_WIN
    QLocalServer server;
#endif
#ifdef Q_OS_WIN
    QString name;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    std::unique_ptr<QWinEventNotifier> notifier;
    bool first = true;
    ~Impl() {
        stop();
    }
    void stop() {
        notifier.reset();
        if (pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pipe, &overlapped);
            DWORD transferred = 0;
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (overlapped.hEvent) {
            CloseHandle(overlapped.hEvent);
            overlapped = {};
        }
    }
    bool start() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        QByteArray user(static_cast<qsizetype>(size), '\0');
        const bool read = GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE;
        CloseHandle(token);
        if (!read)
            return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid))
            return false;
        const QString sddl =
            QStringLiteral("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%1)S:(ML;;NW;;;ME)")
                .arg(QString::fromWCharArray(sid));
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.toStdWString().c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        pipe = CreateNamedPipeW(
            name.toStdWString().c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &attributes);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE)
            return false;
        first = false;
        overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            stop();
            return false;
        }
        notifier = std::make_unique<QWinEventNotifier>(overlapped.hEvent, &q);
        QObject::connect(notifier.get(), &QWinEventNotifier::activated, &q, [this] {
            notifier->setEnabled(false);
            DWORD transferred = 0;
            const bool connected =
                GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
            const HANDLE accepted = pipe;
            pipe = INVALID_HANDLE_VALUE;
            // Defer notifier destruction until its activation signal has unwound.
            notifier.release()->deleteLater();
            CloseHandle(overlapped.hEvent);
            overlapped = {};
            if (!start())
                emit q.acceptError(QAbstractSocket::SocketResourceError);
            if (connected) {
                auto* socket = new QLocalSocket(&q);
                if (socket->setSocketDescriptor(reinterpret_cast<qintptr>(accepted))) {
                    pending.append(socket);
                    emit q.newConnection();
                } else {
                    socket->deleteLater();
                    CloseHandle(accepted);
                }
            } else
                CloseHandle(accepted);
        });
        if (!ConnectNamedPipe(pipe, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED)
                SetEvent(overlapped.hEvent);
            else if (error != ERROR_IO_PENDING) {
                stop();
                return false;
            }
        }
        return true;
    }
#endif
};
PrivilegedLocalServer::PrivilegedLocalServer(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {
#ifndef Q_OS_WIN
    connect(&m_impl->server, &QLocalServer::newConnection, this,
            &PrivilegedLocalServer::newConnection);
    connect(&m_impl->server, &QLocalServer::acceptError, this, &PrivilegedLocalServer::acceptError);
#endif
}
PrivilegedLocalServer::~PrivilegedLocalServer() = default;
bool PrivilegedLocalServer::listen(const QString& name) {
#ifdef Q_OS_WIN
    close();
    m_impl->first = true;
    m_impl->name = QStringLiteral("\\\\.\\pipe\\") + name;
    return m_impl->start();
#else
    return m_impl->server.listen(name);
#endif
}
void PrivilegedLocalServer::close() {
#ifdef Q_OS_WIN
    m_impl->stop();
#endif
#ifndef Q_OS_WIN
    m_impl->server.close();
#endif
    qDeleteAll(m_impl->pending);
    m_impl->pending.clear();
}
QLocalSocket* PrivilegedLocalServer::nextPendingConnection() {
#ifdef Q_OS_WIN
    return m_impl->pending.isEmpty() ? nullptr : m_impl->pending.takeFirst();
#else
    return m_impl->server.nextPendingConnection();
#endif
}
} // namespace snow_shot::platform::windows
