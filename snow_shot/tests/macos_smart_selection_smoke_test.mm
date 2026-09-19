#include "snow_ui_selector.h"
#include "snow_shot/platform/windowcaptureexclusion.h"
#include <QAccessible>
#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QScreen>
#include <QThread>
#include <QVBoxLayout>
#include <CoreGraphics/CoreGraphics.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

namespace {
struct State {
    std::atomic<bool> refreshed = false;
    std::atomic<bool> delivered = false;
    std::mutex mutex;
    bool ok = false;
    SnowUiSelectorStopReason reason = SNOW_UI_SELECTOR_PROVIDER_FAILURE;
    std::vector<SnowUiSelectorRect> rects;
};
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void event(const SnowUiSelectorEvent* e, void* context) {
    auto& state = *static_cast<State*>(context);
    std::lock_guard lock(state.mutex);
    state.ok = e->ok != 0;
    state.reason = e->reason;
    state.rects.assign(e->rects, e->rects + e->count);
    state.delivered = true;
}
void refresh(uint64_t, uint8_t ok, void* context) {
    auto& state = *static_cast<State*>(context);
    state.ok = ok != 0;
    state.refreshed = true;
}
void wait(const std::atomic<bool>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready, "native selector timed out");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        std::cout << "SKIP: requires a native macOS desktop\n";
        return 77;
    }
    uint32_t activeDisplays = 0;
    if (CGGetActiveDisplayList(0, nullptr, &activeDisplays) != kCGErrorSuccess ||
        activeDisplays == 0) {
        std::cout << "SKIP: no active macOS display (desktop may be asleep or headless)\n";
        return 77;
    }
    QAccessible::setActive(true);
    QWidget window;
    window.setWindowTitle(QStringLiteral("Snow Shot Smart selection smoke fixture"));
    auto* layout = new QVBoxLayout(&window);
    auto* edit = new QLineEdit(QStringLiteral("Accessibility child selection"), &window);
    layout->addWidget(edit);
    window.resize(500, 200);
    window.show();
    window.raise();
    window.activateWindow();
    QElapsedTimer exposure;
    exposure.start();
    while (exposure.elapsed() < 300) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    const QPoint logical = edit->mapToGlobal(edit->rect().center());
    CGDirectDisplayID display = 0;
    uint32_t count = 0;
    require(CGGetDisplaysWithPoint(CGPointMake(logical.x(), logical.y()), 1, &display, &count) ==
                    0 &&
                count == 1,
            "fixture display must be identifiable");
    const CGRect bounds = CGDisplayBounds(display);
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    const double scaleX = static_cast<double>(CGDisplayModeGetPixelWidth(mode)) /
                          static_cast<double>(CGDisplayModeGetWidth(mode));
    const double scaleY = static_cast<double>(CGDisplayModeGetPixelHeight(mode)) /
                          static_cast<double>(CGDisplayModeGetHeight(mode));
    CGDisplayModeRelease(mode);
    SnowUiSelectorQuery query{1,
                              1,
                              1,
                              qRound(bounds.origin.x + (logical.x() - bounds.origin.x) * scaleX),
                              qRound(bounds.origin.y + (logical.y() - bounds.origin.y) * scaleY),
                              SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
                              display};
    State state;
    auto* service = snow_ui_selector_service_create(event, refresh, &state);
    require(service != nullptr, "service creation failed");
    require(snow_ui_selector_service_refresh(service, 1, SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY,
                                             nullptr, 0),
            "refresh rejected");
    wait(state.refreshed);
    require(state.ok, "refresh failed");
    require(snow_ui_selector_service_query(service, &query), "window query rejected");
    wait(state.delivered);
    require(state.ok && state.rects.size() == 1, "window selection must be available without AX");
    const auto windowRect = state.rects.front();
    require(windowRect.left <= query.x && query.x < windowRect.right && windowRect.top <= query.y &&
                query.y < windowRect.bottom,
            "window bounds must contain the physical pointer");
    const bool trusted = snow_ui_selector_accessibility_permission(0) != 0;
    state.delivered = false;
    query.request_id++;
    query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
    require(snow_ui_selector_service_query(service, &query), "AX query rejected");
    wait(state.delivered);
    require(state.ok && !state.rects.empty(), "AX query must preserve window fallback");
    if (!trusted) {
        require(state.reason == SNOW_UI_SELECTOR_PERMISSION_REQUIRED && state.rects.size() == 1,
                "missing permission must explicitly return the selected window");
        snow_ui_selector_service_destroy(service);
        std::cout << "SKIP: Accessibility not granted; window and permission fallback verified\n";
        return 77;
    }
    require(state.reason == SNOW_UI_SELECTOR_COMPLETE && state.rects.size() > 1,
            "trusted AX query must traverse the text field and its window");
    const auto child = state.rects.front();
    require(std::abs((child.right - child.left) - edit->width() * scaleX) <= 4,
            "AX child width must match Retina pixels");
    QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlay.setGeometry(window.frameGeometry());
    overlay.show();
    QApplication::processEvents();
    const auto overlayId = snow_shot::platform::captureWindowId(&overlay);
    require(overlayId.has_value(), "overlay must expose a CGWindowID");
    const uintptr_t excluded = *overlayId;
    state.refreshed = false;
    state.delivered = false;
    query.epoch = 2;
    query.request_id++;
    query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW;
    require(snow_ui_selector_service_refresh(service, 2, SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY,
                                             &excluded, 1),
            "exclusion refresh rejected");
    wait(state.refreshed);
    require(snow_ui_selector_service_query(service, &query), "excluded-overlay query rejected");
    wait(state.delivered);
    require(state.ok && state.rects.size() == 1 && state.rects.front().left == windowRect.left &&
                state.rects.front().top == windowRect.top,
            "excluded overlay must not replace the selected window");
    snow_ui_selector_service_destroy(service);
    std::cout << "Native window, AX ancestry, Retina geometry, and overlay exclusion passed\n";
}
