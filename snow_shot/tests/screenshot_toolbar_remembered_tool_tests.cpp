#include "physical_key_test_support.h"
#ifdef Q_OS_WIN
#include "toolbar_capture_exclusion_native_tests.h"
#include "../src/platform/windows/captureexclusioncomposition.h"
#include "snow_shot/platform/windowcaptureexclusion.h"
#endif
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
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include "widgets/radio_button_group.h"
#include "widgets/modal.h"
#include "widgets/button.h"
#include <QAbstractButton>
#include <QEvent>
#include <QElapsedTimer>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QWindow>

#include <cstdlib>
#include <functional>
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
    void setSpeechBubbleTool() override {
        ++speechBubbleToolCount;
    }
    void setArrowTool() override {}
    void setLineTool() override {}
    void setBrushTool() override {}
    void setHighlightTool() override {}
    void setBrushHighlightTool() override {}
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
    void setSequenceNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void splitScrollingScreenshot() override {
        ++scrollingImageSplits;
    }
    void editScrollingScreenshot() override {
        ++scrollingImageEdits;
    }
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&, quint32) override {}
    void setSequenceNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSequenceNumbers() override {}
    void incrementSelectedSequenceNumbers() override {}
    void createTextForSelectedSequenceNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}
    void requestRecapture() override {
        ++recaptureCount;
    }

    void setScrollingScreenshotAutoScrollIntervalMs(int milliseconds) override {
        interval = milliseconds;
        ++intervalChanges;
    }
    int interval = 0;
    int scrollingImageSplits = 0;
    int scrollingImageEdits = 0;
    int intervalChanges = 0;
    int moveToolCount = 0;
    int selectToolCount = 0;
    int shapeToolCount = 0;
    int speechBubbleToolCount = 0;
    int elementEraserCount = 0;
    int rectangleEraserCount = 0;
    int brushEraserCount = 0;
    int recaptureCount = 0;
};

