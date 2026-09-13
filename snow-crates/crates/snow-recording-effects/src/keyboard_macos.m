#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#include <IOKit/hidsystem/IOLLEvent.h>

#include "keyboard_macos.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    CFMachPortRef tap;
    CFRunLoopSourceRef source;
    SnowMacKeyCallback callback;
    void* context;
} SnowMacKeyboard;

static uint8_t modifier_snapshot(CGEventFlags flags) {
    uint8_t result = 0;
    const CGEventFlags sides[] = {
        NX_DEVICELCTLKEYMASK,   NX_DEVICERCTLKEYMASK,   NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK,
        NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK,
    };
    for (unsigned i = 0; i < 8; ++i) {
        if ((flags & sides[i]) != 0) {
            result |= (uint8_t)(1U << i);
        }
    }
    // Some keyboards supply only aggregate flags. Preserve a modifier even if its side is
    // unavailable, without inventing a second modifier when both side bits are supplied.
    const CGEventFlags aggregate[] = {kCGEventFlagMaskControl, kCGEventFlagMaskAlternate,
                                      kCGEventFlagMaskShift, kCGEventFlagMaskCommand};
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned bit = i * 2;
        if ((flags & aggregate[i]) != 0 && (result & (3U << bit)) == 0) {
            result |= (uint8_t)(1U << bit);
        }
    }
    return result;
}

static uint8_t modifier_bit(uint16_t keycode) {
    switch (keycode) {
    case kVK_Control:
        return 1;
    case kVK_RightControl:
        return 2;
    case kVK_Option:
        return 4;
    case kVK_RightOption:
        return 8;
    case kVK_Shift:
        return 16;
    case kVK_RightShift:
        return 32;
    case kVK_Command:
        return 64;
    case kVK_RightCommand:
        return 128;
    default:
        return 0;
    }
}

static void layout_label(uint16_t keycode, char* output, size_t capacity) {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef data =
        source == NULL ? NULL : TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData);
    if (data == NULL) {
        if (source != NULL) {
            CFRelease(source);
        }
        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        data = source == NULL ? NULL
                              : TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData);
    }
    if (data != NULL) {
        UniChar text[16];
        UniCharCount count = 0;
        UInt32 dead = 0;
        const OSStatus status = UCKeyTranslate(
            (const UCKeyboardLayout*)CFDataGetBytePtr(data), keycode, kUCKeyActionDisplay, 0,
            (UInt32)LMGetKbdType(), kUCKeyTranslateNoDeadKeysMask, &dead, 16, &count, text);
        if (status == noErr && count > 0 && count <= 16) {
            NSString* label = [[NSString alloc] initWithCharacters:text length:count];
            if ([label rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location ==
                    NSNotFound &&
                [label
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
                        .length > 0) {
                snprintf(output, capacity, "%s", label.uppercaseString.UTF8String);
            }
        }
    }
    if (source != NULL) {
        CFRelease(source);
    }
}

static CGEventRef observe_key(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                              void* raw) {
    (void)proxy;
    @autoreleasepool {
        SnowMacKeyboard* observer = raw;
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            const SnowMacKeyEvent reset = {.reset = 1};
            observer->callback(observer->context, &reset);
            if (type == kCGEventTapDisabledByTimeout) {
                CGEventTapEnable(observer->tap, true);
            }
            return event;
        }
        if (event == NULL ||
            (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged)) {
            return event;
        }
        const int64_t keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        if (keycode < 0 || keycode >= 128) {
            return event;
        }
        const CGEventFlags flags = CGEventGetFlags(event);
        SnowMacKeyEvent observation = {0};
        observation.keycode = (uint16_t)keycode;
        observation.modifiers = modifier_snapshot(flags);
        observation.repeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
        if (type == kCGEventFlagsChanged) {
            if (keycode == kVK_CapsLock) {
                // Caps Lock's flag is a toggle state, not a physical key-up edge.
                observation.down = 1;
                observer->callback(observer->context, &observation);
                observation.down = 0;
            } else if (keycode == kVK_Function) {
                observation.down = (flags & kCGEventFlagMaskSecondaryFn) != 0;
            } else {
                observation.down = (observation.modifiers & modifier_bit((uint16_t)keycode)) != 0;
            }
        } else {
            observation.down = type == kCGEventKeyDown;
            if (observation.down && !observation.repeat) {
                layout_label(observation.keycode, observation.label, sizeof(observation.label));
            }
        }
        observer->callback(observer->context, &observation);
        // A listen-only tap must neither replace nor consume the user's event.
        return event;
    }
}

