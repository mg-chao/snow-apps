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
    frame({{QStringLiteral("protocol"), 2},
           {QStringLiteral("type"), QStringLiteral("status")},
           {QStringLiteral("status"), value}});
}

void complete(const QString& operation, const char* outcome, const char* state) {
    QJsonObject finalStatus{{QStringLiteral("state"), QString::fromLatin1(state)},
                            {QStringLiteral("version"), QStringLiteral("2.0.0")},
                            {QStringLiteral("received"), 0},
                            {QStringLiteral("total"), 0}};
    if (QByteArray(outcome) == "failed") {
        finalStatus.insert(
            QStringLiteral("error"),
            QJsonObject{{QStringLiteral("code"), QStringLiteral("metadata_download_failed")},
                        {QStringLiteral("message"),
                         QStringLiteral("Could not download signed update metadata")},
                        {QStringLiteral("detail"), QStringLiteral("test detail")}});
    }
    frame({{QStringLiteral("protocol"), 2},
           {QStringLiteral("type"), QStringLiteral("operation_complete")},
           {QStringLiteral("operation"), operation},
           {QStringLiteral("outcome"), QString::fromLatin1(outcome)},
           {QStringLiteral("status"), finalStatus}});
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

int incrementCounter(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    int value = 0;
    if (file.open(QIODevice::ReadOnly)) {
        value = file.readAll().trimmed().toInt();
        file.close();
    }
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write fake-sidecar counter");
    file.write(QByteArray::number(++value));
    return value;
}

int fakeSidecar(int argc, char** argv) {
    const QString scenario = QUrl(argument(argc, argv, "--base-url")).path().mid(1);
    const QString cache = argument(argc, argv, "--cache");
    incrementCounter(QDir(cache).filePath(QStringLiteral("launch-count")));
    if (scenario == u"unsupported-handshake") {
        frame({{QStringLiteral("protocol"), 1},
               {QStringLiteral("type"), QStringLiteral("hello")},
               {QStringLiteral("updaterVersion"), QStringLiteral("test")}});
        return 0;
    }
    frame({{QStringLiteral("protocol"), 2},
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
        frame({{QStringLiteral("protocol"), 2},
               {QStringLiteral("type"), QStringLiteral("command_result")},
               {QStringLiteral("id"), id},
               {QStringLiteral("ok"), true}});
        if (name == u"execute") {
            const QString operation = command.value(QStringLiteral("operation")).toString();
            const QString trigger = command.value(QStringLiteral("trigger")).toString();
            const QString operationCounter =
                QDir(cache).filePath(operation + u'-' + trigger + QStringLiteral("-count"));
            const QString completionCounter =
                QDir(cache).filePath(operation + u'-' + trigger + QStringLiteral("-complete"));
            incrementCounter(operationCounter);
            if (operation == u"probe") {
                const char* probeState = scenario == u"available" ? "Available" : "Idle";
                status(probeState);
                complete(operation, "success", probeState);
                incrementCounter(completionCounter);
                return 0;
            }
            if (operation == u"apply") {
                status("Applying");
                frame({{QStringLiteral("protocol"), 2},
                       {QStringLiteral("type"), QStringLiteral("handoff_ready")}});
                continue;
            }
            if (operation == u"download") {
                status("Downloading", 12, 24);
                status("Ready", 24, 24);
                frame({{QStringLiteral("protocol"), 2},
                       {QStringLiteral("type"), QStringLiteral("update_ready")}});
                complete(operation, "success", "Ready");
                incrementCounter(completionCounter);
                return 0;
            }
            if ((scenario == u"manual-scheduling" || scenario == u"manual-failure") &&
                trigger == u"user") {
                status("Checking");
                QThread::msleep(80);
                const bool failed = scenario == u"manual-failure";
                status(failed ? "Failed" : "Idle");
                complete(operation, failed ? "failed" : "success", failed ? "Failed" : "Idle");
                incrementCounter(completionCounter);
                return 0;
            }
            if (scenario == u"manual-scheduling" || scenario == u"manual-failure") {
                status("Checking");
                status("Idle");
                complete(operation, "success", "Idle");
                incrementCounter(completionCounter);
                return 0;
            }
            if (scenario == u"scheduled") {
                status("Checking");
                status("Idle");
                complete(operation, "success", "Idle");
                incrementCounter(completionCounter);
                return 0;
            }
            if (scenario == u"scheduled-failure") {
                status("Checking");
                status("Failed");
                complete(operation, "failed", "Failed");
                incrementCounter(completionCounter);
                return 0;
            }
            if (scenario == u"adapter-download-policy") {
                status("Checking");
                QThread::msleep(80);
                status("Available");
                complete(operation, "success", "Available");
                incrementCounter(completionCounter);
                return 0;
            }
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
                complete(operation, "failed", "Failed");
                incrementCounter(completionCounter);
                return 0;
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
                frame({{QStringLiteral("protocol"), 2},
                       {QStringLiteral("type"), QStringLiteral("update_ready")}});
                complete(operation, "success", "Ready");
                incrementCounter(completionCounter);
                return 0;
            } else {
                status("Checking");
                status("Ready");
                frame({{QStringLiteral("protocol"), 2},
                       {QStringLiteral("type"), QStringLiteral("update_ready")}});
                complete(operation, "success", "Ready");
                incrementCounter(completionCounter);
                return 0;
            }
        } else if (name == u"handoff_decision") {
            status(command.value(QStringLiteral("proceed")).toBool() ? "Applying" : "Ready");
            if (!command.value(QStringLiteral("proceed")).toBool()) {
                complete(QStringLiteral("apply"), "cancelled", "Ready");
            }
            return 0;
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

int counterValue(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toInt() : 0;
}

void processFor(int duration) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < duration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
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

    {
        auto scheduledOptions = options(QStringLiteral("scheduled"));
        scheduledOptions.startupCheckDelay = std::chrono::milliseconds(25);
        scheduledOptions.automaticCheckInterval = std::chrono::milliseconds(40);
        const QString launches =
            QDir(scheduledOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(scheduledOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        require(waitUntil([&] { return counterValue(launches) >= 3; }, 3000),
                "probe, startup check, and interval each launch a short-lived sidecar");
    }

    {
        auto manualOptions = options(QStringLiteral("scheduled"));
        manualOptions.startupCheckDelay = std::chrono::milliseconds(20);
        manualOptions.automaticCheckInterval = std::chrono::milliseconds(30);
        const QString launches =
            QDir(manualOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(manualOptions));
        service.setMode(QStringLiteral("manual"));
        service.start();
        require(waitUntil([&] { return counterValue(launches) == 1; }),
                "manual mode launches the startup probe");
        processFor(120);
        require(counterValue(launches) == 1,
                "manual mode leaves no resident or scheduled updater process");
    }

    {
        auto failureOptions = options(QStringLiteral("scheduled-failure"));
        failureOptions.startupCheckDelay = std::chrono::milliseconds(25);
        failureOptions.automaticCheckInterval = std::chrono::milliseconds(40);
        const QString launches =
            QDir(failureOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(failureOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        require(waitUntil([&] { return counterValue(launches) >= 3; }, 3000),
                "automatic failures schedule only the next fixed interval");
    }

    {
        auto policyOptions = options(QStringLiteral("adapter-download-policy"));
        const QString cache = policyOptions.cacheDirectory;
        const QString check = QDir(cache).filePath(QStringLiteral("check-periodic-complete"));
        const QString download =
            QDir(cache).filePath(QStringLiteral("download-policyChange-count"));
        UpdateService service(std::move(policyOptions));
        QStringList announced;
        int readyCount = 0;
        QObject::connect(&service, &UpdateService::automaticUpdateAvailable, &app,
                         [&](const QString& version) { announced.append(version); });
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { ++readyCount; });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "download-policy probe");
        service.check(false);
        require(waitUntil([&] { return service.status().state == UpdateState::Checking; }),
                "download-policy check starts");
        require(waitUntil([&] {
                    return counterValue(check) == 1 && counterValue(download) == 1 &&
                           service.status().state == UpdateState::Ready;
                }),
                "adapter schedules a distinct download from current policy");
        require(announced.isEmpty() && readyCount == 1,
                "automatic download keeps its update-ready notification without an early notice");
    }

    {
        auto checkOptions = options(QStringLiteral("adapter-download-policy"));
        const QString cache = checkOptions.cacheDirectory;
        checkOptions.startupCheckDelay = std::chrono::hours(1);
        checkOptions.automaticCheckInterval = std::chrono::hours(1);
        const QString automaticChecks =
            QDir(cache).filePath(QStringLiteral("check-periodic-complete"));
        const QString manualChecks = QDir(cache).filePath(QStringLiteral("check-user-complete"));
        const QString downloads =
            QDir(cache).filePath(QStringLiteral("download-policyChange-count"));
        UpdateService service(std::move(checkOptions));
        service.setMode(QStringLiteral("check"));
        QStringList announced;
        QObject::connect(&service, &UpdateService::automaticUpdateAvailable, &app,
                         [&](const QString& version) { announced.append(version); });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "notification-only probe completes");
        service.check(false);
        require(waitUntil([&] {
                    return counterValue(automaticChecks) == 1 &&
                           service.status().state == UpdateState::Available;
                }),
                "notification-only check finds an update");
        require(announced == QStringList{QStringLiteral("2.0.0")} && counterValue(downloads) == 0,
                "check mode announces the available version without downloading");
        service.check(false);
        require(waitUntil([&] { return counterValue(automaticChecks) == 2; }),
                "repeat automatic check completes");
        service.check(true);
        require(waitUntil([&] { return counterValue(manualChecks) == 1; }),
                "manual check completes");
        require(announced.size() == 1,
                "repeat and manual checks do not duplicate the system notification");
    }

    {
        auto policyOptions = options(QStringLiteral("adapter-download-policy"));
        const QString cache = policyOptions.cacheDirectory;
        const QString check = QDir(cache).filePath(QStringLiteral("check-periodic-complete"));
        const QString download =
            QDir(cache).filePath(QStringLiteral("download-policyChange-count"));
        UpdateService service(std::move(policyOptions));
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "changed-policy probe");
        service.check(false);
        require(waitUntil([&] { return service.status().state == UpdateState::Checking; }),
                "changed-policy check starts");
        service.setMode(QStringLiteral("check"));
        require(waitUntil([&] {
                    return counterValue(check) == 1 &&
                           service.status().state == UpdateState::Available;
                }),
                "changed policy keeps metadata-only result available");
        processFor(100);
        require(counterValue(download) == 0,
                "leaving download mode prevents a queued payload operation");
    }

    {
        auto manualCheckOptions = options(QStringLiteral("manual-scheduling"));
        manualCheckOptions.startupCheckDelay = std::chrono::milliseconds(25);
        manualCheckOptions.automaticCheckInterval = std::chrono::milliseconds(100);
        const QString cache = manualCheckOptions.cacheDirectory;
        const QString userComplete = QDir(cache).filePath(QStringLiteral("check-user-complete"));
        const QString periodic = QDir(cache).filePath(QStringLiteral("check-periodic-count"));
        UpdateService service(std::move(manualCheckOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        service.check(true);
        require(waitUntil([&] { return counterValue(userComplete) == 1; }),
                "successful user check completes while the startup timer is due");
        processFor(40);
        require(counterValue(periodic) == 0,
                "successful user check removes a queued automatic check and resets its timer");
        require(waitUntil([&] { return counterValue(periodic) == 1; }),
                "successful user check schedules the next automatic interval");
    }

    {
        auto manualCheckOptions = options(QStringLiteral("manual-failure"));
        manualCheckOptions.startupCheckDelay = std::chrono::milliseconds(25);
        manualCheckOptions.automaticCheckInterval = std::chrono::milliseconds(100);
        const QString cache = manualCheckOptions.cacheDirectory;
        const QString userComplete = QDir(cache).filePath(QStringLiteral("check-user-complete"));
        const QString periodic = QDir(cache).filePath(QStringLiteral("check-periodic-count"));
        UpdateService service(std::move(manualCheckOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        service.check(true);
        require(waitUntil([&] { return counterValue(userComplete) == 1; }),
                "failed user check completes while the startup timer is due");
        require(waitUntil([&] { return counterValue(periodic) >= 1; }, 500),
                "failed user check does not postpone an already queued automatic check");
    }

    {
        auto availableOptions = options(QStringLiteral("available"));
        const QString launches =
            QDir(availableOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(availableOptions));
        service.setMode(QStringLiteral("manual"));
        QStringList announced;
        int readyCount = 0;
        QObject::connect(&service, &UpdateService::automaticUpdateAvailable, &app,
                         [&](const QString& version) { announced.append(version); });
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { ++readyCount; });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Available; }),
                "probe restores an available update");
        require(announced.isEmpty(), "manual mode keeps cached discovery in About");
        service.setMode(QStringLiteral("download"));
        require(waitUntil([&] {
                    return counterValue(launches) >= 2 &&
                           service.status().state == UpdateState::Ready;
                }),
                "enabling automatic download launches an immediate short-lived download");
        require(announced.isEmpty() && readyCount == 1,
                "switching to automatic download preserves the ready notification");
    }

    {
        auto shutdownOptions = options(QStringLiteral("graceful-shutdown"));
        UpdateService service(std::move(shutdownOptions));
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "operation-scoped probe handshake");
    }
    return 0;
#endif
}
