#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <cstdio>
#include <cstdlib>
#include <functional>
#if defined(Q_OS_MACOS)
#include <malloc/malloc.h>
#endif

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
    QList<QByteArray> targets;
    QHash<QByteArray, int> statuses;
    QSet<QByteArray> heldTargets;
    bool contentLength = true;
    bool closeResponse = true;
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
                        const QByteArray rawTarget = buffer.split('\n').first().split(' ').at(1);
                        const QUrl url = QUrl::fromEncoded(rawTarget);
                        const QByteArray path = url.path().toUtf8();
                        QByteArray target = path;
                        const QString page = QUrlQuery(url).queryItemValue(QStringLiteral("page"));
                        if (!page.isEmpty())
                            target += "?page=" + page.toUtf8();
                        requests.append(path);
                        targets.append(target);
                        if (heldTargets.contains(target) || heldTargets.contains(path))
                            return;
                        const QByteArray body = bodies.value(target, bodies.value(path, "[]"));
                        const int responseStatus = statuses.value(target, status);
                        const QByteArray response =
                            "HTTP/1.1 " + QByteArray::number(responseStatus) + " Test\r\n" +
                            (contentLength
                                 ? "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                                 : QByteArray()) +
                            "Connection: close\r\n\r\n" + body;
                        QTimer::singleShot(delay, socket, [this, socket, response] {
                            socket->write(response);
                            if (closeResponse)
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
QJsonArray githubPage(int bodyBytes = 0) {
    QJsonArray releases;
    for (int i = 0; i < 100; ++i) {
        auto release = githubRelease(QStringLiteral("2.0.%1").arg(i));
        if (bodyBytes > 0)
            release.insert(QStringLiteral("body"), QString(bodyBytes, u'x'));
        releases.append(release);
    }
    return releases;
}
bool hasReplyFor(const UpdateService& service, const Server& server) {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const auto* reply : service.findChildren<QNetworkReply*>()) {
        if (reply->url().port() == server.server.serverPort())
            return true;
    }
    return false;
}
void requireStopped(UpdateService& service) {
    require(!service.busy(), "terminal checks are no longer busy");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(service.findChildren<QNetworkReply*>().isEmpty(),
            "terminal checks release their network replies");
    for (const auto* timer : service.findChildren<QTimer*>())
        require(!timer->isActive(), "manual terminal checks stop their transport timers");
}
#if defined(Q_OS_MACOS)
size_t liveHeapBytes() {
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
}
void responseMemoryIsReleased() {
    // Warm Qt's process-wide network machinery before measuring a fresh, application-owned
    // service. Live allocated bytes are independent of the allocator's resident-page caches.
    {
        Server github;
        Server gitee;
        setGithub(github, {githubRelease(QStringLiteral("2.0.0"))});
        UpdateService warmup(options(github, gitee));
        warmup.setMode(QStringLiteral("manual"));
        warmup.check();
        waitFor([&] { return !warmup.busy(); });
        requireStopped(warmup);
    }
    pump(20);
    Server github;
    Server gitee;
    UpdateService service(options(github, gitee));
    service.setMode(QStringLiteral("manual"));
    const size_t baseline = liveHeapBytes();
    // The margin tolerates small Qt connection/cache allocations. It remains far below the
    // old tiny-response allocation (32 MiB) and the unnecessary large response/JSON retention.
    constexpr size_t budget = 2 * 1024 * 1024;
    setGithub(github, {githubRelease(QStringLiteral("2.0.0"))});
    service.check();
    waitFor([&] { return !service.busy(); });
    requireStopped(service);
    github.bodies.clear();
    gitee.bodies.clear();
    pump(20);
    require(liveHeapBytes() <= baseline + budget,
            "tiny completed responses do not retain allocations based on the response limit");

    // Keep an unknown-length response open to observe its allocation before terminal cleanup.
    github.contentLength = false;
    github.closeResponse = false;
    github.bodies.insert("/releases", QByteArray(31, ' '));
    gitee.heldTargets.insert("/releases");
    bool readPartialResponse = false;
    service.check();
    for (auto* reply : service.findChildren<QNetworkReply*>()) {
        if (reply->url().port() == github.server.serverPort()) {
            QObject::connect(reply, &QNetworkReply::readyRead, &service,
                             [&] { readPartialResponse = true; });
        }
    }
    waitFor([&] { return readPartialResponse; });
    require(service.busy(), "partial unknown-length responses keep the check active");
    require(liveHeapBytes() <= baseline + budget,
            "small active responses allocate for available bytes rather than the response limit");
    service.cancel();
    requireStopped(service);
    github.contentLength = true;
    github.closeResponse = true;
    gitee.heldTargets.clear();
    github.bodies.clear();
    pump(20);
    require(liveHeapBytes() <= baseline + budget, "cancellation releases partial response buffers");

    for (int cycle = 0; cycle < 3; ++cycle) {
        QJsonArray releases;
        for (int i = 0; i < 40; ++i) {
            auto release = githubRelease(QStringLiteral("3.0.%1").arg(i));
            release.insert(QStringLiteral("body"), QString(96 * 1024, u'x'));
            releases.append(release);
        }
        setGithub(github, releases);
        releases = {};
        service.check();
        waitFor([&] { return !service.busy(); });
        require(service.status().version == u"3.0.39",
                "large responses still select the newest release");
        requireStopped(service);
        github.bodies.clear();
        gitee.bodies.clear();
        pump(20);
        require(liveHeapBytes() <= baseline + budget,
                "completed checks release large response bodies and parsed release metadata");
    }

    github.heldTargets.insert("/releases");
    QJsonArray giteeReleases;
    for (int i = 0; i < 40; ++i) {
        auto release = giteeRelease(QStringLiteral("4.0.%1").arg(i));
        if (i < 39)
            release.insert(QStringLiteral("id"), 200 + i);
        release.insert(QStringLiteral("body"), QString(96 * 1024, u'x'));
        giteeReleases.append(release);
    }
    setGitee(gitee, giteeReleases, giteeAssets(QStringLiteral("4.0.39")));
    giteeReleases = {};
    service.check();
    waitFor([&] { return !service.busy(); });
    require(service.status().version == u"4.0.39" &&
                service.status().downloadUrl.host() == u"gitee.com",
            "large Gitee responses still select a validated release");
    requireStopped(service);
    github.bodies.clear();
    gitee.bodies.clear();
    pump(20);
    require(liveHeapBytes() <= baseline + budget,
            "Gitee completion releases release and attachment working data");

    github.heldTargets.clear();
    github.heldTargets.insert("/releases?page=2");
    gitee.heldTargets.insert("/releases");
    setGithub(github, githubPage(32 * 1024));
    service.check();
    waitFor([&] { return github.targets.contains("/releases?page=2"); });
    github.bodies.clear();
    gitee.bodies.clear();
    pump(20);
    require(liveHeapBytes() <= baseline + budget,
            "pagination keeps only required candidate data after parsing a large page");
    service.cancel();
    requireStopped(service);
    pump(20);
    require(liveHeapBytes() <= baseline + budget,
            "cancellation releases candidates accumulated during pagination");

    // A failed channel must discard its data immediately while its peer is still checking.
    github.heldTargets.clear();
    github.targets.clear();
    setGithub(github, githubPage(32 * 1024));
    github.bodies.insert("/releases?page=2", "invalid JSON");
    service.check();
    waitFor([&] {
        return github.targets.contains("/releases?page=2") && !hasReplyFor(service, github);
    });
    require(service.busy(), "one failed source leaves its peer running");
    github.bodies.clear();
    gitee.bodies.clear();
    pump(20);
    require(liveHeapBytes() <= baseline + budget,
            "source failure releases accumulated data before the peer completes");
    service.cancel();
    requireStopped(service);

    {
        Server timeoutGithub;
        Server timeoutGitee;
        timeoutGithub.heldTargets.insert("/releases");
        timeoutGitee.heldTargets.insert("/releases/123/attach_files");
        auto value = options(timeoutGithub, timeoutGitee);
        value.requestTimeout = 1s;
        UpdateService timeoutService(value);
        timeoutService.setMode(QStringLiteral("manual"));
        const size_t timeoutBaseline = liveHeapBytes();
        auto release = giteeRelease(QStringLiteral("5.0.0"));
        release.insert(QStringLiteral("body"), QString(4 * 1024 * 1024, u'x'));
        setGitee(timeoutGitee, {release}, {});
        release = {};
        timeoutService.check();
        waitFor([&] { return timeoutGitee.requests.contains("/releases/123/attach_files"); });
        timeoutGitee.bodies.clear();
        waitFor([&] { return !timeoutService.busy(); });
        require(timeoutService.status().state == UpdateState::Failed,
                "transport deadlines terminate checks with pending attachments");
        requireStopped(timeoutService);
        pump(20);
        require(liveHeapBytes() <= timeoutBaseline + budget,
                "timeout releases pending attachment candidates and response data");
    }
}
#endif
void paginationAndAttachmentFallback() {
    {
        Server github;
        Server gitee;
        auto page = githubPage();
        page.replace(0, githubRelease(QStringLiteral("8.0.0")));
        setGithub(github, page);
        github.bodies.insert(
            "/releases?page=2",
            QJsonDocument(QJsonArray{githubRelease(QStringLiteral("7.0.0"))}).toJson());
        gitee.heldTargets.insert("/releases");
        UpdateService service(options(github, gitee));
        service.setMode(QStringLiteral("manual"));
        service.check();
        waitFor([&] { return !service.busy(); });
        require(github.targets.contains("/releases?page=2"), "GitHub follows full release pages");
        require(service.status().version == u"8.0.0",
                "transport replacement preserves the best candidate from previous pages");
        requireStopped(service);
    }
    {
        Server github;
        Server gitee;
        github.heldTargets.insert("/releases");
        QJsonArray page;
        for (int i = 0; i < 99; ++i) {
            auto release = giteeRelease(QStringLiteral("2.0.%1").arg(i));
            release.insert(QStringLiteral("id"), 200 + i);
            page.append(release);
        }
        page.append(giteeRelease(QStringLiteral("8.0.0")));
        setGitee(gitee, page, giteeAssets(QStringLiteral("8.0.0")));
        auto newer = giteeRelease(QStringLiteral("9.0.0"));
        newer.insert(QStringLiteral("id"), 124);
        gitee.bodies.insert("/releases?page=2", QJsonDocument(QJsonArray{newer}).toJson());
        gitee.bodies.insert("/releases/124/attach_files", "[]");
        UpdateService service(options(github, gitee));
        service.setMode(QStringLiteral("manual"));
        service.check();
        waitFor([&] { return !service.busy(); });
        require(gitee.targets.contains("/releases?page=2"), "Gitee follows full release pages");
        require(gitee.requests.contains("/releases/124/attach_files") &&
                    gitee.requests.contains("/releases/123/attach_files"),
                "Gitee requests attachments for successive release candidates");
        require(service.status().version == u"8.0.0" &&
                    service.status().downloadUrl.host() == u"gitee.com",
                "attachment fallback preserves candidate versions and release IDs across pages");
        requireStopped(service);
    }
}
void cancellationAndDestructionAfterPagination() {
    Server github;
    Server gitee;
    setGithub(github, githubPage());
    github.heldTargets.insert("/releases?page=2");
    gitee.heldTargets.insert("/releases");
    UpdateService service(options(github, gitee));
    service.setMode(QStringLiteral("manual"));
    service.check();
    waitFor([&] { return github.targets.contains("/releases?page=2"); });
    service.cancel();
    require(service.status().state == UpdateState::Idle,
            "cancellation after pagination restores the previous result");
    requireStopped(service);

    // Automatic cancellation uses the same cleanup when the user switches to manual mode.
    github.targets.clear();
    service.setMode(QStringLiteral("check"));
    service.check(false);
    waitFor([&] { return github.targets.contains("/releases?page=2"); });
    service.setMode(QStringLiteral("manual"));
    requireStopped(service);

    github.targets.clear();
    auto* pending = new UpdateService(options(github, gitee));
    pending->setMode(QStringLiteral("manual"));
    pending->check();
    waitFor([&] { return github.targets.contains("/releases?page=2"); });
    QList<QPointer<QNetworkReply>> replies;
    for (auto* reply : pending->findChildren<QNetworkReply*>())
        replies.append(reply);
    require(!replies.isEmpty(), "pending pagination has live network replies");
    delete pending;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (const auto& reply : replies)
        require(!reply, "service destruction releases pending replies");
}
void sourceFailureAndTimeout() {
    Server github;
    Server gitee;
    setGithub(github, githubPage());
    github.bodies.insert("/releases?page=2", "invalid JSON");
    gitee.heldTargets.insert("/releases");
    auto value = options(github, gitee);
    value.requestTimeout = 250ms;
    UpdateService service(value);
    service.setMode(QStringLiteral("manual"));
    service.check();
    waitFor([&] {
        return github.targets.contains("/releases?page=2") && !hasReplyFor(service, github);
    });
    require(service.busy() && service.status().state == UpdateState::Checking,
            "a malformed later page retires only the failed source");
    waitFor([&] { return !service.busy(); });
    require(service.status().state == UpdateState::Failed,
            "the remaining source deadline completes a failed check");
    requireStopped(service);

    github.bodies.remove("/releases?page=2");
    github.statuses.insert("/releases?page=2", 503);
    github.targets.clear();
    service.check();
    waitFor([&] {
        return github.targets.contains("/releases?page=2") && !hasReplyFor(service, github);
    });
    require(service.busy(), "a failed later HTTP response does not terminate its peer");
    service.cancel();
    requireStopped(service);
}
void responseLimit() {
    constexpr qsizetype limit = 8 * 1024 * 1024;
    for (const bool contentLength : {true, false}) {
        Server github;
        Server gitee;
        github.contentLength = contentLength;
        setGithub(github, {githubRelease(QStringLiteral("2.0.0"))});
        auto body = github.bodies.value("/releases");
        body.append(limit - body.size(), ' ');
        github.bodies.insert("/releases", body);
        body = {};
        gitee.heldTargets.insert("/releases");
        UpdateService service(options(github, gitee));
        service.setMode(QStringLiteral("manual"));
        service.check();
        waitFor([&] { return !service.busy(); });
        require(service.status().version == u"2.0.0",
                "responses at the inclusive byte limit remain valid with or without a length");
        requireStopped(service);

        github.bodies["/releases"].append(' ');
        gitee.heldTargets.clear();
        gitee.status = 503;
        service.check();
        waitFor([&] { return !service.busy(); });
        require(service.status().state == UpdateState::Failed,
                "declared and streamed responses exceeding the byte limit are rejected");
        requireStopped(service);
    }
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
    const qsizetype count = github.requests.size() + gitee.requests.size();
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
#if defined(Q_OS_MACOS)
    responseMemoryIsReleased();
#endif
    releaseRaceAndValidation();
    schedulingAndFailures();
    paginationAndAttachmentFallback();
    cancellationAndDestructionAfterPagination();
    sourceFailureAndTimeout();
    responseLimit();
    return 0;
}
