#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTANIMATIONPLAYBACK_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTANIMATIONPLAYBACK_H

#include "snow_shot/image/screenshotanimationsource.h"

#include <QObject>
#include <QTransform>

#include <atomic>
#include <functional>
#include <memory>

// Playback owns timing and decoding. The host acknowledges actual painting before a
// prepared frame becomes the displayed frame and an exportable snapshot.
class ScreenshotAnimationPlayback final : public QObject {
  public:
    struct Hooks {
        std::function<void(QImage original, QImage transformed, int index, quint64 token)>
            frameReady;
        std::function<void()> stateChanged;
        std::function<void(QString)> error;
        // Restore the last painted raster when visibility cancels an unpainted publication.
        std::function<void(quint64 token)> frameDiscarded;
    };
    struct DecodeRequest {
        std::shared_ptr<const ScreenshotAnimationSource> source;
        int frameIndex = 0;
        QTransform transform;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    using DecodeCompletion =
        std::function<void(QImage original, QImage transformed, QString error)>;
    using PreparationCancellation = std::shared_ptr<std::atomic_bool>;
    struct ImagePreparationRequest {
        QImage original;
        QTransform transform;
        PreparationCancellation cancelled;
    };
    struct PreparedTransform {
        QImage original;
        QImage transformed;
        QTransform transform;
        int frameIndex = 0;
        quint64 token = 0;
    };
    using TransformCompletion = std::function<void(PreparedTransform, QString error)>;
    // Optional execution boundaries support deterministic clocks and alternate executors.
    // Supplied scheduler, decode and preparation completions run on this object's thread.
    struct Options {
        std::function<qint64()> monotonicNanoseconds;
        // Replaces this controller's pending deadline; no preceding cancellation is required.
        std::function<void(qint64 deadlineNs, std::function<void()>)> scheduleDeadline;
        // Expected late delivery for the supplied scheduler; the shared default
        // coordinator accounts for its own coalescing tolerance automatically.
        qint64 nominalDeadlineLatenessNs = 0;
        std::function<void()> cancelDeadline;
        std::function<void(DecodeRequest, DecodeCompletion)> decode;
        std::function<void(ImagePreparationRequest, DecodeCompletion)> prepare;
    };
    struct Diagnostics {
        quint64 deadlineWakeups = 0;
        quint64 coordinatorWakeups = 0; // Shared across default-executor pins.
        quint64 activeDeadlines = 0;
        quint64 decodeRequests = 0;
        quint64 publishedFrames = 0;
        quint64 displayedFrames = 0;
        quint64 staleCompletions = 0;
        quint64 completedDecodes = 0;
        qint64 decodeLatencyNs = 0; // Submission through completion, including queue delay.
        qint64 maximumDecodeLatencyNs = 0;
        qint64 preparationLeadNs = 0;
        qint64 cachedBytes = 0;          // Global optional cache, not duplicated per pin.
        qint64 reservedWorkingBytes = 0; // Global conservative mandatory reservation.
        quint64 cacheHits = 0;
        quint64 cacheMisses = 0;
        quint64 decoderResets = 0;
        quint64 compositedFrames = 0;
    };

    explicit ScreenshotAnimationPlayback(QObject* parent = nullptr);
    ScreenshotAnimationPlayback(Hooks hooks, QObject* parent = nullptr);
    ScreenshotAnimationPlayback(Hooks hooks, Options options, QObject* parent = nullptr);
    ~ScreenshotAnimationPlayback() override;

    void setSource(std::shared_ptr<const ScreenshotAnimationSource> source, bool playing = true,
                   int speed = 100, QImage initialTransformed = {});
    bool adoptCroppedSource(std::shared_ptr<const ScreenshotAnimationSource> source,
                            QImage original, QImage transformed, const QTransform& transform);
    void clear();
    void setSuspended(bool suspended);
    void setTransform(const QTransform& transform);
    // Freezes the painted frame and its remaining source time until adoption is painted.
    // Latest preparation wins; canceled/obsolete completions are not delivered.
    quint64 prepareTransform(const QTransform& transform, TransformCompletion completion);
    bool adoptPreparedTransform(const PreparedTransform& prepared);
    // Cancels an unadopted choice. Adopted geometry keeps its raster until painting.
    void cancelTransformPreparation();
    // One-off preparation does not change playback. Set the returned flag to cancel;
    // completion runs on context's thread only while context is alive and not canceled.
    static PreparationCancellation prepareImage(QObject* context, QImage original,
                                                const QTransform& transform,
                                                DecodeCompletion completion);
    void setRefreshRate(qreal refreshRate);
    void play();
    void pause();
    void toggle();
    void nextFrame();
    void previousFrame();
    void decreaseSpeed();
    void increaseSpeed();
    void resetSpeed();
    void framePainted(quint64 token);

    [[nodiscard]] bool requestedPlaying() const;
    [[nodiscard]] bool effectivePlaying() const;
    [[nodiscard]] bool suspended() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] int speedPercent() const;
    [[nodiscard]] int frameIndex() const;
    [[nodiscard]] quint64 currentFrameToken() const;
    [[nodiscard]] Diagnostics diagnostics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif
