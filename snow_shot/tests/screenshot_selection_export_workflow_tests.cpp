#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotselectionexportworkflow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotdialogowner.h"

#include <QCoreApplication>
#include <QObject>

#include <optional>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void saveDialogOwnerFollowsSelection() {
    // The display session treats overlay pointers as opaque identities; no widget is dereferenced.
    alignas(void*) int leftToken = 0;
    alignas(void*) int rightToken = 0;
    auto* leftOverlay = reinterpret_cast<ScreenshotOverlayWindow*>(&leftToken);
    auto* rightOverlay = reinterpret_cast<ScreenshotOverlayWindow*>(&rightToken);
    ScreenshotDisplaySession displays;
    CapturedDisplayModel left;
    left.active = true;
    left.canvasRect = QRect(-2560, 0, 2560, 1440);
    left.logicalRect = QRect(-1707, 0, 1707, 960);
    displays.appendDisplay(left, leftOverlay);
    CapturedDisplayModel right;
    right.active = true;
    right.canvasRect = QRect(0, 0, 1920, 1080);
    right.logicalRect = right.canvasRect;
    displays.appendDisplay(right, rightOverlay);
    ScreenshotGeometryMapper geometry;

    require(screenshotSelectionDialogOwner(displays, geometry, QRectF(-2400, 100, 800, 600),
                                           rightOverlay) == leftOverlay,
            "Save dialog must follow the selection display when keyboard focus is elsewhere");
    require(screenshotSelectionDialogOwner(displays, geometry, QRectF(200, 100, 800, 600),
                                           leftOverlay) == rightOverlay,
            "Save dialog must follow the selection in either monitor direction");
    require(screenshotSelectionDialogOwner(displays, geometry, QRectF(-200, 100, 800, 600),
                                           leftOverlay) == rightOverlay,
            "a spanning selection must use the display containing its canvas center");
    require(screenshotSelectionDialogOwner(displays, geometry, QRectF(-2400, 100, 800, 600),
                                           nullptr) == leftOverlay,
            "a selection must provide a Save dialog owner without keyboard focus");
    require(screenshotSelectionDialogOwner(displays, geometry, QRectF(), rightOverlay) ==
                rightOverlay,
            "an empty selection must preserve the keyboard owner fallback");
    displays.clear();
    require(screenshotSelectionDialogOwner(displays, geometry, QRectF(10, 10, 100, 100),
                                           rightOverlay) == rightOverlay,
            "missing displays must preserve the keyboard owner fallback");
}

class FakeComposer final : public ScreenshotSelectionImageComposerPort {
  public:
    bool requestSelectionResult(const QRect&, const ScreenshotResultStyle&, QObject*,
                                ImageCallback) override {
        return false;
    }

    bool requestSelectionClipboard(const QRect&, const ScreenshotResultStyle&, QObject*,
                                   ClipboardCallback callback) override {
        ++requestCount;
        if (!scheduleSucceeds) {
            return false;
        }
        QImage image;
        if (produceValidPayload) {
            image = QImage(QSize(8, 6), QImage::Format_ARGB32);
            image.fill(QColor(20, 40, 60, 200));
        }
        callback(ScreenshotSelectionClipboardResult{
            image, ScreenshotClipboardService::prepareImage(image)});
        return true;
    }

    std::optional<ScreenshotPinnedSelectionRequest>
    preparePinnedSelection(const QRect&, const ScreenshotResultStyle&) const override {
        return std::nullopt;
    }

    bool schedulePinnedSelection(ScreenshotPinnedSelectionRequest, QObject*,
                                 PinRequestCallback) override {
        return false;
    }

    bool scheduleSucceeds = true;
    bool produceValidPayload = true;
    int requestCount = 0;
};

class FakeDestination final : public ScreenshotSelectionExportDestinationPort {
  public:
    bool publishClipboard(QObject*, ScreenshotClipboardPayload payload,
                          ClipboardCompletion completion) override {
        ++publishCount;
        receivedValidPayload = payload.isValid();
        if (!schedulingSucceeds) {
            return false;
        }
        completion(publicationSucceeds);
        return true;
    }

    bool presentPinnedSelection(const ScreenshotPinnedSelectionRequest& request,
                                ScreenshotPinnedSelectionResultHandle result,
                                PinnedCompletion completion) override {
        Q_UNUSED(result);
        ++presentCount;
        if (!presentationSucceeds) {
            return false;
        }
        QImage image(request.initialWindowSize, QImage::Format_ARGB32);
        image.fill(Qt::white);
        completion(true, std::move(image));
        return true;
    }

