#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

using snow_shot::update::UpdateService;
using snow_shot::update::UpdateState;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

void frame(const QJsonObject& value) {
    std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << '\n'
              << std::flush;
}

void status(const char* state, qint64 received = 0, qint64 total = 0,
            const QJsonObject& error = {}) {
    QJsonObject value{{QStringLiteral("state"), QString::fromLatin1(state)},
                      {QStringLiteral("version"), QStringLiteral("2.0.0")},
                      {QStringLiteral("received"), received},
                      {QStringLiteral("total"), total}};
    if (!error.isEmpty()) {
        value.insert(QStringLiteral("error"), error);
    }
    frame({{QStringLiteral("protocol"), 1},
           {QStringLiteral("type"), QStringLiteral("status")},
           {QStringLiteral("status"), value}});
}

QString argument(int argc, char** argv, const char* name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (QByteArray(argv[index]) == name) {
            return QString::fromLocal8Bit(argv[index + 1]);
        }
    }
    return {};
}

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create fake-sidecar marker");
}

int fakeSidecar(int argc, char** argv) {
    const QString scenario = QUrl(argument(argc, argv, "--base-url")).path().mid(1);
    const QString cache = argument(argc, argv, "--cache");
    if (scenario == u"unsupported-handshake") {
        frame({{QStringLiteral("protocol"), 2},
               {QStringLiteral("type"), QStringLiteral("hello")},
               {QStringLiteral("updaterVersion"), QStringLiteral("test")}});
        return 0;
    }
    frame({{QStringLiteral("protocol"), 1},
           {QStringLiteral("type"), QStringLiteral("hello")},
           {QStringLiteral("updaterVersion"), QStringLiteral("test")},
           {QStringLiteral("platform"), QStringLiteral("windows-x64")},
           {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("check")}}});
    status("Idle");
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const qint64 id = command.value(QStringLiteral("id")).toInteger();
        const QString name = command.value(QStringLiteral("command")).toString();
        frame({{QStringLiteral("protocol"), 1},
               {QStringLiteral("type"), QStringLiteral("command_result")},
               {QStringLiteral("id"), id},
               {QStringLiteral("ok"), true}});
        if (name == u"check") {
            if (scenario == u"all-statuses") {
                status("Unavailable");
                status("Idle");
                status("Checking");
                status("Available");
                status("Downloading", 12, 24);
                status("Verifying", 24, 24);
                status("Ready");
                status("Applying");
                status("Failed", 0, 0,
                       {{QStringLiteral("code"), QStringLiteral("metadata_download_failed")},
                        {QStringLiteral("message"),
                         QStringLiteral("Could not download signed update metadata")},
                        {QStringLiteral("detail"), QStringLiteral("test detail")}});
            } else if (scenario == u"malformed") {
                std::cout << "{not-json}\n" << std::flush;
            } else if (scenario == u"oversized") {
                std::cout << std::string(64 * 1024 + 1, 'x') << '\n' << std::flush;
            } else if (scenario == u"unexpected-exit") {
                status("Checking");
                return 9;
            } else if (scenario == u"manual-respawn") {
                const QString marker = QDir(cache).filePath(QStringLiteral("first-exit"));
                status("Checking");
                if (!QFileInfo::exists(marker)) {
                    touch(marker);
                    return 9;
                }
                status("Ready");
                frame({{QStringLiteral("protocol"), 1},
                       {QStringLiteral("type"), QStringLiteral("update_ready")}});
            } else {
                status("Checking");
                status("Ready");
                frame({{QStringLiteral("protocol"), 1},
                       {QStringLiteral("type"), QStringLiteral("update_ready")}});
            }
        } else if (name == u"begin_apply") {
            status("Applying");
            frame({{QStringLiteral("protocol"), 1},
                   {QStringLiteral("type"), QStringLiteral("handoff_ready")}});
        } else if (name == u"handoff_decision") {
            status(command.value(QStringLiteral("proceed")).toBool() ? "Applying" : "Ready");
        } else if (name == u"shutdown") {
            if (scenario == u"graceful-shutdown") {
                touch(QDir(cache).filePath(QStringLiteral("shutdown")));
            }
            return 0;
        }
    }
    return 0;
}

bool waitUntil(const std::function<bool()>& predicate, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    return predicate();
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--service") {
            return fakeSidecar(argc, argv);
        }
    }
    QCoreApplication app(argc, argv);
#ifndef Q_OS_WIN
    return 0;
