#include "snow_shot/runtime/memoryoptimizationcontroller.h"
#include "snow_shot/runtime/memoryoptimizationuisafety.h"
#include <QApplication>
#include <QKeyEvent>
#include <QWidget>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace snow_shot::runtime;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct Fixture {
    RuntimeActivityTracker::TimePoint now{};
    RuntimeActivityTracker activity{[this] { return now; }};
    bool enabled = true;
    bool safe = true;
    std::optional<bool> pressure = false;
    std::optional<ProcessMemorySample> memory = ProcessMemorySample{24 * 1024 * 1024, 7};
    MemoryTrimResult result{true, 0};
    int calls = 0;
    int reports = 0;
    std::function<void()> beforeSample;
    MemoryOptimizationController controller{activity, options()};
    MemoryOptimizationController::Options options() {
        MemoryOptimizationController::Options value;
        value.clock = [this] { return now; };
        value.enabled = [this] { return enabled; };
        value.safe = [this] { return safe; };
        value.lowMemory = [this] { return pressure; };
        value.sample = [this] {
            if (beforeSample)
                beforeSample();
            return memory;
        };
        value.trim = [this] {
            ++calls;
            return result;
        };
        value.report = [this](const MemoryTrimReport&) { ++reports; };
        return value;
    }
    void tick(std::chrono::seconds elapsed) {
        now = RuntimeActivityTracker::TimePoint{} + elapsed;
        controller.evaluate();
    }
};

void testScheduling() {
    {
        Fixture f;
        f.tick(0s);
        f.tick(59s);
        require(f.calls == 0, "normal trim must respect startup and quiet grace");
        f.tick(60s);
        require(f.calls == 1 && f.reports == 1, "normal trim at exact eligibility boundary");
        f.tick(1000s);
        require(f.calls == 1, "one attempt per idle period even after cooldown");
        auto maintenance = f.activity.acquire(false);
        f.tick(1001s);
        require(f.calls == 1, "periodic housekeeping still blocks admission");
        maintenance = {};
        f.tick(1002s);
        require(f.calls == 1, "housekeeping must not rearm the same idle period");
        f.activity.markActivity();
        f.tick(1061s);
        require(f.calls == 1, "accepted activity restarts quiet period");
        f.tick(1062s);
        require(f.calls == 2, "new quiet period may trim");
    }
    {
        Fixture f;
        f.pressure = true;
        f.tick(0s);
        f.tick(59s);
        require(f.calls == 0, "pressure must not bypass startup grace");
        f.tick(60s);
        require(f.calls == 1, "pressure can trim after startup");
        auto work = f.activity.acquire();
        f.tick(345s);
        work = {};
        f.tick(359s);
        require(f.calls == 1, "pressure must respect quiet grace and cooldown");
        f.tick(360s);
        require(f.calls == 2, "pressure uses 15 second quiet period after work ends");
    }
    {
        Fixture f;
        f.enabled = false;
        f.controller.synchronizePolicy();
        require(!f.controller.isScheduling(), "Disabled stops timer immediately");
        f.tick(100s);
        require(f.calls == 0, "Disabled never trims");
        f.enabled = true;
        f.controller.synchronizePolicy();
        require(f.controller.isScheduling(), "Smart Control starts timer");
        f.tick(100s);
        f.tick(159s);
        require(f.calls == 0, "reenabling starts fresh quiet period");
        f.tick(160s);
        require(f.calls == 1, "reenabled policy eventually trims");
        f.controller.stop();
        f.enabled = false;
        f.controller.synchronizePolicy();
        f.enabled = true;
        f.tick(1000s);
        require(f.calls == 1 && !f.controller.isScheduling(), "shutdown cannot restart scheduling");
    }
    {
        Fixture f;
        f.tick(0s);
        bool firstSample = true;
        f.beforeSample = [&] {
            if (std::exchange(firstSample, false))
                f.now += 40s;
        };
        f.tick(60s);
        require(f.calls == 1, "sampling may delay the native attempt");
        f.activity.markActivity();
        f.tick(360s);
        require(f.calls == 1, "cooldown starts at the actual native attempt, after sampling");
        f.tick(400s);
        require(f.calls == 2, "actual attempt cooldown expires at the exact boundary");
    }
}

