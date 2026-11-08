#include "snow_shot/presentation/screenshotanimationplayback.h"
#include "../src/image/snowimageqtcodec.h"
#include "pinned_animation_fixtures.h"
#include "pinned_animation_native_fixtures.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QColor>
#include <QColorSpace>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QThread>
#include <QProcess>
#include <QtEndian>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>

namespace {
constexpr qint64 ms = 1'000'000;
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool samePixels(const QImage& left, const QImage& right) {
    if (left.size() != right.size())
        return false;
    for (int y = 0; y < left.height(); ++y)
        for (int x = 0; x < left.width(); ++x)
            if (left.pixelColor(x, y) != right.pixelColor(x, y))
                return false;
    return true;
}

void destructionKeepsPlaybackAvailableToDeadlineCancellation() {
    int cancellations = 0;
    ScreenshotAnimationPlayback* retiring = nullptr;
    ScreenshotAnimationPlayback::Options options;
    options.cancelDeadline = [&] {
        require(!retiring->requestedPlaying() && retiring->frameIndex() == 0,
                "deadline cancellation must be able to inspect the retiring playback controller");
        ++cancellations;
    };
    auto playback = std::make_unique<ScreenshotAnimationPlayback>(
        ScreenshotAnimationPlayback::Hooks{}, std::move(options));
    retiring = playback.get();
    playback.reset();
    require(cancellations == 1, "playback destruction must cancel its scheduler exactly once");
}

QByteArray profiledApng(const QColorSpace& colorSpace) {
    QImage profileImage(1, 1, QImage::Format_ARGB32_Premultiplied);
    profileImage.fill(Qt::red);
    profileImage.setColorSpace(colorSpace);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    require(buffer.open(QIODevice::WriteOnly) && profileImage.save(&buffer, "PNG"),
            "Qt must encode the color-profile fixture metadata");
    QByteArray colorChunks;
    for (qsizetype position = 8; position <= encoded.size() - 12;) {
        const auto length =
            static_cast<qsizetype>(qFromBigEndian<quint32>(encoded.constData() + position));
        require(length <= encoded.size() - position - 12,
                "the generated profile PNG must contain complete chunks");
        const auto type = encoded.mid(position + 4, 4);
        if (type == "iCCP" || type == "sRGB" || type == "gAMA" || type == "cHRM" || type == "cICP")
            colorChunks.append(encoded.mid(position, length + 12));
        position += length + 12;
    }
    require(!colorChunks.isEmpty(), "the profile fixture must contain encoded color metadata");
    auto result = pinned_animation_fixtures::images().back().bytes;
    result.insert(33, colorChunks); // Color metadata follows IHDR and precedes animation data.
    return result;
}

std::shared_ptr<ScreenshotAnimationSource> source(quint64 plays = 0) {
    auto result = std::make_shared<ScreenshotAnimationSource>();
    result->canvasSize = QSize(4, 3);
    result->frameDurationsNs = {100 * ms, 200 * ms, 50 * ms};
    result->totalPlayCount = plays;
    result->firstFrame = QImage(result->canvasSize, QImage::Format_ARGB32_Premultiplied);
    result->firstFrame.fill(Qt::red);
    return result;
}

struct Harness {
    struct Job {
        ScreenshotAnimationPlayback::DecodeRequest request;
        ScreenshotAnimationPlayback::DecodeCompletion completion;
    };
    struct Preparation {
        ScreenshotAnimationPlayback::ImagePreparationRequest request;
        ScreenshotAnimationPlayback::DecodeCompletion completion;
    };
    qint64 now = 0;
    std::optional<qint64> deadline;
    std::function<void()> wakeup;
    std::vector<Job> jobs;
    std::vector<Preparation> preparations;
    int publications = 0;
    int changes = 0;
    int errors = 0;
    int discarded = 0;
    int deadlineCancellations = 0;
    quint64 pendingToken = 0;
    int pendingIndex = 0;
    std::function<void()> discardAction;
    std::function<void()> publicationAction;
    QImage lastOriginal;
    QImage lastTransformed;
    bool acknowledge = true;
    bool completeInline = false;
    int inlineCompletions = 0;
    std::unique_ptr<ScreenshotAnimationPlayback> playback;

