#include "snow_shot/app/singleinstancecoordinator.h"
#include "snow_shot/app/applicationinputguard.h"
#include "snow_shot/update/startupupdate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <cstdlib>
#include <iostream>
#include <string>

using snow_shot::update::StartupUpdateResult;
using snow_shot::update::UpdateService;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QString argument(int argc, char** argv, const char* name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (QByteArray(argv[index]) == name)
            return QString::fromLocal8Bit(argv[index + 1]);
    }
    return {};
}

void frame(const QJsonObject& value) {
    std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << '\n'
              << std::flush;
}

void status(const char* state) {
    frame({{QStringLiteral("protocol"), 2},
           {QStringLiteral("type"), QStringLiteral("status")},
           {QStringLiteral("status"),
            QJsonObject{{QStringLiteral("state"), QString::fromLatin1(state)},
                        {QStringLiteral("version"), QStringLiteral("2.0.0")}}}});
}

void complete(const char* outcome, const char* state) {
    frame({{QStringLiteral("protocol"), 2},
           {QStringLiteral("type"), QStringLiteral("operation_complete")},
           {QStringLiteral("operation"), QStringLiteral("apply")},
           {QStringLiteral("outcome"), QString::fromLatin1(outcome)},
           {QStringLiteral("status"),
            QJsonObject{{QStringLiteral("state"), QString::fromLatin1(state)},
                        {QStringLiteral("version"), QStringLiteral("2.0.0")}}}});
}

void saveCommand(const QString& cache, const QJsonObject& command,
                 const QString& name = QStringLiteral("handoff.json")) {
    require(QDir().mkpath(cache), "create fake startup cache");
    QFile file(QDir(cache).filePath(name));
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "record startup handoff");
    file.write(QJsonDocument(command).toJson(QJsonDocument::Compact));
}

QJsonObject handoff(const QString& cache, const QString& name = QStringLiteral("handoff.json")) {
    QFile file(QDir(cache).filePath(name));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                          : QJsonObject{};
}

