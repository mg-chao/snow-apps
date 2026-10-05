#include "../src/presentation/recording/recordingpopupexclusions.h"
#include "widgets/popover.h"
#include "widgets/popup_surface_guard.h"

#include <QApplication>
#include <QHash>
#include <QPushButton>
#include <cstdlib>
#include <iostream>

namespace {
using snow_shot::presentation::RecordingPopupExclusions;
using Completion = RecordingPopupExclusions::Completion;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void acknowledgmentsFollowNativeIdentities() {
    RecordingPopupExclusions state;
    std::uint64_t generation = 0;
    QVector<std::uint32_t> requested;
    const auto request = [&](const auto& windows) {
        requested = windows;
        return ++generation;
    };
    require(!state.canShow(10, {1, 10}, request), "first surface awaits a filter acknowledgment");
    require(state.complete(1, 0, 1) == Completion::Pending, "pending status grants no approval");
    require(state.complete(1, 1, 0) == Completion::Applied, "matching generation is acknowledged");
    require(state.canShow(10, {10, 1, 10}, request) && generation == 1,
            "snapshot order and duplicates do not create another request");

    require(!state.canShow(20, {1, 10, 20}, request), "new child identity waits");
    require(state.canShow(10, {1, 10, 20}, request) && generation == 2,
            "pending child neither hides its approved parent nor resubmits the filter");
    require(state.complete(2, 1, 2) == Completion::Failed, "failed child remains unapproved");
    require(state.canShow(10, {1, 10, 20}, request) && !state.canShow(20, {1, 10, 20}, request) &&
                generation == 2,
            "failed expansion preserves parent approval and does not retry on layout");
    state.retry();
    require(!state.canShow(20, {1, 10, 20}, request) && generation == 3,
            "an explicit new open request retries failure");
    require(state.complete(3, 3, 0) == Completion::Applied &&
                state.canShow(20, {1, 10, 20}, request),
            "child opens after successful retry");

    require(!state.canShow(21, {1, 10, 21}, request) && generation == 4,
            "replacement native identity cannot inherit the old identity's approval");
    require(state.canShow(10, {1, 10, 21}, request), "native replacement preserves parent");
    require(!state.canShow(22, {1, 10, 22}, request) && generation == 5,
            "another replacement supersedes the pending generation");
    require(state.complete(4, 4, 0) == Completion::Pending &&
                !state.canShow(22, {1, 10, 22}, request),
            "late acknowledgment of the obsolete native identity is ignored");
    require(state.complete(5, 4, 0) == Completion::Failed,
            "success without the matching applied generation grants no approval");

    state.retry();
    require(!state.canShow(22, {1, 10, 22}, request), "replacement can be retried");
    require(state.complete(6, 6, 0) == Completion::Applied, "replacement acknowledged");
    require(state.canShow(10, {1, 10}, request) && requested == QVector<std::uint32_t>({1, 10}),
            "destroyed windows are removed from future filters without hiding live parents");
    state.complete(7, 7, 0);
    require(!state.canShow(22, {1, 10, 22}, request),
            "an identity removed from the applied filter needs a new acknowledgment");

    state = {};
    require(state.complete(8, 8, 0) == Completion::Pending && !state.pending(),
            "completion after session teardown is ignored");
    require(!state.canShow(10, {1, 10}, request), "new session inherits no approvals");
}

void rejectedReplacementPreservesAcceptedRequest() {
    RecordingPopupExclusions state;
    int requests = 0;
    const auto request = [&](const auto&) -> std::uint64_t { return ++requests == 1 ? 1 : 0; };
    require(!state.canShow(10, {1, 10}, request), "first request is accepted asynchronously");
    require(!state.canShow(20, {1, 10, 20}, request) && state.pending(),
            "rejected expansion must not abandon the accepted generation");
    require(state.complete(1, 1, 0) == Completion::Applied,
            "accepted generation can still complete after rejection");
    require(state.canShow(10, {1, 10, 20}, request) && !state.canShow(20, {1, 10, 20}, request) &&
                requests == 2,
            "rejection does not revoke approval or continuously resubmit a failed snapshot");

    state = {};
    requests = 0;
    state.canShow(10, {1, 10}, request);
    state.canShow(20, {1, 10, 20}, request);
    state.complete(1, 0, 2);
    require(!state.canShow(10, {1, 10}, request) && !state.canShow(20, {1, 10, 20}, request) &&
                requests == 2,
            "failure of the older accepted request does not forget a rejected replacement");
}

void destroyedPendingWindowIsRemovedFromSubmittedFilter() {
    for (const bool fail : {false, true}) {
        RecordingPopupExclusions state;
        std::uint64_t generation = 0;
        QVector<std::uint32_t> submitted;
        const auto request = [&](const auto& windows) {
            submitted = windows;
            return ++generation;
        };
        state.canShow(10, {1, 10}, request);
        state.complete(1, 1, 0);
        state.canShow(20, {1, 10, 20}, request);
        if (fail)
            state.complete(2, 1, 2);
        require(state.canShow(10, {1, 10}, request) && generation == 3 &&
                    submitted == QVector<std::uint32_t>({1, 10}),
                "removing a pending or failed child reconciles the possibly applied native filter");
        require(state.complete(2, 2, 0) == Completion::Pending,
                "obsolete expansion cannot replace the removal acknowledgment");
        require(state.complete(3, 3, 0) == Completion::Applied,
                "removal acknowledges only the surviving native identities");
    }
}

void submittedRemovalRevokesObsoleteApproval() {
    RecordingPopupExclusions state;
    std::uint64_t generation = 0;
    const auto request = [&](const auto&) { return ++generation; };
    state.canShow(10, {1, 10, 20}, request);
    state.complete(1, 1, 0);
    require(state.canShow(10, {1, 10}, request), "removal keeps the surviving parent approved");
    require(!state.canShow(20, {1, 10, 20}, request),
            "identity reintroduced during a removal waits because that removal can still apply");
    require(state.complete(2, 2, 0) == Completion::Pending,
            "removal completion cannot approve the reintroduced identity");
    require(state.complete(3, 3, 0) == Completion::Applied &&
                state.canShow(20, {1, 10, 20}, request),
            "reintroduced identity opens only after the restoring filter is acknowledged");
}

void pendingNestedPopupPreservesParent() {
    using adqt::widgets::AdPopover;
    QWidget host;
    QPushButton trigger(&host);
    host.resize(400, 260);
    trigger.setGeometry(80, 60, 80, 30);
    host.show();
    QCoreApplication::processEvents();

    RecordingPopupExclusions state;
    std::uint64_t generation = 0;
    QHash<QWidget*, std::uint32_t> identities;
    QVector<std::uint32_t> windows{1};
    const auto request = [&](const auto&) { return ++generation; };
    adqt::widgets::AdPopupSurfaceGuard guard(&host, [&](QWidget* surface) {
        if (!identities.contains(surface)) {
            const auto id = static_cast<std::uint32_t>(identities.size() + 10);
            identities.insert(surface, id);
            windows.push_back(id);
        }
        return state.canShow(identities.value(surface), windows, request);
    });
    QObject::connect(&guard, &adqt::widgets::AdPopupSurfaceGuard::surfaceRequested,
                     [&state](QWidget*) { state.retry(); });
    AdPopover parent;
    parent.setSourceWidget(&trigger);
    parent.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    parent.setTriggers({});
    auto* nestedTrigger = new QPushButton(QStringLiteral("Nested"));
    parent.setContentWidget(nestedTrigger);
    parent.show();
    require(!parent.surfaceWidget()->isVisible(), "initial parent awaits acknowledgment");
    state.complete(1, 1, 0);
    guard.refresh();
    require(parent.surfaceWidget()->isVisible(), "acknowledged parent becomes visible");

    AdPopover child;
    child.setSourceWidget(nestedTrigger);
    child.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    child.setTriggers({});
    child.setText(QStringLiteral("Child"));
    child.show();
    parent.refreshPopupLayout();
    guard.refresh();
    QCoreApplication::processEvents();
    require(parent.surfaceWidget()->isVisible() && child.isVisible() &&
                !child.surfaceWidget()->isVisible() && generation == 2,
            "parent relayout preserves the pending child request and approved parent surface");
    state.complete(2, 1, 2);
    guard.refresh();
    require(parent.surfaceWidget()->isVisible() && child.isVisible() &&
                !child.surfaceWidget()->isVisible() && generation == 2,
            "failed child filter leaves the parent usable without a retry loop");
    child.hide();
    child.show();
    require(generation == 3, "reopening the failed child retries exclusion");
    state.complete(3, 3, 0);
    guard.refresh();
    require(parent.surfaceWidget()->isVisible() && child.surfaceWidget()->isVisible(),
            "acknowledged nested popup opens without reopening its parent");

    identities[child.surfaceWidget()] = 30;
    windows = {1, 10, 30};
    QEvent replacement(QEvent::WinIdChange);
    QApplication::sendEvent(child.surfaceWidget(), &replacement);
    guard.refresh();
    require(parent.surfaceWidget()->isVisible() && child.isVisible() &&
                !child.surfaceWidget()->isVisible() && generation == 4,
            "native replacement hides only the unapproved child identity");
    child.hide();
    state.complete(4, 4, 0);
    guard.refresh();
    require(parent.surfaceWidget()->isVisible() && !child.isVisible() &&
                !child.surfaceWidget()->isVisible(),
            "acknowledgment cannot reopen a cancelled child");
    parent.hide();
}
} // namespace

void recordingPopupExclusionTests() {
    acknowledgmentsFollowNativeIdentities();
    rejectedReplacementPreservesAcceptedRequest();
    destroyedPendingWindowIsRemovedFromSubmittedFilter();
    submittedRemovalRevokesObsoleteApproval();
    pendingNestedPopupPreservesParent();
}
