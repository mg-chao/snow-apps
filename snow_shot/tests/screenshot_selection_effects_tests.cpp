#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotinteractionstate.h"
#include "snow_shot/presentation/screenshotselectioneditworkflow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionsettingsstore.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QJsonArray>
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

void selectionResultStylesUseTheCropOrigin() {
    for (const QPoint origin : {QPoint(), QPoint(100, -70), QPoint(-80, 25)}) {
        for (const bool curved : {false, true}) {
            ScreenshotSelectionParams params;
            params.selection = QRect(origin, QSize(120, 80));
            params.radius = 9;
            params.borderType = ScreenshotSelectionBorderType::Border;
            params.borderWidth = 5;
            params.borderColor = Qt::red;
            params.shadowWidth = 11;
            params.shadowColor = Qt::blue;
            params.backgroundOpacityPercent = 35;
            ScreenshotRegionGeometry region(QRegion(0, 0, 40, 80) + QRect(80, 0, 40, 80));
            if (curved) {
                QPainterPath ellipse;
                ellipse.addEllipse(QRectF(0, 0, 120, 80));
                region = ScreenshotRegionGeometry::fromPath(ellipse, ScreenshotRegionType::Curve);
            }
            params.region = region.translated(origin);
            const auto style = params.resultStyle();
            require(style.region == region && style.regionScale == 1 &&
                        style.cornerRadius == params.radius &&
                        style.borderType == params.borderType &&
                        style.borderWidth == params.borderWidth &&
                        style.borderColor == params.borderColor &&
                        style.shadowWidth == params.shadowWidth &&
                        style.shadowColor == params.shadowColor &&
                        style.backgroundOpacityPercent == params.backgroundOpacityPercent,
                    "selection result styles rebase geometry once and preserve all effects");
            ScreenshotSelectionModel model;
            require(model.applyParams(params, params.selection), "restore the offset selection");
            const auto modelStyle = model.resultStyle();
            require(modelStyle.region == style.region &&
                        modelStyle.borderWidth == style.borderWidth &&
                        modelStyle.backgroundOpacityPercent == style.backgroundOpacityPercent,
                    "document snapshots and selection models share the result-style conversion");
            params.region.reset();
            require(!params.resultStyle().region, "legacy rectangular selections remain unshaped");
        }
    }
}