void editSelectionToolbarFollowsSetting() {
    using Tool = ScreenshotToolPalette::Tool;
    const storage::ScreenshotUiSettings settings;
    const bool original = settings.showEditSelectionToolbar();
    const auto cleanup =
        qScopeGuard([&] { static_cast<void>(settings.setShowEditSelectionToolbar(original)); });
    require(original, "the edit selection toolbar defaults to shown");
    RecordingToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    window.resetForNewCapture();
    auto* palette = window.palette();
    const auto moveControls = [&] {
        return palette->findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls"));
    };
    require(palette->activeTool() == Tool::Move && palette->actionToolbarVisible() &&
                moveControls() != nullptr && !moveControls()->isHidden(),
            "the default capture must show the edit selection sub-toolbar");
    const int shownHeight = palette->sizeHint().height();
    int contentChanges = 0;
    QObject::connect(palette, &ScreenshotToolPalette::visibleContentChanged,
                     [&] { ++contentChanges; });
    require(settings.setShowEditSelectionToolbar(false) && palette->activeTool() == Tool::Move &&
                !palette->actionToolbarVisible() && moveControls()->isHidden() &&
                palette->sizeHint().height() < shownHeight && contentChanges > 0,
            "disabling the setting must immediately hide the row and shrink the toolbar");
    require(commands.moveToolCount == 0,
            "changing toolbar visibility must not issue a tool command");
    require(settings.setShowEditSelectionToolbar(true) && palette->actionToolbarVisible() &&
                !moveControls()->isHidden() && palette->sizeHint().height() == shownHeight,
            "re-enabling the active edit selection row must restore its controls and height");
    require(settings.setShowEditSelectionToolbar(false), "hide the row before a new capture");
    window.resetForNewCapture();
    require(!palette->actionToolbarVisible(),
            "a new capture must preserve the disabled toolbar preference");
    window.setActiveTool(Tool::Shape);
    require(palette->styleToolbarVisible(), "drawing styles remain available");
    require(settings.setShowEditSelectionToolbar(true) && palette->activeTool() == Tool::Shape &&
                palette->styleToolbarVisible() && !palette->actionToolbarVisible(),
            "enabling the edit selection toolbar must preserve the active drawing tool");
    window.setActiveTool(Tool::Move);
    require(palette->actionToolbarVisible() && moveControls() != nullptr &&
                !moveControls()->isHidden(),
            "returning to edit selection must show its enabled sub-toolbar");
    require(settings.setShowEditSelectionToolbar(false), "disable toolbar before construction");
    ScreenshotToolbarWindow hiddenWindow(commands);
    hiddenWindow.resetForNewCapture();
    auto* hiddenPalette = hiddenWindow.palette();
    require(hiddenPalette->activeTool() == Tool::Move && !hiddenPalette->actionToolbarVisible() &&
                hiddenPalette->findChild<QWidget*>(
                    QStringLiteral("screenshotMoveActionControls")) == nullptr,
            "a toolbar constructed while disabled must not materialize edit selection controls");
    const QString recapture = QStringLiteral("recapture");
    for (int check = 0; check < 3; ++check) {
        require(hiddenPalette->canActivateScreenshotShortcut(recapture) &&
                    commands.recaptureCount == 0 && !hiddenPalette->actionToolbarVisible() &&
                    hiddenPalette->findChild<QWidget*>(
                        QStringLiteral("screenshotMoveActionControls")) == nullptr,
                "recapture availability must not materialize a hidden edit selection row");
    }
    require(
        hiddenPalette->activateScreenshotShortcut(recapture) && commands.recaptureCount == 1 &&
            hiddenPalette->activeTool() == Tool::Move && !hiddenPalette->actionToolbarVisible() &&
            hiddenPalette->findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls")) ==
                nullptr,
        "recapture must dispatch once without materializing or showing the hidden row");
    hiddenWindow.setRecaptureBusy(true);
    require(!hiddenPalette->canActivateScreenshotShortcut(recapture) &&
                !hiddenPalette->activateScreenshotShortcut(recapture) &&
                commands.recaptureCount == 1,
            "busy recapture must reject shortcuts while the edit selection row is hidden");
    hiddenWindow.setRecaptureBusy(false);
    require(settings.setShowEditSelectionToolbar(true) && hiddenPalette->actionToolbarVisible() &&
                hiddenPalette->findChild<QWidget*>(
                    QStringLiteral("screenshotMoveActionControls")) != nullptr &&
                hiddenPalette->canActivateScreenshotShortcut(recapture),
            "enabling the row after hidden shortcuts must immediately restore its controls");
    auto* recaptureButton = hiddenPalette->findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotRecaptureButton"));
    require(recaptureButton != nullptr, "the shown edit selection row must expose recapture");
    recaptureButton->click();
    require(commands.recaptureCount == 2, "pointer and shortcut recapture must dispatch once each");
    require(settings.setShowEditSelectionToolbar(false),
            "hide the materialized edit selection row");
    hiddenWindow.resetForNewCapture();
    require(!hiddenPalette->actionToolbarVisible() &&
                hiddenPalette->activateScreenshotShortcut(recapture) &&
                commands.recaptureCount == 3,
            "capture resets must keep recapture available while the row remains hidden");
    hiddenWindow.setActiveTool(Tool::Select);
    require(hiddenPalette->actionToolbarVisible() &&
                !hiddenPalette->canActivateScreenshotShortcut(recapture) &&
                !hiddenPalette->activateScreenshotShortcut(recapture) &&
                commands.recaptureCount == 3,
            "annotation selection actions must remain available without enabling recapture");
    hiddenWindow.setActiveTool(Tool::Move);
    require(
        !hiddenPalette->actionToolbarVisible() &&
            hiddenPalette->findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls")) ==
                nullptr &&
            hiddenPalette->activateScreenshotShortcut(recapture) && commands.recaptureCount == 4,
        "recapture must remain available after tool changes without rebuilding the hidden row");
    require(settings.setShowEditSelectionToolbar(true) && hiddenPalette->actionToolbarVisible() &&
                hiddenPalette->findChild<QWidget*>(
                    QStringLiteral("screenshotMoveActionControls")) != nullptr,
            "enabling the setting must materialize and show the active edit selection row");
}

