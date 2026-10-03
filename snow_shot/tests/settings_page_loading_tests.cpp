#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/components/pathinput.h"
#include "snow_shot/presentation/components/toolbareditorsettingswidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/scroll_area.h"
#include "widgets/color_picker.h"
#include "widgets/select.h"
#include "widgets/multi_select.h"
#include "widgets/slider.h"
#include <QListView>
#include <QLineEdit>
#include "widgets/switch.h"
#include "widgets/button.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "snow_shot/storage/configurationschema.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QProxyStyle>
#include <QPixmap>
#include <QPointer>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTranslator>

#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void settingsRowsHaveOnlySectionSpacing(const settings::SettingsRegistry& registry,
                                        settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("general-appearance"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    const auto metric =
        snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme().metricAlias;
    const auto verifyRows = [&] {
        QWidget* previous = nullptr;
        for (const QString& id :
             {QStringLiteral("interface.theme"), QStringLiteral("interface.theme-primary-color"),
              QStringLiteral("interface.app-font")}) {
            auto* row = page.findChild<QWidget*>(
                settings::generatedObjectName(QStringLiteral("settings-item"), id));
            require(row != nullptr, "general settings row exists");
            auto* field =
                row->findChild<snow_shot::presentation::components::form_fields::FormField*>();
            require(field != nullptr, "general settings row uses the shared field");
            QLabel* title = nullptr;
            QLabel* description = nullptr;
            for (auto* label : row->findChildren<QLabel*>()) {
                if (label->text() == field->metadata().label.translated())
                    title = label;
                if (label->text() == field->metadata().description.translated())
                    description = label;
            }
            require(title != nullptr && description != nullptr,
                    "general settings row presents its title and description");
            const int copyHeight = title->heightForWidth(title->width()) + metric.marginXXS +
                                   description->heightForWidth(description->width());
            require(
                row->height() == qMax(copyHeight, field->controlWidget()->height()),
                "settings rows fit their wrapped copy and control without a form bottom margin");
            if (previous) {
                require(row->y() - previous->y() - previous->height() == metric.paddingLG,
                        "the settings section alone owns spacing between its rows");
            }
            previous = row;
        }
    };
    verifyRows();
    for (int width : {640, 1040, 880}) {
        page.resize(width, 760);
        drainEvents();
        verifyRows();
    }
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    for (const QString& language :
         {QStringLiteral("zh_CN"), QStringLiteral("zh_TW"), QStringLiteral("en_US")}) {
        require(languageManager.setLanguage(language), "change the settings row language");
        drainEvents();
        verifyRows();
    }
}

class CountingStyle final : public QProxyStyle {
  public:
    int polishes = 0;
    void polish(QWidget* widget) override {
        ++polishes;
        QProxyStyle::polish(widget);
    }
};

class TestTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        return qstrcmp(context, "SettingsCatalog") == 0
                   ? QStringLiteral("Translated: ") + QString::fromUtf8(source)
                   : QString();
    }
};

void deferredStateAndKeyboard(const settings::SettingsRegistry& registry,
                              settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("desktop-tools"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    const QString trayId = QStringLiteral("floating-toolbar.enabled");
    const auto* tray = registry.field(trayId);
    require(tray != nullptr, "tray field is registered");
    const bool original = session.state(trayId).draftValue.toBool();
    require(session.submitDraft(trayId, !original), "update a deferred field");
    drainEvents();
    TestTranslator translator;
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    auto* shell = page.findChild<QWidget*>(
        QStringLiteral("settings-section-list-desktop-tools-floating-toolbar"));
    require(shell != nullptr && shell->focusPolicy() == Qt::TabFocus,
            "deferred section participates in keyboard traversal");
    shell->setFocus(Qt::TabFocusReason);
    drainEvents();
    auto* toggle = page.findChild<adqt::widgets::AdSwitch*>(
        settings::generatedObjectName(QStringLiteral("settings-control"), trayId));
    require(toggle != nullptr && toggle->isChecked() == !original,
            "materialization reads the latest session state");
    require(toggle->accessibleName() == tray->definition->title.translated() &&
                toggle->accessibleName().startsWith(QStringLiteral("Translated: ")),
            "deferred controls use the current language");
    require(shell->isAncestorOf(QApplication::focusWidget()),
            "tabbing into a shell transfers focus to a real control");
    QCoreApplication::removeTranslator(&translator);
    drainEvents();
    require(toggle->accessibleName() == tray->definition->title.translated(),
            "loaded controls still retranslate after a language change");
    require(session.submitDraft(trayId, original), "restore the tray value");
}

void scrollingLoadsSections(const settings::SettingsRegistry& registry,
                            settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("files-history"), session);
    page.resize(880, 420);
    page.show();
    drainEvents();
    auto* scroll = page.findChild<adqt::widgets::AdScrollArea*>();
    auto* bar = scroll->verticalScrollBar();
    bar->setValue(bar->maximum());
    drainEvents();
    const auto* definition = registry.catalog().page(page.pageId());
    require(definition != nullptr && !definition->sections.isEmpty() &&
                !definition->sections.constLast().items.isEmpty(),
            "the settings page has a final section with items");
    const QString lastItemId = definition->sections.constLast().items.constLast().id;
    auto* lastItem = page.findChild<QWidget*>(
        settings::generatedObjectName(QStringLiteral("settings-item"), lastItemId));
    require(lastItem != nullptr,
            "scrolling to the bottom loads the last section without navigation");
    require(bar->value() == bar->maximum(),
            "jumping to the bottom must stay at the bottom after deferred layout");
    page.resize(880, 1000);
    drainEvents();
    require(bar->value() <= bar->maximum(), "resize preserves a valid scroll position");
}

