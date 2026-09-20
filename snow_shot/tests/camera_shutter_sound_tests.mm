#include "camerashuttersound.h"

#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>

#include <QCoreApplication>
#include <QFile>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

NSHashTable<AVAudioPlayer*>* players;
int playCalls = 0;
int stopCalls = 0;
BOOL acceptPlayback = YES;

// Keep real resource loading and native MP3 decoding, replacing only the
// hardware boundary so this regression is deterministic without audio devices.
BOOL recordPlay(AVAudioPlayer* player, SEL) {
    ++playCalls;
    require(player.duration > 0, "native player did not decode the embedded shutter MP3");
    require(player.currentTime == 0, "each capture must start its sound at the beginning");
    require(player.delegate != nil, "asynchronous playback has no completion owner");
    [players addObject:player];
    return acceptPlayback;
}

void recordStop(AVAudioPlayer* player, SEL) {
    ++stopCalls;
    require(player.delegate == nil, "shutdown must detach callbacks before stopping playback");
}

NSUInteger livePlayers() {
    @autoreleasepool {
        return players.allObjects.count;
    }
}

void finishOne(bool success, bool decodeError = false) {
    @autoreleasepool {
        AVAudioPlayer* player = players.allObjects.firstObject;
        require(player != nil, "playback was released before its asynchronous completion");
        if (decodeError) {
            [player.delegate
                audioPlayerDecodeErrorDidOccur:player
                                         error:[NSError errorWithDomain:NSCocoaErrorDomain
                                                                   code:1
                                                               userInfo:nil]];
        } else {
            [player.delegate audioPlayerDidFinishPlaying:player successfully:success ? YES : NO];
        }
        require(player.delegate == nil, "completed playback kept a dangling delegate");
    }
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        players = [[NSHashTable weakObjectsHashTable] retain];
        Method playMethod = class_getInstanceMethod([AVAudioPlayer class], @selector(play));
        Method stopMethod = class_getInstanceMethod([AVAudioPlayer class], @selector(stop));
        const IMP originalPlay =
            method_setImplementation(playMethod, reinterpret_cast<IMP>(recordPlay));
        const IMP originalStop =
            method_setImplementation(stopMethod, reinterpret_cast<IMP>(recordStop));
        auto app = std::make_unique<QCoreApplication>(argc, argv);
        require(QFile::exists(QStringLiteral(":/snow-shot/audios/camera_shutter.mp3")),
                "shutter audio must be available without an installed or source-tree audio path");

        playCameraShutterSound();
        playCameraShutterSound();
        require(playCalls == 2 && livePlayers() == 2,
                "each request must start and retain its own native playback");
        finishOne(true);
        require(livePlayers() == 1, "successful completion leaked or stopped another playback");
        finishOne(false);
        require(livePlayers() == 0, "unsuccessful completion leaked its native player");

        acceptPlayback = NO;
        playCameraShutterSound();
        require(playCalls == 3 && livePlayers() == 0, "failed playback start leaked its player");
        acceptPlayback = YES;
        playCameraShutterSound();
        finishOne(false, true);
        require(livePlayers() == 0, "decode error leaked its native player");

        playCameraShutterSound();
        require(livePlayers() == 1, "later capture failed after a playback error");
        app.reset();
        require(livePlayers() == 0 && stopCalls >= 1,
                "application shutdown did not stop and release active playback");

        method_setImplementation(playMethod, originalPlay);
        method_setImplementation(stopMethod, originalStop);
        [players release];
    }
    return 0;
}