void moveOptionsCapabilityIsIndependentOfVisibility() {
    using Tool = ScreenshotToolPalette::Tool;
    using Family = ScreenshotToolPalette::ActionFamily;
    const QString recapture = QStringLiteral("recapture");
    ScreenshotToolPalette::Options options;
    options.showMoveTool = true;
    ScreenshotToolPalette unsupported(options);
    unsupported.setMoveOptionsToolbarVisible(true);
    unsupported.setActiveTool(Tool::Move);
    require(!unsupported.actionToolbarVisible() && !unsupported.ensureActionFamily(Family::Move) &&
                !unsupported.canActivateScreenshotShortcut(recapture) &&
                !unsupported.activateScreenshotShortcut(recapture),
            "palettes without screenshot capture options must not enable the row or recapture");

    options.enableMoveOptionsToolbar = true;
    ScreenshotToolPalette supported(options);
    supported.setMoveOptionsToolbarVisible(false);
    supported.setActiveTool(Tool::Move);
    require(supported.ensureActionFamily(Family::Move),
            "explicit move control materialization must succeed while the row is hidden");
    auto* controls = supported.findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls"));
    require(controls != nullptr && controls->isHidden() && !supported.actionToolbarVisible(),
            "materializing supported controls must preserve the row's hidden state");
    supported.setMoveOptionsToolbarVisible(true);
    require(supported.actionToolbarVisible() && !controls->isHidden() &&
                supported.canActivateScreenshotShortcut(recapture),
            "enabling a materialized row must immediately reveal its controls");
}

