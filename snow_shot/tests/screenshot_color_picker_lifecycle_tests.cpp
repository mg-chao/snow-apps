#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
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
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"
#include "widgets/button.h"

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

class StyleToolbarCommands final : public ScreenshotToolbarCommandSink,
                                   public ScreenshotSelectionToolbarCommandSink {
  public:
    void setMoveTool() override {
        ++moveToolCount;
    }
    void setSelectTool() override {
        ++selectToolCount;
    }
    void setShapeTool() override {
        ++shapeToolCount;
    }
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&, quint32) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}

    void toggleSelectionAspectRatioLockFromToolbar() override {}
    void setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset) override {}
    void openSelectionResizeModalFromToolbar() override {}
    void adjustSelectionFromToolbar(int, int, int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionShadowWidthFromToolbar(int) override {}
    void setSelectionToolbarHovered(bool) override {}
    int moveToolCount = 0;
    int selectToolCount = 0;
    int shapeToolCount = 0;
};

void screenshotStyleBindingFollowsToolbarAttachment() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary style storage available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated style storage");
    {
        NoopOverlayEventSink sink;
        ScreenshotOverlayWindow first(sink, new SnowCanvasWidget);
        ScreenshotOverlayWindow second(sink, new SnowCanvasWidget);
        StyleToolbarCommands commands;
        ScreenshotOverlayUiHost host;
        host.setToolbarCommandSinks(commands, commands);
        host.attachToolbarToOverlay(&first);
        first.canvas()->setInteractionEnabled(true);
        require(first.canvas()->setCanvasTool(SnowCanvasTool::Text), "activate first canvas text");
        host.toolbar()->palette()->setActiveTool(ScreenshotToolPalette::Tool::Text);
        const double firstSize = first.canvas()->canvasStyleToolbarState().textStyle.fontSize;
        require(first.canvas()->stepFontSize(1), "step first screenshot font");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults().text.fontSize ==
                    firstSize + 1,
                "real screenshot UI host persists canvas font edits");
        host.attachToolbarToOverlay(&second);
        second.canvas()->setInteractionEnabled(true);
        require(second.canvas()->setCanvasTool(SnowCanvasTool::Text),
                "activate second canvas text");
        const double secondSize = second.canvas()->canvasStyleToolbarState().textStyle.fontSize;
        require(second.canvas()->stepFontSize(-1), "step second screenshot font");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults().text.fontSize ==
                    secondSize - 1,
                "moving the screenshot toolbar rebinds style persistence");
        host.detachOverlayTransientUi(&second);
        const auto saved = snow_shot::presentation::screenshotCanvasToolStyleDefaults();
        require(second.canvas()->stepFontSize(1),
                "detached canvas can still update its local style");
        require(snow_shot::presentation::screenshotCanvasToolStyleDefaults() == saved,
                "detaching screenshot UI removes its preference binding");
    }
    storage.shutdown();
}

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
        host.updateColorPicker(
            &first, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0,
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
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
        host.updateColorPicker(
            &first, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0,
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        QApplication::processEvents();
        require(tracked->hasCurrentColor() && tracked->isVisible(),
                "the prepared picker must reveal its first sample");
        require(tracked->internalWinId() == preparedWindowId &&
                    backingPixels(*tracked) == preparedPixels,
                "the first sampled frame must reuse the preallocated native surface");
        tracked->cycleColorFormat();
        const QString format = tracked->currentColorText();
        host.updateColorPicker(
            &second, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0,
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
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
        host.updateColorPicker(
            &second, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 1.0,
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
        require(host.colorPicker() == nullptr,
                "late updates and cleanup must leave the picker absent");

        host.createColorPicker();
        tracked = host.colorPicker();
        require(!tracked->hasCurrentColor() && tracked->currentColorText().isEmpty(),
                "a replacement picker must not retain the old sample");
        host.updateColorPicker(
            &first, image, image.rect(), QPoint(8, 8), QPointF(8, 8), 0.0,
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
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

void visibleRecaptureWindowsIncludePicker() {
    NoopOverlayEventSink sink;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow overlay(sink, new SnowCanvasWidget);
    overlay.setGeometry(0, 0, 320, 240);
    overlay.show();
    QApplication::processEvents();

    CapturedDisplayModel display;
    display.logicalRect = overlay.geometry();
    display.physicalRect = display.logicalRect;
    display.active = true;
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &overlay);
    ScreenshotOverlayCoordinator coordinator(sink, runtime, shortcuts);
    coordinator.createColorPicker(overlay.geometry().center());
    coordinator.prepareColorPickerSurface(displays);
    auto* picker = coordinator.colorPicker();
    require(picker != nullptr && !picker->isVisible() &&
                coordinator.visibleRecaptureWindows(displays) == QVector<QWidget*>{&overlay},
            "recapture must include the visible overlay but not the prepared hidden picker");

    QImage image(16, 16, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    coordinator.updateColorPicker(
        &overlay, image, image.rect(), QPoint(8, 8), QPointF(50, 50), 1.0,
        {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
    QApplication::processEvents();
    require(picker->isVisible() && picker->isWindow() &&
                coordinator.visibleRecaptureWindows(displays) ==
                    QVector<QWidget*>{&overlay, picker},
            "recapture must protect the visible native picker as well as its overlay");

    coordinator.hideColorPicker();
    require(coordinator.visibleRecaptureWindows(displays) == QVector<QWidget*>{&overlay},
            "recapture must stop protecting the picker after it is hidden");
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
        coordinator.updateColorPicker(
            &secondary, image, image.rect(), QPoint(8, 8), secondary.rect().center(), 1.0,
            {QPointF(8, 8), ScreenshotSelectionDisplayUnit::PhysicalPixels, false});
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
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    storage.shutdown();
    require(storage
                .initialize({QDir(temporary.path()).filePath(QStringLiteral("bin")),
                             temporary.path(), 60000})
                .success,
            "coordinate integration fixture must initialize isolated storage");
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
    context.selectionDisplayUnit = ScreenshotSelectionDisplayUnit::PhysicalPixels;
    context.selectionPixels = QRect(90, 10, 30, 30);
    controller.updateAtCurrentCursor(context);
    auto* picker = coordinator.colorPicker();
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 100 Y: 20") &&
                controller.toggleCoordinateMode(context) &&
                picker->currentPositionText().simplified() == QStringLiteral("X: 10 Y: 10"),
            "controller must deliver selection-relative positions and refresh on toggle");
    context.selectionPixels.translate(5, 5);
    controller.updateAtCurrentCursor(context);
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 5 Y: 5"),
            "selection movement must refresh the origin with a stationary sample");
    context.selectionPixels = {};
    controller.updateAtCurrentCursor(context);
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 100 Y: 20"),
            "empty selection must display global coordinates");
    context.selectionPixels = QRect(100, 20, 20, 20);
    controller.updateAtCurrentCursor(context);
    require(picker->currentPositionText().simplified() == QStringLiteral("X: 0 Y: 0"),
            "relative mode must resume when a selection appears");
    controller.setSuppressed(true);
    require(!controller.toggleCoordinateMode(context),
            "suppression must disable coordinate toggle");
    controller.setSuppressed(false);
    context.active = false;
    require(!controller.toggleCoordinateMode(context),
            "inactive capture must disable coordinate toggle");
    context.active = true;
    require(controller.toggleCoordinateMode(context) &&
                picker->currentPositionText().simplified() == QStringLiteral("X: 100 Y: 20"),
            "toggling back must restore desktop coordinates");
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
    storage.shutdown();
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

void auxiliaryWindowsPreserveOwnerStacking() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary stacking-test storage available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated stacking-test storage");
    QWidget owner(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    owner.setObjectName(QStringLiteral("stackingOwner"));
    owner.setAttribute(Qt::WA_ShowWithoutActivating);
    owner.setGeometry(50, 50, 500, 400);
    owner.show();
    QWidget toolbar(&owner, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    toolbar.setObjectName(QStringLiteral("stackingToolbar"));
    toolbar.setAttribute(Qt::WA_ShowWithoutActivating);
    toolbar.setGeometry(100, 100, 200, 60);
    toolbar.show();
    QWidget unrelated(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    unrelated.setObjectName(QStringLiteral("stackingUnrelated"));
    unrelated.setAttribute(Qt::WA_ShowWithoutActivating);
    unrelated.setGeometry(600, 50, 100, 100);
    unrelated.show();
    const auto checkCycles = [&]([[maybe_unused]] const char* name,
                                 const std::function<QWidget*()>& reveal,
                                 const std::function<void()>& update,
                                 const std::function<void()>& conceal) {
        for (int cycle = 0; cycle != 3; ++cycle) {
            [[maybe_unused]] const char* stage = "before show";
            unrelated.raise();
            QApplication::processEvents();
#ifdef Q_OS_WIN
            const bool native = QGuiApplication::platformName() == QStringLiteral("windows");
            const auto above = [](QWidget* first, QWidget* second) {
                const HWND a = reinterpret_cast<HWND>(first->internalWinId());
                const HWND b = reinterpret_cast<HWND>(second->internalWinId());
                for (HWND window = GetTopWindow(nullptr); window;
                     window = GetWindow(window, GW_HWNDNEXT)) {
                    if (window == a)
                        return true;
                    if (window == b)
                        return false;
                }
                return false;
            };
            const auto verifyOwner = [&] {
                if (native) {
                    const bool preserved = above(&unrelated, &owner) && above(&unrelated, &toolbar);
                    if (!preserved) {
                        std::cerr << name << " cycle " << cycle << " " << stage << '\n';
                        for (HWND window = GetTopWindow(nullptr); window;
                             window = GetWindow(window, GW_HWNDNEXT)) {
                            if (auto* widget = QWidget::find(reinterpret_cast<WId>(window)))
                                std::cerr << widget->objectName().toStdString()
                                          << " visible=" << IsWindowVisible(window) << '\n';
                        }
                    }
                    require(preserved, "auxiliary show/update/hide must not raise the owner group");
                }
            };
            verifyOwner();
#endif
            QWidget* tool = reveal();
            stage = "after show";
            QApplication::processEvents();
            require(tool && tool->isVisible(), "auxiliary window must remain visible");
#ifdef Q_OS_WIN
            verifyOwner();
            if (native) {
                if (!(above(tool, &toolbar) && above(&unrelated, tool)))
                    std::cerr << name << " cycle " << cycle << " tool ordering after show\n";
                require(above(tool, &toolbar) && above(&unrelated, tool),
                        "auxiliary window must stack above its group, below unrelated topmosts");
            }
#endif
            update();
            tool->raise();
            stage = "after update/raise";
            QApplication::processEvents();
#ifdef Q_OS_WIN
            verifyOwner();
#endif
            conceal();
            stage = "after hide";
            QApplication::processEvents();
#ifdef Q_OS_WIN
            verifyOwner();
#endif
        }
    };

    StyleToolbarCommands commands;
    ScreenshotToolbarWindow drawingToolbar(commands);
    checkCycles(
        "drawing toolbar transient owner",
        [&]() {
            drawingToolbar.restoreNativeSurface();
            drawingToolbar.setTransientOwnerWindow(&owner);
            drawingToolbar.prepareForDisplay();
            drawingToolbar.show();
            return &drawingToolbar;
        },
        [&] {
            drawingToolbar.setTransientOwnerWindow(&owner);
            drawingToolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);
            drawingToolbar.prepareForDisplay();
        },
        [&] { drawingToolbar.releaseNativeSurface(); });
    checkCycles(
        "drawing toolbar widget owner",
        [&]() {
            drawingToolbar.setOwnerWindow(&owner);
            drawingToolbar.restoreNativeSurface();
            drawingToolbar.prepareForDisplay();
            drawingToolbar.show();
            return &drawingToolbar;
        },
        [&] {
            drawingToolbar.setOwnerWindow(&owner);
            drawingToolbar.moveContentTo(drawingToolbar.contentPosition() + QPoint(1, 1));
        },
        [&] { drawingToolbar.hide(); });
    drawingToolbar.setOwnerWindow(nullptr);

    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(Qt::red);
    ScreenshotColorPickerWindow picker;
    picker.setCaptureImage(image, image.rect());
    checkCycles(
        "magnifier",
        [&]() {
            picker.setOwnerWindow(&owner);
            picker.updatePicker(QPoint(8, 8), QPointF(8, 8), 1.0);
            return &picker;
        },
        [&] { picker.updatePicker(QPoint(9, 9), QPointF(20, 20), 1.0); },
        [&] { picker.hidePicker(); });

    ScreenshotCanvasColorSamplerWindow sampler;
    checkCycles(
        "canvas sampler",
        [&]() {
            sampler.beginSampling(&owner);
            sampler.updateSample(image, owner.mapToGlobal(QPoint(20, 20)));
            return &sampler;
        },
        [&] { sampler.updateSample(image, owner.mapToGlobal(QPoint(40, 40))); },
        [&] { sampler.endSampling(); });

    QWidget trigger(&toolbar);
    trigger.setGeometry(20, 10, 40, 30);
    trigger.show();
    adqt::widgets::AdPopover popover;
    popover.setSourceWidget(&trigger);
    popover.setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    auto* content = new QWidget;
    content->setFixedSize(80, 40);
    popover.setContentWidget(content);
    checkCycles(
        "popover",
        [&]() {
            popover.show();
            return content->window();
        },
        [&] { popover.refreshPopupLayout(); }, [&] { popover.hide(); });
    const auto findSurface = [](const QString& name) -> QWidget* {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == name && widget->isVisible())
                return widget;
        }
        return nullptr;
    };
    adqt::widgets::AdSelect select(&toolbar);
    select.setGeometry(70, 10, 100, 30);
    select.setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::QtTool);
    select.show();
    checkCycles(
        "select",
        [&]() {
            select.showPopup();
            return findSurface(QStringLiteral("adselect-popup"));
        },
        [&] { select.move(select.pos() + QPoint(1, 0)); }, [&] { select.hidePopup(); });

    adqt::widgets::AdTooltip tooltip;
    tooltip.setTargetWidget(&trigger);
    tooltip.setLayerMode(adqt::widgets::AdTooltip::LayerMode::TopLevelTransient);
    tooltip.setTriggers(adqt::widgets::AdTooltip::Trigger::Click);
    tooltip.setText(QStringLiteral("Stacking tooltip"));
    checkCycles(
        "tooltip",
        [&]() {
            tooltip.show();
            return findSurface(QStringLiteral("adtooltip-surface"));
        },
        [&] { trigger.move(trigger.pos() + QPoint(1, 0)); }, [&] { tooltip.hide(); });

    // Isolated busy surfaces are a Windows-only presentation; other platforms render inline.
    adqt::widgets::AdButton button(&toolbar);
    button.setGeometry(20, 10, 100, 30);
    button.setBusyIndicatorPresentation(
        adqt::widgets::AdButton::BusyIndicatorPresentation::IsolatedSurface);
    button.show();
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        checkCycles(
            "busy indicator",
            [&]() {
                button.setBusy(true);
                return button.busyIndicatorSurface();
            },
            [&] { button.move(button.pos() + QPoint(1, 0)); }, [&] { button.setBusy(false); });
    }
    storage.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--style-binding-only"))) {
        screenshotStyleBindingFollowsToolbarAttachment();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--stacking-only"))) {
        auxiliaryWindowsPreserveOwnerStacking();
        return 0;
    }
    canvasSamplerFollowsSessionOwner();
    if (application.arguments().contains(QStringLiteral("--canvas-sampler-only")))
        return 0;
    auxiliaryWindowsPreserveOwnerStacking();
    pickerLifetimeFollowsExplicitSessionOperations();
    visibleRecaptureWindowsIncludePicker();
    invocationMonitorOwnsThePreparedSurface();
    startupPickerUsesResolvedOwnerWithoutSamplingNativeCursor();
    return 0;
}
