#include "snow_shot/presentation/screenshotsmartselectiontransition.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void waitForAnimation() {
    QEventLoop loop;
    QTimer::singleShot(ScreenshotSmartSelectionTransition::kDurationMs + 100, &loop,
                       &QEventLoop::quit);
    loop.exec();
}

void firstSmartSelectionIsPresentedDirectly() {
    QRectF presented;
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF& selection) {
        presented = selection;
        ++updateCount;
    });
    const QRectF first(10.0, 20.0, 300.0, 200.0);

    static_cast<void>(transition.update(first, true));

    require(updateCount == 1, "first smart selection must update immediately");
    require(presented == first, "first smart selection must not be interpolated");
    require(!transition.isRunning(), "first smart selection must not animate");
}

void subsequentSmartSelectionUsesConfiguredTransition() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF second(110.0, 70.0, 500.0, 400.0);

    static_cast<void>(transition.update(first, true));
    static_cast<void>(transition.update(second, true));

    require(ScreenshotSmartSelectionTransition::kDurationMs == 101,
            "smart selection transition duration must be 101 ms");
    require(ScreenshotSmartSelectionTransition::kEasingCurve == QEasingCurve::OutQuad,
            "smart selection transition must use quadratic ease-out");
    require(transition.isRunning(), "subsequent smart selection must animate");
    require(transition.displayedSelection() != second,
            "subsequent smart selection must not jump directly to its target");

    waitForAnimation();

    require(!transition.isRunning(), "smart selection transition must finish");
    require(presented == second, "smart selection transition must end at its target");
}

void seededSmartSelectionDoesNotEmitAndNextChangeAnimates() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF initial(10, 20, 80, 60);
    const QRectF next(20, 30, 90, 70);

    transition.seed(initial, true);
    require(updateCount == 0 && transition.displayedSelection() == initial &&
                !transition.isRunning(),
            "seeding the hidden initial frame must not emit or animate");
    require(transition.update(next, true) && transition.isRunning(),
            "the first post-presentation selection change must animate from the seed");
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
    static_cast<void>(transition.update(first, true));
    static_cast<void>(transition.update(second, true));

    require(!transition.enabled(), "disabled smart selection transition must remain disabled");
    require(!transition.isRunning(), "disabled smart selection transition must not animate");
    require(transition.displayedSelection() == second,
            "disabled smart selection transition must display the latest target directly");
    require(presented == second,
            "disabled smart selection transition must present the latest target directly");
    require(updateCount == 2,
            "disabled smart selection transition must emit each changed selection directly");
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

    static_cast<void>(transition.update(first, true));
    static_cast<void>(transition.update(second, true));

    require(transition.isRunning(), "smart selection transition must be running before disable");
    require(transition.displayedSelection() != second,
            "running smart selection transition must not already display its target");

    transition.setEnabled(false);
    const int updateCountAfterDisable = updateCount;

    require(!transition.enabled(), "running smart selection transition must become disabled");
    require(!transition.isRunning(), "disabling a running transition must stop its animation");
    require(transition.displayedSelection() == second,
            "disabling a running transition must display its target immediately");
    require(presented == second,
            "disabling a running transition must present its target immediately");

    waitForAnimation();

    require(updateCount == updateCountAfterDisable,
            "disabled transition must not emit stale animation updates");
    require(presented == second, "disabled transition must keep presenting its target");
}

void leavingSmartFramingResetsTheFirstResultRule() {
    QRectF presented;
    ScreenshotSmartSelectionTransition transition(
        [&](const QRectF& selection) { presented = selection; });
    const QRectF first(10.0, 20.0, 300.0, 200.0);
    const QRectF manual(30.0, 40.0, 100.0, 80.0);
    const QRectF nextFirst(400.0, 300.0, 250.0, 180.0);

    static_cast<void>(transition.update(first, true));
    static_cast<void>(transition.update(manual, false));
    static_cast<void>(transition.update(nextFirst, true));

    require(!transition.isRunning(), "first result after re-entry must not animate");
    require(presented == nextFirst, "first result after re-entry must be direct");
}

void unchangedSmartSelectionDoesNotEmitAnAnimationUpdate() {
    int updateCount = 0;
    ScreenshotSmartSelectionTransition transition([&](const QRectF&) { ++updateCount; });
    const QRectF selection(10.0, 20.0, 300.0, 200.0);

    const bool firstChanged = transition.update(selection, true);
    const bool secondChanged = transition.update(selection, true);

    require(firstChanged, "first smart selection must be reported as changed");
    require(!secondChanged, "unchanged smart selection must not restart animation");
    require(updateCount == 1, "unchanged smart selection must not emit an update");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    firstSmartSelectionIsPresentedDirectly();
    seededSmartSelectionDoesNotEmitAndNextChangeAnimates();
    subsequentSmartSelectionUsesConfiguredTransition();
    disabledTransitionPresentsSmartSelectionsDirectly();
    disablingRunningTransitionPresentsItsTargetDirectly();
    leavingSmartFramingResetsTheFirstResultRule();
    unchangedSmartSelectionDoesNotEmitAnAnimationUpdate();
    return 0;
}