    explicit Harness(bool fakeDecode = true, qint64 nominalLateness = 0) {
        ScreenshotAnimationPlayback::Hooks hooks;
        hooks.frameReady = [this](QImage original, QImage transformed, int index, quint64 token) {
            ++publications;
            lastOriginal = std::move(original);
            lastTransformed = std::move(transformed);
            pendingToken = token;
            pendingIndex = index;
            if (acknowledge)
                playback->framePainted(token);
            if (publicationAction)
                publicationAction();
        };
        hooks.stateChanged = [this] { ++changes; };
        hooks.error = [this](const QString&) { ++errors; };
        hooks.frameDiscarded = [this](quint64) {
            ++discarded;
            if (discardAction)
                discardAction();
        };
        ScreenshotAnimationPlayback::Options options;
        options.monotonicNanoseconds = [this] { return now; };
        options.nominalDeadlineLatenessNs = nominalLateness;
        options.scheduleDeadline = [this](qint64 value, std::function<void()> callback) {
            deadline = value;
            wakeup = std::move(callback);
        };
        options.cancelDeadline = [this] {
            ++deadlineCancellations;
            deadline.reset();
            wakeup = {};
        };
        if (fakeDecode) {
            options.decode = [this](auto request, auto completion) {
                if (completeInline) {
                    require(++inlineCompletions <= 4,
                            "immediate completions must not recurse through repeated preparation");
                    QImage image(request.source->canvasSize, QImage::Format_ARGB32_Premultiplied);
                    image.fill(Qt::blue);
                    completion(image, image.transformed(request.transform), {});
                } else {
                    jobs.push_back({std::move(request), std::move(completion)});
                }
            };
            options.prepare = [this](auto request, auto completion) {
                preparations.push_back({std::move(request), std::move(completion)});
            };
        }
        playback =
            std::make_unique<ScreenshotAnimationPlayback>(std::move(hooks), std::move(options));
    }
    void start(quint64 plays = 0) {
        playback->setSource(source(plays));
        playback->setSuspended(false);
    }
    void wake(qint64 time) {
        now = time;
        require(deadline && now >= *deadline, "a test wake must reach its scheduled deadline");
        auto callback = std::move(wakeup);
        deadline.reset();
        callback();
    }
    void complete(const QString& error = {}) {
        require(!jobs.empty(), "a test decode completion needs an admitted job");
        auto job = std::move(jobs.front());
        jobs.erase(jobs.begin());
        QImage image(job.request.source->canvasSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor::fromRgb(20 + job.request.frameIndex * 70 % 235, 30, 40));
        const auto transformed = image.transformed(job.request.transform);
        job.completion(std::move(image), transformed, error);
    }
    void completePreparation(const QString& error = {}) {
        require(!preparations.empty(), "a test preparation completion needs an admitted job");
        auto job = std::move(preparations.front());
        preparations.erase(preparations.begin());
        const QImage transformed = job.request.original.transformed(job.request.transform);
        job.completion(std::move(job.request.original), transformed, error);
    }
};

std::shared_ptr<ScreenshotAnimationSource>
croppedSource(const ScreenshotAnimationPlayback::FrameHold& hold) {
    auto result = std::make_shared<ScreenshotAnimationSource>(*hold.source);
    result->canvasSize = QSize(2, 3);
    result->sourceCropRect = QRect(0, 0, 2, 3);
    result->firstFrame = snowCanvasCopyImage(result->firstFrame, result->sourceCropRect);
    result->firstDisplayFrame = result->firstFrame;
    return result;
}

void cropTransactionsRetainPaintedFrameAndDeferredSteps() {
    Harness h;
    h.start();
    h.playback->pause();
    while (!h.jobs.empty())
        h.complete();
    const auto hold = h.playback->holdFrameForCrop();
    require(hold && hold->frameIndex == 0 && hold->original == h.lastOriginal,
            "crop holds the painted pixels and their frame identity");
    h.playback->nextFrame();
    h.playback->nextFrame();
    h.playback->previousFrame();
    h.playback->increaseSpeed();
    require(h.jobs.empty() && h.playback->frameIndex() == 0 && !h.playback->requestedPlaying(),
            "crop preparation defers accumulated steps without changing painted identity");
    const auto next = croppedSource(*hold);
    const auto pixels = snowCanvasCopyImage(hold->original, QRect(QPoint(), next->canvasSize));
    auto wrong = *hold;
    wrong.frameIndex = 1;
    require(!h.playback->reserveCroppedSource(wrong, next, pixels, pixels, {}),
            "crop reservation rejects pixels labeled with another frame index");
    require(h.playback->reserveCroppedSource(*hold, next, pixels, pixels, {}) &&
                h.playback->stageReservedCroppedSource(hold->token) && h.publications == 1,
            "crop source adoption stages without invoking presentation callbacks");
    h.acknowledge = false;
    require(h.playback->publishReservedCroppedSource(hold->token) && h.pendingIndex == 0 &&
                h.lastOriginal == pixels && h.playback->frameIndex() == 0 && h.jobs.empty(),
            "prepared frame A is published as A before deferred frame B can decode");
    h.playback->setSuspended(true);
    h.playback->pause();
    require(h.playback->frameIndex() == hold->frameIndex &&
                h.playback->currentFrameToken() == h.pendingToken,
            "a staged crop keeps the last paint identity until actual acknowledgement");
    h.playback->framePainted(h.pendingToken);
    require(h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 1 &&
                h.jobs.front().request.source == next && h.playback->speedPercent() == 110,
            "the crop paint releases deferred steps against the cropped source at latest speed");
    h.acknowledge = true;
    h.complete();
    require(h.playback->frameIndex() == 1 && h.lastOriginal.size() == QSize(2, 3) &&
                !h.playback->requestedPlaying(),
            "deferred crop stepping displays the matching frame and remains paused");
}

void cropReservationCancellationAndReplacementPreserveSource() {
    Harness h;
    h.start();
    h.playback->pause();
    while (!h.jobs.empty())
        h.complete();
    const auto first = h.playback->holdFrameForCrop();
    require(first.has_value(), "a painted source admits crop preparation");
    h.playback->nextFrame();
    h.playback->cancelCropPreparation(first->token);
    require(!h.playback->cropHoldValid(first->token) && h.jobs.size() == 1 &&
                h.jobs.front().request.source == first->source,
            "rejected crop replays legitimate steps against the unchanged original source");
    h.complete();
    const auto second = h.playback->holdFrameForCrop();
    require(second.has_value(), "canceled crop leaves the source usable");
    h.playback->nextFrame();
    h.playback->cancelCropPreparation(second->token, true);
    require(h.jobs.empty(), "source teardown can discard deferred crop steps");
    const auto third = h.playback->holdFrameForCrop();
    require(third.has_value(), "crop can be retried after cancellation");
    const auto next = croppedSource(*third);
    const auto pixels = snowCanvasCopyImage(third->original, QRect(QPoint(), next->canvasSize));
    require(h.playback->reserveCroppedSource(*third, next, pixels, pixels, {}),
            "an unchanged held frame admits its prepared cropped source");
    h.playback->setSource(source(), false);
    require(!h.playback->stageReservedCroppedSource(third->token) &&
                !h.playback->publishReservedCroppedSource(third->token) &&
                h.lastOriginal.size() == QSize(4, 3),
            "replacement invalidates every phase of the previous crop transaction");
}

void cropCallbacksMayDestroyPlayback() {
    Harness discard;
    discard.start();
    discard.playback->pause();
    while (!discard.jobs.empty())
        discard.complete();
    discard.acknowledge = false;
    discard.playback->nextFrame();
    discard.complete();
    discard.discardAction = [&] { discard.playback.reset(); };
    const auto hold = discard.playback->holdFrameForCrop();
    require(!hold && !discard.playback,
            "discard callbacks may destroy the controller while crop acquires a frame hold");

    Harness publish;
    publish.start();
    publish.playback->pause();
    while (!publish.jobs.empty())
        publish.complete();
    const auto current = publish.playback->holdFrameForCrop();
    require(current.has_value(), "publication lifetime fixture holds a painted frame");
    const auto next = croppedSource(*current);
    const auto pixels = snowCanvasCopyImage(current->original, QRect(QPoint(), next->canvasSize));
    require(publish.playback->reserveCroppedSource(*current, next, pixels, pixels, {}) &&
                publish.playback->stageReservedCroppedSource(current->token),
            "publication lifetime fixture stages its crop");
    publish.acknowledge = false;
    publish.publicationAction = [&] { publish.playback.reset(); };
    require(publish.playback->publishReservedCroppedSource(current->token) && !publish.playback,
            "crop publication tolerates synchronous destruction by its recipient");
}

void preparedTransformsFreezeSourceTimeUntilPaint() {
    Harness h;
    h.start();
    h.now = 40 * ms;
    h.acknowledge = false;
    const QImage painted = h.lastOriginal;
    const quint64 decodes = h.playback->diagnostics().decodeRequests;
    std::optional<ScreenshotAnimationPlayback::PreparedTransform> prepared;
    const quint64 token = h.playback->prepareTransform(
        QTransform().rotate(90), [&](auto result, const QString& error) {
            require(error.isEmpty(), "successful transform preparation must have no error");
            prepared = std::move(result);
        });
    require(token != 0 && h.playback->requestedPlaying() && !h.playback->effectivePlaying() &&
                h.playback->busy() && !h.deadline && h.publications == 1 &&
                h.preparations.front().request.original == painted &&
                h.jobs.front().request.cancelled->load(),
            "transform preparation freezes the painted original and cancels playback lookahead");
    h.now = 1000 * ms;
    h.completePreparation();
    require(prepared && prepared->token == token && prepared->frameIndex == 0 &&
                !h.playback->busy() && h.publications == 1 && !h.deadline,
            "worker completion awaits explicit geometry adoption without advancing the clock");
    require(
        h.playback->adoptPreparedTransform(*prepared) && h.publications == 2 &&
            h.lastOriginal == painted && h.lastTransformed.size() == painted.size().transposed() &&
            !h.playback->effectivePlaying() && h.playback->diagnostics().decodeRequests == decodes,
        "geometry adoption publishes prepared pixels exactly once without duplicate decoding");
    require(!h.playback->adoptPreparedTransform(*prepared),
            "the same prepared transformation cannot be adopted twice");
    h.now = 1100 * ms;
    h.playback->framePainted(h.pendingToken);
    require(h.playback->effectivePlaying() && h.deadline == 1160 * ms &&
                h.playback->frameIndex() == 0,
            "painting resumes the remaining source-frame delay after a long transform operation");
}

void latestPreparedTransformSurvivesHiddenAndPausedStates() {
    Harness h;
    h.start();
    h.now = 30 * ms;
    h.acknowledge = false;
    int callbacks = 0;
    std::optional<ScreenshotAnimationPlayback::PreparedTransform> prepared;
    const auto completed = [&](auto result, const QString& error) {
        require(error.isEmpty(), "latest transform preparation must complete successfully");
        ++callbacks;
        prepared = std::move(result);
    };
    h.playback->prepareTransform(QTransform().rotate(90), completed);
    h.playback->prepareTransform(QTransform().rotate(180), completed);
    require(h.preparations.front().request.cancelled->load(),
            "a newer transform cancels the preceding worker preparation");
    h.completePreparation();
    require(callbacks == 0, "canceled transformation completions cannot reach the host");
    h.completePreparation();
    require(callbacks == 1 && prepared && h.playback->adoptPreparedTransform(*prepared),
            "the latest transformation is the only result eligible for geometry adoption");
    const quint64 stalePaint = h.pendingToken;
    const auto stalePrepared = *prepared;
    h.playback->setSuspended(true);
    h.playback->pause();
    h.playback->play();
    require(h.playback->currentFrameToken() == stalePaint && h.discarded == 0 && !h.deadline,
            "an adopted geometry raster survives hidden and playback-intent changes until paint");
    prepared.reset();
    h.playback->prepareTransform({}, completed);
    require(h.discarded == 1 && h.preparations.front().request.original.pixelColor(0, 0) == Qt::red,
            "superseding an unpainted transform uses the last painted source original");
    h.now = 2000 * ms;
    h.completePreparation();
    require(prepared && h.playback->adoptPreparedTransform(*prepared) &&
                !h.playback->adoptPreparedTransform(stalePrepared),
            "hidden transformation and reset complete without adopting obsolete preparations");
    h.playback->framePainted(stalePaint);
    require(h.playback->diagnostics().displayedFrames == 1,
            "an obsolete paint acknowledgement cannot commit a superseded transform");
    h.playback->setSuspended(false);
    require(!h.deadline && !h.playback->effectivePlaying(),
            "revealing an adopted transform waits for its first actual paint");
    h.playback->framePainted(h.pendingToken);
    require(h.playback->effectivePlaying() && h.deadline == 2070 * ms,
            "revealed transformed playback resumes the original remaining source delay");
}

void preparationCancellationFailureAndQueuedStepsPreserveIntent() {
    Harness h;
    h.start();
    h.now = 40 * ms;
    int callbacks = 0;
    h.playback->prepareTransform(QTransform().rotate(90),
                                 [&](const auto&, const QString&) { ++callbacks; });
    h.playback->increaseSpeed();
    h.now = 1000 * ms;
    h.playback->cancelTransformPreparation();
    require(h.playback->requestedPlaying() && h.playback->effectivePlaying() &&
                h.deadline == 1'054'545'455 && h.preparations.front().request.cancelled->load(),
            "geometry rejection resumes remaining media time at the latest playback speed");
    h.completePreparation();
    require(callbacks == 0, "canceled geometry preparation cannot publish a late result");
    h.playback->prepareTransform(
        QTransform().rotate(90), [&](const auto& result, const QString& error) {
            require(result.token == 0 && !error.isEmpty(),
                    "preparation failure is delivered without an adoptable raster");
            ++callbacks;
        });
    h.completePreparation(QStringLiteral("test preparation failure"));
    require(callbacks == 1 && h.playback->effectivePlaying() && h.playback->requestedPlaying(),
            "transform preparation failure preserves the original playing source");

    Harness steps;
    steps.start();
    std::optional<ScreenshotAnimationPlayback::PreparedTransform> prepared;
    steps.playback->prepareTransform(QTransform().rotate(90), [&](auto result, const QString&) {
        prepared = std::move(result);
    });
    steps.playback->nextFrame();
    steps.playback->nextFrame();
    steps.playback->previousFrame();
    require(!steps.playback->requestedPlaying() && steps.jobs.size() == 1 && !steps.deadline,
            "manual steps during preparation accumulate without decoding against old geometry");
    steps.completePreparation();
    require(prepared && steps.playback->adoptPreparedTransform(*prepared),
            "a prepared geometry commits before queued manual steps run");
    steps.complete(); // Canceled ordinary lookahead.
    steps.complete(); // One accumulated manual demand for frame one.
    require(steps.playback->frameIndex() == 1 && !steps.playback->requestedPlaying() &&
                steps.lastTransformed.size() == steps.lastOriginal.size().transposed(),
            "queued manual steps use the adopted transform and remain paused");

    Harness replacement;
    replacement.start();
    replacement.playback->prepareTransform(QTransform().rotate(90),
                                           [&](const auto&, const QString&) { ++callbacks; });
    replacement.playback->clear();
    require(replacement.preparations.front().request.cancelled->load(),
            "closing or clearing a source cancels its transformation worker");
    replacement.completePreparation();
    require(callbacks == 1 && !replacement.playback->busy(),
            "closing rejects all stale prepared-transform completions");
    replacement.acknowledge = false;
    auto next = source();
    next->firstFrame.fill(Qt::blue);
    replacement.playback->setSource(next, false);
    replacement.playback->prepareTransform(QTransform().rotate(90), {});
    require(replacement.preparations.front().request.original.pixelColor(0, 0) == Qt::blue,
            "a replacement prepared before first paint must not reuse the old source original");
}

void failedSupersedingTransformRestoresThePaintedMatrixWithoutDecodingIt() {
    Harness h;
    h.start();
    h.now = 30 * ms;
    h.acknowledge = false;
    std::optional<ScreenshotAnimationPlayback::PreparedTransform> prepared;
    h.playback->prepareTransform(QTransform().rotate(90), [&](auto result, const QString&) {
        prepared = std::move(result);
    });
    h.completePreparation();
    require(prepared && h.playback->adoptPreparedTransform(*prepared),
            "the first unpainted transform must commit before superseding it");
    h.playback->prepareTransform(QTransform().rotate(180), {});
    h.now = 1000 * ms;
    h.completePreparation(QStringLiteral("test superseding preparation failure"));
    require(
        h.publications == 2 && h.discarded == 1 && h.playback->frameIndex() == 0 &&
            h.playback->effectivePlaying() && h.deadline == 1070 * ms &&
            h.jobs.back().request.frameIndex == 1 && h.jobs.back().request.transform.isIdentity(),
        "failure restores the painted matrix and timing without re-decoding the displayed frame");
    prepared.reset();
    h.playback->prepareTransform(QTransform().rotate(180), [&](auto result, const QString&) {
        prepared = std::move(result);
    });
    h.completePreparation();
    h.playback->cancelTransformPreparation();
    require(
        prepared && !h.playback->adoptPreparedTransform(*prepared) && h.publications == 2 &&
            h.jobs.back().request.transform.isIdentity(),
        "geometry rejection invalidates the new prepared choice and preserves the painted matrix");
}

void hostCloseDuringTransformRollbackCancelsPreparation() {
    Harness h;
    h.start();
    h.acknowledge = false;
    std::optional<ScreenshotAnimationPlayback::PreparedTransform> prepared;
    h.playback->prepareTransform(QTransform().rotate(90), [&](auto result, const QString&) {
        prepared = std::move(result);
    });
    h.completePreparation();
    require(prepared && h.playback->adoptPreparedTransform(*prepared),
            "the first adopted transform remains staged before native rollback");
    h.discardAction = [&] { h.playback->clear(); };
    require(h.playback->prepareTransform(QTransform().rotate(180), {}) == 0 &&
                h.preparations.empty() && !h.playback->busy() && !h.playback->requestedPlaying(),
            "a host close during geometry rollback prevents new worker preparation");
    h.complete();
    require(h.publications == 2,
            "cleared rollback rejects previously canceled native decode completions");
}

void startsOnlyAfterPaintingAndVisibility() {
    Harness h;
    h.acknowledge = false;
    h.playback->setSource(source());
    require(h.publications == 1 && !h.deadline && !h.playback->effectivePlaying(),
            "the initial frame must not start a hidden or unpainted animation");
    h.playback->setSuspended(false);
    require(!h.deadline, "visibility alone must not advance an unpainted first frame");
    h.now = 5 * ms;
    h.playback->framePainted(h.pendingToken);
    require(h.playback->effectivePlaying() && h.deadline == 105 * ms,
            "first painting starts the complete initial frame duration");
    h.playback->framePainted(h.pendingToken);
    require(h.playback->diagnostics().displayedFrames == 1,
            "repeated paint acknowledgement must not restart the frame clock");
}

void activeDeadlineReplacementAvoidsCancellation() {
    Harness h;
    h.start();
    const int cancellations = h.deadlineCancellations;
    require(h.deadline == 100 * ms, "the first active deadline is installed");
    h.complete();
    require(h.deadline == 100 * ms && h.deadlineCancellations == cancellations,
            "ready lookahead replaces the same deadline without stopping the native timer");
    h.playback->increaseSpeed();
    require(h.deadline == 90'909'091 && h.deadlineCancellations == cancellations,
            "speed changes replace an active deadline directly without a preceding cancellation");
    h.playback->pause();
    require(!h.deadline && h.deadlineCancellations > cancellations,
            "inactive playback still cancels its pending deadline");
}

void suspensionPreservesIntentAndRemainingDuration() {
    Harness h;
    h.start();
    h.complete();
    h.now = 40 * ms;
    h.playback->setSuspended(true);
    const auto before = h.playback->diagnostics();
    h.now = 100'000 * ms;
    require(h.playback->requestedPlaying() && !h.playback->effectivePlaying() && !h.deadline &&
                h.jobs.empty(),
            "thumbnail and hidden suspension preserve intent without scheduling work");
    const auto after = h.playback->diagnostics();
    require(before.deadlineWakeups == after.deadlineWakeups &&
                before.decodeRequests == after.decodeRequests &&
                before.publishedFrames == after.publishedFrames,
            "a long suspended interval must contain no animation processing");
    h.playback->setSuspended(false);
    require(h.deadline == h.now + 60 * ms,
            "resume preserves the remaining source duration without hidden catch-up");
    h.playback->pause();
    h.playback->setSuspended(true);
    h.playback->setSuspended(false);
    require(!h.playback->requestedPlaying() && !h.deadline,
            "a paused animation must remain paused through thumbnail and hide/show");
}

void steppingWrapsAndRemainsPaused() {
    Harness h;
    h.start();
    h.complete();
    h.playback->previousFrame();
    require(!h.playback->requestedPlaying() && h.jobs.front().request.frameIndex == 2,
            "previous frame wraps from the first frame and pauses playback");
    h.complete();
    require(h.playback->frameIndex() == 2 && !h.deadline,
            "a manual previous frame remains displayed without a timer");
    h.playback->nextFrame();
    h.complete();
    require(h.playback->frameIndex() == 0, "next frame wraps from the last frame");
    h.playback->nextFrame();
    h.playback->nextFrame();
    require(h.jobs.size() == 1 && h.jobs.front().request.cancelled->load(),
            "rapid stepping cancels stale work and keeps one admitted decoder operation");
    h.complete();
    require(h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 2,
            "rapid stepping accumulates commands against the requested frame");
    h.complete();
    require(h.playback->frameIndex() == 2 && !h.playback->requestedPlaying(),
            "only the final requested manual frame is published");
}

void steppingUsesThePaintedFrameDuringPlaybackPreparation() {
    for (int state = 0; state < 3; ++state) {
        Harness h;
        h.start();
        if (state >= 1)
            h.complete();
        if (state == 2) {
            h.acknowledge = false;
            h.wake(100 * ms);
        }
        h.playback->nextFrame();
        if (state == 0)
            h.complete(); // Canceled lookahead retires before the manual decode.
        require(h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 1,
                "Next advances exactly once from the painted frame during every preparation state");
        h.complete();
        h.playback->framePainted(h.pendingToken);
        require(h.playback->frameIndex() == 1 && !h.playback->requestedPlaying(),
                "Next leaves the requested one-frame advance painted and paused");
    }
}

void speedChangesPreserveSourceTimeAndClamp() {
    Harness h;
    h.start();
    h.complete();
    h.now = 25 * ms;
    h.playback->increaseSpeed();
    require(h.playback->speedPercent() == 110 && h.deadline && *h.deadline == h.now + 68'181'819,
            "speed changes scale the remaining source time without restarting the frame");
    for (int index = 0; index < 100; ++index)
        h.playback->decreaseSpeed();
    require(h.playback->speedPercent() == 10, "playback speed must stay at or above ten percent");
    for (int index = 0; index < 100; ++index)
        h.playback->increaseSpeed();
    require(h.playback->speedPercent() == 400, "playback speed must stay at or below four hundred");
    h.playback->resetSpeed();
    require(h.playback->speedPercent() == 100, "reset speed restores one hundred percent");
    h.playback->setSource(source(), false, 160);
    require(h.playback->speedPercent() == 160 && !h.playback->requestedPlaying(),
            "restoration accepts explicit playback state and speed");
    h.playback->setSource(source());
    require(h.playback->speedPercent() == 100 && h.playback->requestedPlaying(),
            "a new source defaults to autoplay at one hundred percent");
}

void finitePlaybackStopsAndPlayRestarts() {
    Harness h;
    h.start(1);
    h.complete();
    h.wake(100 * ms);
    h.complete();
    h.wake(300 * ms);
    h.wake(350 * ms);
    require(!h.playback->requestedPlaying() && !h.deadline && h.playback->frameIndex() == 2,
            "finite playback must stop on its last frame after its final delay");
    h.playback->play();
    require(h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 0,
            "Play after finite completion restarts at the first frame");
    h.complete();
    require(h.playback->frameIndex() == 0 && h.playback->requestedPlaying(),
            "the restarted animation remains requested to play");
}

void finiteCompletionCancelsNextCyclePreparation() {
    Harness h;
    h.start(2);
    h.complete();
    h.wake(100 * ms);
    h.complete();
    h.wake(300 * ms);
    require(h.playback->frameIndex() == 2 && h.jobs.size() == 1 &&
                h.jobs.front().request.frameIndex == 0,
            "the first finite cycle prepares the next cycle after painting its final frame");
    const int publications = h.publications;
    h.wake(800 * ms);
    h.complete();
    require(!h.playback->requestedPlaying() && h.playback->frameIndex() == 2 &&
                h.publications == publications && !h.deadline && h.jobs.empty(),
            "a delayed final wake cancels next-cycle preparation and retains the final frame");
}

void finiteCompletionRetainsForcedFinalFrameAfterCanceledPreparation() {
    Harness h;
    h.start(1);
    h.complete();
    h.wake(100 * ms);
    require(h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 2,
            "the final finite frame may already have a progressing ordinary preparation");
    h.wake(400 * ms);
    h.complete();
    require(!h.playback->requestedPlaying() && h.jobs.size() == 1 &&
                h.jobs.front().request.frameIndex == 2,
            "finite completion retains its forced final demand when canceling the same index");
    h.complete();
    require(h.playback->frameIndex() == 2 && !h.deadline && h.jobs.empty(),
            "the final finite frame is displayed once without restarting background preparation");
}

void staleDecodeAndTransformResultsNeverPublish() {
    Harness h;
    h.start();
    h.wake(100 * ms);
    h.playback->setSuspended(true);
    h.complete();
    require(h.publications == 1 && !h.deadline && h.jobs.empty(),
            "a suspended decode completion cannot publish or schedule more work");
    h.playback->setSuspended(false);
    require(h.deadline.has_value(), "resuming canceled work must restore a deadline");
    h.wake(*h.deadline);
    h.playback->setTransform(QTransform().rotate(90));
    h.complete();
    require(h.jobs.size() == 1 && h.jobs.front().request.transform == QTransform().rotate(90),
            "transform changes discard the old generation and request the displayed frame");
    h.complete();
    require(h.lastTransformed.size() == h.lastOriginal.size().transposed(),
            "new frames use the current transform");
    // Transformation publication resumes a new bounded preparation.
    h.complete();
    h.playback->nextFrame();
    const auto publications = h.publications;
    h.playback->clear();
    h.complete();
    require(h.publications == publications && !h.deadline && !h.playback->busy(),
            "clearing playback rejects late decoder results");
}

void staleSourceAndOrdinaryDeadlinesRespectDecodeOwnership() {
    Harness replaced;
    replaced.start();
    auto replacement = source();
    replacement->firstFrame.fill(Qt::blue);
    replaced.playback->setSource(replacement, false, 130);
    const int published = replaced.publications;
    replaced.complete();
    require(replaced.publications == published &&
                replaced.lastOriginal == replacement->firstFrame &&
                replaced.playback->speedPercent() == 130 && !replaced.playback->requestedPlaying(),
            "replacing the source rejects an older decoder completion and retains restored state");

    Harness progressing;
    progressing.start();
    progressing.wake(100 * ms);
    progressing.now = 320 * ms;
    progressing.playback->setRefreshRate(75);
    require(!progressing.jobs.front().request.cancelled->load(),
            "an ordinary late deadline must not cancel an already progressing source decoder");
    progressing.complete();
    require(progressing.publications == 1 && progressing.jobs.size() == 1 && progressing.deadline &&
                *progressing.deadline > progressing.now,
            "expired complete frames are dropped before preparing a future media occurrence");
    const int futureIndex = progressing.jobs.front().request.frameIndex;
    progressing.complete();
    progressing.wake(*progressing.deadline);
    require(progressing.playback->frameIndex() == futureIndex,
            "sequential composition prepares a complete frame for its future visible occurrence");
}

void preparedInitialTransformAvoidsRedundantDecoding() {
    Harness h;
    const auto animation = source();
    const auto transform = QTransform().rotate(90);
    h.playback->setTransform(transform);
    const QImage initial = animation->firstFrame.transformed(transform);
    h.playback->setSource(animation, false, 100, initial);
    require(h.publications == 1 && h.jobs.empty() && h.lastOriginal == animation->firstFrame &&
                h.lastTransformed == initial,
            "the prepared restored transform publishes frame zero without another conversion");
}

void lateWorkSkipsPresentationAndReportsDecodeFailure() {
    Harness h;
    h.start();
    h.wake(100 * ms);
    h.now = 320 * ms;
    h.complete();
    require(h.publications == 1 && h.jobs.size() == 1 && h.deadline && *h.deadline > h.now,
            "late decoding skips obsolete presentation without GUI catch-up callbacks");
    const int futureIndex = h.jobs.front().request.frameIndex;
    h.complete();
    require(h.playback->frameIndex() == 0 && h.deadline && *h.deadline > h.now,
            "a future prepared frame must wait for its absolute media occurrence");
    h.wake(*h.deadline);
    require(h.playback->frameIndex() == futureIndex,
            "a future complete frame is displayed when its media occurrence becomes current");
    h.complete();
    h.playback->nextFrame();
    h.complete(QStringLiteral("fixture decoding failure"));
    require(h.errors == 1 && !h.playback->requestedPlaying() && !h.deadline && h.jobs.empty(),
            "decode failures preserve the last frame and stop playback once");
    h.playback->play();
    require(h.playback->requestedPlaying() && h.jobs.size() == 1 && h.deadline,
            "an explicit Play after failure admits a fresh decode without background retries");
}

void refreshCapAndPaintBackpressureBoundWork() {
    Harness h;
    auto fast = source();
    fast->frameDurationsNs = {ms, 2 * ms, 3 * ms};
    h.playback->setSource(fast);
    h.playback->setSuspended(false);
    require(h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 2,
            "lookahead prepares the next visible frame instead of every short-delay frame");
    require(h.deadline == 16'666'667,
            "short frame delays must not schedule faster than the display refresh rate");
    h.acknowledge = false;
    h.complete();
    require(h.publications == 1 && h.deadline,
            "a prepared frame must remain unpublished until its presentation deadline");
    h.wake(*h.deadline);
    require(!h.deadline && !h.playback->busy(),
            "one prepared frame awaiting paint must backpressure further decode and wakes");
    const auto oldIndex = h.playback->frameIndex();
    require(oldIndex == 0 && h.pendingIndex != oldIndex,
            "frame identity must remain the last painted frame until acknowledgement");
    h.playback->framePainted(h.pendingToken);
    require(h.playback->frameIndex() == h.pendingIndex && h.deadline,
            "painting commits frame identity and schedules the next bounded presentation");
}

void skippedPresentationsStillBoundRetries() {
    Harness h;
    auto fast = source();
    fast->frameDurationsNs = {ms, 2 * ms, 3 * ms};
    h.playback->setSource(fast);
    h.playback->setSuspended(false);
    h.complete();
    h.wake(18 * ms); // The late wake aliases back to the already painted frame.
    require(h.publications == 1 && h.jobs.empty() && h.deadline &&
                *h.deadline >= h.now + 16'666'667,
            "an aliased presentation reuses ready pixels without decoding or wakes above refresh");

    Harness late;
    late.start();
    late.wake(100 * ms);
    late.now = 360 * ms;
    late.complete();
    require(
        late.publications == 1 && late.jobs.size() == 1 && late.deadline &&
            *late.deadline > late.now + 16'666'667,
        "expired work must prepare a future occurrence with enough time for sequential decoding");
}

void futureAliasesRetainTheirRefreshPermittedOccurrence() {
    for (bool immediate : {false, true}) {
        Harness h(true, 2 * ms);
        auto animation = source();
        animation->frameDurationsNs = {16'666'667, 16'666'667};
        h.playback->setSource(animation);
        h.playback->setSuspended(false);
        h.wake(18'666'667);
        h.now = 32 * ms;
        h.completeInline = immediate;
        h.complete();
        if (!immediate) {
            require(h.jobs.size() == 1,
                    "an expired refresh-limited occurrence admits one future preparation");
            h.complete();
        }
        require(h.jobs.empty() && h.inlineCompletions == (immediate ? 1 : 0) &&
                    h.playback->diagnostics().decodeRequests == 2 && h.publications == 1 &&
                    h.deadline && *h.deadline > h.now + 16'666'667,
                "a matching current frame cannot consume a future occurrence when the refresh "
                "limit would expire its current window");
        h.completeInline = false;
        h.wake(*h.deadline + 2 * ms);
        require(h.playback->frameIndex() == 1 && h.publications == 2,
                "the retained future occurrence paints once at its absolute media deadline");
    }
}

std::shared_ptr<ScreenshotAnimationSource> fastSource() {
    auto result = source();
    result->frameDurationsNs.fill(20 * ms, 12);
    return result;
}

void slowDecodingPreparesFutureCyclesWithoutPolling() {
    Harness h;
    h.playback->setSource(fastSource());
    h.playback->setSuspended(false);
    h.wake(20 * ms);
    require(!h.deadline && h.jobs.size() == 1 && !h.jobs.front().request.cancelled->load(),
            "an overdue progressing decode must wait for completion without deadline polling");
    h.now = 60 * ms;
    h.complete();
    require(h.publications == 1 && h.jobs.size() == 1 && h.deadline &&
                *h.deadline >= h.now + 77 * ms,
            "a slow expired decode increases lead and prepares a future source occurrence");

    for (int count = 0; count < 7; ++count) {
        require(h.jobs.size() == 1 && h.deadline,
                "future preparation admits one bounded decode and one presentation deadline");
        const qint64 futureDeadline = *h.deadline;
        const int futureIndex = h.jobs.front().request.frameIndex;
        h.now += 60 * ms;
        h.complete();
        require(h.publications == count + 1 && h.deadline && h.now < *h.deadline,
                "a complete future frame remains held until its media occurrence");
        h.wake(futureDeadline);
        require(h.playback->frameIndex() == futureIndex && h.publications == count + 2,
                "slow decoding still advances across cycles without publishing expired frames");
    }
    require(h.now > 2 * 240 * ms && h.playback->diagnostics().decodeRequests == 9,
            "slow sequential preparation remains bounded while progressing across source loops");
}

void futurePreparationRebasesSpeedAndSuspension() {
    Harness h;
    h.playback->setSource(fastSource());
    h.playback->setSuspended(false);
    h.wake(20 * ms);
    h.now = 60 * ms;
    h.complete();
    h.now = 120 * ms;
    h.complete();
    h.wake(*h.deadline);
    require(h.playback->frameIndex() == 7 && h.jobs.size() == 1 &&
                h.jobs.front().request.frameIndex == 11,
            "future occurrence preparation retains its absolute source position across loops");
    const qint64 oldDeadline = *h.deadline;
    h.now = 150 * ms;
    h.playback->increaseSpeed();
    require(h.deadline && *h.deadline < oldDeadline && !h.jobs.front().request.cancelled->load(),
            "speed changes rebase the future source occurrence without canceling useful work");
    const qint64 rebasedDeadline = *h.deadline;
    h.now = 200 * ms;
    h.complete();
    h.wake(rebasedDeadline);
    require(h.playback->frameIndex() == 11 && h.playback->speedPercent() == 110,
            "the rebased future occurrence publishes at the updated playback speed");
    h.now += 5 * ms;
    h.playback->setSuspended(true);
    h.complete();
    require(!h.deadline && h.jobs.empty() && h.playback->requestedPlaying(),
            "suspension cancels future preparation while preserving requested playback");
    const auto before = h.playback->diagnostics();
    h.now += 100'000 * ms;
    h.playback->setSuspended(false);
    require(h.deadline && h.jobs.size() == 1 && h.jobs.front().request.frameIndex == 4 &&
                h.playback->diagnostics().decodeRequests == before.decodeRequests + 1,
            "resume freezes hidden media time and restores one future preparation at its speed");
}

void overloadedInteractionsRetainThePaintedFrameRemainder() {
    for (int action = 0; action < 3; ++action) {
        Harness h;
        h.start();
        h.wake(100 * ms);
        h.now = 250 * ms;
        require(h.playback->frameIndex() == 0,
                "a slow source decode retains the last complete painted frame");
        if (action == 0) {
            h.playback->setSuspended(true);
            h.complete();
            h.now += 100'000 * ms;
            h.playback->setSuspended(false);
        } else if (action == 1) {
            h.playback->pause();
            h.complete();
            h.now += 100'000 * ms;
            h.playback->play();
        } else {
            h.playback->increaseSpeed();
        }
        require(h.deadline == h.now + 1 && h.jobs.size() == 1 &&
                    h.jobs.front().request.frameIndex == 1,
                "explicit interaction retains the exhausted painted duration instead of an "
                "unpainted phase");
    }
}

void lateWakeReusesOnlyTheCurrentImmutableFrame() {
    Harness coalesced(true, 2 * ms);
    coalesced.start();
    coalesced.complete();
    coalesced.wake(100 * ms);
    require(coalesced.playback->frameIndex() == 1 && coalesced.publications == 2,
            "an early shared wake presents prepared pixels that already match current media");

    Harness alias;
    alias.start();
    alias.complete();
    alias.wake(450 * ms);
    require(alias.playback->frameIndex() == 1 && alias.publications == 2,
            "late wake pixels can serve a later occurrence of the same current media frame");
    alias.now = 500 * ms;
    alias.playback->setSuspended(true);
    alias.complete();
    alias.now += 100'000 * ms;
    alias.playback->setSuspended(false);
    require(alias.deadline == alias.now + 150 * ms,
            "pausing a reused occurrence preserves the actual painted source-time fraction");

    Harness rescheduled;
    rescheduled.start();
    rescheduled.complete();
    rescheduled.now = 450 * ms;
    rescheduled.playback->setRefreshRate(75);
    require(rescheduled.jobs.empty() && rescheduled.deadline == rescheduled.now + 1,
            "rescheduling retains ready pixels matching a fresh current occurrence");
    rescheduled.wake(*rescheduled.deadline);
    require(rescheduled.playback->frameIndex() == 1,
            "a rescheduled current-index alias reaches its fresh paint acknowledgement");

    Harness obsolete;
    obsolete.start();
    obsolete.complete();
    obsolete.wake(320 * ms);
    require(obsolete.publications == 1 && obsolete.jobs.size() == 1 &&
                obsolete.jobs.front().request.frameIndex == 2,
            "expired pixels for a different media index remain unpublished");
}

void shortFrameWindowsAccountForCoordinatorLateness() {
    for (qint64 duration : {ms, ms / 4, qint64{15'259}}) {
        Harness h(true, 2 * ms);
        auto tiny = source();
        tiny->frameDurationsNs = {duration, duration};
        h.playback->setSource(tiny);
        h.playback->setSuspended(false);
        for (int count = 0; count < 24; ++count) {
            require(h.jobs.size() == 1 && h.deadline,
                    "short-delay source prepares one sample for the known late scheduler");
            const int index = h.jobs.front().request.frameIndex;
            h.complete();
            const qint64 actualWake = *h.deadline + 2 * ms;
            h.wake(actualWake);
            require(h.playback->frameIndex() == index && h.publications == count + 2,
                    "sub-coalescing frame windows continue advancing at the actual sampled time");
            require(h.playback->diagnostics().preparationLeadNs <= 2 * ms,
                    "known scheduler lateness must not inflate fast decoding into long lead times");
        }
        require(h.now >= 24 * 16'666'667 && h.playback->diagnostics().decodeRequests == 25,
                "tiny source delays preserve the refresh cap and bounded decode requests");
    }
}

void suspensionDiscardsAnUnpaintedPublication() {
    Harness h;
    h.start();
    h.complete();
    h.acknowledge = false;
    h.wake(100 * ms);
    const quint64 unpainted = h.pendingToken;
    require(h.publications == 2 && h.playback->frameIndex() == 0,
            "the due frame is prepared but the current snapshot remains the last paint");
    h.playback->setSuspended(true);
    h.playback->framePainted(unpainted);
    require(h.discarded == 1 && h.playback->frameIndex() == 0 && !h.deadline,
            "hidden and thumbnail states discard unpainted frames and reject their paint tokens");
    h.playback->setSuspended(false);
    h.wake(*h.deadline);
    h.complete();
    h.wake(*h.deadline);
    require(h.publications == 3, "resume prepares a replacement token for the current due frame");
}

void transformAndPauseDiscardUnpaintedPublications() {
    Harness h;
    h.start();
    h.complete();
    h.acknowledge = false;
    h.wake(100 * ms);
    const quint64 staleTransformToken = h.pendingToken;
    h.playback->setTransform(QTransform().rotate(90));
    h.playback->framePainted(staleTransformToken);
    require(h.discarded == 1 && h.playback->frameIndex() == 0,
            "a transform change invalidates a published old-transform frame before host painting");
    h.complete();
    require(h.pendingToken != staleTransformToken &&
                h.lastTransformed.size() == h.lastOriginal.size().transposed(),
            "the new transform publishes a replacement token for the last painted frame");
    h.playback->framePainted(h.pendingToken);
    h.complete();
    h.wake(*h.deadline);
    const quint64 stalePausedToken = h.pendingToken;
    h.playback->pause();
    h.playback->framePainted(stalePausedToken);
    require(h.discarded == 2 && h.playback->frameIndex() == 0 && !h.deadline,
            "Pause freezes the last painted frame and invalidates a waiting animation publication");
    h.playback->nextFrame();
    h.complete();
    h.playback->framePainted(h.pendingToken);
    require(h.playback->frameIndex() == 1 && !h.playback->requestedPlaying(),
            "manual stepping still publishes a new frame while playback is paused");
}

void waitUntil(const std::function<bool()>& ready) {
    if (ready())
        return;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    require(ready(), "the real decode worker must deliver its bounded completion");
}

void oneOffPreparationsUseWorkersAndCancelSafely() {
    const auto animation = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_fixtures::images().back().bytes);
    require(bool(animation), "the one-off preparation fixture must decode");
    const QImage original = animation->firstFrame;
    const QImage originalPixels = original.copy();
    const qint64 originalStorage = original.cacheKey();
    const QImage normalized = snowCanvasConvertImage(original, QImage::Format_ARGB32_Premultiplied);
    QObject context;
    int delivered = 0;
    const auto cancellation = ScreenshotAnimationPlayback::prepareImage(
        &context, original, QTransform().rotate(90),
        [&](const QImage& preparedOriginal, const QImage& transformed, const QString& error) {
            require(QThread::currentThread() == context.thread() && error.isEmpty() &&
                        samePixels(preparedOriginal, original) &&
                        samePixels(transformed,
                                   snowCanvasTransformImage(normalized, QTransform().rotate(90),
                                                            Qt::SmoothTransformation)),
                    "one-off worker preparation delivers immutable normalized pixels on the host "
                    "thread");
            ++delivered;
        });
    require(delivered == 0 && !cancellation->load(),
            "one-off transformation work never calls completion synchronously");
    waitUntil([&] { return delivered == 1; });
    require(original == originalPixels && original.cacheKey() == originalStorage,
            "one-off worker preparation preserves the supplied source storage and pixels");
    const auto cancelled = ScreenshotAnimationPlayback::prepareImage(
        &context, original, {}, [&](const QImage&, const QImage&, const QString&) { ++delivered; });
    cancelled->store(true);
    auto closing = std::make_unique<QObject>();
    const auto closed = ScreenshotAnimationPlayback::prepareImage(
        closing.get(), original, {},
        [&](const QImage&, const QImage&, const QString&) { ++delivered; });
    closing.reset();
    require(closed->load(), "destroying a one-off recipient cancels its pending worker request");
    int rejected = 0;
    const std::array rejectedTransforms{
        QTransform().scale(1'000'000, 1'000'000),
        QTransform().scale(1'000'000'000, 1'000'000'000),
        QTransform().scale(1'000'000'000'000.0, 1'000'000'000'000.0),
        QTransform().translate(1'000'000'000'000.0, 0),
        QTransform().translate(-1'000'000'000'000.0, 0),
        QTransform().translate(std::numeric_limits<int>::min(), 0),
    };
    for (const QTransform& transform : rejectedTransforms) {
        ScreenshotAnimationPlayback::prepareImage(
            &context, original, transform,
            [&](const QImage&, const QImage& transformed, const QString& error) {
                require(transformed.isNull() && !error.isEmpty(),
                        "one-off preparation rejects excessive allocation or unrepresentable Qt "
                        "integer bounds without raster work");
                ++rejected;
            });
    }
    waitUntil([&] { return rejected == static_cast<int>(rejectedTransforms.size()); });
    QCoreApplication::processEvents();
    require(delivered == 1,
            "canceled and destroyed-recipient preparations cannot invoke their callbacks");
}

void waitForPreparationWorkWithoutDispatchingEvents() {
    QElapsedTimer deadline;
    deadline.start();
    while (snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount != 0 &&
           deadline.elapsed() < 5000)
        QThread::msleep(1);
    require(snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount == 0,
            "one-off workers must finish and queue their results without dispatching GUI events");
}

int preparationTeardownChild(int argc, char** argv) {
    std::unique_ptr<QObject> recipient;
    int delivered = 0;
    int released = 0;
    {
        QCoreApplication application(argc, argv);
        recipient = std::make_unique<QObject>();
        QImage original(16, 16, QImage::Format_ARGB32_Premultiplied);
        original.fill(Qt::red);
        const auto completion = [&] {
            auto payload = std::shared_ptr<int>(new int, [&](int* value) {
                delete value;
                ++released;
            });
            return [payload = std::move(payload), &delivered](const QImage&, const QImage&,
                                                              const QString&) { ++delivered; };
        };
        for (int index = 0; index < 4; ++index)
            ScreenshotAnimationPlayback::prepareImage(recipient.get(), original,
                                                      QTransform().rotate(90), completion());
        auto closing = std::make_unique<QObject>();
        const auto cancellation =
            ScreenshotAnimationPlayback::prepareImage(closing.get(), original, {}, completion());
        waitForPreparationWorkWithoutDispatchingEvents();
        require(delivered == 0 && released == 0,
                "all preparations must retain an undispatched queued completion");
        closing.reset();
        require(cancellation->load() && delivered == 0 && released == 1,
                "recipient destruction cancels and releases its queued preparation directly");
        // The surviving receiver outlives the application deliberately. Qt must
        // discard its queued completions without invoking a QObject deleter that
        // posts another event while the post-event mutex is held.
    }
    require(delivered == 0 && released == 5,
            "application teardown releases every queued preparation without delivery or deadlock");
    recipient.reset();
    return 0;
}

void queuedPreparationsDoNotDeadlockRecipientOrApplicationTeardown() {
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--preparation-teardown")});
    require(child.waitForStarted(5000),
            "start the isolated queued-preparation teardown regression");
    if (!child.waitForFinished(10000)) {
        child.kill();
        static_cast<void>(child.waitForFinished(1000));
        require(false, "queued preparation application teardown must not deadlock");
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0)
        std::cerr << child.readAllStandardError().constData();
    require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            "queued preparation recipient and application cleanup must release their results");
}

void realCodecsPublishNormalizedImmutableFrames() {
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        QString error;
        const auto animation = snow_shot::image_codec::decodeAnimationSource(fixture.bytes, &error);
        require(animation && error.isEmpty(), "each real animated fixture must decode");
        const QImage sourcePixels = animation->firstFrame.copy();
        const qint64 sourceStorage = animation->firstFrame.cacheKey();
        const auto* sourceAddress = animation->firstFrame.constBits();
        const QByteArray sourceBytes = animation->encoded;
        Harness h(false);
        h.playback->setSource(animation, false);
        h.playback->setSuspended(false);
        waitUntil([&] { return h.publications == 1 && !h.playback->busy(); });
        const QImage retainedFirst = h.lastOriginal;
        const QImage retainedPixels = retainedFirst.copy();
        const qint64 retainedStorage = retainedFirst.cacheKey();
        require(h.lastOriginal.format() == QImage::Format_ARGB32_Premultiplied &&
                    h.lastOriginal.constBits() == h.lastTransformed.constBits() &&
                    h.lastOriginal.cacheKey() == h.lastTransformed.cacheKey(),
                "identity publications share one normalized immutable raster");
        h.playback->nextFrame();
        waitUntil([&] { return h.publications == 2 && !h.playback->busy(); });
        require(h.playback->frameIndex() == 1 && !h.playback->requestedPlaying() && !h.deadline,
                "real next-frame decoding remains paused without background deadlines");
        require(h.lastTransformed.format() == QImage::Format_ARGB32_Premultiplied &&
                    h.lastTransformed.devicePixelRatio() == 1.0 &&
                    (!h.lastTransformed.colorSpace().isValid() ||
                     h.lastTransformed.colorSpace() == QColorSpace(QColorSpace::SRgb)),
                "real publications normalize color, format and pixel ratio for rendering");
        require(h.lastOriginal.constBits() == h.lastTransformed.constBits() &&
                    h.lastOriginal.cacheKey() == h.lastTransformed.cacheKey(),
                "later native decoder frames also share identity display and export storage");
        require(retainedFirst == retainedPixels && retainedFirst.cacheKey() == retainedStorage &&
                    samePixels(retainedFirst, animation->firstFrame),
                "advancing must not mutate an exported or previously displayed frame");
        const QImage second = h.lastTransformed;
        const auto rotation = QTransform().rotate(90);
        h.playback->setTransform(rotation);
        waitUntil([&] { return h.publications == 3 && !h.playback->busy(); });
        require(h.lastTransformed ==
                    snowCanvasTransformImage(second, rotation, Qt::SmoothTransformation),
                "real worker transformations match the static rendering transform");
        require(h.lastOriginal.constBits() != h.lastTransformed.constBits() &&
                    samePixels(h.lastOriginal, second),
                "rotation retains the immutable canonical original in separate display storage");
        h.playback->previousFrame();
        waitUntil([&] { return h.publications == 4 && !h.playback->busy(); });
        require(h.playback->frameIndex() == 0 &&
                    samePixels(h.lastOriginal, animation->firstFrame) && h.errors == 0,
                "real previous-frame decoding wraps through immutable original snapshots");
        h.playback->setTransform({});
        waitUntil([&] { return h.publications == 5 && !h.playback->busy(); });
        require(h.lastOriginal.constBits() == h.lastTransformed.constBits() &&
                    h.lastOriginal.cacheKey() == h.lastTransformed.cacheKey(),
                "restoring identity reuses the cached canonical original storage");
        require(animation->firstFrame == sourcePixels &&
                    animation->firstFrame.cacheKey() == sourceStorage &&
                    animation->firstFrame.constBits() == sourceAddress &&
                    animation->encoded == sourceBytes,
                "normalization and transforms must not detach or mutate the immutable source");
    }
}

