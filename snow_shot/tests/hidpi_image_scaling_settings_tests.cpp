// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/directcaptureworkflow.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void catalogAndPersistence(const QString& path) {
    const auto& catalog = settings::builtInSettingsRegistry().catalog();
    require(catalog.validationErrors().isEmpty(), "settings catalog must remain valid");
    const auto* section =
        catalog.section(QStringLiteral("screenshots"), QStringLiteral("screenshot-settings"));
    const auto* item =
        catalog.item({QStringLiteral("screenshots"), QStringLiteral("screenshot-settings"),
                      QStringLiteral("screenshot.hidpi-image-scaling")});
    require(section && item &&
                section->items.at(5).id ==
                    QStringLiteral("screenshot.quick-selection-modification") &&
                section->items.at(6).id == item->id &&
                item->title.translated() == QStringLiteral("HiDPI image scaling"),
            "HiDPI image scaling must immediately follow quick selection modification");
    const auto& select = std::get<settings::SettingsSelectDefinition>(item->payload);
    constexpr auto binding = settings::SettingsSelectBinding::ScreenshotHiDpiImageScaling;
    require(select.binding == binding && select.options.size() == 12 &&
                item->configurationKey == QStringLiteral("screenshot/hidpi_image_scaling"),
            "HiDPI scaling must expose twelve persisted Select choices");
    const QStringList labels = {QStringLiteral("Disabled"),
                                QStringLiteral("Auto"),
                                QStringLiteral("Scale down to 80% (125%)"),
                                QStringLiteral("Scale down to 66.7% (150%)"),
                                QStringLiteral("Scale down to 57.1% (175%)"),
                                QStringLiteral("Scale down to 50% (200%)"),
                                QStringLiteral("Scale down to 44.4% (225%)"),
                                QStringLiteral("Scale down to 40% (250%)"),
                                QStringLiteral("Scale down to 36.4% (275%)"),
                                QStringLiteral("Scale down to 33.3% (300%)"),
                                QStringLiteral("Scale down to 30.8% (325%)"),
                                QStringLiteral("Scale down to 28.6% (350%)")};
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const storage::ScreenshotSettings screenshot;
    require(backend.selectValue(binding) == QStringLiteral("disabled") &&
                screenshot.hiDpiImageScaling() == QStringLiteral("disabled"),
            "HiDPI scaling must default to Disabled");
    for (qsizetype i = 0; i < select.options.size(); ++i) {
        const QString value = i == 0   ? QStringLiteral("disabled")
                              : i == 1 ? QStringLiteral("auto")
                                       : QString::number(125 + (i - 2) * 25);
        require(
            select.options[i].value == value && select.options[i].label.translated() == labels[i] &&
                backend.applySelectValue(binding, value) &&
                screenshot.hiDpiImageScaling() == value && backend.selectValue(binding) == value,
            "every ordered label must map to its accepted display percentage");
    }
    require(!backend.applySelectValue(binding, QStringLiteral("66.7")) &&
                !screenshot.setHiDpiImageScaling(QStringLiteral("175%")) &&
                screenshot.hiDpiImageScaling() == QStringLiteral("350"),
            "invalid choices must preserve the accepted value");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "HiDPI configuration must flush");
    storage::ConfigurationStore reloaded(path, true, true, 60000);
    require(reloaded.value(item->configurationKey) == QStringLiteral("350"),
            "HiDPI scaling must survive reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings) &&
                screenshot.hiDpiImageScaling() == QStringLiteral("disabled"),
            "Screenshot settings reset must restore Disabled");
    QFile invalid(path + QStringLiteral(".invalid"));
    require(invalid.open(QIODevice::WriteOnly) &&
                invalid.write("{\"screenshot\":{\"hidpi_image_scaling\":\"66.7\"}}") > 0,
            "invalid persisted setting fixture must open");
    invalid.close();
    storage::ConfigurationStore repaired(invalid.fileName(), true, true, 60000);
    require(repaired.value(item->configurationKey) == QStringLiteral("disabled"),
            "invalid stored HiDPI values must fall back to Disabled");
}

