#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotocrcontroller.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::_Exit(EXIT_FAILURE);
    }
}

class OverlayEvents final : public ScreenshotOverlayEventSink {
  public:
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return false;
    }
    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF&) override {}
    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF&) override {}
    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF&) override {}
    ScreenshotOverlayRightClickResult handleOverlayRightClick(ScreenshotOverlayWindow*,
                                                              const QPointF&) override {
        return ScreenshotOverlayRightClickResult::Ignored;
    }
    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QWheelEvent&) override {
        return false;
    }
    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }
    void raiseToolbarForCanvasInteraction() override {}
};

class ToolbarCommands final : public ScreenshotToolbarCommandSink,
                              public ScreenshotSelectionToolbarCommandSink {
  public:
    void setMoveTool() override {}
    void setSelectTool() override {}
    void setShapeTool() override {}
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&, quint32) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}
    void toggleSelectionAspectRatioLockFromToolbar() override {}
    void setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset) override {}
    void openSelectionResizeModalFromToolbar() override {}
    void adjustSelectionFromToolbar(int, int, int, int) override {}
    void adjustSelectionEffectsFromWheel(int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionEffectWidthFromToolbar(int) override {}
    void setSelectionToolbarHovered(bool) override {}
};

class Recognition final : public ScreenshotOcrRecognitionPort {
  public:
    RequestToken recognize(ScreenshotOcrRequest request, QObject*, Completion completion) override {
        requests.push_back(std::move(request));
        completions.push_back(std::move(completion));
        return static_cast<RequestToken>(requests.size());
    }
    void cancel(RequestToken) override {}
    bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
        return true;
    }
    void complete(qsizetype index, bool failed = false, bool empty = false) {
        if (failed) {
            completions.at(index)({{}, QStringLiteral("Recognition failed")});
            return;
        }
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = requests.at(index).canvasRect.toAlignedRect();
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized text");
        line.confidence = 1.0;
        const QRectF rect = requests.at(index).canvasRect;
        line.quad = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
        if (!empty)
            presentation->lines.append(line);
        presentation->prepareForRendering();
        completions.at(index)({std::move(presentation)});
    }
    QVector<ScreenshotOcrRequest> requests;
    QVector<Completion> completions;
};