void colorProfilesPreserveOriginalPixelsAndSharing() {
    const QColorSpace srgb(QColorSpace::SRgb);
    for (const QColorSpace& profile : {srgb, QColorSpace(QColorSpace::AdobeRgb)}) {
        QString error;
        const auto animation =
            snow_shot::image_codec::decodeAnimationSource(profiledApng(profile), &error);
        require(animation && error.isEmpty() && animation->firstFrame.colorSpace() == profile,
                "the real APNG decoder must preserve the encoded source profile");
        const QImage sourcePixels = animation->firstFrame.copy();
        const qint64 sourceStorage = animation->firstFrame.cacheKey();
        Harness h(false);
        h.playback->setSource(animation, false);
        h.playback->setSuspended(false);
        waitUntil([&] { return h.publications == 1 && !h.playback->busy(); });
        require(h.lastOriginal.colorSpace() == profile &&
                    samePixels(h.lastOriginal, animation->firstFrame),
                "normalization preserves source colors and original profile metadata");
        require((h.lastOriginal.constBits() == h.lastTransformed.constBits()) == (profile == srgb),
                "identity frames share storage exactly when no color conversion is needed");
        if (profile != srgb)
            require(h.lastOriginal.format() == animation->firstFrame.format(),
                    "a non-sRGB original retains its native format and source profile");
        h.playback->nextFrame();
        waitUntil([&] { return h.publications == 2 && !h.playback->busy(); });
        auto reference = snow_shot::image_codec::openAnimationDecoder(animation, &error);
        require(reference && reference->advance(&error) && reference->advance(&error),
                "the reference decoder must produce the second profiled animation frame");
        const QImage nativeSecond = reference->snapshot(&error);
        require(error.isEmpty() && h.lastOriginal.colorSpace() == profile &&
                    samePixels(h.lastOriginal, nativeSecond),
                "worker publications preserve original pixels for later profiled frames");
        const QImage expected =
            profile == srgb
                ? snowCanvasConvertImage(nativeSecond, QImage::Format_ARGB32_Premultiplied)
                : snowCanvasColorConvertedImage(nativeSecond, srgb,
                                                QImage::Format_ARGB32_Premultiplied);
        require(h.lastTransformed.colorSpace() == srgb && samePixels(h.lastTransformed, expected),
                "display pixels follow the same sRGB conversion as static images");
        const QImage retained = h.lastOriginal;
        const QImage retainedPixels = retained.copy();
        h.playback->setTransform(QTransform().rotate(90));
        waitUntil([&] { return h.publications == 3 && !h.playback->busy(); });
        require(h.lastOriginal.constBits() != h.lastTransformed.constBits() &&
                    h.lastOriginal.colorSpace() == profile &&
                    samePixels(h.lastOriginal, nativeSecond) && retained == retainedPixels &&
                    samePixels(h.lastTransformed,
                               snowCanvasTransformImage(expected, QTransform().rotate(90),
                                                        Qt::SmoothTransformation)),
                "profiled rotation keeps source pixels immutable and transforms converted colors");
        require(animation->firstFrame == sourcePixels &&
                    animation->firstFrame.cacheKey() == sourceStorage,
                "color conversion must not mutate or detach the encoded source's first frame");
    }
}

