// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottoolpalette.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QThread>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using namespace snow_shot::presentation;
using Axis = ImageEditingDocument::CropAxis;
using Session = ScreenshotRecognitionSessionController;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void drainEvents() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

void awaitCondition(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 15000) {
        drainEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

std::shared_ptr<ScreenshotOcrPresentation> presentation(const QRectF& bounds) {
    auto result = std::make_shared<ScreenshotOcrPresentation>();
    result->selection = bounds.toRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Recognized text");
    line.confidence = 1;
    line.quad = QPolygonF{bounds.topLeft(), bounds.topLeft() + QPointF(12, 0),
                          bounds.topLeft() + QPointF(12, 6), bounds.topLeft() + QPointF(0, 6)};
    result->lines.append(line);
    result->prepareForRendering();
    return result;
}

ImageEditingSource source() {
    ImageEditingSource result;
    result.image = QImage(32, 24, QImage::Format_ARGB32_Premultiplied);
    result.image.fill(Qt::white);
    result.canvasRect = QRectF(QPointF(), QSizeF(result.image.size()));
    result.recognition.key = QStringLiteral("image-editing-branch-original");
    result.recognition.text = ScreenshotOcrRecognitionResult{presentation(result.canvasRect)};
    return result;
}

class DeferredOcr final : public ScreenshotOcrRecognitionPort {
  public:
    struct Pending {
        QPointer<QObject> receiver;
        Completion completion;
    };
    QVector<Pending> pending;

    RequestToken recognize(ScreenshotOcrRequest, QObject* receiver,
                           Completion completion) override {
        pending.append({receiver, std::move(completion)});
        return quint64(pending.size());
    }
    void cancel(RequestToken) override {}
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }
    bool modelFilesReady() const override {
        return true;
    }
    ScreenshotOcrAssetStatus assetStatus() const override {
        return {ScreenshotOcrAssetPhase::ReadyCached};
    }
    void complete(std::shared_ptr<ScreenshotOcrPresentation> result) {
        require(!pending.isEmpty(), "cropped recognition starts a provider request");
        auto& request = pending.last();
        auto completion = std::move(request.completion);
        require(request.receiver && bool(completion), "cropped recognition consumer is live");
        completion(ScreenshotOcrRecognitionResult{std::move(result)});
    }
};

void copyCurrent(ImageEditingWindow& window) {
    bool completed = false;
    bool succeeded = false;
    const auto connection =
        QObject::connect(&window, &ImageEditingWindow::exportFinished, &window, [&](bool success) {
            completed = true;
            succeeded = success;
        });
    window.palette()->copyRequested();
    awaitCondition([&] { return completed; }, "branch fixture export completes");
    QObject::disconnect(connection);
    require(succeeded, "branch fixture export succeeds");
}

