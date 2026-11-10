#include "snow_shot/app/mcp/mcpruntimelifecycle.h"
#include "snow_shot/app/operationadmission.h"
#include "snow_shot/app/quiescencesession.h"
#include "snow_shot/app/applicationinputguard.h"

#include <cstdlib>
#include <array>
#include <iostream>
#include <vector>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void verifyAdmissionMatrix() {
    using namespace snow_shot::app;
    // Columns follow the existing operation-specific product policy, independently of the
    // predicate implementation. Setting one fact must produce exactly the listed reason.
    constexpr std::array operations{
        ApplicationOperation::Restart,       ApplicationOperation::UpdateRequest,
        ApplicationOperation::UpdateHandoff, ApplicationOperation::Relocation,
        ApplicationOperation::Store,         ApplicationOperation::Trim};
    using B = OperationBlocker;
    struct Row {
        bool OperationFacts::* fact;
        std::array<B, operations.size()> reasons;
    };
    const std::array rows{
        Row{&OperationFacts::suspended,
            {B::Suspended, B::None, B::Suspended, B::Suspended, B::Suspended, B::Suspended}},
        Row{&OperationFacts::storageChanging,
            {B::Storage, B::None, B::Storage, B::Storage, B::Storage, B::Storage}},
        Row{&OperationFacts::updaterUnavailableOrApplying,
            {B::Updater, B::None, B::None, B::Updater, B::None, B::None}},
        Row{&OperationFacts::updaterBusy,
            {B::None, B::None, B::None, B::None, B::None, B::Updater}},
        Row{&OperationFacts::captureBusy,
            {B::Capture, B::Capture, B::Capture, B::Capture, B::None, B::None}},
        Row{&OperationFacts::captureInteracting,
            {B::None, B::None, B::None, B::None, B::Capture, B::Capture}},
        Row{&OperationFacts::directCaptureBusy,
            {B::DirectCapture, B::DirectCapture, B::DirectCapture, B::DirectCapture,
             B::DirectCapture, B::DirectCapture}},
        Row{&OperationFacts::unsavedEditors,
            {B::Editors, B::None, B::Editors, B::Editors, B::None, B::None}},
        Row{&OperationFacts::ocrBusy, {B::None, B::None, B::None, B::Ocr, B::Ocr, B::Ocr}},
        Row{&OperationFacts::mcpBusy, {B::None, B::None, B::Mcp, B::Mcp, B::Mcp, B::Mcp}},
        Row{&OperationFacts::diagnosticsBusy,
            {B::None, B::None, B::None, B::Diagnostics, B::Diagnostics, B::Diagnostics}},
        Row{&OperationFacts::exports, {B::None, B::None, B::None, B::Export, B::Export, B::None}},
        Row{&OperationFacts::globalCanvas,
            {B::None, B::None, B::None, B::None, B::Interaction, B::None}},
        Row{&OperationFacts::pinOrToolbarInteraction,
            {B::None, B::None, B::None, B::None, B::Interaction, B::Interaction}},
        Row{&OperationFacts::foreground,
            {B::None, B::None, B::None, B::None, B::None, B::Interaction}},
        Row{&OperationFacts::administratorPending,
            {B::None, B::Administrator, B::None, B::None, B::None, B::None}}};
    for (size_t column = 0; column != operations.size(); ++column) {
        require(inspectOperation(operations[column], {}).allowed(), "empty facts permit operation");
        std::uint32_t combined = 0;
        OperationFacts all;
        for (const auto& row : rows) {
            OperationFacts facts;
            facts.*(row.fact) = true;
            all.*(row.fact) = true;
            const auto expected = static_cast<std::uint32_t>(row.reasons[column]);
            const auto actual = inspectOperation(operations[column], facts);
            require(actual.blockers == expected && actual.allowed() == (expected == 0),
                    "each resource fact preserves every operation's exact admission policy");
            combined |= expected;
        }
        require(inspectOperation(operations[column], all).blockers == combined,
                "simultaneous blockers remain visible without short-circuit loss");
    }
}

