#include "../src/presentation/recording/screenshotrecordingworkflow.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void recordingHandoffStopsCaptureBeforeOpening(bool scrolling) {
    QObject owner;
    QStringList events;
    QRect selected(40, 60, 320, 240);
    QRect recorded;
    snow_shot::presentation::recording::startScreenshotRecording(
        selected, QPoint(-1920, 100),
        {owner, []() { return true; },
         [&]() {
             scrolling = false;
             events << QStringLiteral("stop scrolling");
         },
         [&]() { events << QStringLiteral("reset editing"); },
         [&]() { events << QStringLiteral("invalidate recognition"); },
         [&]() {
             require(!scrolling, "scrolling must stop before cancelling screenshot capture");
             selected = {};
             events << QStringLiteral("cancel capture");
         },
         [&]() { events << QStringLiteral("reset history"); },
         [&](const QRect& region) {
             recorded = region;
             events << QStringLiteral("open recording");
         }});
    require(!scrolling, "Screen Recording must leave active scrolling capture");
    require(recorded.isEmpty(), "recording should open after screenshot cleanup returns");
    QCoreApplication::processEvents();
    require(recorded == QRect(-1880, 160, 320, 240),
            "recording must retain the original physical selection after capture cleanup");
    require(events == QStringList{QStringLiteral("stop scrolling"), QStringLiteral("reset editing"),
                                  QStringLiteral("invalidate recognition"),
                                  QStringLiteral("cancel capture"), QStringLiteral("reset history"),
                                  QStringLiteral("open recording")},
            "recording handoff must stop scrolling and clean up capture before opening");
}

void rejectedRecordingPreservesCapture(const QRect& selection, bool recordingAvailable) {
    QObject owner;
    int mutations = 0;
    auto mutate = [&]() { ++mutations; };
    snow_shot::presentation::recording::startScreenshotRecording(
        selection, QPoint(),
        {owner, [&]() { return recordingAvailable; }, mutate, mutate, mutate, mutate, mutate,
         [&](const QRect&) { ++mutations; }});
    QCoreApplication::processEvents();
    require(mutations == 0, "rejected recording must preserve the current capture session");
}

void destroyedOwnerCancelsPendingRecording() {
    int opened = 0;
    {
        QObject owner;
        auto noOp = []() {};
        snow_shot::presentation::recording::startScreenshotRecording(
            QRect(10, 20, 320, 240), QPoint(),
            {owner, []() { return true; }, noOp, noOp, noOp, noOp, noOp,
             [&](const QRect&) { ++opened; }});
    }
    QCoreApplication::processEvents();
    require(opened == 0, "destroying the screenshot controller must cancel pending recording");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    recordingHandoffStopsCaptureBeforeOpening(true);
    recordingHandoffStopsCaptureBeforeOpening(false);
    rejectedRecordingPreservesCapture(QRect(), true);
    rejectedRecordingPreservesCapture(QRect(0, 0, 1, 240), true);
    rejectedRecordingPreservesCapture(QRect(0, 0, 320, 1), true);
    rejectedRecordingPreservesCapture(QRect(0, 0, 320, 240), false);
    destroyedOwnerCancelsPendingRecording();
    return 0;
}
