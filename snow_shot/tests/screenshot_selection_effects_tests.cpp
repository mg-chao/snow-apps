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
        [&](ScreenshotSelectionAspectRatioPreset preset, bool locked) {
            settings.setAspectRatioPreference(preset, locked);
        },
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

void selectionAspectRatioWorkflowPersistsOnlyUserPreferences() {
    using Preset = ScreenshotSelectionAspectRatioPreset;
    QTemporaryDir directory;
    require(directory.isValid(), "aspect ratio workflow requires isolated storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    const snow_shot::storage::StorageInitializationOptions options{
        directory.filePath(QStringLiteral("bin")), directory.filePath(QStringLiteral("data")),
        60000};
    require(storage.initialize(options).success, "aspect ratio workflow storage must initialize");
    ScreenshotSelectionSettingsStore settings;
    require(settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "the workflow must begin with an unlocked Free preference");

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
    int overlayUpdates = 0;
    int toolbarShows = 0;
    int toolbarMoves = 0;
    int preferenceWrites = 0;
    int mainToolbarShows = 0;
    ScreenshotApplySelectionCallback applyResize;
    ScreenshotSelectionEditUiActions ui;
    ui.updateOverlayState = [&]() { ++overlayUpdates; };
    ui.showSelectionToolbar = [&]() { ++toolbarShows; };
    ui.moveToolbar = [&]() { ++toolbarMoves; };
    ui.showToolbar = [&]() { ++mainToolbarShows; };
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
        [&](Preset preset, bool locked) {
            ++preferenceWrites;
            settings.setAspectRatioPreference(preset, locked);
        },
    });
    const auto requireCounts = [&](int updates, int shows, int moves, int writes) {
        require(overlayUpdates == updates && toolbarShows == shows && toolbarMoves == moves &&
                    preferenceWrites == writes,
                "aspect ratio commands must update and reposition the toolbar exactly once");
    };

    bool atomicPreference = true;
    int preferenceNotifications = 0;
    const auto connection = QObject::connect(
        &storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged, &parent,
        [&](const QString& key, const QJsonValue&) {
            if (key.startsWith(QStringLiteral("screenshot_selection/"))) {
                ++preferenceNotifications;
                atomicPreference &=
                    settings.aspectRatioPreset() == Preset::Landscape4x3 &&
                    storage.configuration()
                        .value(QStringLiteral("screenshot_selection/lock_aspect_ratio"))
                        .toBool();
            }
        });
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Landscape4x3);
    QObject::disconnect(connection);
    require(selection.normalizedSelection() == QRectF(20, 30, 640, 480) &&
                selection.aspectRatioPreset() == Preset::Landscape4x3 &&
                selection.aspectRatioLocked() &&
                settings.aspectRatioPreset() == Preset::Landscape4x3 &&
                settings.aspectRatioLocked() && atomicPreference && preferenceNotifications == 2,
            "choosing a preset must retain width, adjust height, and atomically persist its lock");
    requireCounts(1, 1, 1, 1);
    const auto revision = storage.configuration().revision();
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Landscape4x3);
    requireCounts(1, 1, 1, 2);
    require(storage.configuration().revision() == revision,
            "reselecting an unchanged preset must not generate a storage mutation");

    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Free);
    require(selection.normalizedSelection() == QRectF(20, 30, 640, 480) &&
                selection.aspectRatioPreset() == Preset::Free && !selection.aspectRatioLocked() &&
                settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "Free must disable locking without changing the current geometry");
    requireCounts(2, 2, 2, 3);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    require(selection.aspectRatioPreset() == Preset::Free && selection.aspectRatioLocked() &&
                settings.aspectRatioPreset() == Preset::Free && settings.aspectRatioLocked(),
            "the lock button must support a custom ratio while the preset remains Free");
    requireCounts(3, 2, 2, 4);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    require(!selection.aspectRatioLocked() && !settings.aspectRatioLocked(),
            "unlocking a custom ratio must persist the Free unlocked preference");
    requireCounts(4, 2, 2, 5);

    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Square);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    require(selection.aspectRatioPreset() == Preset::Free && !selection.aspectRatioLocked() &&
                settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "unlocking a preset must clear both the active and saved preset");
    requireCounts(6, 3, 3, 7);
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Portrait2x3);
    require(selection.normalizedSelection() == QRectF(20, 30, 640, 960),
            "switching from a square to a portrait preset must retain the width");
    requireCounts(7, 4, 4, 8);
    storage.shutdown();
    require(storage.initialize(options).success, "aspect ratio preferences must reopen");
    require(settings.aspectRatioPreset() == Preset::Portrait2x3 && settings.aspectRatioLocked(),
            "a chosen preset and its lock must survive storage restart before export");
    require(!settings.hasPreviousSelectionParams(),
            "aspect ratio choices must not create a previous exported selection");

    workflow.openSelectionResizeModalFromToolbar();
    require(static_cast<bool>(applyResize), "explicit resize must provide its apply callback");
    ScreenshotSelectionParams replacement;
    replacement.selection = QRect(50, 60, 320, 180);
    replacement.lockAspectRatio = true;
    replacement.lockDragAspectRatio = true;
    applyResize(replacement);
    require(
        selection.normalizedSelection() == QRectF(replacement.selection) &&
            selection.aspectRatioPreset() == Preset::Free && selection.aspectRatioLocked(),
        "explicit resize must honor supplied geometry and its custom lock over the active preset");
    require(settings.aspectRatioPreset() == Preset::Portrait2x3 && settings.aspectRatioLocked(),
            "explicit replacement must preserve the independently saved toolbar preset");
    requireCounts(8, 4, 4, 8);
    require(mainToolbarShows == 1 && state.sessionState == ScreenshotSessionState::Editing,
            "explicit replacement must restore the editing toolbar once");

    workflow.openSelectionResizeModalFromToolbar();
    replacement.lockAspectRatio = false;
    replacement.lockDragAspectRatio = false;
    applyResize(replacement);
    require(selection.aspectRatioPreset() == Preset::Free && !selection.aspectRatioLocked() &&
                settings.aspectRatioPreset() == Preset::Portrait2x3 && settings.aspectRatioLocked(),
            "an unlocked exact replacement must retain the remembered preset until a user choice");
    requireCounts(9, 4, 4, 8);
    require(mainToolbarShows == 2,
            "each explicit replacement must restore the editing toolbar once");
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Free);
    requireCounts(9, 4, 4, 9);
    require(selection.normalizedSelection() == QRectF(replacement.selection) &&
                settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "explicitly selecting already-active Free must clear the remembered preset without UI "
            "work");
    const auto freeRevision = storage.configuration().revision();
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Free);
    requireCounts(9, 4, 4, 10);
    require(storage.configuration().revision() == freeRevision,
            "reselecting the persisted Free preference must remain a storage and UI no-op");
    workflow.setSelectionAspectRatioPresetFromToolbar(static_cast<Preset>(-1));
    requireCounts(9, 4, 4, 10);

    interaction.reset();
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Square);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    requireCounts(9, 4, 4, 10);
    require(selection.normalizedSelection() == QRectF(replacement.selection) &&
                !selection.aspectRatioLocked(),
            "inactive aspect controls must not modify a retained selection");
    interaction.applySelectionParams();
    const ScreenshotRegionGeometry compound(
        QRegion(QRect(20, 30, 100, 80)).united(QRect(200, 30, 100, 80)));
    selection.setSelectionRegion(compound);
    require(!selection.rectangular(), "the ignored-command fixture must have a compound region");
    workflow.setSelectionAspectRatioPresetFromToolbar(Preset::Square);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    requireCounts(9, 4, 4, 10);
    require(selection.selectionRegion() == compound &&
                settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "nonrectangular aspect controls must leave geometry and saved preferences unchanged");

    settings.setAspectRatioPreference(Preset::Portrait2x3, false);
    require(settings.aspectRatioPreset() == Preset::Portrait2x3 &&
                storage.configuration()
                    .value(QStringLiteral("screenshot_selection/lock_aspect_ratio"))
                    .toBool(),
            "persisting a non-Free preset must enable its saved lock even if false is requested");
    settings.setAspectRatioLocked(true);
    require(settings.aspectRatioPreset() == Preset::Portrait2x3,
            "enabling the saved lock must retain a remembered preset");
    require(storage.configuration().setValue(
                QStringLiteral("screenshot_selection/lock_aspect_ratio"), false) &&
                settings.aspectRatioLocked(),
            "a non-Free imported preset must remain effectively locked despite a false lock flag");
    settings.setAspectRatioLocked(false);
    require(settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "disabling the saved lock must atomically clear a remembered preset");
    settings.setAspectRatioPreference(Preset::Portrait2x3, true);
    settings.clear();
    require(settings.aspectRatioPreset() == Preset::Free && !settings.aspectRatioLocked(),
            "clearing selection preferences must reset both the saved preset and lock");
    storage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    selectionEffectsPersistAcrossRestarts();
    selectionAspectRatioWorkflowPersistsOnlyUserPreferences();
    return 0;
}
