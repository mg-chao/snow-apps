#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace snow_shot::update;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
void waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(predicate(), "asynchronous check completed within deadline");
}
void pump(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
struct Server {
    QTcpServer server;
    QByteArray body = "2.0.0\n";
    QByteArray request;
    int count = 0;
    int delay = 0;
    int status = 200;
    bool omitLength = false;
    Server() {
        require(server.listen(QHostAddress::LocalHost), "local version server starts");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(
                    socket, &QTcpSocket::readyRead, &server,
                    [this, socket, buffer = QByteArray()]() mutable {
                        buffer += socket->readAll();
                        if (!buffer.contains("\r\n\r\n") || socket->property("responded").toBool())
                            return;
                        socket->setProperty("responded", true);
                        request = buffer;
                        ++count;
                        const QByteArray response =
                            "HTTP/1.1 " + QByteArray::number(status) + " Test\r\n" +
                            (omitLength
                                 ? QByteArray()
                                 : "Content-Length: " + QByteArray::number(body.size()) + "\r\n") +
                            "Connection: close\r\n\r\n" + body;
                        QTimer::singleShot(delay, socket, [socket, response] {
                            socket->write(response);
                            socket->disconnectFromHost();
                        });
                    });
            }
        });
    }
    UpdateService::Options options() const {
        UpdateService::Options result;
        result.baseUrl =
            QUrl(QStringLiteral("http://127.0.0.1:%1/ignored/").arg(server.serverPort()));
        result.allowLocalHttp = true;
        result.installedVersion = QStringLiteral("1.0.0");
        result.startupCheckDelay = 10ms;
        result.automaticCheckInterval = 80ms;
        return result;
    }
};

void versionsAndTransport() {
    Server server;
    for (const auto& pair : {std::pair{"1.0.0", UpdateState::Idle},
                             {"0.9.0", UpdateState::Idle},
                             {"1.0.0+build.42", UpdateState::Idle},
                             {"1.0.0-rc.1", UpdateState::Idle},
                             {"1.1.0-beta.1", UpdateState::Available},
                             {"2.0.0\n", UpdateState::Available},
                             {"999999999999999999999999.0.0", UpdateState::Available},
                             {"01.0.0", UpdateState::Failed},
                             {"1.0", UpdateState::Failed},
                             {"1.0.0-01", UpdateState::Failed},
                             {"v2.0.0", UpdateState::Failed},
                             {"2.0.0\n3.0.0", UpdateState::Failed},
                             {"<html>error</html>", UpdateState::Failed},
                             {"", UpdateState::Failed}}) {
        server.body = pair.first;
        UpdateService service(server.options());
        service.check();
        waitFor([&] { return service.status().state != UpdateState::Checking; });
        require(service.status().state == pair.second, pair.first);
        require(server.request.startsWith("GET /latest-version.txt HTTP/1.1"),
                "uses root version endpoint");
    }
    for (const auto& pair : {std::pair{"1.0.0-beta.2", "1.0.0-beta.11"},
                             {"1.0.0-beta.11", "1.0.0-rc.1"},
                             {"1.0.0-rc.1", "1.0.0"},
                             {"1.0.0-1", "1.0.0-alpha"},
                             {"1.0.0-alpha", "1.0.0-alpha.1"}}) {
        auto options = server.options();
        options.installedVersion = QString::fromLatin1(pair.first);
        server.body = pair.second;
        UpdateService service(options);
        service.check();
        waitFor([&] { return service.status().state != UpdateState::Checking; });
        require(service.status().state == UpdateState::Available, "SemVer prerelease precedence");
    }
    for (bool omitLength : {false, true}) {
        server.omitLength = omitLength;
        server.body = QByteArray(4097, '1');
        UpdateService service(server.options());
        service.check();
        waitFor([&] { return service.status().state != UpdateState::Checking; });
        require(service.status().state == UpdateState::Failed,
                "reject oversized version, with or without length");
    }
    server.omitLength = false;
    server.body = "2.0.0";
    server.status = 404;
    UpdateService failed(server.options());
    failed.check();
    waitFor([&] { return failed.status().state != UpdateState::Checking; });
    require(failed.status().state == UpdateState::Failed,
            "HTTP errors fail visibly for manual checks");
    server.status = 200;
    failed.check();
    waitFor([&] { return failed.status().state != UpdateState::Checking; });
    require(failed.status().state == UpdateState::Available, "manual retry recovers");

    auto options = server.options();
    options.allowLocalHttp = false;
    UpdateService insecure(options);
    insecure.check();
    require(insecure.status().state == UpdateState::Failed, "production rejects HTTP");
    options.allowLocalHttp = true;
    options.baseUrl = QUrl(QStringLiteral("http://example.com"));
    UpdateService remoteHttp(options);
    remoteHttp.check();
    require(remoteHttp.status().state == UpdateState::Failed,
            "test HTTP permission is loopback only");
}

