#import <AppKit/AppKit.h>

#include "native.h"

#include <stdio.h>
#include <stdlib.h>

@interface SnowCaptureFixtureView : NSView
@end

@implementation SnowCaptureFixtureView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor blueColor] setFill];
    NSRectFill(self.bounds);
    [[NSColor redColor] setFill];
    NSRectFill(NSMakeRect(0, self.bounds.size.height / 2, self.bounds.size.width,
                          self.bounds.size.height / 2));
}
@end

// Interactive: requires a WindowServer session and screen-recording permission.
// Capture a known window rather than asserting against the user's desktop pixels.
int main(void) {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 300, 200)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.contentView =
            [[SnowCaptureFixtureView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        const uint32_t windowId = (uint32_t)window.windowNumber;
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
              SnowMacDisplay displays[32];
              size_t count = 0;
              int ok = snow_macos_displays(displays, 32, &count) && count > 0 && count <= 32;
              for (size_t i = 0; ok && i < count; ++i) {
                  const CGRect physical = CGRectMake(displays[i].x, displays[i].y,
                                                     displays[i].width, displays[i].height);
                  for (size_t j = i + 1; j < count; ++j) {
                      // Mirrors can overlap logically; distinct desktops must never overlap
                      // in the backing-pixel atlas used by region recording.
                      if (!CGRectIntersectsRect(CGDisplayBounds(displays[i].id),
                                                CGDisplayBounds(displays[j].id))) {
                          const CGRect other = CGRectMake(displays[j].x, displays[j].y,
                                                          displays[j].width, displays[j].height);
                          ok = ok && !CGRectIntersectsRect(physical, other);
                      }
                  }
              }
              for (uint8_t bgra = 0; ok && bgra < 2; ++bgra) {
                  uint8_t* pixels = calloc(600 * 400, 4);
                  char error[2048] = {0};
                  ok = pixels != NULL && snow_macos_capture(0, windowId, 600, 400, bgra, pixels,
                                                            600 * 400 * 4, error, sizeof(error));
                  if (ok) {
                      const size_t top = (50 * 600 + 50) * 4;
                      const size_t bottom = (350 * 600 + 50) * 4;
                      const size_t red = bgra ? 2 : 0;
                      const size_t blue = bgra ? 0 : 2;
                      ok = pixels[top + red] > 240 && pixels[top + blue] < 15 &&
                           pixels[bottom + red] < 15 && pixels[bottom + blue] > 240 &&
                           pixels[top + 3] == 255 && pixels[bottom + 3] == 255;
                  }
                  if (!ok) {
                      fprintf(stderr, "Capture format %u failed: %s\n", bgra, error);
                  }
                  free(pixels);
              }
              for (uint32_t attempt = 0; ok && attempt < 3; ++attempt) {
                  // Restart with a cropped source to prove stream lifetime, source units and
                  // RGBA/BGRA conversion independently of the one-shot bitmap renderer.
                  const uint32_t width = attempt == 1 ? 200 : 600;
                  const uint32_t height = attempt == 1 ? 100 : 400;
                  const uint32_t source_y = attempt == 1 ? 250 : 0;
                  char error[2048] = {0};
                  void* stream = snow_macos_stream_start(0, windowId, 0, source_y, width, height,
                                                         error, sizeof(error));
                  uint8_t* pixels = calloc((size_t)width * height, 4);
                  uint64_t sequence = 0;
                  ok = stream != NULL && pixels != NULL;
                  for (uint8_t bgra = 0; ok && bgra < 2; ++bgra) {
                      ok = snow_macos_stream_read(stream, bgra, pixels, (size_t)width * height * 4,
                                                  &sequence, error, sizeof(error));
                      const size_t offset = ((size_t)(height / 2) * width + width / 2) * 4;
                      const size_t blue = bgra ? 0 : 2;
                      ok = ok && sequence > 0 && pixels[offset + blue] > 240 &&
                           pixels[offset + 3] == 255;
                  }
                  if (!ok) {
                      fprintf(stderr, "Continuous capture attempt %u failed: %s\n", attempt, error);
                  }
                  free(pixels);
                  if (stream != NULL) {
                      snow_macos_stream_stop(stream);
                  }
              }
              printf("macOS capture: displays=%zu, RGBA/BGRA orientation, alpha, continuous "
                     "crop/restart %s\n",
                     count, ok ? "passed" : "failed");
              dispatch_async(dispatch_get_main_queue(), ^{
                [app stop:nil];
                [window close];
                exit(ok ? 0 : 1);
              });
            });
        [app run];
    }
    return 1;
}
