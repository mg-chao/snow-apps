#include "snow_shot/app/distribution.h"
#include "snow_shot/app/storeupdatelifecycle.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void distributionPolicy() {
    using snow_shot::app::distribution::resolvePolicy;
    require(!resolvePolicy(false, {}).managedByStore, "unpackaged standalone remains native");
    require(resolvePolicy(true, {}).managedByStore,
            "Store builds enforce policy before activation");
    const auto packaged = resolvePolicy(false, QStringLiteral("Package_family"));
    require(packaged.managedByStore && packaged.packageFamily == QStringLiteral("Package_family"),
            "runtime package identity disables native installation operations");
    require(resolvePolicy(true, {}).packageFamily ==
                QStringLiteral("SnowApps.SnowShot_3pp9w6ncw7bs0"),
            "unpackaged Store development retains a separate stable identity");
}

void shutdownTransitions() {
    QStringList events;
    bool busy = true;
    bool canSave = true;
    int preservationBudget = 0;
    snow_shot::app::StoreUpdateLifecycle lifecycle({
        [&] { return busy; },
        [&](bool suspend) {
            events.append(suspend ? QStringLiteral("freeze") : QStringLiteral("resume"));
        },
        [&] {
            events.append(QStringLiteral("save"));
            return canSave;
        },
        [&](int budget) {
            preservationBudget = budget;
            events.append(QStringLiteral("recording"));
        },
        [&] { events.append(QStringLiteral("terminal-freeze")); },
        [&] {
            events.append(QStringLiteral("terminal-save"));
            std::promise<void> saved;
            saved.set_value();
            return saved.get_future().share();
        },
    });
    require(!lifecycle.queryEndSession() && events.isEmpty(),
            "active work must veto cooperative update shutdown without changing state");
    busy = false;
    canSave = false;
    require(!lifecycle.queryEndSession() &&
                events == QStringList{QStringLiteral("freeze"), QStringLiteral("save"),
                                      QStringLiteral("resume")},
            "checkpoint failure must restore app availability");
    events.clear();
    canSave = true;
    require(lifecycle.queryEndSession() && lifecycle.queryEndSession() && lifecycle.pending() &&
                events == QStringList{QStringLiteral("freeze"), QStringLiteral("save")},
            "duplicate queries must checkpoint and suspend once");
    lifecycle.endSession(false);
    lifecycle.endSession(false);
    require(!lifecycle.pending() && events.last() == QStringLiteral("resume") && events.size() == 3,
            "cancelled update shutdown must resume work exactly once");
    events.clear();
    lifecycle.endSession(true);
    lifecycle.endSession(true);
    require(preservationBudget >= 0 && preservationBudget <= 4000 &&
                events == QStringList{QStringLiteral("terminal-freeze"),
                                      QStringLiteral("terminal-save"), QStringLiteral("recording")},
            "forced shutdown must save concurrently without invoking cooperative teardown");
}

void exceptionalShutdownTransitions() {
    QStringList events;
    enum class Failure { None, Busy, Suspend, Checkpoint } failure = Failure::Busy;
    snow_shot::app::StoreUpdateLifecycle lifecycle({
        [&] {
            if (failure == Failure::Busy)
                throw std::runtime_error("busy inspection failed");
            return false;
        },
        [&](bool suspend) {
            events.append(suspend ? QStringLiteral("freeze") : QStringLiteral("resume"));
            if (suspend && failure == Failure::Suspend)
                throw std::runtime_error("partial suspension failed");
        },
        [&] {
            events.append(QStringLiteral("save"));
            if (failure == Failure::Checkpoint)
                throw std::runtime_error("checkpoint failed");
            return true;
        },
        {},
        {},
        {},
    });
    require(!lifecycle.queryEndSession() && !lifecycle.pending() && events.isEmpty(),
            "failed busy inspection must veto shutdown without suspending work");
    failure = Failure::Suspend;
    require(!lifecycle.queryEndSession() && !lifecycle.pending() &&
                events == QStringList{QStringLiteral("freeze"), QStringLiteral("resume")},
            "partially failed suspension must restore availability");
    events.clear();
    failure = Failure::Checkpoint;
    require(!lifecycle.queryEndSession() && !lifecycle.pending() &&
                events == QStringList{QStringLiteral("freeze"), QStringLiteral("save"),
                                      QStringLiteral("resume")},
            "throwing checkpoint must restore availability and leave the query retryable");
    events.clear();
    failure = Failure::None;
    require(lifecycle.queryEndSession() && lifecycle.pending(),
            "a successful query can retry after an exception");
    lifecycle.endSession(false);
    require(!lifecycle.pending() &&
                events == QStringList{QStringLiteral("freeze"), QStringLiteral("save"),
                                      QStringLiteral("resume")},
            "a retried query still resumes normally if Windows cancels shutdown");

    events.clear();
    snow_shot::app::StoreUpdateLifecycle terminal({
        {},
        [&](bool) { events.append(QStringLiteral("cooperative-teardown")); },
        {},
        [&](int budget) {
            require(budget >= 0 && budget <= 4000, "terminal preservation budget stays bounded");
            events.append(QStringLiteral("recording"));
            throw std::runtime_error("preservation failed");
        },
        [&] {
            events.append(QStringLiteral("terminal-freeze"));
            throw std::runtime_error("terminal suspension failed");
        },
        [&]() -> std::shared_future<void> {
            events.append(QStringLiteral("terminal-save"));
            throw std::runtime_error("checkpoint launch failed");
        },
    });
    terminal.endSession(true);
    terminal.endSession(true);
    require(terminal.pending() &&
                events == QStringList{QStringLiteral("terminal-freeze"),
                                      QStringLiteral("terminal-save"), QStringLiteral("recording")},
            "each terminal preservation hook runs once even when an earlier hook throws");
}

