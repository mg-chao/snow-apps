#include "snow_shot/presentation/floatingtoolbarcontroller.h"
#include "snow_shot/presentation/screenshotcontentdrop.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/floatingtoolbarsettings.h"
#include "platform/macos/capturewindowlayers_p.h"
#include "../../cmake/test-support/macos_native_input.h"

#import <AppKit/AppKit.h>
#include <QApplication>
#include <QAbstractButton>
#include <QBuffer>
#include <QClipboard>
#include <QEventLoop>
#include <QFile>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <iostream>
#include <stdexcept>

// An AppKit source exercises Qt's native pasteboard conversion, rather than
// delivering synthetic QDragEnterEvent/QDropEvent objects to the widget.
@interface FloatingToolbarDragSource : NSView <NSDraggingSource>
@property(nonatomic, retain) NSPasteboardItem* item;
@property(nonatomic) NSUInteger started;
@property(nonatomic) NSUInteger finished;
@property(nonatomic) NSDragOperation result;
@end

@implementation FloatingToolbarDragSource
- (void)mouseDown:(NSEvent*)event {
    NSDraggingItem* drag = [[NSDraggingItem alloc] initWithPasteboardWriter:self.item];
    [drag setDraggingFrame:NSMakeRect(0, 0, 32, 32)
                  contents:[NSImage imageNamed:NSImageNameApplicationIcon]];
    ++self.started;
    [self beginDraggingSessionWithItems:@[ drag ] event:event source:self];
    [drag release];
}
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    Q_UNUSED(session);
    Q_UNUSED(context);
    return NSDragOperationCopy | NSDragOperationMove;
}
- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)point
              operation:(NSDragOperation)operation {
    Q_UNUSED(session);
    Q_UNUSED(point);
    self.result = operation;
    ++self.finished;
}
- (void)dealloc {
    [_item release];
    [super dealloc];
}
@end

namespace {
using namespace snow_shot::platform::detail;
using snow_shot::presentation::FloatingToolbarController;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void pump() {
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
}

bool waitFor(const std::function<bool()>& predicate) {
    if (predicate())
        return true;
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (predicate())
            loop.quit();
    });
    poll.start(5);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    return predicate();
}

QWidget* surface(const char* name) {
    for (QWidget* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QLatin1String(name))
            return widget;
    throw std::runtime_error("floating toolbar surface must exist");
}

