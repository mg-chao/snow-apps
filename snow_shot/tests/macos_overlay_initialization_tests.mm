#include "snow_shot/platform/screenshotnative.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include "widgets/modal.h"
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QEventLoop>
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
                        !native.hidesOnDeactivate,
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
    snow_shot::platform::configureScreenshotToolbarWindow(&toolbar);
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
    for (int attempt = 0; attempt != 2; ++attempt) {
        overlay.show();
        selectionToolbar.show();
        toolbar.show();
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
        modal->setWindowModality(Qt::ApplicationModal);
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
            require(NSApp.modalWindow == reinterpret_cast<NSView*>(surface->winId()).window,
                    "the test must exercise an actual Cocoa modal session");
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
        if (attempt == 0)
            modal->reject();
        else
            modal->accept();
        delete modal;
        finishNativeModalTransition();
        if (cocoa) {
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
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool cocoa = QGuiApplication::platformName() == QStringLiteral("cocoa");
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
        if (cocoa)
            screenshotNativeSettingsFollowOwnership();
        screenshotWindowsKeepTheirStackingOrder(cocoa);
    }
    return 0;
}
