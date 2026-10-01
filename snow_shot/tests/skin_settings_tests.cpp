#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/components/pathinput.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QPointer>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;
namespace presentation = snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void drainEvents() {
    for (int i = 0; i < 6; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// Keep real settings values while controlling transient image status without
// introducing executor timing into the layout and translation checks.
class SkinStatusBackend final : public settings::SettingsBackend {
  public:
    enum class Status { Ready, Loading, Error };

    explicit SkinStatusBackend(settings::BuiltInSettingsBackend& backend) : m_backend(backend) {
        connect(&backend, &settings::SettingsBackend::synchronized, this,
                &settings::SettingsBackend::synchronized);
    }

    void setStatus(Status status) {
        m_status = status;
        emit synchronized();
    }

    QString filePathStatus(settings::SettingsFilePathBinding binding) const override {
        if (binding != settings::SettingsFilePathBinding::SkinPath)
            return m_backend.filePathStatus(binding);
        switch (m_status) {
        case Status::Ready:
            return {};
        case Status::Loading:
            return QCoreApplication::translate("MainWindowSkin", "Loading skin...");
        case Status::Error:
            return QCoreApplication::translate("MainWindowSkin",
                                               "The skin image could not be opened.");
        }
        return {};
    }
    bool filePathStatusError(settings::SettingsFilePathBinding binding) const override {
        return binding == settings::SettingsFilePathBinding::SkinPath && m_status == Status::Error;
    }
    QVariant selectValue(settings::SettingsSelectBinding binding) const override {
        return m_backend.selectValue(binding);
    }
    QVector<settings::SettingsRuntimeOption>
    dynamicSelectOptions(settings::SettingsSelectBinding binding) const override {
        return m_backend.dynamicSelectOptions(binding);
    }
    bool applySelectValue(settings::SettingsSelectBinding binding, const QVariant& value) override {
        return m_backend.applySelectValue(binding, value);
    }
    bool switchValue(settings::SettingsSwitchBinding binding) const override {
        return m_backend.switchValue(binding);
    }
    bool applySwitchValue(settings::SettingsSwitchBinding binding, bool value) override {
        return m_backend.applySwitchValue(binding, value);
    }
    QVariantList multiSelectValue(settings::SettingsMultiSelectBinding binding) const override {
        return m_backend.multiSelectValue(binding);
    }
    bool applyMultiSelectValue(settings::SettingsMultiSelectBinding binding,
                               const QVariantList& value) override {
        return m_backend.applyMultiSelectValue(binding, value);
    }
    int integerValue(settings::SettingsIntegerBinding binding) const override {
        return m_backend.integerValue(binding);
    }
    bool applyIntegerValue(settings::SettingsIntegerBinding binding, int value) override {
        return m_backend.applyIntegerValue(binding, value);
    }
    int sliderValue(settings::SettingsSliderBinding binding) const override {
        return m_backend.sliderValue(binding);
    }
    bool applySliderValue(settings::SettingsSliderBinding binding, int value) override {
        return m_backend.applySliderValue(binding, value);
    }
    QColor colorValue(settings::SettingsColorBinding binding) const override {
        return m_backend.colorValue(binding);
    }
    bool applyColorValue(settings::SettingsColorBinding binding, const QColor& value) override {
        return m_backend.applyColorValue(binding, value);
    }
    QVariant radioValue(settings::SettingsRadioBinding binding) const override {
        return m_backend.radioValue(binding);
    }
    bool applyRadioValue(settings::SettingsRadioBinding binding, const QVariant& value) override {
        return m_backend.applyRadioValue(binding, value);
    }
    QString filePathValue(settings::SettingsFilePathBinding binding) const override {
        return m_backend.filePathValue(binding);
    }
    bool applyFilePathValue(settings::SettingsFilePathBinding binding,
                            const QString& value) override {
        return m_backend.applyFilePathValue(binding, value);
    }
    QString directoryPathValue(settings::SettingsDirectoryPathBinding binding) const override {
        return m_backend.directoryPathValue(binding);
    }
    bool applyDirectoryPathValue(settings::SettingsDirectoryPathBinding binding,
                                 const QString& value) override {
        return m_backend.applyDirectoryPathValue(binding, value);
    }
    QString textValue(settings::SettingsTextBinding binding) const override {
        return m_backend.textValue(binding);
    }
    bool applyTextValue(settings::SettingsTextBinding binding, const QString& value) override {
        return m_backend.applyTextValue(binding, value);
    }
    storage::ScreenshotToolbarLayout
    toolbarLayout(storage::ScreenshotToolbarLayoutKind kind) const override {
        return m_backend.toolbarLayout(kind);
    }
    bool applyToolbarLayout(storage::ScreenshotToolbarLayoutKind kind,
                            const storage::ScreenshotToolbarLayout& layout) override {
        return m_backend.applyToolbarLayout(kind, layout);
    }
    presentation::GlobalShortcutRegistrationState
    shortcutState(presentation::GlobalShortcutAction action) const override {
        return m_backend.shortcutState(action);
    }
    presentation::GlobalShortcutValidationResult
    validateShortcut(presentation::GlobalShortcutAction action,
                     const snow_shot::shortcuts::ShortcutBinding& shortcut) const override {
        return m_backend.validateShortcut(action, shortcut);
    }
    bool applyShortcuts(presentation::GlobalShortcutAction action,
                        const snow_shot::shortcuts::ShortcutBindingList& shortcuts) override {
        return m_backend.applyShortcuts(action, shortcuts);
    }
    snow_shot::shortcuts::ShortcutBindingList
    localShortcuts(settings::SettingsLocalShortcutScope scope,
                   const QString& shortcutId) const override {
        return m_backend.localShortcuts(scope, shortcutId);
    }
    presentation::GlobalShortcutValidationResult
    validateLocalShortcut(settings::SettingsLocalShortcutScope scope, const QString& shortcutId,
                          const snow_shot::shortcuts::ShortcutBinding& shortcut) const override {
        return m_backend.validateLocalShortcut(scope, shortcutId, shortcut);
    }
    bool applyLocalShortcuts(settings::SettingsLocalShortcutScope scope, const QString& shortcutId,
                             const snow_shot::shortcuts::ShortcutBindingList& shortcuts) override {
        return m_backend.applyLocalShortcuts(scope, shortcutId, shortcuts);
    }
    settings::SettingsActionState
    actionState(settings::SettingsActionBinding binding) const override {
        return m_backend.actionState(binding);
    }
    bool triggerAction(settings::SettingsActionBinding binding, const QString& path) override {
        return m_backend.triggerAction(binding, path);
    }
    storage::StorageStatus storageStatus() const override {
        return m_backend.storageStatus();
    }
    bool resetSection(settings::SettingsSectionReset reset) override {
        return m_backend.resetSection(reset);
    }

  private:
    settings::BuiltInSettingsBackend& m_backend;
    Status m_status = Status::Ready;
};

void missingSkinKeysUseDefaults(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create the previous-version configuration fixture");
    const QByteArray bytes =
        R"({"storage":{"schema_version":3},"interface":{"theme_mode":"dark"}})";
    require(file.write(bytes) == bytes.size(), "write the previous-version configuration fixture");
    file.close();
    storage::ConfigurationStore configuration(path, true, true, 60000);
    require(configuration.value(QStringLiteral("interface/skin_path")).toString().isEmpty() &&
                configuration.value(QStringLiteral("interface/skin_display_mode")).toString() ==
                    QStringLiteral("overlay") &&
                configuration.value(QStringLiteral("interface/skin_opacity")).toInt() == 100 &&
                configuration.value(QStringLiteral("interface/skin_blur_level")).toInt() == 0 &&
                configuration.value(QStringLiteral("interface/skin_mask_opacity")).toInt() == 80 &&
                configuration.value(QStringLiteral("interface/theme_mode")).toString() ==
                    QStringLiteral("dark") &&
                configuration.value(QStringLiteral("storage/schema_version")).toInt() == 3,
            "existing configurations must gain skin defaults without a migration or theme change");
}

void emptySkinSettingsStayLazy(const QString& path) {
    require(presentation::MainWindowSkinController::existingInstance() == nullptr,
            "an empty Skin Path must not create a controller during startup");
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    constexpr auto skin = settings::SettingsFilePathBinding::SkinPath;
    require(session.filePathStatus(skin).isEmpty() && !session.filePathStatusError(skin) &&
                session.applyFilePathValue(skin, QString()),
            "empty Skin status and same-path reload must remain harmless");
    session.refreshAll();
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
    page.resize(560, 640);
    page.reveal({page.pageId(), QStringLiteral("skin"), {}});
    require(presentation::MainWindowSkinController::existingInstance() == nullptr,
            "backend construction, refresh and Skin controls must keep an empty skin lazy");

    int notifications = 0;
    QObject::connect(&backend, &settings::SettingsBackend::synchronized, &backend,
                     [&notifications] { ++notifications; });
    require(session.applyFilePathValue(skin, path), "configure the first lazy skin path");
    const QPointer<presentation::MainWindowSkinController> controller(
        presentation::MainWindowSkinController::existingInstance());
    require(controller != nullptr && controller->diagnostics().decodeJobs == 0,
            "the first nonempty path must connect a controller without decoding before attach");
    const int beforeReload = notifications;
    require(session.applyFilePathValue(skin, path) && notifications == beforeReload + 1,
            "same-path reload must notify through exactly one controller status connection");
    require(session.reset(settings::SettingsSectionReset::Skin), "clear the lazy skin fixture");
    drainEvents();
    require(storage::InterfaceSettings().skinPath().isEmpty() && controller.isNull() &&
                presentation::MainWindowSkinController::existingInstance() == nullptr,
            "clearing an unattached skin must retire its controller and restore the cold state");
}

void skinSettingsPersistValidateAndReset(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    storage::InterfaceSettings interfaceSettings;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    constexpr auto pathBinding = settings::SettingsFilePathBinding::SkinPath;
    constexpr auto modeBinding = settings::SettingsSelectBinding::SkinDisplayMode;
    constexpr auto opacityBinding = settings::SettingsSliderBinding::SkinOpacity;
    constexpr auto blurBinding = settings::SettingsSliderBinding::SkinBlurLevel;
    constexpr auto maskBinding = settings::SettingsSliderBinding::SkinMaskOpacity;
    require(session.filePathValue(pathBinding).isEmpty() &&
                session.selectValue(modeBinding).toString() == QStringLiteral("overlay") &&
                session.sliderValue(opacityBinding) == 100 &&
                session.sliderValue(blurBinding) == 0 && session.sliderValue(maskBinding) == 80,
            "skin settings must expose the configured default values through the runtime session");
    const QString path = QStringLiteral("/skins/背景 image.WEBP");
    require(session.applyFilePathValue(pathBinding,
                                       QStringLiteral("  ") + path + QStringLiteral(" ")) &&
                session.applySelectValue(modeBinding, QStringLiteral("contain")) &&
                session.applySliderValue(opacityBinding, 42) &&
                session.applySliderValue(blurBinding, 17) &&
                session.applySliderValue(maskBinding, 65) && interfaceSettings.skinPath() == path &&
                interfaceSettings.skinDisplayMode() == QStringLiteral("contain") &&
                interfaceSettings.skinOpacity() == 42 && interfaceSettings.skinBlurLevel() == 17 &&
                interfaceSettings.skinMaskOpacity() == 65,
            "all skin fields must round-trip through the runtime session, backend and adapters");
    const auto beforeRejected = configuration.snapshot();
    require(!backend.applySelectValue(modeBinding, QStringLiteral("stretch")) &&
                !backend.applySliderValue(opacityBinding, -1) &&
                !backend.applySliderValue(blurBinding, 101) &&
                !backend.applySliderValue(maskBinding, 101) &&
                !configuration.setValue(QStringLiteral("interface/skin_blur_level"), 0.5) &&
                !configuration.setValue(QStringLiteral("interface/skin_opacity"),
                                        QStringLiteral("50")) &&
                !configuration.setValue(QStringLiteral("interface/skin_path"), 123) &&
                configuration.snapshot() == beforeRejected,
            "invalid modes, ranges and types must be rejected without changing skin configuration");
    require(configuration.flushNow().success, "skin settings must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(QStringLiteral("interface/skin_path")).toString() == path &&
                reloaded.value(QStringLiteral("interface/skin_display_mode")).toString() ==
                    QStringLiteral("contain") &&
                reloaded.value(QStringLiteral("interface/skin_opacity")).toInt() == 42 &&
                reloaded.value(QStringLiteral("interface/skin_blur_level")).toInt() == 17 &&
                reloaded.value(QStringLiteral("interface/skin_mask_opacity")).toInt() == 65,
            "skin settings must survive a configuration reload");
    for (const auto binding : {opacityBinding, blurBinding, maskBinding}) {
        require(backend.applySliderValue(binding, 0) && backend.sliderValue(binding) == 0 &&
                    backend.applySliderValue(binding, 100) && backend.sliderValue(binding) == 100,
                "every skin slider must accept both boundary values");
    }
    require(interfaceSettings.setThemePrimaryColor(QColor(QStringLiteral("#8765AB"))),
            "prepare an independent interface preference before the Skin reset");
    const QColor primary = interfaceSettings.themePrimaryColor();
    require(session.reset(settings::SettingsSectionReset::Skin) &&
                interfaceSettings.skinPath().isEmpty() &&
                interfaceSettings.skinDisplayMode() == QStringLiteral("overlay") &&
                interfaceSettings.skinOpacity() == 100 && interfaceSettings.skinBlurLevel() == 0 &&
                interfaceSettings.skinMaskOpacity() == 80 &&
                interfaceSettings.themePrimaryColor() == primary,
            "Skin reset must restore its five defaults without changing General settings");
    require(session.applyFilePathValue(pathBinding, path) &&
                session.applyFilePathValue(pathBinding, QString()) &&
                interfaceSettings.skinPath().isEmpty(),
            "clearing the path must disable the skin without resetting other preferences");
}

QLabel* descriptionForField(SettingsPageWidget& page, const settings::SettingsRegistry& registry,
                            const QString& fieldId, const QString& status = {}) {
    const auto* field = registry.field(fieldId);
    require(field != nullptr, "the Skin layout field must be registered");
    auto* row = page.findChild<QWidget*>(
        settings::generatedObjectName(QStringLiteral("settings-item"), fieldId));
    require(row != nullptr, "the Skin layout row must be materialized");
    QString expected = field->definition->description.translated();
    if (!status.isEmpty())
        expected += QStringLiteral("\n") + status;
    for (auto* label : row->findChildren<QLabel*>()) {
        if (label->text() == expected)
            return label;
    }
    require(false, "the Skin row must contain its full translated description and status");
    return nullptr;
}

void requireCopyFits(SettingsPageWidget& page, const settings::SettingsRegistry& registry,
                     const QString& fieldId, const QString& status = {}) {
    QLabel* description = descriptionForField(page, registry, fieldId, status);
    require(description->width() > 0 &&
                description->height() >= description->heightForWidth(description->width()),
            "wrapped Skin descriptions and status must have enough height at their actual width");
    auto* row = page.findChild<QWidget*>(
        settings::generatedObjectName(QStringLiteral("settings-item"), fieldId));
    const QRect descriptionBounds(description->mapTo(row, QPoint()), description->size());
    require(row->rect().contains(descriptionBounds),
            "Skin description geometry must fit inside its row without clipping");
    const auto* field = registry.field(fieldId);
    for (auto* label : row->findChildren<QLabel*>()) {
        if (label->text() != field->definition->title.translated())
            continue;
        require(label->height() >= label->heightForWidth(label->width()) &&
                    row->rect().contains(QRect(label->mapTo(row, QPoint()), label->size())) &&
                    label->mapTo(row, QPoint(0, label->height())).y() <= descriptionBounds.top(),
                "Skin titles must also fit above their descriptions");
        return;
    }
    require(false, "the Skin row must retain its translated title");
}

void skinCopyFitsAfterStatusLanguageThemeAndResize() {
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend builtIn(shortcuts);
    SkinStatusBackend backend(builtIn);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
    page.resize(560, 640);
    page.show();
    page.reveal({page.pageId(), QStringLiteral("skin"), {}});
    drainEvents();
    auto* generalRow = page.findChild<QWidget*>(QStringLiteral("settings-item-interface-theme"));
    require(generalRow != nullptr &&
                generalRow->sizePolicy().verticalPolicy() == QSizePolicy::Fixed,
            "wrapping Skin copy must preserve the existing General row sizing policy");

    auto& languages = presentation::LanguageManager::instance();
    auto& themes = presentation::styles::ThemeManager::instance();
    const auto initialTheme = themes.themeMode();
    const QString pathId = QStringLiteral("interface.skin.path");
    const QStringList fieldIds = {pathId, QStringLiteral("interface.skin.display-mode"),
                                  QStringLiteral("interface.skin.opacity"),
                                  QStringLiteral("interface.skin.blur-level"),
                                  QStringLiteral("interface.skin.mask-opacity")};
    for (const auto& language :
         {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        backend.setStatus(SkinStatusBackend::Status::Error);
        require(languages.setLanguage(language), "load each real Skin translation catalog");
        drainEvents();
        requireCopyFits(page, registry, pathId,
                        session.filePathStatus(settings::SettingsFilePathBinding::SkinPath));
        const auto* pathField = registry.field(pathId);
        require(pathField->definition->title.translated() ==
                    (language == QStringLiteral("en_US")   ? QStringLiteral("Skin Path")
                     : language == QStringLiteral("zh_CN") ? QStringLiteral("皮肤路径")
                                                           : QStringLiteral("皮膚路徑")),
                "the layout checks must use the real English, Simplified and Traditional copy");
        for (const auto theme :
             {presentation::styles::ThemeMode::Light, presentation::styles::ThemeMode::Dark}) {
            backend.setStatus(SkinStatusBackend::Status::Error);
            themes.setThemeMode(theme);
            drainEvents();
            requireCopyFits(page, registry, pathId,
                            session.filePathStatus(settings::SettingsFilePathBinding::SkinPath));
            for (const int width : {820, 560, 820}) {
                backend.setStatus(SkinStatusBackend::Status::Error);
                page.resize(width, 640);
                drainEvents();
                requireCopyFits(
                    page, registry, pathId,
                    session.filePathStatus(settings::SettingsFilePathBinding::SkinPath));
                int readyHeight = 0;
                for (const auto status :
                     {SkinStatusBackend::Status::Ready, SkinStatusBackend::Status::Loading,
                      SkinStatusBackend::Status::Error, SkinStatusBackend::Status::Ready}) {
                    backend.setStatus(status);
                    drainEvents();
                    const QString pathStatus =
                        session.filePathStatus(settings::SettingsFilePathBinding::SkinPath);
                    for (const auto& fieldId : fieldIds)
                        requireCopyFits(page, registry, fieldId,
                                        fieldId == pathId ? pathStatus : QString());
                    QLabel* pathDescription =
                        descriptionForField(page, registry, pathId, pathStatus);
                    if (status == SkinStatusBackend::Status::Ready) {
                        if (readyHeight != 0)
                            require(
                                pathDescription->height() == readyHeight,
                                "cleared Skin status must release the extra description height");
                        readyHeight = pathDescription->height();
                    } else {
                        require(pathDescription->height() > readyHeight,
                                "loading and error status must expand the Skin Path description");
                    }
                    auto* input = page.findChild<FilePathInput*>(
                        QStringLiteral("settings-control-interface-skin-path"));
                    require(
                        input != nullptr &&
                            input->lineEdit()->accessibleDescription() == pathDescription->text() &&
                            input->lineEdit()->status() ==
                                (status == SkinStatusBackend::Status::Error
                                     ? adqt::widgets::AdLineEdit::Status::Error
                                     : adqt::widgets::AdLineEdit::Status::None) &&
                            pathDescription->palette().color(QPalette::WindowText) ==
                                (status == SkinStatusBackend::Status::Error
                                     ? themes.themeColorScheme().map.colorErrorText
                                     : themes.themeColorScheme().map.colorTextSecondary),
                        "Skin status must retranslate accessibly and keep its themed error style");
                }
            }
        }
    }
    require(languages.setLanguage(QStringLiteral("en_US")),
            "restore English after the layout check");
    themes.setThemeMode(initialTheme);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    require(QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf"))) >= 0 &&
                QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc"))) >= 0,
            "load Windows UI fonts for offscreen Skin layout checks");
#endif
    QTemporaryDir temporary;
    require(temporary.isValid(), "create an isolated skin settings directory");
    missingSkinKeysUseDefaults(temporary.filePath(QStringLiteral("previous-config.json")));
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize isolated settings storage");
    presentation::LanguageManager::instance().initialize();
    presentation::styles::ThemeManager::instance().initialize(application);
    emptySkinSettingsStayLazy(temporary.filePath(QStringLiteral("lazy.png")));
    skinSettingsPersistValidateAndReset(temporary.filePath(QStringLiteral("data/config.json")));
    skinCopyFitsAfterStatusLanguageThemeAndResize();
    applicationStorage.shutdown();
    return 0;
}
