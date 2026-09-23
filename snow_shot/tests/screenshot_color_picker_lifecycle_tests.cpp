#include "snow_shot/presentation/screenshotcanvascolorsamplerwindow.h"
#include "snow_shot/platform/screenshotnative.h"
#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>
#endif
#include "snow_shot/presentation/screenshotcolorpickerwindow.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/screenshotoverlayuihost.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QWindow>

#include <cstdlib>
#include <functional>
#include <iostream>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

const uchar* backingPixels(ScreenshotColorPickerWindow& picker) {
    QBackingStore* store = picker.backingStore();
    require(store != nullptr && store->size() == picker.size(),
            "hidden preparation must allocate the full logical backing-store size");
    store->beginPaint(picker.rect());
    QPaintDevice* device = store->paintDevice();
    require(device != nullptr && device->devType() == QInternal::Image,
            "the raster backend must expose an image paint device");
    const auto* image = static_cast<const QImage*>(device);
    require(!image->isNull() && image->size() == picker.size() * picker.devicePixelRatioF(),
            "preparation must allocate backing pixels at the window device pixel ratio");
    const uchar* pixels = image->constBits();
    store->endPaint();
    return pixels;
}

class NoopOverlayEventSink final : public ScreenshotOverlayEventSink {
  public:
    ScreenshotOverlayRightClickResult rightClickResult = ScreenshotOverlayRightClickResult::Ignored;
    std::function<void()> cancel = [] {};
    void completeRightClickCancellation() override {
        cancel();
    }
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return false;
    }

    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF&) override {}

    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF&) override {}

    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF&) override {}

    ScreenshotOverlayRightClickResult handleOverlayRightClick(ScreenshotOverlayWindow*,
                                                              const QPointF&) override {
        return rightClickResult;
    }

    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QPointF&, const QPoint&,
                            const QPoint&) override {
        return false;
    }

    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }

    void raiseToolbarForCanvasInteraction() override {}
};