void cooperativeCommitRechecksAfterPreparation() {
    enum class Boundary { Suspend, Checkpoint, Validation };
    for (const auto boundary : {Boundary::Suspend, Boundary::Checkpoint, Boundary::Validation}) {
        bool paused = false;
        bool protectedWork = false;
        bool inject = true;
        int checkpoints = 0;
        int validations = 0;
        int resumes = 0;
        int recordingPreservations = 0;
        snow_shot::app::StoreUpdateLifecycle::Hooks hooks;
        hooks.busy = [&] { return paused || protectedWork; };
        hooks.suspend = [&](bool suspended) {
            paused = suspended;
            if (!suspended)
                ++resumes;
            else if (inject && boundary == Boundary::Suspend)
                protectedWork = true;
        };
        hooks.checkpoint = [&] {
            require(paused, "cooperative checkpoint runs with admission closed");
            ++checkpoints;
            if (inject && boundary == Boundary::Checkpoint)
                protectedWork = true;
            return true;
        };
        hooks.validateAfterCheckpoint = [&] {
            require(paused && checkpoints == validations + 1,
                    "final validation runs after each acquired pause and checkpoint");
            ++validations;
            if (inject && boundary == Boundary::Validation)
                throw std::runtime_error("final admission failed");
            return !protectedWork;
        };
        hooks.preserveRecording = [&](int budget) {
            require(budget >= 0 && budget <= 4000, "forced preservation keeps its deadline");
            ++recordingPreservations;
        };
        snow_shot::app::StoreUpdateLifecycle lifecycle(std::move(hooks));
        require(!lifecycle.queryEndSession() && !lifecycle.pending() && !paused &&
                    checkpoints == 1 && validations == 1 && resumes == 1,
                "new work or failed final validation vetoes cooperative commitment and restores");
        if (boundary == Boundary::Validation) {
            lifecycle.endSession(true);
            lifecycle.endSession(true);
            require(lifecycle.pending() && validations == 1 && recordingPreservations == 1,
                    "forced termination bypasses failed cooperative admission and preserves once");
            continue;
        }
        inject = false;
        protectedWork = false;
        require(lifecycle.queryEndSession() && lifecycle.pending() && paused && checkpoints == 2 &&
                    validations == 2,
                "retry validates current work without treating its own pause as a blocker");
        require(lifecycle.queryEndSession() && checkpoints == 2 && validations == 2,
                "accepted duplicate query does not acquire or validate another pause");
        lifecycle.endSession(false);
        require(!lifecycle.pending() && !paused && resumes == 2,
                "cancelled retry restores exactly its own accepted pause");
    }
}