void borderStateAndPersistence() {
    const QRect bounds(0, 0, 1000, 800);
    ScreenshotSelectionModel model;
    model.setSelectionRect(QRect(20, 20, 300, 200));
    static_cast<void>(model.setShadowWidth(11));
    model.setShadowColor(QColor(Qt::black));
    model.setBorderType(ScreenshotSelectionBorderType::Border);
    model.setEffectWidth(6);
    model.setBorderColor(QColor(Qt::red));
    require(model.shadowWidth() == 11 && model.effectWidth() == 6,
            "active width edits preserve shadow");
    const auto params = model.params(bounds);
    ScreenshotSelectionModel copy;
    require(copy.applyParams(params, bounds) && copy.params(bounds) == params,
            "selection copies preserve both effects and type");
    model.setBorderType(ScreenshotSelectionBorderType::Shadow);
    require(model.effectWidth() == 11 && model.borderWidth() == 6, "type switch restores width");
    snow_shot::storage::PersistedSelection saved;
    saved.rectangle = params.selection;
    saved.shadowWidth = params.shadowWidth;
    saved.shadowColor = params.shadowColor;
    saved.borderType = params.borderType;
    saved.borderWidth = params.borderWidth;
    saved.borderColor = params.borderColor;
    auto json = snow_shot::storage::persistedSelectionToJson(saved);
    const auto restored = snow_shot::storage::normalizePersistedSelection(json);
    require(restored.valid && restored.value == saved, "border history JSON round trips");
    for (const auto& key : {QStringLiteral("border_type"), QStringLiteral("border_width"),
                            QStringLiteral("border_color")}) {
        auto invalid = json;
        invalid[key] = QJsonArray();
        require(!snow_shot::storage::normalizePersistedSelection(invalid).valid,
                "invalid border fields are rejected");
        json.remove(key);
    }
    const auto legacy = snow_shot::storage::normalizePersistedSelection(json);
    require(legacy.valid && legacy.value.borderType == ScreenshotSelectionBorderType::Shadow &&
                legacy.value.borderWidth == 0 && legacy.value.borderColor == QColor(Qt::white),
            "legacy border defaults");
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
    require(settings.shadowColor() == QColor(0x33, 0x33, 0x33),
            "missing shadow color preferences must use the selection model default");
    require(!settings.aspectRatioLocked(),
            "new installations must default to an unlocked selection aspect ratio");
    require(settings.selectionTarget() == ScreenshotIntelligentSelectionTarget::WindowSubElement,
            "new installations must default to the existing child-element selection mode");
    require(settings.layoutCompaction() == ScreenshotLayoutCompaction{}, "compaction defaults");
    settings.setLayoutCompaction({true, 1000});
    require(settings.layoutCompaction() == ScreenshotLayoutCompaction{true, 100},
            "saved spacing must clamp to 100 canvas units");
    require(storage.configuration().setValue(
                QStringLiteral("screenshot_selection/layout_compaction_spacing"), 500) &&
                settings.layoutCompaction().spacing == 100,
            "legacy configuration spacing must clamp to the shared maximum");
    require(!storage.configuration().setValue(
                QStringLiteral("screenshot_selection/layout_compaction_spacing"), 1001),
            "configuration spacing outside the legacy range must be rejected");
    settings.setLayoutCompaction({true, 17});
    require(settings.backgroundOpacityPercent() == 0, "background opacity defaults to zero");
    settings.setBackgroundOpacityPercent(37);
    ScreenshotSelectionParams presetParams;
    presetParams.selection = QRect(10, 20, 80, 60);
    presetParams.backgroundOpacityPercent = 61;
    presetParams.borderType = ScreenshotSelectionBorderType::Border;
    presetParams.borderWidth = 7;
    presetParams.borderColor = QColor(Qt::yellow);
    settings.setSelectionBorder(presetParams.borderType, presetParams.borderWidth,
                                presetParams.borderColor);
    settings.setPresets({{QStringLiteral("Background"), presetParams}});
    require(settings.presets().first().params == presetParams && settings.borderWidth() == 7 &&
                settings.borderType() == ScreenshotSelectionBorderType::Border &&
                settings.borderColor() == QColor(Qt::yellow),
            "preferences and presets retain border settings");
    settings.setSelectionTarget(ScreenshotIntelligentSelectionTarget::Window);
    require(settings.selectionTarget() == ScreenshotIntelligentSelectionTarget::Window &&
                storage.smartSelectionMode() == snow_shot::SmartSelectionMode::Window &&
                storage.configuration().value(QStringLiteral(
                    "screenshot_selection/smart_selection")) == QStringLiteral("window"),
            "Tab target preference and smart-selection dropdown must share one stored mode");

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
        [&](const ScreenshotSelectionParams& params) { settings.setSelectionEffects(params); },
        [&](ScreenshotSelectionAspectRatioPreset preset, bool locked) {
            settings.setAspectRatioPreference(preset, locked);
        },
    });

    workflow.setSelectionCornerRadiusFromToolbar(24);
    workflow.setSelectionEffectWidthFromToolbar(12);
    workflow.toggleSelectionAspectRatioLockFromToolbar();
    require(settings.cornerRadius() == 24 && settings.shadowWidth() == 12,
            "toolbar edits must persist both effects without exporting a screenshot");
    workflow.adjustSelectionEffectsFromWheel(3, -2);
    require(settings.cornerRadius() == 27 && settings.shadowWidth() == 10,
            "wheel values must persist before their deferred preview is presented");
    workflow.adjustSelectionEffectsFromWheel(-3, 2);
    require(settings.aspectRatioLocked(),
            "the toolbar aspect-ratio toggle must persist without exporting a screenshot");
    require(storage.captureHistory().records().isEmpty(),
            "editing effects must not create a previous selection rectangle");

    // Shutdown flushes the debounced writes, just as a normal application exit does.
    storage.shutdown();
    require(storage.initialize(options).success, "storage must reopen after shutdown");
    ScreenshotSelectionSettingsStore restartedSettings;
    require(restartedSettings.backgroundOpacityPercent() == 37,
            "background opacity survives restart");
    require(restartedSettings.presets().size() == 1 &&
                restartedSettings.presets().first().params.backgroundOpacityPercent == 61,
            "named presets preserve their own background opacity across restarts");
    require(restartedSettings.layoutCompaction() == ScreenshotLayoutCompaction{true, 17},
            "compaction settings must survive a restart");
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
    params.shadowColor = QColor(12, 34, 56, 78);
    applyResize(params);
    require(selection.shadowColor() == params.shadowColor,
            "confirmed resize parameters must apply the shadow color to the active selection");
    require(storage.configuration()
                    .value(QStringLiteral("screenshot_selection/shadow_color"))
                    .toString() == QStringLiteral("#0C22384E"),
            "confirmed resize parameters must persist the shadow color without exporting");
    require(settings.cornerRadius() == 32 && settings.shadowWidth() == 16,
            "confirmed resize parameters must update the persistent effects");
    require(storage.captureHistory().records().isEmpty(),
            "persisting the shadow color must not require a previous screenshot");

    storage.shutdown();
    require(storage.initialize(options).success, "custom shadow color must reload");
    require(settings.shadowColor() == params.shadowColor,
            "the chosen shadow color including alpha must survive a storage restart");

    workflow.openSelectionResizeModalFromToolbar();
    applyResize = {};
    require(settings.shadowColor() == params.shadowColor,
            "cancelling the resize dialog must preserve the saved shadow color");

    workflow.openSelectionResizeModalFromToolbar();
    params.shadowColor = QColor(90, 120, 150);
    applyResize(params);
    require(settings.shadowColor() == params.shadowColor && settings.cornerRadius() == 32 &&
                settings.shadowWidth() == 16,
            "a color-only dialog edit must persist without changing the other effects");

    workflow.setSelectionCornerRadiusFromToolbar(1000);
    workflow.setSelectionEffectWidthFromToolbar(1000);
    require(settings.cornerRadius() == 256 && settings.shadowWidth() == 64,
            "toolbar edits must persist the clamped effect values");
    require(settings.shadowColor() == params.shadowColor,
            "radius and shadow width edits must retain the chosen shadow color");
    auto& configuration = storage.configuration();
    require(!configuration.setValue(QStringLiteral("screenshot_selection/corner_radius"),
                                    QStringLiteral("invalid")) &&
                !configuration.setValue(QStringLiteral("screenshot_selection/shadow_width"), 1.5) &&
                !configuration.setValue(QStringLiteral("screenshot_selection/lock_aspect_ratio"),
                                        QStringLiteral("invalid")),
            "persistent selection settings must reject values of the wrong type");
    require(!configuration.setValue(QStringLiteral("screenshot_selection/shadow_color"), 42) &&
                !configuration.setValue(QStringLiteral("screenshot_selection/shadow_color"),
                                        QStringLiteral("invalid")) &&
                settings.shadowColor() == params.shadowColor,
            "invalid shadow color preferences must be rejected without overwriting the color");

    workflow.setSelectionCornerRadiusFromToolbar(-1);
    workflow.setSelectionEffectWidthFromToolbar(-1);
    storage.shutdown();
    require(storage.initialize(options).success, "disabled effects must reload");
    require(settings.cornerRadius() == 0 && settings.shadowWidth() == 0,
            "turning effects off must survive a restart");
    require(settings.shadowColor() == params.shadowColor,
            "disabling the shadow must retain its color for the next time it is enabled");
    require(!settings.aspectRatioLocked(),
            "turning off the aspect-ratio lock must survive a restart");

    settings.setSelectionEffects(20, 10, QColor(12, 34, 56));
    settings.setAspectRatioLocked(true);
    settings.clear();
    require(settings.backgroundOpacityPercent() == 0,
            "clearing selection preferences resets background opacity");
    require(settings.cornerRadius() == 0 && settings.shadowWidth() == 0,
            "clearing selection settings must reset the persistent effects");
    require(settings.shadowColor() == QColor(0x33, 0x33, 0x33),
            "clearing selection settings must reset the shadow color");
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
        [&](const ScreenshotSelectionParams& params) { settings.setSelectionEffects(params); },
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
    require(storage.captureHistory().records().isEmpty(),
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
void backgroundOpacitySelectionState() {
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(10, 20, 80, 60));
    require(selection.backgroundOpacityPercent() == 0, "selection opacity defaults to zero");
    const auto revision = selection.changeRevision();
    require(selection.setBackgroundOpacityPercent(37) && selection.changeRevision() == revision + 1,
            "opacity changes invalidate selection state once");
    require(!selection.setBackgroundOpacityPercent(37) &&
                selection.changeRevision() == revision + 1,
            "identical opacity must not invalidate state");
    const auto params = selection.params(QRect(0, 0, 200, 200));
    ScreenshotSelectionModel restored;
    require(restored.applyParams(params, QRect(0, 0, 200, 200)) &&
                restored.backgroundOpacityPercent() == 37 &&
                restored.resultStyle().backgroundOpacityPercent == 37,
            "history and preset restoration must carry opacity into the result style");
    snow_shot::storage::PersistedSelection persisted{
        params.selection,   params.radius,           params.shadowWidth,
        params.shadowColor, params.lockAspectRatio,  params.lockDragAspectRatio,
        params.region,      params.layoutCompaction, params.backgroundOpacityPercent};
    auto json = snow_shot::storage::persistedSelectionToJson(persisted);
    require(snow_shot::storage::normalizePersistedSelection(json).value == persisted,
            "persisted selection opacity must round trip");
    json.remove(QStringLiteral("background_opacity_percent"));
    require(snow_shot::storage::normalizePersistedSelection(json).value.backgroundOpacityPercent ==
                0,
            "legacy selections default to zero background opacity");
    for (double invalid : {-1.0, .5, 101.0}) {
        json.insert(QStringLiteral("background_opacity_percent"), invalid);
        require(!snow_shot::storage::normalizePersistedSelection(json).valid,
                "invalid persisted opacity must be rejected");
    }
    require(restored.setBackgroundOpacityPercent(200) && restored.backgroundOpacityPercent() == 100,
            "opacity clamps at 100 percent");
    ScreenshotSelectionModel copied;
    copied = restored;
    require(copied.backgroundOpacityPercent() == 100, "selection assignment preserves opacity");
    copied.reset();
    require(copied.backgroundOpacityPercent() == 0, "selection reset clears opacity");
}