void pickerLifetimeFollowsExplicitSessionOperations() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize({QDir(temporary.path()).filePath(QStringLiteral("bin")),
                             temporary.path(), 60000})
                .success,
            "storage must initialize");
    NoopOverlayEventSink sink;
    ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
    ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
    first.setGeometry(0, 0, 800, 600);
    second.setGeometry(800, 0, 800, 600);
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto screens = QGuiApplication::screens();
        require(!screens.isEmpty(), "native smoke test requires an available display");
        first.setScreen(screens.first());
        first.setGeometry(screens.first()->geometry());
        second.setScreen(screens.last());
        second.setGeometry(screens.last()->geometry());
        std::cout << "Native picker smoke test: " << screens.size() << " display(s) available\n";
    }
    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    QPointer<ScreenshotColorPickerWindow> tracked;
    {
        ScreenshotOverlayUiHost host;
        require(host.colorPicker() == nullptr, "idle host must not own a picker");
        host.setColorPickerCenterGuideLineColor(Qt::green);
        host.prepareColorPickerSurface(&first);
        host.updateColorPicker(&first, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0);
        require(host.colorPicker() == nullptr,
                "preparation and updates must never create a picker");
        host.createColorPicker();
        tracked = host.colorPicker();
        host.createColorPicker();
        require(tracked && tracked == host.colorPicker() && !tracked->isVisible(),
                "session creation must be idempotent and hidden");
        host.prepareColorPickerSurface(&first);
        require(tracked->windowHandle() && !tracked->isVisible() &&
                    tracked->windowHandle()->transientParent() == first.windowHandle(),
                "preparation must create a hidden native window owned by the overlay");
        const auto requireNonNativeCanvases = [&]() {
            for (const auto* overlay : {&first, &second}) {
                const auto* canvas = overlay->findChild<SnowCanvasWidget*>();
                require(canvas && !canvas->testAttribute(Qt::WA_NativeWindow) &&
                            canvas->internalWinId() == 0,
                        "preparing and moving the picker must not create native canvas children");
            }
        };
        requireNonNativeCanvases();
        const uchar* preparedPixels = backingPixels(*tracked);
        const WId preparedWindowId = tracked->internalWinId();
        host.prepareColorPickerSurface(&first);
        require(!tracked->isVisible() && !tracked->windowHandle()->isVisible() &&
                    tracked->internalWinId() == preparedWindowId &&
                    backingPixels(*tracked) == preparedPixels && !tracked->hasCurrentColor(),
                "repeated preparation must retain hidden native pixels without requiring an image");
        host.updateColorPicker(&first, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0);
        QApplication::processEvents();
        require(tracked->hasCurrentColor() && tracked->isVisible(),
                "the prepared picker must reveal its first sample");
        require(tracked->internalWinId() == preparedWindowId &&
                    backingPixels(*tracked) == preparedPixels,
                "the first sampled frame must reuse the preallocated native surface");
        tracked->cycleColorFormat();
        const QString format = tracked->currentColorText();
        host.updateColorPicker(&second, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0);
        require(tracked == host.colorPicker() && tracked->parentWidget() == &second &&
                    tracked->windowHandle()->transientParent() == second.windowHandle(),
                "moving between overlays must retain one picker and update its native owner");
        requireNonNativeCanvases();
        const QPoint globalCursor = second.mapToGlobal(QPoint(8, 8));
        require(tracked->pos().x() > globalCursor.x() && tracked->pos().y() > globalCursor.y(),
                "changing displays must position the picker next to the new global cursor");
        host.prepareColorPickerSurface(&first);
        require(tracked->parentWidget() == &second && tracked->isVisible(),
                "capture-result preparation must preserve the current display and visibility");
        bool imageReleased = false;
        unsigned char pixels[16 * 16 * 4]{};
        QImage retainedImage(
            pixels, 16, 16, QImage::Format_RGBA8888,
            [](void* state) { *static_cast<bool*>(state) = true; }, &imageReleased);
        retainedImage.fill(Qt::red);
        tracked->setCaptureImage(retainedImage, retainedImage.rect());
        retainedImage = QImage();
        require(!imageReleased, "the picker must hold the capture image during the session");
#ifdef Q_OS_WIN
        const HWND nativeWindow = QGuiApplication::platformName() == QStringLiteral("windows")
                                      ? reinterpret_cast<HWND>(tracked->winId())
                                      : nullptr;
#endif
        host.releaseColorPicker();
        require(tracked.isNull() && host.colorPicker() == nullptr && imageReleased,
                "session release must synchronously destroy the window and release its image");
#ifdef Q_OS_WIN
        require(nativeWindow == nullptr || !IsWindow(nativeWindow),
                "session release must destroy the native Windows window");
#endif
        host.releaseColorPicker();
        host.resetColorPickerForNewCapture();
        host.updateColorPicker(&second, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0);
        require(host.colorPicker() == nullptr,
                "late updates and cleanup must leave the picker absent");

        host.createColorPicker();
        tracked = host.colorPicker();
        require(!tracked->hasCurrentColor() && tracked->currentColorText().isEmpty(),
                "a replacement picker must not retain the old sample");
        host.updateColorPicker(&first, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 0.0);
        require(tracked->currentColorText() == format,
                "a replacement picker must restore the selected format");
        host.detachOverlayTransientUi(&first);
        require(tracked && tracked->parentWidget() == nullptr,
                "detaching an overlay must preserve its session picker");
    }
    require(tracked.isNull(), "host destruction must release a detached picker");
    {
        ScreenshotOverlayUiHost host;
        auto* owner = new ScreenshotOverlayWindow(sink, new SnowCanvasWidget);
        host.createColorPicker();
        host.prepareColorPickerSurface(owner);
        tracked = host.colorPicker();
        delete owner;
        require(tracked.isNull() && host.colorPicker() == nullptr,
                "owner destruction must clear host tracking");
        host.releaseColorPicker();
    }
    storage.shutdown();
}
void invocationMonitorOwnsThePreparedSurface() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    // Include displays left of and above the primary, and use physical bounds
    // different from logical bounds to catch mixing coordinate systems.
    for (const QRect secondaryRect :
         {QRect(800, 0, 800, 600), QRect(-800, 0, 800, 600), QRect(0, -600, 800, 600)}) {
        ScreenshotOverlayWindow primary(sink, new SnowCanvasWidget);
        ScreenshotOverlayWindow secondary(sink, new SnowCanvasWidget);
        primary.setGeometry(0, 0, 800, 600);
        secondary.setGeometry(secondaryRect);
        CapturedDisplayModel primaryDisplay;
        primaryDisplay.logicalRect = primary.geometry();
        primaryDisplay.physicalRect = primary.geometry();
        primaryDisplay.active = true;
        CapturedDisplayModel secondaryDisplay;
        secondaryDisplay.logicalRect = secondaryRect;
        secondaryDisplay.physicalRect = QRect(5000, 5000, 1600, 1200);
        secondaryDisplay.active = true;
        ScreenshotDisplaySession displays;
        displays.appendDisplay(primaryDisplay, &primary);
        displays.appendDisplay(secondaryDisplay, &secondary);
        ScreenshotOverlayCoordinator coordinator(sink, runtime, shortcuts);

        const QPoint invocationPosition = secondaryRect.center();
        coordinator.createColorPicker(invocationPosition);
        coordinator.prepareColorPickerSurface(displays);
        auto* picker = coordinator.colorPicker();
        require(picker && picker->parentWidget() == &secondary && !picker->isVisible(),
                "the invocation monitor must own the hidden picker even when it is not first");
        require(secondaryRect.contains(picker->geometry().center()) &&
                    picker->windowHandle()->transientParent() == secondary.windowHandle(),
                "native creation must place the picker on its selected monitor before allocation");
        const uchar* pixels = backingPixels(*picker);
        const WId nativeId = picker->internalWinId();
        coordinator.prepareColorPickerSurface(displays);
        require(picker->parentWidget() == &secondary && backingPixels(*picker) == pixels,
                "later preparation must keep the invocation monitor's surface");
        QImage image(16, 16, QImage::Format_RGBA8888);
        image.fill(Qt::blue);
        coordinator.updateColorPicker(&secondary, image, image.rect(), QPoint(8, 8),
                                      secondary.rect().center(), 1.0);
        QApplication::processEvents();
        require(picker->internalWinId() == nativeId && backingPixels(*picker) == pixels,
                "first reveal on the invocation monitor must retain the preallocated pixels");
        coordinator.releaseColorPicker();

        coordinator.createColorPicker(primary.geometry().center());
        coordinator.prepareColorPickerSurface(displays);
        require(coordinator.colorPicker()->parentWidget() == &primary,
                "each new session must use its own invocation monitor");
        coordinator.releaseColorPicker();
        displays.displayAt(1).active = false;
        coordinator.createColorPicker(invocationPosition);
        coordinator.prepareColorPickerSurface(displays);
        require(coordinator.colorPicker()->parentWidget() == &primary,
                "a removed invocation display must fall back to an active overlay");
    }
}

