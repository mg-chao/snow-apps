#include "snow_shot/platform/screenshotnative.h"
#include "platform/macos/capturewindowlayers_p.h"
#include "presentation/pinned/pinnedwindowplatform.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include "widgets/modal.h"
#include "widgets/popover.h"
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QEventLoop>
#include <QFileDialog>
#include <QTimer>
#include <QEvent>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <cstdlib>
#include <iostream>

@interface ScreenshotLevelChanges : NSObject {
  @public
    bool changed;
}
@end
@implementation ScreenshotLevelChanges
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
    Q_UNUSED(keyPath);
    Q_UNUSED(object);
    Q_UNUSED(context);
    changed |= [change[NSKeyValueChangeOldKey] integerValue] !=
               [change[NSKeyValueChangeNewKey] integerValue];
}
@end

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
class OverlayFixture final : public QWidget {
  public:
    OverlayFixture()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {}
    void recreateSurface() {
        destroy();
    }

  protected:
    bool event(QEvent* event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::Show)
            snow_shot::platform::configureScreenshotOverlayWindow(this);
        return handled;
    }
};
class ToolFixture final : public QWidget {
  public:
    explicit ToolFixture(QWidget* parent = nullptr)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        resize(32, 32);
    }
    void recreateSurface() {
        hide();
        destroy();
    }
};

void finishNativeModalTransition() {
    // Unlike processEvents(), an event loop drives Cocoa's native modal session
    // and its deferred cleanup. Wait for its idle boundary, not a timer delay.
    QEventLoop loop;
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
                     &loop, &QEventLoop::quit, Qt::QueuedConnection);
    loop.exec();
}

void captureFamiliesFollowOwnership() {
    using namespace snow_shot::platform::detail;
    QWindow screenshot;
    screenshot.setProperty(kScreenshotLayer, kOverlayLayer);
    QWindow recording;
    recording.setProperty(kScreenshotLayer, kOverlayLayer);
    recording.setProperty(kCaptureFamily, static_cast<int>(CaptureFamily::Recording));
    QWindow toolbar;
    toolbar.setProperty(kScreenshotLayer, kToolbarLayer);
    toolbar.setProperty(kCaptureFamily, static_cast<int>(CaptureFamily::Recording));
    QWindow popup;
    QWindow nested;
    nested.setTransientParent(&popup);
    for (QWindow* owner : {&recording, &screenshot, &toolbar}) {
        popup.setTransientParent(owner);
        const auto parent = captureLayer(owner);
        const auto child = captureLayer(&popup);
        require(child.family == parent.family && child.offset() > parent.offset(),
                "pooled popups must inherit their current owner's capture family");
        require(captureLayer(&nested).offset() > child.offset(),
                "nested popups must remain above their parent");
    }
    popup.setModality(Qt::WindowModal);
    const ModalFloors floors{20, 7};
    require(captureLayer(&popup, floors).layer == 7,
            "recording modals must not inherit the screenshot modal floor");
    popup.setTransientParent(&screenshot);
    require(captureLayer(&popup, floors).layer == 20,
            "reparented modals must use the new family's floor");
    popup.setTransientParent(nullptr);
    require(!captureLayer(&popup).valid() && !captureLayer(&nested).valid(),
            "detached popups must release inherited capture roles");
    require(captureLayer(&recording).offset() == -128 && captureLayer(&screenshot).offset() == 0,
            "recording must occupy the reserved band below screenshots");
    require(CaptureLayer{CaptureFamily::Recording, 1000}.offset() == -1,
            "deep recording descendants must never enter the screenshot band");
}