void verifyLifecycleInterleavings() {
    using snow_shot::app::mcp::McpRuntimeLifecycle;
    int starts = 0;
    int stops = 0;
    std::vector<std::function<void()>> drains;
    McpRuntimeLifecycle lifecycle({[&] {
                                       ++starts;
                                       return McpRuntimeLifecycle::StartOutcome{true, {}};
                                   },
                                   [&](auto done) { drains.push_back(std::move(done)); },
                                   [&] { ++stops; }});
    lifecycle.setSuspended(true);
    lifecycle.setEnabled(true);
    lifecycle.setEnabled(false);
    lifecycle.setEnabled(true);
    require(starts == 0 && drains.empty(), "configuration while paused cannot open admission");
    lifecycle.setSuspended(false);
    lifecycle.setEnabled(false);
    lifecycle.setEnabled(false);
    lifecycle.setEnabled(true);
    lifecycle.setEnabled(false);
    require(starts == 1 && drains.size() == 1, "drain coalesces repeated desired-state changes");
    drains[0]();
    require(starts == 1 && stops == 1, "latest disabled state wins over earlier re-enable");
    lifecycle.setEnabled(true);
    lifecycle.setEnabled(false);
    drains[0]();
    require(stops == 1, "old completion cannot retire a different draining graph");
    lifecycle.setEnabled(true);
    lifecycle.shutdown();
    drains[1]();
    require(starts == 2 && stops == 2, "terminal shutdown invalidates pending drain and restart");
    lifecycle.shutdown();
    require(stops == 2, "terminal teardown is idempotent");

    int immediateStarts = 0;
    int immediateStops = 0;
    McpRuntimeLifecycle immediate({[&] {
                                       ++immediateStarts;
                                       return McpRuntimeLifecycle::StartOutcome{true, {}};
                                   },
                                   [](auto done) { done(); }, [&] { ++immediateStops; }});
    immediate.setEnabled(true);
    immediate.setEnabled(false);
    immediate.setEnabled(true);
    require(immediateStarts == 2 && immediateStops == 1,
            "inline empty-drain completion leaves a restartable stopped graph");
}