void startupPickerUsesResolvedOwnerWithoutSamplingNativeCursor() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime canvas;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
    ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
    first.setGeometry(0, 0, 80, 60);
    second.setGeometry(80, 0, 80, 60);
    CapturedDisplayModel left;
    left.stableId = QStringLiteral("left");
    left.logicalRect = first.geometry();
    left.physicalRect = left.logicalRect;
    left.active = true;
    left.geometryResolved = true;
    left.image = QImage(80, 60, QImage::Format_RGB32);
    left.image.fill(Qt::red);
    auto right = left;
    right.stableId = QStringLiteral("right");
    right.logicalRect = second.geometry();
    right.physicalRect = right.logicalRect;
    right.image = QImage(80, 60, QImage::Format_RGB32);
    right.image.fill(Qt::blue);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(left, &first);
    displays.appendDisplay(right, &second);
    auto startup = std::make_shared<ScreenshotStartupContext>();
    startup->phase = ScreenshotStartupContext::Phase::Revealed;
    startup->displaySlot = 1;
    startup->displayId = right.stableId;
    startup->logicalPosition = QPoint(100, 20);
    startup->physicalPosition = startup->logicalPosition;
    displays.startup = startup;
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    int reads = 0;
    snow_shot::platform::PhysicalCursor cursor({true,
                                                [&]() -> std::optional<QPoint> {
                                                    ++reads;
                                                    return QPoint(20, 20);
                                                },
                                                [](const QPoint&) { return true; },
                                                [&]() -> std::optional<QPointF> {
                                                    ++reads;
                                                    return QPointF(20, 20);
                                                }});
    ScreenshotOverlayCoordinator coordinator(sink, canvas, shortcuts);
    coordinator.createColorPicker(QPoint(20, 20));
    coordinator.prepareColorPickerSurface(displays);
    ScreenshotColorPickerController controller(coordinator, geometry, displays, cursor);
    ScreenshotColorPickerContext context;
    context.active = true;
    context.moveToolActive = true;
    context.intelligentSelecting = true;
    controller.updateAtCurrentCursor(context);
    require(reads == 0 && coordinator.colorPicker()->parentWidget() == &second &&
                coordinator.colorPicker()->currentColorText().compare(QStringLiteral("#0000ff"),
                                                                      Qt::CaseInsensitive) == 0,
            "startup picker must share the invocation owner and sample without reading the live "
            "cursor");
    require(startup->suppressesInput(), "reading the gate must keep the invocation anchor");
    startup->resumeLiveInput();
    require(!startup->anchored(), "revealed input must release the cursor anchor");
    controller.updateAtCurrentCursor(context);
    require(reads == 1 && coordinator.colorPicker()->parentWidget() == &first,
            "live picker must sample once and follow the newly selected display");
    coordinator.releaseColorPicker();
}