void deferredSections(const settings::SettingsRegistry& registry,
                      settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("screenshots"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    const QString drawing = QStringLiteral("interface.toolbar.drawing-toolbar-editor");
    const QString snapshot = qEnvironmentVariable("SNOW_SETTINGS_SNAPSHOT");
    if (!snapshot.isEmpty()) {
        require(page.grab().save(snapshot), "save settings layout snapshot");
    }
    const QString drawingObject =
        settings::generatedObjectName(QStringLiteral("settings-item"), drawing);
    require(page.findChild<QWidget*>(drawingObject) == nullptr,
            "offscreen toolbar editor should not be constructed at first display");
    const auto initialCount = page.findChildren<QWidget*>().size();
    for (auto* picker : page.findChildren<adqt::widgets::AdColorPicker*>()) {
        require(picker->findChild<QWidget*>(QStringLiteral("ad-color-picker-picker-panel")) ==
                    nullptr,
                "first display must not build unopened color popup editors");
    }
    page.reveal({page.pageId(), QStringLiteral("drawing"), drawing});
    drainEvents();
    QWidget* editor = page.findChild<QWidget*>(drawingObject);
    require(editor != nullptr && editor->isVisible(), "search navigation materializes its target");
    bool hasToolbar = false;
    for (QWidget* child : editor->findChildren<QWidget*>()) {
        hasToolbar = hasToolbar || dynamic_cast<ToolbarEditorSettingsWidget*>(child) != nullptr;
    }
    require(hasToolbar, "search navigation creates the actual toolbar editor, not just its shell");
    auto* scroll = page.findChild<adqt::widgets::AdScrollArea*>();
    require(scroll != nullptr && scroll->viewport()->rect().intersects(QRect(
                                     editor->mapTo(scroll->viewport(), QPoint()), editor->size())),
            "a newly materialized search target must be in the viewport");
    const auto* definition = registry.catalog().page(page.pageId());
    for (const auto& section : definition->sections) {
        page.reveal({page.pageId(), section.id, {}});
        drainEvents();
    }
    require(page.findChildren<QWidget*>().size() > initialCount,
            "visiting deferred sections creates additional controls");
    for (const auto& field : registry.fields()) {
        if (field.pageId == page.pageId()) {
            require(page.findChild<QWidget*>(settings::generatedObjectName(
                        QStringLiteral("settings-item"), field.id)) != nullptr,
                    "every interface setting remains reachable");
        }
    }
    const auto fullCount = page.findChildren<QWidget*>().size();
    page.reveal({page.pageId(), QStringLiteral("drawing"), drawing});
    drainEvents();
    require(page.findChild<QWidget*>(drawingObject) == editor &&
                page.findChildren<QWidget*>().size() == fullCount + 1,
            "revisiting a section must reuse its controls");
    page.reveal({page.pageId(), {}, {}});
    drainEvents();
    require(scroll->verticalScrollBar()->value() == 0, "page navigation still reveals the top");
}

bool hasSelectableOption(const QAbstractItemModel* model) {
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->flags(model->index(row, 0)).testFlag(Qt::ItemIsSelectable)) {
            return true;
        }
    }
    return false;
}

