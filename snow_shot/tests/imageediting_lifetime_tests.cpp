// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "widgets/color_picker.h"
#include "widgets/modal.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using adqt::widgets::AdModal;
using snow_shot::presentation::ImageEditingDocument;
using snow_shot::presentation::ImageEditingSource;
using snow_shot::presentation::ImageEditingWindow;
using snow_shot::presentation::ImageEditingWindowCoordinator;

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

struct ImageStorage {
    std::unique_ptr<uchar[]> pixels;
    std::shared_ptr<std::atomic_int> released;
};

QImage trackedImage(const std::shared_ptr<std::atomic_int>& released) {
    constexpr int width = 16;
    constexpr int height = 12;
    auto storage = std::make_unique<ImageStorage>();
    storage->pixels = std::make_unique<uchar[]>(width * height * 4);
    storage->released = released;
    std::memset(storage->pixels.get(), 0xff, width * height * 4);
    QImage image(
        storage->pixels.get(), width, height, width * 4, QImage::Format_ARGB32,
        [](void* value) {
            auto* owned = static_cast<ImageStorage*>(value);
            owned->released->fetch_add(1);
            delete owned;
        },
        storage.get());
    storage.release();
    return image;
}

ImageEditingSource source() {
    ImageEditingSource result;
    result.image = QImage(16, 12, QImage::Format_ARGB32);
    result.image.fill(Qt::white);
    result.canvasRect = QRectF(QPointF(), QSizeF(result.image.size()));
    return result;
}

ImageEditingWindow* open(ImageEditingWindowCoordinator& coordinator, QImage image) {
    coordinator.presentSource(ScreenshotExportSource::fromImage(std::move(image)));
    const auto windows = coordinator.windows();
    require(!windows.isEmpty(), "opening creates an editor owned by its coordinator");
    const QPointer<ImageEditingWindow> window = windows.last();
    awaitCondition([&] { return window && !window->document()->image().isNull(); },
                   "editor finishes loading the lifetime fixture");
    return window;
}

struct SessionObjects {
    QPointer<ImageEditingWindow> window;
    QPointer<ImageEditingDocument> document;
    QPointer<QWidget> canvas;
    QPointer<AdModal> modal;
    QPointer<QWidget> toolbar;
    QPointer<ScreenshotAutoFilterController> autoFilter;
    QVector<QPointer<ScreenshotRecognitionSessionController>> recognition;
    QVector<QPointer<ScreenshotRecognitionWindow>> recognitionViews;

    explicit SessionObjects(ImageEditingWindow* value)
        : window(value), document(value->document()), canvas(value->canvas()),
          modal(value->modal()), toolbar(value->palette()->window()),
          autoFilter(value->findChild<ScreenshotAutoFilterController*>()) {
        for (auto* item : value->findChildren<ScreenshotRecognitionSessionController*>())
            recognition.append(item);
        for (auto* item :
             value->canvas()->parentWidget()->findChildren<ScreenshotRecognitionWindow*>())
            recognitionViews.append(item);
    }

    bool destroyed() const {
        return !window && !document && !canvas && !modal && !toolbar && !autoFilter &&
               std::all_of(recognition.cbegin(), recognition.cend(),
                           [](const auto& item) { return item.isNull(); }) &&
               std::all_of(recognitionViews.cbegin(), recognitionViews.cend(),
                           [](const auto& item) { return item.isNull(); });
    }
};

AdModal* confirmation(ImageEditingWindow* window) {
    for (auto* dialog : window->findChildren<AdModal*>())
        if (dialog != window->modal())
            return dialog;
    return nullptr;
}

class DeferredOcr final : public ScreenshotOcrRecognitionPort {
  public:
    struct Pending {
        QImage image;
        QPointer<QObject> receiver;
        Completion completion;
        bool cancelled = false;
    };
    QVector<Pending> pending;
    int cancellations = 0;
    int deliveries = 0;

