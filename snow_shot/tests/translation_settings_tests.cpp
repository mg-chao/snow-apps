#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationarchive.h"

#include "snow_shot/presentation/components/settingscustomwidget.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "widgets/button.h"
#include "widgets/popconfirm.h"
#include "widgets/switch.h"
#include "theme/theme_manager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QDir>
#include <QHash>
#include <QGridLayout>
#include <QImage>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void clickWidget(QWidget* button) {
    const QPointF local = button->rect().center();
    const QPointF global = button->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(button, &press);
    QApplication::sendEvent(button, &release);
}
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool thumbIsOnRight(adqt::widgets::AdSwitch* toggle) {
    const QImage image = toggle->grab().toImage();
    int left = 0;
    int right = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y) == QColor(Qt::magenta)) {
                (x < image.width() / 2 ? left : right)++;
            }
        }
    }
    require(left + right > 0, "rendered switch contains the test thumb color");
    return right > left;
}
class FakeTranslationHotkeyBackend final : public snow_shot::presentation::GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    snow_shot::presentation::GlobalShortcutValidationResult
    validateShortcut(const snow_shot::shortcuts::ShortcutBinding& shortcut) const override {
        return {shortcut.portableText, true,
                snow_shot::presentation::GlobalShortcutFailureReason::None, shortcut};
    }
    snow_shot::presentation::GlobalShortcutBackendResult
    registerShortcut(int id, const snow_shot::shortcuts::ShortcutBinding& shortcut) override {
        if (registrations.values().contains(shortcut)) {
            return {false, snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse};
        }
        registrations.insert(id, shortcut);
        return {true};
    }
    void unregisterShortcut(int id) override {
        registrations.remove(id);
    }
    ActivationHandler handler;
    QHash<int, snow_shot::shortcuts::ShortcutBinding> registrations;
};

void requireCompactTrayOptions(QWidget* widget) {
    auto* options = widget->findChild<QWidget*>(QStringLiteral("settings-tray-menu-options-grid"));
    auto* grid = qobject_cast<QGridLayout*>(options->layout());
    require(grid != nullptr, "tray options use a grid");
    widget->resize(1000, widget->sizeHint().height());
    widget->show();
    QCoreApplication::processEvents();
    int row = 0;
    bool firstGroup = true;
    for (const auto& group :
         snow_shot::presentation::settings::builtInSettingsRegistry().catalog().trayMenuGroups()) {
        int visibleCount = 0;
        for (const auto& option : group.options) {
            auto* checkbox = widget->findChild<QAbstractButton*>(
                QStringLiteral("settings-tray-menu-option-%1").arg(option.id));
            require(checkbox != nullptr, "tray option retains its widget");
            if (checkbox->isHidden()) {
                continue;
            }
            if (visibleCount == 0 && !firstGroup) {
                ++row; // Group divider.
            }
            int actualRow = -1;
            int column = -1;
            int rowSpan = 0;
            int columnSpan = 0;
            grid->getItemPosition(grid->indexOf(checkbox), &actualRow, &column, &rowSpan,
                                  &columnSpan);
            require(actualRow == row + visibleCount / 2 && column == visibleCount % 2,
                    "visible tray options fill consecutive cells without hidden-option gaps");
            ++visibleCount;
        }
        if (visibleCount > 0) {
            row += (visibleCount + 1) / 2;
            firstGroup = false;
        }
    }
}

