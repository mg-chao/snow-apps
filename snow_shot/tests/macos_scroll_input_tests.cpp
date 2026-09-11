#include "../src/platform/macos/scrollinput_p.h"

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

namespace {
namespace native = snow_shot::platform::macos::detail;
using Result = native::ScrollInputResult;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
QRect selectionAt(QPoint center) {
    return {center - QPoint(10, 10), QSize(21, 21)};
}

void physicalCoordinatesUseTheCaptureDesktop() {
    const QVector<native::ScrollDisplay> displays{
        {{0, 0, 1800, 1169}, 2}, {{1800, 0, 1920, 1080}, 1}, {{-656, -1800, 3200, 1800}, 2}};
    require(native::logicalScrollPosition({200, 300}, displays) == QPointF(100, 150),
            "Retina positions must convert from capture pixels to desktop points");
    require(native::logicalScrollPosition({3610, 120}, displays) == QPointF(1810, 120),
            "mixed DPI must use the shared desktop origin and the target's local backing scale");
    require(native::logicalScrollPosition({-200, -100}, displays) == QPointF(-100, -50),
            "negative display origins must retain the capture coordinate mapping");
    require(!native::logicalScrollPosition({5600, 100}, displays),
            "a point outside every physical display must not scroll an arbitrary window");
    require(!native::logicalScrollPosition({3600, 1100}, displays),
            "mixed-DPI gaps must not be mistaken for valid desktop coordinates");
}

void requestsTargetExternalWindowsAndReportFailures() {
    int permissionChecks = 0;
    int posts = 0;
    bool permission = true;
    QVector<native::ScrollDisplay> displays{{{0, 0, 1800, 1169}, 2}};
    QVector<native::ScrollWindow> windows{
        {10, 100, {0, 0, 1000, 1000}, 1, 0},     // Our capture overlay.
        {20, 200, {0, 0, 1000, 1000}, 0, 0},     // Fully transparent.
        {30, 300, {0, 0, 1000, 1000}, 1, -1},    // Desktop surface.
        {40, 400, {200, 200, 1000, 1000}, 1, 0}, // Outside the selection.
        {50, 500, {0, 0, 1000, 1000}, 1, 8},     // A visible external floating/dialog window.
        {60, 600, {0, 0, 1000, 1000}, 1, 0}};
    Result posted{Result::Status::Posted, 0};
    native::ScrollRequest captured;
    const native::ScrollNativeApi api{10,
                                      [&] {
                                          ++permissionChecks;
                                          return permission;
                                      },
                                      [&] { return displays; }, [&] { return windows; },
                                      [&](const native::ScrollRequest& request) {
                                          ++posts;
                                          captured = request;
                                          return posted;
                                      }};
    require(native::dispatchScrollingWheelStep({}, {0, -120}, api).status ==
                    Result::Status::InvalidRequest &&
                permissionChecks == 0 && posts == 0,
            "invalid geometry must not inspect permission or dispatch events");
    permission = false;
    require(native::dispatchScrollingWheelStep(selectionAt({200, 300}), {0, -120}, api).status ==
                    Result::Status::PermissionDenied &&
                posts == 0,
            "denied accessibility must not silently succeed or post input");
    permission = true;
    require(native::dispatchScrollingWheelStep(selectionAt({200, 300}), {0, -120}, api).status ==
                    Result::Status::Posted &&
                posts == 1 && captured.process == 50 && captured.window == 500 &&
                captured.position == QPointF(100, 150) && captured.lines == QPoint(0, -3),
            "vertical scrolling must target the front external window beneath the capture overlay");
    require(native::dispatchScrollingWheelStep(selectionAt({200, 300}), {120, 0}, api).status ==
                    Result::Status::Posted &&
                captured.lines == QPoint(-3, 0),
            "rightward Windows wheel deltas must become negative Quartz horizontal deltas");
    posted = {Result::Status::PostFailed, 7};
    const auto failed = native::dispatchScrollingWheelStep(selectionAt({200, 300}), {0, -120}, api);
    require(failed.status == posted.status && failed.error == 7,
            "native posting failures must preserve their error code for the caller");
    const int beforeInvalid = posts;
    windows.clear();
    require(native::dispatchScrollingWheelStep(selectionAt({200, 300}), {0, -120}, api).status ==
                    Result::Status::TargetNotFound &&
                posts == beforeInvalid,
            "a missing target must not post to the foreground app as a fallback");
    displays.clear();
    require(native::dispatchScrollingWheelStep(selectionAt({200, 300}), {0, -120}, api).status ==
                    Result::Status::CoordinateFailure &&
                posts == beforeInvalid,
            "missing display geometry must prevent unsafe coordinate guesses");
}

void nativeEventsRetainPositionDirectionAndSyntheticProvenance() {
    for (const QPoint lines : {QPoint(0, -3), QPoint(-3, 0)}) {
        const native::ScrollRequest request{123, 456, {-120.5, -65.5}, lines};
        const auto event = native::createScrollEvent(request);
        require(event != nullptr, "native scroll events must be constructible without posting");
        const CGPoint point = CGEventGetLocation(event);
        require(CGEventGetType(event) == kCGEventScrollWheel && point.x == request.position.x() &&
                    point.y == request.position.y() && CGEventGetFlags(event) == 0,
                "a wheel event must carry the target's position without inherited modifiers");
        require(CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1) == lines.y() &&
                    CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2) ==
                        lines.x() &&
                    CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous) == 0,
                "native wheel fields must retain the selected axis and line direction");
        require(CGEventGetIntegerValueField(event, kCGEventTargetUnixProcessID) ==
                        request.process &&
                    CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == getpid(),
                "targeted events must retain their process and synthetic provenance");
        CFRelease(event);
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    physicalCoordinatesUseTheCaptureDesktop();
    requestsTargetExternalWindowsAndReportFailures();
    nativeEventsRetainPositionDirectionAndSyntheticProvenance();
    std::cout << "macOS auto-scroll mapping, targeting, failures and native event fields passed\n";
}