void canvasSamplerFollowsSessionOwner() {
    QWidget overlay(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    QWidget canvas(&overlay);
    overlay.resize(320, 240);
    overlay.show();
#ifdef Q_OS_MACOS
    snow_shot::platform::configureScreenshotOverlayWindow(&overlay);
#endif
    QWidget pinned(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint);
    pinned.show();
    ScreenshotCanvasColorSamplerWindow sampler;
    QImage preview(7, 7, QImage::Format_RGB32);
    preview.fill(Qt::red);
    const bool cocoa = QGuiApplication::platformName() == QStringLiteral("cocoa");
    for (QWidget* owner : {&overlay, &pinned, &overlay}) {
        QWidget pickerControl(owner);
        sampler.beginSampling(&pickerControl);
        require(!sampler.isVisible(), "sampling must wait for a valid preview before showing");
        sampler.updateSample(preview, owner->mapToGlobal(QPoint(20, 20)));
        QCoreApplication::processEvents();
        require(sampler.isVisible() &&
                    sampler.windowHandle()->transientParent() == owner->windowHandle(),
                "the sampling HUD must be visible and transient to the current session owner");
        require(sampler.parentWidget() == nullptr && !canvas.testAttribute(Qt::WA_NativeWindow),
                "sampling must preserve controller ownership and non-native canvas input");
        owner->raise();
        QCoreApplication::processEvents();
#ifdef Q_OS_MACOS
        if (cocoa) {
            NSWindow* hud = reinterpret_cast<NSView*>(sampler.winId()).window;
            NSWindow* nativeOwner = reinterpret_cast<NSView*>(owner->winId()).window;
            require(hud.visible && hud.level >= nativeOwner.level,
                    "the sampler must not be hidden below its owner's native level");
            if (owner == &overlay)
                require(hud.level > nativeOwner.level,
                        "the sampler must join the elevated screenshot stacking hierarchy");
            else
                require(hud.level < CGWindowLevelForKey(kCGScreenSaverWindowLevelKey),
                        "pinned sampling must not retain the screenshot's elevated level");
            require(hud.ignoresMouseEvents, "the sampler must not intercept canvas input");
        }
#else
        Q_UNUSED(cocoa);
#endif
        sampler.endSampling();
        require(!sampler.isVisible() &&
                    (!sampler.windowHandle() || !sampler.windowHandle()->transientParent()),
                "ending sampling must hide the HUD and release its transient owner");
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    canvasSamplerFollowsSessionOwner();
    if (application.arguments().contains(QStringLiteral("--canvas-sampler-only")))
        return 0;
    pickerLifetimeFollowsExplicitSessionOperations();
    invocationMonitorOwnsThePreparedSurface();
    startupPickerUsesResolvedOwnerWithoutSamplingNativeCursor();
    return 0;
}