void fontPreviewAndFiltering(const settings::SettingsRegistry& registry,
                             settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("general-appearance"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    auto* font = page.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-app-font"));
    require(font != nullptr && font->searchEnabled(), "font selector supports input filtering");
    auto* model = font->model();
    require(model->rowCount() == 1, "unopened font selector contains only System default");
    session.refreshAll();
    TestTranslator initialTranslator;
    QCoreApplication::installTranslator(&initialTranslator);
    drainEvents();
    require(font->model()->rowCount() == 1, "refresh and translation do not load unopened fonts");
    QCoreApplication::removeTranslator(&initialTranslator);
    drainEvents();
    int selectionChanges = 0;
    QObject::connect(font, &adqt::widgets::AdSelect::currentValueChanged, font,
                     [&selectionChanges] { ++selectionChanges; });
    int liveFontInsertions = 0;
    QObject::connect(font->model(), &QAbstractItemModel::rowsInserted, &page,
                     [&liveFontInsertions] { ++liveFontInsertions; });
    QPointer<QAbstractItemModel> unloadedModel = font->model();
    font->showPopup();
    require(liveFontInsertions == 0,
            "opening must publish complete fonts without rebuilding the live selector per font");
    require(selectionChanges == 0, "loading fonts must not commit a selection");
    font->hidePopup();
    model = font->model();
    drainEvents();
    require(unloadedModel.isNull(), "loading releases the replaced font model");
    const int loadedRows = model->rowCount();
    font->showPopup();
    require(font->model() == model && model->rowCount() == loadedRows && selectionChanges == 0,
            "reopening reuses the font model without changing selection");
    font->hidePopup();

    require(
        model->rowCount() > 1 &&
            model->index(0, 0).data(adqt::widgets::AdSelect::DefaultValueRole).toString().isEmpty(),
        "System default is first");
    require(font->currentValue().isValid() && font->currentValue().toString().isEmpty() &&
                font->currentModelIndex().row() == 0 &&
                font->currentText() == model->index(0, 0).data(Qt::DisplayRole).toString() &&
                !font->currentText().isEmpty() && font->lineEdit()->text() == font->currentText(),
            "System default is visibly selected when the application font is unset");
    for (int row = 1; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        require(index.data(Qt::FontRole).value<QFont>().family() ==
                    index.data(adqt::widgets::AdSelect::DefaultValueRole).toString(),
                "font options carry their corresponding preview family");
    }
    const QString family =
        model->index(1, 0).data(adqt::widgets::AdSelect::DefaultValueRole).toString();
    const QVariant saved = font->currentValue();
    font->showPopup();
    font->setSearchText(family.toUpper());
    drainEvents();
    require(font->view()->model()->rowCount() >= 1 && font->currentValue() == saved,
            "font filtering is case insensitive and does not commit a selection");
    for (int row = 0; row < font->view()->model()->rowCount(); ++row) {
        const auto index = font->view()->model()->index(row, 0);
        require(index.data(Qt::DisplayRole).toString().contains(family, Qt::CaseInsensitive) &&
                    index.data(Qt::FontRole).value<QFont>().family() ==
                        index.data(Qt::DisplayRole).toString(),
                "filtered popup retains labels and preview fonts");
    }
    font->setSearchText(QStringLiteral("no-font-matches-this-unique-query-019837"));
    require(font->currentValue() == saved, "no matches cannot clear the saved font");
    require(!hasSelectableOption(font->view()->model()), "unmatched query filters every font");
    font->setSearchText(QString());
    font->hidePopup();
    font->setCurrentValue(family);
    drainEvents();
    require(session.selectValue(settings::SettingsSelectBinding::AppFont).toString() == family,
            "choosing a font commits the setting");
    {
        SettingsPageWidget unopened(registry, QStringLiteral("general-appearance"), session);
        unopened.resize(880, 760);
        unopened.show();
        drainEvents();
        auto* unopenedFont = unopened.findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("settings-control-interface-app-font"));
        session.refreshAll();
        require(unopenedFont != nullptr && unopenedFont->model()->rowCount() == 2 &&
                    unopenedFont->currentText() == family,
                "another selector loading fonts does not populate unopened selectors");
    }

    TestTranslator translator;
    QObject::connect(model, &QAbstractItemModel::rowsInserted, &page,
                     [&liveFontInsertions] { ++liveFontInsertions; });
    QPointer<QAbstractItemModel> untranslatedModel = model;
    const int changesBeforeTranslation = selectionChanges;
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    model = font->model();
    require(liveFontInsertions == 0 && untranslatedModel.isNull() &&
                selectionChanges == changesBeforeTranslation,
            "translation publishes complete fonts, releases the old model and never commits");
    require(font->currentValue().toString() == family &&
                model->index(0, 0)
                    .data(Qt::DisplayRole)
                    .toString()
                    .startsWith(QStringLiteral("Translated: ")),
            "language changes retain the selected family and translate System default");
    require(model->index(1, 0).data(Qt::FontRole).value<QFont>().family() == family,
            "language changes preserve font preview roles");
    QCoreApplication::removeTranslator(&translator);
    drainEvents();
    const auto previous =
        snow_shot::storage::ApplicationStorage::instance().configuration().snapshot();
    auto imported = previous;
    const QString unavailable = QStringLiteral("SnowShot Missing UI Font Family");
    imported.insert(QStringLiteral("interface/app_font"), unavailable);
    require(session.importConfigurationSnapshot(imported, 3), "import a missing font family");
    drainEvents();
    require(font->currentValue().toString() == unavailable && font->currentText() == unavailable,
            "imported unavailable fonts remain visibly selected after options refresh");
    require(session.importConfigurationSnapshot(previous, 3), "restore the font snapshot");
    drainEvents();
    require(font->currentValue().toString() == family, "rollback refreshes the selected font");
    auto* theme = page.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-theme"));
    const QVariant originalTheme = theme->currentValue();
    require(theme->searchEnabled(), "ordinary settings selects support filtering");
    theme->showPopup();
    theme->setFocus();
    drainEvents();
    QKeyEvent input(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, QStringLiteral("dArK"));
    QApplication::sendEvent(theme->lineEdit(), &input);
    drainEvents();
    require(theme->view()->model()->rowCount() == 1 && theme->currentValue() == originalTheme,
            "typing filters settings options without committing");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(theme->lineEdit(), &enter);
    drainEvents();
    require(session.selectValue(settings::SettingsSelectBinding::Theme).toString() ==
                QStringLiteral("dark"),
            "Enter commits the filtered choice");
    theme->hidePopup();
    require(session.applySelectValue(settings::SettingsSelectBinding::Theme, originalTheme),
            "restore theme");
    require(session.applySelectValue(settings::SettingsSelectBinding::AppFont, saved),
            "restore font");
    drainEvents();
    require(font->currentModelIndex().row() == 0 && !font->currentText().isEmpty() &&
                font->lineEdit()->text() == font->currentText(),
            "returning to System default restores its visible label");
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    model = font->model();
    require(font->currentModelIndex().row() == 0 &&
                font->currentText() == model->index(0, 0).data(Qt::DisplayRole).toString() &&
                font->currentText().startsWith(QStringLiteral("Translated: ")) &&
                font->lineEdit()->text() == font->currentText(),
            "language changes keep System default visibly selected");
    QCoreApplication::removeTranslator(&translator);
    drainEvents();
    font->setCurrentValue(family);
    drainEvents();
    require(session.reset(settings::SettingsSectionReset::GeneralSettings),
            "reset General settings with a custom font selected");
    drainEvents();
    require(font->currentModelIndex().row() == 0 && !font->currentText().isEmpty() &&
                font->lineEdit()->text() == font->currentText(),
            "resetting General settings visibly selects System default");
}

