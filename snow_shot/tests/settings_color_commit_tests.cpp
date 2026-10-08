#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "theme/theme_manager.h"
#include "widgets/color_picker.h"

#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTranslator>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class PresetTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QString::fromLatin1(context) == QStringLiteral("SettingsCatalog") &&
            QString::fromUtf8(source) == QStringLiteral("Stroke color preset %1")) {
            return QStringLiteral("Localized stroke preset %1");
        }
        return {};
    }
};

void settingsColorPalettesCommitOnPopupClose(QApplication& application) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    require(registry.isValid(), "palette settings must preserve a valid registry");
    settings::SettingsRuntimeSession session(registry, backend);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    QString pageId;
    std::unique_ptr<SettingsPageWidget> page;
    for (const auto binding : {settings::SettingsColorPaletteBinding::StrokeColors,
                               settings::SettingsColorPaletteBinding::FillColors}) {
        const auto* descriptor = registry.fieldForColorPalette(binding);
        require(descriptor != nullptr &&
                    descriptor->kind == settings::SettingsFieldKind::ColorPalette &&
                    descriptor->defaultValue.toArray().size() == 5,
                "each quick-set palette has five persisted slots in the settings registry");
        if (!page || pageId != descriptor->pageId) {
            pageId = descriptor->pageId;
            page = std::make_unique<SettingsPageWidget>(registry, pageId, session);
            page->resize(880, 760);
            page->show();
        }
        page->reveal({descriptor->pageId, descriptor->sectionId, descriptor->id});
        application.processEvents();
        auto* row = page->findChild<QWidget*>(
            settings::generatedObjectName(QStringLiteral("settings-item"), descriptor->id));
        require(row != nullptr, "palette setting row is materialized when revealed");
        const auto pickers = row->findChildren<adqt::widgets::AdColorPicker*>();
        require(pickers.size() == 5, "each palette setting displays five color picker buttons");
        const auto original = backend.colorPaletteValue(binding);
        require(original.size() == 5, "backend supplies five initial palette colors");
        int previousRight = -1;
        int top = -1;
        for (int index = 0; index < pickers.size(); ++index) {
            auto* picker = pickers.at(index);
            const QPoint position = picker->mapTo(row, QPoint());
            require(!picker->triggerTextVisible() && picker->alphaChannelEnabled() &&
                        !picker->allowClear() && picker->value().solidColor == original.at(index),
                    "preset slots use single-button color pickers with alpha and current values");
            require(!picker->accessibleName().isEmpty() &&
                        (index == 0 ||
                         picker->accessibleName() != pickers.at(index - 1)->accessibleName()),
                    "each slot has a distinct accessible name");
            require(index == 0 || (position.y() == top && position.x() >= previousRight),
                    "the five picker buttons are displayed side by side without overlap");
            top = position.y();
            previousRight = position.x() + picker->width();
        }
        int writes = 0;
        const auto connection =
            QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, page.get(),
                             [&writes, key = descriptor->configurationKey](const QString& changed) {
                                 if (changed == key)
                                     ++writes;
                             });
        auto* picker = pickers.at(2);
        picker->setPopupVisible(true);
        application.processEvents();
        require(picker->popupVisible(), "preset color popup opens offscreen");
        const QColor intermediate(34, 81, 129, 112);
        const QColor finalColor(65, 119, 179, 144);
        for (const QColor& color : {intermediate, finalColor}) {
            picker->commitValue(adqt::widgets::AdColorValue::solid(color));
            application.processEvents();
            require(picker->value().solidColor == color && writes == 0 &&
                        backend.colorPaletteValue(binding) == original,
                    "editing a preset previews locally until its popup closes");
        }
        picker->setPopupVisible(false);
        application.processEvents();
        auto expected = original;
        expected[2] = finalColor;
        require(writes == 1 && backend.colorPaletteValue(binding) == expected,
                "closing a preset popup persists its final RGBA value once and preserves siblings");
        require(configuration.value(descriptor->configurationKey).toArray().at(2).toString() ==
                    storage::colorToRgbaString(finalColor),
                "custom preset alpha survives the persisted configuration representation");
        picker->setPopupVisible(true);
        picker->setPopupVisible(false);
        application.processEvents();
        require(writes == 1, "an unchanged preset popup does not save again");
        expected[0] = QColor(71, 121, 171, 0);
        require(backend.applyColorPaletteValue(binding, expected), "externally update the palette");
        application.processEvents();
        require(pickers.at(0)->value().solidColor == expected.at(0) &&
                    pickers.at(2)->value().solidColor == finalColor,
                "settings picker buttons react to external palette changes");
        if (binding == settings::SettingsColorPaletteBinding::StrokeColors) {
            PresetTranslator translator;
            application.installTranslator(&translator);
            QEvent languageChange(QEvent::LanguageChange);
            application.sendEvent(page.get(), &languageChange);
            application.processEvents();
            require(pickers.at(0)->accessibleName() == QStringLiteral("Localized stroke preset 1"),
                    "preset slot accessible names retranslate on language changes");
            application.removeTranslator(&translator);
            application.sendEvent(page.get(), &languageChange);
        }
        QObject::disconnect(connection);
    }
    require(configuration.flushNow().success, "flush custom palette configuration to disk");
    page.reset();
    page = std::make_unique<SettingsPageWidget>(registry, pageId, session);
    page->resize(880, 760);
    page->show();
    for (const auto binding : {settings::SettingsColorPaletteBinding::StrokeColors,
                               settings::SettingsColorPaletteBinding::FillColors}) {
        const auto* descriptor = registry.fieldForColorPalette(binding);
        page->reveal({descriptor->pageId, descriptor->sectionId, descriptor->id});
        application.processEvents();
        auto* row = page->findChild<QWidget*>(
            settings::generatedObjectName(QStringLiteral("settings-item"), descriptor->id));
        const auto pickers = row->findChildren<adqt::widgets::AdColorPicker*>();
        require(pickers.at(2)->value().solidColor == QColor(65, 119, 179, 144),
                "reopening settings restores custom palette values");
    }
    const auto* stroke =
        registry.fieldForColorPalette(settings::SettingsColorPaletteBinding::StrokeColors);
    require(session.reset(stroke->reset),
            "reset the annotation settings including custom palettes");
    application.processEvents();
    for (const auto binding : {settings::SettingsColorPaletteBinding::StrokeColors,
                               settings::SettingsColorPaletteBinding::FillColors}) {
        const auto* descriptor = registry.fieldForColorPalette(binding);
        require(configuration.value(descriptor->configurationKey) == descriptor->defaultValue,
                "section reset restores all five original quick-set colors");
    }
}