    RequestToken recognize(ScreenshotOcrRequest request, QObject* receiver,
                           Completion completion) override {
        pending.append({std::move(request.image), receiver, std::move(completion)});
        return quint64(pending.size());
    }
    void cancel(RequestToken token) override {
        require(token > 0 && token <= quint64(pending.size()),
                "OCR cancellation uses a live token");
        auto& request = pending[qsizetype(token - 1)];
        if (!request.cancelled) {
            request.cancelled = true;
            request.image = {};
            ++cancellations;
        }
    }
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }
    void complete(int index, std::shared_ptr<ScreenshotOcrPresentation> result = {}) {
        auto& request = pending[index];
        request.image = {};
        auto callback = std::exchange(request.completion, {});
        if (request.receiver && callback) {
            ++deliveries;
            callback({std::move(result), {}});
        }
    }
};

void normalCloseReleasesBacking() {
    ImageEditingWindowCoordinator coordinator({});
    const auto previousWindows = QApplication::topLevelWidgets();
    const auto released = std::make_shared<std::atomic_int>(0);
    SessionObjects objects(open(coordinator, trackedImage(released)));
    require(released->load() == 0, "an open editor retains its source pixels");
    objects.window->palette()->setActiveTool(ScreenshotToolPalette::Tool::Shape);
    drainEvents();
    adqt::widgets::AdColorPicker* picker = nullptr;
    for (auto* candidate : objects.window->palette()->findChildren<adqt::widgets::AdColorPicker*>())
        if (candidate->isVisible()) {
            picker = candidate;
            break;
        }
    require(picker, "editor exposes a drawing color picker for popup lifetime checks");
    picker->setPopupVisible(true);
    require(picker->popupVisible(), "color picker materializes its detached popup");
    objects.window->palette()->canvasColorSamplingRequested(picker);
    QVector<QPointer<QWidget>> createdWindows;
    bool hasSampler = false;
    for (auto* widget : QApplication::topLevelWidgets())
        if (!previousWindows.contains(widget)) {
            createdWindows.append(widget);
            hasSampler |=
                widget->objectName() == QStringLiteral("screenshotCanvasColorSamplerWindow");
        }
    require(hasSampler, "canvas sampling creates its independent preview window");
    objects.window->requestClose();
    awaitCondition(
        [&] {
            return objects.destroyed() &&
                   std::all_of(createdWindows.cbegin(), createdWindows.cend(),
                               [](const auto& widget) { return widget.isNull(); });
        },
        "normal close destroys the complete editor, detached popup and sampling preview");
    require(coordinator.windows().isEmpty(), "normal close removes the coordinator entry");
    awaitCondition([&] { return released->load() == 1; },
                   "normal close releases the last source image backing");
}

void dirtyCloseCanCancelThenDiscard() {
    ImageEditingWindowCoordinator coordinator({});
    SessionObjects objects(open(coordinator, source().image));
    require(objects.document->crop(ImageEditingDocument::CropAxis::Rows, 2, 4),
            "lifetime close fixture contains an unsaved crop");
    objects.window->requestClose();
    QPointer<AdModal> decision = confirmation(objects.window);
    require(decision, "closing an unsaved editor creates a discard decision");
    decision->reject();
    awaitCondition([&] { return !decision; }, "cancelling discard destroys its confirmation modal");
    require(objects.window && objects.modal->isOpen() && objects.window->dirty() &&
                coordinator.windows().size() == 1,
            "cancelling close retains the live editor and its unsaved document");
    objects.window->requestClose();
    decision = confirmation(objects.window);
    require(decision, "closing again creates a fresh discard decision");
    decision->accept();
    awaitCondition([&] { return !decision && objects.destroyed(); },
                   "discarding destroys the confirmation and complete editor session");
}