void selectionToolbarStateSurvivesEvictedMoveControls() {
    using Tool = ScreenshotToolPalette::Tool;
    using Button = adqt::widgets::AdButton;
    const storage::ScreenshotUiSettings settings;
    const bool originalShown = settings.showEditSelectionToolbar();
    const bool originalHidden = settings.selectionToolbarHidden();
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(settings.setShowEditSelectionToolbar(originalShown));
        static_cast<void>(settings.setSelectionToolbarHidden(originalHidden));
    });
    require(settings.setShowEditSelectionToolbar(true) && settings.setSelectionToolbarHidden(false),
            "the lifecycle fixture must materialize a shown Move action row");
    struct Commands final : RecordingToolbarCommands {
        std::function<void()> closeCapture;
        int cancelCount = 0;
        int visibilityCount = 0;
        bool hidden = false;
        void cancelCapture() override {
            ++cancelCount;
            closeCapture();
        }
        void setSelectionToolbarHidden(bool value) override {
            ++visibilityCount;
            hidden = value;
        }
    } commands;
    ScreenshotToolbarWindow window(commands);
    auto* palette = window.palette();
    const auto moveControls = [&] {
        return palette->findChild<QWidget*>(QStringLiteral("screenshotMoveActionControls"));
    };
    const auto hideButton = [&] {
        return palette->findChild<Button*>(QStringLiteral("screenshotHideSelectionToolbarButton"));
    };
    QPointer<QWidget> retiredRow = moveControls();
    QPointer<Button> retiredButton = hideButton();
    require(retiredRow && retiredButton, "the initial capture must expose Move controls");
    retiredButton->click();
    require(settings.selectionToolbarHidden() && palette->selectionToolbarHidden() &&
                commands.visibilityCount == 1 && commands.hidden,
            "the Move visibility action must save and dispatch its selected state once");
    require(window.activateDrawingShortcut(QStringLiteral("shape")) &&
                window.activateDrawingShortcut(QStringLiteral("line")) &&
                palette->activeTool() == Tool::Line && moveControls() == nullptr &&
                hideButton() == nullptr,
            "switching drawing tools must retire the previous Move controls");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(retiredRow.isNull() && retiredButton.isNull(),
            "the retired Move row and visibility button must actually be destroyed");

    require(settings.setShowEditSelectionToolbar(false),
            "disable Move row materialization for the capture reset");
    commands.closeCapture = [&] {
        // Capture cancellation conceals the toolbar before deferred maintenance resets it.
        window.hide();
        window.resetForNewCapture();
    };
    require(palette->activateScreenshotShortcut(QStringLiteral("cancel_screenshot")) &&
                commands.cancelCount == 1 && palette->activeTool() == Tool::Move &&
                palette->selectionToolbarHidden() && moveControls() == nullptr &&
                hideButton() == nullptr,
            "closing after tool switches must reset state without using destroyed Move controls");
    require(settings.setSelectionToolbarHidden(false) && !palette->selectionToolbarHidden() &&
                moveControls() == nullptr && hideButton() == nullptr,
            "live visibility settings must update state without rebuilding an absent Move row");
    palette->setSelectionToolbarHidden(true);
    require(palette->selectionToolbarHidden() && moveControls() == nullptr,
            "direct visibility synchronization must also work while the row is absent");
    window.resetForNewCapture();
    require(!palette->selectionToolbarHidden() && moveControls() == nullptr &&
                hideButton() == nullptr && commands.visibilityCount == 1,
            "repeated hidden-row capture resets must restore saved state without a command");
    require(settings.setSelectionToolbarHidden(true) && settings.setShowEditSelectionToolbar(true),
            "reopen the Move action row with a saved hidden-selection preference");
    auto* restoredButton = hideButton();
    require(moveControls() != nullptr && restoredButton != nullptr &&
                palette->actionToolbarVisible() && palette->selectionToolbarHidden() &&
                restoredButton->buttonStyle() == Button::ButtonStyle::Solid,
            "rebuilt Move controls must restore the saved visibility button state");
    restoredButton->click();
    require(!settings.selectionToolbarHidden() && !palette->selectionToolbarHidden() &&
                restoredButton->buttonStyle() == Button::ButtonStyle::Text &&
                commands.visibilityCount == 2 && !commands.hidden,
            "the rebuilt visibility button must save and dispatch exactly one new action");
}

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
        auto* splitButton = palette->findChild<QAbstractButton*>(
            QStringLiteral("screenshotScrollingImageSplittingButton"));
        require(splitButton != nullptr, "scrolling toolbar must expose Image Splitting");
        splitButton->click();
        require(commands.scrollingImageSplits == 1,
                "Image Splitting must reach the capture command exactly once");
        auto* editingButton = palette->findChild<QAbstractButton*>(
            QStringLiteral("screenshotScrollingImageEditingButton"));
        require(editingButton != nullptr, "scrolling toolbar must expose Image Editing");
        editingButton->click();
        require(commands.scrollingImageEdits == 1,
                "Image Editing must reach the capture command exactly once");
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
void selectionShortcutsReachToolbarCommands(bool hiddenTools = false, bool captureAction = false) {
    using Tool = ScreenshotToolPalette::Tool;
    const storage::ScreenshotShortcutSettings screenshotSettings;
    const storage::DrawingShortcutSettings drawingSettings;
    const storage::DrawingSettings drawingPreferences;
    const storage::ScreenshotToolbarSettings toolbarSettings;
    const auto originalScreenshot = screenshotSettings.allShortcuts();
    const auto originalDrawing = drawingSettings.allShortcuts();
    const bool originalRemember = drawingPreferences.rememberLastUsedTool();
    const QString originalTool = toolbarSettings.lastDrawingTool();
    const auto drawingKind = storage::ScreenshotToolbarLayoutKind::DrawingTools;
    const auto actionKind = storage::ScreenshotToolbarLayoutKind::ActionTools;
    const auto originalDrawingLayout = toolbarSettings.layout(drawingKind);
    const auto originalActionLayout = toolbarSettings.layout(actionKind);
    const auto cleanup = qScopeGuard([&] {
        static_cast<void>(screenshotSettings.setAllShortcutsAtomic(originalScreenshot));
        static_cast<void>(drawingSettings.setAllShortcutsAtomic(originalDrawing));
        static_cast<void>(drawingPreferences.setRememberLastUsedTool(originalRemember));
        static_cast<void>(toolbarSettings.setLastDrawingTool(originalTool));
        static_cast<void>(toolbarSettings.setLayout(drawingKind, originalDrawingLayout));
        static_cast<void>(toolbarSettings.setLayout(actionKind, originalActionLayout));
    });
    require(drawingPreferences.setRememberLastUsedTool(true), "enable remembered tool fixture");
    if (hiddenTools) {
        require(toolbarSettings.setLayout(
                    drawingKind,
                    {{}, snow_shot::presentation::toolbar_layout::defaultOrder(drawingKind)}) &&
                    toolbarSettings.setLayout(
                        actionKind,
                        {{}, snow_shot::presentation::toolbar_layout::defaultOrder(actionKind)}),
                "hide configurable capture tools before constructing the toolbar");
    }
    const auto drawingLayout = toolbarSettings.layout(drawingKind);
    const auto actionLayout = toolbarSettings.layout(actionKind);

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
            if (hiddenTools)
                window.show();
        };
        actions.updateOverlayState = [&] {
            require(commands.calls == 1 && !interaction.moveToolActive(),
                    "the first committed overlay must already use the requested tool");
        };
        actions.selectionConfirmed = [&] {
            ++confirmations;
            if (captureAction) {
                if (drawing) {
                    window.restoreRememberedDrawingTool();
                } else {
                    window.suppressRememberedDrawingTool();
                    refreshAvailability();
                    require(window.palette()->activateScreenshotShortcut(id),
                            "pending capture action must activate its toolbar command");
                }
            }
        };
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
        if (recognition && !captureAction) {
            selection.setSelectionRect(QRectF(0, 0, 3841, 2160));
            require(!dispatch() && interaction.selecting() && preparations == 0 &&
                        commands.calls == 0 && presentations == 0,
                    "oversized recognition selection must remain untouched");
            selection.setSelectionRect(QRectF(10, 20, 80, 60));
        }
        intelligent.beginPress(QPointF(30, 40), selection.normalizedSelection());
        if (captureAction)
            handler.handleMouseRelease(nullptr, QPointF(30, 40));
        else
            require(dispatch(), "keyboard shortcut must be handled");
        require(commands.calls == 1 && preparations == (captureAction ? 0 : 1),
                "selection must reach its real toolbar command exactly once");
        require(presentations == (completion ? 0 : 1) &&
                    confirmations == (completion && !captureAction ? 0 : 1),
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
        if (hiddenTools) {
            require(toolbarSettings.layout(drawingKind) == drawingLayout &&
                        toolbarSettings.layout(actionKind) == actionLayout,
                    "capture hotkeys must preserve the saved hidden layouts");
            for (auto* button :
                 window.palette()->mainPanel()->findChildren<adqt::widgets::AdButton*>()) {
                if (!button->property("screenshotToolbarItemId").toString().isEmpty())
                    require(button->isHidden(), "capture hotkeys must not reveal hidden buttons");
            }
            if (!completion) {
                require(dispatch() && commands.calls == 1 &&
                            window.palette()->activeTool() == Tool::Select,
                        "hidden tool hotkeys toggle back to Select after selection is committed");
            }
        }
    }
}
void speechBubbleReachesScreenshotCommands() {
    RecordingToolbarCommands commands;
    ScreenshotToolbarWindow window(commands);
    auto* palette = window.palette();
    require(palette != nullptr &&
                palette->activateDrawingShortcut(QStringLiteral("speech_bubble")) &&
                commands.speechBubbleToolCount == 1 &&
                palette->activeTool() == ScreenshotToolPalette::Tool::SpeechBubble,
            "Speech Bubble activates screenshot command routing exactly once");
    window.setActiveTool(ScreenshotToolPalette::Tool::SpeechBubble);
    require(palette->activeTool() == ScreenshotToolPalette::Tool::SpeechBubble,
            "screenshot tool synchronization recognizes Speech Bubble");
}
#ifdef Q_OS_WIN
void scrollingCompositionPreservesVisibleToolbar() {
    // Match main.cpp: native toolbars must not materialize every sibling widget.
    const bool nativeSiblingsDisabled =
        QCoreApplication::testAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    const auto restoreNativeSiblings = qScopeGuard([&] {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings,
                                       nativeSiblingsDisabled);
    });
    QWidget owner(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    owner.setGeometry(100, 100, 1200, 700);
    owner.show();
    RecordingToolbarCommands commands;
    ScreenshotToolbarWindow toolbar(commands);
    toolbar.setOwnerWindow(&owner);
    toolbar.moveContentTo(QPoint(150, 300));
    toolbar.show();
    QCoreApplication::processEvents();
    namespace composition = snow_shot::platform::windows::detail;
    require(snow_shot::platform::setWindowExcludedFromCapture(&owner, true), "exclude overlay");
    const QPointer<QWindow> overlayWindow(owner.windowHandle());
    const QPointer<QWindow> toolbarWindow(toolbar.windowHandle());
    QWidget thumbnail(&owner);
    thumbnail.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    thumbnail.setAttribute(Qt::WA_TranslucentBackground);
    thumbnail.hide();
    composition::enableCaptureExclusionCompositionFallback(&thumbnail);
    require(!thumbnail.windowHandle(),
            "the scrolling thumbnail must start without a native surface");
    // Exercise the fallback even on Windows versions where native affinity succeeds.
    require(composition::applyCaptureExclusionCompositionFallback(&thumbnail), "compose thumbnail");
    require(snow_shot::platform::setWindowExcludedFromCapture(&thumbnail, true),
            "exclude thumbnail");
    require(overlayWindow && owner.windowHandle() == overlayWindow && toolbarWindow &&
                toolbar.windowHandle() == toolbarWindow && toolbar.isVisible(),
            "excluding the hidden thumbnail must preserve the overlay and visible toolbar");
    DWORD affinity = WDA_NONE;
    require(GetWindowDisplayAffinity(reinterpret_cast<HWND>(owner.internalWinId()), &affinity) &&
                affinity == WDA_EXCLUDEFROMCAPTURE && thumbnail.isHidden(),
            "thumbnail composition must preserve overlay exclusion and stay hidden until preview");
    require(composition::applyCaptureExclusionCompositionFallback(&toolbar), "compose toolbar");
    require(snow_shot::platform::setWindowExcludedFromCapture(&toolbar, true), "exclude toolbar");
    for (const bool scrolling : {true, false, true}) {
        toolbar.setScrollingScreenshotMode(scrolling);
        const auto hwnd = reinterpret_cast<HWND>(toolbar.internalWinId());
        const QPoint center = toolbar.palette()->mainPanel()->mapTo(
            &toolbar, toolbar.palette()->mainPanel()->rect().center());
        const qreal dpr = toolbar.devicePixelRatioF();
        const auto hasCoverage = [&] {
            const HRGN region = CreateRectRgn(0, 0, 0, 0);
            const int type = GetWindowRgn(hwnd, region);
            const bool covered = type != ERROR && PtInRegion(region, qRound(center.x() * dpr),
                                                             qRound(center.y() * dpr));
            DeleteObject(region);
            return covered;
        };
        QElapsedTimer deadline;
        deadline.start();
        do {
            QCoreApplication::processEvents();
            if (hasCoverage())
                break;
            QThread::msleep(1);
        } while (deadline.elapsed() < 3000);
        require(toolbar.isVisible() && IsWindowVisible(hwnd) && hasCoverage(),
                "scrolling mode changes must preserve visible, interactive toolbar content");
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "failed to create toolbar test storage directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({storageDirectory.filePath(QStringLiteral("bin")),
                                       storageDirectory.filePath(QStringLiteral("data")), 60000}));
