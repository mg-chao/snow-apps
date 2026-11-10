#include "translation_test_support.h"
#include "snow_shot/presentation/components/texttranslationsettingswidget.h"
#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/modal.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_password_edit.h"
#include "widgets/input_number.h"
#include "widgets/combo_box.h"
#include "widgets/tag.h"
#include <QApplication>
#include <QPointer>
#include <QFontDatabase>
#include <QDir>
#include <QTemporaryDir>
#include <QTimer>

using namespace snow_shot;
using namespace adqt::widgets;
using namespace translation_tests;
namespace settings = snow_shot::presentation::settings;
namespace form_fields = snow_shot::presentation::components::form_fields;
namespace {
void settle() {
    QEventLoop loop;
    QTimer::singleShot(50, &loop, &QEventLoop::quit);
    loop.exec();
    flushEvents();
}
void contracts(QApplication& app) {
    QTemporaryDir directory;
    auto& store = storage::ApplicationStorage::instance();
    require(store.initialize({directory.path(), directory.path(), 0}).success,
            "initialize isolated storage");
    presentation::styles::ThemeManager::instance().initialize(app);
    presentation::LanguageManager::instance().initialize();
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "English");
    {
        presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        const auto registry = settings::buildBuiltInSettingsRegistry();
        settings::SettingsRuntimeSession session(registry, backend);
        SettingsPageWidget page(registry, QStringLiteral("connections-services"), session);
        page.resize(880, 900);
        page.show();
        page.reveal({page.pageId(), QStringLiteral("text-translation"),
                     QStringLiteral("api.text-translation")});
        settle();
        auto* widget = page.findChild<TextTranslationSettingsWidget*>();
        require(widget, "text translation category rendered");
        auto* add = widget->findChild<AdButton*>(QStringLiteral("textTranslationAdd"));
        require(add && session.textTranslationConfigurations().isEmpty(), "initial empty state");
        const auto open = [&]() {
            add->click();
            settle();
            auto* modal = widget->findChild<AdModal*>(QStringLiteral("textTranslationEditor"));
            require(modal, "editor opens");
            return modal;
        };
        auto* modal = open();
        const auto sharedFields = modal->contentWidget()->findChildren<form_fields::FormField*>();
        require(sharedFields.size() == 6, "translation editor uses six shared fields");
        int sharedEdits = 0;
        int sharedCommits = 0;
        for (auto* field : sharedFields) {
            require(!field->item()->isTouched() && !field->item()->isDirty(),
                    "translation editor initializes a clean AdForm baseline");
            QObject::connect(field, &form_fields::FormField::valueEdited, modal,
                             [&sharedEdits] { ++sharedEdits; });
            QObject::connect(field, &form_fields::FormField::valueCommitted, modal,
                             [&sharedCommits] { ++sharedCommits; });
        }
        modal->acceptButton()->click();
        settle();
        require(session.textTranslationConfigurations().isEmpty(), "empty form rejected");
        auto* body = modal->contentWidget();
        auto* name = body->findChild<AdLineEdit*>(QStringLiteral("modelName"));
        auto* url = body->findChild<AdLineEdit*>(QStringLiteral("apiUrl"));
        auto* secret = body->findChild<AdPasswordEdit*>(QStringLiteral("apiKey"));
        auto* provider = body->findChild<AdComboBox*>(QStringLiteral("translationProvider"));
        auto* limit = body->findChild<AdInputNumber*>(QStringLiteral("translationConcurrency"));
        auto* applicationId = body->findChild<AdLineEdit*>(QStringLiteral("applicationId"));
        require(name && url && secret && provider && limit && applicationId,
                "all configuration fields");
        require(!secret->textVisible() && limit->value() == 8 && limit->minimum() == 1 &&
                    limit->maximum() == 16,
                "masked secret and concurrency defaults");
        require(!applicationId->isVisible(), "DeepL hides application ID");
        const auto preview = app.arguments().indexOf(QStringLiteral("--preview-dir"));
        if (preview >= 0 && preview + 1 < app.arguments().size()) {
            QDir output(app.arguments()[preview + 1]);
            require(output.mkpath(QStringLiteral(".")), "create preview directory");
            require(page.grab().save(output.filePath(QStringLiteral("page.png"))),
                    "save settings preview");
            require(body->window()->grab().save(output.filePath(QStringLiteral("editor.png"))),
                    "save editor preview");
        }
        provider->setCurrentValue(QStringLiteral("baidu"));
        settle();
        require(applicationId->isVisible(), "Baidu shows application ID");
        name->setText(QStringLiteral("My translator"));
        url->setText(QStringLiteral("http://localhost:7777/custom?route=1"));
        secret->setText(QStringLiteral("secret"));
        applicationId->setText(QStringLiteral("app"));
        limit->setValue(16);
        require(sharedEdits >= 6 && sharedCommits == 0,
                "translation fields keep edits local until a successful Save");
        modal->acceptButton()->click();
        settle();
        require(session.textTranslationConfigurations().size() == 1, "saved configuration");
        require(sharedCommits == 6,
                "successful translation save commits each changed shared field once");
        const auto original = session.textTranslationConfigurations().first();
        require(original.provider == QStringLiteral("baidu") && original.concurrency == 16,
                "provider and limit persisted");
        auto* editAction = widget->findChild<AdButton*>(QStringLiteral("edit:") + original.id);
        QPointer<AdButton> originalAction(editAction);
        page.activateWindow();
        settle();
        editAction->setFocus();
        require(QApplication::focusWidget() == editAction,
                "focus the translation action before theming");
        for (const auto phase :
             {settings::SettingsWritePhase::Pending, settings::SettingsWritePhase::Clean}) {
            auto state = session.state(QStringLiteral("api.text-translation"));
            state.phase = phase;
            state.busy = phase == settings::SettingsWritePhase::Pending;
            state.dirty = state.busy;
            emit session.fieldChanged(QStringLiteral("api.text-translation"), state);
            settle();
            require(originalAction && QApplication::focusWidget() == originalAction,
                    "unchanged translation persistence notifications preserve actions and focus");
        }
        auto& themes = presentation::styles::ThemeManager::instance();
        const auto initialMode = themes.themeMode();
        themes.setThemeMode(presentation::styles::ThemeMode::Dark);
        settle();
        require(originalAction, "theme changes do not rebuild the translation action");
        require(QApplication::focusWidget() == originalAction,
                "theme changes preserve the focused translation action");
        themes.setThemeMode(initialMode);
        settle();
        widget->findChild<AdButton*>(QStringLiteral("copy:") + original.id)->click();
        settle();
        require(session.textTranslationConfigurations().size() == 2 &&
                    session.textTranslationConfigurations().last().id != original.id,
                "copy creates distinct same-provider identity");
        require(widget->findChild<QWidget*>(QStringLiteral("textTranslationRow:") +
                                            session.textTranslationConfigurations().last().id),
                "a changed translation list renders its new configuration");
        widget->findChild<AdButton*>(QStringLiteral("edit:") + original.id)->click();
        settle();
        modal = widget->findChild<AdModal*>(QStringLiteral("textTranslationEditor"));
        name = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("modelName"));
        name->setText(session.textTranslationConfigurations().last().name.toUpper());
        modal->acceptButton()->click();
        settle();
        require(session.textTranslationConfigurations().first().name == original.name,
                "case-insensitive duplicate name rejected");
        name->setText(QStringLiteral("Renamed"));
        modal->acceptButton()->click();
        settle();
        require(session.textTranslationConfigurations().first().id == original.id &&
                    session.textTranslationConfigurations().first().name ==
                        QStringLiteral("Renamed"),
                "rename preserves ID");
        modal = open();
        int cancelledEdits = 0;
        int cancelledCommits = 0;
        for (auto* field : modal->contentWidget()->findChildren<form_fields::FormField*>()) {
            QObject::connect(field, &form_fields::FormField::valueEdited, modal,
                             [&cancelledEdits] { ++cancelledEdits; });
            QObject::connect(field, &form_fields::FormField::valueCommitted, modal,
                             [&cancelledCommits] { ++cancelledCommits; });
        }
        modal->contentWidget()
            ->findChild<AdLineEdit*>(QStringLiteral("modelName"))
            ->setText(QStringLiteral("Cancelled draft"));
        require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("zh_CN")),
                "Simplified Chinese");
        settle();
        require(add->text() != QStringLiteral("Add configuration") &&
                    modal->windowTitle() != QStringLiteral("Add configuration"),
                "open editor and page retranslate");
        require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("zh_TW")),
                "Traditional Chinese");
        settle();
        AdButton currentFocus(&page);
        currentFocus.show();
        currentFocus.setFocus(Qt::MouseFocusReason);
        require(currentFocus.hasFocus(), "establish focus before dismissing translation editor");
        modal->reject();
        settle();
        require(currentFocus.hasFocus(), "translation editor dismissal must not refocus Add");
        require(session.textTranslationConfigurations().size() == 2 && cancelledEdits == 1 &&
                    cancelledCommits == 0,
                "language refresh stays silent and Cancel does not commit or save shared drafts");
        require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
                "restore English");
        settle();
        widget->findChild<AdButton*>(QStringLiteral("delete:") + original.id)->click();
        settle();
        auto* confirmation =
            widget->findChild<AdModal*>(QStringLiteral("textTranslationDeleteModal"));
        require(confirmation, "delete confirmation");
        currentFocus.setFocus(Qt::MouseFocusReason);
        confirmation->reject();
        settle();
        require(currentFocus.hasFocus(), "translation delete dismissal must not refocus Add");
        widget->findChild<AdButton*>(QStringLiteral("delete:") + original.id)->click();
        settle();
        confirmation = widget->findChild<AdModal*>(QStringLiteral("textTranslationDeleteModal"));
        require(confirmation, "reopen translation delete confirmation");
        confirmation->acceptButton()->click();
        settle();
        require(session.textTranslationConfigurations().size() == 1, "delete saved");
        {
            auto* deepLXModal = open();
            auto* deepLXBody = deepLXModal->contentWidget();
            auto* deepLXProvider =
                deepLXBody->findChild<AdComboBox*>(QStringLiteral("translationProvider"));
            deepLXProvider->setCurrentValue(QStringLiteral("deeplx"));
            settle();
            require(deepLXProvider->currentValue().toString() == QStringLiteral("deeplx") &&
                        !deepLXBody->findChild<AdLineEdit*>(QStringLiteral("applicationId"))
                             ->isVisible(),
                    "DeepLX is selectable and hides application ID");
            deepLXBody->findChild<AdLineEdit*>(QStringLiteral("modelName"))
                ->setText(QStringLiteral("My DeepLX"));
            const QString endpoint = QStringLiteral("http://localhost:1188/custom?route=deeplx");
            deepLXBody->findChild<AdLineEdit*>(QStringLiteral("apiUrl"))->setText(endpoint);
            deepLXModal->acceptButton()->click();
            settle();
            const auto deepLX = session.textTranslationConfigurations().last();
            require(
                session.textTranslationConfigurations().size() == 2 &&
                    deepLX.provider == QStringLiteral("deeplx") && deepLX.endpoint == endpoint &&
                    deepLX.apiKey.isEmpty() && deepLX.applicationId.isEmpty() &&
                    deepLX.concurrency == 8,
                "DeepLX saves with its endpoint, optional credentials, and default concurrency");
            auto* row =
                widget->findChild<QWidget*>(QStringLiteral("textTranslationRow:") + deepLX.id);
            require(row && row->findChild<AdTag*>() &&
                        row->findChild<AdTag*>()->text() == QStringLiteral("DeepLX"),
                    "DeepLX configuration displays its provider badge");
            widget->findChild<AdButton*>(QStringLiteral("edit:") + deepLX.id)->click();
            settle();
            deepLXModal = widget->findChild<AdModal*>(QStringLiteral("textTranslationEditor"));
            deepLXBody = deepLXModal->contentWidget();
            deepLXProvider =
                deepLXBody->findChild<AdComboBox*>(QStringLiteral("translationProvider"));
            auto* deepLXSecret = deepLXBody->findChild<AdPasswordEdit*>(QStringLiteral("apiKey"));
            require(deepLXProvider->currentValue().toString() == QStringLiteral("deeplx") &&
                        deepLXSecret && !deepLXSecret->textVisible() &&
                        deepLXBody->findChild<AdLineEdit*>(QStringLiteral("apiUrl"))->text() ==
                            endpoint,
                    "DeepLX reopens with its saved format, endpoint, and masked token");
            form_fields::FormField* tokenField = nullptr;
            for (auto* field : deepLXBody->findChildren<form_fields::FormField*>()) {
                if (field->metadata().id == QStringLiteral("apiKey"))
                    tokenField = field;
            }
            require(tokenField && tokenField->item()->label() == QStringLiteral("API key") &&
                        tokenField->metadata().description.translated() ==
                            QStringLiteral("Optional access token sent as a Bearer token"),
                    "DeepLX explains optional Bearer authentication");
            deepLXSecret->setText(QStringLiteral("access-token"));
            for (const auto& language :
                 {QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("en_US")}) {
                require(presentation::LanguageManager::instance().setLanguage(language),
                        "switch DeepLX editor language");
                settle();
                require(
                    deepLXProvider->currentValue().toString() == QStringLiteral("deeplx") &&
                        !deepLXBody->findChild<AdLineEdit*>(QStringLiteral("applicationId"))
                             ->isVisible() &&
                        deepLXSecret->text() == QStringLiteral("access-token"),
                    "language switching preserves DeepLX selection, visibility, and draft token");
                require(tokenField->metadata().description.translated().contains(
                            QStringLiteral("Bearer")) &&
                            (language == QStringLiteral("en_US") ||
                             tokenField->metadata().description.translated() !=
                                 QStringLiteral("Optional access token sent as a Bearer token")),
                        "DeepLX Bearer help is translated in each language");
            }
            deepLXModal->acceptButton()->click();
            settle();
            require(session.textTranslationConfigurations().last().id == deepLX.id &&
                        session.textTranslationConfigurations().last().apiKey ==
                            QStringLiteral("access-token"),
                    "editing DeepLX saves its token and preserves identity");
        }
        const CustomAiModelConfiguration ai{QUuid::createUuid().toString(QUuid::WithoutBraces),
                                            QStringLiteral("AI"),
                                            QStringLiteral("http://localhost/v1"),
                                            {},
                                            QStringLiteral("model")};
        require(session.applyCustomAiModels({ai}), "unrelated AI model");
        require(session.reset(settings::SettingsSectionReset::TextTranslationConfigurations),
                "reset category");
        settle();
        require(session.textTranslationConfigurations().isEmpty() &&
                    session.customAiModels().size() == 1,
                "category reset preserves AI models");
    }
    store.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("text_translation_settings_tests"));
#ifdef Q_OS_WIN
    static_cast<void>(
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")));
    static_cast<void>(
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")));
#endif
    contracts(app);
    return 0;
}
