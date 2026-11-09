// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/deferredstartup.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void dispatchStartup() {
    // Each of the six finite steps is posted separately. No wall-clock/native work is needed.
    for (int i = 0; i != 12; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void preservesOrderAndPausesBetweenSteps() {
    using snow_shot::app::DeferredStartup;
    QObject context;
    bool available = true;
    std::vector<int> sequence;
    DeferredStartup startup(context, [&] { return available; });
    startup.start({[&] {
                       sequence.push_back(1);
                       available = false;
                   },
                   [&] { sequence.push_back(2); }, [&] { sequence.push_back(3); },
                   [&] { sequence.push_back(4); }, [&] { sequence.push_back(5); },
                   [&] { sequence.push_back(6); }});
    require(sequence.empty(), "first-file completion returns before background work starts");
    dispatchStartup();
    require(sequence == std::vector<int>{1},
            "reentrant pause during update startup defers native registration maintenance");
    startup.start({[&] { sequence.push_back(99); }, {}, {}, {}, {}, {}});
    startup.resume();
    dispatchStartup();
    require(sequence == std::vector<int>{1}, "duplicate continuation cannot replace paused work");
    available = true;
    startup.resume();
    startup.resume();
    dispatchStartup();
    require(sequence == std::vector<int>({1, 2, 3, 4, 5, 6}),
            "restoration runs remaining work once with maintenance before prewarm and restores");
    startup.resume();
    dispatchStartup();
    require(sequence.size() == 6, "completed startup cannot be replayed");
}

void dispatchObservesNewPauseAndShutdown() {
    using snow_shot::app::DeferredStartup;
    QObject context;
    bool available = true;
    int calls = 0;
    DeferredStartup startup(context, [&] { return available; });
    startup.start({[&] { ++calls; }, {}, {}, {}, {}, {}});
    available = false;
    dispatchStartup();
    require(calls == 0, "a new pause after scheduling prevents delayed work from starting");
    available = true;
    startup.resume();
    startup.stop();
    dispatchStartup();
    startup.resume();
    startup.start({[&] { ++calls; }, {}, {}, {}, {}, {}});
    dispatchStartup();
    require(calls == 0, "terminal shutdown discards paused and queued startup work");
}

void externalStepsMayRetireTheirOwners() {
    using snow_shot::app::DeferredStartup;
    QObject context;
    int calls = 0;
    auto startup = std::make_unique<DeferredStartup>(context, [] { return true; });
    startup->start({[&] { startup.reset(); }, [&] { ++calls; }, {}, {}, {}, {}});
    dispatchStartup();
    require(!startup && calls == 0,
            "destruction during an external step cancels maintenance without expired owner access");

    auto transient = std::make_unique<QObject>();
    DeferredStartup orphaned(*transient, [] { return true; });
    orphaned.start({[&] { ++calls; }, {}, {}, {}, {}, {}});
    transient.reset();
    dispatchStartup();
    orphaned.resume();
    dispatchStartup();
    require(calls == 0, "queued work cannot outlive the application controller's Qt context");

    std::unique_ptr<DeferredStartup> checked;
    checked = std::make_unique<DeferredStartup>(context, [&] {
        checked.reset();
        return true;
    });
    checked->start({[&] { ++calls; }, {}, {}, {}, {}, {}});
    dispatchStartup();
    require(!checked && calls == 0,
            "admission callback destruction cannot invoke expired startup work");
}

void nestedEventsCannotStartTheNextStep() {
    using snow_shot::app::DeferredStartup;
    QObject context;
    std::vector<int> sequence;
    DeferredStartup startup(context, [] { return true; });
    startup.start({[&] {
                       sequence.push_back(1);
                       startup.resume();
                       dispatchStartup();
                       require(sequence == std::vector<int>{1},
                               "reentrant restoration cannot overlap an executing startup step");
                   },
                   [&] { sequence.push_back(2); },
                   {},
                   {},
                   {},
                   {}});
    dispatchStartup();
    require(sequence == std::vector<int>({1, 2}),
            "remaining work starts once after the reentrant step returns");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    preservesOrderAndPausesBetweenSteps();
    dispatchObservesNewPauseAndShutdown();
    externalStepsMayRetireTheirOwners();
    nestedEventsCannotStartTheNextStep();
    return EXIT_SUCCESS;
}
