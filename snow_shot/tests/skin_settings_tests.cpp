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
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>

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

void waitForSkinWork(const QPointer<presentation::MainWindowSkinController>& controller) {
    QElapsedTimer timeout;
    timeout.start();
    while (controller && controller->diagnostics().busy && timeout.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    require(!controller || !controller->diagnostics().busy,
            "surface skin work must complete within the deterministic timeout");
    drainEvents();
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
        if (binding == settings::SettingsFilePathBinding::TrayCustomIcon)
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
        return binding != settings::SettingsFilePathBinding::TrayCustomIcon &&
               m_status == Status::Error;
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
    require(
        configuration.value(QStringLiteral("interface/skin_path")).toString().isEmpty() &&
            configuration.value(QStringLiteral("interface/toolbar_skin_path"))
                .toString()
                .isEmpty() &&
            configuration.value(QStringLiteral("interface/tray_menu_skin_path"))
                .toString()
                .isEmpty() &&
            configuration.value(QStringLiteral("interface/skin_position")).toString() ==
                QStringLiteral("center") &&
            configuration.value(QStringLiteral("interface/toolbar_skin_position")).toString() ==
                QStringLiteral("center") &&
            configuration.value(QStringLiteral("interface/tray_menu_skin_position")).toString() ==
                QStringLiteral("center") &&
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

void invalidStoredSkinValuesUseDefaults(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "create an invalid stored skin configuration");
    const QByteArray bytes = R"({"storage":{"schema_version":3},"interface":{
        "theme_mode":"dark","skin_path":"/skins/preserved.png","skin_position":"top-left",
        "toolbar_skin_path":123,"toolbar_skin_position":"unknown","tray_menu_skin_path":false,
        "tray_menu_skin_position":42,"skin_display_mode":"stretch","skin_opacity":-1,
        "skin_blur_level":0.5,"skin_mask_opacity":101}})";
    require(file.write(bytes) == bytes.size(), "write the invalid skin configuration fixture");
    file.close();
    storage::ConfigurationStore configuration(path, true, true, 60000);
    require(
        configuration.value(QStringLiteral("interface/skin_path")).toString() ==
                QStringLiteral("/skins/preserved.png") &&
            configuration.value(QStringLiteral("interface/toolbar_skin_path"))
                .toString()
                .isEmpty() &&
            configuration.value(QStringLiteral("interface/tray_menu_skin_path"))
                .toString()
                .isEmpty() &&
            configuration.value(QStringLiteral("interface/skin_position")).toString() ==
                QStringLiteral("center") &&
            configuration.value(QStringLiteral("interface/toolbar_skin_position")).toString() ==
                QStringLiteral("center") &&
            configuration.value(QStringLiteral("interface/tray_menu_skin_position")).toString() ==
                QStringLiteral("center") &&
            configuration.value(QStringLiteral("interface/skin_display_mode")).toString() ==
                QStringLiteral("overlay") &&
            configuration.value(QStringLiteral("interface/skin_opacity")).toInt() == 100 &&
            configuration.value(QStringLiteral("interface/skin_blur_level")).toInt() == 0 &&
            configuration.value(QStringLiteral("interface/skin_mask_opacity")).toInt() == 80 &&
            configuration.value(QStringLiteral("interface/theme_mode")).toString() ==
                QStringLiteral("dark"),
        "invalid stored skin values must recover independently without dropping valid preferences");
    require(configuration.flushNow().success, "persist the recovered skin defaults");
    storage::ConfigurationStore reloaded(path, true, true, 60000);
    require(reloaded.snapshot() == configuration.snapshot(),
            "recovered skin defaults must survive a configuration reload");
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

    for (const auto binding : {skin, settings::SettingsFilePathBinding::ToolbarSkinPath,
                               settings::SettingsFilePathBinding::TrayMenuSkinPath}) {
        require(session.filePathStatus(binding).isEmpty() && !session.filePathStatusError(binding),
                "all empty surface skins must have no transient status");
        const QString key = binding == skin ? QStringLiteral("interface/skin_path")
                            : binding == settings::SettingsFilePathBinding::ToolbarSkinPath
                                ? QStringLiteral("interface/toolbar_skin_path")
                                : QStringLiteral("interface/tray_menu_skin_path");
        require(storage::ApplicationStorage::instance().configuration().setValue(key, path),
                "load a persisted surface skin path without an explicit settings commit");
        const QPointer<presentation::MainWindowSkinController> controller(
            presentation::MainWindowSkinController::existingInstance());
        require(controller != nullptr && controller->diagnostics().decodeJobs == 0,
                "loading a persisted path must connect a controller without image work");
        session.refreshAll();
        drainEvents();
        require(controller && controller->diagnostics().decodeJobs == 0 &&
                    controller->diagnostics().preparationJobs == 0 &&
                    session.filePathStatus(binding).isEmpty() &&
                    !session.filePathStatusError(binding),
                "configuration and settings refresh must preserve lazy startup for hidden skins");
        require(session.reset(settings::SettingsSectionReset::Skin),
                "clear the lazy surface skin fixture");
        drainEvents();
        require(
            session.filePathValue(binding).isEmpty() && controller.isNull() &&
                presentation::MainWindowSkinController::existingInstance() == nullptr,
            "clearing all unattached skins must retire the controller and restore the cold state");
    }
}

