#include "snow_shot/app/applicationrestart.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#ifdef Q_OS_WIN
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace {

using snow_shot::app::ApplicationRestartResult;
using snow_shot::app::ApplicationRestartTransactionOperations;
using snow_shot::app::runApplicationRestartTransaction;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QString privilegeState() {
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return QStringLiteral("unknown");
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool read =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return read && elevation.TokenIsElevated ? QStringLiteral("elevated")
                                             : QStringLiteral("standard");
#else
    return QString::number(static_cast<qulonglong>(geteuid()));
#endif
}

bool writeFixtureState(const QString& path, const QString& argumentsState) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray content = QByteArray::number(QCoreApplication::applicationPid()) + ' ' +
                               privilegeState().toUtf8() + ' ' + argumentsState.toUtf8();
    return file.write(content) == content.size();
}

QString readFixtureState(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void transactionBehavior() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        int guardCalls = 0;
        bool prepared = false;
        bool flushed = false;
        bool committed = false;
        bool cancelled = false;
        bool quit = false;
        ApplicationRestartTransactionOperations operations;
        operations.guard = [&]() {
            ++guardCalls;
            return scenario != 0 && !(scenario == 2 && guardCalls == 2);
        };
        operations.prepare = [&]() {
            prepared = true;
            return scenario == 1
                       ? ApplicationRestartResult{false, QStringLiteral("handshake failed")}
                       : ApplicationRestartResult{true, {}};
        };
        operations.flush = [&]() {
            flushed = true;
            return scenario != 3;
        };
        operations.commit = [&]() {
            committed = true;
            return ApplicationRestartResult{true, {}};
        };
        operations.cancel = [&]() { cancelled = true; };
        operations.quit = [&]() { quit = true; };

        const ApplicationRestartResult result = runApplicationRestartTransaction(operations);
        require(result.success == (scenario == 4),
                "only a fully committed restart transaction should succeed");
        require(prepared == (scenario != 0), "a busy application must not prepare a child");
        require(flushed == (scenario >= 3), "flush must follow both guard checks");
        require(committed == (scenario == 4), "commit must follow a successful flush");
        require(cancelled == (scenario == 2 || scenario == 3),
                "a prepared child must be cancelled after later failure");
        require(quit == (scenario == 4), "only a committed restart may quit the parent");
    }
}

void argumentBehavior() {
    const QStringList ordinary{QStringLiteral("snow-shot"), QStringLiteral("--show-main-window")};
    require(!snow_shot::app::isApplicationRestartHelperLaunch(ordinary) &&
                snow_shot::app::normalApplicationArguments(ordinary) == ordinary,
            "ordinary launch arguments must pass through unchanged");
    const QStringList helper{QStringLiteral("snow-shot"), QStringLiteral("--restart-helper"),
                             QStringLiteral("snow-shot-restart-channel"),
                             QStringLiteral("0123456789abcdef0123456789abcdef")};
    require(snow_shot::app::isApplicationRestartHelperLaunch(helper) &&
                snow_shot::app::normalApplicationArguments(helper) ==
                    QStringList{QStringLiteral("snow-shot")},
            "restart helper arguments must not reach normal startup");
}

void nativeHandoff() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "restart fixture temporary directory");
    const QString parentState = temporary.filePath(QStringLiteral("parent.txt"));
    const QString childState = temporary.filePath(QStringLiteral("child.txt"));
    qputenv("SNOW_SHOT_RESTART_TEST_DIRECTORY", temporary.path().toUtf8());

    QProcess parent;
    parent.start(QCoreApplication::applicationFilePath(),
                 {QStringLiteral("--restart-fixture-parent")});
    require(parent.waitForStarted(10000), "restart fixture parent should start");
    require(parent.waitForFinished(45000) && parent.exitStatus() == QProcess::NormalExit &&
                parent.exitCode() == 0,
            "restart fixture parent should complete its committed handoff");

    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(childState) && timer.elapsed() < 10000) {
        QThread::msleep(25);
    }
    const QStringList before = readFixtureState(parentState).split(QLatin1Char(' '));
    const QStringList after = readFixtureState(childState).split(QLatin1Char(' '));
    require(before.size() == 3 && after.size() == 3,
            "both restart fixture processes should publish their state");
    require(before.at(0) != after.at(0), "restart must continue in a replacement process");
    require(before.at(1) == after.at(1), "restart must preserve the current privilege state");
    require(after.at(2) == QStringLiteral("normal"),
            "the replacement must enter normal startup without helper arguments");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShot"));
    QCoreApplication::setApplicationName(QStringLiteral("snow-shot-application-restart-tests"));

    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == u"--restart-helper") {
        QString argumentsState;
        {
            QCoreApplication helper(argc, argv);
            const QStringList arguments = helper.arguments();
            const int result = snow_shot::app::dispatchApplicationRestartHelper(arguments);
            if (result != -1) {
                return result;
            }
            argumentsState = snow_shot::app::normalApplicationArguments(arguments).size() == 1
                                 ? QStringLiteral("normal")
                                 : QStringLiteral("helper");
        }
        const QString directory = qEnvironmentVariable("SNOW_SHOT_RESTART_TEST_DIRECTORY");
        return writeFixtureState(QDir(directory).filePath(QStringLiteral("child.txt")),
                                 argumentsState)
                   ? 0
                   : 40;
    }

    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--restart-fixture-parent"))) {
        const QString directory = qEnvironmentVariable("SNOW_SHOT_RESTART_TEST_DIRECTORY");
        if (!writeFixtureState(QDir(directory).filePath(QStringLiteral("parent.txt")),
                               QStringLiteral("fixture"))) {
            return 41;
        }
        snow_shot::app::ApplicationRestartCoordinator coordinator(application);
        QTimer::singleShot(0, &application, [&]() {
            const auto result = coordinator.restart([]() { return true; }, []() { return true; });
            if (!result.success) {
                application.exit(42);
            }
        });
        return application.exec();
    }

    transactionBehavior();
    argumentBehavior();
    nativeHandoff();
    return 0;
}
