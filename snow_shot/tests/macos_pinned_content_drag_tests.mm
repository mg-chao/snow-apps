// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../src/presentation/pinned/screenshotpinneddragexport.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QScreen>
#include <QScopeGuard>
#include <QTimer>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
#include <stdexcept>
#import <AppKit/AppKit.h>

// A non-Qt destination exercises the external pasteboard and completion path.
@interface SnowImageDragReceiver : NSView
@property(nonatomic, copy) NSURL* receivedURL;
@property(nonatomic) BOOL receivedImage;
@end
@implementation SnowImageDragReceiver
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return sender.draggingSourceOperationMask & NSDragOperationCopy;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    self.receivedURL = [NSURL URLFromPasteboard:sender.draggingPasteboard];
    self.receivedImage =
        [[[NSImage alloc] initWithPasteboard:sender.draggingPasteboard] autorelease] != nil;
    return self.receivedURL != nil && self.receivedImage;
}
- (void)dealloc {
    [_receivedURL release];
    [super dealloc];
}
@end

@interface SnowImageDragSource : NSView <NSDraggingSource>
@property(nonatomic, retain) NSPasteboardItem* item;
@property(nonatomic) BOOL started;
@property(nonatomic) BOOL finished;
@property(nonatomic) NSDragOperation result;
@end
@implementation SnowImageDragSource
- (void)mouseDown:(NSEvent*)event {
    self.started = YES;
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:self.item];
    [item setDraggingFrame:NSMakeRect(0, 0, 32, 32)
                  contents:[NSImage imageNamed:NSImageNameApplicationIcon]];
    [self beginDraggingSessionWithItems:@[ item ] event:event source:self];
    [item release];
}
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    Q_UNUSED(session);
    Q_UNUSED(context);
    return NSDragOperationCopy;
}
- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)point
              operation:(NSDragOperation)operation {
    Q_UNUSED(session);
    Q_UNUSED(point);
    self.result = operation;
    self.finished = YES;
}
- (void)dealloc {
    [_item release];
    [super dealloc];
}
@end

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void waitFor(const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 10000) {
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    require(predicate(), message);
}
void postMouse(CGEventType type, QPoint point, CGEventFlags flags) {
    CGEventRef event =
        CGEventCreateMouseEvent(nullptr, type, point.toCGPoint(), kCGMouseButtonLeft);
    CGEventSetFlags(event, flags);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}