void zeroOpacitySkinSettingsRemainLazyAndValidateExplicitly(const QTemporaryDir& directory) {
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto registry = settings::buildBuiltInSettingsRegistry();
    QImage image(160, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#3377CC")));
    const auto writeInvalid = [](const QString& path) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write("corrupt png") == 11,
                "write a corrupt zero-opacity image fixture");
    };
    int surfaceIndex = 0;
    for (const auto binding : {settings::SettingsFilePathBinding::SkinPath,
                               settings::SettingsFilePathBinding::ToolbarSkinPath,
                               settings::SettingsFilePathBinding::TrayMenuSkinPath}) {
        const QString key = binding == settings::SettingsFilePathBinding::SkinPath
                                ? QStringLiteral("interface/skin_path")
                            : binding == settings::SettingsFilePathBinding::ToolbarSkinPath
                                ? QStringLiteral("interface/toolbar_skin_path")
                                : QStringLiteral("interface/tray_menu_skin_path");
        const QString persistedPath =
            directory.filePath(QStringLiteral("zero-opacity-persisted-%1.png").arg(surfaceIndex));
        const QString invalidEdit = directory.filePath(
            QStringLiteral("zero-opacity-invalid-edit-%1.png").arg(surfaceIndex));
        const QString validEdit =
            directory.filePath(QStringLiteral("zero-opacity-valid-edit-%1.png").arg(surfaceIndex));
        ++surfaceIndex;
        writeInvalid(persistedPath);
        writeInvalid(invalidEdit);
        require(image.save(validEdit), "write a valid zero-opacity image fixture");
        require(configuration.setValues({{key, persistedPath},
                                         {QStringLiteral("interface/skin_opacity"), 0},
                                         {QStringLiteral("interface/skin_blur_level"), 24}}),
                "seed a persisted zero-opacity surface before constructing settings");
        presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        settings::SettingsRuntimeSession session(registry, backend);
        session.refreshAll();
        drainEvents();
        require(presentation::MainWindowSkinController::existingInstance() == nullptr &&
                    session.filePathValue(binding) == persistedPath &&
                    session.filePathStatus(binding).isEmpty() &&
                    !session.filePathStatusError(binding),
                "persisted zero-opacity paths and settings refresh must not create skin runtime");

        int statusNotifications = 0;
        QObject statusObserver;
        QObject::connect(&session, &settings::SettingsRuntimeSession::filePathStatusChanged,
                         &statusObserver, [&statusNotifications, binding](auto changed) {
                             if (changed == binding)
                                 ++statusNotifications;
                         });
        const auto requireDecodeOnly = [&](bool error) {
            const QPointer<presentation::MainWindowSkinController> controller(
                presentation::MainWindowSkinController::existingInstance());
            require(controller != nullptr, "explicit validation must create a status controller");
            waitForSkinWork(controller);
            require(controller != nullptr, "validation status must survive worker retirement");
            const auto diagnostics = controller->diagnostics();
            require(session.filePathStatusError(binding) == error &&
                        session.filePathStatus(binding).isEmpty() != error &&
                        diagnostics.preparationJobs == 0 && diagnostics.pixmapConversions == 0 &&
                        diagnostics.retainedBytes == 0 && diagnostics.idleFrameBytes == 0 &&
                        diagnostics.executorCount == 0 && diagnostics.scratchRetainedBytes == 0,
                    "zero-opacity validation must preserve status without pixels or render work");
            return diagnostics.decodeJobs;
        };
        require(session.applyFilePathValue(binding, persistedPath),
                "explicitly reload a persisted zero-opacity path with no existing controller");
        const quint64 firstDecode = requireDecodeOnly(true);
        require(firstDecode == 1 && statusNotifications > 0,
                "a zero-opacity reload must validate once and notify its runtime session");
        require(image.save(persistedPath), "repair the persisted zero-opacity image in place");
        require(session.applyFilePathValue(binding, persistedPath),
                "reload the repaired zero-opacity image at the same path");
        require(requireDecodeOnly(false) == firstDecode + 1,
                "a repaired same-path reload must clear cached errors with one decode");
        require(session.applyFilePathValue(binding, invalidEdit),
                "explicitly edit a zero-opacity path to an invalid image");
        require(requireDecodeOnly(true) == firstDecode + 2,
                "an invalid zero-opacity path edit must update status with one decode");
        require(session.applyFilePathValue(binding, validEdit),
                "explicitly edit a zero-opacity path to a valid image");
        require(requireDecodeOnly(false) == firstDecode + 3,
                "a valid zero-opacity path edit must clear status without retaining pixels");

        const QPointer<presentation::MainWindowSkinController> retired(
            presentation::MainWindowSkinController::existingInstance());
        require(session.applyFilePathValue(binding, QString()),
                "clear the validated zero-opacity path before reconnecting settings");
        waitForSkinWork(retired);
        require(retired.isNull() &&
                    presentation::MainWindowSkinController::existingInstance() == nullptr,
                "clearing the last validation path must retire its status controller");
        require(configuration.setValue(key, invalidEdit),
                "load a new persisted zero-opacity path through the live backend");
        session.refreshAll();
        drainEvents();
        require(presentation::MainWindowSkinController::existingInstance() == nullptr,
                "live configuration notifications must preserve the zero-opacity cold state");
        require(session.applySliderValue(settings::SettingsSliderBinding::SkinOpacity, 100),
                "enable skin rendering after the original status controller was retired");
        QPointer<presentation::MainWindowSkinController> reenabled(
            presentation::MainWindowSkinController::existingInstance());
        require(reenabled && reenabled->diagnostics().decodeJobs == 0 &&
                    reenabled->diagnostics().preparationJobs == 0,
                "positive opacity must reconnect settings without rendering a hidden surface");
        const int beforeReload = statusNotifications;
        require(session.applyFilePathValue(binding, invalidEdit),
                "reload through the reconnected positive-opacity status controller");
        waitForSkinWork(reenabled);
        require(reenabled && session.filePathStatusError(binding) &&
                    !session.filePathStatus(binding).isEmpty() &&
                    statusNotifications > beforeReload &&
                    reenabled->diagnostics().decodeJobs == 1 &&
                    reenabled->diagnostics().preparationJobs == 0 &&
                    reenabled->diagnostics().pixmapConversions == 0,
                "reenabling opacity must reconnect status notifications to the new controller");
        require(session.reset(settings::SettingsSectionReset::Skin),
                "restore skin defaults after each zero-opacity surface regression");
        waitForSkinWork(reenabled);
        require(reenabled.isNull() &&
                    presentation::MainWindowSkinController::existingInstance() == nullptr,
                "reset must retire the reconnected skin controller");
    }
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
    constexpr auto toolbarPathBinding = settings::SettingsFilePathBinding::ToolbarSkinPath;
    constexpr auto trayPathBinding = settings::SettingsFilePathBinding::TrayMenuSkinPath;
    constexpr auto positionBinding = settings::SettingsSelectBinding::SkinPosition;
    constexpr auto toolbarPositionBinding = settings::SettingsSelectBinding::ToolbarSkinPosition;
    constexpr auto trayPositionBinding = settings::SettingsSelectBinding::TrayMenuSkinPosition;
    require(session.filePathValue(pathBinding).isEmpty() &&
                session.filePathValue(toolbarPathBinding).isEmpty() &&
                session.filePathValue(trayPathBinding).isEmpty() &&
                session.selectValue(positionBinding).toString() == QStringLiteral("center") &&
                session.selectValue(toolbarPositionBinding).toString() ==
                    QStringLiteral("center") &&
                session.selectValue(trayPositionBinding).toString() == QStringLiteral("center") &&
                session.selectValue(modeBinding).toString() == QStringLiteral("overlay") &&
                session.sliderValue(opacityBinding) == 100 &&
                session.sliderValue(blurBinding) == 0 && session.sliderValue(maskBinding) == 80,
            "skin settings must expose the configured default values through the runtime session");
    const QString path = QStringLiteral("/skins/背景 image.WEBP");
    const QString toolbarPath = QStringLiteral("/skins/toolbar image.png");
    const QString trayPath = QStringLiteral("/skins/tray image.jpg");
    require(session.applyFilePathValue(pathBinding,
                                       QStringLiteral("  ") + path + QStringLiteral(" ")) &&
                session.applyFilePathValue(toolbarPathBinding, QStringLiteral("  ") + toolbarPath +
                                                                   QStringLiteral("  ")) &&
                session.applyFilePathValue(trayPathBinding, QStringLiteral("  ") + trayPath +
                                                                QStringLiteral("  ")) &&
                session.applySelectValue(positionBinding, QStringLiteral("top_left")) &&
                session.applySelectValue(toolbarPositionBinding, QStringLiteral("bottom_center")) &&
                session.applySelectValue(trayPositionBinding, QStringLiteral("center_right")) &&
                session.applySelectValue(modeBinding, QStringLiteral("contain")) &&
                session.applySliderValue(opacityBinding, 42) &&
                session.applySliderValue(blurBinding, 17) &&
                session.applySliderValue(maskBinding, 65) && interfaceSettings.skinPath() == path &&
                interfaceSettings.toolbarSkinPath() == toolbarPath &&
                interfaceSettings.trayMenuSkinPath() == trayPath &&
                interfaceSettings.skinPosition() == QStringLiteral("top_left") &&
                interfaceSettings.toolbarSkinPosition() == QStringLiteral("bottom_center") &&
                interfaceSettings.trayMenuSkinPosition() == QStringLiteral("center_right") &&
                interfaceSettings.skinDisplayMode() == QStringLiteral("contain") &&
                interfaceSettings.skinOpacity() == 42 && interfaceSettings.skinBlurLevel() == 17 &&
                interfaceSettings.skinMaskOpacity() == 65,
            "all skin fields must round-trip through the runtime session, backend and adapters");
    const auto beforeRejected = configuration.snapshot();
    require(!backend.applySelectValue(modeBinding, QStringLiteral("stretch")) &&
                !backend.applySelectValue(positionBinding, QStringLiteral("top-left")) &&
                !backend.applySelectValue(toolbarPositionBinding, QStringLiteral("stretch")) &&
                !backend.applySelectValue(trayPositionBinding, QStringLiteral("")) &&
                !backend.applySliderValue(opacityBinding, -1) &&
                !backend.applySliderValue(blurBinding, 101) &&
                !backend.applySliderValue(maskBinding, 101) &&
                !configuration.setValue(QStringLiteral("interface/skin_blur_level"), 0.5) &&
                !configuration.setValue(QStringLiteral("interface/skin_opacity"),
                                        QStringLiteral("50")) &&
                !configuration.setValue(QStringLiteral("interface/skin_path"), 123) &&
                !configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), 123) &&
                !configuration.setValue(QStringLiteral("interface/tray_menu_skin_path"), false) &&
                configuration.snapshot() == beforeRejected,
            "invalid modes, ranges and types must be rejected without changing skin configuration");
    require(configuration.flushNow().success, "skin settings must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(QStringLiteral("interface/skin_path")).toString() == path &&
                reloaded.value(QStringLiteral("interface/toolbar_skin_path")).toString() ==
                    toolbarPath &&
                reloaded.value(QStringLiteral("interface/tray_menu_skin_path")).toString() ==
                    trayPath &&
                reloaded.value(QStringLiteral("interface/skin_position")).toString() ==
                    QStringLiteral("top_left") &&
                reloaded.value(QStringLiteral("interface/toolbar_skin_position")).toString() ==
                    QStringLiteral("bottom_center") &&
                reloaded.value(QStringLiteral("interface/tray_menu_skin_position")).toString() ==
                    QStringLiteral("center_right") &&
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
    for (const auto binding : {positionBinding, toolbarPositionBinding, trayPositionBinding}) {
        const auto* field = registry.fieldForSelect(binding);
        const auto& options =
            std::get<settings::SettingsSelectDefinition>(field->definition->payload).options;
        for (const auto& option : options) {
            require(session.applySelectValue(binding, option.value) &&
                        session.selectValue(binding) == option.value,
                    "every nine-point alignment must round-trip through its own surface selector");
        }
    }
    require(interfaceSettings.setThemePrimaryColor(QColor(QStringLiteral("#8765AB"))),
            "prepare an independent interface preference before the Skin reset");
    const QColor primary = interfaceSettings.themePrimaryColor();
    require(session.reset(settings::SettingsSectionReset::Skin) &&
                interfaceSettings.skinPath().isEmpty() &&
                interfaceSettings.toolbarSkinPath().isEmpty() &&
                interfaceSettings.trayMenuSkinPath().isEmpty() &&
                interfaceSettings.skinPosition() == QStringLiteral("center") &&
                interfaceSettings.toolbarSkinPosition() == QStringLiteral("center") &&
                interfaceSettings.trayMenuSkinPosition() == QStringLiteral("center") &&
                interfaceSettings.skinDisplayMode() == QStringLiteral("overlay") &&
                interfaceSettings.skinOpacity() == 100 && interfaceSettings.skinBlurLevel() == 0 &&
                interfaceSettings.skinMaskOpacity() == 80 &&
                interfaceSettings.themePrimaryColor() == primary,
            "Skin reset must restore its ten defaults without changing General settings");
    require(session.applyFilePathValue(pathBinding, path) &&
                session.applyFilePathValue(toolbarPathBinding, toolbarPath) &&
                session.applyFilePathValue(trayPathBinding, trayPath) &&
                session.applyFilePathValue(pathBinding, QString()) &&
                interfaceSettings.skinPath().isEmpty() &&
                interfaceSettings.toolbarSkinPath() == toolbarPath &&
                interfaceSettings.trayMenuSkinPath() == trayPath &&
                session.applyFilePathValue(toolbarPathBinding, QString()) &&
                interfaceSettings.trayMenuSkinPath() == trayPath &&
                session.applyFilePathValue(trayPathBinding, QString()),
            "clearing a path must disable only its skin without resetting other surface images");
}

