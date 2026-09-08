#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated translation settings storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create translation settings executable directory");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize translation settings storage");
    {
        snow_shot::presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        require(!applicationStorage.configuration()
                        .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                        .toBool() &&
                    !backend.switchValue(settings::SettingsSwitchBinding::DirectMlAcceleration),
                "DirectML acceleration should be disabled by default");
        const auto binding = settings::SettingsSwitchBinding::OriginalImageTranslation;
        const storage::ScreenshotTranslationSettings translation;
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        const auto pinBinding = settings::SettingsSelectBinding::PinDoubleClickAction;
        const QString pinId = QStringLiteral("pin-to-screen.double-click-action");
        require(backend.selectValue(pinBinding).toString() == QStringLiteral("thumbnail_mode") &&
                    session.state(pinId).enabled,
                "pinned double-click defaults to the enabled thumbnail selector");
        for (const QString& action :
             {QStringLiteral("none"), QStringLiteral("thumbnail_mode"), QStringLiteral("close")}) {
            require(backend.applySelectValue(pinBinding, action) &&
                        backend.selectValue(pinBinding).toString() == action,
                    "pinned double-click backend must apply every option");
        }
        require(!backend.applySelectValue(pinBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(pinBinding).toString() == QStringLiteral("close"),
                "pinned double-click backend must reject invalid actions");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(pinBinding).toString() == QStringLiteral("thumbnail_mode"),
                "resetting pin behavior must restore thumbnail double-click");
        const auto middleBinding = settings::SettingsSelectBinding::PinMiddleClickAction;
        const QString middleId = QStringLiteral("pin-to-screen.middle-mouse-button-action");
        require(backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom") &&
                    session.state(middleId).enabled,
                "pinned middle-click defaults to the enabled reset zoom selector");
        for (const QString& action : {QStringLiteral("none"), QStringLiteral("reset_zoom"),
                                      QStringLiteral("thumbnail_mode"), QStringLiteral("close")}) {
            require(backend.applySelectValue(middleBinding, action) &&
                        backend.selectValue(middleBinding).toString() == action,
                    "pinned middle-click backend must apply every option");
        }
        require(!backend.applySelectValue(middleBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("close"),
                "pinned middle-click backend must reject invalid actions");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom"),
                "resetting pin behavior must restore reset zoom middle-click");
        const auto fillBinding = settings::SettingsSelectBinding::OcrFillStyle;
        require(backend.selectValue(fillBinding).toString() == QStringLiteral("background_fill"),
                "OCR fill defaults to Background Fill");
        require(backend.applySelectValue(fillBinding, QStringLiteral("blur")) &&
                    backend.selectValue(fillBinding).toString() == QStringLiteral("blur"),
                "OCR fill selection must persist");
        require(!backend.applySelectValue(fillBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(fillBinding).toString() == QStringLiteral("blur"),
                "unsupported fill styles must not replace the saved choice");
        require(backend.resetSection(
                    settings::SettingsSectionReset::TextRecognitionInterfaceSettings) &&
                    backend.selectValue(fillBinding).toString() ==
                        QStringLiteral("background_fill"),
                "resetting Text Recognition appearance restores Background Fill");
        const auto layoutBinding = settings::SettingsSelectBinding::TranslationLayoutProcessing;
        const QString layoutId = QStringLiteral("translation.layout-processing");
        require(backend.selectValue(layoutBinding).toString() == QStringLiteral("smart_merge") &&
                    session.state(layoutId).enabled,
                "Smart Merge is the enabled default");
        require(backend.applySelectValue(layoutBinding, QStringLiteral("original")),
                "set Original layout");
        require(backend.applySwitchValue(binding, false), "disable original-image translation");
        session.refreshAll();
        require(!session.state(layoutId).enabled &&
                    translation.layoutProcessing() == QStringLiteral("original"),
                "disabled layout selector retains its choice");
        require(!backend.applySelectValue(layoutBinding, QStringLiteral("unsupported")),
                "reject unknown mode");
        require(backend.resetSection(settings::SettingsSectionReset::Translation),
                "reset translation layout");
        session.refreshAll();
        require(session.state(layoutId).enabled &&
                    translation.layoutProcessing() == QStringLiteral("smart_merge"),
                "reset enables original-image translation and restores Smart Merge");
        const storage::ScreenshotTranslationConfiguration languages{
            QStringLiteral("ja"), QStringLiteral("zh-Hant"), QStringLiteral("chosen-model")};
        require(backend.switchEnabled(binding) && backend.switchValue(binding),
                "backend should expose an enabled, default-on translation switch");
        require(translation.setConfiguration(languages) &&
                    backend.applySwitchValue(binding, false) && !backend.switchValue(binding),
                "backend should persist the display toggle");
        require(backend.resetSection(settings::SettingsSectionReset::Translation) &&
                    backend.switchValue(binding) && translation.configuration() == languages,
                "reset Translation should restore only the display toggle");

        require(backend.applySelectValue(settings::SettingsSelectBinding::OcrModelType,
                                         QStringLiteral("medium")) &&
                    backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                        QStringLiteral("medium") &&
                    applicationStorage.configuration().setValue(
                        QStringLiteral("text_recognition/direct_ml_acceleration"), true) &&
                    backend.resetSection(settings::SettingsSectionReset::TextRecognition) &&
                    backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                        QStringLiteral("small") &&
                    !applicationStorage.configuration()
                         .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                         .toBool(),
                "reset Text Recognition should restore Small and disable DirectML acceleration");
    }
    require(storage::PinToScreenSettings().setDoubleClickAction(QStringLiteral("close")),
            "save pinned double-click action before restart");
    require(storage::PinToScreenSettings().setMiddleMouseButtonAction(QStringLiteral("none")),
            "save pinned middle-click action before restart");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                storage::PinToScreenSettings().doubleClickAction() == QStringLiteral("close"),
            "pinned double-click action must survive a storage restart");
    require(storage::PinToScreenSettings().middleMouseButtonAction() == QStringLiteral("none"),
            "pinned middle-click action must survive a storage restart");
    applicationStorage.shutdown();
    return 0;
}