int fakeSidecar(int argc, char** argv) {
    const QString scenario = QUrl(argument(argc, argv, "--github-api-url")).path().mid(1);
    const QString cache = argument(argc, argv, "--cache");
    frame({{QStringLiteral("protocol"), 2},
           {QStringLiteral("type"), QStringLiteral("hello")},
           {QStringLiteral("updaterVersion"), QStringLiteral("test")}});
    status("Idle");
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString name = command.value(QStringLiteral("command")).toString();
        if (name == u"execute") {
            require(command.value(QStringLiteral("operation")).toString() == u"apply" &&
                        command.value(QStringLiteral("trigger")).toString() == u"startup" &&
                        command.value(QStringLiteral("mode")).toString() == u"next_launch",
                    "startup uses only the cached apply operation");
            if (scenario == u"absent" || scenario == u"invalid" || scenario == u"partial") {
                complete("success", "Available");
                return 0;
            }
            if (scenario == u"declined" || scenario == u"failed") {
                complete("failed", "Failed");
                return 0;
            }
            if (scenario == u"malformed") {
                std::cout << "{invalid}\n" << std::flush;
                return 0;
            }
            if (scenario == u"timeout")
                continue;
            status("Applying");
            if (scenario == u"delayed")
                QThread::msleep(250);
            frame({{QStringLiteral("protocol"), 2},
                   {QStringLiteral("type"), QStringLiteral("handoff_ready")}});
        } else if (name == u"handoff_decision") {
            saveCommand(cache, command);
            const bool proceed = command.value(QStringLiteral("proceed")).toBool();
            if (scenario.startsWith(u"late-forward")) {
                QElapsedTimer deadline;
                deadline.start();
                while (!QFileInfo::exists(QDir(cache).filePath(QStringLiteral("forwarded"))) &&
                       deadline.elapsed() < 5000)
                    QThread::msleep(1);
                require(deadline.elapsed() < 5000,
                        "late foreground request arrives before commitment acknowledgment");
            }
            frame({{QStringLiteral("protocol"), 2},
                   {QStringLiteral("type"), QStringLiteral("command_result")},
                   {QStringLiteral("id"), command.value(QStringLiteral("id"))},
                   {QStringLiteral("ok"), scenario != u"commit-failed"}});
            if (!proceed) {
                complete("cancelled", "Ready");
                return 0;
            }
            if (scenario == u"commit-failed")
                return 0;
        } else if (name == u"relaunch_arguments") {
            const bool accepted = scenario != u"late-forward-failed";
            saveCommand(cache, command, QStringLiteral("relaunch.json"));
            if (scenario == u"late-forward-disconnected")
                return 0;
            frame({{QStringLiteral("protocol"), 2},
                   {QStringLiteral("type"), QStringLiteral("command_result")},
                   {QStringLiteral("id"), command.value(QStringLiteral("id"))},
                   {QStringLiteral("ok"), accepted}});
            if (!accepted) {
                complete("failed", "Failed");
                return 0;
            }
        } else if (name == u"cancel" || name == u"shutdown") {
            complete("cancelled", "Idle");
            return 0;
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (QByteArray(argv[index]) == "--service")
            return fakeSidecar(argc, argv);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(argument(argc, argv, "--forward-startup-test").isEmpty()
                                             ? QStringLiteral("startup-update-") +
                                                   QUuid::createUuid().toString(QUuid::Id128)
                                             : argument(argc, argv, "--forward-startup-test"));
    QCoreApplication app(argc, argv);
    if (!argument(argc, argv, "--forward-startup-test").isEmpty()) {
        snow_shot::app::SingleInstanceCoordinator secondary;
        return secondary.acquireOrForward(
                            {QStringLiteral("snow-shot"), QStringLiteral("--show-main-window")})
                           .outcome == snow_shot::app::SingleInstanceOutcome::Forwarded
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    {
        snow_shot::app::ApplicationInputGuard guard(nullptr);
        for (const auto type : {QEvent::KeyPress, QEvent::MouseButtonPress, QEvent::Shortcut,
                                QEvent::Drop, QEvent::Close, QEvent::Quit}) {
            QEvent event(type);
            require(!guard.eventFilter(nullptr, &event), "ordinary input is allowed");
            guard.active = true;
            require(guard.eventFilter(nullptr, &event), "handoff input cannot mutate or close");
            guard.active = false;
            require(!guard.eventFilter(nullptr, &event), "failed handoff restores input");
        }
        guard.active = true;
        for (const auto type : {QEvent::Paint, QEvent::Timer, QEvent::SockAct}) {
            QEvent event(type);
            require(!guard.eventFilter(nullptr, &event),
                    "handoff keeps painting and IPC responsive");
        }
    }

    const QStringList original{QStringLiteral("snow-shot"), QStringLiteral("--autostart"),
                               QStringLiteral("--skip-startup-update")};
    require(snow_shot::update::isStartupUpdateRelaunch(original), "detect updater relaunch guard");
    require(snow_shot::update::normalStartupArguments(original) ==
                QStringList{QStringLiteral("snow-shot"), QStringLiteral("--autostart")},
            "strip updater guard before ordinary launch processing");
    require(snow_shot::update::startupUpdateRelaunchArguments() ==
                QStringList{QStringLiteral("--autostart"), QStringLiteral("--skip-startup-update")},
            "automatic updates relaunch in the tray and skip the startup gate once");
    require(snow_shot::update::startupUpdateRelaunchArguments(true) ==
                QStringList{QStringLiteral("--show-main-window"),
                            QStringLiteral("--skip-startup-update")},
            "a new foreground request opens the replacement and skips the startup gate once");
    const auto defaults = snow_shot::update::defaultUpdateServiceOptions();
    require(defaults.root == QFileInfo(defaults.applicationDirectory).dir().absolutePath() &&
                QFileInfo(defaults.cacheDirectory).fileName().size() == 24,
            "startup and running services share the installation-specific cache factory");

#ifdef Q_OS_WIN
    QTemporaryDir directory;
    require(directory.isValid(), "create startup test directory");
    const QString root = directory.filePath(QStringLiteral("Snow Shot"));
    const QString binary = QDir(root).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(binary), "create startup installation");
    require(QFile::copy(QCoreApplication::applicationFilePath(),
                        QDir(binary).filePath(QStringLiteral("snow-shot-updater.exe"))),
            "copy startup fake updater");
    int index = 0;
    const auto options = [&](const QString& scenario) {
        UpdateService::Options result;
        result.root = root;
        result.applicationDirectory = binary;
        result.cacheDirectory = directory.filePath(QStringLiteral("cache-%1").arg(++index));
        result.githubApiUrl = QUrl(QStringLiteral("https://updates.example.test/") + scenario);
        return result;
    };

    for (const QString& scenario :
         {QStringLiteral("absent"), QStringLiteral("invalid"), QStringLiteral("partial"),
          QStringLiteral("declined"), QStringLiteral("failed"), QStringLiteral("malformed"),
          QStringLiteral("timeout"), QStringLiteral("commit-failed")}) {
        UpdateService service(options(scenario));
        service.setMode(QStringLiteral("next_launch"));
        int flushes = 0;
        const auto result = snow_shot::update::runStartupUpdate(
            service,
            [&] {
                ++flushes;
                return true;
            },
            {}, scenario == u"timeout" ? std::chrono::milliseconds(100) : std::chrono::seconds(5));
        require(result == StartupUpdateResult::ContinueStartup,
                "cache no-op, rejected handoff, malformed response, and timeout continue startup");
        require(flushes == (scenario == u"commit-failed" ? 1 : 0),
                "only a prepared handoff flushes application storage");
    }

    for (const bool foregroundRequested : {false, true}) {
        auto value = options(QStringLiteral("ready"));
        const QString cache = value.cacheDirectory;
        UpdateService service(std::move(value));
        service.setMode(QStringLiteral("next_launch"));
        const auto result = snow_shot::update::runStartupUpdate(
            service, [] { return true; }, [&] { return foregroundRequested; });
        require(result == StartupUpdateResult::ExitForUpdate,
                "automatic update commits the requested launch behavior");
        require(handoff(cache).value(QStringLiteral("relaunchArguments")).toArray() ==
                    QJsonArray{foregroundRequested ? QStringLiteral("--show-main-window")
                                                   : QStringLiteral("--autostart"),
                               QStringLiteral("--skip-startup-update")},
                "automatic update stays in the tray unless a new foreground request arrives");
    }

    for (const bool flushSucceeds : {false, true}) {
        auto value = options(QStringLiteral("ready"));
        const QString cache = value.cacheDirectory;
        UpdateService service(std::move(value));
        service.setMode(QStringLiteral("next_launch"));
        int flushes = 0;
        const auto result = snow_shot::update::runStartupUpdate(service, [&] {
            ++flushes;
            return flushSucceeds;
        });
        require(flushes == 1, "startup handoff flushes exactly once");
        require(result == (flushSucceeds ? StartupUpdateResult::ExitForUpdate
                                         : StartupUpdateResult::ContinueStartup),
                "only a flushed and acknowledged handoff exits the application");
        const auto decision = handoff(cache);
        require(decision.value(QStringLiteral("proceed")).toBool() == flushSucceeds,
                "flush failure cancels installation before the parent exits");
        if (flushSucceeds)
            require(decision.value(QStringLiteral("relaunchArguments")).toArray() ==
                        QJsonArray{QStringLiteral("--autostart"),
                                   QStringLiteral("--skip-startup-update")},
                    "autostart stays in the tray and skips the next startup gate once");
    }

    for (const bool flushSucceeds : {false, true}) {
        snow_shot::app::SingleInstanceCoordinator primary;
        require(primary.acquireOrForward({QStringLiteral("snow-shot")}).outcome ==
                    snow_shot::app::SingleInstanceOutcome::Primary,
                "acquire startup singleton");
        auto value = options(QStringLiteral("delayed"));
        const QString cache = value.cacheDirectory;
        UpdateService service(std::move(value));
        service.setMode(QStringLiteral("next_launch"));
        QProcess secondary;
        bool secondaryStarted = false;
        bool foreground = false;
        QObject::connect(&service, &UpdateService::statusChanged, &app, [&] {
            if (service.status().state == snow_shot::update::UpdateState::Applying &&
                !secondaryStarted) {
                secondaryStarted = true;
                secondary.start(QCoreApplication::applicationFilePath(),
                                {QStringLiteral("--forward-startup-test"),
                                 QCoreApplication::applicationName()});
            }
        });
        const auto connection = QObject::connect(
            &primary, &snow_shot::app::SingleInstanceCoordinator::launchRequestReceived, &app,
            [&](const QStringList&) { foreground = true; });
        const auto result = snow_shot::update::runStartupUpdate(
            service, [&] { return flushSucceeds; }, [&] { return foreground; });
        QObject::disconnect(connection);
        require(secondaryStarted && foreground, "duplicate launch forwards during startup apply");
        require((secondary.state() == QProcess::NotRunning || secondary.waitForFinished(3000)) &&
                    secondary.exitCode() == 0,
                "duplicate launch does not become a second primary");
        require(result == (flushSucceeds ? StartupUpdateResult::ExitForUpdate
                                         : StartupUpdateResult::ContinueStartup),
                "forwarded activation preserves the startup handoff result");
        if (flushSucceeds) {
            require(handoff(cache).value(QStringLiteral("relaunchArguments")).toArray() ==
                        QJsonArray{QStringLiteral("--show-main-window"),
                                   QStringLiteral("--skip-startup-update")},
                    "forwarded foreground activation reaches the replacement application");
        } else {
            bool handled = false;
            primary.setLaunchRequestHandler([&](const QStringList&) { handled = true; });
            require(handled,
                    "fallback retains queued launch requests for the application controller");
        }
    }

    for (const QString& scenario :
         {QStringLiteral("late-forward"), QStringLiteral("late-forward-failed"),
          QStringLiteral("late-forward-disconnected")}) {
        const bool accepted = scenario == u"late-forward";
        snow_shot::app::SingleInstanceCoordinator primary;
        require(primary.acquireOrForward({QStringLiteral("snow-shot")}).outcome ==
                    snow_shot::app::SingleInstanceOutcome::Primary,
                "acquire late-forward startup singleton");
        auto value = options(scenario);
        const QString cache = value.cacheDirectory;
        UpdateService service(std::move(value));
        service.setMode(QStringLiteral("next_launch"));
        QProcess secondary;
        bool foreground = false;
        bool suspended = false;
        int commits = 0;
        QObject::connect(&service, &UpdateService::handoffPendingChanged, &app, [&](bool pending) {
            suspended = pending;
            if (pending)
                QTimer::singleShot(0, &app, [&] {
                    secondary.start(QCoreApplication::applicationFilePath(),
                                    {QStringLiteral("--forward-startup-test"),
                                     QCoreApplication::applicationName()});
                });
        });
        const auto connection = QObject::connect(
            &primary, &snow_shot::app::SingleInstanceCoordinator::launchRequestReceived, &app,
            [&](const QStringList&) {
                require(suspended && commits == 0, "forwarded launch arrives during commitment");
                foreground = true;
                service.setRelaunchArguments(
                    snow_shot::update::startupUpdateRelaunchArguments(foreground));
                QFile marker(QDir(cache).filePath(QStringLiteral("forwarded")));
                require(marker.open(QIODevice::WriteOnly),
                        "acknowledge late forwarding to fixture");
            });
        QObject::connect(&service, &UpdateService::handoffCommitted, &app, [&] {
            require(foreground && suspended, "commit follows the accepted late relaunch intent");
            ++commits;
        });
        const auto result = snow_shot::update::runStartupUpdate(service, [] { return true; });
        QObject::disconnect(connection);
        require(
            foreground &&
                (secondary.state() == QProcess::NotRunning || secondary.waitForFinished(3000)) &&
                secondary.exitCode() == 0,
            "late foreground request is successfully forwarded");
        require(
            handoff(cache).value(QStringLiteral("relaunchArguments")).toArray() ==
                QJsonArray{QStringLiteral("--autostart"), QStringLiteral("--skip-startup-update")},
            "initial handoff predates the foreground request");
        require(handoff(cache, QStringLiteral("relaunch.json"))
                        .value(QStringLiteral("relaunchArguments"))
                        .toArray() == QJsonArray{QStringLiteral("--show-main-window"),
                                                 QStringLiteral("--skip-startup-update")},
                "late foreground intent reaches the updater before the application exits");
        require(result == (accepted ? StartupUpdateResult::ExitForUpdate
                                    : StartupUpdateResult::ContinueStartup) &&
                    commits == (accepted ? 1 : 0) && suspended == accepted,
                "relaunch acknowledgment gates exit and failure restores suspended actions");
        if (!accepted) {
            bool handled = false;
            primary.setLaunchRequestHandler([&](const QStringList&) { handled = true; });
            require(handled, "failed commitment retains foreground requests for normal startup");
        }
    }
#endif
    return 0;
}
