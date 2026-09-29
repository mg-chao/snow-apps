#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
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
    QHash<QByteArray, QByteArray> bodies;
    QList<QByteArray> requests;
    int delay = 0;
    int status = 200;
    Server() {
        require(server.listen(QHostAddress::LocalHost), "release server starts");
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
                        const QByteArray path =
                            buffer.split('\n').first().split(' ').at(1).split('?').first();
                        requests.append(path);
                        const QByteArray body = bodies.value(path, "[]");
                        const QByteArray response =
                            "HTTP/1.1 " + QByteArray::number(status) +
                            " Test\r\nContent-Length: " + QByteArray::number(body.size()) +
                            "\r\nConnection: close\r\n\r\n" + body;
                        QTimer::singleShot(delay, socket, [socket, response] {
                            socket->write(response);
                            socket->disconnectFromHost();
                        });
                    });
            }
        });
    }
    QUrl api() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/releases").arg(server.serverPort()));
    }
};
UpdateService::Options options(const Server& github, const Server& gitee) {
    UpdateService::Options value;
    value.githubApiUrl = github.api();
    value.giteeApiUrl = gitee.api();
    value.allowLocalHttp = true;
    value.installedVersion = QStringLiteral("1.0.0");
    value.startupCheckDelay = 10ms;
    value.automaticCheckInterval = 80ms;
    return value;
}
QString assetName(const QString& version) {
#if defined(Q_PROCESSOR_ARM_64)
    const QString arch = QStringLiteral("arm64");
#else
    const QString arch = QStringLiteral("x86_64");
#endif
    return QStringLiteral("snow-shot-%1-macos-%2.dmg").arg(version, arch);
}
QJsonObject githubRelease(const QString& version, bool draft = false) {
    const QString tag = QStringLiteral("v%1_snow-shot").arg(version);
    const QString name = assetName(version);
    QJsonArray assets;
    for (const QString& asset : {name, name + QStringLiteral(".sha256")})
        assets.append(QJsonObject{
            {QStringLiteral("name"), asset},
            {QStringLiteral("browser_download_url"),
             QStringLiteral("https://github.com/mg-chao/snow-apps/releases/download/%1/%2")
                 .arg(tag, asset)}});
    return {{QStringLiteral("tag_name"), tag},
            {QStringLiteral("draft"), draft},
            {QStringLiteral("prerelease"), version.contains(u'-')},
            {QStringLiteral("assets"), assets}};
}
QJsonObject giteeRelease(const QString& version) {
    return {{QStringLiteral("id"), 123},
            {QStringLiteral("tag_name"), QStringLiteral("v%1_snow-shot").arg(version)}};
}
QJsonArray giteeAssets(const QString& version) {
    const QString name = assetName(version);
    QJsonArray assets;
    for (const QString& asset : {name, name + QStringLiteral(".sha256")})
        assets.append(QJsonObject{
            {QStringLiteral("name"), asset},
            {QStringLiteral("browser_download_url"),
             QStringLiteral(
                 "https://gitee.com/mg-chao/snow-apps/releases/download/v%1_snow-shot/%2")
                 .arg(version, asset)}});
    return assets;
}
void setGithub(Server& server, const QJsonArray& releases) {
    server.bodies.insert("/releases", QJsonDocument(releases).toJson());
}
void setGitee(Server& server, const QJsonArray& releases, const QJsonArray& assets) {
    server.bodies.insert("/releases", QJsonDocument(releases).toJson());
    server.bodies.insert("/releases/123/attach_files", QJsonDocument(assets).toJson());
}
void releaseRaceAndValidation() {
    Server github;
    Server gitee;
    setGithub(github, {githubRelease(QStringLiteral("2.0.0"))});
    setGitee(gitee, {giteeRelease(QStringLiteral("3.0.0"))}, giteeAssets(QStringLiteral("3.0.0")));
    gitee.delay = 40;
    UpdateService service(options(github, gitee));
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"2.0.0" &&
                service.status().downloadUrl.host() == u"github.com",
            "first validated GitHub release wins even when Gitee has a newer version");
    require(!github.requests.isEmpty() && !gitee.requests.isEmpty(), "both channels start");

    github.delay = 40;
    gitee.delay = 0;
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"3.0.0" &&
                service.status().downloadUrl.host() == u"gitee.com",
            "first validated Gitee release wins");

    setGitee(gitee, {giteeRelease(QStringLiteral("9.0.0"))}, {});
    github.delay = 0;
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"2.0.0", "incomplete Gitee release cannot win");

    auto incompleteGitee = giteeRelease(QStringLiteral("9.0.0"));
    incompleteGitee.insert(QStringLiteral("id"), 124);
    setGitee(gitee, {incompleteGitee, giteeRelease(QStringLiteral("3.0.0"))},
             giteeAssets(QStringLiteral("3.0.0")));
    gitee.bodies.insert("/releases/124/attach_files", "[]");
    github.delay = 80;
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"3.0.0" &&
                service.status().downloadUrl.host() == u"gitee.com",
            "Gitee skips an incomplete newer release");

    auto foreignAssets = giteeAssets(QStringLiteral("3.0.0"));
    auto foreignDmg = foreignAssets[0].toObject();
    foreignDmg.insert(QStringLiteral("browser_download_url"),
                      QStringLiteral("https://example.invalid/package.dmg"));
    foreignAssets.replace(0, foreignDmg);
    setGitee(gitee, {giteeRelease(QStringLiteral("3.0.0"))}, foreignAssets);
    github.delay = 40;
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"2.0.0" &&
                service.status().downloadUrl.host() == u"github.com",
            "foreign Gitee attachment URL cannot win");

    auto incompleteGithub = githubRelease(QStringLiteral("9.0.0"));
    incompleteGithub.insert(QStringLiteral("assets"), QJsonArray{});
    setGithub(github, {incompleteGithub, githubRelease(QStringLiteral("2.0.0"))});
    setGitee(gitee, {}, {});
    github.delay = 0;
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"2.0.0" &&
                service.status().downloadUrl.host() == u"github.com",
            "GitHub skips an incomplete newer release");

    setGithub(github, {githubRelease(QStringLiteral("8.0.0"), true),
                       githubRelease(QStringLiteral("2.0.0-beta"))});
    setGitee(gitee, {}, {});
    service.check();
    waitFor([&] { return service.status().state != UpdateState::Checking; });
    require(service.status().version == u"2.0.0-beta",
            "published previews qualify but drafts do not");
}
void schedulingAndFailures() {
    Server github;
    Server gitee;
    setGithub(github, {githubRelease(QStringLiteral("2.0.0"))});
    setGitee(gitee, {}, {});
    UpdateService service(options(github, gitee));
    int notices = 0;
    QObject::connect(&service, &UpdateService::automaticUpdateAvailable, &service,
                     [&](const QString&) { ++notices; });
    service.setMode(QStringLiteral("manual"));
    service.start();
    pump(30);
    require(github.requests.isEmpty() && gitee.requests.isEmpty(), "manual mode does not check");
    service.setMode(QStringLiteral("download"));
    waitFor([&] { return notices == 1; });
    require(service.status().state == UpdateState::Available, "automatic check finds release");
    service.setMode(QStringLiteral("manual"));
    const int count = github.requests.size() + gitee.requests.size();
    pump(120);
    require(github.requests.size() + gitee.requests.size() == count,
            "manual mode stops scheduled checks");
    service.download();
    service.beginApply();
    service.requestRestart();
    require(service.status().state == UpdateState::Available, "macOS does not install in app");

    github.status = 503;
    gitee.status = 503;
    service.check();
    waitFor([&] { return service.status().state == UpdateState::Failed; });
    require(!service.status().error.isEmpty(), "both failing channels report a manual error");
    auto* network = service.findChild<QNetworkAccessManager*>();
    require(network && network->proxy().type() == QNetworkProxy::NoProxy, "proxy defaults to none");
    service.setSystemProxy(true);
    require(network->proxyFactory() != nullptr, "system proxy is configurable");
    service.setSystemProxy(false);
    require(network->proxyFactory() == nullptr, "direct networking restored");

    github.status = 200;
    gitee.status = 200;
    github.delay = 80;
    gitee.delay = 80;
    service.check();
    service.cancel();
    require(!service.busy(), "cancellation aborts both channels");
    pump(100);
    require(service.status().state == UpdateState::Failed, "late responses cannot replace status");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    releaseRaceAndValidation();
    schedulingAndFailures();
    return 0;
}