void fixedScalingUsesDisplayPercentages() {
    const QSize expected[] = {{8000, 3600}, {6667, 3000}, {5714, 2571}, {5000, 2250}, {4444, 2000},
                              {4000, 1800}, {3636, 1636}, {3333, 1500}, {3077, 1385}, {2857, 1286}};
    const QVector<ScreenshotHiDpiDisplay> mixed = {{{-100, 0, 100, 100}, 1.25},
                                                   {{0, 0, 100, 100}, 2.0}};
    for (int i = 0; i < 10; ++i) {
        const QString mode = QString::number(125 + i * 25);
        const qreal ratio = screenshotHiDpiImageScale(mode, QRectF(-50, 10, 100, 50), mixed);
        require(screenshotHiDpiImageSize({10000, 4500}, ratio) == expected[i],
                "fixed scaling must use the display percentage, even with mixed DPI");
        require(screenshotHiDpiImageSize({1, 1}, ratio) == QSize(1, 1),
                "downscaling tiny selections must retain at least one pixel");
    }
    require(screenshotHiDpiImageSize({101, 51}, 0.5) == QSize(51, 26),
            "fractional output dimensions round to the nearest pixel");
    require(screenshotHiDpiImageScale(u"disabled", {}, mixed) == 1.0 &&
                screenshotHiDpiImageScale(u"invalid", {}, mixed) == 1.0,
            "Disabled and unsupported modes must preserve pixels");
}

void autoUsesOnlyIntersectingMonitors() {
    QVector<ScreenshotHiDpiDisplay> displays = {
        {{-100, -50, 100, 100}, 1.5}, {{0, -50, 100, 100}, 1.5}, {{100, -50, 100, 100}, 2.25}};
    require(
        qFuzzyCompare(screenshotHiDpiImageScale(u"auto", {-90, 0, 50, 20}, displays), 2.0 / 3.0),
        "Auto must use the selected monitor and ignore unrelated DPI values");
    require(
        qFuzzyCompare(screenshotHiDpiImageScale(u"auto", {-10, 0, 110, 20}, displays), 2.0 / 3.0),
        "equal-DPI monitors may downscale, including a boundary-only third monitor");
    require(screenshotHiDpiImageScale(u"auto", {90, 0, 20, 20}, displays) == 1.0,
            "Auto must preserve mixed-DPI selections");
    require(screenshotHiDpiImageScale(u"auto", {500, 0, 20, 20}, displays) == 1.0 &&
                screenshotHiDpiImageScale(u"auto", {}, displays) == 1.0,
            "Auto must preserve regions without intersecting monitors");
    for (const qreal invalid : {0.0, -1.0, std::numeric_limits<qreal>::infinity(),
                                std::numeric_limits<qreal>::quiet_NaN()}) {
        displays[0].scale = invalid;
        require(screenshotHiDpiImageScale(u"auto", {-10, 0, 20, 20}, displays) == 1.0,
                "unknown monitor scaling must preserve the full selection");
    }
    displays[0].scale = 1.5 + 1e-14;
    require(
        qFuzzyCompare(screenshotHiDpiImageScale(u"auto", {-10, 0, 20, 20}, displays), 2.0 / 3.0),
        "Auto must tolerate floating-point noise in equal monitor factors");
    displays[0].scale = 0.75;
    require(screenshotHiDpiImageScale(u"auto", {-90, 0, 20, 20}, displays) == 1.0,
            "Auto must never upscale");
}

void directCaptureUsesCapturedBounds() {
    using namespace snow_shot::presentation;
    DirectCaptureRequest request;
    request.hiDpiImageScaling = QStringLiteral("auto");
    request.scalingDisplays = {{{-100, 0, 100, 100}, 1.25}, {{0, 0, 100, 100}, 2.0}};
    DirectCaptureFrame frame;
    frame.physicalBounds = {-80, 10, 50, 20};
    require(directCaptureImageScale(request, frame) == 0.8,
            "direct captures must use their actual bounds instead of the cursor monitor");
    frame.physicalBounds = {-10, 10, 50, 20};
    require(directCaptureImageScale(request, frame) == 1.0,
            "a direct window crossing different DPI monitors must remain unscaled in Auto");
    frame.logicalBounds = {10, 10, 50, 20};
    require(directCaptureImageScale(request, frame) == 0.5,
            "point-based captures must intersect logical monitor bounds");
    request.hiDpiImageScaling = QStringLiteral("225");
    require(screenshotHiDpiImageSize({4500, 2250}, directCaptureImageScale(request, frame)) ==
                QSize(2000, 1000),
            "fixed direct capture scaling must use the exact reciprocal");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated settings directory must exist");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({temporary.filePath(QStringLiteral("bin")),
                                       temporary.filePath(QStringLiteral("data")), 60000}));
    catalogAndPersistence(temporary.filePath(QStringLiteral("data/config.json")));
    fixedScalingUsesDisplayPercentages();
    autoUsesOnlyIntersectingMonitors();
    directCaptureUsesCapturedBounds();
    applicationStorage.shutdown();
    return EXIT_SUCCESS;
}