void scheduling() {
    Server server;
    UpdateService service(server.options());
    int notices = 0;
    QObject::connect(&service, &UpdateService::automaticUpdateAvailable, &service,
                     [&](const QString& version) {
                         require(version == u"2.0.0", "notice includes version");
                         ++notices;
                     });
    service.setMode(QStringLiteral("manual"));
    service.start();
    service.start();
    pump(30);
    require(server.count == 0, "manual startup does not contact server or probe a helper");
    service.setMode(QStringLiteral("download"));
    waitFor([&] { return notices == 1; });
    waitFor([&] { return server.count >= 2 && service.status().state == UpdateState::Available; });
    require(notices == 1, "automatic checks notify once per version per session");
    service.setMode(QStringLiteral("manual"));
    const int stopped = server.count;
    pump(120);
    require(server.count == stopped, "switching to manual stops periodic checks");
    service.download();
    service.beginApply();
    service.requestRestart();
    require(service.status().state == UpdateState::Available && server.count == stopped,
            "macOS never downloads or applies updates");

    server.delay = 40;
    service.setMode(QStringLiteral("check"));
    waitFor([&] { return service.status().state == UpdateState::Checking; });
    service.setMode(QStringLiteral("manual"));
    require(service.status().state == UpdateState::Available,
            "cancel background check preserves available update");
    pump(60);
    require(notices == 1, "cancelled check cannot deliver notice");
    const int before = server.count;
    service.check();
    service.check();
    service.check(false);
    waitFor([&] { return service.status().state == UpdateState::Available; });
    require(server.count == before + 1 && notices == 1,
            "overlapping manual checks coalesce without notice");
}

void failuresAndProxy() {
    Server server;
    auto options = server.options();
    options.requestTimeout = 15ms;
    server.delay = 100;
    UpdateService service(options);
    service.check();
    waitFor([&] { return service.status().state == UpdateState::Failed; });
    require(service.status().error.contains(u"timed out"), "wall-clock timeout is bounded");
    server.delay = 0;
    service.check();
    waitFor([&] { return service.status().state == UpdateState::Available; });
    server.body = "invalid";
    service.check(false);
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().state == UpdateState::Available && service.status().error.isEmpty(),
            "background failures preserve the prior update without error UI");
    auto* network = service.findChild<QNetworkAccessManager*>();
    require(network && network->proxy().type() == QNetworkProxy::NoProxy, "proxy defaults to none");
    service.setSystemProxy(true);
    require(network->proxyFactory() != nullptr, "system proxy uses per-request system lookup");
    service.setSystemProxy(false);
    require(network->proxyFactory() == nullptr && network->proxy().type() == QNetworkProxy::NoProxy,
            "turning off system proxy restores direct networking");
    server.delay = 5;
    service.check(false);
    service.check(true);
    waitFor([&] { return service.status().state == UpdateState::Failed; });
    require(!service.status().error.isEmpty(),
            "manual check joins automatic request with visible errors");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    versionsAndTransport();
    scheduling();
    failuresAndProxy();
    return 0;
}