void clearedSkinPathsRemoveStatusImmediately(const QTemporaryDir& directory) {
    drainEvents();
    waitForSkinWork(presentation::MainWindowSkinController::existingInstance());
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    const QString missing = directory.filePath(QStringLiteral("missing-status.png"));
    for (const auto binding : {settings::SettingsFilePathBinding::SkinPath,
                               settings::SettingsFilePathBinding::ToolbarSkinPath,
                               settings::SettingsFilePathBinding::TrayMenuSkinPath}) {
        require(session.applyFilePathValue(binding, missing), "configure a missing surface image");
        QPointer<presentation::MainWindowSkinController> controller =
            presentation::MainWindowSkinController::existingInstance();
        require(controller != nullptr, "a configured skin must provide its status controller");
        waitForSkinWork(controller);
        require(controller && !controller->diagnostics().busy &&
                    session.filePathStatusError(binding) &&
                    !session.filePathStatus(binding).isEmpty() &&
                    controller->diagnostics().preparationJobs == 0 &&
                    controller->diagnostics().pixmapConversions == 0,
                "a hidden missing surface image must expose its error without rendering work");
        require(
            session.applyFilePathValue(binding, QString()) &&
                session.filePathStatus(binding).isEmpty() && !session.filePathStatusError(binding),
            "clearing a skin path must immediately remove its status before queued engine refresh");
        waitForSkinWork(controller);
    }
}

