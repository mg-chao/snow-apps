// SPDX-License-Identifier: Apache-2.0
#import "native.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <mach/mach_time.h>
#include <math.h>

static void snow_audio_error(char* output, size_t capacity, NSString* message) {
    if (output && capacity) {
        snprintf(output, capacity, "%s", message.UTF8String ?: "macOS audio capture failed");
    }
}

uint64_t snow_audio_host_time_ns(void) {
    return (uint64_t)([AVAudioTime secondsForHostTime:mach_absolute_time()] * 1e9);
}

static AudioObjectPropertyAddress snow_address(AudioObjectPropertySelector selector,
                                               AudioObjectPropertyScope scope) {
    return (AudioObjectPropertyAddress){selector, scope, kAudioObjectPropertyElementMain};
}

static NSString* snow_device_string(AudioDeviceID device, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress address = snow_address(selector, kAudioObjectPropertyScopeGlobal);
    CFStringRef value = NULL;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value) != noErr || !value) {
        return @"";
    }
    return CFBridgingRelease(value);
}

static NSArray<NSNumber*>* snow_input_devices(void) {
    AudioObjectPropertyAddress address =
        snow_address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) !=
            noErr ||
        size > 1024 * sizeof(AudioDeviceID)) {
        return @[];
    }
    NSMutableData* storage = [NSMutableData dataWithLength:size];
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size,
                                   storage.mutableBytes) != noErr) {
        return @[];
    }
    const AudioDeviceID* devices = storage.bytes;
    NSMutableArray<NSNumber*>* inputs = [NSMutableArray array];
    for (size_t index = 0; index < size / sizeof(AudioDeviceID); ++index) {
        AudioObjectPropertyAddress streams =
            snow_address(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput);
        UInt32 stream_size = 0;
        if (AudioObjectGetPropertyDataSize(devices[index], &streams, 0, NULL, &stream_size) ==
                noErr &&
            stream_size) {
            [inputs addObject:@(devices[index])];
        }
    }
    return inputs;
}

static AudioDeviceID snow_default_input(void) {
    AudioObjectPropertyAddress address =
        snow_address(kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal);
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device);
    return device;
}