void verifyLifecycleStartOutcomes() {
    using Lifecycle = snow_shot::app::mcp::McpRuntimeLifecycle;
    int starts = 0;
    int stops = 0;
    bool available = false;
    Lifecycle lifecycle({[&] {
                             ++starts;
                             return Lifecycle::StartOutcome{available, QStringLiteral("locked")};
                         },
                         [](auto done) { done(); }, [&] { ++stops; }});
    lifecycle.setEnabled(true);
    require(lifecycle.state() == Lifecycle::State::Failed &&
                lifecycle.error() == QStringLiteral("locked") && starts == 1 && stops == 1,
            "failed construction reports unavailability and retires its partial graph once");
    lifecycle.setEnabled(true);
    require(starts == 1 && stops == 1,
            "unchanged desired state cannot spin on a failed synchronous start");
    lifecycle.retry();
    require(starts == 2 && stops == 2 && lifecycle.state() == Lifecycle::State::Failed,
            "explicit retry can fail without leaking a partial graph");
    available = true;
    lifecycle.retry();
    require(starts == 3 && stops == 2 && lifecycle.state() == Lifecycle::State::Running &&
                lifecycle.error().isEmpty(),
            "successful retry publishes running only after actual availability");
    lifecycle.setEnabled(false);
    lifecycle.retry();
    require(starts == 3 && stops == 3 && lifecycle.state() == Lifecycle::State::Stopped,
            "retry cannot reopen disabled admission");
    available = false;
    lifecycle.setEnabled(true);
    lifecycle.setSuspended(true);
    lifecycle.retry();
    require(starts == 4 && stops == 4, "retry while suspended cannot construct a graph");
    lifecycle.setEnabled(false);
    lifecycle.setEnabled(true);
    available = true;
    lifecycle.setSuspended(false);
    require(starts == 5 && lifecycle.state() == Lifecycle::State::Running,
            "resume reconciles the latest desire after retiring a failed graph");
    lifecycle.shutdown();
    lifecycle.retry();
    lifecycle.setEnabled(true);
    require(starts == 5 && stops == 5, "terminal failure recovery cannot reopen admission");

    int exceptionStops = 0;
    Lifecycle throwing({[]() -> Lifecycle::StartOutcome { throw std::runtime_error("startup"); },
                        [](auto done) { done(); }, [&] { ++exceptionStops; }});
    throwing.setEnabled(true);
    require(throwing.state() == Lifecycle::State::Failed && exceptionStops == 1,
            "throwing construction also retires acquired resources exactly once");
    throwing.shutdown();
    require(exceptionStops == 1, "shutdown does not retire an already failed graph twice");

    bool constructing = false;
    int suspendedStops = 0;
    std::unique_ptr<Lifecycle> suspended;
    suspended = std::make_unique<Lifecycle>(Lifecycle::Hooks{
        [&] {
            constructing = true;
            require(suspended->state() == Lifecycle::State::Starting,
                    "availability remains pending while synchronous construction runs");
            suspended->setEnabled(false);
            suspended->setEnabled(true);
            suspended->setSuspended(true);
            require(suspendedStops == 0,
                    "reentrant suspension cannot destroy a graph still being constructed");
            constructing = false;
            return Lifecycle::StartOutcome{true, {}};
        },
        [](auto done) { done(); },
        [&] {
            require(!constructing, "retirement waits until synchronous start returns");
            ++suspendedStops;
        }});
    suspended->setEnabled(true);
    require(suspendedStops == 1 && suspended->state() == Lifecycle::State::Stopped,
            "suspension during construction leaves no active graph after start returns");

    std::unique_ptr<Lifecycle> retiring;
    int retiringStops = 0;
    retiring = std::make_unique<Lifecycle>(
        Lifecycle::Hooks{[&] {
                             retiring.reset();
                             return Lifecycle::StartOutcome{true, {}};
                         },
                         [](auto done) { done(); }, [&] { ++retiringStops; }});
    retiring->setEnabled(true);
    require(!retiring && retiringStops == 1,
            "destruction inside start retires resources without dereferencing an expired owner");
}

void verifyLifecycleReplacementFailure() {
    using Lifecycle = snow_shot::app::mcp::McpRuntimeLifecycle;
    int starts = 0;
    int stops = 0;
    bool available = true;
    std::vector<std::function<void()>> drains;
    Lifecycle lifecycle({[&] {
                             ++starts;
                             return Lifecycle::StartOutcome{available, QStringLiteral("locked")};
                         },
                         [&](auto done) { drains.push_back(std::move(done)); }, [&] { ++stops; }});
    lifecycle.setEnabled(true);
    lifecycle.setEnabled(false);
    lifecycle.setEnabled(true);
    available = false;
    drains[0]();
    require(lifecycle.state() == Lifecycle::State::Failed && starts == 2 && stops == 2 &&
                lifecycle.error() == QStringLiteral("locked"),
            "unavailable replacement retires both graphs and preserves the startup failure");
    drains[0]();
    lifecycle.setEnabled(true);
    require(starts == 2 && stops == 2 && lifecycle.state() == Lifecycle::State::Failed,
            "old drain and repeated desired enable cannot retry a failed replacement");
    available = true;
    lifecycle.retry();
    require(starts == 3 && stops == 2 && lifecycle.state() == Lifecycle::State::Running,
            "explicit retry opens a new graph only after failed replacement cleanup");
    drains[0]();
    require(starts == 3 && stops == 2 && lifecycle.state() == Lifecycle::State::Running,
            "old drain cannot retire the successfully retried graph");
}