void multiSettingsSelectsSearch(const settings::SettingsRegistry& registry,
                                settings::SettingsRuntimeSession& session) {
    int count = 0;
    for (const auto& field : registry.fields()) {
        if (field.kind != settings::SettingsFieldKind::MultiSelect) {
            continue;
        }
        SettingsPageWidget page(registry, field.pageId, session);
        page.resize(880, 760);
        page.show();
        page.reveal({field.pageId, field.sectionId, field.id});
        drainEvents();
        auto* select = page.findChild<adqt::widgets::AdMultiSelect*>(
            settings::generatedObjectName(QStringLiteral("settings-control"), field.id));
        ++count;
        require(select != nullptr && select->searchEnabled(),
                "every settings multi-select supports input filtering");
        const auto selected = select->selectedValues();
        select->setSearchText(QStringLiteral("no-matching-setting-option-1937"));
        require(!hasSelectableOption(select->view()->model()) &&
                    select->selectedValues() == selected,
                "multi-select filtering cannot alter the selection");
        select->setSearchText(QString());
        require(hasSelectableOption(select->view()->model()),
                "clearing search restores multi-select options");
    }
    require(count > 0, "exercise the shared multi-select settings control");
}

void reorganizedSettingsPreserveIndependentState(const settings::SettingsRegistry& registry,
                                                 settings::SettingsRuntimeSession& session) {
    SettingsPageWidget general(registry, QStringLiteral("general"), session);
    general.resize(880, 760);
    general.show();
    drainEvents();
    auto* language = general.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-language"));
    require(language != nullptr, "General exposes the application language selector");
    language->setCurrentValue(QStringLiteral("zh_TW"));
    drainEvents();
    require(session.selectValue(settings::SettingsSelectBinding::Language).toString() ==
                QStringLiteral("zh_TW"),
            "General language edits persist");
    require(session.reset(settings::SettingsSectionReset::GeneralSettings) &&
                session.selectValue(settings::SettingsSelectBinding::Language).toString() ==
                    QStringLiteral("zh_TW"),
            "resetting appearance preserves the language chosen in General");
    require(
        session.applySelectValue(settings::SettingsSelectBinding::Theme, QStringLiteral("dark")) &&
            session.reset(settings::SettingsSectionReset::Language) &&
            session.selectValue(settings::SettingsSelectBinding::Theme).toString() ==
                QStringLiteral("dark") &&
            session.selectValue(settings::SettingsSelectBinding::Language).toString() ==
                snow_shot::storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("interface/language"))
                    .toString(),
        "resetting language preserves appearance and restores the language default");
    require(session.applySelectValue(settings::SettingsSelectBinding::Language,
                                     QStringLiteral("en_US")) &&
                session.reset(settings::SettingsSectionReset::GeneralSettings),
            "restore language and appearance for subsequent checks");

    SettingsPageWidget hotkeys(registry, QStringLiteral("global-hotkeys"), session);
    hotkeys.resize(880, 760);
    hotkeys.show();
    hotkeys.reveal({QStringLiteral("global-hotkeys"), QStringLiteral("global-hotkeys"),
                    QStringLiteral("global-hotkeys.disable-on-focused-fullscreen-window")});
    drainEvents();
    require(hotkeys.findChild<adqt::widgets::AdSwitch*>(QStringLiteral(
                "settings-control-global-hotkeys-disable-on-focused-fullscreen-window")) != nullptr,
            "Global Hotkeys materializes the relocated full-screen switch");
    const auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const auto screenshotShortcut =
        configuration.value(QStringLiteral("global_shortcuts/screenshot"));
    require(session.applySwitchValue(
                settings::SettingsSwitchBinding::DisableHotkeysOnFocusedFullscreen, true) &&
                session.reset(settings::SettingsSectionReset::GlobalHotkeys) &&
                configuration.value(
                    QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window")) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(
                        QStringLiteral("global_shortcuts/disable_on_focused_fullscreen_window")) &&
                configuration.value(QStringLiteral("global_shortcuts/screenshot")) ==
                    screenshotShortcut,
            "full-screen reset preserves global shortcut bindings");
}