#ifdef Q_OS_WIN
void windowAlwaysOnTopShortcutSettings() {
    using namespace snow_shot::presentation;
    namespace storage = snow_shot::storage;
    constexpr auto action = GlobalShortcutAction::ToggleWindowUnderCursorAlwaysOnTop;
    const QString id = QStringLiteral("quick.toggle-window-under-cursor-always-on-top");
    const QString fullscreenKey =
        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
    auto& store = storage::ApplicationStorage::instance().configuration();
    const auto previousFullscreen = store.value(fullscreenKey);
    const storage::ShortcutSettings persisted;
    const snow_shot::shortcuts::ShortcutBindingList keys{QStringLiteral("Ctrl+Alt+F11"),
                                                         QStringLiteral("Ctrl+Alt+F12")};
    require(persisted.toggleWindowUnderCursorAlwaysOnTop().isEmpty() &&
                !storage::TraySettings().menuOptions().contains(id),
            "window-under-cursor topmost has neither a shortcut nor a tray entry by default");
    require(storage::ExtendedFeaturesSettings().setTranslationPageEnabled(false),
            "disable translation independently of the topmost action");
    {
        auto native = std::make_unique<FakeTranslationHotkeyBackend>();
        auto* input = native.get();
        bool fullscreen = false;
        GlobalShortcutManager manager(std::move(native), nullptr, [&] { return fullscreen; });
        settings::BuiltInSettingsBackend backend(manager);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        manager.initialize();
        require(session.state(id).visible && session.state(id).enabled &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "the topmost action is usable while translation is disabled");
        require(session.applyShortcuts(action, keys) &&
                    persisted.toggleWindowUnderCursorAlwaysOnTop() == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "both topmost bindings register and persist through settings");
        int activations = 0;
        QObject::connect(&manager, &GlobalShortcutManager::activated, &manager,
                         [&](GlobalShortcutAction received) {
                             require(received == action,
                                     "topmost bindings dispatch their own action");
                             ++activations;
                         });
        const auto activate = [&] {
            for (const auto& binding : manager.state(action).bindings) {
                for (auto it = input->registrations.cbegin(); it != input->registrations.cend();
                     ++it) {
                    if (it.value() == binding.binding)
                        input->handler(it.key());
                }
            }
        };
        activate();
        require(activations == 2, "each assigned topmost shortcut activates once");
        manager.setGlobalHotkeysEnabled(false);
        require(manager.state(action).status == GlobalShortcutStatus::Disabled,
                "manual global suppression disables topmost shortcuts");
        manager.setGlobalHotkeysEnabled(true);
        require(store.setValue(fullscreenKey, true), "enable fullscreen suppression");
        fullscreen = true;
        manager.refreshFocusedFullscreenState();
        require(manager.state(action).status == GlobalShortcutStatus::Disabled,
                "fullscreen suppression also disables topmost shortcuts");
        fullscreen = false;
        manager.refreshFocusedFullscreenState();
        require(manager.state(action).status == GlobalShortcutStatus::Registered,
                "leaving fullscreen restores topmost shortcuts");
    }
    {
        GlobalShortcutManager manager(std::make_unique<FakeTranslationHotkeyBackend>(), nullptr,
                                      [] { return false; });
        settings::BuiltInSettingsBackend backend(manager);
        manager.initialize();
        require(manager.state(action).shortcuts == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "topmost bindings survive manager recreation");
        require(backend.resetSection(settings::SettingsSectionReset::OtherShortcuts) &&
                    persisted.toggleWindowUnderCursorAlwaysOnTop().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Other reset clears topmost shortcuts and their native registrations");
    }
    require(store.setValue(fullscreenKey, previousFullscreen), "restore fullscreen preference");
}
#endif

void selectedTextShortcutSettings() {
    using namespace snow_shot::presentation;
    namespace storage = snow_shot::storage;
    const auto action = GlobalShortcutAction::TranslateSelectedText;
    const storage::ShortcutSettings persisted;
    require(persisted.translateSelectedText().isEmpty(),
            "selected text shortcut is unassigned by default");
    const storage::TraySettings tray;
    const QStringList defaultMenu = tray.menuOptions();
    const QString menuId = QStringLiteral("quick.translate-selected-text");
    require(!defaultMenu.contains(menuId), "selected text translation is optional in the tray");
    QStringList menuWithTranslation = defaultMenu;
    menuWithTranslation.append(menuId);
    require(tray.setMenuOptions(menuWithTranslation) && tray.menuOptions().contains(menuId),
            "tray settings allow the selected text translation action");
    require(tray.setMenuOptions(defaultMenu), "restore default tray actions");
    const snow_shot::shortcuts::ShortcutBindingList keys{QStringLiteral("Ctrl+Alt+T"),
                                                         QStringLiteral("Ctrl+Shift+T")};
    {
        auto native = std::make_unique<FakeTranslationHotkeyBackend>();
        auto* input = native.get();
        bool fullscreen = false;
        GlobalShortcutManager manager(std::move(native), nullptr, [&]() { return fullscreen; });
        settings::BuiltInSettingsBackend backend(manager);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        manager.initialize();
        const auto standaloneBinding = settings::SettingsSwitchBinding::StandaloneTranslationWindow;
        const QString standaloneId =
            QStringLiteral("extended-features.standalone-translation-window");
        const auto jumpBinding = settings::SettingsSwitchBinding::JumpToTranslationPage;
        const QString jumpId = QStringLiteral("extended-features.jump-to-translation-page");
        require(!storage::ExtendedFeaturesSettings().standaloneTranslationWindow() &&
                    session.state(standaloneId).visible && !session.state(standaloneId).enabled &&
                    !backend.applySwitchValue(standaloneBinding, true),
                "standalone defaults off and is visible but disabled under master off");
        require(!storage::ExtendedFeaturesSettings().jumpToTranslationPage() &&
                    session.state(jumpId).visible && !session.state(jumpId).enabled &&
                    !backend.applySwitchValue(jumpBinding, true),
                "OCR translation jump defaults off and is disabled under master off");
        require(manager.state(action).status == GlobalShortcutStatus::Unset,
                "unassigned action does not register a native shortcut");
        require(!storage::ExtendedFeaturesSettings().translationPageEnabled(),
                "translation page defaults off");
        require(!session.state(QStringLiteral("quick.translate-selected-text")).visible,
                "selected text shortcut is hidden while disabled");
        require(tray.setMenuOptions(menuWithTranslation),
                "preserve selected translation tray preference");
        QCoreApplication::processEvents();
        const auto& registry = settings::builtInSettingsRegistry();
        std::unique_ptr<SettingsCustomWidget> trayWidget(createSettingsCustomWidget(
            settings::SettingsCustomRenderer::TrayMenuOptions, registry,
            *registry.field(QStringLiteral("tray.menu-options"))->definition, session));
        auto* translationCheckbox = trayWidget->findChild<QAbstractButton*>(
            QStringLiteral("settings-tray-menu-option-quick.translate-selected-text"));
        auto* screenshotCheckbox = trayWidget->findChild<QAbstractButton*>(
            QStringLiteral("settings-tray-menu-option-quick.screenshot"));
        require(translationCheckbox != nullptr && screenshotCheckbox != nullptr &&
                    translationCheckbox->isHidden() && translationCheckbox->isChecked(),
                "disabled tray customization hides translation but retains its checked preference");
        requireCompactTrayOptions(trayWidget.get());
        screenshotCheckbox->setChecked(!screenshotCheckbox->isChecked());
        require(tray.menuOptions().contains(menuId),
                "editing another tray option preserves the hidden translation preference");
        manager.setShortcuts(action, keys);
        require(persisted.translateSelectedText() == keys &&
                    !input->registrations.values().contains(keys.first()),
                "disabled feature preserves configured keys without native registration");
        require(
            backend.applySwitchValue(settings::SettingsSwitchBinding::TranslationPageEnabled, true),
            "enable translation page through settings backend");
        QCoreApplication::processEvents();
        require(backend.applySwitchValue(jumpBinding, true),
                "enable OCR translation jump before category reset");
        require(!translationCheckbox->isHidden() && translationCheckbox->isChecked(),
                "live enabling restores the selected tray customization checkbox");
        requireCompactTrayOptions(trayWidget.get());
        require(tray.setMenuOptions(defaultMenu),
                "restore tray configuration after feature checks");
        require(session.state(QStringLiteral("quick.translate-selected-text")).visible,
                "enabling feature reveals shortcut controls");
        require(session.applyShortcuts(action, keys) && persisted.translateSelectedText() == keys &&
                    manager.state(action).status == GlobalShortcutStatus::Registered,
                "editing selected text bindings updates storage and native registrations");
        {
            SettingsPageWidget page(registry, QStringLiteral("text-recognition-translation"),
                                    session);
            page.resize(880, 760);
            page.show();
            page.reveal({page.pageId(), QStringLiteral("translation"),
                         QStringLiteral("extended-features.translation-page")});
            QCoreApplication::processEvents();
            auto* header = page.findChild<SectionHeaderWidget*>(
                QStringLiteral("settings-section-text-recognition-translation-translation"));
            require(header != nullptr, "translation section exposes its own header");
            auto* reset =
                header->findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionResetButton"));
            auto* confirmation = header->findChild<adqt::widgets::AdPopconfirm*>();
            auto* toggle = page.findChild<adqt::widgets::AdSwitch*>(
                QStringLiteral("settings-control-extended-features-translation-page"));
            require(reset != nullptr && reset->isVisible() && reset->isEnabled() &&
                        confirmation != nullptr && toggle != nullptr && toggle->isChecked() &&
                        reset->geometry().right() == header->contentsRect().right(),
                    "extended category uses the shared right-aligned reset and generated toggle");
            adqt::widgets::AdSwitch::ComponentTokens tokens;
            tokens.colors.thumb = QColor(Qt::magenta);
            toggle->setComponentTokens(tokens);
            require(thumbIsOnRight(toggle), "enabled translation renders the thumb on the right");
            clickWidget(reset);
            QCoreApplication::processEvents();
            require(confirmation->isVisible(), "extended reset opens shared confirmation");
            confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Cancel)->click();
            require(storage::ExtendedFeaturesSettings().translationPageEnabled() &&
                        storage::ExtendedFeaturesSettings().jumpToTranslationPage(),
                    "cancel extended category reset preserves feature opt-ins");
            clickWidget(reset);
            QCoreApplication::processEvents();
            confirmation->button(adqt::widgets::AdPopconfirm::StandardButton::Ok)->click();
            QCoreApplication::processEvents();
            require(!storage::ExtendedFeaturesSettings().translationPageEnabled() &&
                        !storage::ExtendedFeaturesSettings().jumpToTranslationPage() &&
                        !toggle->isChecked() &&
                        !session.state(QStringLiteral("extended-features.translation-page"))
                             .acceptedValue.toBool() &&
                        !session.state(QStringLiteral("quick.translate-selected-text")).visible &&
                        !input->registrations.values().contains(keys.first()) &&
                        persisted.translateSelectedText() == keys,
                    "reset disables translation, refreshes UI and hotkeys, and preserves bindings");
            require(!thumbIsOnRight(toggle), "reset moves the translation thumb to the left");
            requireCompactTrayOptions(trayWidget.get());
            for (const bool enabled : {true, false, true}) {
                clickWidget(toggle);
                QCoreApplication::processEvents();
                require(toggle->isChecked() == enabled && thumbIsOnRight(toggle) == enabled &&
                            storage::ExtendedFeaturesSettings().translationPageEnabled() == enabled,
                        "clicking translation after reset updates both rendering and storage");
                requireCompactTrayOptions(trayWidget.get());
            }
            QCoreApplication::processEvents();
        }
        require(session.state(standaloneId).enabled && session.state(jumpId).enabled,
                "master on enables both dependent translation switches live");
        require(backend.applySwitchValue(jumpBinding, true) &&
                    storage::ExtendedFeaturesSettings().jumpToTranslationPage(),
                "OCR translation jump persists through the settings backend");
        for (const bool enabled : {true, false, true}) {
            require(backend.applySwitchValue(standaloneBinding, enabled) &&
                        storage::ExtendedFeaturesSettings().standaloneTranslationWindow() ==
                            enabled,
                    "standalone switch persists through backend");
            QCoreApplication::processEvents();
            require(manager.state(action).status == GlobalShortcutStatus::Registered &&
                        session.state(QStringLiteral("quick.translate-selected-text")).visible &&
                        !translationCheckbox->isHidden(),
                    "standalone flag never gates shortcut or tray availability");
        }
        int activations = 0;
        QObject::connect(
            &manager, &GlobalShortcutManager::activated, &manager,
            [&](GlobalShortcutAction activated) { activations += activated == action; });
        int id = input->registrations.key(keys.first());
        require(id != 0, "selected text shortcut has a native registration");
        input->handler(id);
        require(activations == 1, "native activation dispatches selected text translation");
        require(storage::ExtendedFeaturesSettings().setTranslationPageEnabled(false),
                "disable feature live");
        QCoreApplication::processEvents();
        require(!session.state(standaloneId).enabled && !session.state(jumpId).enabled &&
                    storage::ExtendedFeaturesSettings().standaloneTranslationWindow() &&
                    storage::ExtendedFeaturesSettings().jumpToTranslationPage(),
                "master off disables child controls without clearing either preference");
        input->handler(id);
        require(activations == 1 && !input->registrations.values().contains(keys.first()) &&
                    persisted.translateSelectedText() == keys,
                "disable unregisters and suppresses stale activation without deleting keys");
        require(storage::ExtendedFeaturesSettings().setTranslationPageEnabled(true),
                "re-enable feature live");
        require(input->registrations.values().contains(keys.first()),
                "re-enable restores native registration");
        id = input->registrations.key(keys.first());
        manager.setGlobalHotkeysEnabled(false);
        require(
            !input->registrations.values().contains(keys.first()) &&
                !input->registrations.values().contains(keys.last()) &&
                persisted.translateSelectedText() == keys,
            "manual hotkey disablement releases both selected text bindings and preserves keys");
        input->handler(id);
        require(activations == 1, "disabled global hotkeys suppress selected text translation");
        manager.setGlobalHotkeysEnabled(true);
        const int restoredId = input->registrations.key(keys.first());
        require(restoredId != 0 && restoredId != id &&
                    input->registrations.values().contains(keys.last()),
                "manual re-enablement restores both selected text bindings with fresh IDs");
        input->handler(id);
        require(activations == 1, "manual re-enablement must keep the released ID inactive");
        id = restoredId;
        input->handler(id);
        require(activations == 2, "manual re-enablement restores selected text translation");
        auto& store = storage::ApplicationStorage::instance().configuration();
        const QString fullscreenKey =
            QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window");
        const auto previousFullscreen = store.value(fullscreenKey);
        require(store.setValue(fullscreenKey, true), "enable fullscreen suppression");
        fullscreen = true;
        manager.refreshFocusedFullscreenState();
        require(!input->registrations.values().contains(keys.first()) &&
                    !input->registrations.values().contains(keys.last()) &&
                    persisted.translateSelectedText() == keys,
                "focused fullscreen releases both selected text bindings and preserves keys");
        input->handler(id);
        require(activations == 2, "fullscreen suppression ignores released translation IDs");
        fullscreen = false;
        manager.refreshFocusedFullscreenState();
        const int fullscreenRestoredId = input->registrations.key(keys.first());
        require(fullscreenRestoredId != 0 && fullscreenRestoredId != id &&
                    input->registrations.values().contains(keys.last()),
                "leaving fullscreen restores both selected text bindings with fresh IDs");
        input->handler(id);
        require(activations == 2, "leaving fullscreen must keep the released ID inactive");
        input->handler(fullscreenRestoredId);
        require(activations == 3, "leaving fullscreen restores selected text translation");
        require(store.setValue(fullscreenKey, previousFullscreen), "restore fullscreen preference");
        const auto conflict = persisted.pinClipboardContent();
        require(!conflict.isEmpty(), "pin clipboard default provides a conflict fixture");
        manager.setShortcuts(action, {conflict.first()});
        require(manager.state(action).status == GlobalShortcutStatus::Failed &&
                    manager.state(action).bindings.first().failureReason ==
                        GlobalShortcutFailureReason::AlreadyInUse,
                "selected text shortcut reports native conflicts through existing status");
        require(session.reset(settings::SettingsSectionReset::OtherShortcuts) &&
                    persisted.translateSelectedText().isEmpty() &&
                    manager.state(action).status == GlobalShortcutStatus::Unset,
                "Other reset clears selected text bindings and unregisters them");
        require(session.applyShortcuts(action, keys), "prepare selected text bindings for reload");
    }
    {
        GlobalShortcutManager reloaded(std::make_unique<FakeTranslationHotkeyBackend>(), nullptr,
                                       []() { return false; });
        reloaded.initialize();
        require(reloaded.state(action).shortcuts == keys &&
                    reloaded.state(action).status == GlobalShortcutStatus::Registered,
                "a recreated shortcut manager loads both persisted selected text bindings");
        reloaded.setShortcuts(action, {});
    }
}
void ocrHighlightingDefaultsPersistsAndResets() {
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    auto& configuration = applicationStorage.configuration();
    snow_shot::presentation::GlobalShortcutManager shortcuts(
        std::make_unique<FakeTranslationHotkeyBackend>());
    settings::BuiltInSettingsBackend backend(shortcuts);
    const storage::TextRecognitionSettings recognition;
    constexpr auto binding = settings::SettingsSwitchBinding::ShowTextHighlighting;
    const QString key = QStringLiteral("text_recognition/show_text_highlighting");
    const QString itemId = QStringLiteral("interface.text-recognition.show-text-highlighting");
    const QString fillKey = QStringLiteral("text_recognition/fill_style");
    require(backend.switchEnabled(binding) && !backend.switchValue(binding) &&
                !recognition.showTextHighlighting(),
            "OCR highlighting must default off through the backend and typed settings");
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    SettingsPageWidget page(settings::builtInSettingsRegistry(),
                            QStringLiteral("text-recognition-translation"), session);
    page.resize(880, 760);
    page.show();
    page.reveal({page.pageId(), QStringLiteral("interface-text-recognition"), itemId});
    QCoreApplication::processEvents();
    auto* toggle = page.findChild<adqt::widgets::AdSwitch*>(
        QStringLiteral("settings-control-interface-text-recognition-show-text-highlighting"));
    require(toggle != nullptr,
            "revealing Text background must materialize its highlighting switch");
    require(!toggle->isChecked() && toggle->isEnabled() &&
                !session.state(itemId).acceptedValue.toBool() && session.state(itemId).enabled,
            "OCR highlighting must appear as an enabled, default-off settings switch");
    require(session.submitDraft(itemId, true) && backend.switchValue(binding) &&
                recognition.showTextHighlighting() && configuration.flushNow().success,
            "the highlighting switch must persist enabled through the runtime session");
    QCoreApplication::processEvents();
    require(toggle->isChecked(), "runtime highlighting changes must update the generated switch");
    storage::ConfigurationStore reloaded(
        QDir(applicationStorage.configurationDirectory()).filePath(QStringLiteral("config.json")),
        true, false, 60000);
    require(reloaded.value(key) == QJsonValue(true),
            "enabled OCR highlighting must survive configuration reload");
    toggle->click();
    QCoreApplication::processEvents();
    require(!recognition.showTextHighlighting() && !session.state(itemId).acceptedValue.toBool() &&
                !toggle->isChecked(),
            "clicking the highlighting switch must disable its runtime and stored values");
    require(recognition.setShowTextHighlighting(true),
            "enable highlighting outside the settings UI");
    QCoreApplication::processEvents();
    require(session.state(itemId).acceptedValue.toBool() && toggle->isChecked(),
            "external highlighting changes must update the runtime session and switch live");
    require(!configuration.setValue(key, 1) && recognition.showTextHighlighting(),
            "nonboolean configuration writes must not replace enabled highlighting");
    require(configuration.setValue(fillKey, QStringLiteral("blur")) &&
                session.reset(settings::SettingsSectionReset::TextRecognitionBehavior),
            "reset recognition behavior while custom appearance is enabled");
    QCoreApplication::processEvents();
    require(recognition.showTextHighlighting() && toggle->isChecked() &&
                configuration.value(fillKey) == QJsonValue(QStringLiteral("blur")),
            "recognition behavior reset must preserve OCR highlighting and fill style");
    require(session.reset(settings::SettingsSectionReset::TextRecognitionInterfaceSettings),
            "reset Text background appearance");
    QCoreApplication::processEvents();
    require(!backend.switchValue(binding) && !recognition.showTextHighlighting() &&
                !session.state(itemId).acceptedValue.toBool() && !toggle->isChecked() &&
                configuration.value(fillKey) == QJsonValue(QStringLiteral("background_fill")),
            "appearance reset must restore disabled highlighting and Background Fill live");
    require(backend.applySwitchValue(binding, true) && backend.switchValue(binding) &&
                backend.applySwitchValue(binding, false) && !backend.switchValue(binding),
            "direct backend writes must support both highlighting states");
}

