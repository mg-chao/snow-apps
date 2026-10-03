#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"
#include "snow_shot/presentation/screenshotoverlayinputhandler.h"
#include "snow_shot/presentation/screenshotoverlayshortcutcontroller.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshottoolbarpresentationstatefactory.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include "widgets/radio_button_group.h"
#include "widgets/modal.h"
#include <QAbstractButton>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class RecordingToolbarCommands : public ScreenshotToolbarCommandSink {
  public:
    void setMoveTool() override {
        ++moveToolCount;
    }
    void setSelectTool() override {
        ++selectToolCount;
    }
    void setShapeTool() override {
        ++shapeToolCount;
    }
    void setArrowTool() override {}
    void setLineTool() override {}
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {
        ++elementEraserCount;
    }
    void setRectangleEraserTool() override {
        ++rectangleEraserCount;
    }
    void setBrushEraserTool() override {
        ++brushEraserCount;
    }
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

    void setScrollingScreenshotAutoScrollIntervalMs(int milliseconds) override {
        interval = milliseconds;
        ++intervalChanges;
    }
    int interval = 0;
    int intervalChanges = 0;
    int moveToolCount = 0;
    int selectToolCount = 0;
    int shapeToolCount = 0;
    int elementEraserCount = 0;
    int rectangleEraserCount = 0;
    int brushEraserCount = 0;
};

void eraserSubtoolsReachToolbarCommands() {
    using Tool = ScreenshotToolPalette::Tool;
    const storage::ScreenshotToolbarSettings settings;
    const QString original = settings.lastEraserTool();
    const auto cleanup =
        qScopeGuard([&] { static_cast<void>(settings.setLastEraserTool(original)); });
    require(settings.setLastEraserTool(QStringLiteral("eraser")), "seed element eraser command");
    RecordingToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    auto* palette = window.palette();
    require(window.activateDrawingShortcut(QStringLiteral("eraser")) &&
                commands.elementEraserCount == 1 && palette->activeTool() == Tool::Eraser,
            "generic eraser shortcut reaches the existing command");
    const auto switchMode = [&](Tool tool) {
        auto* selector =
            palette->findChild<QWidget*>(QStringLiteral("screenshotEraserModeSelector"));
        auto* group = selector != nullptr
                          ? selector->findChild<adqt::widgets::AdRadioButtonGroup*>()
                          : nullptr;
        require(group && group->button(static_cast<int>(tool)),
                "eraser selector exposes requested command");
        group->button(static_cast<int>(tool))->click();
    };
    switchMode(Tool::RectangleEraser);
    require(commands.rectangleEraserCount == 1 && palette->activeTool() == Tool::RectangleEraser,
            "rectangle selector reaches its distinct command exactly once");
    switchMode(Tool::BrushEraser);
    require(commands.brushEraserCount == 1 && palette->activeTool() == Tool::BrushEraser,
            "brush selector reaches its distinct command exactly once");
    window.resetForNewCapture();
    require(window.activateDrawingShortcut(QStringLiteral("eraser")) &&
                commands.brushEraserCount == 2 && palette->activeTool() == Tool::BrushEraser,
            "a new capture restores the remembered brush command");
}

void scrollingIntervalRestoresAndReachesCommands() {
    const storage::ScreenshotSettings settings;
    require(settings.setScrollingAutoScrollIntervalMs(350), "save initial interval");
    RecordingToolbarCommands commands;
    {
        ScreenshotToolbarWindow window(commands);
        auto* palette = window.palette();
        require(palette != nullptr && palette->scrollingAutoScrollIntervalMs() == 350,
                "toolbar must restore the saved interval before scrolling controls materialize");
        palette->setScrollingAutoScrollIntervalMs(470);
        emit palette->scrollingAutoScrollIntervalMsChanged(470);
        require(commands.interval == 470 && commands.intervalChanges == 1 &&
                    settings.scrollingAutoScrollIntervalMs() == 470,
                "interval changes must persist and reach the capture command exactly once");
        window.resetForNewCapture();
        require(palette->scrollingAutoScrollIntervalMs() == 470,
                "new captures must retain the selected interval");
        QWidget owner;
        window.setTransientOwnerWindow(&owner);
        palette->setScrollingScreenshotMode(true);
        auto* settingsButton = palette->findChild<QAbstractButton*>(
            QStringLiteral("screenshotScrollingSettingsButton"));
        require(settingsButton != nullptr, "scrolling toolbar must expose settings");
        settingsButton->click();
        auto* modal = palette->findChild<adqt::widgets::AdModal*>(
            QStringLiteral("screenshotScrollingSettingsModal"));
        require(modal && modal->ownerWindow() == &owner,
                "scrolling settings must use the toolbar's overlay owner for popup alignment");
        window.setTransientOwnerWindow(nullptr);
        require(!modal->isOpen(), "detaching the overlay must dismiss scrolling settings");
    }
    ScreenshotToolbarWindow restored(commands);
    require(restored.palette()->scrollingAutoScrollIntervalMs() == 470,
            "new toolbar windows must restore the persisted interval");
}