void postMouse(CGEventType type, const QPoint& position) {
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, type, CGPointMake(position.x(), position.y()), kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

void verifyDropPolicy() {
    QWindow desktop;
    desktop.setProperty(kScreenshotLayer, kToolbarLayer);
    desktop.setProperty(kCaptureFamily, static_cast<int>(CaptureFamily::DesktopToolbar));
    QWindow popup;
    popup.setTransientParent(&desktop);
    QWindow nested;
    nested.setTransientParent(&popup);
    const auto toolbarLevel = captureWindowLevel(captureLayer(&desktop));
    const auto popupLevel = captureWindowLevel(captureLayer(&popup));
    const auto nestedLevel = captureWindowLevel(captureLayer(&nested));
    require(toolbarLevel > CGWindowLevelForKey(kCGDockWindowLevelKey) &&
                toolbarLevel > CGWindowLevelForKey(kCGFloatingWindowLevelKey),
            "desktop toolbar must stay above ordinary windows and the Dock");
    require(toolbarLevel < popupLevel && popupLevel < nestedLevel &&
                nestedLevel < CGWindowLevelForKey(kCGDraggingWindowLevelKey),
            "desktop toolbar and its popups must stay below native content drags");
    require(captureWindowLevel({CaptureFamily::DesktopToolbar, 1000}) <
                CGWindowLevelForKey(kCGDraggingWindowLevelKey),
            "deep desktop descendants must not enter the dragging window level");
    popup.setModality(Qt::WindowModal);
    ModalFloors floors{};
    floors[static_cast<std::size_t>(CaptureFamily::DesktopToolbar)] = 20;
    require(captureLayer(&popup, floors).layer == 20 &&
                captureWindowLevel(nativePanelLayer(captureLayer(&desktop), floors)) > toolbarLevel,
            "desktop modals and native sheets must inherit their owner's band");
    QWindow screenshot;
    screenshot.setProperty(kScreenshotLayer, kOverlayLayer);
    popup.setTransientParent(&screenshot);
    require(captureLayer(&popup).family == CaptureFamily::Screenshot &&
                captureWindowLevel(captureLayer(&popup)) >
                    captureWindowLevel(captureLayer(&screenshot)),
            "pooled popups must return to screenshot levels with a screenshot owner");
}

class NativeDragFixture final {
  public:
    NativeDragFixture() {
        previous = [NSWorkspace.sharedWorkspace.frontmostApplication retain];
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
        source =
            [[NSPanel alloc] initWithContentRect:NSMakeRect(available.left() + 100,
                                                            top - available.top() - 200, 160, 100)
                                       styleMask:NSWindowStyleMaskBorderless
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
        view = [FloatingToolbarDragSource new];
        source.contentView = view;
        source.level = NSStatusWindowLevel;
        source.hidesOnDeactivate = NO;
        [source orderFrontRegardless];
        macActivateApplication();
        pump();
    }
    ~NativeDragFixture() {
        [source orderOut:nil];
        [source release];
        [view release];
        [previous activateWithOptions:0];
        [previous release];
    }
    void drop(NSPasteboardItem* item, QWidget* target) {
        view.item = item;
        const NSRect frame = source.frame;
        const QPoint start(qRound(NSMidX(frame)),
                           qRound(NSMaxY(NSScreen.screens.firstObject.frame) - NSMidY(frame)));
        QPoint destination = target->mapToGlobal(target->rect().center());
        if (target->objectName() == QStringLiteral("floatingToolbarIconWindow")) {
            auto* icon =
                target->findChild<QAbstractButton*>(QStringLiteral("floatingToolbarSnowflake"));
            const QRect visible = icon->geometry().intersected(target->rect());
            destination = target->mapToGlobal(visible.center());
        }
        const NSUInteger started = view.started;
        const NSUInteger finished = view.finished;
        require(macWindowReceivesPoint(target, destination),
                "native drop destination must not be covered by another window");
        // beginDraggingSession can enter AppKit's tracking loop synchronously.
        // Post the rest of the gesture independently of that main-thread loop.
        const auto queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), queue, ^{
          postMouse(kCGEventMouseMoved, start);
          postMouse(kCGEventLeftMouseDown, start);
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), queue, ^{
          postMouse(kCGEventLeftMouseDragged, start + QPoint(24, 0));
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), queue, ^{
          postMouse(kCGEventLeftMouseDragged, destination);
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), queue, ^{
          postMouse(kCGEventLeftMouseUp, destination);
        });
        require(waitFor([&] { return view.finished == finished + 1; }),
                "AppKit content drag must finish");
        require(view.started == started + 1, "AppKit source must begin a content drag");
        require(view.result == NSDragOperationCopy,
                "native floating toolbar drop must complete with the copy action");
    }

  private:
    MacCursorRestore cursor;
    NSRunningApplication* previous = nil;
    NSPanel* source = nil;
    FloatingToolbarDragSource* view = nil;
};

