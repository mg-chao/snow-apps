#ifndef SNOW_SHOT_MACOS_NATIVEPRINTPANEL_H
#define SNOW_SHOT_MACOS_NATIVEPRINTPANEL_H

#include "snow_shot/presentation/screenshotprintservice.h"

#import <AppKit/AppKit.h>

@interface SnowShotPrintPanelCompletion : NSObject {
  @public
    ScreenshotPrintService::Confirmation confirmed;
}
@property(nonatomic, strong) id forwardingDelegate;
@property(nonatomic, strong) NSPrintInfo* printInfo;
@property(nonatomic) SEL forwardingSelector;
@property(nonatomic) void* forwardingContext;
- (void)printPanelDidEnd:(NSPrintPanel*)panel
              returnCode:(NSInteger)returnCode
             contextInfo:(void*)context;
@end

@interface SnowShotPrintPanel : NSPrintPanel {
  @public
    ScreenshotPrintService::Confirmation confirmed;
}
@end

#endif