void copyAfterRecognitionOwnsCompletion() {
    enum class Scenario { Fresh, Cached, Cancelled, Failed, Empty, Superseded, QueuedCancellation };
    for (const QString& followUp : {QStringLiteral("no_action"), QStringLiteral("enable_edit_mode"),
                                    QStringLiteral("copy_text")}) {
        require(
            snow_shot::storage::ScreenshotSettings().setAutoExecuteAfterTextRecognition(followUp),
            "configure the general recognition follow-up");
        for (const auto scenario :
             {Scenario::Fresh, Scenario::Cached, Scenario::Cancelled, Scenario::Failed,
              Scenario::Empty, Scenario::Superseded, Scenario::QueuedCancellation}) {
            ScreenshotCaptureState capture;
            capture.presentationSuppressed = true;
            ScreenshotInteractionState interaction;
            ScreenshotSelectionModel selection;
            selection.setSelectionRect(QRectF(10, 10, 80, 60));
            ScreenshotDisplaySession displays;
            CapturedDisplayModel display;
            display.stableId = QStringLiteral("copy-recognition");
            display.logicalRect = QRect(0, 0, 200, 160);
            display.physicalRect = display.logicalRect;
            display.active = true;
            display.geometryResolved = true;
            display.image = QImage(display.physicalRect.size(), QImage::Format_RGB32);
            display.image.fill(Qt::white);
            displays.appendDisplay(display);
            ScreenshotGeometryMapper geometry;
            geometry.rebuild(displays);
            OverlayEvents events;
            SnowCanvasRuntime runtime;
            snow_shot::presentation::WindowShortcutManager shortcuts;
            ScreenshotOverlayCoordinator coordinator(events, runtime, shortcuts);
            Recognition recognition;
            int ended = 0;
            int automaticActions = 0;
            ScreenshotOcrController* activeController = nullptr;
            ScreenshotOcrController controller({capture, interaction, selection, displays, geometry,
                                                coordinator, recognition, nullptr, nullptr, [] {},
                                                [&] {
                                                    ++ended;
                                                    activeController->invalidateSession();
                                                }});
            activeController = &controller;
            QObject::connect(&controller, &ScreenshotOcrController::textResultChanged, &controller,
                             [&](bool available) {
                                 if (available)
                                     ++automaticActions;
                             });
            if (scenario == Scenario::Cached) {
                controller.activate();
                recognition.complete(0);
                automaticActions = 0;
            }
            QApplication::clipboard()->setText(QStringLiteral("Previous clipboard"));
            controller.activateAndCopy();
            require(recognition.requests.size() == 1,
                    "copy recognition must start OCR or reuse the existing result");
            if (scenario != Scenario::Cached) {
                require(ended == 0 && QApplication::clipboard()->text() ==
                                          QStringLiteral("Previous clipboard"),
                        "copy recognition must wait for text before copying or ending capture");
                if (scenario == Scenario::Cancelled)
                    controller.invalidateSession();
                else if (scenario == Scenario::Superseded)
                    controller.activate();
                recognition.complete(0, scenario == Scenario::Failed, scenario == Scenario::Empty);
                if (scenario == Scenario::QueuedCancellation)
                    controller.invalidateSession();
            }
            QCoreApplication::processEvents();
            const bool copied = scenario == Scenario::Fresh || scenario == Scenario::Cached ||
                                scenario == Scenario::Empty;
            require(ended == (copied ? 1 : 0) &&
                        QApplication::clipboard()->text() ==
                            (scenario == Scenario::Empty ? QString()
                             : copied                    ? QStringLiteral("Recognized text")
                                                         : QStringLiteral("Previous clipboard")),
                    "only successful active copy recognition may replace the clipboard and end "
                    "capture");
            require(scenario == Scenario::Superseded ? automaticActions > 0 : automaticActions == 0,
                    "explicit copy recognition must supersede the configurable follow-up");
            controller.invalidateSession();
            QApplication::clipboard()->setText(QStringLiteral("Next capture"));
            controller.activate();
            recognition.complete(recognition.requests.size() - 1);
            require(ended == (copied ? 1 : 0) &&
                        QApplication::clipboard()->text() == QStringLiteral("Next capture"),
                    "copy recognition must not leak into later screenshot activations");
            if (scenario == Scenario::Cached && followUp == QStringLiteral("no_action")) {
                auto* session = controller.findChild<ScreenshotRecognitionSessionController*>();
                require(session, "screenshot controller owns a recognition session");
                auto results = controller.recognitionResultsSnapshot();
                results.key += QStringLiteral("-image-copy-failure");
                results.translatedText =
                    std::make_shared<ScreenshotOcrPresentation>(*results.text->presentation);
                results.translatedText->lines[0].text = QStringLiteral("Translated result");
                QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::white);
                session->setTarget({results.key, image, QRectF(10, 10, 80, 60)});
                session->seedRecognitionResults(std::move(results));
                session->activate(ScreenshotRecognitionSessionController::Mode::Text);
                require(session->activateCachedTextTranslation(), "restore screenshot translation");
                const snow_shot::storage::ScreenshotTranslationSettings settings;
                require(settings.setCopyTranslationResultsAsImage(true), "enable image copying");
                const int previousEnded = ended;
                require(!controller.copyRecognitionToClipboard(true) && ended == previousEnded &&
                            QApplication::clipboard()->text() == QStringLiteral("Next capture"),
                        "failed translation image generation retains the clipboard and capture");
                require(settings.setCopyTranslationResultsAsImage(false), "restore copy default");
            }
        }
    }
}