void reachableSessionsPreserveDraftsAndSavedCheckpoints() {
    DeferredOcr provider;
    ImageEditingWindow window(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    window.present(source());
    window.palette()->ocrRequested();
    const QPointer<Session> original = window.recognition();
    require(original && original->hasTextResult() && !original->editing(),
            "original cached recognition opens on the image");
    copyCurrent(window);
    require(!window.dirty(), "exported original recognition starts clean");
    window.palette()->moveRequested();
    require(window.document()->crop(Axis::Rows, 4, 8), "reachable-session fixture crops rows");
    require(!original->hasTarget(), "inactive crop session releases its source raster");
    window.palette()->ocrRequested();
    provider.complete(presentation(window.document()->bounds()));
    const QPointer<Session> cropped = window.recognition();
    window.palette()->textEditRequested();
    cropped->setTextDraft(QStringLiteral("Saved cropped draft"));
    copyCurrent(window);
    window.palette()->moveRequested();
    copyCurrent(window);
    require(!window.dirty(), "exporting cropped text and image saves their checkpoints");

    require(window.document()->undo(), "crop undo returns to the original image");
    drainEvents();
    require(original && cropped, "undo retains both current and redo-reachable sessions");
    require(!cropped->hasTarget(),
            "redo-reachable recognition retains drafts without source pixels");
    window.palette()->ocrRequested();
    require(window.recognition() == original && original->hasTarget() &&
                original->textDraft() == QStringLiteral("Recognized text"),
            "undo reuses original recognition and its draft");
    window.palette()->moveRequested();
    require(window.document()->redo(), "redo restores the recognized cropped image");
    window.palette()->ocrRequested();
    require(window.recognition() == cropped &&
                cropped->textDraft() == QStringLiteral("Saved cropped draft") && !window.dirty(),
            "redo restores the same recognition session and its exported checkpoint");
    cropped->setTextDraft(QStringLiteral("Unsaved reachable draft"));
    window.palette()->moveRequested();
    require(window.document()->undo(), "unsaved cropped text can be left on the redo branch");
    drainEvents();
    require(cropped && window.dirty(), "redo-reachable drafts remain unsaved and retained");
    require(window.document()->redo(), "unsaved cropped text can be recovered with redo");
    window.palette()->ocrRequested();
    require(window.recognition() == cropped &&
                cropped->textDraft() == QStringLiteral("Unsaved reachable draft"),
            "redo recovers the exact unsaved draft");
    window.palette()->textEditRequested();
    copyCurrent(window);
    require(!window.dirty(), "exporting a recovered draft restores the clean checkpoint");
    window.shutdown();
}

void abandonedBranchesReleaseRecognition(bool cropFork) {
    DeferredOcr provider;
    ImageEditingWindow window(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    window.present(source());
    window.palette()->ocrRequested();
    const QPointer<Session> original = window.recognition();
    copyCurrent(window);
    window.palette()->moveRequested();
    require(window.document()->crop(Axis::Rows, 4, 8), "abandoned-session fixture crops rows");
    const auto abandonedState = window.document()->baseState();
    window.palette()->ocrRequested();
    auto result = presentation(window.document()->bounds());
    const std::weak_ptr<ScreenshotOcrPresentation> backing = result;
    provider.complete(std::move(result));
    QPointer<Session> abandoned = window.recognition();
    abandoned->setTextDraft(QStringLiteral("Draft on abandoned crop"));
    window.palette()->moveRequested();
    require(window.document()->undo(), "undo retains the cropped recognition as redo content");
    drainEvents();
    require(abandoned && !backing.expired(), "redo-reachable recognition retains its backing");
    if (cropFork) {
        require(window.document()->crop(Axis::Columns, 3, 7), "new crop replaces the redo branch");
    } else {
        require(!window.document()
                     ->runtime()
                     .applyAnnotationTransaction(QByteArrayLiteral(
                         R"({"version":1,"operations":[{"type":"rectangle","bounds":[2,3,8,6]}]})"))
                     .isEmpty(),
                "new annotation replaces the redo branch");
    }
    require(!window.document()->runtime().canRedo() &&
                !window.document()->retainsBaseHistoryState(abandonedState),
            "a new edit makes the previous cropped image unreachable");
    awaitCondition([&] { return !abandoned && backing.expired(); },
                   "discarded crop recognition releases its controller and cached backing");
    require(original, "discarding another crop preserves the original recognition session");
    copyCurrent(window);
    require(!window.document()->dirty() && !window.dirty(),
            "successful image export is clean after removing unreachable recognition drafts");
    if (cropFork) {
        window.palette()->ocrRequested();
        provider.complete(presentation(window.document()->bounds()));
        const QPointer<Session> current = window.recognition();
        window.palette()->textEditRequested();
        current->setTextDraft(QStringLiteral("Current crop draft"));
        copyCurrent(window);
        drainEvents();
        require(current && window.recognition() == current && !window.dirty(),
                "the current crop session remains usable and can establish a clean checkpoint");
        window.palette()->moveRequested();
        require(window.document()->undo(), "the fork can return to the original base");
    } else {
        require(window.document()->undo(), "the annotation fork can return to the original base");
    }
    window.palette()->ocrRequested();
    require(window.recognition() == original, "the original reachable session remains reusable");
    window.shutdown();
}

void replacementRetiresSessionsOnlyAfterSuccessfulLoad() {
    ImageEditingWindow window;
    window.present(source());
    window.palette()->ocrRequested();
    QPointer<Session> previous = window.recognition();
    require(previous && previous->hasTextResult() && !previous->editing(),
            "replacement fixture opens cached recognition on the image");
    window.palette()->textEditRequested();
    previous->setTextDraft(QStringLiteral("Draft before replacement"));
    const auto checkpoint = window.contentCheckpoint();
    const std::weak_ptr<ScreenshotOcrPresentation> backing =
        previous->cachedRecognitionResults().text->presentation;

    auto invalid = source();
    invalid.canvasSession = QByteArrayLiteral("invalid session");
    window.present(std::move(invalid));
    drainEvents();
    require(previous && window.recognition() == previous &&
                previous->textDraft() == QStringLiteral("Draft before replacement") &&
                window.contentCheckpoint() == checkpoint && window.dirty() && !backing.expired(),
            "rejected replacement preserves recognition ownership, backing and unsaved drafts");

    auto replacement = source();
    replacement.image.fill(Qt::green);
    replacement.recognition.text->presentation->lines[0].text =
        QStringLiteral("Replacement recognition");
    replacement.recognition.text->presentation->prepareForRendering();
    window.present(std::move(replacement));
    awaitCondition([&] { return !previous && backing.expired(); },
                   "successful replacement releases the previous state-zero session and backing");
    require(!window.dirty(), "a successfully loaded replacement starts at a clean checkpoint");
    window.palette()->ocrRequested();
    require(window.recognition() && !window.recognition()->editing() &&
                window.recognition()->textDraft() == QStringLiteral("Replacement recognition"),
            "replacement state-zero recognition uses the newly loaded document");
    copyCurrent(window);
    require(!window.dirty(), "replacement recognition can establish its own exported checkpoint");
    window.shutdown();
}
} // namespace

void runImageEditingBranchTests() {
    reachableSessionsPreserveDraftsAndSavedCheckpoints();
    abandonedBranchesReleaseRecognition(true);
    abandonedBranchesReleaseRecognition(false);
    replacementRetiresSessionsOnlyAfterSuccessfulLoad();
}