void compactionSelectionState() {
    ScreenshotSelectionModel selection;
    selection.setSelectionRect(QRectF(0, 0, 20, 20));
    selection.setLayoutCompaction({true, 8});
    require(!selection.layoutCompactionAvailable() && !selection.exportLayoutCompaction().enabled,
            "single rectangles must not compact");
    selection.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Add);
    selection.setSelectionRect(QRectF(50, 0, 20, 20));
    require(!selection.layoutCompactionAvailable(),
            "unfinished multi-region operations are unavailable");
    selection.commitRegionOperation();
    require(selection.layoutCompactionAvailable() && selection.exportLayoutCompaction().enabled,
            "committed regions enable compaction");
    auto reduced = selection;
    reduced.beginRegionOperation(ScreenshotSelectionModel::RegionOperation::Subtract);
    reduced.setSelectionRect(QRectF(50, 0, 20, 20));
    reduced.commitRegionOperation();
    require(!reduced.layoutCompactionAvailable(), "removed regions must not retain eligibility");
    const auto params = selection.params(QRect(0, 0, 100, 100));
    ScreenshotSelectionModel restored;
    require(restored.applyParams(params, QRect(0, 0, 100, 100)) &&
                restored.layoutCompactionAvailable() &&
                restored.layoutCompaction() == params.layoutCompaction,
            "history restoration must retain compaction and multi-region eligibility");
    const auto revision = restored.changeRevision();
    require(!restored.setLayoutCompaction(params.layoutCompaction) &&
                restored.changeRevision() == revision,
            "identical settings must not invalidate selection presentation");
    snow_shot::storage::PersistedSelection persisted{
        params.selection,   params.radius,          params.shadowWidth,
        params.shadowColor, params.lockAspectRatio, params.lockDragAspectRatio,
        params.region,      params.layoutCompaction};
    auto json = snow_shot::storage::persistedSelectionToJson(persisted);
    require(snow_shot::storage::normalizePersistedSelection(json).value == persisted,
            "compaction history JSON must round trip");
    json.insert(QStringLiteral("layout_compaction_spacing"), 1000);
    const auto clamped = snow_shot::storage::normalizePersistedSelection(json);
    require(clamped.valid && clamped.changed &&
                clamped.value.layoutCompaction == ScreenshotLayoutCompaction{true, 100} &&
                clamped.value.rectangle == persisted.rectangle &&
                clamped.value.region == persisted.region,
            "legacy spacing above 100 must clamp without discarding the saved selection");
    json.remove(QStringLiteral("layout_compaction_enabled"));
    json.remove(QStringLiteral("layout_compaction_spacing"));
    const auto legacy = snow_shot::storage::normalizePersistedSelection(json);
    require(legacy.valid && legacy.value.layoutCompaction == ScreenshotLayoutCompaction{},
            "legacy history must default to disabled compaction");
    json.insert(QStringLiteral("layout_compaction_spacing"), 1.5);
    require(!snow_shot::storage::normalizePersistedSelection(json).valid,
            "fractional persisted spacing rejected");
    restored.reset();
    require(restored.layoutCompaction() == ScreenshotLayoutCompaction{},
            "capture reset clears local settings");
    QRegion ring(QRect(0, 0, 40, 40));
    ring -= QRect(10, 10, 20, 20);
    restored.setSelectionRegion(ring);
    require(!restored.layoutCompactionAvailable(),
            "scanline partitions and holes are not multiple selections");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    selectionResultStylesUseTheCropOrigin();
    borderStateAndPersistence();
    backgroundOpacitySelectionState();
    compactionSelectionState();
    selectionEffectsPersistAcrossRestarts();
    selectionAspectRatioWorkflowPersistsOnlyUserPreferences();
    return 0;
}
