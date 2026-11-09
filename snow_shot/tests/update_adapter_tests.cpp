#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
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

void writeJson(const QString& path, const QJsonObject& value) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write fake-sidecar command");
    file.write(QJsonDocument(value).toJson(QJsonDocument::Compact));
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject{};
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

void waitForRelease(const QString& cache, const QString& operation) {
    touch(QDir(cache).filePath(operation + QStringLiteral("-blocked")));
    const QString release = QDir(cache).filePath(operation + QStringLiteral("-release"));
    QElapsedTimer deadline;
    deadline.start();
    while (!QFileInfo::exists(release) && deadline.elapsed() < 10000)
        QThread::msleep(1);
    require(QFileInfo::exists(release), "parent releases the blocked sidecar operation");
}

void releaseSidecar(const QString& cache, const QString& operation) {
    touch(QDir(cache).filePath(operation + QStringLiteral("-release")));
}

int fakeSidecar(int argc, char** argv) {
    const QString scenario = QUrl(argument(argc, argv, "--github-api-url")).path().mid(1);
    require(!argument(argc, argv, "--gitee-api-url").isEmpty(), "Gitee endpoint supplied");
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
        bool rejectedHandoff = false;
        if (name == u"handoff_decision") {
            writeJson(QDir(cache).filePath(QStringLiteral("handoff.json")), command);
            if (scenario == u"delayed-handoff")
                waitForRelease(cache, QStringLiteral("handoff"));
            if (scenario == u"retry-shell-handoff")
                rejectedHandoff =
                    incrementCounter(QDir(cache).filePath(QStringLiteral("handoff-count"))) == 1;
        }
        frame({{QStringLiteral("protocol"), 2},
               {QStringLiteral("type"), QStringLiteral("command_result")},
               {QStringLiteral("id"), id},
               {QStringLiteral("ok"), !rejectedHandoff}});
        if (rejectedHandoff)
            return 0;
        if (name == u"execute") {
            writeJson(QDir(cache).filePath(QStringLiteral("execute.json")), command);
            const QString operation = command.value(QStringLiteral("operation")).toString();
            const QString trigger = command.value(QStringLiteral("trigger")).toString();
            const QString operationCounter =
                QDir(cache).filePath(operation + u'-' + trigger + QStringLiteral("-count"));
            const QString completionCounter =
                QDir(cache).filePath(operation + u'-' + trigger + QStringLiteral("-complete"));
            const int operationCount = incrementCounter(operationCounter);
            if (operation == u"probe") {
                const char* probeState = scenario == u"available" || scenario == u"slow-download" ||
                                                 scenario == u"policy-resume"
                                             ? "Available"
                                         : scenario == u"ready" ? "Ready"
                                                                : "Idle";
                status(probeState);
                complete(operation, "success", probeState);
                incrementCounter(completionCounter);
                return 0;
            }
            if (operation == u"apply") {
                if (scenario == u"apply-newer-release") {
                    status("Checking");
                    status("Available");
                    complete(operation, "success", "Available");
                    incrementCounter(completionCounter);
                    return 0;
                }
                status("Applying");
                frame({{QStringLiteral("protocol"), 2},
                       {QStringLiteral("type"), QStringLiteral("handoff_ready")}});
                continue;
            }
            if (operation == u"download") {
                status("Downloading", 12, 24);
                if (scenario == u"slow-download" ||
                    (scenario == u"policy-resume" && operationCount == 1))
                    continue;
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
                waitForRelease(cache, QStringLiteral("manual-check"));
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
            if (scenario == u"adapter-download-policy" || scenario == u"gated-download-policy") {
                status("Checking");
                if (scenario == u"gated-download-policy")
                    waitForRelease(cache, QStringLiteral("check"));
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
        } else if (name == u"configure") {
            incrementCounter(QDir(cache).filePath(QStringLiteral("configure-count")));
            writeJson(QDir(cache).filePath(QStringLiteral("configure.json")), command);
        } else if (name == u"cancel") {
            incrementCounter(QDir(cache).filePath(QStringLiteral("cancel-count")));
            if (scenario == u"policy-resume")
                waitForRelease(cache, QStringLiteral("cancel"));
            complete(QStringLiteral("download"), "cancelled", "Available");
            return 0;
        } else if (name == u"handoff_decision") {
            status(command.value(QStringLiteral("proceed")).toBool() ? "Applying" : "Ready");
            if (!command.value(QStringLiteral("proceed")).toBool()) {
                complete(QStringLiteral("apply"), "cancelled", "Ready");
                return 0;
            }
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

QTimer* scheduledTimer(UpdateService& service, std::chrono::milliseconds interval) {
    QTimer* scheduled = nullptr;
    for (auto* timer : service.findChildren<QTimer*>()) {
        if (timer->isActive() && timer->isSingleShot() && timer->intervalAsDuration() == interval) {
            require(scheduled == nullptr, "exactly one updater timer owns the expected deadline");
            scheduled = timer;
        }
    }
    require(scheduled != nullptr, "the automatic update deadline must be armed");
    return scheduled;
}

void expireScheduledTimer(UpdateService& service, std::chrono::milliseconds interval) {
    auto* timer = scheduledTimer(service, interval);
    // Exercise Qt's normal single-shot expiry and the service's connected scheduling logic.
    // Long real intervals keep machine load from advancing the next phase of this fixture.
    QTimerEvent expiration(timer->timerId());
    QCoreApplication::sendEvent(timer, &expiration);
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
    qputenv("PATH", QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toLocal8Bit() +
                        ';' + qgetenv("PATH"));
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
        value.githubApiUrl = QUrl(QStringLiteral("https://updates.example.test/") + scenario);
        value.giteeApiUrl = QUrl(QStringLiteral("https://updates.example.test/") + scenario);
        value.storeManaged = false;
        value.startupCheckDelay = std::chrono::hours(1);
        value.automaticCheckInterval = std::chrono::hours(2);
        return value;
    };

    {
        auto storeOptions = options(QStringLiteral("store-managed"));
        const QString cache = storeOptions.cacheDirectory;
        storeOptions.storeManaged = true;
        storeOptions.startupCheckDelay = std::chrono::milliseconds(1);
        UpdateService service(std::move(storeOptions));
        int completions = 0;
        bool restart = false;
        bool handoff = false;
        QObject::connect(&service, &UpdateService::operationFinished, &app,
                         [&](const QString&, const QString& outcome) {
                             require(outcome == u"unsupported", "Store rejects native operations");
                             ++completions;
                         });
        QObject::connect(&service, &UpdateService::restartRequested, &app, [&] { restart = true; });
        service.setHandoffApproval(&app, [&] {
            handoff = true;
            return true;
        });
        service.setMode(QStringLiteral("next_launch"));
        service.start();
        service.check();
        service.download();
        service.beginApply();
        service.applyAtStartup();
        service.requestRestart();
        service.cancel();
        QCoreApplication::processEvents();
        const auto capabilities = service.capabilities();
        require(!capabilities.check && !capabilities.download && !capabilities.install,
                "Store policy exposes no native update capabilities");
        require(service.managedByStore() && !service.busy() && !service.handoffPending() &&
                    service.status().state == UpdateState::Unavailable && completions == 4 &&
                    !restart && !handoff && !QFileInfo::exists(cache),
                "Store policy never starts the available helper or its background schedule");
    }

    {
        UpdateService service(options(QStringLiteral("default")));
        bool updateReady = false;
        bool handoff = false;
        QList<QPair<QString, QString>> operations;
        QObject::connect(&service, &UpdateService::operationFinished, &app,
                         [&](const QString& operation, const QString& outcome) {
                             operations.append({operation, outcome});
                         });
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { updateReady = true; });
        const auto capabilities = service.capabilities();
        require(capabilities.check && capabilities.download && capabilities.install,
                "standalone Windows owns discovery, download, and installation");
        service.setHandoffApproval(&app, [&] {
            handoff = true;
            require(!service.handoffPending(), "approval precedes application suspension");
            service.reportBlocked(QStringLiteral("blocked for adapter test"));
            return false;
        });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "sidecar handshake reaches idle");
        service.check();
        require(service.busy(), "accepted update work owns the updater lifecycle immediately");
        require(
            waitUntil([&] { return updateReady && service.status().state == UpdateState::Ready; }),
            "status and update-ready mapping");
        require(operations.contains({QStringLiteral("check"), QStringLiteral("success")}),
                "service completion reports exact operation and outcome without polling");
        service.beginApply();
        require(waitUntil([&] { return handoff && service.status().state == UpdateState::Ready; }),
                "direct handoff refusal preserves the running application");
        require(!service.handoffPending(), "refused handoff never suspends application input");
    }

    for (int refusal = 0; refusal < 5; ++refusal) {
        auto refusedOptions = options(QStringLiteral("default"));
        const QString cache = refusedOptions.cacheDirectory;
        UpdateService service(std::move(refusedOptions));
        int approvals = 0;
        int suspensions = 0;
        bool committed = false;
        auto context = std::make_unique<QObject>();
        service.setHandoffApproval(context.get(), [&]() -> bool {
            ++approvals;
            require(QThread::currentThread() == service.thread() && !service.handoffPending(),
                    "handoff authority executes on owner thread before suspension");
            if (refusal == 1)
                throw std::runtime_error("approval failure");
            if (refusal == 4) {
                context.reset();
                return true;
            }
            return false;
        });
        if (refusal == 2)
            context.reset();
        else if (refusal == 3)
            service.setHandoffApproval(nullptr, {});
        QObject::connect(&service, &UpdateService::handoffPendingChanged, &app,
                         [&](bool pending) { suspensions += pending ? 1 : 0; });
        QObject::connect(&service, &UpdateService::handoffCommitted, &app,
                         [&] { committed = true; });
        service.applyAtStartup();
        require(waitUntil([&] {
                    return !service.busy() &&
                           QFileInfo::exists(QDir(cache).filePath(QStringLiteral("handoff.json")));
                }),
                "refused or unavailable authority completes the helper rollback");
        require(approvals == (refusal == 2 || refusal == 3 ? 0 : 1) && suspensions == 0 &&
                    !committed && service.status().state == UpdateState::Ready &&
                    !readJson(QDir(cache).filePath(QStringLiteral("handoff.json")))
                         .value(QStringLiteral("proceed"))
                         .toBool(true),
                "refusal, exception, expired/missing or self-expiring authority never commit or "
                "suspend");
    }

    {
        auto refusedOptions = options(QStringLiteral("default"));
        const QString cache = refusedOptions.cacheDirectory;
        auto service = std::make_unique<UpdateService>(std::move(refusedOptions));
        const QPointer<UpdateService> lifetime(service.get());
        bool validated = false;
        bool committed = false;
        service->setHandoffApproval(&app, [] { return true; });
        service->setHandoffValidation(&app, [&] {
            validated = true;
            return false;
        });
        QObject::connect(service.get(), &UpdateService::handoffCommitted, &app,
                         [&] { committed = true; });
        QObject::connect(service.get(), &UpdateService::statusChanged, &app, [&] {
            if (service && service->handoffPending() &&
                service->status().state == UpdateState::Ready)
                service.reset();
        });
        service->applyAtStartup();
        require(waitUntil([&] { return lifetime.isNull(); }) && validated && !committed,
                "a refusal observer can destroy the service without a stale protocol callback");
        require(!QFileInfo::exists(QDir(cache).filePath(QStringLiteral("handoff.json"))),
                "destruction during refusal cannot send a later helper decision");
    }

    for (int refusal = 0; refusal != 7; ++refusal) {
        auto refusedOptions = options(QStringLiteral("default"));
        const QString cache = refusedOptions.cacheDirectory;
        UpdateService service(std::move(refusedOptions));
        auto context = std::make_unique<QObject>();
        bool newlyAdmittedWork = false;
        int validations = 0;
        int suspensions = 0;
        int restorations = 0;
        bool committed = false;
        service.setHandoffApproval(&app, [&] {
            require(!newlyAdmittedWork && !service.handoffPending(),
                    "initial approval sees no work before suspension observers run");
            return true;
        });
        service.setHandoffValidation(context.get(), [&]() -> bool {
            ++validations;
            require(service.handoffPending(), "final validation runs with admission closed");
            if (refusal == 1)
                throw std::runtime_error("final validation failure");
            if (refusal == 5) {
                context.reset();
                return true;
            }
            if (refusal == 6) {
                QSaveFile checkpoint(QDir(cache).filePath(QStringLiteral("missing/checkpoint")));
                require(!checkpoint.open(QIODevice::WriteOnly),
                        "isolated final checkpoint has a deterministic storage failure");
                return false;
            }
            return !newlyAdmittedWork;
        });
        if (refusal == 3)
            service.setHandoffValidation(nullptr, {});
        QObject::connect(&service, &UpdateService::handoffPendingChanged, &app, [&](bool pending) {
            if (pending) {
                ++suspensions;
                newlyAdmittedWork = true;
                if (refusal == 2)
                    context.reset();
                if (refusal == 4)
                    throw std::runtime_error("suspension observer failure");
            } else {
                ++restorations;
            }
        });
        QObject::connect(&service, &UpdateService::handoffCommitted, &app,
                         [&] { committed = true; });
        service.applyAtStartup();
        require(waitUntil([&] {
                    return !service.busy() &&
                           QFileInfo::exists(QDir(cache).filePath(QStringLiteral("handoff.json")));
                }),
                "failed final authority completes the helper rollback");
        require(suspensions == 1 && restorations == 1 && !service.handoffPending() && !committed &&
                    validations == (refusal < 2 || refusal >= 5 ? 1 : 0) &&
                    service.status().state == UpdateState::Ready &&
                    !readJson(QDir(cache).filePath(QStringLiteral("handoff.json")))
                         .value(QStringLiteral("proceed"))
                         .toBool(true),
                "late work, authority/pause failure and checkpoint write failure restore once");
    }

    {
        UpdateService service(options(QStringLiteral("all-statuses")));
        // This case tests status mapping. Automatic download from its intermediate Available
        // frame would legitimately replace the final failure with a later successful operation.
        service.setMode(QStringLiteral("manual"));
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
        int failedChecks = 0;
        QObject::connect(&service, &UpdateService::operationFinished, &service,
                         [&](const QString& operation, const QString& outcome) {
                             if (operation == u"check" && outcome == u"failed")
                                 ++failedChecks;
                         });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "protocol-failure handshake");
        service.check();
        require(waitUntil([&] { return service.status().state == UpdateState::Failed; }),
                "terminate malformed protocol peer");
        require(waitUntil([&] { return !service.busy(); }) && failedChecks == 1,
                "protocol failure completes the owned operation exactly once");
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
        int failedChecks = 0;
        QObject::connect(&service, &UpdateService::operationFinished, &service,
                         [&](const QString& operation, const QString& outcome) {
                             if (operation == u"check" && outcome == u"failed")
                                 ++failedChecks;
                         });
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "unexpected-exit handshake");
        service.check();
        require(waitUntil([&] { return service.status().state == UpdateState::Failed; }),
                "unexpected active exit exposes failed");
        require(failedChecks == 1, "unexpected exit completes the owned operation exactly once");
    }

    {
        auto missingOptions = options(QStringLiteral("missing-helper"));
        missingOptions.applicationDirectory = directory.filePath(QStringLiteral("missing"));
        UpdateService service(std::move(missingOptions));
        int failedChecks = 0;
        QObject::connect(&service, &UpdateService::operationFinished, &service,
                         [&](const QString& operation, const QString& outcome) {
                             if (operation == u"check" && outcome == u"failed")
                                 ++failedChecks;
                         });
        service.check();
        require(waitUntil([&] { return failedChecks == 1 && !service.busy(); }),
                "failed helper launch completes the owned operation");
        service.check();
        require(waitUntil([&] { return failedChecks == 2 && !service.busy(); }),
                "retry failure receives its own completion");
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
        const QString launches =
            QDir(scheduledOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(scheduledOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        require(waitUntil([&] { return counterValue(launches) == 1 && !service.busy(); }),
                "the startup probe finishes before its automatic check is due");
        expireScheduledTimer(service, std::chrono::hours(1));
        require(waitUntil([&] { return counterValue(launches) == 2 && !service.busy(); }),
                "the startup deadline launches a short-lived check");
        expireScheduledTimer(service, std::chrono::hours(2));
        require(waitUntil([&] { return counterValue(launches) == 3 && !service.busy(); }),
                "probe, startup check, and interval each launch a short-lived sidecar");
        scheduledTimer(service, std::chrono::hours(2));
    }

    {
        auto manualOptions = options(QStringLiteral("scheduled"));
        const QString launches =
            QDir(manualOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(manualOptions));
        service.setMode(QStringLiteral("manual"));
        service.start();
        require(waitUntil([&] { return counterValue(launches) == 1 && !service.busy(); }),
                "manual mode launches the startup probe");
        for (auto* timer : service.findChildren<QTimer*>())
            require(!timer->isActive(),
                    "manual mode leaves no resident or scheduled updater process");
    }

    {
        auto failureOptions = options(QStringLiteral("scheduled-failure"));
        const QString launches =
            QDir(failureOptions.cacheDirectory).filePath(QStringLiteral("launch-count"));
        UpdateService service(std::move(failureOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        require(waitUntil([&] { return counterValue(launches) == 1 && !service.busy(); }),
                "the failure fixture finishes its startup probe");
        expireScheduledTimer(service, std::chrono::hours(1));
        require(waitUntil([&] {
                    return counterValue(launches) == 2 && !service.busy() &&
                           service.status().state == UpdateState::Failed;
                }),
                "the first automatic failure finishes before the next interval");
        expireScheduledTimer(service, std::chrono::hours(2));
        require(waitUntil([&] {
                    return counterValue(launches) == 3 && !service.busy() &&
                           service.status().state == UpdateState::Failed;
                }),
                "automatic failures schedule only the next fixed interval");
        scheduledTimer(service, std::chrono::hours(2));
    }

    {
        auto policyOptions = options(QStringLiteral("gated-download-policy"));
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
        releaseSidecar(cache, QStringLiteral("check"));
        require(waitUntil([&] {
                    return counterValue(check) == 1 && counterValue(download) == 1 &&
                           service.status().state == UpdateState::Ready;
                }),
                "adapter schedules a distinct download from current policy");
        require(announced.isEmpty() && readyCount == 1,
                "automatic download keeps its update-ready notification without an early notice");
    }

    {
        auto applyOptions = options(QStringLiteral("apply-newer-release"));
        const QString cache = applyOptions.cacheDirectory;
        const QString check = QDir(cache).filePath(QStringLiteral("check-user-complete"));
        const QString apply = QDir(cache).filePath(QStringLiteral("apply-user-complete"));
        const QString download =
            QDir(cache).filePath(QStringLiteral("download-policyChange-count"));
        UpdateService service(std::move(applyOptions));
        service.start();
        require(waitUntil([&] { return service.status().state == UpdateState::Idle; }),
                "newer-release probe completes");
        service.check();
        require(waitUntil([&] {
                    return counterValue(check) == 1 && service.status().state == UpdateState::Ready;
                }),
                "the first check makes the cached release ready");
        service.beginApply();
        require(waitUntil([&] {
                    return counterValue(apply) == 1 && counterValue(download) == 1 &&
                           service.status().state == UpdateState::Ready;
                }),
                "a newer release found before apply is downloaded automatically");
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
        auto policyOptions = options(QStringLiteral("gated-download-policy"));
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
        releaseSidecar(cache, QStringLiteral("check"));
        require(waitUntil([&] {
                    return counterValue(check) == 1 && !service.busy() &&
                           service.status().state == UpdateState::Available;
                }),
                "changed policy keeps metadata-only result available");
        require(counterValue(download) == 0,
                "leaving download mode prevents a queued payload operation");
    }

    {
        auto manualCheckOptions = options(QStringLiteral("manual-scheduling"));
        const QString cache = manualCheckOptions.cacheDirectory;
        const QString userComplete = QDir(cache).filePath(QStringLiteral("check-user-complete"));
        const QString periodic = QDir(cache).filePath(QStringLiteral("check-periodic-count"));
        UpdateService service(std::move(manualCheckOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        service.check(true);
        require(waitUntil([&] { return service.status().state == UpdateState::Checking; }),
                "user check remains in progress until its automatic deadline is delivered");
        expireScheduledTimer(service, std::chrono::hours(1));
        releaseSidecar(cache, QStringLiteral("manual-check"));
        require(waitUntil([&] { return counterValue(userComplete) == 1 && !service.busy(); }),
                "successful user check completes while the startup timer is due");
        require(counterValue(periodic) == 0,
                "successful user check removes a queued automatic check and resets its timer");
        expireScheduledTimer(service, std::chrono::hours(2));
        require(waitUntil([&] { return counterValue(periodic) == 1 && !service.busy(); }),
                "successful user check schedules the next automatic interval");
    }

    {
        auto manualCheckOptions = options(QStringLiteral("manual-failure"));
        const QString cache = manualCheckOptions.cacheDirectory;
        const QString userComplete = QDir(cache).filePath(QStringLiteral("check-user-complete"));
        const QString periodic = QDir(cache).filePath(QStringLiteral("check-periodic-count"));
        UpdateService service(std::move(manualCheckOptions));
        service.setMode(QStringLiteral("check"));
        service.start();
        service.check(true);
        require(waitUntil([&] { return service.status().state == UpdateState::Checking; }),
                "failing user check remains in progress until the automatic deadline is due");
        expireScheduledTimer(service, std::chrono::hours(1));
        releaseSidecar(cache, QStringLiteral("manual-check"));
        require(waitUntil([&] { return counterValue(userComplete) == 1; }),
                "failed user check completes while the startup timer is due");
        require(waitUntil([&] { return counterValue(periodic) == 1 && !service.busy(); }),
                "failed user check does not postpone an already queued automatic check");
        scheduledTimer(service, std::chrono::hours(2));
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
        auto quietOptions = options(QStringLiteral("adapter-download-policy"));
        const QString cache = quietOptions.cacheDirectory;
        UpdateService service(std::move(quietOptions));
        int availableCount = 0;
        int readyCount = 0;
        QObject::connect(&service, &UpdateService::automaticUpdateAvailable, &app,
                         [&](const QString&) { ++availableCount; });
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { ++readyCount; });
        service.setMode(QStringLiteral("next_launch"));
        service.start();
        require(waitUntil([&] { return !service.busy(); }), "quiet startup probe completes");
        service.check(false);
        require(waitUntil([&] {
                    return service.status().state == UpdateState::Ready && !service.busy();
                }),
                "next-launch policy downloads updates automatically");
        require(availableCount == 0 && readyCount == 0,
                "next-launch updates suppress both available and ready notifications");
        require(counterValue(QDir(cache).filePath(QStringLiteral("apply-startup-count"))) == 0 &&
                    counterValue(QDir(cache).filePath(QStringLiteral("apply-user-count"))) == 0,
                "newly downloaded updates are not applied in the running session");
    }

    {
        UpdateService service(options(QStringLiteral("ready")));
        int readyCount = 0;
        QObject::connect(&service, &UpdateService::updateReady, &app, [&] { ++readyCount; });
        service.setMode(QStringLiteral("next_launch"));
        service.start();
        require(waitUntil([&] { return !service.busy(); }), "quiet cached probe completes");
        require(service.status().state == UpdateState::Ready && readyCount == 0,
                "cached-ready completion also suppresses its notification");
    }

    {
        auto slowOptions = options(QStringLiteral("slow-download"));
        const QString cache = slowOptions.cacheDirectory;
        UpdateService service(std::move(slowOptions));
        const QJsonObject initialAppearance{
            {QStringLiteral("background"), 0xffffff},
            {QStringLiteral("primary"), 0x1677ff},
            {QStringLiteral("fontFamily"), QStringLiteral("Segoe UI")},
            {QStringLiteral("fontSize"), 14}};
        service.setProgressAppearance(initialAppearance);
        service.setProxyConfiguration(QStringLiteral("manual"),
                                      QStringLiteral("http://127.0.0.1:7890"));
        service.setMode(QStringLiteral("manual"));
        service.start();
        require(waitUntil([&] { return !service.busy(); }), "slow-download probe completes");
        const auto initialCommand = readJson(QDir(cache).filePath(QStringLiteral("execute.json")));
        require(initialCommand.value(QStringLiteral("progressAppearance")).toObject() ==
                    initialAppearance,
                "initial appearance accompanies the first operation");
        const auto proxyCommand = readJson(QDir(cache).filePath(QStringLiteral("execute.json")));
        require(!proxyCommand.value(QStringLiteral("systemProxy")).toBool() &&
                    proxyCommand.value(QStringLiteral("httpProxy")).toString() ==
                        QStringLiteral("http://127.0.0.1:7890"),
                "manual proxy is passed to the updater helper");
        service.setMode(QStringLiteral("download"));
        require(waitUntil([&] { return service.status().state == UpdateState::Downloading; }),
                "slow payload download begins");
        service.setMode(QStringLiteral("next_launch"));
        service.setMode(QStringLiteral("download"));
        service.setMode(QStringLiteral("next_launch"));
        QJsonObject changedAppearance = initialAppearance;
        changedAppearance.insert(QStringLiteral("background"), 0x1f1f1f);
        changedAppearance.insert(QStringLiteral("primary"), 0x722ed1);
        service.setProgressAppearance(changedAppearance);
        service.setProgressAppearance(changedAppearance);
        const QJsonObject oversizedAppearance{
            {QStringLiteral("fontFamily"), QString(64 * 1024, QLatin1Char('A'))}};
        service.setProgressAppearance(oversizedAppearance);
        require(service.busy() && service.status().state == UpdateState::Downloading,
                "oversized cosmetic configuration never interrupts the active update");
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&service, &languageChange);
        const QString configured = QDir(cache).filePath(QStringLiteral("configure.json"));
        require(waitUntil([&] {
                    return counterValue(QDir(cache).filePath(QStringLiteral("configure-count"))) >=
                           5;
                }),
                "policy, appearance, and language changes configure the running updater");
        const auto command = readJson(configured);
        require(
            command.value(QStringLiteral("mode")).toString() == u"next_launch" &&
                command.value(QStringLiteral("progressTexts")).toObject().size() == 15 &&
                command.value(QStringLiteral("progressAppearance")).toObject() ==
                    changedAppearance &&
                counterValue(QDir(cache).filePath(QStringLiteral("configure-count"))) == 5 &&
                counterValue(QDir(cache).filePath(QStringLiteral("cancel-count"))) == 0,
            "live appearance changes preserve downloads and ignore redundant or oversized data");
        service.setProgressAppearance({});
        require(waitUntil([&] {
                    const auto configuration = readJson(configured);
                    return counterValue(QDir(cache).filePath(QStringLiteral("configure-count"))) ==
                               6 &&
                           configuration.contains(QStringLiteral("progressAppearance")) &&
                           configuration.value(QStringLiteral("progressAppearance"))
                               .toObject()
                               .isEmpty();
                }),
                "clearing appearance explicitly restores updater defaults");
        service.setMode(QStringLiteral("check"));
        require(waitUntil([&] { return !service.busy(); }),
                "leaving automatic download policies cancels the automatic payload operation");
    }

    for (const int behavior : {0, 1, 2}) {
        const bool explicitCancel = behavior == 1;
        const bool reenableAtCompletion = behavior == 2;
        auto resumeOptions = options(QStringLiteral("policy-resume"));
        const QString cache = resumeOptions.cacheDirectory;
        UpdateService service(std::move(resumeOptions));
        QObject::connect(&service, &UpdateService::operationFinished, &app,
                         [&](const QString& operation, const QString& outcome) {
                             if (reenableAtCompletion && operation == u"download" &&
                                 outcome == u"cancelled")
                                 service.setMode(QStringLiteral("next_launch"));
                         });
        service.setMode(QStringLiteral("manual"));
        service.start();
        require(waitUntil([&] { return !service.busy(); }), "policy-resume probe completes");
        service.setMode(QStringLiteral("download"));
        require(waitUntil([&] { return service.status().state == UpdateState::Downloading; }),
                "automatic download starts before its policy cancellation");
        if (explicitCancel) {
            service.cancel();
            service.setMode(QStringLiteral("next_launch"));
        } else {
            service.setMode(QStringLiteral("check"));
            if (!reenableAtCompletion)
                service.setMode(QStringLiteral("next_launch"));
        }
        require(waitUntil([&] {
                    return QFileInfo::exists(
                        QDir(cache).filePath(QStringLiteral("cancel-blocked")));
                }),
                "the original download remains owned until its cancellation is acknowledged");
        releaseSidecar(cache, QStringLiteral("cancel"));
        require(waitUntil([&] {
                    return !service.busy() &&
                           service.status().state ==
                               (explicitCancel ? UpdateState::Available : UpdateState::Ready) &&
                           counterValue(QDir(cache).filePath(QStringLiteral(
                               "download-policyChange-count"))) == (explicitCancel ? 1 : 2);
                }),
                "download cancellation and any requested resumption finish");
        require(counterValue(QDir(cache).filePath(QStringLiteral("download-policyChange-count"))) ==
                    (explicitCancel ? 1 : 2),
                "only a reversed policy cancellation resumes the automatic download exactly once");
        require(service.status().state ==
                    (explicitCancel ? UpdateState::Available : UpdateState::Ready),
                "explicit cancellation stays stopped while re-enabled automatic policy downloads");
    }

    {
        auto pendingOptions = options(QStringLiteral("available"));
        const QString cache = pendingOptions.cacheDirectory;
        UpdateService service(std::move(pendingOptions));
        service.setMode(QStringLiteral("manual"));
        service.start();
        require(waitUntil([&] { return !service.busy(); }), "pending policy probe completes");
        service.setMode(QStringLiteral("download"));
        service.setMode(QStringLiteral("check"));
        service.setMode(QStringLiteral("next_launch"));
        require(waitUntil([&] { return !service.busy(); }),
                "policy cancellation withdrawn before handshake finishes");
        require(counterValue(QDir(cache).filePath(QStringLiteral("cancel-count"))) == 0 &&
                    counterValue(
                        QDir(cache).filePath(QStringLiteral("download-policyChange-count"))) == 1 &&
                    service.status().state == UpdateState::Ready,
                "unsent policy cancellation keeps the original download without a replacement");
    }

    {
        auto handoffOptions = options(QStringLiteral("delayed-handoff"));
        const QString cache = handoffOptions.cacheDirectory;
        UpdateService service(std::move(handoffOptions));
        bool prepared = false;
        bool committed = false;
        QByteArray checkpointValue = "before-pause";
        const QString checkpointPath = QDir(cache).filePath(QStringLiteral("pause-checkpoint"));
        service.setHandoffApproval(&app, [&] {
            prepared = true;
            require(!committed, "handoff preparation does not imply acceptance");
            service.setRelaunchArguments(
                {QStringLiteral("--autostart"), QStringLiteral("--skip-startup-update")});
            return true;
        });
        service.setHandoffValidation(&app, [&] {
            require(prepared && service.handoffPending(),
                    "successful final validation follows preparation and suspension");
            QSaveFile checkpoint(checkpointPath);
            require(checkpoint.open(QIODevice::WriteOnly) &&
                        checkpoint.write(checkpointValue) == checkpointValue.size() &&
                        checkpoint.commit(),
                    "final authority persists checkpoint state produced by suspension observers");
            return true;
        });
        QObject::connect(&service, &UpdateService::handoffPendingChanged, &app, [&](bool pending) {
            if (pending)
                checkpointValue = "after-pause";
        });
        QObject::connect(&service, &UpdateService::handoffCommitted, &app,
                         [&] { committed = true; });
        service.setMode(QStringLiteral("next_launch"));
        service.applyAtStartup();
        require(waitUntil([&] {
                    return prepared && QFileInfo::exists(
                                           QDir(cache).filePath(QStringLiteral("handoff-blocked")));
                }),
                "cached startup apply prepares handoff");
        require(!committed, "application remains alive before updater handoff acknowledgement");
        QFile checkpoint(checkpointPath);
        require(checkpoint.open(QIODevice::ReadOnly) && checkpoint.readAll() == "after-pause",
                "helper decision follows durable state written after pause observer callbacks");
        require(!service.setRelaunchArguments({QString(24 * 1024, u'x')}),
                "oversized late intent cannot mutate an accepted pending handoff");
        releaseSidecar(cache, QStringLiteral("handoff"));
        require(waitUntil([&] { return committed; }), "updater acknowledges committed handoff");
        require(!service.setRelaunchArguments(
                    {QStringLiteral("--autostart"), QStringLiteral("--skip-startup-update")}),
                "committed handoff closes late-intent admission before terminal observers");
        const auto execute = readJson(QDir(cache).filePath(QStringLiteral("execute.json")));
        require(execute.value(QStringLiteral("operation")).toString() == u"apply" &&
                    execute.value(QStringLiteral("trigger")).toString() == u"startup" &&
                    execute.value(QStringLiteral("mode")).toString() == u"next_launch" &&
                    execute.value(QStringLiteral("progressTexts")).toObject().size() == 15,
                "startup apply uses the cached-only protocol and translated progress text");
        const auto decision = readJson(QDir(cache).filePath(QStringLiteral("handoff.json")));
        require(
            decision.value(QStringLiteral("relaunchArguments")).toArray() ==
                QJsonArray{QStringLiteral("--autostart"), QStringLiteral("--skip-startup-update")},
            "the final handoff includes arguments chosen by preparation callbacks");
    }

    {
        auto retryOptions = options(QStringLiteral("retry-shell-handoff"));
        const QString cache = retryOptions.cacheDirectory;
        UpdateService service(std::move(retryOptions));
        service.setHandoffApproval(&app, [] { return true; });
        service.setHandoffValidation(&app, [] { return true; });
        const QString image = directory.filePath(QStringLiteral("selected image.png"));
        service.setMode(QStringLiteral("manual"));
        service.setRelaunchArguments({QStringLiteral("--edit-with-snow-shot"), image,
                                      QStringLiteral("--skip-startup-update")});
        QObject::connect(&service, &UpdateService::handoffPendingChanged, &app, [&](bool pending) {
            if (!pending)
                service.clearRelaunchArguments();
        });
        bool committed = false;
        bool failed = false;
        QObject::connect(&service, &UpdateService::operationFinished, &app,
                         [&](const QString& operation, const QString& outcome) {
                             failed = failed || (operation == u"apply" && outcome == u"failed");
                         });
        QObject::connect(&service, &UpdateService::handoffCommitted, &app,
                         [&] { committed = true; });
        service.beginApply();
        require(waitUntil([&] { return failed && !service.busy(); }),
                "first shell handoff fails and restores local processing");
        require(readJson(QDir(cache).filePath(QStringLiteral("handoff.json")))
                    .value(QStringLiteral("relaunchArguments"))
                    .toArray()
                    .contains(image),
                "failed handoff initially carried the selected image");
        service.beginApply();
        require(waitUntil([&] { return committed; }), "retry handoff commits");
        require(!readJson(QDir(cache).filePath(QStringLiteral("handoff.json")))
                     .contains(QStringLiteral("relaunchArguments")),
                "retry preserves ordinary relaunch behavior without replaying consumed images");
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