    bool publicationSucceeds = true;
    bool schedulingSucceeds = true;
    bool receivedValidPayload = false;
    int publishCount = 0;
    int presentCount = 0;
    bool presentationSucceeds = true;
};

class FakeStore final : public ScreenshotSelectionParamsStorePort {
  public:
    void setPreviousSelectionParams(const ScreenshotSelectionParams& params) override {
        ++writeCount;
        lastParams = params;
    }

    int writeCount = 0;
    std::optional<ScreenshotSelectionParams> lastParams;
};

struct Fixture {
    Fixture()
        : workflow(ScreenshotSelectionExportWorkflowContext{captureState,
                                                            geometry,
                                                            selection,
                                                            composer,
                                                            destination,
                                                            store,
                                                            callbackContext,
                                                            {}}) {
        CapturedDisplayModel display;
        display.physicalRect = QRect(0, 0, 100, 80);
        display.canvasRect = display.physicalRect;
        display.imageSourceCanvasRect = display.physicalRect;
        display.logicalRect = display.physicalRect;
        display.active = true;
        displays.appendDisplay(std::move(display));
        geometry.rebuild(displays);
        selection.setSelectionRect(QRectF(10, 12, 30, 24));
    }

    ScreenshotCaptureState captureState;
    ScreenshotDisplaySession displays;
    ScreenshotGeometryMapper geometry;
    ScreenshotSelectionModel selection;
    FakeComposer composer;
    FakeDestination destination;
    FakeStore store;
    QObject callbackContext;
    ScreenshotSelectionExportWorkflow workflow;
};

void publicationSuccessPersistsAndCompletes() {
    Fixture fixture;
    std::optional<bool> completion;
    QImage completedImage;
    require(fixture.workflow.copySelectionToClipboard(
                []() { return true; },
                [&completion, &completedImage](bool success, QImage image) {
                    completion = success;
                    completedImage = std::move(image);
                }),
            "clipboard request was not scheduled");
    require(fixture.composer.requestCount == 1 && fixture.destination.publishCount == 1,
            "successful copy did not execute exactly once");
    require(fixture.destination.receivedValidPayload,
            "destination did not receive a valid prepared payload");
    require(completion == true && completedImage.size() == QSize(8, 6),
            "successful publication did not return the rendered image");
    require(fixture.store.writeCount == 1 && fixture.store.lastParams.has_value(),
            "selection settings were not persisted after publication");
}

void publicationFailureDoesNotPersist() {
    Fixture fixture;
    fixture.destination.publicationSucceeds = false;
    std::optional<bool> completion;
    require(fixture.workflow.copySelectionToClipboard(
                []() { return true; },
                [&completion](bool success, QImage image) {
                    completion = success;
                    require(image.isNull(), "failed publication returned a result image");
                }),
            "failing clipboard request was not scheduled");
    require(fixture.destination.publishCount == 1, "publication failure bypassed destination");
    require(completion == false, "publication failure was not propagated");
    require(fixture.store.writeCount == 0,
            "selection settings were persisted after publication failure");
}

void invalidPayloadDoesNotPublish() {
    Fixture fixture;
    fixture.composer.produceValidPayload = false;
    std::optional<bool> completion;
    require(fixture.workflow.copySelectionToClipboard(
                []() { return true; },
                [&completion](bool success, QImage image) {
                    completion = success;
                    require(image.isNull(), "invalid payload returned a result image");
                }),
            "invalid-payload request was not scheduled");
    require(fixture.destination.publishCount == 0,
            "invalid payload reached the clipboard destination");
    require(completion == false && fixture.store.writeCount == 0,
            "invalid payload was treated as a successful copy");
}

void staleResultHasNoSideEffects() {
    Fixture fixture;
    bool completionCalled = false;
    require(fixture.workflow.copySelectionToClipboard([]() { return false; },
                                                      [&completionCalled](bool, QImage image) {
                                                          completionCalled = true;
                                                          require(image.isNull(),
                                                                  "stale result returned an image");
                                                      }),
            "stale-result request was not scheduled");
    require(fixture.destination.publishCount == 0 && fixture.store.writeCount == 0 &&
                completionCalled,
            "stale clipboard result did not complete with a failure");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        saveDialogOwnerFollowsSelection();
        publicationSuccessPersistsAndCompletes();
        publicationFailureDoesNotPersist();
        invalidPayloadDoesNotPublish();
        staleResultHasNoSideEffects();
    } catch (const std::exception& error) {
        qCritical("%s", error.what());
        return 1;
    }
    return 0;
}