void captureFamiliesKeepNativeOrder() {
    using namespace snow_shot::platform;
    using namespace snow_shot::presentation;
    OverlayFixture screenshot;
    ToolFixture pin;
    ToolFixture recording;
    ToolFixture toolbar;
    ToolFixture popup;
    ToolFixture nested;
    for (QWidget* widget : {static_cast<QWidget*>(&screenshot), static_cast<QWidget*>(&pin),
                            static_cast<QWidget*>(&recording), static_cast<QWidget*>(&toolbar),
                            static_cast<QWidget*>(&popup), static_cast<QWidget*>(&nested)}) {
        widget->setGeometry(100, 100, 80, 80);
        static_cast<void>(widget->winId());
    }
    auto pinnedPlatform = createPinnedWindowPlatform(static_cast<QWidget*>(&pin));
    require(pinnedPlatform->attach(), "pin must attach to its real Cocoa policy");
    configureScreenRecordingAreaWindow(static_cast<QWidget*>(&recording));
    configureScreenRecordingToolbarWindow(static_cast<QWidget*>(&toolbar));
    toolbar.windowHandle()->setTransientParent(recording.windowHandle());
    popup.windowHandle()->setTransientParent(toolbar.windowHandle());
    nested.windowHandle()->setTransientParent(popup.windowHandle());
    auto native = [](QWidget& widget) { return reinterpret_cast<NSView*>(widget.winId()).window; };
    auto verify = [&] {
        QCoreApplication::processEvents();
        require(native(pin).level < native(recording).level &&
                    native(recording).level < native(toolbar).level &&
                    native(toolbar).level < native(popup).level &&
                    native(popup).level < native(nested).level &&
                    native(nested).level < native(screenshot).level,
                "native levels must enforce screenshot > recording and its popups > pin");
        require(native(recording).level > CGWindowLevelForKey(kCGMainMenuWindowLevelKey) &&
                    native(recording).level > CGWindowLevelForKey(kCGDockWindowLevelKey),
                "recording must retain its position above system chrome");
        CFArrayRef windows =
            CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
        auto position = [&](NSWindow* window) {
            for (CFIndex i = 0; i < CFArrayGetCount(windows); ++i) {
                auto* info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
                int number = 0;
                CFNumberGetValue(
                    static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowNumber)),
                    kCFNumberIntType, &number);
                if (number == window.windowNumber)
                    return i;
            }
            return CFIndex(-1);
        };
        require(windows != nullptr, "WindowServer must provide window ordering");
        CFIndex previous = -1;
        for (QWidget* widget : {static_cast<QWidget*>(&screenshot), static_cast<QWidget*>(&nested),
                                static_cast<QWidget*>(&popup), static_cast<QWidget*>(&toolbar),
                                static_cast<QWidget*>(&recording), static_cast<QWidget*>(&pin)}) {
            const CFIndex current = position(native(*widget));
            require(current >= 0 && current > previous,
                    "WindowServer must preserve the capture hierarchy after raising windows");
            previous = current;
        }
        CFRelease(windows);
    };
    for (int attempt = 0; attempt != 2; ++attempt) {
        for (QWidget* widget : {static_cast<QWidget*>(&screenshot), static_cast<QWidget*>(&nested),
                                static_cast<QWidget*>(&popup), static_cast<QWidget*>(&toolbar),
                                static_cast<QWidget*>(&recording), static_cast<QWidget*>(&pin)})
            widget->show();
        for (QWidget* widget :
             {static_cast<QWidget*>(&screenshot), static_cast<QWidget*>(&recording),
              static_cast<QWidget*>(&pin), static_cast<QWidget*>(&toolbar)}) {
            widget->raise();
            widget->activateWindow();
            verify();
        }
        for (bool topmost : {false, true}) {
            require(pinnedPlatform->setStaysOnTop(topmost), "pin topmost toggle must succeed");
            verify();
        }
        const NSInteger recordingLevel = native(recording).level;
        native(recording).level = NSNormalWindowLevel;
        require(native(recording).level == recordingLevel,
                "Qt level resets must not break the recording band");
        popup.windowHandle()->setTransientParent(screenshot.windowHandle());
        require(native(popup).level > native(screenshot).level,
                "reparented recording popups must enter the screenshot band immediately");
        popup.windowHandle()->setTransientParent(toolbar.windowHandle());
        verify();
        {
            ToolFixture modal(static_cast<QWidget*>(&recording));
            modal.setWindowModality(Qt::WindowModal);
            modal.show();
            finishNativeModalTransition();
            require(native(modal).level > native(nested).level &&
                        native(modal).level < native(screenshot).level,
                    "recording modals must stay above recording popups and below screenshots");
            modal.hide();
            finishNativeModalTransition();
        }
        verify();
        nested.recreateSurface();
        popup.recreateSurface();
        toolbar.recreateSurface();
        recording.recreateSurface();
        static_cast<void>(recording.winId());
        static_cast<void>(toolbar.winId());
        static_cast<void>(popup.winId());
        static_cast<void>(nested.winId());
        toolbar.windowHandle()->setTransientParent(recording.windowHandle());
        popup.windowHandle()->setTransientParent(toolbar.windowHandle());
        nested.windowHandle()->setTransientParent(popup.windowHandle());
    }
}