int32_t snow_audio_devices(SnowAudioDevice* devices, size_t capacity, char* error,
                           size_t error_capacity) {
    @autoreleasepool {
        NSArray<NSNumber*>* inputs = snow_input_devices();
        if (inputs.count > capacity) {
            snow_audio_error(error, error_capacity, @"Too many microphone devices");
            return -1;
        }
        AudioDeviceID default_device = snow_default_input();
        for (NSUInteger index = 0; index < inputs.count; ++index) {
            AudioDeviceID device = inputs[index].unsignedIntValue;
            NSString* uid = snow_device_string(device, kAudioDevicePropertyDeviceUID);
            NSString* name = snow_device_string(device, kAudioObjectPropertyName);
            if (uid.length == 0 || [uid lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= 1024 ||
                [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= 1024) {
                snow_audio_error(error, error_capacity, @"Invalid microphone device identity");
                return -1;
            }
            snprintf(devices[index].id, sizeof(devices[index].id), "%s", uid.UTF8String);
            snprintf(devices[index].name, sizeof(devices[index].name), "%s", name.UTF8String);
            devices[index].is_default = device == default_device;
        }
        return (int32_t)inputs.count;
    }
}

@interface SnowAudioChunk : NSObject
@property(nonatomic, strong) NSData* samples;
@property(nonatomic) uint32_t frames;
@property(nonatomic) uint64_t endHostNs;
@end
@implementation SnowAudioChunk
@end

// All asynchronous callbacks retain this object, never a Rust pointer. Queue state is protected by
// condition; conversion runs on each source's serial callback queue. Closing wakes a waiting poll.
@interface SnowAudioSource
    : NSObject <SCStreamOutput, SCStreamDelegate, AVCaptureAudioDataOutputSampleBufferDelegate>
@property(nonatomic, strong) NSCondition* condition;
@property(nonatomic, strong) NSMutableArray<SnowAudioChunk*>* packets;
@property(nonatomic, strong) SCStream* stream;
@property(nonatomic, strong) AVCaptureSession* session;
@property(nonatomic, strong) AVCaptureAudioDataOutput* microphoneOutput;
@property(nonatomic, strong) AVAudioConverter* converter;
@property(nonatomic, strong) AVAudioFormat* outputFormat;
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, strong) NSString* failure;
@property(nonatomic) size_t capacity;
@property(nonatomic) uint64_t droppedFrames;
@property(nonatomic) BOOL closed;
@property(nonatomic, strong) id errorObserver;
- (void)fail:(NSString*)message;
- (void)append:(AVAudioPCMBuffer*)buffer startHostNs:(uint64_t)start;
- (void)appendSample:(CMSampleBufferRef)sample;
- (void)close;
@end

@implementation SnowAudioSource
- (instancetype)init {
    if ((self = [super init])) {
        _condition = [[NSCondition alloc] init];
        _packets = [NSMutableArray array];
        _queue = dispatch_queue_create("snow.audio.capture", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (void)fail:(NSString*)message {
    [self.condition lock];
    if (!self.closed && !self.failure) {
        self.failure = message;
    }
    [self.condition signal];
    [self.condition unlock];
}
- (void)append:(AVAudioPCMBuffer*)buffer startHostNs:(uint64_t)start {
    @autoreleasepool {
        [self.condition lock];
        BOOL closed = self.closed;
        [self.condition unlock];
        if (closed || !buffer.frameLength) {
            return;
        }
        if (!self.converter || ![self.converter.inputFormat isEqual:buffer.format]) {
            self.converter = [[AVAudioConverter alloc] initFromFormat:buffer.format
                                                             toFormat:self.outputFormat];
        }
        if (!self.converter || buffer.format.sampleRate <= 0) {
            [self fail:@"The captured audio format cannot be converted"];
            return;
        }
        double capacity = ceil((double)buffer.frameLength * self.outputFormat.sampleRate /
                               buffer.format.sampleRate) +
                          64;
        if (!isfinite(capacity) || capacity > 192000) {
            [self fail:@"The audio packet exceeds the capture buffer limit"];
            return;
        }
        AVAudioPCMBuffer* output =
            [[AVAudioPCMBuffer alloc] initWithPCMFormat:self.outputFormat
                                          frameCapacity:(AVAudioFrameCount)capacity];
        __block BOOL supplied = NO;
        NSError* error = nil;
        AVAudioConverterOutputStatus result =
            [self.converter convertToBuffer:output
                                      error:&error
                         withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount count,
                                                            AVAudioConverterInputStatus* status) {
                           (void)count;
                           if (supplied) {
                               *status = AVAudioConverterInputStatus_NoDataNow;
                               return nil;
                           }
                           supplied = YES;
                           *status = AVAudioConverterInputStatus_HaveData;
                           return buffer;
                         }];
        if (result == AVAudioConverterOutputStatus_Error) {
            [self fail:error.localizedDescription ?: @"Audio sample conversion failed"];
            return;
        }
        if (!output.frameLength) {
            return;
        }
        SnowAudioChunk* chunk = [[SnowAudioChunk alloc] init];
        chunk.frames = output.frameLength;
        chunk.endHostNs =
            start + (uint64_t)((double)buffer.frameLength / buffer.format.sampleRate * 1e9);
        chunk.samples = [NSData dataWithBytes:output.int16ChannelData[0]
                                       length:(NSUInteger)output.frameLength *
                                              self.outputFormat.channelCount * sizeof(int16_t)];
        [self.condition lock];
        if (!self.closed) {
            while (self.packets.count >= self.capacity) {
                self.droppedFrames += self.packets.firstObject.frames;
                [self.packets removeObjectAtIndex:0];
            }
            [self.packets addObject:chunk];
            [self.condition signal];
        }
        [self.condition unlock];
    }
}
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type == SCStreamOutputTypeAudio) {
        [self appendSample:sample];
    }
}
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    [self appendSample:sample];
}
- (void)appendSample:(CMSampleBufferRef)sample {
    if (!CMSampleBufferDataIsReady(sample)) {
        return;
    }
    CMFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sample);
    const AudioStreamBasicDescription* asbd =
        CMAudioFormatDescriptionGetStreamBasicDescription(description);
    CMItemCount frame_count = CMSampleBufferGetNumSamples(sample);
    if (!asbd || frame_count <= 0 || frame_count > 192000) {
        return;
    }
    AVAudioFormat* format = [[AVAudioFormat alloc] initWithStreamDescription:asbd];
    AVAudioPCMBuffer* buffer =
        [[AVAudioPCMBuffer alloc] initWithPCMFormat:format
                                      frameCapacity:(AVAudioFrameCount)frame_count];
    if (!buffer) {
        [self fail:@"The capture source returned an unsupported audio format"];
        return;
    }
    buffer.frameLength = (AVAudioFrameCount)frame_count;
    OSStatus copied = CMSampleBufferCopyPCMDataIntoAudioBufferList(sample, 0, (int32_t)frame_count,
                                                                   buffer.mutableAudioBufferList);
    if (copied != noErr) {
        [self fail:@"Unable to copy captured audio samples"];
        return;
    }
    double timestamp = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
    uint64_t start = isfinite(timestamp) && timestamp >= 0 ? (uint64_t)(timestamp * 1e9)
                                                           : snow_audio_host_time_ns();
    [self append:buffer startHostNs:start];
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    [self fail:error.localizedDescription];
}
- (void)close {
    [self.condition lock];
    self.closed = YES;
    [self.packets removeAllObjects];
    [self.condition broadcast];
    [self.condition unlock];
    if (self.errorObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:self.errorObserver];
        self.errorObserver = nil;
    }
    [self.session stopRunning];
    [self.microphoneOutput setSampleBufferDelegate:nil queue:NULL];
    // setSampleBufferDelegate does not retain its delegate. Finish queued calls before releasing
    // the source; close is only invoked by the Rust worker, never by this callback queue.
    dispatch_sync(self.queue, ^{
                  });
    self.microphoneOutput = nil;
    self.session = nil;
    SCStream* stream = self.stream;
    self.stream = nil;
    if (stream) {
        // The completion retains the stream until the asynchronous stop has finished. A timeout
        // cannot invalidate an in-flight callback or leave a pointer into the Rust engine.
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [stream stopCaptureWithCompletionHandler:^(NSError* error) {
          (void)error;
          [stream removeStreamOutput:self type:SCStreamOutputTypeAudio error:nil];
          [stream removeStreamOutput:self type:SCStreamOutputTypeScreen error:nil];
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    }
}
@end

static BOOL snow_request_microphone(void) {
    if (![[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"]) {
        return NO;
    }
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusAuthorized) {
        return YES;
    }
    if (status != AVAuthorizationStatusNotDetermined) {
        return NO;
    }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    // The result and semaphore are block-owned, including after a permission-dialog timeout.
    __block BOOL granted = NO;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL allowed) {
                               granted = allowed;
                               dispatch_semaphore_signal(done);
                             }];
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) ==
               0 &&
           granted;
}

