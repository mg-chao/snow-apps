#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "native.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static double display_scale(CGDirectDisplayID display) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (mode == NULL) {
        return 1.0;
    }
    const size_t width = CGDisplayModeGetWidth(mode);
    const double scale = width > 0 ? (double)CGDisplayModeGetPixelWidth(mode) / (double)width : 1.0;
    CGDisplayModeRelease(mode);
    return scale;
}

int snow_macos_displays(SnowMacDisplay* displays, size_t capacity, size_t* count) {
    @autoreleasepool {
        uint32_t total = 0;
        if (CGGetActiveDisplayList(0, NULL, &total) != kCGErrorSuccess) {
            return 0;
        }
        *count = total;
        if (capacity < total) {
            return 0;
        }
        CGDirectDisplayID* ids = calloc(total, sizeof(CGDirectDisplayID));
        if (ids == NULL) {
            return 0;
        }
        const CGError result = CGGetActiveDisplayList(total, ids, &total);
        if (result != kCGErrorSuccess) {
            free(ids);
            return 0;
        }
        *count = total;
        for (uint32_t i = 0; i < total; ++i) {
            const CGRect bounds = CGDisplayBounds(ids[i]);
            const double scale = display_scale(ids[i]);
            SnowMacDisplay entry = {0};
            entry.id = ids[i];
            // Match Qt's virtual desktop: origins in desktop points, sizes in backing pixels.
            entry.x = (int32_t)lround(bounds.origin.x);
            entry.y = (int32_t)lround(bounds.origin.y);
            entry.width = (uint32_t)lround(bounds.size.width * scale);
            entry.height = (uint32_t)lround(bounds.size.height * scale);
            entry.scale = scale;
            entry.primary = ids[i] == CGMainDisplayID();
            snprintf(entry.name, sizeof(entry.name), "Display %u", ids[i]);
            for (NSScreen* screen in NSScreen.screens) {
                if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == ids[i]) {
                    snprintf(entry.name, sizeof(entry.name), "%s", screen.localizedName.UTF8String);
                    break;
                }
            }
            displays[i] = entry;
        }
        free(ids);
        return 1;
    }
}

int snow_macos_windows(SnowMacWindow* windows, size_t capacity, size_t* count) {
    @autoreleasepool {
        NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID));
        if (list == nil) {
            return 0;
        }
        size_t total = 0;
        for (NSDictionary* window in list) {
            if ([window[(__bridge NSString*)kCGWindowLayer] intValue] != 0 ||
                [window[(__bridge NSString*)kCGWindowOwnerPID] intValue] == getpid() ||
                [window[(__bridge NSString*)kCGWindowAlpha] doubleValue] <= 0) {
                continue;
            }
            CGRect bounds;
            if (!CGRectMakeWithDictionaryRepresentation(
                    (__bridge CFDictionaryRef)window[(__bridge NSString*)kCGWindowBounds],
                    &bounds) ||
                bounds.size.width <= 0 || bounds.size.height <= 0) {
                continue;
            }
            CGDirectDisplayID display = CGMainDisplayID();
            uint32_t found = 0;
            CGGetDisplaysWithPoint(bounds.origin, 1, &display, &found);
            const CGRect display_bounds = CGDisplayBounds(display);
            const double scale = display_scale(display);
            if (total < capacity) {
                windows[total] = (SnowMacWindow){
                    [window[(__bridge NSString*)kCGWindowNumber] unsignedIntValue],
                    (int32_t)lround(display_bounds.origin.x +
                                    (bounds.origin.x - display_bounds.origin.x) * scale),
                    (int32_t)lround(display_bounds.origin.y +
                                    (bounds.origin.y - display_bounds.origin.y) * scale),
                    (uint32_t)lround(bounds.size.width * scale),
                    (uint32_t)lround(bounds.size.height * scale),
                };
            }
            ++total;
        }
        *count = total;
        return total <= capacity;
    }
}

// Completion blocks retain this state even if the caller times out. They never refer to the
// caller's pixel/error buffers. The semaphore provides the publication barrier before reading.
@interface SnowMacCaptureResult : NSObject
@property(nonatomic, strong) SCShareableContent* content;
@property(nonatomic, strong) NSError* error;
@property(nonatomic, strong) id image;
@end
@implementation SnowMacCaptureResult
@end

static int report_error(char* error, size_t capacity, NSString* message) {
    if (capacity > 0) {
        snprintf(error, capacity, "%s", message.UTF8String);
    }
    return 0;
}

int snow_macos_capture(uint32_t display_id, uint32_t window_id, uint32_t width, uint32_t height,
                       uint8_t bgra, uint8_t* pixels, size_t length, char* error,
                       size_t error_size) {
    @autoreleasepool {
        if (width == 0 || height == 0 || (size_t)width > SIZE_MAX / 4 / height ||
            length != (size_t)width * height * 4) {
            return report_error(error, error_size, @"Invalid screenshot buffer size");
        }
        if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
            return report_error(error, error_size,
                                @"Allow Snow Shot in System Settings > Privacy & Security > Screen "
                                @"& System Audio Recording, then try again.");
        }
        SnowMacCaptureResult* state = [SnowMacCaptureResult new];
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent* content,
                                                         NSError* failure) {
                                       state.content = content;
                                       state.error = failure;
                                       dispatch_semaphore_signal(ready);
                                     }];
        if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) !=
            0) {
            return report_error(error, error_size, @"Timed out enumerating screen capture sources");
        }
        if (state.error != nil || state.content == nil) {
            NSString* description = state.error.localizedDescription;
            return report_error(error, error_size,
                                description != nil ? description
                                                   : @"No screen capture sources available");
        }
        SCContentFilter* filter = nil;
        if (window_id != 0) {
            for (SCWindow* window in state.content.windows) {
                if (window.windowID == window_id) {
                    filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
                    break;
                }
            }
        } else {
            for (SCDisplay* display in state.content.displays) {
                if (display.displayID == display_id) {
                    filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                    break;
                }
            }
        }
        if (filter == nil) {
            return report_error(error, error_size,
                                @"The selected display or window is no longer available");
        }
        SCStreamConfiguration* configuration = [SCStreamConfiguration new];
        configuration.width = width;
        configuration.height = height;
        configuration.showsCursor = NO;
        configuration.ignoreShadowsSingleWindow = YES;
        configuration.colorSpaceName = kCGColorSpaceSRGB;
        state.content = nil;
        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:configuration
                                  completionHandler:^(CGImageRef image, NSError* failure) {
                                    state.image = (__bridge id)image;
                                    state.error = failure;
                                    dispatch_semaphore_signal(ready);
                                  }];
        if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) !=
            0) {
            return report_error(error, error_size, @"Timed out capturing the screen");
        }
        if (state.error != nil || state.image == nil) {
            NSString* description = state.error.localizedDescription;
            return report_error(error, error_size,
                                description != nil ? description
                                                   : @"Screen capture returned no image");
        }
        CGImageRef image = (__bridge CGImageRef)state.image;
        CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        const CGBitmapInfo format =
            bgra ? kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst
                 : kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast;
        CGContextRef context =
            CGBitmapContextCreate(pixels, width, height, 8, (size_t)width * 4, color_space, format);
        CGColorSpaceRelease(color_space);
        if (context == NULL) {
            return report_error(error, error_size,
                                @"Failed to allocate the screenshot bitmap context");
        }
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
        CGContextRelease(context);
        return 1;
    }
}