void translationIdentitySurvivesSelectionResize(bool completeBeforeResize, bool cancelResize,
                                                bool moveWithSpace = false, qreal scale = 1.0,
                                                bool topLeft = false) {
    ScreenshotCaptureState capture;
    capture.presentationSuppressed = !moveWithSpace;
    ScreenshotInteractionState interaction;
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 10, 80, 60));
    ScreenshotIntelligentSelectionModel intelligent;
    OverlayEvents events;
    SnowCanvasRuntime runtime;
    snow_shot::presentation::WindowShortcutManager shortcuts;
    ScreenshotOverlayWindow overlay(events, new SnowCanvasWidget);
    overlay.setGeometry(0, 0, 200, 160);
    CapturedDisplayModel display;
    display.stableId = QStringLiteral("translation-resize");
    display.logicalRect = overlay.geometry();
    display.physicalRect = QRect(0, 0, qRound(200 * scale), qRound(160 * scale));
    display.logicalToPhysicalScale = scale;
    display.active = true;
    display.geometryResolved = true;
    display.image = QImage(display.physicalRect.size(), QImage::Format_RGB32);
    display.image.fill(Qt::white);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display, &overlay);
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    ScreenshotOverlayCoordinator coordinator(events, runtime, shortcuts);
    ToolbarCommands commands;
    coordinator.setToolbarCommandSinks(commands, commands);
    coordinator.attachToolbarToOverlay(&overlay);
    Recognition recognition;
    ScreenshotOverlayInputHandler* input = nullptr;
    ScreenshotOcrController controller(
        {capture, interaction, selection, displays, geometry, coordinator, recognition, nullptr,
         nullptr, [] {}, [] {},
         [&](const QPointF& point) {
             return input->selectionResizeDragModeAtCanvasPosition(point);
         },
         [&](const QPointF& point) { return input->beginSelectionResizeAtCanvasPosition(point); },
         [&](const QPointF& point) { input->updateSelectionResizeAtCanvasPosition(point); },
         [&](const QPointF& point) { input->finishSelectionResizeAtCanvasPosition(point); }});

    controller.activateTextTranslation();
    const auto requireTranslation = [&] {
        require(controller.active() && controller.mode() == ScreenshotOcrController::Mode::Text &&
                    interaction.activeTool() == ScreenshotActiveTool::TextTranslation,
                "translation activation must retain its own interaction tool while using OCR");
        require(coordinator.toolbar()->palette()->activeTool() ==
                    ScreenshotToolPalette::Tool::TextTranslation,
                "recognition state updates must retain the translation toolbar tool");
        require(!interaction.selectionHandlesVisible(),
                "translation must keep recognition selection handles hidden");
    };
    requireTranslation();
    require(recognition.requests.size() == 1, "translation should start shared OCR recognition");
    if (completeBeforeResize) {
        recognition.complete(0);
        requireTranslation();
    }

    QVector<ScreenshotActiveTool> activated;
    ScreenshotOverlayInputActions actions;
    actions.activateTool = [&](ScreenshotActiveTool tool, ScreenshotToolActivationReason reason) {
        require(reason == ScreenshotToolActivationReason::SelectionResize,
                "translation resize must preserve its recognition event receiver");
        activated.append(tool);
        if (tool == ScreenshotActiveTool::Move) {
            controller.deactivateForSelectionResize();
            interaction.setMoveTool(true, false);
        } else {
            QTimer::singleShot(0, &controller, [&, tool] {
                if (!interaction.moveToolActive() || interaction.dragging() ||
                    !selection.hasPixelSelection())
                    return;
                if (tool == ScreenshotActiveTool::TextTranslation)
                    controller.activateTextTranslation();
                else
                    controller.activate();
            });
        }
        return true;
    };
    ScreenshotOverlayInputHandler handler(
        {capture, interaction, selection, intelligent, geometry, displays, std::move(actions)});
    input = &handler;
    if (moveWithSpace) {
        ScreenshotRecognitionWindow* window = nullptr;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* recognitionWindow = qobject_cast<ScreenshotRecognitionWindow*>(widget))
                window = recognitionWindow;
        }
        require(window != nullptr, "translation must present its recognition window");
        const auto globalPoint = [&](const QPointF& point) {
            return geometry.logicalPositionForCanvasPoint(
                *geometry.displayForOverlay(displays, &overlay), point);
        };
        const auto localPoint = [&](const QPointF& point) {
            return window->mapFromGlobal(globalPoint(point));
        };
        const auto send = [&](QEvent::Type type, const QPointF& local, const QPointF& global) {
            const bool move = type == QEvent::MouseMove;
            QMouseEvent event(type, local, global, move ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(window, &event);
        };
        const QPointF pressPoint = topLeft ? QPointF(10, 10) : QPointF(90, 40);
        const QPointF resizePoint = topLeft ? QPointF(5, 5) : QPointF(110, 40);
        const QRect resized = topLeft ? QRect(5, 5, 85, 65) : QRect(10, 10, 100, 60);
        send(QEvent::MouseButtonPress, localPoint(pressPoint), globalPoint(pressPoint));
        const QPointF resizeLocal = localPoint(resizePoint);
        send(QEvent::MouseMove, resizeLocal, globalPoint(resizePoint));
        require(selection.pixelSelection() == resized,
                "translation border drag must resize before Space is held");
        send(QEvent::MouseMove, resizeLocal, globalPoint(resizePoint));
        require(selection.pixelSelection() == resized,
                "border resizing alone must not feed surface geometry back into pointer input");
        require(handler.activateMoveEntireSelectionShortcut(),
                "Space must temporarily move the resized translation selection");
        // Queued mouse samples can retain local coordinates from before the window moves.
        const QPointF movePoint = resizePoint + QPointF(10, 10);
        const QPointF nextGlobal = globalPoint(movePoint);
        const QRect moved = resized.translated(10, 10);
        const QPointF queuedLocal = window->mapFromGlobal(nextGlobal);
        send(QEvent::MouseMove, queuedLocal, nextGlobal);
        require(selection.pixelSelection() == moved,
                "Space movement must follow the screen pointer delta");
        for (int sample = 0; sample < 4; ++sample) {
            send(QEvent::MouseMove, queuedLocal, nextGlobal);
            require(selection.pixelSelection() == moved,
                    "a stationary screen pointer must not feed window movement back into the "
                    "translation selection");
        }
        require(handler.releaseMoveEntireSelectionShortcut(), "Space release resumes resizing");
        const QPointF resumePoint = movePoint + QPointF(10, 5);
        send(QEvent::MouseMove, localPoint(resumePoint), globalPoint(resumePoint));
        require(selection.pixelSelection() ==
                    (topLeft ? QRect(25, 20, 75, 60) : QRect(20, 20, 110, 60)),
                "resizing must resume from the translated selection without jumping");
        const QPointF releasePoint = resumePoint + QPointF(5, 5);
        const QPointF releaseLocal = localPoint(releasePoint);
        send(QEvent::MouseMove, releaseLocal, globalPoint(releasePoint));
        send(QEvent::MouseButtonRelease, releaseLocal, globalPoint(releasePoint));
    } else {
        require(handler.beginSelectionResizeAtCanvasPosition(QPointF(90, 40)),
                "translation should allow selection border resizing");
    }
    require(!controller.active() && interaction.moveToolActive(),
            "resizing should temporarily leave the recognition tool");
    if (cancelResize) {
        handler.resetTransientShortcuts();
        interaction.cancelDrag();
        require(selection.pixelSelection() == QRect(10, 10, 80, 60),
                "canceling an unmodified resize must retain the original selection");
    } else if (!moveWithSpace) {
        handler.updateSelectionResizeAtCanvasPosition(QPointF(110, 40));
        handler.finishSelectionResizeAtCanvasPosition(QPointF(110, 40));
    }
    QCoreApplication::processEvents();
    require(activated == QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move,
                                                       ScreenshotActiveTool::TextTranslation},
            "finishing or canceling resize must restore translation rather than OCR");
    requireTranslation();
    if (!cancelResize) {
        const QRect expected = moveWithSpace
                                   ? (topLeft ? QRect(30, 25, 70, 55) : QRect(20, 20, 115, 60))
                                   : QRect(10, 10, 100, 60);
        require(selection.pixelSelection() == expected && recognition.requests.size() == 2 &&
                    recognition.requests.constLast().canvasRect == QRectF(expected),
                "restored translation must recognize the modified selection");
        if (!completeBeforeResize) {
            recognition.complete(0);
            require(!controller.hasTextResult(),
                    "an old selection result must not replace the resized translation target");
        }
        recognition.complete(1);
        requireTranslation();
    }

    controller.activate();
    require(interaction.activeTool() == ScreenshotActiveTool::Ocr &&
                coordinator.toolbar()->palette()->activeTool() == ScreenshotToolPalette::Tool::Ocr,
            "explicit text recognition must still select OCR after translation");
    if (controller.hasTextResult()) {
        controller.beginTextTranslation();
        require(controller.translating(),
                "translation should activate from an existing OCR result");
        requireTranslation();
    }
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "translation tool tests require isolated settings");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 0}).success,
            "initialize isolated translation tool settings");
    require(
        snow_shot::storage::ScreenshotTranslationSettings().setConfiguration(
            {QStringLiteral("en"), QStringLiteral("zh-Hans"),
             QStringLiteral("missing-translation-tool-test-model"), QStringLiteral("original")}),
        "translation state coverage must not use an external model");
    for (const bool completeBeforeResize : {false, true}) {
        for (const bool cancelResize : {false, true}) {
            translationIdentitySurvivesSelectionResize(completeBeforeResize, cancelResize);
        }
    }
    for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        for (const bool topLeft : {false, true})
            translationIdentitySurvivesSelectionResize(true, false, true, scale, topLeft);
    }
    copyAfterRecognitionOwnsCompletion();
    storage.shutdown();
    return 0;
}