static int32_t snow_start_microphone(SnowAudioSource* source, NSString* uid, NSString** error) {
    if (!snow_request_microphone()) {
        *error = @"Microphone access was not granted. In System Settings > Privacy & Security > "
                 @"Microphone, allow this application, then restart recording";
        return -2;
    }
    if (!uid.length) {
        uid = snow_device_string(snow_default_input(), kAudioDevicePropertyDeviceUID);
    }
    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:uid];
    if (!device || ![device hasMediaType:AVMediaTypeAudio]) {
        *error = @"The selected microphone is unavailable";
        return -3;
    }
    NSError* native_error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                                        error:&native_error];
    if (!input) {
        *error = native_error.localizedDescription ?: @"Unable to open the selected microphone";
        return -3;
    }
    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    source.session = session;
    AVCaptureAudioDataOutput* output = [[AVCaptureAudioDataOutput alloc] init];
    source.microphoneOutput = output;
    [output setSampleBufferDelegate:source queue:source.queue];
    [session beginConfiguration];
    if (![session canAddInput:input] || ![session canAddOutput:output]) {
        [session commitConfiguration];
        *error = @"The selected microphone cannot be used for audio capture";
        return -3;
    }
    [session addInput:input];
    [session addOutput:output];
    [session commitConfiguration];
    __weak SnowAudioSource* weak_source = source;
    source.errorObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVCaptureSessionRuntimeErrorNotification
                    object:session
                     queue:nil
                usingBlock:^(NSNotification* notification) {
                  NSError* failure = notification.userInfo[AVCaptureSessionErrorKey];
                  [weak_source fail:failure.localizedDescription ?: @"Microphone capture stopped"];
                }];
    [session startRunning];
    if (!session.isRunning) {
        *error = @"Unable to start microphone capture";
        return -1;
    }
    return 0;
}

