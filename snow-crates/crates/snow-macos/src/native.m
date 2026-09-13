#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "native.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>

static _Atomic(uint64_t) display_generation = 1;

static void display_changed(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags,
                            void* context) {
    (void)display;
    (void)context;
    if ((flags & kCGDisplayBeginConfigurationFlag) == 0) {
        atomic_fetch_add_explicit(&display_generation, 1, memory_order_relaxed);
    }
}

uint64_t snow_macos_display_generation(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      CGDisplayRegisterReconfigurationCallback(display_changed, NULL);
    });
    return atomic_load_explicit(&display_generation, memory_order_relaxed);
}

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

// macOS desktop origins are points, unlike Windows' physical monitor origins. Use one
// desktop scale for all origins so monitors never overlap in the physical capture space.
// Individual display images retain their native backing resolution (mixed-DPI gaps are valid).
static double desktop_scale(void) {
    CGDirectDisplayID displays[64];
    uint32_t count = 0;
    double scale = 1.0;
    if (CGGetActiveDisplayList(64, displays, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; ++i) {
            scale = fmax(scale, display_scale(displays[i]));
        }
    }
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
        const double desktopScale = desktop_scale();
        for (uint32_t i = 0; i < total; ++i) {
            const CGRect bounds = CGDisplayBounds(ids[i]);
            const double scale = display_scale(ids[i]);
            SnowMacDisplay entry = {0};
            entry.id = ids[i];
            // Match the macOS branch of ScreenshotGeometryMapper::physicalRectForScreen.
            entry.x = (int32_t)lround(bounds.origin.x * desktopScale);
            entry.y = (int32_t)lround(bounds.origin.y * desktopScale);
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
        const double desktopScale = desktop_scale();
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
                    (int32_t)lround(display_bounds.origin.x * desktopScale +
                                    (bounds.origin.x - display_bounds.origin.x) * scale),
                    (int32_t)lround(display_bounds.origin.y * desktopScale +
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

// Query the captured window's application, not the system-wide AX root: the
// screenshot overlay must never become the hit-test target. Permission denial,
// unsupported app controls and timed-out applications all fall back to the window.
int snow_macos_window_element(uint32_t window_id, int32_t x, int32_t y, SnowMacWindow* element) {
    @autoreleasepool {
        if (element == NULL || window_id == 0 || !AXIsProcessTrusted())
            return 0;
        NSArray* windows = CFBridgingRelease(
            CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, window_id));
        NSDictionary* window = windows.firstObject;
        if (window == nil ||
            [window[(__bridge NSString*)kCGWindowNumber] unsignedIntValue] != window_id)
            return 0;
        const pid_t pid = [window[(__bridge NSString*)kCGWindowOwnerPID] intValue];
        CGRect bounds;
        if (pid <= 0 || pid == getpid() ||
            !CGRectMakeWithDictionaryRepresentation(
                (__bridge CFDictionaryRef)window[(__bridge NSString*)kCGWindowBounds], &bounds))
            return 0;
        CGDirectDisplayID display = CGMainDisplayID();
        uint32_t found = 0;
        CGGetDisplaysWithPoint(bounds.origin, 1, &display, &found);
        const CGRect displayBounds = CGDisplayBounds(display);
        const double scale = display_scale(display);
        const double desktopScale = desktop_scale();
        const CGPoint point = CGPointMake(
            displayBounds.origin.x + ((double)x - displayBounds.origin.x * desktopScale) / scale,
            displayBounds.origin.y + ((double)y - displayBounds.origin.y * desktopScale) / scale);
        if (!CGRectContainsPoint(bounds, point))
            return 0;
        AXUIElementRef application = AXUIElementCreateApplication(pid);
        if (AXUIElementSetMessagingTimeout(application, 0.03f) != kAXErrorSuccess) {
            CFRelease(application);
            return 0;
        }
        AXUIElementRef hit = NULL;
        const AXError status =
            AXUIElementCopyElementAtPosition(application, (float)point.x, (float)point.y, &hit);
        CFRelease(application);
        if (status != kAXErrorSuccess || hit == NULL) {
            if (hit != NULL)
                CFRelease(hit);
            return 0;
        }
        if (AXUIElementSetMessagingTimeout(hit, 0.03f) != kAXErrorSuccess) {
            CFRelease(hit);
            return 0;
        }
        CFTypeRef position = NULL;
        CFTypeRef size = NULL;
        CGPoint origin = CGPointZero;
        CGSize extent = CGSizeZero;
        const bool valid =
            AXUIElementCopyAttributeValue(hit, kAXPositionAttribute, &position) ==
                kAXErrorSuccess &&
            AXUIElementCopyAttributeValue(hit, kAXSizeAttribute, &size) == kAXErrorSuccess &&
            position != NULL && size != NULL && CFGetTypeID(position) == AXValueGetTypeID() &&
            CFGetTypeID(size) == AXValueGetTypeID() &&
            AXValueGetValue((AXValueRef)position, kAXValueCGPointType, &origin) &&
            AXValueGetValue((AXValueRef)size, kAXValueCGSizeType, &extent);
        if (position != NULL)
            CFRelease(position);
        if (size != NULL)
            CFRelease(size);
        CFRelease(hit);
        if (!valid || !isfinite(origin.x) || !isfinite(origin.y) || !isfinite(extent.width) ||
            !isfinite(extent.height))
            return 0;
        const CGRect clipped = CGRectIntersection(bounds, (CGRect){origin, extent});
        if (CGRectIsEmpty(clipped) || CGRectIsNull(clipped) || !CGRectContainsPoint(clipped, point))
            return 0;
        *element =
            (SnowMacWindow){window_id,
                            (int32_t)lround(displayBounds.origin.x * desktopScale +
                                            (clipped.origin.x - displayBounds.origin.x) * scale),
                            (int32_t)lround(displayBounds.origin.y * desktopScale +
                                            (clipped.origin.y - displayBounds.origin.y) * scale),
                            (uint32_t)lround(clipped.size.width * scale),
                            (uint32_t)lround(clipped.size.height * scale)};
        return element->width > 0 && element->height > 0;
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

int snow_macos_pointer(SnowMacCursor* cursor) {
    @autoreleasepool {
        CGEventRef event = CGEventCreate(NULL);
        if (event == NULL) {
            return 0;
        }
        const CGPoint point = CGEventGetLocation(event);
        CFRelease(event);
        CGDirectDisplayID display = CGMainDisplayID();
        uint32_t count = 0;
        CGGetDisplaysWithPoint(point, 1, &display, &count);
        const CGRect bounds = CGDisplayBounds(display);
        const double scale = display_scale(display);
        const double desktopScale = desktop_scale();
        cursor->x =
            (int32_t)lround(bounds.origin.x * desktopScale + (point.x - bounds.origin.x) * scale);
        cursor->y =
            (int32_t)lround(bounds.origin.y * desktopScale + (point.y - bounds.origin.y) * scale);
        cursor->buttons =
            (uint8_t)((CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                                kCGMouseButtonLeft)
                           ? 1
                           : 0) |
                      (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                                kCGMouseButtonRight)
                           ? 2
                           : 0) |
                      (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                                kCGMouseButtonCenter)
                           ? 4
                           : 0));
        return 1;
    }
}

int snow_macos_cursor(SnowMacCursor* cursor, uint8_t* rgba, size_t capacity) {
    @autoreleasepool {
        if (!snow_macos_pointer(cursor)) {
            return 0;
        }
        CGEventRef event = CGEventCreate(NULL);
        if (event == NULL) {
            return 0;
        }
        CGDirectDisplayID display = CGMainDisplayID();
        uint32_t count = 0;
        CGGetDisplaysWithPoint(CGEventGetLocation(event), 1, &display, &count);
        CFRelease(event);
        const double scale = display_scale(display);
        NSCursor* systemCursor = NSCursor.currentSystemCursor;
        if (systemCursor == nil) {
            return 0;
        }
        NSImage* image = systemCursor.image;
        const NSSize size = image.size;
        if (size.width <= 0 || size.height <= 0 || size.width * scale > 256 ||
            size.height * scale > 256) {
            return 0;
        }
        cursor->width = (uint32_t)ceil(size.width * scale);
        cursor->height = (uint32_t)ceil(size.height * scale);
        cursor->hotspot_x = (uint32_t)fmax(0, round(systemCursor.hotSpot.x * scale));
        cursor->hotspot_y = (uint32_t)fmax(0, round(systemCursor.hotSpot.y * scale));
        const size_t length = (size_t)cursor->width * cursor->height * 4;
        if (length > capacity) {
            return 0;
        }
        CGImageRef bitmap = [image CGImageForProposedRect:NULL context:nil hints:nil];
        if (bitmap == NULL) {
            return 0;
        }
        memset(rgba, 0, length);
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(
            rgba, cursor->width, cursor->height, 8, (size_t)cursor->width * 4, colorSpace,
            kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(colorSpace);
        if (context == NULL) {
            return 0;
        }
        CGContextDrawImage(context, CGRectMake(0, 0, cursor->width, cursor->height), bitmap);
        CGContextRelease(context);
        // Cursor composition expects straight alpha, while CoreGraphics produces premultiplied.
        for (size_t offset = 0; offset < length; offset += 4) {
            const unsigned alpha = rgba[offset + 3];
            if (alpha > 0 && alpha < 255) {
                for (size_t channel = 0; channel < 3; ++channel) {
                    const unsigned straight =
                        ((unsigned)rgba[offset + channel] * 255U + alpha / 2U) / alpha;
                    rgba[offset + channel] = (uint8_t)(straight > 255U ? 255U : straight);
                }
            }
        }
        return 1;
    }
}