void originalImagePreviewDefaultsPersistsAndResets() {
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const storage::TextRecognitionSettings recognition;
    constexpr auto binding = settings::SettingsSwitchBinding::ShowOriginalImagePreview;
    const QString key = QStringLiteral("text_recognition/show_original_image_preview");
    const QString itemId = QStringLiteral("text-recognition.show-original-image-preview");
    require(storage::ConfigurationSchema::defaultValue(key).toBool() &&
                backend.switchEnabled(binding) && backend.switchValue(binding) &&
                recognition.showOriginalImagePreview(),
            "original image preview must default on through schema, backend and typed settings");
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(session.state(itemId).acceptedValue.toBool() && session.state(itemId).enabled,
            "original image preview is an enabled function settings switch");
    require(session.submitDraft(itemId, false) && !backend.switchValue(binding) &&
                !recognition.showOriginalImagePreview() &&
                applicationStorage.configuration().flushNow().success,
            "the preview switch must persist a disabled value through the runtime session");
    storage::ConfigurationStore reloaded(
        QDir(applicationStorage.configurationDirectory()).filePath(QStringLiteral("config.json")),
        true, true, 60000);
    require(!reloaded.value(key).toBool(), "disabled preview survives configuration reload");
    const QString archivePath =
        QDir(applicationStorage.configurationDirectory()).filePath(QStringLiteral("preview.zip"));
    require(storage::ConfigurationArchive::write(
                archivePath, {{key, false}}, storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "the preview preference must be exportable in configuration archives");
    const auto imported = storage::ConfigurationArchive::read(archivePath);
    require(imported.isValid() && imported.values.contains(key) &&
                !imported.values.value(key).toBool() &&
                reloaded.applySnapshot(imported.values, imported.schemaVersion) &&
                !reloaded.value(key).toBool(),
            "imported configuration restores the disabled preview preference");
    require(reloaded.applySnapshot({{QStringLiteral("text_recognition/default_formatting"),
                                     QStringLiteral("keep")}}) &&
                reloaded.value(key).toBool(),
            "older configuration snapshots missing the preference use the enabled default");
    require(!applicationStorage.configuration().setValue(key, QStringLiteral("false")) &&
                !recognition.showOriginalImagePreview(),
            "the preview setting must reject nonboolean writes without changing the preference");
    require(session.reset(settings::SettingsSectionReset::TextRecognitionBehavior) &&
                backend.switchValue(binding) && recognition.showOriginalImagePreview(),
            "resetting Text Recognition behavior restores the enabled preview default");
    require(recognition.setShowOriginalImagePreview(false), "typed preview setter disables");
    session.refreshAll();
    require(!session.state(itemId).acceptedValue.toBool(),
            "externally changed preview preference refreshes the settings switch");
    require(recognition.setShowOriginalImagePreview(true), "restore enabled preview default");
}

void textDetectionProcessingDefaultsPersistsAndResets() {
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    auto& configuration = applicationStorage.configuration();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    constexpr auto binding = settings::SettingsSelectBinding::OcrTextDetectionProcessing;
    const QString key = QStringLiteral("text_recognition/text_detection_processing");
    const QString itemId = QStringLiteral("text-recognition.text-detection-processing");
    require(backend.selectValue(binding).toString() == QStringLiteral("accuracy_first") &&
                session.state(itemId).acceptedValue.toString() ==
                    QStringLiteral("accuracy_first") &&
                session.state(itemId).enabled,
            "OCR processing defaults to enabled Accuracy First through the backend and session");
    require(session.submitDraft(itemId, QStringLiteral("speed_first")) &&
                backend.selectValue(binding).toString() == QStringLiteral("speed_first") &&
                configuration.value(key).toString() == QStringLiteral("speed_first") &&
                configuration.flushNow().success,
            "Speed First must persist through the runtime settings session");
    storage::ConfigurationStore reloaded(
        QDir(applicationStorage.configurationDirectory()).filePath(QStringLiteral("config.json")),
        true, false, 60000);
    require(reloaded.value(key).toString() == QStringLiteral("speed_first"),
            "OCR processing must survive configuration reload");
    require(!backend.applySelectValue(binding, QStringLiteral("unsupported")) &&
                backend.selectValue(binding).toString() == QStringLiteral("speed_first"),
            "unsupported OCR processing writes must retain the saved choice");
    require(configuration.setValue(key, QStringLiteral("accuracy_first")),
            "an external configuration change must update OCR processing");
    QCoreApplication::processEvents();
    require(session.state(itemId).acceptedValue.toString() == QStringLiteral("accuracy_first"),
            "the OCR processing selector must watch external configuration changes");
    require(session.submitDraft(itemId, QStringLiteral("speed_first")) &&
                session.reset(settings::SettingsSectionReset::TextRecognition),
            "the OCR performance category reset must succeed");
    QCoreApplication::processEvents();
    require(backend.selectValue(binding).toString() == QStringLiteral("accuracy_first") &&
                session.state(itemId).acceptedValue.toString() == QStringLiteral("accuracy_first"),
            "resetting OCR performance must restore Accuracy First in the backend and session");
    storage::ConfigurationStore imported(
        QDir(applicationStorage.configurationDirectory())
            .filePath(QStringLiteral("ocr-processing-import.json")),
        false, true, 60000);
    require(imported.applySnapshot({{key, QStringLiteral("unsupported")}}) &&
                imported.value(key).toString() == QStringLiteral("accuracy_first"),
            "invalid imported OCR processing values must fall back to Accuracy First");
    require(imported.setValue(key, QStringLiteral("speed_first")) && imported.applySnapshot({}) &&
                imported.value(key).toString() == QStringLiteral("accuracy_first"),
            "older configuration snapshots missing OCR processing must use Accuracy First");
}
void languageDetectionConfidenceDefaultsPersistsAndResets() {
    namespace settings = snow_shot::presentation::settings;
    namespace storage = snow_shot::storage;
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const storage::ScreenshotTranslationSettings translation;
    const auto binding =
        settings::SettingsIntegerBinding::TranslationLanguageDetectionConfidenceThreshold;
    const QString id = QStringLiteral("translation.language-detection-confidence-threshold");
    require(backend.integerValue(binding) == 60 && session.state(id).acceptedValue.toInt() == 60 &&
                session.applyIntegerValue(binding, 30) && backend.integerValue(binding) == 30 &&
                session.applyIntegerValue(binding, 100) &&
                !backend.applyIntegerValue(binding, 29) &&
                !backend.applyIntegerValue(binding, 101) &&
                translation.configuration().languageDetectionConfidenceThreshold == 100,
            "confidence settings expose the default and persist only percentages in range");
    require(backend.resetSection(settings::SettingsSectionReset::Translation),
            "reset translation confidence");
    session.refreshAll();
    require(backend.integerValue(binding) == 60 && session.state(id).acceptedValue.toInt() == 60 &&
                translation.configuration().languageDetectionConfidenceThreshold == 60,
            "translation reset restores the confidence threshold across settings views");
    const QString key =
        QStringLiteral("screenshot_translation/language_detection_confidence_threshold");
    storage::ConfigurationStore imported(
        QDir(storage::ApplicationStorage::instance().configurationDirectory())
            .filePath(QStringLiteral("translation-confidence-import.json")),
        false, true, 60000);
    require(imported.applySnapshot({{key, 29}}) && imported.value(key).toInt() == 60 &&
                imported.setValue(key, 90) && imported.applySnapshot({}) &&
                imported.value(key).toInt() == 60,
            "invalid and older imported configurations use the default confidence threshold");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    auto themeConfig = adqt::theme::ThemeManager::instance().config();
    themeConfig.motion = false;
    adqt::theme::ThemeManager::instance().setConfig(themeConfig);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated translation settings storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create translation settings executable directory");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize translation settings storage");
#ifdef Q_OS_WIN
    if (application.arguments().contains(QStringLiteral("--window-always-on-top-only"))) {
        windowAlwaysOnTopShortcutSettings();
        applicationStorage.shutdown();
        return 0;
    }
#endif
    if (application.arguments().contains(QStringLiteral("--translation-confidence-only"))) {
        languageDetectionConfidenceDefaultsPersistsAndResets();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-highlighting-only"))) {
        ocrHighlightingDefaultsPersistsAndResets();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--original-image-preview-only"))) {
        originalImagePreviewDefaultsPersistsAndResets();
        applicationStorage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--text-detection-processing-only"))) {
        textDetectionProcessingDefaultsPersistsAndResets();
        applicationStorage.shutdown();
        return 0;
    }
    ocrHighlightingDefaultsPersistsAndResets();
    originalImagePreviewDefaultsPersistsAndResets();
    textDetectionProcessingDefaultsPersistsAndResets();
    languageDetectionConfidenceDefaultsPersistsAndResets();
    selectedTextShortcutSettings();
    {
        snow_shot::presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        require(storage::RecordingSettings().setMicrophoneGainDb(-12) &&
                    storage::RecordingSettings().setSystemAudioGainDb(9) &&
                    backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    storage::RecordingSettings().microphoneGainDb() == 0 &&
                    storage::RecordingSettings().systemAudioGainDb() == 0,
                "recording section reset restores both gains to unity");
        const auto separateAudio = settings::SettingsSwitchBinding::SeparateRecordingAudioTracks;
        require(backend.switchEnabled(separateAudio) && !backend.switchValue(separateAudio) &&
                    backend.applySwitchValue(separateAudio, true) &&
                    storage::RecordingSettings().separateAudioTracks() &&
                    backend.switchValue(separateAudio),
                "separate audio tracks default off and persist enabled");
        require(backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    !backend.switchValue(separateAudio) &&
                    !storage::RecordingSettings().separateAudioTracks(),
                "screen recording reset restores mixed audio");
        const storage::RecordingSettings recording;
        require(recording.setPostProcessingEnabled(true) &&
                    recording.setPostProcessingEffect(QStringLiteral("playback_time")) &&
                    recording.setProgressBarColor(QColor(12, 34, 56, 78)) &&
                    backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    !recording.postProcessingEnabled() &&
                    recording.postProcessingEffect() == QStringLiteral("progress_bar") &&
                    recording.progressBarColor() == QColor(22, 119, 255),
                "screen recording reset restores automatic real-time mode and overlay defaults");
        const auto loopImages = settings::SettingsSwitchBinding::LoopAnimatedImages;
        require(backend.switchEnabled(loopImages) && backend.switchValue(loopImages) &&
                    backend.applySwitchValue(loopImages, false) &&
                    !storage::RecordingSettings().loopAnimatedImages() &&
                    !backend.switchValue(loopImages),
                "animated image loop switch defaults on and persists disabled");
        require(backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                    backend.switchValue(loopImages),
                "screen recording reset restores animated image looping");
        const auto recognitionSave = settings::SettingsSwitchBinding::SaveRecognitionResultAsImage;
        require(backend.switchValue(recognitionSave), "recognition image export defaults on");
        require(backend.applySwitchValue(recognitionSave, false) &&
                    !storage::TextRecognitionSettings().saveRecognitionResultAsImage(),
                "recognition image export persists disabled setting");
        const auto defaultFormatting = settings::SettingsSelectBinding::OcrDefaultFormatting;
        const auto defaultPunctuation = settings::SettingsSelectBinding::OcrDefaultPunctuation;
        require(backend.selectValue(defaultFormatting).toString() == QStringLiteral("none") &&
                    backend.selectValue(defaultPunctuation).toString() == QStringLiteral("none") &&
                    backend.applySelectValue(defaultFormatting, QStringLiteral("remove")) &&
                    backend.applySelectValue(defaultPunctuation, QStringLiteral("full")) &&
                    storage::TextRecognitionSettings().defaultFormatting() ==
                        QStringLiteral("remove") &&
                    storage::TextRecognitionSettings().defaultPunctuation() ==
                        QStringLiteral("full"),
                "recognized-text defaults persist through the settings backend");
        require(backend.applySelectValue(defaultFormatting, QStringLiteral("smart")) &&
                    backend.selectValue(defaultFormatting).toString() == QStringLiteral("smart") &&
                    storage::TextRecognitionSettings().defaultFormatting() ==
                        QStringLiteral("smart"),
                "Smart Typesetting persists through the settings backend and schema");
        const auto oldFill =
            applicationStorage.configuration().value(QStringLiteral("text_recognition/fill_style"));
        require(applicationStorage.configuration().setValue(
                    QStringLiteral("text_recognition/fill_style"), QStringLiteral("blur")) &&
                    backend.resetSection(settings::SettingsSectionReset::TextRecognitionBehavior) &&
                    backend.switchValue(recognitionSave) &&
                    backend.selectValue(defaultFormatting).toString() == QStringLiteral("none") &&
                    backend.selectValue(defaultPunctuation).toString() == QStringLiteral("none") &&
                    applicationStorage.configuration().value(
                        QStringLiteral("text_recognition/fill_style")) == QStringLiteral("blur"),
                "recognition behavior reset restores defaults without changing appearance");
        require(applicationStorage.configuration().setValue(
                    QStringLiteral("text_recognition/fill_style"), oldFill),
                "restore recognition appearance fixture");

        const auto binding = settings::SettingsSwitchBinding::OriginalImageTranslation;
        const storage::ScreenshotTranslationSettings translation;
        const auto copyImageBinding =
            settings::SettingsSwitchBinding::CopyTranslationResultsAsImage;
        require(backend.switchEnabled(copyImageBinding) && !backend.switchValue(copyImageBinding) &&
                    backend.applySwitchValue(copyImageBinding, true) &&
                    translation.copyTranslationResultsAsImage(),
                "translation image copy defaults off and persists through its switch binding");
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        const auto resident = settings::SettingsSwitchBinding::OcrResidentProcess;
        const auto hot = settings::SettingsSwitchBinding::OcrModelHotStart;
        const auto acceleration = settings::SettingsSwitchBinding::OcrHardwareAcceleration;
        const QString accelerationId = QStringLiteral("text-recognition.hardware-acceleration");
        require(!backend.switchValue(acceleration) && session.state(accelerationId).enabled,
                "hardware acceleration defaults off and remains available without a GPU probe");
        require(backend.applySwitchValue(acceleration, true) &&
                    applicationStorage.configuration()
                        .value(QStringLiteral("text_recognition/hardware_acceleration"))
                        .toBool(),
                "hardware acceleration persists without preparing OCR resources");
        QCoreApplication::processEvents();
        require(session.state(accelerationId).acceptedValue.toBool() &&
                    backend.switchValue(resident) && !backend.switchValue(hot),
                "hardware acceleration is independent of residency and model hot start");
        const QString hotId = QStringLiteral("text-recognition.model-hot-start");
        const QString residentId = QStringLiteral("text-recognition.resident-process");
        require(backend.switchValue(resident) && !backend.switchValue(hot) &&
                    session.state(hotId).enabled && session.state(residentId).enabled &&
                    session.state(residentId).acceptedValue.toBool(),
                "OCR residency defaults on and hot start is available but unchecked");
        require(backend.applySwitchValue(hot, true),
                "OCR settings must persist without attempting resource preparation");
        QCoreApplication::processEvents();
        require(session.state(hotId).enabled && session.state(hotId).acceptedValue.toBool(),
                "model hot start can be enabled with default residency");
        require(backend.applySwitchValue(resident, false), "resident preference can be disabled");
        QCoreApplication::processEvents();
        require(!session.state(hotId).enabled && session.state(hotId).acceptedValue.toBool() &&
                    backend.switchValue(hot) && session.state(hotId).error.isEmpty() &&
                    session.state(residentId).error.isEmpty(),
                "resident-off retains the checked hot-start value without a settings failure");
        require(backend.applySwitchValue(resident, true), "resident preference can be restored");
        QCoreApplication::processEvents();
        require(session.state(hotId).enabled && session.state(hotId).acceptedValue.toBool(),
                "restoring residency restores the saved hot-start preference");
        require(backend.applySwitchValue(resident, false),
                "disable residency before resetting OCR preferences");
        bool resetObserved = false;
        auto resetConnection = QObject::connect(
            &applicationStorage.configuration(), &storage::ConfigurationStore::valueChanged,
            &application, [&](const QString& key, const QJsonValue&) {
                if (key == QStringLiteral("text_recognition/resident_process") ||
                    key == QStringLiteral("text_recognition/model_hot_start")) {
                    resetObserved = true;
                    require(backend.switchValue(resident) && !backend.switchValue(hot),
                            "all observers must see both values reset atomically");
                }
            });
        require(backend.resetSection(settings::SettingsSectionReset::TextRecognition),
                "OCR category reset must succeed");
        QObject::disconnect(resetConnection);
        QCoreApplication::processEvents();
        require(resetObserved && backend.switchValue(resident) && !backend.switchValue(hot) &&
                    session.state(residentId).acceptedValue.toBool() &&
                    session.state(hotId).enabled && !backend.switchValue(acceleration) &&
                    !session.state(accelerationId).acceptedValue.toBool(),
                "OCR category reset must restore residency and clear hot start and acceleration");
        const auto pinBinding = settings::SettingsSelectBinding::PinDoubleClickAction;
        const QString pinId = QStringLiteral("pin-to-screen.double-click-action");
        require(backend.selectValue(pinBinding).toString() == QStringLiteral("thumbnail_mode") &&
                    session.state(pinId).enabled,
                "pinned double-click defaults to the enabled thumbnail selector");
        for (const QString& action :
             {QStringLiteral("none"), QStringLiteral("reset_zoom"),
              QStringLiteral("thumbnail_mode"), QStringLiteral("hide_to_top"),
              QStringLiteral("show_text_recognition_results"), QStringLiteral("close"),
              QStringLiteral("destroy")}) {
            require(backend.applySelectValue(pinBinding, action) &&
                        backend.selectValue(pinBinding).toString() == action,
                    "pinned double-click backend must apply every option");
        }
        require(!backend.applySelectValue(pinBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(pinBinding).toString() == QStringLiteral("destroy"),
                "pinned double-click backend must reject invalid actions");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(pinBinding).toString() == QStringLiteral("thumbnail_mode"),
                "resetting pin behavior must restore thumbnail double-click");
        const auto middleBinding = settings::SettingsSelectBinding::PinMiddleClickAction;
        const QString middleId = QStringLiteral("pin-to-screen.middle-mouse-button-action");
        require(backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom") &&
                    session.state(middleId).enabled,
                "pinned middle-click defaults to the enabled reset zoom selector");
        for (const QString& action :
             {QStringLiteral("none"), QStringLiteral("reset_zoom"),
              QStringLiteral("thumbnail_mode"), QStringLiteral("hide_to_top"),
              QStringLiteral("show_text_recognition_results"), QStringLiteral("close"),
              QStringLiteral("destroy")}) {
            require(backend.applySelectValue(middleBinding, action) &&
                        backend.selectValue(middleBinding).toString() == action,
                    "pinned middle-click backend must apply every option");
        }
        require(!backend.applySelectValue(middleBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("destroy"),
                "pinned middle-click backend must reject invalid actions");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom"),
                "resetting pin behavior must restore reset zoom middle-click");
        require(
            session.applySelectValue(pinBinding, QStringLiteral("show_text_recognition_results")) &&
                backend.selectValue(pinBinding).toString() ==
                    QStringLiteral("show_text_recognition_results") &&
                backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom") &&
                session.applySelectValue(middleBinding,
                                         QStringLiteral("show_text_recognition_results")) &&
                backend.selectValue(pinBinding).toString() ==
                    QStringLiteral("show_text_recognition_results") &&
                backend.selectValue(middleBinding).toString() ==
                    QStringLiteral("show_text_recognition_results"),
            "runtime selectors must independently apply text recognition");
        require(backend.resetSection(settings::SettingsSectionReset::PinToScreenBehavior) &&
                    backend.selectValue(pinBinding).toString() ==
                        QStringLiteral("thumbnail_mode") &&
                    backend.selectValue(middleBinding).toString() == QStringLiteral("reset_zoom"),
                "resetting recognition mouse actions must restore both defaults");
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
        require(backend.switchEnabled(copyImageBinding),
                "translation image copy remains available in text mode");
        session.refreshAll();
        require(!session.state(layoutId).enabled &&
                    translation.layoutProcessing() == QStringLiteral("original"),
                "disabled layout selector retains its choice");
        require(!backend.applySelectValue(layoutBinding, QStringLiteral("unsupported")),
                "reject unknown mode");
        require(backend.resetSection(settings::SettingsSectionReset::Translation),
                "reset translation layout");
        require(!backend.switchValue(copyImageBinding), "translation reset disables image copying");
        session.refreshAll();
        require(session.state(layoutId).enabled &&
                    translation.layoutProcessing() == QStringLiteral("smart_merge"),
                "reset enables original-image translation and restores Smart Merge");
        const storage::ScreenshotTranslationConfiguration languages{
            QStringLiteral("ja"),           QStringLiteral("zh-Hant"),
            QStringLiteral("chosen-model"), QStringLiteral("smart_merge"),
            QStringLiteral("en"),           75};
        require(backend.switchEnabled(binding) && backend.switchValue(binding),
                "backend should expose an enabled, default-on translation switch");
        require(translation.setConfiguration(languages) &&
                    backend.applySwitchValue(binding, false) && !backend.switchValue(binding),
                "backend should persist the display toggle");
        require(backend.resetSection(settings::SettingsSectionReset::Translation) &&
                    backend.switchValue(binding) &&
                    translation.configuration().sourceLanguage == QStringLiteral("auto") &&
                    translation.configuration().targetLanguage.isEmpty() &&
                    translation.configuration().secondaryTargetLanguage == QStringLiteral("en") &&
                    translation.configuration().languageDetectionConfidenceThreshold == 60 &&
                    translation.configuration().modelId.isEmpty() &&
                    translation.configuration().layoutProcessing == QStringLiteral("smart_merge"),
                "reset Translation restores all seven settings");

        const auto resizeBinding = settings::SettingsSelectBinding::OcrDetectorResizePolicy;
        require(backend.selectValue(resizeBinding).toString() == QStringLiteral("max") &&
                    backend.applySelectValue(resizeBinding, QStringLiteral("min")) &&
                    backend.selectValue(resizeBinding).toString() == QStringLiteral("min") &&
                    !backend.applySelectValue(resizeBinding, QStringLiteral("unsupported")) &&
                    backend.selectValue(resizeBinding).toString() == QStringLiteral("min"),
                "detector scaling must default to max and persist only supported policies");
        require(
            backend.applySelectValue(settings::SettingsSelectBinding::OcrModelType,
                                     QStringLiteral("medium")) &&
                backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                    QStringLiteral("medium") &&
                !applicationStorage.configuration().setValue(
                    QStringLiteral("text_recognition/direct_ml_acceleration"), true) &&
                backend.resetSection(settings::SettingsSectionReset::TextRecognition) &&
                backend.selectValue(settings::SettingsSelectBinding::OcrModelType).toString() ==
                    QStringLiteral("small") &&
                backend.selectValue(resizeBinding).toString() == QStringLiteral("max") &&
                !applicationStorage.configuration()
                     .value(QStringLiteral("text_recognition/direct_ml_acceleration"))
                     .toBool(),
            "obsolete acceleration settings are rejected and reset restores Small and max scaling");
    }
    require(storage::PinToScreenSettings().setDoubleClickAction(QStringLiteral("reset_zoom")),
            "save pinned double-click action before restart");
    require(storage::PinToScreenSettings().setMiddleMouseButtonAction(QStringLiteral("destroy")),
            "save pinned middle-click action before restart");
    const snow_shot::shortcuts::ShortcutBindingList selectedTextKeys{
        QStringLiteral("Ctrl+Alt+T"), QStringLiteral("Ctrl+Shift+T")};
    require(storage::ShortcutSettings().setTranslateSelectedText(selectedTextKeys),
            "save selected text shortcut bindings before restart");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                storage::PinToScreenSettings().doubleClickAction() == QStringLiteral("reset_zoom"),
            "pinned double-click action must survive a storage restart");
    require(storage::ExtendedFeaturesSettings().translationPageEnabled(),
            "feature opt-in survives storage restart");
    require(storage::ShortcutSettings().translateSelectedText() == selectedTextKeys,
            "both selected text shortcut bindings survive a storage restart");
    require(storage::PinToScreenSettings().middleMouseButtonAction() == QStringLiteral("destroy"),
            "pinned middle-click action must survive a storage restart");
    applicationStorage.shutdown();
    return 0;
}