void coordinatorShutdownDestroysRecognitionSessions() {
    DeferredOcr provider;
    ImageEditingWindowCoordinator coordinator(
        {[&] { return ScreenshotPinnedRecognitionProviders{&provider, nullptr, nullptr}; }});
    const auto released = std::make_shared<std::atomic_int>(0);
    auto* first = open(coordinator, trackedImage(released));
    first->palette()->ocrRequested();
    require(provider.pending.size() == 1 && first->recognition(),
            "first editor owns a pending recognition session");
    SessionObjects firstObjects(first);
    require(!firstObjects.recognition.isEmpty(),
            "pending recognition is included in lifetime checks");

    auto* second = open(coordinator, trackedImage(released));
    second->palette()->ocrRequested();
    require(provider.pending.size() == 2, "second editor starts an independent OCR request");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = second->document()->bounds().toRect();
    presentation->prepareForRendering();
    provider.complete(1, std::move(presentation));
    require(second->recognition()->hasTextResult() && !second->recognition()->editing(),
            "completed OCR opens its image overlay");
    require(second->document()->crop(ImageEditingDocument::CropAxis::Columns, 2, 4),
            "crop creates a second recognition state");
    second->palette()->ocrRequested();
    require(provider.pending.size() == 3, "cropped state owns a new pending OCR request");
    SessionObjects secondObjects(second);
    require(secondObjects.recognition.size() == 2 && !secondObjects.recognitionViews.isEmpty(),
            "lifetime fixture includes cached and active OCR sessions and recognition widgets");

    coordinator.shutdown();
    require(coordinator.windows().isEmpty(), "shutdown clears live editor tracking");
    first->requestClose();
    second->requestClose();
    require(!confirmation(first) && !confirmation(second),
            "retired editors cannot reopen discard confirmations before deferred destruction");
    awaitCondition([&] { return firstObjects.destroyed() && secondObjects.destroyed(); },
                   "coordinator shutdown destroys all sessions and owned UI resources");
    require(provider.cancellations == 2, "shutdown cancels both pending OCR requests exactly once");
    require(!provider.pending[0].receiver && !provider.pending[2].receiver,
            "destroyed OCR sessions invalidate their provider callback receivers");
    const int deliveries = provider.deliveries;
    provider.complete(0);
    provider.complete(2);
    drainEvents();
    require(provider.deliveries == deliveries && coordinator.windows().isEmpty(),
            "late provider completions cannot deliver to or recreate closed sessions");
    awaitCondition([&] { return released->load() == 2; },
                   "shutdown releases original backing retained by recognition and canvas state");

    coordinator.shutdown();
    for (int iteration = 0; iteration < 2; ++iteration) {
        SessionObjects reopened(open(coordinator, trackedImage(released)));
        coordinator.shutdown();
        coordinator.shutdown();
        awaitCondition([&] { return reopened.destroyed(); },
                       "new sessions still close after repeated coordinator shutdown");
    }
    awaitCondition([&] { return released->load() == 4; },
                   "repeated sessions release each image backing exactly once");
}

void lateImageLoadingCannotReopenSession() {
    ImageEditingWindowCoordinator coordinator({});
    std::function<void(QImage)> deliver;
    QPointer<QObject> loadReceiver;
    coordinator.presentSource(ScreenshotExportSource::fromImageLoader(
        [&](QObject* receiver, std::function<void(QImage)> completion) {
            loadReceiver = receiver;
            deliver = std::move(completion);
            return true;
        }));
    require(coordinator.windows().size() == 1, "pending source creates an editor session");
    SessionObjects objects(coordinator.windows().first());
    awaitCondition([&] { return bool(deliver); }, "pending source records its deferred delivery");
    int loads = 0;
    QObject::connect(objects.window, &ImageEditingWindow::loaded, &coordinator,
                     [&](bool) { ++loads; });
    objects.window->requestClose();
    awaitCondition([&] { return objects.destroyed() && !loadReceiver; },
                   "closing while loading destroys the editor and its loading artifact");
    const auto released = std::make_shared<std::atomic_int>(0);
    std::exchange(deliver, {})(trackedImage(released));
    drainEvents();
    require(loads == 0 && coordinator.windows().isEmpty(),
            "late image delivery cannot emit loaded or reopen a closed editor");
    awaitCondition([&] { return released->load() == 1; },
                   "late loading delivery releases its unused image backing");
}