void testSafetyAndFailures() {
    {
        Fixture f;
        f.safe = false;
        f.tick(100s);
        f.safe = true;
        f.tick(100s);
        f.tick(159s);
        require(f.calls == 0, "visible UI transition restarts inactivity");
        f.tick(160s);
        require(f.calls == 1, "hidden or minimized UI becomes eligible after quiet");
    }
    {
        Fixture f;
        f.tick(0s);
        auto work = f.activity.acquire();
        f.tick(100s);
        require(f.calls == 0, "active work blocks trim");
        work = {};
        f.tick(159s);
        require(f.calls == 0, "actual worker completion restarts quiet");
        f.tick(160s);
        require(f.calls == 1, "completed work permits later trim");
    }
    {
        Fixture f;
        f.tick(0s);
        f.memory = std::nullopt;
        f.tick(60s);
        require(f.calls == 0, "failed sampling must fail closed");
        f.memory = ProcessMemorySample{18 * 1024 * 1024 - 1, 7};
        f.tick(61s);
        require(f.calls == 0, "private working set below 18 MiB must not trim without pressure");
        f.memory->privateWorkingSetBytes++;
        f.tick(62s);
        require(f.calls == 1, "exact 18 MiB private working-set threshold is eligible");
    }
    {
        Fixture f;
        f.pressure = true;
        f.memory->privateWorkingSetBytes = 12 * 1024 * 1024 - 1;
        f.tick(0s);
        f.tick(60s);
        require(f.calls == 0, "pressure private working-set threshold boundary");
        f.memory->privateWorkingSetBytes++;
        f.tick(61s);
        require(f.calls == 1, "exact 12 MiB pressure private working-set threshold is eligible");
    }
    {
        Fixture f;
        f.memory->privateWorkingSetBytes = 12 * 1024 * 1024;
        f.pressure = std::nullopt;
        f.tick(0s);
        f.tick(60s);
        require(f.calls == 0, "unavailable pressure uses the normal private working-set threshold");
        f.pressure = true;
        f.tick(61s);
        require(f.calls == 1,
                "reported memory pressure lowers the threshold in the same idle period");
    }
    {
        Fixture f;
        f.memory->privateWorkingSetBytes = 0;
        f.tick(0s);
        f.tick(60s);
        require(f.calls == 0, "zero private working set must not trim");
        f.pressure = true;
        f.tick(61s);
        require(f.calls == 0, "memory pressure cannot make a zero private working set eligible");
    }
    {
        Fixture f;
        f.pressure = std::nullopt;
        f.result = {false, 5};
        f.tick(0s);
        f.tick(60s);
        f.tick(1000s);
        require(
            f.calls == 1 && f.reports == 1,
            "native failure consumes idle attempt; pressure query failure preserves normal policy");
    }
    {
        Fixture f;
        f.tick(0s);
        f.beforeSample = [&f] { f.activity.markActivity(); };
        f.tick(60s);
        require(f.calls == 0, "activity during sampling invalidates admission generation");
    }
    {
        Fixture f;
        f.tick(0s);
        f.beforeSample = [&f] { f.enabled = false; };
        f.tick(60s);
        require(f.calls == 0, "latest policy rechecked before admission");
    }
    {
        Fixture f;
        f.tick(0s);
        QWidget input;
        f.now += 30s;
        QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
        QApplication::sendEvent(&input, &key);
        f.tick(89s);
        require(f.calls == 0, "application input resets inactivity");
        f.tick(90s);
        require(f.calls == 1, "input quiet boundary");
    }
}

void testAdmissionAndLifetime() {
    RuntimeActivityTracker tracker;
    auto first = tracker.acquire();
    auto copy = first;
    require(tracker.snapshot().activeCount == 1, "lease copies retain one counted reservation");
    first = {};
    require(tracker.snapshot().activeCount == 1, "owner destruction does not release worker lease");
    const auto generation = tracker.snapshot().generation;
    require(!tracker.tryRunWhenIdle(generation, [] {}), "live worker rejects admission");
    copy = {};
    require(tracker.snapshot().activeCount == 0, "last lease retires reservation");
    require(!tracker.tryRunWhenIdle(generation, [] {}), "stale generation rejects admission");

    const auto idle = tracker.snapshot();
    std::promise<void> trying;
    std::atomic_bool acquired = false;
    std::thread submitter;
    require(tracker.tryRunWhenIdle(
                idle.generation,
                [&] {
                    submitter = std::thread([&] {
                        trying.set_value();
                        auto pending = tracker.acquire();
                        acquired = true;
                    });
                    trying.get_future().wait();
                    require(!acquired, "new work cannot acquire admission during native trim");
                }),
            "idle generation admitted");
    submitter.join();
    require(acquired, "waiting work admitted after trim returns");

    const auto baseline = RuntimeActivityTracker::shared().snapshot().activeCount;
    std::promise<void> release;
    auto barrier = release.get_future().share();
    auto wrapped = trackRuntimeWork([barrier] { barrier.wait(); });
    require(RuntimeActivityTracker::shared().snapshot().activeCount == baseline + 1,
            "worker wrapper reserves before submission");
    auto running = std::async(std::launch::async, std::move(wrapped));
    require(RuntimeActivityTracker::shared().snapshot().activeCount == baseline + 1,
            "outstanding async work retains reservation");
    release.set_value();
    running.wait();
    require(running.valid() && RuntimeActivityTracker::shared().snapshot().activeCount == baseline,
            "ready unconsumed futures must not retain completed work reservations");
    running.get();
    require(RuntimeActivityTracker::shared().snapshot().activeCount == baseline,
            "actual worker completion releases reservation");
}