void rememberedDrawingToolRestoresOncePerCapture() {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings toolbarSettings;
    const snow_shot::storage::DrawingSettings drawingSettings;
    const QString originalDrawingTool = toolbarSettings.lastDrawingTool();
    const bool originalRememberSwitch = drawingSettings.rememberLastUsedTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(toolbarSettings.setLastDrawingTool(originalDrawingTool));
        static_cast<void>(drawingSettings.setRememberLastUsedTool(originalRememberSwitch));
    });
    require(drawingSettings.setRememberLastUsedTool(true) &&
                toolbarSettings.setLastDrawingTool(QStringLiteral("shape")),
            "remembered tool window tests must start from a remembered shape tool");

    RecordingToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.resetForNewCapture();
    require(window.palette() != nullptr && window.palette()->activeTool() == Tool::Move,
            "a new capture must reset the toolbar to the move tool");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 1 && commands.moveToolCount == 0 &&
                window.palette()->activeTool() == Tool::Shape,
            "the first restore of a capture must activate the remembered drawing tool");
    require(window.palette()->activateRememberedDrawingTool() && commands.shapeToolCount == 1 &&
                window.palette()->activeTool() == Tool::Shape,
            "restoring an already-active remembered tool must not emit another tool command");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 1,
            "repeated restore requests must not re-activate the remembered tool");
    window.resetForNewCapture();
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 2 && window.palette()->activeTool() == Tool::Shape,
            "the next capture must restore the remembered drawing tool again");

    // An explicit tool set supersedes the remembered restore for that capture.
    window.resetForNewCapture();
    window.setActiveTool(Tool::Select);
    require(commands.selectToolCount == 0 && commands.shapeToolCount == 2,
            "an explicit tool set must not emit tool commands by itself");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 2 && window.palette()->activeTool() == Tool::Select,
            "an explicit tool set must cancel the pending remembered restore");

    // Requesting the remembered tool explicitly must select it, not toggle it off.
    window.resetForNewCapture();
    window.suppressRememberedDrawingTool();
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 2 && window.palette()->activeTool() == Tool::Move,
            "explicit command preparation must not restore the remembered tool");
    require(window.palette()->canActivateDrawingShortcut(QStringLiteral("shape")) &&
                window.activateDrawingShortcut(QStringLiteral("shape")) &&
                commands.shapeToolCount == 3 && window.palette()->activeTool() == Tool::Shape,
            "requesting the remembered tool explicitly must activate it exactly once");
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 3 && window.palette()->activeTool() == Tool::Shape,
            "later presentation must not restore over the explicit tool");

    // The switch disables the restore entirely.
    require(drawingSettings.setRememberLastUsedTool(false), "the switch must be writable");
    window.resetForNewCapture();
    window.restoreRememberedDrawingTool();
    require(commands.shapeToolCount == 3 && window.palette()->activeTool() == Tool::Move,
            "a disabled switch must keep the move tool after a capture reset");
}
// Exercise production keyboard routing and toolbar signal connections together.
// Only the command sink substitutes for external recognition/export services.
void selectionShortcutsReachToolbarCommands() {
    using Tool = ScreenshotToolPalette::Tool;
    const storage::ScreenshotShortcutSettings screenshotSettings;
    const storage::DrawingShortcutSettings drawingSettings;
    const storage::DrawingSettings drawingPreferences;
    const storage::ScreenshotToolbarSettings toolbarSettings;
    const auto originalScreenshot = screenshotSettings.allShortcuts();
    const auto originalDrawing = drawingSettings.allShortcuts();
    const bool originalRemember = drawingPreferences.rememberLastUsedTool();
    const QString originalTool = toolbarSettings.lastDrawingTool();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(screenshotSettings.setAllShortcutsAtomic(originalScreenshot));
        static_cast<void>(drawingSettings.setAllShortcutsAtomic(originalDrawing));
        static_cast<void>(drawingPreferences.setRememberLastUsedTool(originalRemember));
        static_cast<void>(toolbarSettings.setLastDrawingTool(originalTool));
    });
    require(drawingPreferences.setRememberLastUsedTool(true), "enable remembered tool fixture");

    for (const QString& id :
         {QStringLiteral("shape"), QStringLiteral("text_recognition"),
          QStringLiteral("table_recognition"), QStringLiteral("qr_code_recognition"),
          QStringLiteral("copy_to_clipboard"), QStringLiteral("video_recording")}) {
        ScreenshotCaptureState capture;
        capture.sessionState = ScreenshotSessionState::OverlayVisible;
        ScreenshotDisplaySession displays;
        ScreenshotGeometryMapper geometry;
        ScreenshotSelectionModel selection;
        ScreenshotIntelligentSelectionModel intelligent;
        ScreenshotInteractionState interaction;
        interaction.enterOverlayVisible(true);
        selection.setSelectionRect(QRectF(10, 20, 80, 60));
        const bool drawing = id == QStringLiteral("shape");
        const bool completion =
            id == QStringLiteral("copy_to_clipboard") || id == QStringLiteral("video_recording");
        const bool recognition = !drawing && !completion;
        struct Commands final : RecordingToolbarCommands {
            ScreenshotInteractionState& interaction;
            ScreenshotSelectionModel& selection;
            ScreenshotCaptureState& capture;
            int calls = 0;
            Commands(ScreenshotInteractionState& i, ScreenshotSelectionModel& s,
                     ScreenshotCaptureState& c)
                : interaction(i), selection(s), capture(c) {}
            void checkCommitted() {
                require(!interaction.selecting() && !interaction.dragging() &&
                            selection.pixelSelection() == QRect(10, 20, 80, 60) &&
                            capture.sessionState == ScreenshotSessionState::Editing,
                        "real toolbar command must receive committed bounds and editing state");
                ++calls;
            }
            void setShapeTool() override {
                checkCommitted();
                interaction.setCanvasTool(ScreenshotActiveTool::Shape);
            }
            void setOcrTool() override {
                checkCommitted();
                interaction.setOcrTool();
            }
            void setTableTool() override {
                checkCommitted();
                interaction.setTableTool();
            }
            void setQrTool() override {
                checkCommitted();
                interaction.setQrTool();
            }
            void copySelectionToClipboard() override {
                checkCommitted();
                interaction.reset();
            }
            void startScreenRecording() override {
                checkCommitted();
                capture.presentationSuppressed = true;
            }
        } commands(interaction, selection, capture);
        require(toolbarSettings.setLastDrawingTool(QStringLiteral("shape")),
                "seed remembered shape for every command");
        ScreenshotToolbarWindow window(commands);
        window.resetForNewCapture();
        int presentations = 0;
        int confirmations = 0;
        int preparations = 0;
        const auto refreshAvailability = [&] {
            window.setRecognitionEnabled(
                makeScreenshotToolbarPresentationState(interaction, selection).ocrAvailable);
        };
        ScreenshotOverlayInputActions actions;
        actions.mainToolbarVisible = [&] { return window.isVisible(); };
        actions.canActivateDrawingShortcut = [&](const QString& tool) {
            return window.palette()->canActivateDrawingShortcut(tool);
        };
        actions.canActivateScreenshotShortcut = [&](const QString& action) {
            refreshAvailability();
            return window.palette()->canActivateScreenshotShortcut(action);
        };
        actions.activateDrawingShortcut = [&](const QString& tool) {
            return window.activateDrawingShortcut(tool);
        };
        actions.activateScreenshotShortcut = [&](const QString& action) {
            return window.palette()->activateScreenshotShortcut(action);
        };
        actions.prepareExplicitSelectionCommand = [&] {
            ++preparations;
            window.suppressRememberedDrawingTool();
        };
        actions.showToolbar = [&] {
            require(commands.calls == 1, "command must execute before toolbar presentation");
            ++presentations;
            window.restoreRememberedDrawingTool();
        };
        actions.selectionConfirmed = [&] { ++confirmations; };
        ScreenshotOverlayInputHandler handler(
            {capture, interaction, selection, intelligent, geometry, displays, actions});
        auto screenshot = originalScreenshot;
        auto draw = originalDrawing;
        for (auto& binding : screenshot)
            binding.clear();
        for (auto& binding : draw)
            binding.clear();
        (drawing ? draw : screenshot).insert(id, {QStringLiteral("Alt+J")});
        require(screenshotSettings.setAllShortcutsAtomic(screenshot) &&
                    drawingSettings.setAllShortcutsAtomic(draw),
                "configure integration shortcut");
        QWidget receiver;
        snow_shot::presentation::WindowShortcutManager manager;
        manager.addScopeWindow(&receiver);
        ScreenshotOverlayShortcutController shortcuts(manager, handler, interaction, intelligent,
                                                      actions);
        const auto dispatch = [&] {
            PhysicalKeyEvent overrideEvent(QEvent::ShortcutOverride, Qt::Key_J, Qt::AltModifier);
            overrideEvent.setAccepted(false);
            QCoreApplication::sendEvent(&receiver, &overrideEvent);
            PhysicalKeyEvent event(QEvent::KeyPress, Qt::Key_J, Qt::AltModifier);
            event.setAccepted(false);
            QCoreApplication::sendEvent(&receiver, &event);
            return event.isAccepted();
        };
        if (recognition) {
            selection.setSelectionRect(QRectF(0, 0, 3841, 2160));
            require(!dispatch() && interaction.selecting() && preparations == 0 &&
                        commands.calls == 0 && presentations == 0,
                    "oversized recognition selection must remain untouched");
            selection.setSelectionRect(QRectF(10, 20, 80, 60));
        }
        intelligent.beginPress(QPointF(30, 40), selection.normalizedSelection());
        require(dispatch() && commands.calls == 1 && preparations == 1,
                "keyboard shortcut must reach its real toolbar command exactly once");
        require(presentations == (completion ? 0 : 1) && confirmations == (completion ? 0 : 1),
                "completed captures must not be presented or confirmed again");
        handler.handleMouseRelease(nullptr, QPointF(300, 300));
        require(commands.calls == 1 && selection.pixelSelection() == QRect(10, 20, 80, 60),
                "pending mouse release must not repeat the command or change the region");
        if (drawing)
            require(window.palette()->activeTool() == Tool::Shape,
                    "remembered shape must not toggle off an explicitly requested shape");
        if (recognition) {
            const Tool expected = id == QStringLiteral("text_recognition")    ? Tool::Ocr
                                  : id == QStringLiteral("table_recognition") ? Tool::Table
                                                                              : Tool::Qr;
            require(window.palette()->activeTool() == expected,
                    "the toolbar must retain the exact requested recognition tool");
        }
        if (completion)
            require(id == QStringLiteral("copy_to_clipboard") ? interaction.inactive()
                                                              : capture.presentationSuppressed,
                    "completion shortcut must reach the correct command sink operation");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "failed to create toolbar test storage directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({storageDirectory.filePath(QStringLiteral("bin")),
                                       storageDirectory.filePath(QStringLiteral("data")), 60000}));
    if (application.arguments().contains(QStringLiteral("--eraser-only"))) {
        eraserSubtoolsReachToolbarCommands();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--scrolling-interval-only"))) {
        scrollingIntervalRestoresAndReachesCommands();
        applicationStorage.shutdown();
        return 0;
    }
    eraserSubtoolsReachToolbarCommands();
    rememberedDrawingToolRestoresOncePerCapture();
    selectionShortcutsReachToolbarCommands();
    applicationStorage.shutdown();
    return 0;
}