void verifyNativeDrops() {
    storage::FloatingToolbarSettings settings;
    settings.setHideInFullscreen(false);
    settings.setToolbarMode(false);
    settings.setPlacement({{QStringLiteral("x"), 0.0}, {QStringLiteral("y"), 0.35}});
    settings.setEnabled(true);
    FloatingToolbarController controller(nullptr, [](QScreen*) { return false; });
    pump();
    ScreenshotClipboardContentSnapshot snapshot;
    QStringList paths;
    int delivered = 0;
    QObject::connect(&controller, &FloatingToolbarController::contentDropped, &controller,
                     [&](const auto& content, const auto& files) {
                         snapshot = content;
                         paths = files;
                         ++delivered;
                     });
    // The fixture uses a named dragging pasteboard; preserve the user's clipboard.
    const NSInteger clipboardChanges = NSPasteboard.generalPasteboard.changeCount;
    NativeDragFixture source;
    QTemporaryDir files;
    const QString textPath = files.filePath(QStringLiteral("native-drop.txt"));
    QFile textFile(textPath);
    require(textFile.open(QIODevice::WriteOnly) && textFile.write("Native file text") == 16,
            "native file fixture must be created");
    textFile.close();
    for (int mode = 0; mode < 3; ++mode) {
        if (mode == 1) {
            auto* reveal =
                controller.findChild<QTimer*>(QStringLiteral("floatingToolbarRevealTimer"));
            require(reveal && QMetaObject::invokeMethod(reveal, "timeout", Qt::DirectConnection),
                    "docked icon must reveal its expanded toolbar");
        } else if (mode == 2) {
            settings.setToolbarMode(true);
        }
        pump();
        QWidget* target =
            surface(mode == 0 ? "floatingToolbarIconWindow" : "floatingToolbarWindow");
        require(target->isVisible(), "native drop surface must be visible");
        NSWindow* native = reinterpret_cast<NSView*>(target->winId()).window;
        require(native.level < CGWindowLevelForKey(kCGDraggingWindowLevelKey),
                "actual toolbar window must be below AppKit's dragging window");
        require(!native.canBecomeKeyWindow && !native.movable && !native.hidesOnDeactivate,
                "drop support must preserve passive visibility and controlled dragging");
        settings.setHideDuringCapture(false);
        pump();
        controller.setCaptureActive(QStringLiteral("selection"), true);
        controller.setCaptureActive(QStringLiteral("direct"), true);
        controller.setCaptureActive(QStringLiteral("selection"), false);
        pump();
        require(target->isVisible() &&
                    native.level > captureWindowLevel({CaptureFamily::Screenshot, kOverlayLayer}),
                "explicitly visible toolbar must remain above every active capture");
        controller.setCaptureActive(QStringLiteral("direct"), false);
        pump();
        require(native.level < CGWindowLevelForKey(kCGDraggingWindowLevelKey),
                "last capture completion must restore native content drop delivery");
        settings.setHideDuringCapture(true);
        pump();
        // Qt/native settings can be rewritten during opacity updates and reshow.
        const auto flags = target->windowHandle()->flags();
        target->windowHandle()->setFlags(flags & ~Qt::WindowStaysOnTopHint);
        target->windowHandle()->setFlags(flags);
        target->hide();
        target->show();
        pump();
        require(native.level < CGWindowLevelForKey(kCGDraggingWindowLevelKey),
                "reshowing the native toolbar must preserve its drop-capable level");
        const auto drop = [&](NSPasteboardItem* item) {
            if (mode == 1) {
                auto* reveal =
                    controller.findChild<QTimer*>(QStringLiteral("floatingToolbarRevealTimer"));
                require(QMetaObject::invokeMethod(reveal, "timeout", Qt::DirectConnection),
                        "expanded toolbar must be available before each drag");
            }
            std::cout << "Dropping " << item.types.description.UTF8String << " onto "
                      << target->objectName().toStdString() << std::endl;
            const int before = delivered;
            source.drop(item, target);
            require(delivered == before + 1, "native drop must dispatch its content exactly once");
        };
        NSPasteboardItem* text = [[[NSPasteboardItem alloc] init] autorelease];
        [text setString:@"Native toolbar text" forType:NSPasteboardTypeString];
        drop(text);
        auto decoded = decodeScreenshotDropContent(snapshot);
        require(decoded && decoded->plainText == QStringLiteral("Native toolbar text") &&
                    paths.isEmpty(),
                "native text drag must retain the source text");
        NSPasteboardItem* html = [[[NSPasteboardItem alloc] init] autorelease];
        [html setString:@"<b>Native HTML</b>" forType:NSPasteboardTypeHTML];
        drop(html);
        decoded = decodeScreenshotDropContent(snapshot);
        require(decoded && decoded->originalContent.html == QStringLiteral("<b>Native HTML</b>"),
                "native HTML drag must retain formatting");
        NSPasteboardItem* file = [[[NSPasteboardItem alloc] init] autorelease];
        [file setString:[NSURL fileURLWithPath:textPath.toNSString()].absoluteString
                forType:NSPasteboardTypeFileURL];
        drop(file);
        require(paths == QStringList{textPath} && !snapshot.isValid(),
                "native file drag must dispatch paths without reading files on the GUI thread");
        QImage pixels(4, 4, QImage::Format_ARGB32_Premultiplied);
        pixels.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        require(buffer.open(QIODevice::WriteOnly) && pixels.save(&buffer, "PNG"),
                "native image fixture must encode");
        NSBitmapImageRep* image =
            [[[NSBitmapImageRep alloc] initWithData:png.toNSData()] autorelease];
        NSPasteboardItem* tiff = [[[NSPasteboardItem alloc] init] autorelease];
        [tiff setData:image.TIFFRepresentation forType:NSPasteboardTypeTIFF];
        drop(tiff);
        decoded = decodeScreenshotDropContent(snapshot);
        require(decoded && decoded->image.size() == pixels.size() &&
                    decoded->image.pixelColor(1, 1) == QColor(Qt::red) && paths.isEmpty(),
                "native image drag must retain its pixels");
    }
    require(NSPasteboard.generalPasteboard.changeCount == clipboardChanges,
            "native content drops must preserve the system clipboard");
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        QApplication app(argc, argv);
        app.setQuitOnLastWindowClosed(false);
        QTemporaryDir directory;
        auto& storage = storage::ApplicationStorage::instance();
        try {
            verifyDropPolicy();
            if (app.arguments().contains(QStringLiteral("--native"))) {
                if (!macCanPostMouseEvents())
                    return 77;
                require(
                    storage
                        .initialize({directory.filePath(QStringLiteral("bin")), directory.path()})
                        .success,
                    "temporary storage must initialize");
                snow_shot::presentation::styles::ThemeManager::instance().initialize(app);
                verifyNativeDrops();
                storage.shutdown();
            }
        } catch (const std::exception& error) {
            storage.shutdown();
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