#ifdef Q_OS_WIN
    if (application.arguments().contains(QStringLiteral("--scrolling-composition-only"))) {
        scrollingCompositionPreservesVisibleToolbar();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--native-capture-exclusion-only")) ||
        application.arguments().contains(
            QStringLiteral("--native-capture-exclusion-affinity-only"))) {
        RecordingToolbarCommands commands;
        ScreenshotToolbarWindow toolbar(commands);
        const int result = application.arguments().contains(
                               QStringLiteral("--native-capture-exclusion-affinity-only"))
                               ? verifyToolbarCaptureExclusionAffinity(toolbar, "screenshot")
                               : verifyToolbarCaptureExclusion(toolbar, "screenshot");
        applicationStorage.shutdown();
        return result;
    }
#endif
    if (application.arguments().contains(QStringLiteral("--speech-bubble-only"))) {
        speechBubbleReachesScreenshotCommands();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--eraser-only"))) {
        eraserSubtoolsReachToolbarCommands();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--edit-selection-toolbar-only"))) {
        editSelectionToolbarFollowsSetting();
        moveOptionsCapabilityIsIndependentOfVisibility();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selection-toolbar-lifecycle-only"))) {
        selectionToolbarStateSurvivesEvictedMoveControls();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--hidden-tools-only"))) {
        selectionShortcutsReachToolbarCommands(true);
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--scrolling-interval-only"))) {
        scrollingIntervalRestoresAndReachesCommands();
        applicationStorage.shutdown();
        return 0;
    }
    eraserSubtoolsReachToolbarCommands();
    editSelectionToolbarFollowsSetting();
    moveOptionsCapabilityIsIndependentOfVisibility();
    selectionToolbarStateSurvivesEvictedMoveControls();
    rememberedDrawingToolRestoresOncePerCapture();
    selectionShortcutsReachToolbarCommands();
    selectionShortcutsReachToolbarCommands(false, true);
    applicationStorage.shutdown();
    return 0;
}
