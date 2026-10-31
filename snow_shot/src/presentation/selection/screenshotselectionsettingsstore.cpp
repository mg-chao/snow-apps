#include "snow_shot/presentation/screenshotselectionsettingsstore.h"

#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/settingsadapters.h"

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
    return {params.selection,   params.radius,           params.shadowWidth,
            params.shadowColor, params.lockAspectRatio,  params.lockDragAspectRatio,
            params.region,      params.layoutCompaction, params.backgroundOpacityPercent,
            params.borderType,  params.borderWidth,      params.borderColor};
}

std::optional<ScreenshotSelectionParams> selectionFromJson(const QJsonValue& value) {
    const auto normalized = storage::normalizePersistedSelection(value);
    if (!normalized.valid) {
        return std::nullopt;
    }
    const storage::PersistedSelection& selection = normalized.value;
    return ScreenshotSelectionParams{
        selection.rectangle,   selection.cornerRadius,     selection.shadowWidth,
        selection.shadowColor, selection.lockAspectRatio,  selection.lockDragAspectRatio,
        selection.region,      selection.layoutCompaction, selection.backgroundOpacityPercent,
        selection.borderType,  selection.borderWidth,      selection.borderColor};
}
} // namespace

ScreenshotSelectionSettingsStore::ScreenshotSelectionSettingsStore() = default;

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

QColor ScreenshotSelectionSettingsStore::shadowColor() const {
    return storage::colorFromRgbaString(
        configuration().value(QStringLiteral("screenshot_selection/shadow_color")).toString());
}

ScreenshotSelectionBorderType ScreenshotSelectionSettingsStore::borderType() const {
    return configuration().value(QStringLiteral("screenshot_selection/border_type")) ==
                   QStringLiteral("border")
               ? ScreenshotSelectionBorderType::Border
               : ScreenshotSelectionBorderType::Shadow;
}
int ScreenshotSelectionSettingsStore::borderWidth() const {
    return configuration().value(QStringLiteral("screenshot_selection/border_width")).toInt();
}
QColor ScreenshotSelectionSettingsStore::borderColor() const {
    return storage::colorFromRgbaString(
        configuration().value(QStringLiteral("screenshot_selection/border_color")).toString());
}
void ScreenshotSelectionSettingsStore::setSelectionBorder(ScreenshotSelectionBorderType type,
                                                          int width, const QColor& color) {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/border_type"), screenshotSelectionBorderTypeId(type)},
        {QStringLiteral("screenshot_selection/border_width"), width},
        {QStringLiteral("screenshot_selection/border_color"), storage::colorToRgbaString(color)},
    }));
}

void ScreenshotSelectionSettingsStore::setSelectionEffects(
    const ScreenshotSelectionParams& params) {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/corner_radius"), params.radius},
        {QStringLiteral("screenshot_selection/shadow_width"), params.shadowWidth},
        {QStringLiteral("screenshot_selection/shadow_color"),
         storage::colorToRgbaString(params.shadowColor)},
        {QStringLiteral("screenshot_selection/border_type"),
         screenshotSelectionBorderTypeId(params.borderType)},
        {QStringLiteral("screenshot_selection/border_width"), params.borderWidth},
        {QStringLiteral("screenshot_selection/border_color"),
         storage::colorToRgbaString(params.borderColor)},
    }));
}

void ScreenshotSelectionSettingsStore::setSelectionEffects(int cornerRadius, int shadowWidth,
                                                           const QColor& shadowColor) {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/corner_radius"), cornerRadius},
        {QStringLiteral("screenshot_selection/shadow_width"), shadowWidth},
        {QStringLiteral("screenshot_selection/shadow_color"),
         storage::colorToRgbaString(shadowColor)},
    }));
}

ScreenshotSelectionAspectRatioPreset ScreenshotSelectionSettingsStore::aspectRatioPreset() const {
    return screenshotSelectionAspectRatioPresetFromId(
        configuration().value(QStringLiteral("screenshot_selection/aspect_ratio")).toString());
}

