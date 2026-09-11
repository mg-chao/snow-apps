#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "native.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static NSMutableSet<NSNumber*>* excluded_window_ids(void) {
    static NSMutableSet<NSNumber*>* ids;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
      ids = [NSMutableSet set];
    });
    return ids;
}

// The UI establishes exclusions before creating a recording session and restores them after
// stopping it. Only explicitly excluded windows are hidden; the annotation canvas must remain.
void snow_macos_exclude_window(uint32_t window_id, uint8_t excluded) {
    NSMutableSet<NSNumber*>* ids = excluded_window_ids();
    @synchronized(ids) {
        if (excluded) {
            [ids addObject:@(window_id)];
        } else {
            [ids removeObject:@(window_id)];
        }
    }
}

// Keep only the newest complete surface. SCStream never waits for the encoder and retained
// memory stays bounded when the encoder is slower than the display refresh rate.
@interface SnowMacVideoStream : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, strong) SCStream* stream;
@property(nonatomic, strong) NSCondition* condition;
@property(nonatomic, strong) id surface;
@property(nonatomic, strong) NSError* failure;
@property(nonatomic, strong) SCShareableContent* content;
@property(nonatomic) uint64_t sequence;
@property(nonatomic) uint32_t width;
@property(nonatomic) uint32_t height;
@property(nonatomic) BOOL closed;
@end

@implementation SnowMacVideoStream
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    [self.condition lock];
    self.failure = error;
    [self.condition broadcast];
    [self.condition unlock];
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer)) {
        return;
    }
    NSArray* attachments =
        (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    NSNumber* status = attachments.firstObject[SCStreamFrameInfoStatus];
    if (status != nil && (status.integerValue == SCFrameStatusBlank ||
                          status.integerValue == SCFrameStatusSuspended ||
                          status.integerValue == SCFrameStatusStopped)) {
        [self.condition lock];
        if (!self.closed) {
            self.failure =
                [NSError errorWithDomain:@"SnowMacCapture"
                                    code:status.integerValue
                                userInfo:@{
                                    NSLocalizedDescriptionKey :
                                        @"The recording source stopped providing visible frames"
                                }];
            [self.condition broadcast];
        }
        [self.condition unlock];
        return;
    }
    if (status == nil || status.integerValue != SCFrameStatusComplete) {
        return;
    }
    CVPixelBufferRef surface = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (surface == NULL) {
        return;
    }
    [self.condition lock];
    if (!self.closed) {
        self.surface = (__bridge id)surface;
        ++self.sequence;
        [self.condition broadcast];
    }
    [self.condition unlock];
}
@end

static int stream_error(char* error, size_t capacity, NSString* message) {
    if (capacity > 0) {
        snprintf(error, capacity, "%s", message.UTF8String);
    }
    return 0;
}

static void close_stream(SnowMacVideoStream* state) {
    [state.condition lock];
    state.closed = YES;
    state.surface = nil;
    SCStream* stream = state.stream;
    state.stream = nil;
    [state.condition broadcast];
    [state.condition unlock];
    if (stream != nil) {
        [stream removeStreamOutput:state type:SCStreamOutputTypeScreen error:nil];
        // The completion retains the native stream until shutdown; no caller buffers escape.
        [stream stopCaptureWithCompletionHandler:^(NSError* error) {
          (void)error;
          (void)stream;
          (void)state;
        }];
    }
}