void hiddenSurfaceSkinsValidateAndReuseSources(const QTemporaryDir& directory) {
    constexpr auto toolbar = settings::SettingsFilePathBinding::ToolbarSkinPath;
    constexpr auto tray = settings::SettingsFilePathBinding::TrayMenuSkinPath;
    const QString toolbarPath = directory.filePath(QStringLiteral("hidden-toolbar.png"));
    const QString trayPath = directory.filePath(QStringLiteral("hidden-tray.png"));
    for (const auto& path : {toolbarPath, trayPath}) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write("corrupt png") == 11,
                "create independent corrupt hidden surface images");
    }
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    require(session.applyFilePathValue(toolbar,
                                       QStringLiteral("  ") + toolbarPath + QStringLiteral("  ")),
            "commit the hidden toolbar skin path with whitespace trimming");
    QPointer<presentation::MainWindowSkinController> controller =
        presentation::MainWindowSkinController::existingInstance();
    waitForSkinWork(controller);
    require(controller && session.filePathValue(toolbar) == toolbarPath &&
                session.filePathStatusError(toolbar) &&
                !session.filePathStatus(toolbar).isEmpty() &&
                session.filePathStatus(tray).isEmpty() && !session.filePathStatusError(tray),
            "a corrupt hidden toolbar image must report an error independently of the tray");
    const QString toolbarError = session.filePathStatus(toolbar);
    const quint64 beforeTray = controller->diagnostics().decodeJobs;
    require(session.applyFilePathValue(tray, trayPath), "commit the hidden tray skin path");
    waitForSkinWork(controller);
    require(controller && controller->diagnostics().decodeJobs == beforeTray + 1 &&
                session.filePathStatusError(tray) && !session.filePathStatus(tray).isEmpty() &&
                session.filePathStatus(toolbar) == toolbarError &&
                session.filePathStatus(settings::SettingsFilePathBinding::SkinPath).isEmpty(),
            "a corrupt hidden tray image must validate once without changing other profile status");
    const QString trayError = session.filePathStatus(tray);
    require(controller->diagnostics().preparationJobs == 0 &&
                controller->diagnostics().pixmapConversions == 0,
            "hidden validation must perform no raster preparation or pixmap conversion");

    QImage image(160, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#3377CC")));
    for (const auto binding : {toolbar, tray}) {
        const QString path = binding == toolbar ? toolbarPath : trayPath;
        require(image.save(path), "repair the hidden image without changing its path");
        const quint64 beforeReload = controller->diagnostics().decodeJobs;
        require(backend.applyFilePathValue(binding, path),
                "an unchanged backend write must remain accepted without a reload intent");
        drainEvents();
        require(controller && controller->diagnostics().decodeJobs == beforeReload,
                "an unchanged backend write must avoid duplicate validation work");
        require(session.applyFilePathValue(binding, path),
                "explicitly reload the repaired hidden image at the same path");
        waitForSkinWork(controller);
        require(controller && controller->diagnostics().decodeJobs == beforeReload + 1 &&
                    session.filePathStatus(binding).isEmpty() &&
                    !session.filePathStatusError(binding) &&
                    controller->diagnostics().preparationJobs == 0 &&
                    controller->diagnostics().pixmapConversions == 0,
                "same-path reload must clear the repaired image error with decode-only work");
        if (binding == toolbar) {
            require(session.filePathStatus(tray) == trayError && session.filePathStatusError(tray),
                    "repairing the hidden toolbar image must preserve the tray image error");
        }
    }

    for (const auto surface :
         {presentation::SkinSurface::Toolbar, presentation::SkinSurface::TrayMenu}) {
        const auto before = controller->diagnostics();
        QObject view;
        controller->attach(&view, surface, QSize(80, 40), 1.0);
        waitForSkinWork(controller);
        require(controller && controller->skinActive(&view) &&
                    controller->diagnostics().decodeJobs == before.decodeJobs &&
                    controller->diagnostics().preparationJobs == before.preparationJobs + 1 &&
                    controller->diagnostics().pixmapConversions == before.pixmapConversions + 1,
                "first opening a validated surface must reuse its decode and prepare one raster");
        controller->detach(&view);
        waitForSkinWork(controller);
    }
    require(
        session.applyFilePathValue(toolbar, QString()) &&
            session.filePathStatus(toolbar).isEmpty() && !session.filePathStatusError(toolbar) &&
            session.filePathValue(tray) == trayPath &&
            session.applyFilePathValue(tray, QString()) && session.filePathStatus(tray).isEmpty() &&
            !session.filePathStatusError(tray),
        "clearing hidden paths must remove each status immediately and preserve the other path");
    waitForSkinWork(controller);
    require(controller.isNull() &&
                presentation::MainWindowSkinController::existingInstance() == nullptr,
            "clearing the last validated image must restore the cold controller state");
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
    require(generalRow != nullptr, "the General row must be materialized alongside Skin");
    const QSizePolicy generalSizePolicy = generalRow->sizePolicy();

    auto& languages = presentation::LanguageManager::instance();
    auto& themes = presentation::styles::ThemeManager::instance();
    const auto initialTheme = themes.themeMode();
    const QString pathId = QStringLiteral("interface.skin.path");
    const QStringList fieldIds = {pathId,
                                  QStringLiteral("interface.skin.position"),
                                  QStringLiteral("interface.skin.toolbar-path"),
                                  QStringLiteral("interface.skin.toolbar-position"),
                                  QStringLiteral("interface.skin.tray-menu-path"),
                                  QStringLiteral("interface.skin.tray-menu-position"),
                                  QStringLiteral("interface.skin.display-mode"),
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
                    (language == QStringLiteral("en_US")
                         ? QStringLiteral("Main Interface Skin Path")
                     : language == QStringLiteral("zh_CN") ? QStringLiteral("主界面皮肤路径")
                                                           : QStringLiteral("主介面皮膚路徑")),
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
                    require(generalRow->sizePolicy() == generalSizePolicy,
                            "Skin status must preserve the General form field sizing policy");
                    const QString pathStatus =
                        session.filePathStatus(settings::SettingsFilePathBinding::SkinPath);
                    for (const auto& fieldId : fieldIds)
                        requireCopyFits(page, registry, fieldId,
                                        fieldId == pathId ||
                                                fieldId.endsWith(QStringLiteral("-path"))
                                            ? pathStatus
                                            : QString());
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
    invalidStoredSkinValuesUseDefaults(
        temporary.filePath(QStringLiteral("invalid-skin-config.json")));
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize isolated settings storage");
    presentation::LanguageManager::instance().initialize();
    presentation::styles::ThemeManager::instance().initialize(application);
    emptySkinSettingsStayLazy(temporary.filePath(QStringLiteral("lazy.png")));
    zeroOpacitySkinSettingsRemainLazyAndValidateExplicitly(temporary);
    skinSettingsPersistValidateAndReset(temporary.filePath(QStringLiteral("data/config.json")));
    clearedSkinPathsRemoveStatusImmediately(temporary);
    hiddenSurfaceSkinsValidateAndReuseSources(temporary);
    skinCopyFitsAfterStatusLanguageThemeAndResize();
    applicationStorage.shutdown();
    return 0;
}