bool ScreenshotSelectionSettingsStore::aspectRatioLocked() const {
    return aspectRatioPreset() != ScreenshotSelectionAspectRatioPreset::Free ||
           configuration().value(QStringLiteral("screenshot_selection/lock_aspect_ratio")).toBool();
}

void ScreenshotSelectionSettingsStore::setAspectRatioPreference(
    ScreenshotSelectionAspectRatioPreset preset, bool locked) {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/aspect_ratio"),
         screenshotSelectionAspectRatioPresetId(preset)},
        {QStringLiteral("screenshot_selection/lock_aspect_ratio"),
         locked || preset != ScreenshotSelectionAspectRatioPreset::Free},
    }));
}

void ScreenshotSelectionSettingsStore::setAspectRatioLocked(bool locked) {
    setAspectRatioPreference(
        locked ? aspectRatioPreset() : ScreenshotSelectionAspectRatioPreset::Free, locked);
}

ScreenshotIntelligentSelectionTarget ScreenshotSelectionSettingsStore::selectionTarget() const {
    return configuration().value(QStringLiteral("screenshot_selection/smart_selection")) ==
                   QStringLiteral("window")
               ? ScreenshotIntelligentSelectionTarget::Window
               : ScreenshotIntelligentSelectionTarget::WindowSubElement;
}

void ScreenshotSelectionSettingsStore::setSelectionTarget(
    ScreenshotIntelligentSelectionTarget target) {
    static_cast<void>(
        configuration().setValue(QStringLiteral("screenshot_selection/smart_selection"),
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

ScreenshotLayoutCompaction ScreenshotSelectionSettingsStore::layoutCompaction() const {
    return {configuration()
                .value(QStringLiteral("screenshot_selection/layout_compaction_enabled"))
                .toBool(),
            configuration()
                .value(QStringLiteral("screenshot_selection/layout_compaction_spacing"))
                .toInt()};
}

void ScreenshotSelectionSettingsStore::setLayoutCompaction(ScreenshotLayoutCompaction options) {
    options = options.normalized();
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/layout_compaction_enabled"), options.enabled},
        {QStringLiteral("screenshot_selection/layout_compaction_spacing"), options.spacing},
    }));
}

int ScreenshotSelectionSettingsStore::backgroundOpacityPercent() const {
    return configuration()
        .value(QStringLiteral("screenshot_selection/background_opacity_percent"))
        .toInt();
}

void ScreenshotSelectionSettingsStore::setBackgroundOpacityPercent(int percent) {
    static_cast<void>(
        configuration().setValue(QStringLiteral("screenshot_selection/background_opacity_percent"),
                                 std::clamp(percent, 0, 100)));
}

void ScreenshotSelectionSettingsStore::clear() {
    static_cast<void>(configuration().setValues({
        {QStringLiteral("screenshot_selection/selection_rect_presets"), QJsonArray()},
        {QStringLiteral("screenshot_selection/background_opacity_percent"), 0},
        {QStringLiteral("screenshot_selection/layout_compaction_enabled"), false},
        {QStringLiteral("screenshot_selection/layout_compaction_spacing"), 0},
        {QStringLiteral("screenshot_selection/corner_radius"), 0},
        {QStringLiteral("screenshot_selection/border_type"), QStringLiteral("shadow")},
        {QStringLiteral("screenshot_selection/border_width"), 0},
        {QStringLiteral("screenshot_selection/border_color"), QStringLiteral("#FFFFFFFF")},
        {QStringLiteral("screenshot_selection/shadow_width"), 0},
        {QStringLiteral("screenshot_selection/shadow_color"), QStringLiteral("#333333FF")},
        {QStringLiteral("screenshot_selection/aspect_ratio"), QStringLiteral("free")},
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
