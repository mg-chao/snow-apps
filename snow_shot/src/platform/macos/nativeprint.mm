#include "../../presentation/services/nativeprintbackend.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QWidget>

#import <AppKit/AppKit.h>

#include <cstring>
#include <utility>

@interface SnowShotPrintImageView : NSView
@property(nonatomic, strong) NSImage* image;
@end

@implementation SnowShotPrintImageView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)knowsPageRange:(NSRangePointer)range {
    *range = NSMakeRange(1, 1);
    NSPrintInfo* info = NSPrintOperation.currentOperation.printInfo;
    NSRect area = info.imageablePageBounds;
    if (area.size.width > 0 && area.size.height > 0) {
        info.leftMargin = area.origin.x;
        info.rightMargin = info.paperSize.width - NSMaxX(area);
        info.bottomMargin = area.origin.y;
        info.topMargin = info.paperSize.height - NSMaxY(area);
        [self setFrameSize:area.size];
    }
    return YES;
}
- (NSRect)rectForPage:(NSInteger)page {
    return page == 1 ? self.bounds : NSZeroRect;
}
- (void)drawRect:(NSRect)rect {
    (void)rect;
    [[NSColor whiteColor] setFill];
    NSRectFill(self.bounds);
    const QSize imageSize(qRound(self.image.size.width), qRound(self.image.size.height));
    const auto fitted = ScreenshotPrintService::fittedRect(
        imageSize, {0, 0, self.bounds.size.width, self.bounds.size.height});
    [self.image drawInRect:NSMakeRect(fitted.x(), fitted.y(), fitted.width(), fitted.height())
                  fromRect:NSZeroRect
                 operation:NSCompositingOperationSourceOver
                  fraction:1.0
            respectFlipped:YES
                     hints:nil];
}
@end

@interface SnowShotPrintCompletion : NSObject {
  @public
    ScreenshotPrintService::Completion completion;
    QMetaObject::Connection ownerDestroyed;
}
- (void)printOperationDidRun:(NSPrintOperation*)operation
                     success:(BOOL)success
                 contextInfo:(void*)context;
@end

@implementation SnowShotPrintCompletion
- (void)printOperationDidRun:(NSPrintOperation*)operation
                     success:(BOOL)success
                 contextInfo:(void*)context {
    QObject::disconnect(ownerDestroyed);
    const bool cancelled = [operation.printInfo.jobDisposition isEqualToString:NSPrintCancelJob];
    const auto status = success     ? ScreenshotPrintService::Status::Submitted
                        : cancelled ? ScreenshotPrintService::Status::Cancelled
                                    : ScreenshotPrintService::Status::Failed;
    completion({status, status == ScreenshotPrintService::Status::Failed
                            ? QCoreApplication::translate("ScreenshotPrintService",
                                                          "The native print operation failed")
                            : QString()});
    // Retained across the asynchronous native sheet; release only after its callback.
    (void)CFBridgingRelease(context);
}
@end

ScreenshotPrintService::Backend screenshotNativePrintBackend(bool legacy) {
    if (legacy)
        return {};
    return [](QWidget* owner, QImage image, ScreenshotPrintService::Completion completion) {
        @autoreleasepool {
            SnowShotPrintCompletion* delegate = nil;
            void* retainedDelegate = nullptr;
            @try {
                NSView* ownerView = (__bridge NSView*)reinterpret_cast<void*>(owner->winId());
                NSWindow* ownerWindow = ownerView.window;
                if (!ownerWindow) {
                    completion({ScreenshotPrintService::Status::Unavailable, {}});
                    return;
                }
                const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
                NSBitmapImageRep* bitmap =
                    [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                            pixelsWide:rgba.width()
                                                            pixelsHigh:rgba.height()
                                                         bitsPerSample:8
                                                       samplesPerPixel:4
                                                              hasAlpha:YES
                                                              isPlanar:NO
                                                        colorSpaceName:NSDeviceRGBColorSpace
                                                           bytesPerRow:rgba.bytesPerLine()
                                                          bitsPerPixel:32];
                if (!bitmap) {
                    completion({ScreenshotPrintService::Status::Failed,
                                QCoreApplication::translate(
                                    "ScreenshotPrintService",
                                    "The image could not be prepared for printing")});
                    return;
                }
                std::memcpy(bitmap.bitmapData, rgba.constBits(),
                            static_cast<size_t>(rgba.sizeInBytes()));
                NSImage* nativeImage =
                    [[NSImage alloc] initWithSize:NSMakeSize(image.width(), image.height())];
                [nativeImage addRepresentation:bitmap];
                NSPrintInfo* info = [NSPrintInfo.sharedPrintInfo copy];
                info.horizontalPagination = NSPrintingPaginationModeFit;
                info.verticalPagination = NSPrintingPaginationModeFit;
                info.horizontallyCentered = YES;
                info.verticallyCentered = YES;
                SnowShotPrintImageView* view = [[SnowShotPrintImageView alloc]
                    initWithFrame:NSMakeRect(0, 0, info.imageablePageBounds.size.width,
                                             info.imageablePageBounds.size.height)];
                view.image = nativeImage;
                NSPrintOperation* operation = [NSPrintOperation printOperationWithView:view
                                                                             printInfo:info];
                operation.jobTitle = @"SnowShot";
                operation.showsPrintPanel = YES;
                operation.showsProgressPanel = YES;
                operation.canSpawnSeparateThread = NO;
                delegate = [SnowShotPrintCompletion new];
                delegate->completion = std::move(completion);
                __weak NSPrintOperation* weakOperation = operation;
                __weak NSWindow* weakWindow = ownerWindow;
                delegate->ownerDestroyed =
                    QObject::connect(owner, &QObject::destroyed, qApp, [weakOperation, weakWindow] {
                        NSPrintOperation* active = weakOperation;
                        active.printInfo.jobDisposition = NSPrintCancelJob;
                        NSWindow* window = weakWindow;
                        if (window.attachedSheet)
                            [window endSheet:window.attachedSheet returnCode:NSModalResponseCancel];
                    });
                retainedDelegate = (__bridge_retained void*)delegate;
                [operation
                    runOperationModalForWindow:ownerWindow
                                      delegate:delegate
                                didRunSelector:@selector(printOperationDidRun:success:contextInfo:)
                                   contextInfo:retainedDelegate];
            } @catch (NSException* exception) {
                (void)exception;
                if (delegate) {
                    QObject::disconnect(delegate->ownerDestroyed);
                    completion = std::move(delegate->completion);
                }
                if (retainedDelegate)
                    (void)CFBridgingRelease(retainedDelegate);
                if (completion)
                    completion({ScreenshotPrintService::Status::Failed,
                                QCoreApplication::translate("ScreenshotPrintService",
                                                            "The native print operation failed")});
            }
        }
    };
}
