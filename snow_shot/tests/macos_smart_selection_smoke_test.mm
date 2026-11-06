#include "snow_ui_selector.h"
#include "snow_shot/platform/windowcaptureexclusion.h"
#include "../src/platform/macos/selectorwindowtarget_p.h"
#include "../src/platform/macos/selectorlocalelements_p.h"
#include "../src/presentation/selector/screenshotselectorserviceclient.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QProcess>
#include <QScreen>
#include <QThread>
#include <QVBoxLayout>
#include <CoreGraphics/CoreGraphics.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
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
        throw std::runtime_error(message);
    }
}
class FixtureProcess final : public QProcess {
  public:
    ~FixtureProcess() override {
        if (state() == QProcess::NotRunning)
            return;
        terminate();
        if (!waitForFinished(3000)) {
            kill();
            waitForFinished();
        }
    }
};
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
bool quartzWindowMatchesFrame(CGWindowID id, const QRect& expected) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, id);
    if (!windows)
        return false;
    CGRect bounds{};
    bool matches = false;
    if (CFArrayGetCount(windows) == 1) {
        const auto window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
        const auto value =
            static_cast<CFDictionaryRef>(CFDictionaryGetValue(window, kCGWindowBounds));
        matches = value && CGRectMakeWithDictionaryRepresentation(value, &bounds) &&
                  std::abs(bounds.origin.x - expected.x()) <= 1 &&
                  std::abs(bounds.origin.y - expected.y()) <= 1 &&
                  std::abs(bounds.size.width - expected.width()) <= 1 &&
                  std::abs(bounds.size.height - expected.height()) <= 1;
    }
    CFRelease(windows);
    return matches;
}
} // namespace
int main(int argc, char** argv) try {
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
    QWidget window;
    window.setWindowTitle(QStringLiteral("Snow Shot Smart selection smoke fixture"));
    auto* layout = new QVBoxLayout(&window);
    auto* edit = new QLineEdit(QStringLiteral("Accessibility child selection"), &window);
    layout->addWidget(edit);
    window.resize(500, 200);
    // Geometry assertions describe a stationary fixture, not AppKit's opening animation.
    auto* nativeView = reinterpret_cast<NSView*>(window.winId());
    nativeView.window.animationBehavior = NSWindowAnimationBehaviorNone;
    window.show();
    window.raise();
    window.activateWindow();
    [NSApp activate];
    const auto fixtureWindowId = snow_shot::platform::captureWindowId(&window);
    require(fixtureWindowId.has_value(), "fixture must expose a CGWindowID");
    bool fixtureIsTarget = false;
    QElapsedTimer exposure;
    exposure.start();
    while (!fixtureIsTarget && exposure.elapsed() < 5000) {
        QApplication::processEvents();
        const QPoint point = edit->mapToGlobal(edit->rect().center());
        // Quartz can still report the opening animation's smaller bounds after
        // AppKit exposes the window. Wait for the snapshot geometry we will query.
        fixtureIsTarget = snow_shot::platform::detail::selectorWindowAtDesktopPoint(point, {}) ==
                              *fixtureWindowId &&
                          quartzWindowMatchesFrame(static_cast<CGWindowID>(*fixtureWindowId),
                                                   window.frameGeometry());
        if (!fixtureIsTarget)
            QThread::msleep(1);
    }
    require(fixtureIsTarget, "fixture must become the native mouse target before querying");
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
                              display,
                              0,
                              0};
    if (app.arguments().contains(QStringLiteral("--fixture"))) {
        const QRect frame = window.frameGeometry();
        const QJsonObject target{{QStringLiteral("x"), query.x},
                                 {QStringLiteral("y"), query.y},
                                 {QStringLiteral("display"), static_cast<qint64>(display)},
                                 {QStringLiteral("left"), frame.x()},
                                 {QStringLiteral("top"), frame.y()},
                                 {QStringLiteral("width"), frame.width()},
                                 {QStringLiteral("height"), frame.height()},
                                 {QStringLiteral("pixelWidth"), frame.width() * scaleX},
                                 {QStringLiteral("pixelHeight"), frame.height() * scaleY},
                                 {QStringLiteral("childPixelWidth"), edit->width() * scaleX}};
        std::cout << QJsonDocument(target).toJson(QJsonDocument::Compact).constData() << std::endl;
        return app.exec();
    }
    const auto displays = snow_shot::platform::detail::selectorDisplayGeometry();
    const auto resolveWindow = [&](std::span<const std::uintptr_t> excluded = {}) {
        const auto hit = snow_shot::platform::detail::selectorWindowAtPhysicalPoint(
            QPoint(query.x, query.y), query.display_id, displays, excluded);
        require(hit.has_value(), "native physical pointer must resolve a mouse hit");
        query.window_id = *hit;
        query.window_hit_tested = 1;
    };
    resolveWindow();
    State state;
    std::unique_ptr<SnowUiSelectorService, decltype(&snow_ui_selector_service_destroy)> service(
        snow_ui_selector_service_create(event, refresh, &state), snow_ui_selector_service_destroy);
    require(service != nullptr, "service creation failed");
    require(snow_ui_selector_service_refresh(service.get(), 1,
                                             SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY, nullptr, 0),
            "refresh rejected");
    wait(state.refreshed);
    require(state.ok, "refresh failed");
    require(snow_ui_selector_service_query(service.get(), &query), "window query rejected");
    wait(state.delivered);
    require(state.ok && state.rects.size() == 1, "window selection must be available without AX");
    auto windowRect = state.rects.front();
    require(windowRect.left <= query.x && query.x < windowRect.right && windowRect.top <= query.y &&
                query.y < windowRect.bottom,
            "window bounds must contain the physical pointer");
    require(std::abs((windowRect.right - windowRect.left) -
                     window.frameGeometry().width() * scaleX) <= 4 &&
                std::abs((windowRect.bottom - windowRect.top) -
                         window.frameGeometry().height() * scaleY) <= 4,
            "window selection must match the fixture, not a full-screen shell surface");
    state.delivered = false;
    query.request_id++;
    query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
    require(snow_ui_selector_service_query(service.get(), &query), "own-window query rejected");
    wait(state.delivered);
    require(state.ok && state.reason == SNOW_UI_SELECTOR_COMPLETE && state.rects.size() == 1 &&
                state.rects.front().left == windowRect.left &&
                state.rects.front().top == windowRect.top &&
                state.rects.front().right == windowRect.right &&
                state.rects.front().bottom == windowRect.bottom,
            "own-window element query must complete with snapshot bounds without entering AX");
    ScreenshotSelectorResult local;
    local.ok = true;
    local.mode = ScreenshotSelectorHitTestMode::WindowSubElement;
    local.nativeWindowId = query.window_id;
    local.point = QPoint(query.x, query.y);
    local.displayId = query.display_id;
    local.rects = {QRectF(windowRect.left, windowRect.top, windowRect.right - windowRect.left,
                          windowRect.bottom - windowRect.top)};
    snow_shot::platform::detail::resolveSelectorLocalElements(local, displays);
    require(local.rects.size() > 1 &&
                std::abs(local.rects.front().width() - edit->width() * scaleX) <= 4 &&
                std::abs(local.rects.front().height() - edit->height() * scaleY) <= 4,
            "GUI-thread local selection must reach the Qt text field without AX permission");

    // Exercise the actual worker -> queued GUI callback integration as well as the
    // local provider. No accessibility object may be accessed by the Rust worker.
    std::atomic<bool> clientRefreshed = false;
    std::atomic<bool> clientDelivered = false;
    ScreenshotSelectorResult clientResult;
    ScreenshotSelectorServiceClient client(
        {[&](quint64, bool ok) {
             require(ok, "local client refresh failed");
             clientRefreshed = true;
         },
         [&](const ScreenshotSelectorResult& result) {
             require([NSThread isMainThread], "local results must be delivered on the GUI thread");
             clientResult = result;
             clientDelivered = true;
         }});
    require(client.startRefreshWithDisplays(1, {}, displays), "local client refresh rejected");
    wait(clientRefreshed);
    const auto submitLocal = [&](quint64 request, quint64 epoch = 1) {
        clientDelivered = false;
        require(client.startHitTest(epoch, request, request, QPoint(query.x, query.y),
                                    ScreenshotSelectorHitTestMode::WindowSubElement,
                                    query.display_id),
                "local client query rejected");
    };
    submitLocal(1);
    wait(clientDelivered);
    require(clientResult.rects == local.rects && !clientResult.canRefine,
            "service client must complete own-window results with local controls");
    submitLocal(2);
    clientRefreshed = false;
    require(client.startRefreshWithDisplays(2, {}, displays),
            "local client second refresh rejected");
    wait(clientDelivered);
    require(clientResult.rects.size() <= 1,
            "new refresh epochs must not enrich results from the previous snapshot");
    wait(clientRefreshed);
    submitLocal(3, 2);
    require(client.releaseCache(), "local client cache release failed");
    wait(clientDelivered);
    require(clientResult.rects.size() <= 1,
            "released client must not enrich pending worker results with local controls");
    client.destroyService();

    // Direct AppKit controls use the same local path, without a Qt widget interface.
    NSPanel* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(100, 100, 300, 180)
                                                styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    panel.releasedWhenClosed = NO;
    panel.animationBehavior = NSWindowAnimationBehaviorNone;
    NSButton* button = [NSButton buttonWithTitle:@"Local selection fixture" target:nil action:nil];
    button.frame = NSMakeRect(30, 50, 180, 32);
    [panel.contentView addSubview:button];
    [panel orderFront:nil];
    const qreal desktopTop = NSMaxY(NSScreen.screens.firstObject.frame);
    const QRectF buttonFrame = snow_shot::platform::detail::selectorLocalDesktopFrame(
        button.accessibilityFrame, desktopTop);
    const auto* panelDisplay = &displays.front();
    for (const auto& candidate : displays) {
        if (QRectF(candidate.capturedLogicalRect).contains(buttonFrame.center())) {
            panelDisplay = &candidate;
            break;
        }
    }
    const QRectF panelFrame = snow_shot::platform::detail::selectorLocalDesktopFrame(
        panel.accessibilityFrame, desktopTop);
    const QRectF buttonPixels =
        snow_shot::platform::detail::selectorLocalPixelFrame(buttonFrame, *panelDisplay);
    local.point = buttonPixels.center().toPoint();
    local.displayId = panelDisplay->nativeDisplayId;
    local.nativeWindowId = static_cast<std::uintptr_t>(panel.windowNumber);
    local.rects = {snow_shot::platform::detail::selectorLocalPixelFrame(
        panelFrame.intersected(QRectF(panelDisplay->capturedLogicalRect)), *panelDisplay)};
    snow_shot::platform::detail::resolveSelectorLocalElements(local, displays);
    require(local.rects.size() > 1 && local.rects.front() == buttonPixels,
            "GUI-thread local selection must reach native AppKit controls");
    [panel close];
    [panel release];

    state.delivered = false;
    require(snow_ui_selector_service_refine(service.get(), &query),
            "own-window refinement rejected");
    wait(state.delivered);
    require(state.ok && state.reason == SNOW_UI_SELECTOR_COMPLETE && state.rects.size() == 1,
            "own-window refinement must also avoid in-process AX");

    // Foreign AX must still traverse real controls. A separate fixture process
    // ensures this assertion never relies on unsafe in-process AppKit dispatch.
    window.hide();
    FixtureProcess fixture;
    fixture.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    fixture.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--fixture"), QStringLiteral("-platform"), QStringLiteral("cocoa")});
    require(fixture.waitForStarted(), "foreign fixture failed to start");
    QElapsedTimer readiness;
    readiness.start();
    while (!fixture.canReadLine() && readiness.elapsed() < 5000)
        fixture.waitForReadyRead(100);
    const auto target = QJsonDocument::fromJson(fixture.readLine()).object();
    require(target.contains(QStringLiteral("childPixelWidth")),
            "foreign fixture did not report geometry");
    const QRect targetFrame(target.value(QStringLiteral("left")).toInt(),
                            target.value(QStringLiteral("top")).toInt(),
                            target.value(QStringLiteral("width")).toInt(),
                            target.value(QStringLiteral("height")).toInt());
    query.x = target.value(QStringLiteral("x")).toInt();
    query.y = target.value(QStringLiteral("y")).toInt();
    query.display_id = static_cast<uint32_t>(target.value(QStringLiteral("display")).toInteger());
    query.epoch++;
    query.request_id++;
    query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW;
    resolveWindow();
    const auto foreignWindowId = query.window_id;
    state.refreshed = false;
    state.delivered = false;
    require(snow_ui_selector_service_refresh(service.get(), query.epoch,
                                             SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY, nullptr, 0),
            "foreign fixture refresh rejected");
    wait(state.refreshed);
    require(snow_ui_selector_service_query(service.get(), &query), "foreign window query rejected");
    wait(state.delivered);
    require(state.ok && state.rects.size() == 1, "foreign fixture must offer window selection");
    windowRect = state.rects.front();
    require(std::abs((windowRect.right - windowRect.left) -
                     target.value(QStringLiteral("pixelWidth")).toDouble()) <= 4 &&
                std::abs((windowRect.bottom - windowRect.top) -
                         target.value(QStringLiteral("pixelHeight")).toDouble()) <= 4,
            "foreign window selection must match the fixture");
    const bool trusted = snow_ui_selector_accessibility_permission(0) != 0;
    state.delivered = false;
    query.request_id++;
    query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
    require(snow_ui_selector_service_query(service.get(), &query), "foreign AX query rejected");
    wait(state.delivered);
    require(state.ok && !state.rects.empty(), "AX query must preserve window fallback");
    if (!trusted) {
        require(state.reason == SNOW_UI_SELECTOR_PERMISSION_REQUIRED && state.rects.size() == 1,
                "missing permission must explicitly return the selected window");
        service.reset();
        std::cout << "SKIP: Accessibility not granted; window and permission fallback verified\n";
        return 77;
    }
    require(state.reason == SNOW_UI_SELECTOR_COMPLETE && state.rects.size() > 1,
            "trusted AX query must traverse the text field and its window");
    const auto child = state.rects.front();
    require(std::abs((child.right - child.left) -
                     target.value(QStringLiteral("childPixelWidth")).toDouble()) <= 4,
            "AX child width must match Retina pixels");

    QWidget clickThrough(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                                      Qt::WindowTransparentForInput);
    clickThrough.setAttribute(Qt::WA_TranslucentBackground);
    clickThrough.setGeometry(window.screen()->geometry());
    clickThrough.show();
    QApplication::processEvents();
    const auto clickThroughId = snow_shot::platform::captureWindowId(&clickThrough);
    require(clickThroughId.has_value(), "click-through surface must expose a CGWindowID");
    resolveWindow();
    require(query.window_id != *clickThroughId,
            "native mouse hit must bypass a full-screen click-through surface");
    state.refreshed = false;
    state.delivered = false;
    query.epoch++;
    query.request_id++;
    require(snow_ui_selector_service_refresh(service.get(), query.epoch,
                                             SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY, nullptr, 0),
            "click-through refresh rejected");
    wait(state.refreshed);
    require(snow_ui_selector_service_query(service.get(), &query),
            "click-through AX query rejected");
    wait(state.delivered);
    require(state.ok && state.reason == SNOW_UI_SELECTOR_COMPLETE && state.rects.size() > 1 &&
                state.rects.front().left == child.left && state.rects.front().right == child.right,
            "click-through surface must preserve underlying element selection");

    QWidget overlay(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlay.setGeometry(targetFrame);
    overlay.show();
    QApplication::processEvents();
    const auto overlayId = snow_shot::platform::captureWindowId(&overlay);
    require(overlayId.has_value(), "overlay must expose a CGWindowID");
    const uintptr_t excluded = *overlayId;
    resolveWindow({&excluded, 1});
    state.refreshed = false;
    state.delivered = false;
    query.epoch++;
    query.request_id++;
    query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW;
    require(snow_ui_selector_service_refresh(service.get(), query.epoch,
                                             SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY, &excluded, 1),
            "exclusion refresh rejected");
    wait(state.refreshed);
    require(snow_ui_selector_service_query(service.get(), &query),
            "excluded-overlay query rejected");
    wait(state.delivered);
    require(query.window_id == foreignWindowId && state.ok && state.rects.size() == 1 &&
                state.rects.front().left == windowRect.left &&
                state.rects.front().top == windowRect.top,
            "excluded overlay must not replace the selected window");
    service.reset();
    std::cout << "Local Qt/AppKit selection, worker safety, foreign AX ancestry, Retina geometry, "
                 "click-through surfaces, and overlay exclusion passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