void testRetainedWorkLifetime() {
    auto& activity = RuntimeActivityTracker::shared();
    const auto baseline = activity.snapshot().activeCount;
    auto retained = trackRuntimeWork([&activity, baseline] {
        require(activity.snapshot().activeCount == baseline + 1,
                "each invocation retains activity while executing");
        return 42;
    });
    auto copy = retained;
    require(activity.snapshot().activeCount == baseline + 1,
            "callable copies share one queued reservation");
    require(copy() == 42 && activity.snapshot().activeCount == baseline,
            "completed work releases activity even while callable copies survive");
    require(retained() == 42 && activity.snapshot().activeCount == baseline,
            "a repeated invocation reacquires and releases activity");

    auto throwing = trackRuntimeWork([]() -> void { throw std::runtime_error("work failed"); });
    auto throwingCopy = throwing;
    auto failed = std::async(std::launch::async, std::move(throwingCopy));
    failed.wait();
    require(failed.valid() && activity.snapshot().activeCount == baseline,
            "throwing work releases activity before future consumption and alias destruction");
    bool propagated = false;
    try {
        failed.get();
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    require(propagated, "tracked work preserves its original exception");

    QObject receiver;
    int callbacks = 0;
    auto callback = trackRuntimeWork([&] {
        require(activity.snapshot().activeCount == baseline + 1,
                "queued callbacks retain activity through result consumption");
        ++callbacks;
    });
    require(QMetaObject::invokeMethod(&receiver, callback, Qt::QueuedConnection),
            "submit a tracked callback while retaining its original callable");
    require(activity.snapshot().activeCount == baseline + 1 && callbacks == 0,
            "queued callbacks reserve activity before dispatch");
    QCoreApplication::sendPostedEvents(&receiver, QEvent::MetaCall);
    require(callbacks == 1 && activity.snapshot().activeCount == baseline,
            "dispatched callbacks release activity despite retained callable aliases");

    {
        QObject canceledReceiver;
        require(QMetaObject::invokeMethod(&canceledReceiver,
                                          trackRuntimeWork([&callbacks] { ++callbacks; }),
                                          Qt::QueuedConnection),
                "submit a tracked callback before destroying its receiver");
        require(activity.snapshot().activeCount == baseline + 1,
                "undispatched callbacks retain queued activity");
    }
    require(callbacks == 1 && activity.snapshot().activeCount == baseline,
            "receiver destruction releases canceled activity without invoking work");

    std::function<void()> abandoned = trackRuntimeWork([&callbacks] { ++callbacks; });
    auto abandonedCopy = abandoned;
    abandoned = {};
    require(activity.snapshot().activeCount == baseline + 1,
            "an uninvoked callable alias preserves its pending reservation");
    abandonedCopy = {};
    require(callbacks == 1 && activity.snapshot().activeCount == baseline,
            "destroying the last uninvoked alias releases its pending reservation");
}

void testConcurrentWorkLifetime() {
    auto& activity = RuntimeActivityTracker::shared();
    const auto baseline = activity.snapshot().activeCount;
    std::promise<void> firstEntered;
    std::promise<void> secondEntered;
    auto firstStarted = firstEntered.get_future();
    auto secondStarted = secondEntered.get_future();
    std::promise<void> releaseFirst;
    std::promise<void> releaseSecond;
    auto work = trackRuntimeWork([](std::promise<void>& entered, std::shared_future<void> release) {
        entered.set_value();
        release.wait();
    });
    auto first = std::async(std::launch::async, work, std::ref(firstEntered),
                            releaseFirst.get_future().share());
    auto second = std::async(std::launch::async, work, std::ref(secondEntered),
                             releaseSecond.get_future().share());
    firstStarted.wait();
    secondStarted.wait();
    require(activity.snapshot().activeCount == baseline + 2,
            "concurrent invocations each retain their own execution lease");
    releaseFirst.set_value();
    first.wait();
    require(activity.snapshot().activeCount == baseline + 1,
            "one completed invocation must not release another invocation's protection");
    releaseSecond.set_value();
    second.wait();
    require(activity.snapshot().activeCount == baseline,
            "all completed invocations release activity despite retained futures and aliases");
    first.get();
    second.get();
}

void testUiGates() {
    QWidget primary;
    QWidget staticPin;
    QWidget toolbar;
    bool interacting = false;
    auto idle = [&](const QWidget* surface) {
        return !interacting && (surface == &staticPin || surface == &toolbar);
    };
    require(memoryOptimizationUiIsSafe(idle), "hidden windows are eligible");
    staticPin.show();
    toolbar.show();
    require(memoryOptimizationUiIsSafe(idle), "static visible pins and toolbar are eligible");
    interacting = true;
    require(!memoryOptimizationUiIsSafe(idle), "interactive or animated surface blocks trim");
    interacting = false;
    primary.show();
    require(!memoryOptimizationUiIsSafe(idle), "visible primary window blocks trim");
    primary.showMinimized();
    require(memoryOptimizationUiIsSafe(idle), "minimized primary window permits trim");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    testScheduling();
    testSafetyAndFailures();
    testAdmissionAndLifetime();
    testRetainedWorkLifetime();
    testConcurrentWorkLifetime();
    testUiGates();
    std::cout << "Memory optimization controller tests passed\n";
}