void dropNativeItem(NSPasteboardItem* item, QPoint origin, QWidget& target) {
    NSPanel* panel =
        [[NSPanel alloc] initWithContentRect:NSMakeRect(origin.x(),
                                                        NSMaxY(NSScreen.screens.firstObject.frame) -
                                                            origin.y() - 100,
                                                        160, 100)
                                   styleMask:NSWindowStyleMaskBorderless
                                     backing:NSBackingStoreBuffered
                                       defer:NO];
    SnowImageDragSource* source = [SnowImageDragSource new];
    source.item = item;
    panel.contentView = source;
    panel.level = NSStatusWindowLevel;
    panel.hidesOnDeactivate = NO;
    [panel orderFrontRegardless];
    const auto cleanup = qScopeGuard([&] {
        [panel orderOut:nil];
        [panel release];
        [source release];
    });
    const QPoint start = origin + QPoint(80, 50);
    waitFor(
        [&] {
            const NSPoint point =
                NSMakePoint(start.x(), NSMaxY(NSScreen.screens.firstObject.frame) - start.y());
            return [NSWindow windowNumberAtPoint:point
                       belowWindowWithWindowNumber:0] == panel.windowNumber;
        },
        "External drag source must be exposed");
    const QPoint destination = target.mapToGlobal(target.rect().center());
    const auto queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), queue, ^{
      postMouse(kCGEventMouseMoved, start, 0);
      postMouse(kCGEventLeftMouseDown, start, 0);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), queue, ^{
      postMouse(kCGEventLeftMouseDragged, destination, 0);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), queue, ^{
      postMouse(kCGEventLeftMouseUp, destination, 0);
    });
    waitFor([&] { return source.started; }, "External source must receive its native press");
    waitFor([&] { return source.finished; }, "External source must finish its native drag");
    require(source.result == NSDragOperationCopy, "A pin must accept an external native drop");
}
void dragTo(QWidget& source, ScreenshotPinnedDragExport& exporter, QPoint destination,
            const std::function<bool()>& completed) {
    bool finished = false;
    const auto reset = qScopeGuard([&] {
        exporter.setExecutor([](QDrag& drag) { return drag.exec(Qt::CopyAction, Qt::CopyAction); });
    });
    exporter.setExecutor([&](QDrag& drag) {
        // Preparation is asynchronous. Schedule release from native execution so
        // a slow render cannot release the pointer before AppKit starts tracking.
        const auto queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), queue, ^{
          postMouse(kCGEventLeftMouseDragged, destination, kCGEventFlagMaskCommand);
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), queue, ^{
          postMouse(kCGEventLeftMouseUp, destination, 0);
        });
        const auto result = drag.exec(Qt::CopyAction, Qt::CopyAction);
        std::cout << "Native drag result: " << int(result) << std::endl;
        finished = true;
        return result;
    });
    const QPoint start = source.mapToGlobal(source.rect().center());
    // Qt maps Command to ControlModifier on macOS. Keep it held throughout export.
    const CGEventFlags flags = kCGEventFlagMaskCommand;
    const auto queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), queue, ^{
      postMouse(kCGEventMouseMoved, start, flags);
      postMouse(kCGEventLeftMouseDown, start, flags);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), queue, ^{
      postMouse(kCGEventLeftMouseDragged, start + QPoint(30, 0), flags);
    });
    waitFor([&] { return finished; }, "Native image drag must finish");
    waitFor(completed, "Native image drag must deliver its content");
}
void presentPin(ScreenshotPinnedWindow& pin, QPoint position, QColor color,
                const std::function<QImage(const ScreenshotPinnedWindow&)>& readImage) {
    QImage image(200, 140, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    ScreenshotPinnedWindow::Config config;
    config.screen = QGuiApplication::primaryScreen();
    config.automaticTextRecognition = false;
    config.nativeGeometry = QRect(position, image.size());
    config.canvasSourceRect = QRectF(QPointF(), image.size());
    config.initialWindowSize = image.size();
    config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
    config.initialShadowVisible = false;
    bool ready = false;
    require(pin.present(config, [&](bool succeeded, QImage) { ready = succeeded; }),
            "Native content drag pin must present");
    pin.setAttribute(Qt::WA_DeleteOnClose, false);
    waitFor([&] { return ready && pin.isVisible() && !readImage(pin).isNull(); },
            "Native content drag pin must publish its image");
}
} // namespace