void* snow_macos_stream_start(uint32_t display_id, uint32_t window_id, uint32_t source_x,
                              uint32_t source_y, uint32_t width, uint32_t height, char* error,
                              size_t error_size) {
    @autoreleasepool {
        if (width == 0 || height == 0 || (size_t)width > SIZE_MAX / 4 / height) {
            stream_error(error, error_size, @"Invalid recording dimensions");
            return NULL;
        }
        if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
            stream_error(error, error_size, @"Screen recording permission is required");
            return NULL;
        }
        SnowMacVideoStream* state = [SnowMacVideoStream new];
        state.condition = [NSCondition new];
        state.width = width;
        state.height = height;
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent* content,
                                                         NSError* failure) {
                                       state.content = content;
                                       state.failure = failure;
                                       dispatch_semaphore_signal(ready);
                                     }];
        if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) !=
            0) {
            stream_error(error, error_size, @"Timed out enumerating recording sources");
            return NULL;
        }
        if (state.content == nil || state.failure != nil) {
            stream_error(error, error_size,
                         state.failure.localizedDescription != nil
                             ? state.failure.localizedDescription
                             : @"No recording sources");
            return NULL;
        }
        SCContentFilter* filter = nil;
        double scale = 1;
        if (window_id != 0) {
            for (SCWindow* window in state.content.windows) {
                if (window.windowID == window_id) {
                    filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
                    scale = (double)filter.pointPixelScale;
                    break;
                }
            }
        } else {
            for (SCDisplay* display in state.content.displays) {
                if (display.displayID == display_id) {
                    NSMutableArray<SCWindow*>* excluded = [NSMutableArray array];
                    NSMutableSet<NSNumber*>* ids = excluded_window_ids();
                    @synchronized(ids) {
                        for (SCWindow* window in state.content.windows) {
                            if ([ids containsObject:@(window.windowID)]) {
                                [excluded addObject:window];
                            }
                        }
                    }
                    filter = [[SCContentFilter alloc] initWithDisplay:display
                                                     excludingWindows:excluded];
                    scale = (double)filter.pointPixelScale;
                    break;
                }
            }
        }
        state.content = nil;
        if (filter == nil || scale <= 0) {
            stream_error(error, error_size, @"The recording source is no longer available");
            return NULL;
        }
        SCStreamConfiguration* configuration = [SCStreamConfiguration new];
        configuration.width = width;
        configuration.height = height;
        configuration.sourceRect =
            CGRectMake(source_x / scale, source_y / scale, width / scale, height / scale);
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        // The shared capture scheduler controls output rate, including 83/120 FPS settings.
        configuration.minimumFrameInterval = kCMTimeZero;
        configuration.queueDepth = 3;
        configuration.showsCursor = NO;
        configuration.ignoreShadowsSingleWindow = YES;
        configuration.colorSpaceName = kCGColorSpaceSRGB;
        configuration.scalesToFit = YES;
        state.stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:configuration
                                               delegate:state];
        NSError* failure = nil;
        dispatch_queue_t queue = dispatch_queue_create("snow.capture.video", DISPATCH_QUEUE_SERIAL);
        if (![state.stream addStreamOutput:state
                                      type:SCStreamOutputTypeScreen
                        sampleHandlerQueue:queue
                                     error:&failure]) {
            stream_error(error, error_size,
                         failure.localizedDescription != nil ? failure.localizedDescription
                                                             : @"Cannot attach recording output");
            close_stream(state);
            return NULL;
        }
        SCStream* startingStream = state.stream;
        [startingStream startCaptureWithCompletionHandler:^(NSError* startFailure) {
          [state.condition lock];
          const BOOL closed = state.closed;
          if (startFailure != nil) {
              state.failure = startFailure;
          }
          [state.condition unlock];
          // A stop issued while startup is pending can fail. If startup completes after
          // the caller timed out, stop the original stream again instead of orphaning it.
          if (closed && startFailure == nil) {
              [startingStream stopCaptureWithCompletionHandler:^(NSError* stopFailure) {
                (void)stopFailure;
                (void)startingStream;
                (void)state;
              }];
          }
          dispatch_semaphore_signal(ready);
        }];
        if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) !=
            0) {
            stream_error(error, error_size, @"Timed out starting screen recording");
            close_stream(state);
            return NULL;
        }
        [state.condition lock];
        failure = state.failure;
        [state.condition unlock];
        if (failure != nil) {
            stream_error(error, error_size, failure.localizedDescription);
            close_stream(state);
            return NULL;
        }
        return (__bridge_retained void*)state;
    }
}

int snow_macos_stream_read(void* handle, uint8_t bgra, uint8_t* pixels, size_t length,
                           uint64_t* sequence, char* error, size_t error_size) {
    @autoreleasepool {
        SnowMacVideoStream* state = (__bridge SnowMacVideoStream*)handle;
        if (length != (size_t)state.width * state.height * 4) {
            return stream_error(error, error_size, @"Invalid recording buffer size");
        }
        [state.condition lock];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:5];
        while (state.surface == nil && state.failure == nil && !state.closed) {
            if (![state.condition waitUntilDate:deadline]) {
                break;
            }
        }
        id retainedSurface = state.surface;
        NSError* failure = state.failure;
        *sequence = state.sequence;
        [state.condition unlock];
        if (failure != nil || retainedSurface == nil) {
            return stream_error(error, error_size,
                                failure.localizedDescription != nil
                                    ? failure.localizedDescription
                                    : @"No recording frame received");
        }
        CVPixelBufferRef surface = (__bridge CVPixelBufferRef)retainedSurface;
        if (CVPixelBufferGetWidth(surface) != state.width ||
            CVPixelBufferGetHeight(surface) != state.height ||
            CVPixelBufferGetPixelFormatType(surface) != kCVPixelFormatType_32BGRA ||
            CVPixelBufferLockBaseAddress(surface, kCVPixelBufferLock_ReadOnly) !=
                kCVReturnSuccess) {
            return stream_error(error, error_size, @"Invalid recording surface");
        }
        const size_t stride = CVPixelBufferGetBytesPerRow(surface);
        const uint8_t* source = CVPixelBufferGetBaseAddress(surface);
        const size_t row_size = (size_t)state.width * 4;
        if (source == NULL || stride < row_size) {
            CVPixelBufferUnlockBaseAddress(surface, kCVPixelBufferLock_ReadOnly);
            return stream_error(error, error_size, @"Invalid recording surface stride");
        }
        for (uint32_t y = 0; y < state.height; ++y) {
            uint8_t* row = pixels + (size_t)y * row_size;
            memcpy(row, source + (size_t)y * stride, row_size);
            if (!bgra) {
                for (size_t x = 0; x < row_size; x += 4) {
                    const uint8_t blue = row[x];
                    row[x] = row[x + 2];
                    row[x + 2] = blue;
                }
            }
        }
        CVPixelBufferUnlockBaseAddress(surface, kCVPixelBufferLock_ReadOnly);
        return 1;
    }
}

void snow_macos_stream_stop(void* handle) {
    @autoreleasepool {
        SnowMacVideoStream* state = (__bridge_transfer SnowMacVideoStream*)handle;
        close_stream(state);
    }
}