void tinyRasterCachesAccountForMetadataAndReleaseRetiredStorage() {
    Harness observer(false);
    waitUntil([&] {
        const auto state = observer.playback->diagnostics();
        return state.cachedBytes == 0 && state.reservedWorkingBytes == 0;
    });
    const auto animation = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_fixtures::images().back().bytes);
    require(animation && animation->canvasSize == QSize(2, 2),
            "tiny-raster cache accounting uses a genuine native animation");
    Harness h(false);
    h.playback->setSource(animation, false);
    h.playback->setSuspended(false);
    waitUntil([&] { return h.publications == 1 && !h.playback->busy(); });
    h.playback->nextFrame();
    waitUntil([&] { return h.publications == 2 && !h.playback->busy(); });
    h.playback->previousFrame();
    waitUntil([&] { return h.publications == 3 && !h.playback->busy(); });
    const qint64 twoRasterBytes = h.lastTransformed.sizeInBytes() * 2;
    require(h.playback->diagnostics().cachedBytes > twoRasterBytes * 4,
            "tiny-frame cache budgets include dominant node and image metadata beyond pixels");
    h.playback->clear();
    waitUntil([&] {
        const auto state = observer.playback->diagnostics();
        return state.cachedBytes == 0 && state.reservedWorkingBytes == 0;
    });
    require(h.playback->frameIndex() == 0 && !h.playback->busy(),
            "retirement releases all optional cache node storage and mandatory reservations");
}

