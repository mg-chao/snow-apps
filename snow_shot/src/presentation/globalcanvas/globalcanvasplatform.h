#pragma once

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

class QWidget;
namespace snow_shot::presentation {
#ifdef __OBJC__
NSWindowCollectionBehavior globalCanvasCollectionBehavior(NSWindowCollectionBehavior current);
#endif
bool setGlobalCanvasInputTransparent(QWidget* window, bool transparent);
} // namespace snow_shot::presentation
