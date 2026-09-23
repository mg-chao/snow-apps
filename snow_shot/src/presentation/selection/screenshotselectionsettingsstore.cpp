#include "snow_shot/presentation/screenshotselectionsettingsstore.h"

#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/persistedselectioncodec.h"

#include <QJsonArray>

#include <optional>

namespace {
namespace storage = snow_shot::storage;
snow_shot::storage::ConfigurationStore& configuration() {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!storage.isInitialized()) {
        static_cast<void>(storage.initialize());
    }
    return storage.configuration();
}

storage::PersistedSelection persistedSelection(const ScreenshotSelectionParams& params) {
    return {params.selection,   params.radius,          params.shadowWidth,
            params.shadowColor, params.lockAspectRatio, params.lockDragAspectRatio,
            params.region};
}

std::optional<ScreenshotSelectionParams> selectionFromJson(const QJsonValue& value) {
    const auto normalized = storage::normalizePersistedSelection(value);
    if (!normalized.valid) {
        return std::nullopt;
    }
    const storage::PersistedSelection& selection = normalized.value;
    return ScreenshotSelectionParams{selection.rectangle,       selection.cornerRadius,
                                     selection.shadowWidth,     selection.shadowColor,
                                     selection.lockAspectRatio, selection.lockDragAspectRatio,
                                     selection.region};
}
} // namespace

ScreenshotSelectionSettingsStore::ScreenshotSelectionSettingsStore() = default;

bool ScreenshotSelectionSettingsStore::hasPreviousSelectionParams() const {
    return selectionFromJson(
               configuration().value(QStringLiteral("screenshot_selection/previous_selection")))
        .has_value();
}

ScreenshotSelectionParams ScreenshotSelectionSettingsStore::previousSelectionParams() const {
    return selectionFromJson(
               configuration().value(QStringLiteral("screenshot_selection/previous_selection")))
        .value_or(ScreenshotSelectionParams{});
}

void ScreenshotSelectionSettingsStore::setPreviousSelectionParams(
    const ScreenshotSelectionParams& params) {
    static_cast<void>(
        configuration().setValue(QStringLiteral("screenshot_selection/previous_selection"),
                                 storage::persistedSelectionToJson(persistedSelection(params))));
}

QVector<ScreenshotSelectionPreset> ScreenshotSelectionSettingsStore::presets() const {
    QVector<ScreenshotSelectionPreset> result;
    const QJsonArray array =
        configuration()
            .value(QStringLiteral("screenshot_selection/selection_rect_presets"))
            .toArray();
    for (const QJsonValue& item : array) {
        const auto params = selectionFromJson(item);
        if (params.has_value()) {
            result.push_back({item.toObject().value(QStringLiteral("name")).toString(), *params});
        }
    }
    return result;
}

int ScreenshotSelectionSettingsStore::cornerRadius() const {
    return configuration().value(QStringLiteral("screenshot_selection/corner_radius")).toInt();
}

int ScreenshotSelectionSettingsStore::shadowWidth() const {
    return configuration().value(QStringLiteral("screenshot_selection/shadow_width")).toInt();
}

void ScreenshotSelectionSettingsStore::setSelectionEffects(int cornerRadius, int shadowWidth) {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/corner_radius"), cornerRadius},
        {QStringLiteral("screenshot_selection/shadow_width"), shadowWidth},
    }));
}

bool ScreenshotSelectionSettingsStore::aspectRatioLocked() const {
    return configuration().value(QStringLiteral("screenshot_selection/lock_aspect_ratio")).toBool();
}

void ScreenshotSelectionSettingsStore::setAspectRatioLocked(bool locked) {
    static_cast<void>(
        configuration().setValue(QStringLiteral("screenshot_selection/lock_aspect_ratio"), locked));
}

ScreenshotIntelligentSelectionTarget ScreenshotSelectionSettingsStore::selectionTarget() const {
    return configuration().value(QStringLiteral("screenshot_selection/selection_target")) ==
                   QStringLiteral("window")
               ? ScreenshotIntelligentSelectionTarget::Window
               : ScreenshotIntelligentSelectionTarget::WindowSubElement;
}

void ScreenshotSelectionSettingsStore::setSelectionTarget(
    ScreenshotIntelligentSelectionTarget target) {
    static_cast<void>(
        configuration().setValue(QStringLiteral("screenshot_selection/selection_target"),
                                 target == ScreenshotIntelligentSelectionTarget::Window
                                     ? QStringLiteral("window")
                                     : QStringLiteral("window_sub_element")));
}

void ScreenshotSelectionSettingsStore::setPresets(
    const QVector<ScreenshotSelectionPreset>& presets) {
    QJsonArray array;
    for (const ScreenshotSelectionPreset& preset : presets) {
        auto rectangleParams = preset.params;
        rectangleParams.region.reset();
        QJsonObject object = storage::persistedSelectionToJson(persistedSelection(rectangleParams));
        object.insert(QStringLiteral("name"), preset.name);
        array.push_back(object);
    }
    static_cast<void>(configuration().setValue(
        QStringLiteral("screenshot_selection/selection_rect_presets"), array));
}

void ScreenshotSelectionSettingsStore::clear() {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/previous_selection"), QJsonValue::Null},
        {QStringLiteral("screenshot_selection/selection_rect_presets"), QJsonArray()},
        {QStringLiteral("screenshot_selection/corner_radius"), 0},
        {QStringLiteral("screenshot_selection/shadow_width"), 0},
        {QStringLiteral("screenshot_selection/lock_aspect_ratio"), false},
    }));
}

ScreenshotRegionType ScreenshotSelectionSettingsStore::regionType() const {
    return screenshotRegionTypeFromId(
        configuration().value(QStringLiteral("screenshot_selection/region_type")).toString());
}
void ScreenshotSelectionSettingsStore::setRegionType(ScreenshotRegionType type) {
    static_cast<void>(configuration().setValue(QStringLiteral("screenshot_selection/region_type"),
                                               screenshotRegionTypeId(type)));
}