void sharedSourceCacheSurvivesSiblingRetirement() {
    const auto animation = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_fixtures::images().back().bytes);
    require(animation && animation->isAnimated(), "shared cache uses a real animation source");
    Harness first(false);
    Harness second(false);
    first.playback->setSource(animation, false);
    first.playback->setSuspended(false);
    waitUntil([&] { return first.publications == 1 && !first.playback->busy(); });
    first.playback->nextFrame();
    waitUntil([&] { return first.publications == 2 && !first.playback->busy(); });
    second.playback->setSource(animation, false);
    second.playback->setSuspended(false);
    waitUntil([&] { return second.publications == 1 && !second.playback->busy(); });
    second.playback->nextFrame();
    waitUntil([&] { return second.publications == 2 && !second.playback->busy(); });
    require(second.playback->diagnostics().cacheHits != 0,
            "two controllers reuse a shared source's immutable frame");
    first.playback->clear();
    waitUntil([&] { return second.playback->diagnostics().reservedWorkingBytes == 0; });
    require(second.playback->diagnostics().cachedBytes > 0,
            "retiring one controller preserves frames owned by another source worker");
    second.playback->previousFrame();
    waitUntil([&] { return second.publications == 3 && !second.playback->busy(); });
    const auto before = second.playback->diagnostics();
    second.playback->nextFrame();
    waitUntil([&] { return second.publications == 4 && !second.playback->busy(); });
    const auto after = second.playback->diagnostics();
    require(after.cacheHits == before.cacheHits + 1 && after.cacheMisses == before.cacheMisses,
            "a surviving controller reuses the retained frame without decoding it again");
    second.playback->clear();
    waitUntil([&] {
        const auto state = second.playback->diagnostics();
        return state.cachedBytes == 0 && state.reservedWorkingBytes == 0;
    });
}

