// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../presentation/services/nativeairdropbackend.h"

#include <QCoreApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

#include <utility>

@interface SnowShotAirDropSession : NSObject <NSSharingServiceDelegate> {
  @public
    ScreenshotAirDropService::Completion completion;
}
@property(nonatomic, strong) NSSharingService* service;
@property(nonatomic, strong) SnowShotAirDropSession* lifetime;
@property(nonatomic, weak) NSWindow* ownerWindow;
@property(nonatomic, strong) NSURL* sharedFile;
- (void)finish:(ScreenshotAirDropService::Result)result;
@end

@implementation SnowShotAirDropSession
- (void)finish:(ScreenshotAirDropService::Result)result {
    auto finished = std::move(completion);
    self.service.delegate = nil;
    self.service = nil;
    if (finished)
        finished(std::move(result));
    self.lifetime = nil;
}
- (void)sharingService:(NSSharingService*)service didShareItems:(NSArray*)items {
    (void)items;
    // macOS 27's AirDrop extension calls didShareItems for Cancel as well, with
    // the original input items. Require the documented shared-attachment result;
    // an absent result is a handoff, never proof that the user sent the file.
    const bool confirmed = [service.attachmentFileURLs containsObject:self.sharedFile];
    [self finish:{confirmed ? ScreenshotAirDropService::Status::Shared
                            : ScreenshotAirDropService::Status::HandedOff,
                  {}}];
}
- (void)sharingService:(NSSharingService*)service
    didFailToShareItems:(NSArray*)items
                  error:(NSError*)error {
    (void)service;
    (void)items;
    const bool cancelled =
        ([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSUserCancelledError) ||
        ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) ||
        ([error.domain isEqualToString:NSOSStatusErrorDomain] && error.code == userCanceledErr);
    [self finish:{cancelled ? ScreenshotAirDropService::Status::Cancelled
                            : ScreenshotAirDropService::Status::Failed,
                  cancelled ? QString() : QString::fromNSString(error.localizedDescription)}];
}
- (NSWindow*)sharingService:(NSSharingService*)service
    sourceWindowForShareItems:(NSArray*)items
          sharingContentScope:(NSSharingContentScope*)scope {
    (void)service;
    (void)items;
    *scope = NSSharingContentScopeItem;
    return self.ownerWindow;
}
@end

ScreenshotAirDropService::Backend screenshotNativeAirDropBackend() {
    return [](QWidget* owner, QString path, ScreenshotAirDropService::Completion completion) {
        @autoreleasepool {
            SnowShotAirDropSession* session = nil;
            @try {
                NSView* view = (__bridge NSView*)reinterpret_cast<void*>(owner->winId());
                NSSharingService* service =
                    [NSSharingService sharingServiceNamed:NSSharingServiceNameSendViaAirDrop];
                NSArray* items = @[ [NSURL fileURLWithPath:path.toNSString()] ];
                if (!view.window || !service || ![service canPerformWithItems:items]) {
                    completion({ScreenshotAirDropService::Status::Unavailable,
                                QCoreApplication::translate("ScreenshotAirDropService",
                                                            "AirDrop is unavailable on this Mac")});
                    return;
                }
                session = [SnowShotAirDropSession new];
                session->completion = std::move(completion);
                session.service = service;
                session.ownerWindow = view.window;
                session.sharedFile = items.firstObject;
                session.lifetime = session;
                service.delegate = session;
                [service performWithItems:items];
            } @catch (NSException* exception) {
                (void)exception;
                ScreenshotAirDropService::Result result{
                    ScreenshotAirDropService::Status::Failed,
                    QCoreApplication::translate("ScreenshotAirDropService",
                                                "The AirDrop sharing window could not be opened")};
                if (session)
                    [session finish:std::move(result)];
                else if (completion)
                    completion(std::move(result));
            }
        }
    };
}