void* snow_recording_keyboard_start(SnowMacKeyCallback callback, void* context, char* error,
                                    size_t error_size) {
    @autoreleasepool {
        if (!CGPreflightListenEventAccess()) {
            snprintf(
                error, error_size, "%s",
                "Keyboard display requires Input Monitoring permission. Allow Snow Shot in System "
                "Settings > Privacy & Security > Input Monitoring, then restart the app.");
            return NULL;
        }
        SnowMacKeyboard* observer = calloc(1, sizeof(SnowMacKeyboard));
        if (observer == NULL) {
            snprintf(error, error_size, "%s", "Cannot allocate keyboard observer");
            return NULL;
        }
        observer->callback = callback;
        observer->context = context;
        const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                                 CGEventMaskBit(kCGEventFlagsChanged);
        observer->tap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap,
                                         kCGEventTapOptionListenOnly, mask, observe_key, observer);
        if (observer->tap != NULL) {
            observer->source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, observer->tap, 0);
        }
        if (observer->source == NULL) {
            snow_recording_keyboard_stop(observer);
            snprintf(
                error, error_size, "%s",
                "Cannot observe keyboard input. Check Input Monitoring permission and try again.");
            return NULL;
        }
        CFRunLoopAddSource(CFRunLoopGetCurrent(), observer->source, kCFRunLoopDefaultMode);
        CGEventTapEnable(observer->tap, true);
        return observer;
    }
}

void snow_recording_keyboard_pump(void* handle) {
    (void)handle;
    @autoreleasepool {
        // Bounded wait lets the Rust owner stop and join without invoking run-loop APIs on a
        // different thread. Callback context remains owned until this source is removed.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, true);
    }
}

void snow_recording_keyboard_stop(void* handle) {
    SnowMacKeyboard* observer = handle;
    if (observer == NULL) {
        return;
    }
    if (observer->source != NULL) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), observer->source, kCFRunLoopDefaultMode);
        CFRunLoopSourceInvalidate(observer->source);
        CFRelease(observer->source);
    }
    if (observer->tap != NULL) {
        CFMachPortInvalidate(observer->tap);
        CFRelease(observer->tap);
    }
    free(observer);
}

static CGColorRef keycap_color(CGColorSpaceRef space, const uint8_t* rgba) {
    const CGFloat components[] = {rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0,
                                  rgba[3] / 255.0};
    return CGColorCreate(space, components);
}

int snow_recording_keycap(const char* label, float scale, const uint8_t* background,
                          const uint8_t* foreground, const uint8_t* border, uint32_t* width,
                          uint32_t* height, uint8_t* pixels, size_t capacity) {
    @autoreleasepool {
        if (!isfinite(scale) || scale < 0.5f || scale > 2.0f) {
            return 0;
        }
        NSString* text = [[NSString alloc] initWithUTF8String:label];
        if (text == nil || text.length == 0 || text.length > 1024) {
            return 0;
        }
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGColorRef textColor = keycap_color(colorSpace, foreground);
        CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 32, NULL);
        if (font == NULL) {
            CGColorRelease(textColor);
            CGColorSpaceRelease(colorSpace);
            return 0;
        }
        NSAttributedString* attributed = [[NSAttributedString alloc]
            initWithString:text
                attributes:@{
                    (__bridge NSString*)kCTFontAttributeName : (__bridge id)font,
                    (__bridge NSString*)kCTForegroundColorAttributeName : (__bridge id)textColor,
                }];
        CTLineRef line =
            CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
        CGFloat ascent = 0;
        CGFloat descent = 0;
        const double measured = fmax(1, CTLineGetTypographicBounds(line, &ascent, &descent, NULL));
        const double content = measured <= 32 ? measured : 32 * pow(measured / 32, 0.85);
        const double logicalWidth = ceil(content + 40);
        int result = 0;
        if (isfinite(logicalWidth) && logicalWidth <= 8192) {
            *width = (uint32_t)ceil(logicalWidth * scale);
            *height = (uint32_t)lround(64 * scale);
            const size_t length = (size_t)*width * *height * 4;
            if (pixels == NULL) {
                result = 1;
            } else if (capacity == length) {
                memset(pixels, 0, length);
                CGContextRef context = CGBitmapContextCreate(
                    pixels, *width, *height, 8, (size_t)*width * 4, colorSpace,
                    kCGBitmapByteOrder32Big | kCGImageAlphaPremultipliedLast);
                if (context != NULL) {
                    CGContextScaleCTM(context, scale, scale);
                    CGColorRef fillColor = keycap_color(colorSpace, background);
                    CGColorRef borderColor = keycap_color(colorSpace, border);
                    CGPathRef path = CGPathCreateWithRoundedRect(
                        CGRectMake(1, 1, logicalWidth - 2, 62), 12, 12, NULL);
                    CGContextSetFillColorWithColor(context, fillColor);
                    CGContextSetStrokeColorWithColor(context, borderColor);
                    CGContextSetLineWidth(context, 2);
                    CGContextAddPath(context, path);
                    CGContextDrawPath(context, kCGPathFillStroke);
                    CGContextSetShouldSmoothFonts(context, false);
                    const double shrink = content / measured;
                    CGContextSetTextMatrix(context, CGAffineTransformMakeScale(shrink, shrink));
                    CGContextSetTextPosition(context, (logicalWidth - content) / 2,
                                             (64 - (ascent + descent) * shrink) / 2 +
                                                 descent * shrink);
                    CTLineDraw(line, context);
                    CGPathRelease(path);
                    CGColorRelease(fillColor);
                    CGColorRelease(borderColor);
                    CGContextRelease(context);
                    // CoreGraphics bitmap scanlines already match overlay row order.
                    result = 1;
                }
            }
        }
        CFRelease(line);
        CFRelease(font);
        CGColorRelease(textColor);
        CGColorSpaceRelease(colorSpace);
        return result;
    }
}