void scaledNativeFramesRespectTransformedRasterAdmission() {
    const auto animation = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_native_fixtures::images().back().bytes);
    require(animation && animation->canvasSize == QSize(320, 180),
            "scaled publication admission uses a real native-sized source");
    for (const QTransform& rejected :
         {QTransform().scale(100, 100),
          QTransform().scale(1'000'000'000'000.0, 1'000'000'000'000.0)}) {
        Harness h(false);
        h.playback->setTransform(QTransform().scale(2, 2));
        h.playback->setSource(animation, false);
        h.playback->setSuspended(false);
        waitUntil([&] { return h.publications == 1 && !h.playback->busy(); });
        require(h.lastTransformed.size() == QSize(640, 360),
                "a valid restored scale prepares the first transformed frame off thread");
        h.playback->nextFrame();
        waitUntil([&] { return h.publications == 2 && !h.playback->busy(); });
        require(h.lastTransformed.size() == QSize(640, 360) && h.playback->frameIndex() == 1 &&
                    samePixels(h.lastTransformed,
                               snowCanvasTransformImage(h.lastOriginal, QTransform().scale(2, 2),
                                                        Qt::SmoothTransformation)),
                "later native frames reserve and publish their enlarged transformed output");
        const QImage preserved = h.lastTransformed;
        h.playback->setTransform(rejected);
        waitUntil([&] { return h.errors == 1 && !h.playback->busy(); });
        require(h.publications == 2 && h.lastTransformed == preserved &&
                    h.playback->frameIndex() == 1 && !h.playback->requestedPlaying() && !h.deadline,
                "unrepresentable or oversized worker transforms fail before Qt raster work and "
                "preserve the last painted frame");
        waitUntil([&] {
            const auto state = h.playback->diagnostics();
            return state.cachedBytes == 0 && state.reservedWorkingBytes == 0;
        });
    }
}