void screenshotNativeSettingsFollowOwnership() {
    OverlayFixture overlay;
    ToolFixture ordinaryOwner;
    ToolFixture tool;
    overlay.show();
    ordinaryOwner.show();
    for (int surface = 0; surface != 2; ++surface) {
        tool.show();
        tool.windowHandle()->setTransientParent(ordinaryOwner.windowHandle());
        finishNativeModalTransition();
        NSWindow* native = reinterpret_cast<NSView*>(tool.winId()).window;
        for (int reuse = 0; reuse != 2; ++reuse) {
            const auto ordinaryBehavior = NSWindowCollectionBehaviorMoveToActiveSpace |
                                          NSWindowCollectionBehaviorFullScreenAuxiliary;
            native.level = NSFloatingWindowLevel;
            native.collectionBehavior = ordinaryBehavior;
            native.hidesOnDeactivate = YES;
            tool.windowHandle()->setTransientParent(overlay.windowHandle());
            const auto screenshotBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                            NSWindowCollectionBehaviorFullScreenAuxiliary;
            require(native.level > reinterpret_cast<NSView*>(overlay.winId()).window.level &&
                        native.collectionBehavior == screenshotBehavior &&
                        !native.hidesOnDeactivate && native.movable,
                    "capture ownership must immediately apply all screenshot native settings");
            // Repeated synchronization must not replace the saved ordinary settings.
            overlay.raise();
            tool.raise();
            tool.windowHandle()->setTransientParent(ordinaryOwner.windowHandle());
            require(native.collectionBehavior == ordinaryBehavior && native.hidesOnDeactivate,
                    "leaving capture must restore Space and deactivation settings");

            tool.windowHandle()->setTransientParent(overlay.windowHandle());
            const NSInteger screenshotLevel = native.level;
            // Qt can request different settings while capture owns the native surface.
            // Preserve those requests without allowing them to break capture stacking.
            native.level = NSNormalWindowLevel;
            native.collectionBehavior = NSWindowCollectionBehaviorDefault;
            native.hidesOnDeactivate = YES;
            require(native.level == screenshotLevel &&
                        native.collectionBehavior == screenshotBehavior &&
                        !native.hidesOnDeactivate,
                    "ordinary native requests must not override active capture settings");
            tool.windowHandle()->setTransientParent(nullptr);
            require(native.level == NSNormalWindowLevel &&
                        native.collectionBehavior == NSWindowCollectionBehaviorDefault &&
                        native.hidesOnDeactivate,
                    "detaching capture must restore the latest ordinary native requests");
            native.level = NSFloatingWindowLevel;
            native.collectionBehavior = ordinaryBehavior;
            native.hidesOnDeactivate = NO;
            require(native.level == NSFloatingWindowLevel &&
                        native.collectionBehavior == ordinaryBehavior && !native.hidesOnDeactivate,
                    "released surfaces must accept native settings without interception");
        }
        tool.recreateSurface();
    }
}