void forcedTerminationOwnsReentrantCooperativeCompletion() {
    enum class Boundary { Busy, Suspend, Checkpoint, Validation };
    for (const auto boundary :
         {Boundary::Busy, Boundary::Suspend, Boundary::Checkpoint, Boundary::Validation}) {
        for (const bool throws : {false, true}) {
            int resumes = 0;
            int checkpoints = 0;
            int validations = 0;
            int terminalFreezes = 0;
            int preserved = 0;
            snow_shot::app::StoreUpdateLifecycle* current = nullptr;
            const auto terminate = [&](Boundary point) {
                if (point != boundary)
                    return;
                current->endSession(true);
                if (throws)
                    throw std::runtime_error("cooperative callback failed after forced shutdown");
            };
            snow_shot::app::StoreUpdateLifecycle::Hooks hooks;
            hooks.busy = [&] {
                terminate(Boundary::Busy);
                return boundary == Boundary::Busy;
            };
            hooks.suspend = [&](bool suspended) {
                if (suspended)
                    terminate(Boundary::Suspend);
                else
                    ++resumes;
            };
            hooks.checkpoint = [&] {
                ++checkpoints;
                terminate(Boundary::Checkpoint);
                return boundary != Boundary::Checkpoint;
            };
            hooks.validateAfterCheckpoint = [&] {
                ++validations;
                terminate(Boundary::Validation);
                return false;
            };
            hooks.suspendForTermination = [&] { ++terminalFreezes; };
            hooks.preserveRecording = [&](int) { ++preserved; };
            snow_shot::app::StoreUpdateLifecycle lifecycle(std::move(hooks));
            current = &lifecycle;
            require(lifecycle.queryEndSession() && lifecycle.pending() && resumes == 0 &&
                        terminalFreezes == 1 && preserved == 1 &&
                        checkpoints ==
                            (boundary == Boundary::Busy || boundary == Boundary::Suspend ? 0 : 1) &&
                        validations == (boundary == Boundary::Validation ? 1 : 0),
                    "terminal ownership prevents obsolete cooperative callbacks reopening work");
            require(lifecycle.queryEndSession() && lifecycle.pending() && resumes == 0 &&
                        preserved == 1,
                    "a query after forced termination preserves the terminal result");
            lifecycle.endSession(false);
            require(lifecycle.pending() && resumes == 0,
                    "late cooperative cancellation cannot undo forced termination");
        }
    }
}

void terminalSavesShareOneDeadline() {
    std::promise<void> pendingSave;
    const auto save = pendingSave.get_future().share();
    int preservationBudget = 0;
    snow_shot::app::StoreUpdateLifecycle lifecycle({
        {},
        {},
        {},
        [&](int budget) {
            preservationBudget = budget;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        },
        {},
        [&] { return save; },
    });
    QElapsedTimer elapsed;
    elapsed.start();
    lifecycle.endSession(true);
    require(preservationBudget > 0 && preservationBudget <= 4000 && elapsed.elapsed() >= 3500 &&
                elapsed.elapsed() < 5500,
            "a blocked checkpoint must return at the shared terminal deadline");
    pendingSave.set_value();
}

#ifdef Q_OS_WIN
int terminalChild(const QString& marker) {
    struct TeardownProbe {
        QString marker;
        ~TeardownProbe() {
            QFile file(marker);
            if (file.open(QIODevice::WriteOnly))
                file.write("ordinary teardown ran");
        }
    } teardown{marker};
    snow_shot::app::StoreUpdateLifecycle lifecycle({});
    MSG message{};
    message.message = WM_ENDSESSION;
    message.wParam = TRUE;
    message.lParam = ENDSESSION_CLOSEAPP;
    qintptr result = 0;
    static_cast<void>(lifecycle.nativeEventFilter({}, &message, &result));
    return 99;
}

void terminalNativeCallbackSkipsDestructiveTeardown() {
    QTemporaryDir directory;
    require(directory.isValid(), "terminal callback fixture should have a writable directory");
    const auto marker = directory.filePath(QStringLiteral("teardown.txt"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--terminal-child"), marker});
    require(child.waitForStarted() && child.waitForFinished(5000) &&
                child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 &&
                !QFile::exists(marker),
            "terminal Windows shutdown must bypass destructor joins and media cancellation");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
#ifdef Q_OS_WIN
    if (application.arguments().size() == 3 &&
        application.arguments().at(1) == QStringLiteral("--terminal-child"))
        return terminalChild(application.arguments().at(2));
#endif
    distributionPolicy();
    shutdownTransitions();
    exceptionalShutdownTransitions();
    cooperativeCommitRechecksAfterPreparation();
    forcedTerminationOwnsReentrantCooperativeCompletion();
    terminalSavesShareOneDeadline();
#ifdef Q_OS_WIN
    terminalNativeCallbackSkipsDestructiveTeardown();
#endif
    return 0;
}