void preparedInitialIdentitySharesNormalizedStorage() {
    QString error;
    const auto animation = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_fixtures::images().back().bytes, &error);
    require(animation && error.isEmpty(), "the prepared identity fixture must decode");
    const QImage originalPixels = animation->firstFrame.copy();
    const qint64 originalStorage = animation->firstFrame.cacheKey();
    const QImage prepared =
        snowCanvasConvertImage(animation->firstFrame, QImage::Format_ARGB32_Premultiplied);
    Harness h(false);
    h.playback->setSource(animation, false, 100, prepared);
    require(h.publications == 1 && !h.playback->busy() &&
                h.lastOriginal.constBits() == prepared.constBits() &&
                h.lastTransformed.constBits() == prepared.constBits(),
            "a prepared initial identity frame shares display and export storage without a worker");
    require(animation->firstFrame == originalPixels &&
                animation->firstFrame.cacheKey() == originalStorage,
            "prepared identity publication preserves the immutable native source raster");
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && QByteArray(argv[1]) == "--preparation-teardown")
        return preparationTeardownChild(argc, argv);
    QCoreApplication application(argc, argv);
    destructionKeepsPlaybackAvailableToDeadlineCancellation();
    cropTransactionsRetainPaintedFrameAndDeferredSteps();
    cropReservationCancellationAndReplacementPreserveSource();
    cropCallbacksMayDestroyPlayback();
    preparedTransformsFreezeSourceTimeUntilPaint();
    latestPreparedTransformSurvivesHiddenAndPausedStates();
    preparationCancellationFailureAndQueuedStepsPreserveIntent();
    failedSupersedingTransformRestoresThePaintedMatrixWithoutDecodingIt();
    hostCloseDuringTransformRollbackCancelsPreparation();
    startsOnlyAfterPaintingAndVisibility();
    activeDeadlineReplacementAvoidsCancellation();
    suspensionPreservesIntentAndRemainingDuration();
    steppingWrapsAndRemainsPaused();
    steppingUsesThePaintedFrameDuringPlaybackPreparation();
    speedChangesPreserveSourceTimeAndClamp();
    finitePlaybackStopsAndPlayRestarts();
    finiteCompletionCancelsNextCyclePreparation();
    finiteCompletionRetainsForcedFinalFrameAfterCanceledPreparation();
    staleDecodeAndTransformResultsNeverPublish();
    staleSourceAndOrdinaryDeadlinesRespectDecodeOwnership();
    preparedInitialTransformAvoidsRedundantDecoding();
    lateWorkSkipsPresentationAndReportsDecodeFailure();
    refreshCapAndPaintBackpressureBoundWork();
    skippedPresentationsStillBoundRetries();
    futureAliasesRetainTheirRefreshPermittedOccurrence();
    slowDecodingPreparesFutureCyclesWithoutPolling();
    futurePreparationRebasesSpeedAndSuspension();
    overloadedInteractionsRetainThePaintedFrameRemainder();
    lateWakeReusesOnlyTheCurrentImmutableFrame();
    shortFrameWindowsAccountForCoordinatorLateness();
    suspensionDiscardsAnUnpaintedPublication();
    transformAndPauseDiscardUnpaintedPublications();
    realCodecsPublishNormalizedImmutableFrames();
    oneOffPreparationsUseWorkersAndCancelSafely();
    queuedPreparationsDoNotDeadlockRecipientOrApplicationTeardown();
    colorProfilesPreserveOriginalPixelsAndSharing();
    tinyRasterCachesAccountForMetadataAndReleaseRetiredStorage();
    sharedSourceCacheSurvivesSiblingRetirement();
    scaledNativeFramesRespectTransformedRasterAdmission();
    preparedInitialIdentitySharesNormalizedStorage();
    std::cout << "Animation playback tests passed\n";
    return 0;
}