void verifyLifecycleReentrantRetirement() {
    using Lifecycle = snow_shot::app::mcp::McpRuntimeLifecycle;
    for (const bool enabledAfterPause : {false, true}) {
        int starts = 0;
        int stops = 0;
        std::vector<std::function<void()>> drains;
        std::unique_ptr<Lifecycle> lifecycle;
        lifecycle = std::make_unique<Lifecycle>(Lifecycle::Hooks{
            [&] {
                ++starts;
                return Lifecycle::StartOutcome{true, {}};
            },
            [&](auto done) { drains.push_back(std::move(done)); },
            [&] {
                ++stops;
                if (stops != 1)
                    return;
                // Native cancellation/checkpoint observers may change configuration and
                // pause the application while the draining graph is being retired.
                lifecycle->setEnabled(true);
                lifecycle->setSuspended(true);
                lifecycle->setEnabled(enabledAfterPause);
                require(starts == 1 && lifecycle->state() == Lifecycle::State::Stopping,
                        "retirement observers cannot open a replacement before stop returns");
            }});
        lifecycle->setEnabled(true);
        lifecycle->setEnabled(false);
        drains[0]();
        require(starts == 1 && stops == 1 && lifecycle->state() == Lifecycle::State::Stopped,
                "pause raised by a retirement observer leaves the graph synchronously stopped");
        drains[0]();
        require(starts == 1 && stops == 1,
                "duplicate drain remains inert after reentrant pause changed its generation");
        lifecycle->setSuspended(false);
        require(starts == (enabledAfterPause ? 2 : 1) &&
                    lifecycle->state() ==
                        (enabledAfterPause ? Lifecycle::State::Running : Lifecycle::State::Stopped),
                "resume reconciles configuration changed inside resource retirement");
    }
}
} // namespace
int main() {
    using namespace snow_shot::app;
    ApplicationInputGuard input(nullptr);
    input.active = true;
    QEvent quit(QEvent::Quit);
    require(input.eventFilter(nullptr, &quit) && !quit.isAccepted(),
            "reversible pause blocks ordinary quit before helper commitment");
    input.active = false;
    require(!input.eventFilter(nullptr, &quit),
            "committed handoff must retire the input filter before terminal Quit");
    std::vector<int> released;
    {
        QuiescenceSession pause;
        pause.acquired([&] { released.push_back(1); });
        pause.acquired([&] { released.push_back(2); });
        pause.restore();
        pause.restore();
    }
    require(released == std::vector<int>({2, 1}),
            "partial preparation releases acquired participants in reverse order exactly once");
    {
        QuiescenceSession pause;
        pause.acquired([&] { released.push_back(3); });
        pause.commit();
    }
    require(released.size() == 2, "committed handoff never reopens admission during destruction");
    verifyAdmissionMatrix();
    verifyLifecycleInterleavings();
    verifyLifecycleStartOutcomes();
    verifyLifecycleReplacementFailure();
    verifyLifecycleReentrantRetirement();
    int starts = 0;
    int stops = 0;
    std::vector<std::function<void()>> drains;
    mcp::McpRuntimeLifecycle lifecycle({[&] {
                                            ++starts;
                                            return mcp::McpRuntimeLifecycle::StartOutcome{true, {}};
                                        },
                                        [&](auto done) { drains.push_back(std::move(done)); },
                                        [&] { ++stops; }});
    lifecycle.setEnabled(true);
    lifecycle.setEnabled(false);
    lifecycle.setEnabled(true);
    require(starts == 1 && stops == 0 && drains.size() == 1,
            "re-enable waits for admission-closed draining graph");
    drains[0]();
    require(starts == 2 && stops == 1, "completed drain retires old graph before fresh start");
    drains[0]();
    require(starts == 2 && stops == 1, "duplicate completion cannot stop replacement graph");
    lifecycle.setEnabled(false);
    lifecycle.setSuspended(true);
    require(stops == 2, "suspension synchronously stops draining resources");
    lifecycle.setEnabled(true);
    drains[1]();
    require(starts == 2, "stale drain cannot restart while suspended");
    lifecycle.setSuspended(false);
    require(starts == 3, "resume uses current enabled state");
    lifecycle.setSuspended(true);
    lifecycle.setEnabled(false);
    lifecycle.setSuspended(false);
    require(starts == 3, "disabled while paused stays stopped after resume");
    lifecycle.setEnabled(true);
    lifecycle.shutdown();
    lifecycle.setEnabled(true);
    lifecycle.setSuspended(false);
    require(starts == 4 && stops == 4, "terminal shutdown cannot be undone by configuration");
    {
        mcp::McpRuntimeLifecycle temporary(
            {[] { return mcp::McpRuntimeLifecycle::StartOutcome{true, {}}; },
             [&](auto done) { drains.push_back(done); }, [] {}});
        temporary.setEnabled(true);
        temporary.setEnabled(false);
    }
    drains.back()(); // callback remains harmless after its owner dies
    {
        bool stopping = false;
        int replacementStarts = 0;
        std::function<void()> drained;
        std::unique_ptr<mcp::McpRuntimeLifecycle> reentrant;
        reentrant = std::make_unique<mcp::McpRuntimeLifecycle>(mcp::McpRuntimeLifecycle::Hooks{
            [&] {
                require(!stopping, "replacement starts only after the old graph finishes stopping");
                ++replacementStarts;
                return mcp::McpRuntimeLifecycle::StartOutcome{true, {}};
            },
            [&](auto done) { drained = std::move(done); },
            [&] {
                stopping = true;
                const auto previousStarts = replacementStarts;
                reentrant->setEnabled(true);
                require(replacementStarts == previousStarts,
                        "stop callback cannot start a graph reentrantly");
                stopping = false;
            }});
        reentrant->setEnabled(true);
        reentrant->setEnabled(false);
        drained();
        require(replacementStarts == 2, "latest desired state starts after retirement completes");
        // Stop callbacks may also destroy the lifecycle. Its completion must not dereference it.
        std::unique_ptr<mcp::McpRuntimeLifecycle> retiring;
        retiring = std::make_unique<mcp::McpRuntimeLifecycle>(mcp::McpRuntimeLifecycle::Hooks{
            [] { return mcp::McpRuntimeLifecycle::StartOutcome{true, {}}; },
            [&](auto done) { drained = std::move(done); }, [&] { retiring.reset(); }});
        retiring->setEnabled(true);
        retiring->setEnabled(false);
        drained();
        require(!retiring, "destruction inside stop safely retires the lifecycle");
    }

    OperationFacts facts;
    facts.mcpBusy = true;
    require(inspectOperation(ApplicationOperation::Restart, facts).allowed() &&
                inspectOperation(ApplicationOperation::UpdateRequest, facts).allowed(),
            "existing restart and initial update policy do not add source/job vetoes");
    for (auto operation : {ApplicationOperation::UpdateHandoff, ApplicationOperation::Relocation,
                           ApplicationOperation::Store, ApplicationOperation::Trim})
        require(inspectOperation(operation, facts).contains(OperationBlocker::Mcp),
                "final handoff, relocation, Store and trim protect MCP work");
    facts = {};
    facts.unsavedEditors = true;
    require(!inspectOperation(ApplicationOperation::Restart, facts).allowed() &&
                !inspectOperation(ApplicationOperation::Relocation, facts).allowed() &&
                inspectOperation(ApplicationOperation::UpdateRequest, facts).allowed(),
            "restart snapshot veto remains distinct from asynchronous update editor review");
    facts = {};
    facts.captureInteracting = true;
    require(inspectOperation(ApplicationOperation::Restart, facts).allowed() &&
                !inspectOperation(ApplicationOperation::Store, facts).allowed() &&
                !inspectOperation(ApplicationOperation::Trim, facts).allowed(),
            "interaction policy is distinct from update blocking capture state");
    facts = {};
    require(inspectOperation(ApplicationOperation::UpdateHandoff, facts).allowed(),
            "initial admission can succeed");
    facts.directCaptureBusy = true;
    require(!inspectOperation(ApplicationOperation::UpdateHandoff, facts).allowed(),
            "final admission sees new work instead of retaining an earlier snapshot");
    facts = {};
    require(inspectOperation(ApplicationOperation::UpdateRequest, facts).allowed(),
            "initial update request may proceed without an editor owner");
    facts.unsavedEditors = true;
    require(inspectOperation(ApplicationOperation::UpdateHandoff, facts)
                .contains(OperationBlocker::Editors),
            "an editor created and dirtied during preparation vetoes final helper commitment");
    return EXIT_SUCCESS;
}