#else
    QTemporaryDir directory;
    require(directory.isValid(), "create adapter test directory");
    const QString root = directory.filePath(QStringLiteral("Snow Shot"));
    require(QDir().mkpath(QDir(root).filePath(QStringLiteral("bin"))), "create fake installation");
    const QString helper = QDir(root).filePath(QStringLiteral("bin/snow-shot-updater.exe"));
    require(QFile::copy(QCoreApplication::applicationFilePath(), helper), "copy fake sidecar");

    int cacheIndex = 0;
    const auto options = [&](const QString& scenario) {
        UpdateService::Options value;
        value.applicationDirectory = QDir(root).filePath(QStringLiteral("bin"));
        value.root = root;
        value.cacheDirectory = directory.filePath(QStringLiteral("cache-%1").arg(++cacheIndex));
        value.baseUrl = QUrl(QStringLiteral("https://updates.example.test/") + scenario);
        return value;
    };

    {
        UpdateService service(options(QStringLiteral("default")));
        bool updateReady = false;
        bool handoff = false;
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { updateReady = true; });
        QObject::connect(&service, &UpdateService::handoffReady, &app, [&] {
            handoff = true;
            service.reportBlocked(QStringLiteral("blocked for adapter test"));
        });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "sidecar handshake reaches idle");
        service.check();
        require(
            waitUntil([&] { return updateReady && service.status().state == UpdateState::Ready; }),
            "status and update-ready mapping");
        service.beginApply();
        require(waitUntil([&] { return handoff && service.status().state == UpdateState::Ready; }),
                "reentrant handoff cancellation");
    }

    {
        UpdateService service(options(QStringLiteral("all-statuses")));
        QList<UpdateState> seen;
        bool progress = false;
        QObject::connect(&service, &UpdateService::statusChanged, &app, [&] {
            seen.append(service.status().state);
            progress =
                progress || (service.status().state == UpdateState::Downloading &&
                             service.status().received == 12 && service.status().total == 24);
        });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "all-statuses handshake");
        service.check();
        require(waitUntil([&] { return service.status().state == UpdateState::Failed; }),
                "all-statuses reaches failed");
        for (const auto state :
             {UpdateState::Unavailable, UpdateState::Idle, UpdateState::Checking,
              UpdateState::Available, UpdateState::Downloading, UpdateState::Verifying,
              UpdateState::Ready, UpdateState::Applying, UpdateState::Failed}) {
            require(seen.contains(state), "map every updater state");
        }
        require(progress, "map download progress");
        require(service.status().error ==
                    QCoreApplication::translate("UpdateErrors",
                                                "Could not download signed update metadata"),
                "translate sidecar error source");
    }

    for (const auto& scenario : {QStringLiteral("malformed"), QStringLiteral("oversized")}) {
        UpdateService service(options(scenario));
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "protocol-failure handshake");
        service.check();
        require(waitUntil([&] { return service.status().state == UpdateState::Failed; }),
                "terminate malformed protocol peer");
    }

    {
        UpdateService service(options(QStringLiteral("unsupported-handshake")));
        service.start();
        require(waitUntil([&] {
                    return service.status().state == UpdateState::Unavailable &&
                           !service.status().error.isEmpty();
                }),
                "unsupported handshake exposes unavailable");
        require(service.status().error ==
                    QCoreApplication::translate(
                        "UpdateErrors", "The update service protocol version is unsupported"),
                "unsupported protocol translation");
    }

    {
        UpdateService service(options(QStringLiteral("unexpected-exit")));
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "unexpected-exit handshake");
        service.check();
        require(waitUntil([&] { return service.status().state == UpdateState::Failed; }),
                "unexpected active exit exposes failed");
    }

    {
        UpdateService service(options(QStringLiteral("manual-respawn")));
        bool ready = false;
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { ready = true; });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "manual-respawn handshake");
        service.check();
        require(waitUntil([&] { return service.status().state == UpdateState::Failed; }),
                "manual-respawn first exit");
        service.check();
        require(waitUntil([&] { return ready && service.status().state == UpdateState::Ready; }),
                "one-shot manual respawn succeeds");
    }

    QString shutdownMarker;
    {
        auto shutdownOptions = options(QStringLiteral("graceful-shutdown"));
        shutdownMarker = QDir(shutdownOptions.cacheDirectory).filePath(QStringLiteral("shutdown"));
        UpdateService service(std::move(shutdownOptions));
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "graceful-shutdown handshake");
    }
    require(QFileInfo::exists(shutdownMarker), "adapter sends graceful shutdown command");
    return 0;
#endif
}
