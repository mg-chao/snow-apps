#include "../../presentation/capture/camerashuttersound.h"

#import <AVFoundation/AVFoundation.h>

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QObject>
#include <QPointer>

// Each request owns a player until completion, including overlapping captures.
@interface SnowShotShutterPlayback : NSObject <AVAudioPlayerDelegate> {
    NSMutableSet<AVAudioPlayer*>* m_players;
}
- (void)play;
@end

@implementation SnowShotShutterPlayback
- (instancetype)init {
    self = [super init];
    if (self != nil) {
        m_players = [[NSMutableSet alloc] init];
    }
    return self;
}

- (void)dealloc {
    for (AVAudioPlayer* player in m_players) {
        player.delegate = nil;
        [player stop];
    }
    [m_players release];
    [super dealloc];
}

- (void)play {
    QFile audio(QStringLiteral(":/snow-shot/audios/camera_shutter.mp3"));
    if (!audio.open(QIODevice::ReadOnly)) {
        qWarning("Camera shutter audio is unavailable: %s", qPrintable(audio.errorString()));
        return;
    }
    const QByteArray bytes = audio.readAll();
    NSData* data = [NSData dataWithBytes:bytes.constData()
                                  length:static_cast<NSUInteger>(bytes.size())];
    NSError* error = nil;
    AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithData:data error:&error];
    if (player == nil) {
        qWarning("Failed to open camera shutter audio: %s", error.localizedDescription.UTF8String);
        return;
    }
    player.delegate = self;
    [m_players addObject:player];
    if (![player play]) {
        qWarning("Failed to play camera shutter audio");
        player.delegate = nil;
        [m_players removeObject:player];
    }
    [player release];
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)success {
    if (!success) {
        qWarning("Camera shutter audio playback did not finish successfully");
    }
    player.delegate = nil;
    [m_players removeObject:player];
}

- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer*)player error:(NSError*)error {
    qWarning("Failed to decode camera shutter audio: %s", error.localizedDescription.UTF8String);
    player.delegate = nil;
    [m_players removeObject:player];
}
@end

namespace {
class ShutterPlaybackOwner final : public QObject {
  public:
    explicit ShutterPlaybackOwner(QObject* parent) : QObject(parent) {}
    ~ShutterPlaybackOwner() override {
        [playback release];
    }

    SnowShotShutterPlayback* playback = [[SnowShotShutterPlayback alloc] init];
};
} // namespace

void playCameraShutterSound() {
    // DirectCaptureWorkflow invokes this on the application thread. Tie native
    // playback to that application's lifetime rather than a capture worker.
    static QPointer<ShutterPlaybackOwner> owner;
    if (!owner) {
        owner = new ShutterPlaybackOwner(QCoreApplication::instance());
    }
    @autoreleasepool {
        [owner->playback play];
    }
}
