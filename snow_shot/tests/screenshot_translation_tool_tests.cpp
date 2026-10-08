#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotocrcontroller.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
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
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionShadowWidthFromToolbar(int) override {}
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
    void complete(qsizetype index) {
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = requests.at(index).canvasRect.toAlignedRect();
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized text");
        line.confidence = 1.0;
        const QRectF rect = requests.at(index).canvasRect;
        line.quad = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
        presentation->lines.append(line);
        presentation->prepareForRendering();
        completions.at(index)({std::move(presentation)});
    }
    QVector<ScreenshotOcrRequest> requests;
    QVector<Completion> completions;
};

void translationIdentitySurvivesSelectionResize(bool completeBeforeResize, bool cancelResize) {
    ScreenshotCaptureState capture;
    capture.presentationSuppressed = true;
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
    display.physicalRect = display.logicalRect;
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
    ScreenshotOcrController controller(
        {capture, interaction, selection, displays, geometry, coordinator, recognition});

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
    actions.activateToolForSelectionResize = [&](ScreenshotActiveTool tool) {
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
    require(handler.beginSelectionResizeAtCanvasPosition(QPointF(90, 40)),
            "translation should allow selection border resizing");
    require(!controller.active() && interaction.moveToolActive(),
            "resizing should temporarily leave the recognition tool");
    if (cancelResize) {
        handler.resetTransientShortcuts();
        interaction.cancelDrag();
        require(selection.pixelSelection() == QRect(10, 10, 80, 60),
                "canceling an unmodified resize must retain the original selection");
    } else {
        handler.updateSelectionResizeAtCanvasPosition(QPointF(110, 40));
        handler.finishSelectionResizeAtCanvasPosition(QPointF(110, 40));
    }
    QCoreApplication::processEvents();
    require(activated == QVector<ScreenshotActiveTool>{ScreenshotActiveTool::Move,
                                                       ScreenshotActiveTool::TextTranslation},
            "finishing or canceling resize must restore translation rather than OCR");
    requireTranslation();
    if (!cancelResize) {
        require(selection.pixelSelection() == QRect(10, 10, 100, 60) &&
                    recognition.requests.size() == 2 &&
                    recognition.requests.constLast().canvasRect == QRectF(10, 10, 100, 60),
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
    storage.shutdown();
    return 0;
}
