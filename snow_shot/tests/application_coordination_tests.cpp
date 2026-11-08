#include "snow_shot/app/mcp/mcpruntimelifecycle.h"
#include "snow_shot/app/operationadmission.h"
#include "snow_shot/app/quiescencesession.h"
#include "snow_shot/app/applicationinputguard.h"

#include <cstdlib>
#include <array>
#include <iostream>
#include <vector>
#include <memory>

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
    McpRuntimeLifecycle lifecycle({[&] { ++starts; },
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
    McpRuntimeLifecycle immediate(
        {[&] { ++immediateStarts; }, [](auto done) { done(); }, [&] { ++immediateStops; }});
    immediate.setEnabled(true);
    immediate.setEnabled(false);
    immediate.setEnabled(true);
    require(immediateStarts == 2 && immediateStops == 1,
            "inline empty-drain completion leaves a restartable stopped graph");
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
    int starts = 0;
    int stops = 0;
    std::vector<std::function<void()>> drains;
    mcp::McpRuntimeLifecycle lifecycle({[&] { ++starts; },
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
            {[] {}, [&](auto done) { drains.push_back(done); }, [] {}});
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
            [] {}, [&](auto done) { drained = std::move(done); }, [&] { retiring.reset(); }});
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
