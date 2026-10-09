#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearRect(const QRectF& actual, const QRectF& expected) {
    constexpr qreal tolerance = 0.000001;
    return std::abs(actual.x() - expected.x()) < tolerance &&
           std::abs(actual.y() - expected.y()) < tolerance &&
           std::abs(actual.width() - expected.width()) < tolerance &&
           std::abs(actual.height() - expected.height()) < tolerance;
}

QRectF interpolated(const QRectF& start, const QRectF& target, qreal progress) {
    const qreal eased = 1.0 - (1.0 - progress) * (1.0 - progress);
    return QRectF(start.x() + (target.x() - start.x()) * eased,
                  start.y() + (target.y() - start.y()) * eased,
                  start.width() + (target.width() - start.width()) * eased,
                  start.height() + (target.height() - start.height()) * eased);
}

void firstSmartSelectionIsPresentedDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);

    require(transition.update(first, true, 1000), "first smart selection must change presentation");
    require(updateCount == 1, "first smart selection must update immediately");
    require(presented == first, "first smart selection must not be interpolated");
    require(!transition.isRunning(), "first smart selection must not animate");
    require(!transition.advance(2000), "direct selection must not produce later frames");
}

void framesUseAbsoluteTimeAndConfiguredTransition() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true, 1000));
    require(transition.update(second, true, 1100), "new smart target must start a transition");
    require(ScreenshotSmartSelectionTransition::kDurationMs == 101,
            "smart selection transition duration must be 101 ms");
    require(ScreenshotSmartSelectionTransition::kEasingCurve == QEasingCurve::OutQuad,
            "smart selection transition must use quadratic ease-out");
    require(transition.isRunning(), "subsequent smart selection must animate");
    require(presented == first, "transition must begin at the displayed rectangle");
    require(!transition.advance(1100), "transition start must not repeat the previous frame");

    // The presentation scheduler may supply frames more frequently than Qt's former 16 ms timer.
    require(transition.advance(1108), "an eight-millisecond frame must advance presentation");
    require(nearRect(presented, interpolated(first, second, 8.0 / 101.0)),
            "each frame must use absolute elapsed time and quadratic easing");
    require(transition.advance(1116), "high-refresh frames must not wait for a second timer");
    require(nearRect(presented, interpolated(first, second, 16.0 / 101.0)),
            "high-refresh interpolation must preserve the configured duration");
    const int updatesAfterFrame = updateCount;
    require(!transition.advance(1116), "repeated frame timestamps must not emit an update");
    require(!transition.advance(1110), "older frame timestamps must not reverse the transition");
    require(updateCount == updatesAfterFrame, "repeated or older frames must leave callbacks idle");

    require(transition.advance(1201), "final frame must reach the target exactly");
    require(!transition.isRunning(), "smart selection transition must finish at its duration");
    require(presented == second, "smart selection transition must end at its exact target");
    require(!transition.advance(100000), "completed transition must not emit extra frames");
}

void retargetingSamplesTheCurrentTrajectory() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);
    const QRectF third(30.0, 200.0, 100.0, 80.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.update(third, true, 1150));
    const QRectF retargetStart = interpolated(first, second, 50.0 / 101.0);
    require(nearRect(presented, retargetStart),
            "retargeting must sample the preceding trajectory at the supplied timestamp");

    static_cast<void>(transition.advance(1200));
    require(nearRect(presented, interpolated(retargetStart, third, 50.0 / 101.0)),
            "retargeted animation must start from the interpolated current rectangle");
    static_cast<void>(transition.advance(100000));
    require(presented == third, "a delayed frame must finish at the latest target");
    require(!transition.isRunning(), "a delayed final frame must stop the transition");
}

void disabledTransitionPresentsSmartSelectionsDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    transition.setEnabled(false);
    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1008));

    require(!transition.enabled(), "disabled smart selection transition must remain disabled");
    require(!transition.isRunning(), "disabled smart selection transition must not animate");
    require(presented == second, "disabled transition must present the latest target directly");
    require(!transition.update(second, true, 1016),
            "unchanged disabled selection must not report a presentation change");
    require(updateCount == 2, "disabled transition must emit each changed selection once");
}

void disablingRunningTransitionPresentsItsTargetDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(second, true, 1100));
    static_cast<void>(transition.advance(1116));
    transition.setEnabled(false);
    const int updateCountAfterDisable = updateCount;

    require(!transition.enabled(), "running smart selection transition must become disabled");
    require(!transition.isRunning(), "disabling a running transition must stop its animation");
    require(presented == second,
            "disabling a running transition must immediately reach its target");
    require(!transition.advance(1201), "disabled transition must not produce stale frames");
    require(updateCount == updateCountAfterDisable,
            "disabled transition must not emit stale animation callbacks");
}

void leavingSmartFramingAndClearingResetTheFirstResultRule() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF target(110.0, 70.0, 500.0, 400.0);
    const QRectF manual(30.0, 40.0, 100.0, 80.0);
    const QRectF nextFirst(400.0, 300.0, 250.0, 180.0);

    static_cast<void>(transition.update(first, true, 1000));
    static_cast<void>(transition.update(target, true, 1100));
    static_cast<void>(transition.update(manual, false, 1116));
    require(presented == manual, "leaving smart framing must present manual geometry immediately");
    require(!transition.advance(1201), "leaving smart framing must cancel all animation frames");
    static_cast<void>(transition.update(nextFirst, true, 1300));
    require(!transition.isRunning(), "first result after re-entry must not animate");
    require(presented == nextFirst, "first result after re-entry must be direct");

    static_cast<void>(transition.update(target, true, 1400));
    static_cast<void>(transition.update({}, true, 1416));
    require(presented.isEmpty(), "clearing must present an empty selection immediately");
    require(!transition.advance(1501), "clearing must cancel all animation frames");
    static_cast<void>(transition.update(first, true, 1600));
    require(!transition.isRunning(), "first result after clearing must not animate");
    require(presented == first, "first result after clearing must be direct");
}

void unchangedTargetsAndDirectGeometryDoNotEmitUpdates() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    require(transition.update(first, true, 1000), "first smart selection must report a change");
    require(!transition.update(first, true, 1050), "unchanged smart target must not restart");
    require(updateCount == 1, "unchanged smart target must not emit an update");
    static_cast<void>(transition.update(second, true, 1100));
    require(!transition.update(second, true, 1150), "unchanged running target must not restart");
    require(updateCount == 1,
            "target delivery must not advance frames independently of presentation");
    require(transition.advance(1150), "the presentation frame must still advance a running target");
    require(!transition.update(transition.displayedSelection(), false, 1150),
            "unchanged manual geometry must not emit a redundant update");
    require(!transition.isRunning(), "manual geometry must stop a running transition");
    require(!transition.update(transition.displayedSelection(), false, 1200),
            "repeated manual geometry must remain unchanged");
    require(updateCount == 2, "direct geometry repeats must leave presentation callbacks idle");
}
} // namespace

int main() {
    firstSmartSelectionIsPresentedDirectly();
    framesUseAbsoluteTimeAndConfiguredTransition();
    retargetingSamplesTheCurrentTrajectory();
    disabledTransitionPresentsSmartSelectionsDirectly();
    disablingRunningTransitionPresentsItsTargetDirectly();
    leavingSmartFramingAndClearingResetTheFirstResultRule();
    unchangedTargetsAndDirectGeometryDoNotEmitUpdates();
    return 0;
}