void screenshotWindowsKeepTheirStackingOrder(bool cocoa) {
    OverlayFixture overlay;
    overlay.resize(64, 64);
    ToolFixture toolbar(&overlay);
    // Materialize tools before raising the overlay, as the pooled UI does.
    static_cast<void>(toolbar.winId());
    const Class toolbarClass =
        cocoa ? object_getClass(reinterpret_cast<NSView*>(toolbar.winId()).window) : Nil;
    snow_shot::platform::configureScreenshotToolbarWindow(static_cast<QWidget*>(&toolbar));
    if (cocoa)
        require(object_getClass(reinterpret_cast<NSView*>(toolbar.winId()).window) == toolbarClass,
                "stacking must preserve AppKit's native window class and observer bookkeeping");
    QWidget selectionToolbar(&overlay);
    ToolFixture selectionEditor(&selectionToolbar);
    ToolFixture recognition;
    ToolFixture popup;
    ToolFixture nestedPopup;
    ToolFixture unrelated;
    unrelated.show();
    const auto level = [](QWidget& widget) {
        return reinterpret_cast<NSView*>(widget.winId()).window.level;
    };
    const NSInteger unrelatedLevel = cocoa ? level(unrelated) : 0;
    for (int attempt = 0; attempt != 4; ++attempt) {
        overlay.show();
        selectionToolbar.show();
        toolbar.show();
        if (cocoa) {
            for (QWidget* controlled : {static_cast<QWidget*>(&overlay),
                                        static_cast<QWidget*>(static_cast<QWidget*>(&toolbar))}) {
                NSWindow* native = reinterpret_cast<NSView*>(controlled->winId()).window;
                require(!native.movable && !native.movableByWindowBackground,
                        "Qt-controlled screenshot surfaces must disable AppKit dragging");
            }
        }
        static_cast<void>(recognition.winId());
        recognition.windowHandle()->setTransientParent(overlay.windowHandle());
        recognition.show();
        selectionEditor.show();
        // AdQt popovers can have only a QWindow transient owner, with no QWidget parent.
        static_cast<void>(popup.winId());
        popup.windowHandle()->setTransientParent(toolbar.windowHandle());
        popup.show();
        static_cast<void>(nestedPopup.winId());
        nestedPopup.windowHandle()->setTransientParent(popup.windowHandle());
        nestedPopup.show();
        QCoreApplication::processEvents();
        for (int interaction = 0; interaction != 2; ++interaction) {
            overlay.raise();
            toolbar.raise();
            QCoreApplication::processEvents();
            require(selectionToolbar.isVisible(), "the child selection toolbar must stay visible");
            if (cocoa) {
                require(level(toolbar) > level(overlay),
                        "the drawing toolbar must remain above the screenshot after canvas raises");
                require(level(selectionEditor) > level(toolbar),
                        "selection editors must remain above the drawing toolbar");
                require(level(popup) > level(toolbar),
                        "transient-only tool popovers must remain above the drawing toolbar");
                require(level(nestedPopup) > level(popup),
                        "nested popovers must remain above their parent popup");
                require(level(unrelated) == unrelatedLevel,
                        "unrelated application tools must retain their original level");
            }
        }
        auto* modal = new adqt::widgets::AdModal(&overlay);
        modal->setMode(adqt::widgets::AdModal::Mode::Window);
        modal->setWindowModality(attempt < 2 ? Qt::ApplicationModal : Qt::WindowModal);
        auto* content = new QWidget;
        modal->setContentWidget(content);
        modal->open();
        finishNativeModalTransition();
        QWidget* surface = content->window();
        recognition.raise();
        toolbar.raise();
        require(QApplication::activeModalWidget() == surface,
                "the selection editor must own modal interaction");
        if (cocoa) {
            NSWindow* nativeModal = reinterpret_cast<NSView*>(surface->winId()).window;
            require(attempt < 2 ? NSApp.modalWindow == nativeModal
                                : nativeModal.sheetParent ==
                                      reinterpret_cast<NSView*>(overlay.winId()).window,
                    "the test must exercise a Cocoa modal session or window-modal sheet");
            require(level(*surface) > level(recognition) && level(*surface) > level(toolbar) &&
                        level(*surface) > level(nestedPopup),
                    "the selection modal must cover OCR results, toolbars, and their popups");
        }
        // Popups within the modal must still appear above it.
        {
            ToolFixture modalPopup(surface);
            modalPopup.show();
            finishNativeModalTransition();
            if (cocoa)
                require(level(modalPopup) > level(*surface),
                        "modal-owned popups must remain above the modal");
        }
        ScreenshotLevelChanges* trace = nil;
        NSWindow* toolbarNative = nil;
        NSWindow* recognitionNative = nil;
        if (cocoa) {
            trace = [ScreenshotLevelChanges new];
            toolbarNative = reinterpret_cast<NSView*>(toolbar.winId()).window;
            recognitionNative = reinterpret_cast<NSView*>(recognition.winId()).window;
            [toolbarNative addObserver:trace
                            forKeyPath:@"level"
                               options:NSKeyValueObservingOptionOld | NSKeyValueObservingOptionNew
                               context:nullptr];
            [recognitionNative
                addObserver:trace
                 forKeyPath:@"level"
                    options:NSKeyValueObservingOptionOld | NSKeyValueObservingOptionNew
                    context:nullptr];
        }
        if (attempt % 2 == 0)
            modal->reject();
        else
            modal->accept();
        delete modal;
        finishNativeModalTransition();
        if (cocoa) {
            require(NSApp.keyWindow == reinterpret_cast<NSView*>(overlay.winId()).window,
                    "closing a screenshot modal must return native keyboard focus to its owner");
            [toolbarNative removeObserver:trace forKeyPath:@"level"];
            [recognitionNative removeObserver:trace forKeyPath:@"level"];
            const bool changed = trace->changed;
            [trace release];
            require(!changed,
                    "modal closure must not change toolbar or OCR levels, even transiently");
        }
        require(toolbar.isVisible() && recognition.isVisible() &&
                    QApplication::activeModalWidget() == nullptr,
                "closing selection editing must preserve the screenshot tools and OCR result");
        if (cocoa) {
            require(level(toolbar) > level(overlay) && level(recognition) > level(overlay),
                    "ending Cocoa's modal session must not bury the toolbar or OCR result");
            require(reinterpret_cast<NSView*>(toolbar.winId()).window.visible &&
                        reinterpret_cast<NSView*>(recognition.winId()).window.visible,
                    "the native toolbar and OCR window must remain visible after modal cleanup");
        }
        // Reuse a transient-only tool outside and inside a screenshot session.
        // Native enforcement must stop as soon as it no longer belongs to capture.
        for (int reuse = 0; reuse != 2; ++reuse) {
            recognition.windowHandle()->setTransientParent(unrelated.windowHandle());
            if (cocoa) {
                recognitionNative.level = NSNormalWindowLevel;
                require(recognitionNative.level == NSNormalWindowLevel,
                        "a reused tool outside capture must accept ordinary native levels");
            }
            recognition.windowHandle()->setTransientParent(overlay.windowHandle());
            if (cocoa)
                require(level(recognition) > level(overlay),
                        "a reused OCR surface must reacquire screenshot stacking immediately");
        }
        recognition.hide();
        finishNativeModalTransition();
        require(!recognition.isVisible(), "stacking must not reopen intentionally hidden results");
        if (cocoa)
            require(!recognitionNative.visible,
                    "native OCR visibility must follow an explicit hide");
        recognition.recreateSurface();
        nestedPopup.recreateSurface();
        popup.recreateSurface();
        selectionEditor.recreateSurface();
        toolbar.recreateSurface();
        overlay.hide();
        overlay.recreateSurface();
    }
}
void nativeFilePanelsCoverScreenshotModals(bool cocoa) {
    OverlayFixture overlay;
    overlay.show();
    adqt::widgets::AdModal modal(&overlay);
    modal.setMode(adqt::widgets::AdModal::Mode::Window);
    modal.setWindowModality(Qt::ApplicationModal);
    auto* content = new QWidget;
    modal.setContentWidget(content);
    for (bool withModal : {false, true}) {
        if (withModal)
            modal.open();
        finishNativeModalTransition();
        QWidget* owner = withModal ? content->window() : &overlay;
        for (bool directory : {false, true}) {
            QFileDialog dialog(owner);
            dialog.setAcceptMode(directory ? QFileDialog::AcceptOpen : QFileDialog::AcceptSave);
            dialog.setFileMode(directory ? QFileDialog::Directory : QFileDialog::AnyFile);
            bool inspected = false;
            NSSavePanel* inspectedPanel = nil;
            QTimer inspect;
            QObject::connect(&inspect, &QTimer::timeout, &dialog, [&] {
                if (cocoa) {
                    NSSavePanel* panel = nil;
                    for (NSWindow* window in NSApp.windows) {
                        if (window.visible && [window isKindOfClass:[NSSavePanel class]])
                            panel = static_cast<NSSavePanel*>(window);
                    }
                    if (!panel)
                        return;
                    require([panel isKindOfClass:[NSOpenPanel class]] == directory,
                            "the native fixture must use the requested save or directory panel");
                    require(panel.level > reinterpret_cast<NSView*>(owner->winId()).window.level &&
                                panel.level >
                                    reinterpret_cast<NSView*>(overlay.winId()).window.level,
                            "native file panels must cover the screenshot and its modal");
                    inspected = true;
                    inspectedPanel = [panel retain];
                    [panel cancel:nil];
                } else {
                    inspected = true;
                    dialog.reject();
                }
            });
            inspect.start(0);
            QTimer::singleShot(5000, &dialog, &QDialog::reject);
            dialog.exec();
            inspect.stop();
            require(inspected, "the file dialog must be inspected before closing");
            if (cocoa) {
                require(inspectedPanel.level <
                            reinterpret_cast<NSView*>(overlay.winId()).window.level,
                        "closing a native file panel must restore its ordinary window level");
                [inspectedPanel release];
            }
        }
        modal.reject();
    }
}