void coordinatorDestructionCompletesPendingReview() {
    auto coordinator = std::make_unique<ImageEditingWindowCoordinator>(
        snow_shot::presentation::ImageEditingServices{});
    SessionObjects objects(open(*coordinator, source().image));
    require(objects.document->crop(ImageEditingDocument::CropAxis::Rows, 2, 4),
            "pending shutdown review contains unsaved changes");
    auto retained = std::make_shared<int>(0);
    const std::weak_ptr<int> lifetime = retained;
    int completions = 0;
    bool accepted = true;
    coordinator->confirmShutdown([retained, &completions, &accepted](bool result) {
        ++completions;
        accepted = result;
    });
    QPointer<AdModal> decision = confirmation(objects.window);
    require(decision, "shutdown review owns a pending discard decision");
    retained.reset();
    coordinator.reset();
    awaitCondition([&] { return lifetime.expired() && !decision && objects.destroyed(); },
                   "coordinator destruction releases review callbacks and all editor resources");
    require(completions == 1 && !accepted,
            "destroying the coordinator cancels its pending shutdown review exactly once");
}

void shutdownCancelsManualSaveDialog() {
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restoreDialogs = qScopeGuard([nativeDialogsDisabled] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    });
    ImageEditingWindowCoordinator coordinator({});
    const auto released = std::make_shared<std::atomic_int>(0);
    auto* window = open(coordinator, trackedImage(released));
    SessionObjects objects(window);
    int exports = 0;
    QObject::connect(window, &ImageEditingWindow::exportFinished, &coordinator,
                     [&](bool) { ++exports; });
    window->palette()->saveRequested();
    QPointer<QFileDialog> dialog = window->canvas()->window()->findChild<QFileDialog*>(
        QStringLiteral("screenshotManualSaveDialog"));
    require(dialog && dialog->isVisible(), "Save As creates a pending editor-owned file dialog");
    coordinator.shutdown();
    awaitCondition([&] { return !dialog && objects.destroyed(); },
                   "shutdown cancels and destroys the pending manual-save dialog");
    require(exports == 0,
            "cancelled save callbacks cannot publish export completion after shutdown");
    awaitCondition([&] { return released->load() == 1; },
                   "cancelled manual save releases its image snapshot");
}

void shutdownStopsAutoFilterBeforeDestruction() {
    ImageEditingWindow window;
    window.present(source());
    auto* controller = window.findChild<ScreenshotAutoFilterController*>();
    require(controller, "editor owns an auto-filter controller");
    int failures = 0;
    QObject::connect(controller, &ScreenshotAutoFilterController::detectionFailed, &window,
                     [&](const QString&) { ++failures; });
    require(window.canvas()->setCanvasTool(SnowCanvasTool::AutoFilter),
            "auto-filter lifetime fixture activates its canvas tool");
    controller->validate();
    require(controller->detecting(), "auto-filter submits pending detection before shutdown");
    const auto timers = controller->findChildren<QTimer*>();
    require(!timers.isEmpty() && timers.first()->isActive(),
            "pending detection starts the auto-filter animation timer");
    const auto checkpoint = window.document()->checkpoint();
    window.shutdown();
    require(!controller->detecting() &&
                std::none_of(timers.cbegin(), timers.cend(),
                             [](const auto* timer) { return timer->isActive(); }),
            "shutdown cancels auto-filter work and stops its timer before window destruction");
    drainEvents();
    require(failures == 0 && window.document()->checkpoint() == checkpoint,
            "retired auto-filter detection cannot publish an error or mutate the document");
}
} // namespace

void runImageEditingLifetimeTests() {
    normalCloseReleasesBacking();
    dirtyCloseCanCancelThenDiscard();
    coordinatorShutdownDestroysRecognitionSessions();
    coordinatorDestructionCompletesPendingReview();
    lateImageLoadingCannotReopenSession();
    shutdownCancelsManualSaveDialog();
    shutdownStopsAutoFilterBeforeDestruction();
}
