#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void selectionEffectsPersistAcrossRestarts() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory must exist");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    const snow_shot::storage::StorageInitializationOptions options{
        directory.filePath(QStringLiteral("bin")), directory.filePath(QStringLiteral("data")),
        60000};
    require(storage.initialize(options).success, "test storage must initialize");
    ScreenshotSelectionSettingsStore settings;
    require(settings.cornerRadius() == 0 && settings.shadowWidth() == 0,
            "new installations must default to disabled selection effects");
    require(!settings.aspectRatioLocked(),
            "new installations must default to an unlocked selection aspect ratio");
    require(settings.selectionTarget() == ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "new installations must default to the existing child-element selection mode");
    settings.setSelectionTarget(ScreenshotIntelligentSelectionTarget::Window);
    require(settings.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window,
            "selection target preference must be writable");

    QObject parent;
    ScreenshotCaptureState state;
    ScreenshotDisplaySession displays;
    CapturedDisplayModel display;
    display.active = true;
    display.physicalRect = QRect(0, 0, 1920, 1080);
    display.logicalRect = display.physicalRect;
    displays.appendDisplay(display);
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    ScreenshotInteractionState interaction;
    interaction.applySelectionParams();
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(20, 30, 640, 360));
    ScreenshotApplySelectionCallback applyResize;
    ScreenshotSelectionEditUiActions ui;
    ui.openResizeModal = [&](QObject*, const ScreenshotSelectionResizeRequest&,
                             ScreenshotApplySelectionCallback apply) {
        applyResize = std::move(apply);
        return true;
    };
    ScreenshotSelectionEditWorkflow workflow({
        parent,
        state,
        displays,
        geometry,
        interaction,
        selection,
        ui,
        [&](int radius, int shadowWidth) { settings.setSelectionEffects(radius, shadowWidth); },
        [&](bool locked) { settings.setAspectRatioLocked(locked); },
    });

    workflow.setSelectionCornerRadiusFromToolbar(24);
    workflow.setSelectionShadowWidthFromToolbar(12);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    require(settings.cornerRadius() == 24 && settings.shadowWidth() == 12,
            "toolbar edits must persist both effects without exporting a screenshot");
    require(settings.aspectRatioLocked(),
            "the toolbar aspect-ratio toggle must persist without exporting a screenshot");
    require(!settings.hasPreviousSelectionParams(),
            "editing effects must not create a previous selection rectangle");

    // Shutdown flushes the debounced writes, just as a normal application exit does.
    storage.shutdown();
    require(storage.initialize(options).success, "storage must reopen after shutdown");
    ScreenshotSelectionSettingsStore restartedSettings;
    require(restartedSettings.cornerRadius() == 24 && restartedSettings.shadowWidth() == 12,
            "selection effects must survive a storage restart");
    require(restartedSettings.aspectRatioLocked(),
            "the selection aspect-ratio toggle must survive a storage restart");
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    require(!restartedSettings.aspectRatioLocked(),
            "disabling the toolbar aspect-ratio toggle must update the saved preference");
    require(restartedSettings.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window,
            "selection target preference must survive a storage restart");
    restartedSettings.setSelectionTarget(ScreenshotIntelligentSelectionTarget::WindowSubElement);

    workflow.openSelectionResizeModalFromToolbar();
    require(static_cast<bool>(applyResize), "resize workflow must expose its apply callback");
    applyResize = {};
    require(settings.cornerRadius() == 24 && settings.shadowWidth() == 12,
            "cancelling the resize dialog must preserve the saved effects");

    workflow.openSelectionResizeModalFromToolbar();
    ScreenshotSelectionParams params;
    params.selection = QRect(50, 60, 320, 180);
    params.radius = 32;
    params.shadowWidth = 16;
    applyResize(params);
    require(settings.cornerRadius() == 32 && settings.shadowWidth() == 16,
            "confirmed resize parameters must update the persistent effects");

    workflow.setSelectionCornerRadiusFromToolbar(1000);
    workflow.setSelectionShadowWidthFromToolbar(1000);
    require(settings.cornerRadius() == 256 && settings.shadowWidth() == 64,
            "toolbar edits must persist the clamped effect values");
    auto& configuration = storage.configuration();
    require(!configuration.setValue(QStringLiteral("screenshot_selection/corner_radius"),
                                    QStringLiteral("invalid")) &&
                !configuration.setValue(QStringLiteral("screenshot_selection/shadow_width"), 1.5) &&
                !configuration.setValue(QStringLiteral("screenshot_selection/lock_aspect_ratio"),
                                        QStringLiteral("invalid")),
            "persistent selection settings must reject values of the wrong type");

    workflow.setSelectionCornerRadiusFromToolbar(-1);
    workflow.setSelectionShadowWidthFromToolbar(-1);
    storage.shutdown();
    require(storage.initialize(options).success, "disabled effects must reload");
    require(settings.cornerRadius() == 0 && settings.shadowWidth() == 0,
            "turning effects off must survive a restart");
    require(!settings.aspectRatioLocked(),
            "turning off the aspect-ratio lock must survive a restart");

    settings.setSelectionEffects(20, 10);
    settings.setAspectRatioLocked(true);
    settings.clear();
    require(settings.cornerRadius() == 0 && settings.shadowWidth() == 0,
            "clearing selection settings must reset the persistent effects");
    require(!settings.aspectRatioLocked(),
            "clearing selection settings must reset the aspect-ratio lock preference");
    require(settings.selectionTarget() == ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "selection target preference must remain independently persisted when effects clear");
    storage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    selectionEffectsPersistAcrossRestarts();
    return 0;
}