void settingsColorsCommitOnPopupClose(QApplication& application) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    auto& theme = styles::ThemeManager::instance();
    std::unique_ptr<SettingsPageWidget> page;
    QString colorPageId;
    int tested = 0;
    for (const auto& field : registry.fields()) {
        if (field.kind != settings::SettingsFieldKind::Color) {
            continue;
        }
        std::cout << "Checking " << field.id.toStdString() << '\n';
        const auto& definition =
            std::get<settings::SettingsColorDefinition>(field.definition->payload);
        if (!page || colorPageId != field.pageId) {
            colorPageId = field.pageId;
            page = std::make_unique<SettingsPageWidget>(registry, colorPageId, session);
            page->resize(880, 760);
            page->show();
            application.processEvents();
        }
        page->reveal({field.pageId, field.sectionId, field.id});
        application.processEvents();
        auto* row = page->findChild<QWidget*>(
            settings::generatedObjectName(QStringLiteral("settings-item"), field.id));
        auto* picker = row ? row->findChild<adqt::widgets::AdColorPicker*>() : nullptr;
        require(picker != nullptr, "color setting creates a picker");
        const QColor original = backend.colorValue(definition.binding);
        const QColor originalPrimary = adqt::theme::ThemeManager::instance().config().primary;
        int writes = 0;
        int themeChanges = 0;
        const auto writeConnection = QObject::connect(
            &storage::ApplicationStorage::instance().configuration(),
            &storage::ConfigurationStore::valueChanged, page.get(),
            [&writes, configurationKey = field.configurationKey](const QString& key) {
                if (key == configurationKey) {
                    ++writes;
                }
            });
        const auto themeConnection =
            QObject::connect(&theme, &styles::ThemeManager::themeChanged, page.get(),
                             [&themeChanges] { ++themeChanges; });
        picker->setPopupVisible(true);
        application.processEvents();
        require(picker->popupVisible(), "color popup opens offscreen");
        QColor finalColor;
        for (int step = 0; step < 3; ++step) {
            finalColor = QColor(31 + step * 30, 97, 183,
                                definition.alphaChannelEnabled ? 100 + step * 30 : 255);
            picker->commitValue(adqt::widgets::AdColorValue::solid(finalColor));
            application.processEvents();
            require(picker->value().solidColor == finalColor,
                    "picker previews the current edit locally");
            require(backend.colorValue(definition.binding) == original && writes == 0,
                    "intermediate popup edits must not persist settings");
            require(themeChanges == 0 &&
                        adqt::theme::ThemeManager::instance().config().primary == originalPrimary,
                    "intermediate popup edits must not rebuild the application theme");
        }
        picker->setPopupVisible(false);
        application.processEvents();
        require(backend.colorValue(definition.binding) == finalColor && writes == 1,
                "closing the popup persists only its final color, including alpha");
        const bool primary =
            definition.binding == settings::SettingsColorBinding::ThemePrimaryColor;
        require(themeChanges == (primary ? 1 : 0), "theme updates only once after completion");
        if (primary) {
            require(adqt::theme::ThemeManager::instance().config().primary == finalColor,
                    "completed primary color is applied to the application theme");
        }
        picker->setPopupVisible(true);
        application.processEvents();
        picker->setPopupVisible(false);
        application.processEvents();
        require(writes == 1 && themeChanges == (primary ? 1 : 0),
                "opening and closing without editing does not apply another change");
        QObject::disconnect(writeConnection);
        QObject::disconnect(themeConnection);
        ++tested;
    }
    require(tested == 10, "cover all ten settings color pickers");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated settings storage");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize settings storage");
    snow_shot::presentation::LanguageManager::instance().initialize();
    styles::ThemeManager::instance().initialize(application);
    if (!application.arguments().contains(QStringLiteral("--color-presets-only")))
        settingsColorsCommitOnPopupClose(application);
    settingsColorPalettesCommitOnPopupClose(application);
    appStorage.shutdown();
    return 0;
}