void adqtPopupPreservesScreenshotLayers(bool cocoa) {
    OverlayFixture overlay;
    overlay.resize(400, 300);
    ToolFixture toolbar(&overlay);
    toolbar.resize(200, 60);
    snow_shot::platform::configureScreenshotToolbarWindow(static_cast<QWidget*>(&toolbar));
    QWidget trigger(static_cast<QWidget*>(&toolbar));
    trigger.setGeometry(40, 10, 80, 30);
    adqt::widgets::AdPopover popover(&trigger);
    popover.setSourceWidget(&trigger);
    popover.setPopupLayerMode(adqt::widgets::AdPopover::PopupLayerMode::QtTool);
    auto* content = new QWidget;
    content->setFixedSize(100, 60);
    popover.setContentWidget(content);
    overlay.show();
    toolbar.show();
    popover.show();
    QCoreApplication::processEvents();
    toolbar.raise();
    require(content->isVisible(), "the screenshot toolbar's AdQt popup must stay visible");
    if (cocoa) {
        NSWindow* popup = reinterpret_cast<NSView*>(content->window()->winId()).window;
        NSWindow* owner = reinterpret_cast<NSView*>(toolbar.winId()).window;
        require(popup.parentWindow == owner && popup.level > owner.level,
                "Cocoa popup ownership must preserve the screenshot's explicit layers");
    }
    ToolFixture recognition;
    snow_shot::platform::configureScreenshotRecognitionWindow(&recognition);
    static_cast<void>(recognition.winId());
    recognition.windowHandle()->setTransientParent(overlay.windowHandle());
    for (int attempt = 0; attempt != 2; ++attempt) {
        recognition.show();
        recognition.raise();
        recognition.activateWindow();
        QCoreApplication::processEvents();
        popover.show();
        QCoreApplication::processEvents();
        recognition.raise();
        toolbar.raise();
        QCoreApplication::processEvents();
        require(content->isVisible(), "the toolbar popover must open above recognition results");
        if (cocoa) {
            NSWindow* result = reinterpret_cast<NSView*>(recognition.winId()).window;
            NSWindow* canvas = reinterpret_cast<NSView*>(overlay.winId()).window;
            NSWindow* owner = reinterpret_cast<NSView*>(toolbar.winId()).window;
            NSWindow* popup = reinterpret_cast<NSView*>(content->window()->winId()).window;
            require(result.level > canvas.level && result.level < owner.level &&
                        owner.level < popup.level,
                    "recognition results must stay below the toolbar and its open popover");
        }
        recognition.recreateSurface();
        static_cast<void>(recognition.winId());
        recognition.windowHandle()->setTransientParent(overlay.windowHandle());
    }
    popover.hide();
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool cocoa = QGuiApplication::platformName() == QStringLiteral("cocoa");
    if (app.arguments().contains(QStringLiteral("--recognition-stacking-only"))) {
        @autoreleasepool {
            adqtPopupPreservesScreenshotLayers(cocoa);
        }
        return 0;
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        auto display = ScreenshotGeometryMapper::preCaptureDisplayModel(*screen);
        require(display.active && display.image.isNull(), "preparation must not require an image");
        require(display.logicalRect == screen->geometry(),
                "preparation must cover the full screen");
        if (cocoa) {
            require(display.nativeDisplayId != 0 &&
                        display.stableId ==
                            QStringLiteral("display:%1").arg(display.nativeDisplayId),
                    "initial selector results must have a matching native display identity");
            require(display.canvasUsesPoints && display.capturedLogicalRect == screen->geometry(),
                    "the initial canvas must use the captured frame's point coordinate space");
        }
        ScreenshotDisplaySession session;
        session.appendDisplay(display);
        ScreenshotGeometryMapper geometry;
        geometry.rebuild(session);
        const QRectF physical = display.physicalRect;
        const QRectF expected = session.displayAt(0).canvasRect;
        require(geometry.canvasRectForPhysicalRect(session, physical, display.stableId) == expected,
                "a selection received before the image must map onto the initial canvas");
        require(!expected.isEmpty(),
                "the initial selection must be visible without mouse movement");
        if (cocoa)
            require(expected.size() == QSizeF(screen->geometry().size()),
                    "initial Retina selection must not be enlarged by the backing scale");
    }
    OverlayFixture overlay;
    overlay.resize(32, 32);
    for (int attempt = 0; attempt != 2; ++attempt) {
        overlay.show();
        QCoreApplication::processEvents();
        if (cocoa) {
            NSWindow* window = reinterpret_cast<NSView*>(overlay.winId()).window;
            require(window.level > CGWindowLevelForKey(kCGMainMenuWindowLevelKey) &&
                        window.level > CGWindowLevelForKey(kCGDockWindowLevelKey),
                    "the overlay must stay above the menu bar and Dock on each reveal");
            require(!window.hidesOnDeactivate, "the overlay must remain visible when deactivated");
            require((window.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) !=
                        0,
                    "the overlay must support full-screen application spaces");
        }
        overlay.hide();
        overlay.recreateSurface();
    }
    // Native windows are autoreleased after Qt destroys their surfaces. Draining
    // the pool is essential: otherwise NSWindow/KVO teardown crashes go untested.
    @autoreleasepool {
        captureFamiliesFollowOwnership();
        if (cocoa) {
            captureFamiliesKeepNativeOrder();
            screenshotNativeSettingsFollowOwnership();
        }
        screenshotWindowsKeepTheirStackingOrder(cocoa);
        adqtPopupPreservesScreenshotLayers(cocoa);
        nativeFilePanelsCoverScreenshotModals(cocoa);
    }
    return 0;
}