void unchangedPresentation(const settings::SettingsRegistry& registry,
                           settings::SettingsRuntimeSession& session) {
    CountingStyle style;
    SettingsPageWidget page(registry, QStringLiteral("general-appearance"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    auto* select = page.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-theme"));
    require(select != nullptr, "theme selector exists");
    select->setStyle(&style);
    drainEvents();
    style.polishes = 0;
    auto* model = select->model();
    page.retranslateUi();
    drainEvents();
    require(style.polishes == 0, "unchanged field state must not repolish its control");
    require(select->model() == model, "identical options must retain the selector model");
}

void skinControlsCommitAndRetranslate(const settings::SettingsRegistry& registry,
                                      settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("general-appearance"), session);
    page.resize(880, 520);
    page.show();
    page.reveal({page.pageId(), QStringLiteral("skin"), QStringLiteral("interface.skin.path")});
    drainEvents();
    auto* path =
        page.findChild<FilePathInput*>(QStringLiteral("settings-control-interface-skin-path"));
    auto* mode = page.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("settings-control-interface-skin-display-mode"));
    require(path != nullptr && mode != nullptr && path->isEnabled() && mode->isEnabled() &&
                path->allowClear() && path->browseButtonText() == QStringLiteral("Browse"),
            "revealing Skin must create an editable path with browse/clear and a display mode");
    path->setText(QStringLiteral("  /skins/settings-preview.webp  "));
    require(QMetaObject::invokeMethod(path, "editingFinished", Qt::DirectConnection),
            "commit the skin path through the settings widget");
    mode->setCurrentValue(QStringLiteral("contain"));
    drainEvents();
    require(session.filePathValue(settings::SettingsFilePathBinding::SkinPath) ==
                    QStringLiteral("/skins/settings-preview.webp") &&
                path->text() == QStringLiteral("/skins/settings-preview.webp") &&
                session.selectValue(settings::SettingsSelectBinding::SkinDisplayMode).toString() ==
                    QStringLiteral("contain"),
            "path editing must normalize the skin path and commit with mode selection");
    for (const auto& id : {QStringLiteral("toolbar-path"), QStringLiteral("tray-menu-path")}) {
        auto* surfacePath =
            page.findChild<FilePathInput*>(QStringLiteral("settings-control-interface-skin-") + id);
        const auto binding = id == QStringLiteral("toolbar-path")
                                 ? settings::SettingsFilePathBinding::ToolbarSkinPath
                                 : settings::SettingsFilePathBinding::TrayMenuSkinPath;
        const QString value = QStringLiteral("/skins/") + id + QStringLiteral(".png");
        require(surfacePath != nullptr && surfacePath->isEnabled() && surfacePath->allowClear(),
                "Skin must expose independent editable toolbar and tray image paths");
        surfacePath->setText(QStringLiteral("  ") + value + QStringLiteral("  "));
        require(QMetaObject::invokeMethod(surfacePath, "editingFinished", Qt::DirectConnection),
                "commit each surface image path through its settings widget");
        drainEvents();
        require(
            session.filePathValue(binding) == value && surfacePath->text() == value &&
                session.filePathValue(settings::SettingsFilePathBinding::SkinPath) ==
                    QStringLiteral("/skins/settings-preview.webp"),
            "toolbar and tray path editing must trim whitespace without replacing the main skin");
    }
    for (const auto& id : {QStringLiteral("position"), QStringLiteral("toolbar-position"),
                           QStringLiteral("tray-menu-position")}) {
        auto* position = page.findChild<adqt::widgets::AdSelect*>(
            QStringLiteral("settings-control-interface-skin-") + id);
        const auto binding = id == QStringLiteral("position")
                                 ? settings::SettingsSelectBinding::SkinPosition
                             : id == QStringLiteral("toolbar-position")
                                 ? settings::SettingsSelectBinding::ToolbarSkinPosition
                                 : settings::SettingsSelectBinding::TrayMenuSkinPosition;
        require(position != nullptr && position->isEnabled() && position->model()->rowCount() == 9,
                "each skin position selector must display nine editable alignment choices");
        position->setCurrentValue(QStringLiteral("bottom_right"));
        drainEvents();
        require(session.selectValue(binding).toString() == QStringLiteral("bottom_right"),
                "each surface position must commit through its own settings selector");
    }
    for (const auto& id : {QStringLiteral("opacity"), QStringLiteral("blur-level"),
                           QStringLiteral("mask-opacity")}) {
        auto* slider = page.findChild<adqt::widgets::AdSlider*>(
            QStringLiteral("settings-control-interface-skin-") + id);
        require(slider != nullptr && slider->isEnabled(),
                "Skin must expose three editable sliders");
        slider->setValue(37);
        require(QMetaObject::invokeMethod(slider, "editingFinished", Qt::DirectConnection),
                "complete the skin slider adjustment");
    }
    drainEvents();
    require(session.sliderValue(settings::SettingsSliderBinding::SkinOpacity) == 37 &&
                session.sliderValue(settings::SettingsSliderBinding::SkinBlurLevel) == 37 &&
                session.sliderValue(settings::SettingsSliderBinding::SkinMaskOpacity) == 37,
            "skin slider values must commit through the runtime session");
    TestTranslator translator;
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    require(
        path->lineEdit()->accessibleName() ==
                QStringLiteral("Translated: Main Interface Skin Path") &&
            mode->accessibleName() == QStringLiteral("Translated: Skin Display Mode") &&
            path->browseButtonText() == QStringLiteral("Translated: Browse") &&
            path->lineEdit()->accessibleDescription().startsWith(QStringLiteral("Translated: ")),
        "skin controls and descriptions must retranslate after LanguageChange");
    QCoreApplication::removeTranslator(&translator);
    drainEvents();
    require(path->lineEdit()->accessibleName() == QStringLiteral("Main Interface Skin Path") &&
                session.reset(settings::SettingsSectionReset::Skin),
            "skin controls must restore English and the independent Skin defaults");
}
void relatedSettingsLayoutAndKeyboard(const settings::SettingsRegistry& registry,
                                      settings::SettingsRuntimeSession& session) {
    using snow_shot::presentation::styles::ThemeMode;
    auto& theme = snow_shot::presentation::styles::ThemeManager::instance();
    const auto originalMode = theme.themeMode();
    auto& language = snow_shot::presentation::LanguageManager::instance();
    SettingsPageWidget page(registry, QStringLiteral("screenshots"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    auto* panel = page.findChild<QWidget*>(QStringLiteral("settings-related-screenshots"));
    auto* heading = page.findChild<QLabel*>(QStringLiteral("settingsRelatedHeading"));
    require(panel != nullptr && heading != nullptr, "related settings have a named panel");
    const auto* definition = registry.catalog().page(page.pageId());
    QList<adqt::widgets::AdButton*> links;
    for (const auto& link : definition->relatedLinks) {
        auto* button = panel->findChild<adqt::widgets::AdButton*>(
            settings::generatedObjectName(QStringLiteral("settings-link"),
                                          page.pageId() + QLatin1Char('-') + link.location.pageId +
                                              QLatin1Char('-') + link.location.sectionId));
        require(button != nullptr, "every related destination is present");
        links.push_back(button);
    }
    for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
        theme.setThemeMode(mode);
        for (const QString& locale :
             {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            require(language.setLanguage(locale), "translate the related settings panel");
            for (const int width : {440, 880}) {
                page.resize(width, 760);
                page.reveal({page.pageId(), {}, {}});
                drainEvents();
                require(panel->accessibleName() == heading->text(),
                        "the panel's accessible name follows its translated heading");
                const QRect headingRect(heading->mapTo(panel, QPoint()), heading->size());
                const auto metric = theme.themeColorScheme().metricAlias;
                QRect previous;
                for (int index = 0; index < links.size(); ++index) {
                    const auto* button = links.at(index);
                    const QRect buttonRect(button->mapTo(panel, QPoint()), button->size());
                    require(panel->rect().contains(buttonRect) &&
                                buttonRect.top() > headingRect.bottom() &&
                                !previous.intersects(buttonRect),
                            "translated links wrap below the heading without clipping or overlap");
                    require(button->accessibleName() ==
                                definition->relatedLinks.at(index).title.translated(),
                            "each navigation button retains its translated accessible name");
                    QFont font = button->font();
                    font.setPixelSize(metric.fontSize);
                    const QFontMetrics metrics(font);
                    // Glyph bearings can add a pixel or two beyond the text advance.
                    require(qAbs(buttonRect.width() -
                                 metrics.horizontalAdvance(button->accessibleName())) <= 2 &&
                                buttonRect.height() == metrics.height(),
                            "related buttons size to their text without built-in padding");
                    if (previous.isNull() || previous.top() != buttonRect.top()) {
                        require(buttonRect.left() == headingRect.left(),
                                "each row of related buttons aligns with the heading");
                    } else {
                        require(buttonRect.left() - previous.right() - 1 ==
                                    metric.paddingContentHorizontal,
                                "related buttons have half their former text spacing");
                    }
                    previous = buttonRect;
                }
                if (width == 440 && locale == QStringLiteral("en_US")) {
                    require(links.last()->y() > links.first()->y(),
                            "related destinations wrap onto another row at narrow widths");
                }
                const QString snapshots = qEnvironmentVariable("SNOW_RELATED_SETTINGS_SNAPSHOTS");
                if (!snapshots.isEmpty()) {
                    require(QDir().mkpath(snapshots),
                            "create the related settings snapshot folder");
                    const QString name = QStringLiteral("related-%1-%2-%3.png")
                                             .arg(mode == ThemeMode::Dark ? u"dark" : u"light")
                                             .arg(locale)
                                             .arg(width);
                    require(page.grab().save(QDir(snapshots).filePath(name)),
                            "save the related settings preview");
                }
            }
        }
    }
    settings::SettingsLocation destination;
    QObject::connect(&page, &SettingsPageWidget::commandRequested, &page,
                     [&](const settings::SettingsCommand& command) {
                         require(command.kind == settings::SettingsCommandKind::Navigate,
                                 "related buttons issue navigation commands");
                         destination = command.location;
                     });
    links.first()->setFocus(Qt::TabFocusReason);
    for (int index = 0; index < links.size(); ++index) {
        auto* button = links.at(index);
        require(QApplication::focusWidget() == button,
                "related links follow their visual order when tabbing");
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(button, &press);
        QApplication::sendEvent(button, &release);
        require(destination == definition->relatedLinks.at(index).location,
                "keyboard activation preserves the exact related destination");
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(button, &tab);
    }
    theme.setThemeMode(originalMode);
    require(language.setLanguage(QStringLiteral("en_US")), "restore English after panel checks");
    drainEvents();
}

void pinnedSettingsGroups(const settings::SettingsRegistry& registry,
                          settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("pinned-windows"), session);
    page.resize(880, 760);
    page.show();
    const QString toolbarSection = QStringLiteral("pin-to-screen-toolbar");
    const QString toolbarId = QStringLiteral("interface.pin-to-screen.pinned-toolbar-editor");
    page.reveal({page.pageId(), toolbarSection, toolbarId});
    drainEvents();
    auto* editor = page.findChild<QWidget*>(
        settings::generatedObjectName(QStringLiteral("settings-item"), toolbarId));
    require(editor != nullptr && editor->isVisible(),
            "pinned toolbar navigation reveals its editor in the new section");

    auto& language = snow_shot::presentation::LanguageManager::instance();
    for (const QString& locale :
         {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        require(language.setLanguage(locale), "switch pinned settings language");
        drainEvents();
        const QStringList titles =
            locale == u"en_US"
                ? QStringList{QStringLiteral("Interaction"), QStringLiteral("Window interface"),
                              QStringLiteral("Annotation toolbar")}
            : locale == u"zh_CN"
                ? QStringList{QString::fromUtf8("操作方式"), QString::fromUtf8("窗口界面"),
                              QString::fromUtf8("标注工具栏")}
                : QStringList{QString::fromUtf8("操作方式"), QString::fromUtf8("視窗介面"),
                              QString::fromUtf8("標註工具列")};
        const auto& sections = registry.catalog().page(page.pageId())->sections;
        for (qsizetype index = 0; index < sections.size(); ++index) {
            auto* header = page.findChild<SectionHeaderWidget*>(settings::generatedObjectName(
                QStringLiteral("settings-section"),
                QStringLiteral("%1-%2").arg(page.pageId(), sections.at(index).id)));
            require(header != nullptr, "pinned settings section header exists");
            bool hasTitle = false;
            for (auto* label : header->findChildren<QLabel*>())
                hasTitle = hasTitle || label->text() == titles.at(index);
            require(hasTitle, "pinned section titles retranslate in every supported language");
        }
    }
    require(language.setLanguage(QStringLiteral("en_US")), "restore English pinned settings");
    drainEvents();

    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const QString border = QStringLiteral("pin_to_screen/border_color");
    const QString activeBorder = QStringLiteral("pin_to_screen/border_active_color");
    const QString toolbar = QStringLiteral("pin_to_screen/action_tools_layout");
    const QString action = QStringLiteral("pin_to_screen/double_click_action");
    const QMap<QString, QJsonValue> previous{{border, configuration.value(border)},
                                             {activeBorder, configuration.value(activeBorder)},
                                             {toolbar, configuration.value(toolbar)},
                                             {action, configuration.value(action)}};
    auto customToolbar = snow_shot::storage::ConfigurationSchema::defaultValue(toolbar).toObject();
    auto positions = customToolbar.value(QStringLiteral("positions")).toArray();
    positions.prepend(positions.takeAt(positions.size() - 1));
    customToolbar.insert(QStringLiteral("positions"), positions);
    const QJsonValue color(QStringLiteral("#123456FF"));
    require(configuration.setValues({{border, color},
                                     {activeBorder, color},
                                     {toolbar, customToolbar},
                                     {action, QStringLiteral("close")}}),
            "prepare custom pinned appearance, toolbar and interaction preferences");
    const auto savedToolbar = configuration.value(toolbar);
    require(savedToolbar != snow_shot::storage::ConfigurationSchema::defaultValue(toolbar),
            "pinned toolbar fixture differs from the default layout");
    require(session.reset(settings::SettingsSectionReset::PinToScreen) &&
                configuration.value(border) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(border) &&
                configuration.value(activeBorder) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(activeBorder) &&
                configuration.value(toolbar) == savedToolbar &&
                configuration.value(action).toString() == u"close",
            "window interface reset restores both borders and preserves toolbar and interaction");
    require(configuration.setValues({{border, color}, {activeBorder, color}}) &&
                session.reset(settings::SettingsSectionReset::PinToScreenToolbar) &&
                configuration.value(toolbar) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(toolbar) &&
                configuration.value(border) == color &&
                configuration.value(activeBorder) == color &&
                configuration.value(action).toString() == u"close",
            "annotation toolbar reset restores its layout and preserves borders and interaction");
    require(configuration.setValues(previous), "restore pinned preferences after reset checks");
    session.refreshAll();
}

void featureLinksAdvancedControlsAndResets(const settings::SettingsRegistry& registry,
                                           settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("screen-recording"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    auto* encoding =
        page.findChild<QWidget*>(QStringLiteral("settings-section-list-screen-recording-encoding"));
    require(encoding != nullptr && !encoding->isHidden(), "encoding controls start expanded");
    page.reveal(
        {page.pageId(), QStringLiteral("encoding"), QStringLiteral("screen-recording.encoder")});
    drainEvents();
    auto* encoder =
        page.findChild<QWidget*>(QStringLiteral("settings-control-screen-recording-encoder"));
    require(encoding->isVisible() && encoder != nullptr && encoder->isVisible() &&
                page.findChild<QFrame*>(QStringLiteral("settingsSearchHighlight")) != nullptr,
            "search reveals encoding controls and highlights its exact destination");
    auto* header = page.findChild<SectionHeaderWidget*>(
        QStringLiteral("settings-section-screen-recording-encoding"));
    require(header != nullptr, "encoding section header exists");
    auto* expand =
        header->findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionExpandButton"));
    require(expand == nullptr || expand->isHidden(),
            "encoding uses the same expanded presentation as the other settings sections");

    settings::SettingsLocation destination;
    QObject::connect(
        &page, &SettingsPageWidget::commandRequested, &page,
        [&](const settings::SettingsCommand& command) { destination = command.location; });
    auto* link = page.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-link-screen-recording-files-history-screen-recording-output"));
    require(link != nullptr, "recording preferences link to the shared output settings");
    TestTranslator translator;
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    require(link->text() == QStringLiteral("Translated: Video export") &&
                link->accessibleName() == QStringLiteral("Translated: Video export"),
            "related links retranslate accessibly");
    link->click();
    require(destination == settings::SettingsLocation{QStringLiteral("files-history"),
                                                      QStringLiteral("screen-recording-output"),
                                                      {}},
            "related links navigate to the canonical section without duplicating controls");
    QCoreApplication::removeTranslator(&translator);
    drainEvents();

    auto& configuration = snow_shot::storage::ApplicationStorage::instance().configuration();
    const QString video = QStringLiteral("screen_recording/video_quality");
    const QString animation = QStringLiteral("screen_recording/animated_image_frame_rate");
    const QString codec = QStringLiteral("screen_recording/encoder");
    const QString overlay = QStringLiteral("screen_recording/show_keyboard");
    require(configuration.setValues(
                {{video, 37}, {animation, 5}, {codec, QStringLiteral("h265")}, {overlay, true}}),
            "prepare distinct recording preferences");
    require(session.reset(settings::SettingsSectionReset::ScreenRecordingVideo) &&
                configuration.value(video) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(video) &&
                configuration.value(animation).toInt() == 5 &&
                configuration.value(codec).toString() == QStringLiteral("h265") &&
                configuration.value(overlay).toBool(),
            "video reset preserves animation, advanced encoding and recording overlay preferences");
    require(session.reset(settings::SettingsSectionReset::ScreenRecordingAnimation) &&
                configuration.value(animation) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(animation) &&
                configuration.value(codec).toString() == QStringLiteral("h265"),
            "animation reset preserves the selected video encoder");
    require(session.reset(settings::SettingsSectionReset::ScreenRecordingEncoding) &&
                configuration.value(codec) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(codec),
            "encoding reset restores its own defaults");
#ifndef Q_OS_MACOS
    const QString cursor = QStringLiteral("screenshot/capture_cursor");
    const QString captureApi = QStringLiteral("screenshot/api_mode");
    const QString alternateApi =
        snow_shot::storage::ConfigurationSchema::defaultValue(captureApi).toString() == u"gdi"
            ? QStringLiteral("dxgi")
            : QStringLiteral("gdi");
    require(configuration.setValues({{cursor, true}, {captureApi, alternateApi}}) &&
                session.reset(settings::SettingsSectionReset::ScreenshotCaptureBehavior) &&
                !configuration.value(cursor).toBool() &&
                configuration.value(captureApi).toString() == alternateApi,
            "ordinary capture reset leaves the compatibility backend unchanged");
    require(configuration.setValue(cursor, true) &&
                session.reset(settings::SettingsSectionReset::ScreenshotCaptureCompatibility) &&
                configuration.value(cursor).toBool() &&
                configuration.value(captureApi) ==
                    snow_shot::storage::ConfigurationSchema::defaultValue(captureApi),
            "advanced capture reset preserves cursor visibility");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    require(QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf"))) >= 0 &&
                QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc"))) >= 0,
            "load Windows UI fonts for offscreen layout checks");
#endif
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated storage");
    auto& languageManager = snow_shot::presentation::LanguageManager::instance();
    languageManager.initialize();
    require(languageManager.setLanguage(QStringLiteral("en_US")),
            "use English settings labels for deterministic filtering checks");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    if (application.arguments().contains(QStringLiteral("--navigation-only"))) {
        relatedSettingsLayoutAndKeyboard(registry, session);
        reorganizedSettingsPreserveIndependentState(registry, session);
        pinnedSettingsGroups(registry, session);
        featureLinksAdvancedControlsAndResets(registry, session);
        storage.shutdown();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selects-only"))) {
        fontPreviewAndFiltering(registry, session);
        multiSettingsSelectsSearch(registry, session);
        storage.shutdown();
        return 0;
    }
    deferredSections(registry, session);
    settingsRowsHaveOnlySectionSpacing(registry, session);
    deferredStateAndKeyboard(registry, session);
    scrollingLoadsSections(registry, session);
    unchangedPresentation(registry, session);
    skinControlsCommitAndRetranslate(registry, session);
    storage.shutdown();
    return 0;
}