int macPinnedContentDragTests(
    const std::function<QImage(const ScreenshotPinnedWindow&)>& readImage,
    const std::function<ScreenshotPinnedDragExport&(ScreenshotPinnedWindow&)>& readExporter) {
    if (!CGPreflightPostEventAccess()) {
        std::cout << "SKIP: native content drags require existing event-posting permission\n";
        return 77;
    }
    const NSPoint cursor = NSEvent.mouseLocation;
    NSRunningApplication* previous = [NSWorkspace.sharedWorkspace.frontmostApplication retain];
    const auto restore = qScopeGuard([&] {
        const QPoint point(qRound(cursor.x),
                           qRound(NSMaxY(NSScreen.screens.firstObject.frame) - cursor.y));
        postMouse(kCGEventLeftMouseUp, point, 0);
        postMouse(kCGEventMouseMoved, point, 0);
        [previous activateWithOptions:0];
        [previous release];
    });
    [NSApp activateIgnoringOtherApps:YES];
    const NSInteger clipboardChange = NSPasteboard.generalPasteboard.changeCount;
    const snow_shot::storage::ScreenshotSettings settings;
    require(settings.setImageFormat(QStringLiteral("png")) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("drag image # % 中文")),
            "Configure native drag fixture");
    const QPoint origin =
        QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(100, 160);
    ScreenshotPinnedWindow source;
    presentPin(source, origin, Qt::green, readImage);
    if (QCoreApplication::arguments().contains(QStringLiteral("--external"))) {
        NSPanel* panel = [[NSPanel alloc]
            initWithContentRect:NSMakeRect(origin.x() + 380,
                                           NSMaxY(NSScreen.screens.firstObject.frame) - origin.y() -
                                               200,
                                           240, 200)
                      styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        SnowImageDragReceiver* view = [SnowImageDragReceiver new];
        [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeTIFF ]];
        panel.contentView = view;
        panel.level = NSStatusWindowLevel;
        panel.hidesOnDeactivate = NO;
        [panel orderFrontRegardless];
        const auto cleanup = qScopeGuard([&] {
            [panel orderOut:nil];
            [panel release];
            [view release];
        });
        dragTo(source, readExporter(source), origin + QPoint(500, 100),
               [&] { return view.receivedURL != nil; });
        const QString path = QString::fromNSString(view.receivedURL.path);
        require(view.receivedImage, "Native receiver must decode the image representation");
        require(!QImage(path).isNull(),
                "External receivers must be able to open the file after the drag returns");
        QTemporaryDir copied;
        require(QFile::copy(path, copied.filePath(QStringLiteral("received.png"))),
                "Finder-style deferred file copy must succeed");
    } else if (QCoreApplication::arguments().contains(QStringLiteral("--incoming"))) {
        ScreenshotPinnedWindow target;
        presentPin(target, origin + QPoint(380, 0), Qt::red, readImage);
        dragTo(source, readExporter(source), target.mapToGlobal(target.rect().center()),
               [&] { return readImage(target).pixelColor(50, 50) == QColor(Qt::green); });
        source.hide();
        QTemporaryDir files;
        const QString path = files.filePath(QStringLiteral("external image # 中文.PNG"));
        QImage replacement(200, 140, QImage::Format_RGB32);
        replacement.fill(Qt::blue);
        require(replacement.save(path, "PNG"), "Create external file drop fixture");
        NSPasteboardItem* file = [[[NSPasteboardItem alloc] init] autorelease];
        [file setString:[NSURL fileURLWithPath:path.toNSString()].absoluteString
                forType:NSPasteboardTypeFileURL];
        dropNativeItem(file, origin, target);
        waitFor([&] { return readImage(target).pixelColor(50, 50) == QColor(Qt::blue); },
                "A pin must load an external file URL through Cocoa");
        replacement.fill(Qt::red);
        require(replacement.save(path, "PNG"), "Create external pixel drop fixture");
        NSImage* nativeImage =
            [[[NSImage alloc] initWithContentsOfFile:path.toNSString()] autorelease];
        NSPasteboardItem* pixels = [[[NSPasteboardItem alloc] init] autorelease];
        [pixels setData:nativeImage.TIFFRepresentation forType:NSPasteboardTypeTIFF];
        dropNativeItem(pixels, origin, target);
        waitFor([&] { return readImage(target).pixelColor(50, 50) == QColor(Qt::red); },
                "A pin must decode external image pixels without a file URL");
    } else {
        using namespace snow_shot::presentation;
        CollageDocument document;
        components::CollageEditorWidget collage(&document);
        collage.setGeometry(QRect(origin + QPoint(350, 0), QSize(650, 450)));
        collage.show();
        QStringList paths;
        QObject::connect(&collage, &components::CollageEditorWidget::imageFilesDropped, &collage,
                         [&](const QStringList& files) { paths = files; });
        dragTo(source, readExporter(source), collage.mapToGlobal(collage.rect().center()),
               [&] { return !paths.isEmpty(); });
        require(paths.size() == 1 && !QImage(paths.first()).isNull(),
                "Image Stitching must receive a readable image file from a pin");
        collage.hide();
        ImageSplitDocument splitDocument;
        components::ImageSplitEditorWidget split(&splitDocument);
        split.setGeometry(collage.geometry());
        split.show();
        paths.clear();
        QObject::connect(&split, &components::ImageSplitEditorWidget::imageFilesDropped, &split,
                         [&](const QStringList& files) { paths = files; });
        dragTo(source, readExporter(source), split.mapToGlobal(split.rect().center()),
               [&] { return !paths.isEmpty(); });
        require(paths.size() == 1 && !QImage(paths.first()).isNull(),
                "Image Splitting must receive a readable image file from a pin");
    }
    require(NSPasteboard.generalPasteboard.changeCount == clipboardChange,
            "Content drags must preserve the system clipboard");
    return 0;
}