static int32_t snow_start_system(SnowAudioSource* source, NSString** error) {
    if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
        *error = @"System audio access was denied. In System Settings > Privacy & Security > "
                 @"Screen & System Audio Recording, allow this application, then restart recording";
        return -2;
    }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block SCShareableContent* content = nil;
    __block NSError* native_error = nil;
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* result, NSError* failure) {
                                   content = result;
                                   native_error = failure;
                                   dispatch_semaphore_signal(done);
                                 }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0) {
        *error = @"ScreenCaptureKit audio setup timed out";
        return -1;
    }
    SCDisplay* display = content.displays.firstObject;
    if (!display) {
        *error = native_error.localizedDescription
                     ?: @"No display is available for system audio capture";
        return -1;
    }
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                      excludingWindows:@[]];
    SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
    configuration.width = 2;
    configuration.height = 2;
    configuration.minimumFrameInterval = CMTimeMake(1, 1);
    configuration.showsCursor = NO;
    configuration.capturesAudio = YES;
    configuration.sampleRate = 48000;
    configuration.channelCount = 2;
    configuration.excludesCurrentProcessAudio = NO;
    SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                          configuration:configuration
                                               delegate:source];
    source.stream = stream;
    // SCK expects a screen sink for a display stream even when only audio is consumed. Discard the
    // tiny 1 fps video frames; they never enter the audio queue or the recording's video pipeline.
    if (![stream addStreamOutput:source
                            type:SCStreamOutputTypeAudio
              sampleHandlerQueue:source.queue
                           error:&native_error] ||
        ![stream addStreamOutput:source
                            type:SCStreamOutputTypeScreen
              sampleHandlerQueue:source.queue
                           error:&native_error]) {
        *error = native_error.localizedDescription;
        return -1;
    }
    done = dispatch_semaphore_create(0);
    [stream startCaptureWithCompletionHandler:^(NSError* failure) {
      native_error = failure;
      [source.condition lock];
      BOOL closed = source.closed;
      [source.condition unlock];
      if (closed && !failure) {
          // A successful start may arrive after our setup timeout and its cleanup attempt.
          [stream stopCaptureWithCompletionHandler:^(NSError* stop_error) {
            (void)stop_error;
          }];
      }
      dispatch_semaphore_signal(done);
    }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0) {
        *error = @"Starting system audio capture timed out";
        return -1;
    }
    if (native_error) {
        *error = native_error.localizedDescription;
        return -1;
    }
    return 0;
}

void* snow_audio_start(uint8_t microphone, const char* device, uint32_t rate, uint16_t channels,
                       size_t queue_depth, int32_t* status, char* error, size_t error_capacity) {
    @autoreleasepool {
        if (!rate || rate > 192000 || !channels || channels > 2 || !queue_depth ||
            queue_depth > 1024) {
            *status = -4;
            snow_audio_error(error, error_capacity,
                             @"macOS audio requires 1 or 2 channels at up to 192 kHz");
            return NULL;
        }
        SnowAudioSource* source = [[SnowAudioSource alloc] init];
        source.capacity = queue_depth;
        source.outputFormat = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatInt16
                                                               sampleRate:rate
                                                                 channels:channels
                                                              interleaved:YES];
        NSString* failure = nil;
        NSString* uid = device ? [NSString stringWithUTF8String:device] : @"";
        @try {
            *status = microphone ? snow_start_microphone(source, uid, &failure)
                                 : snow_start_system(source, &failure);
        } @catch (NSException* exception) {
            failure = exception.reason;
            *status = -1;
        }
        if (*status != 0) {
            [source close];
            snow_audio_error(error, error_capacity, failure);
            return NULL;
        }
        return (__bridge_retained void*)source;
    }
}

int32_t snow_audio_poll(void* handle, int16_t* samples, size_t sample_capacity,
                        SnowAudioPacket* packet, uint32_t timeout_ms, char* error,
                        size_t error_capacity) {
    @autoreleasepool {
        SnowAudioSource* source = (__bridge SnowAudioSource*)handle;
        [source.condition lock];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:(double)timeout_ms / 1000.0];
        while (!source.packets.count && !source.closed && !source.failure && timeout_ms) {
            if (![source.condition waitUntilDate:deadline]) {
                break;
            }
        }
        if (source.failure || source.closed) {
            snow_audio_error(error, error_capacity, source.failure ?: @"Audio capture stopped");
            [source.condition unlock];
            return -1;
        }
        SnowAudioChunk* chunk = source.packets.firstObject;
        if (!chunk) {
            [source.condition unlock];
            return 0;
        }
        if (chunk.samples.length / sizeof(int16_t) > sample_capacity) {
            snow_audio_error(error, error_capacity, @"Audio destination buffer is too small");
            [source.condition unlock];
            return -1;
        }
        memcpy(samples, chunk.samples.bytes, chunk.samples.length);
        packet->frames = chunk.frames;
        packet->end_host_ns = chunk.endHostNs;
        packet->dropped_frames = source.droppedFrames;
        source.droppedFrames = 0;
        [source.packets removeObjectAtIndex:0];
        [source.condition unlock];
        return 1;
    }
}

void snow_audio_stop(void* handle) {
    @autoreleasepool {
        SnowAudioSource* source = CFBridgingRelease(handle);
        [source close];
    }
}
