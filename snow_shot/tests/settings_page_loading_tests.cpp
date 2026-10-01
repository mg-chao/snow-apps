#include "snow_shot/presentation/components/settingspagewidget.h"
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

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QKeyEvent>
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
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
    page.resize(880, 760);
    page.show();
    drainEvents();
    const QString trayId = QStringLiteral("interface.tray.enabled");
    const auto* tray = registry.field(trayId);
    require(tray != nullptr, "tray field is registered");
    const bool original = session.state(trayId).draftValue.toBool();
    require(session.submitDraft(trayId, !original), "update a deferred field");
    drainEvents();
    TestTranslator translator;
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    auto* shell =
        page.findChild<QWidget*>(QStringLiteral("settings-section-list-interface-settings-tray"));
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
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
    page.resize(880, 420);
    page.show();
    drainEvents();
    auto* scroll = page.findChild<adqt::widgets::AdScrollArea*>();
    auto* bar = scroll->verticalScrollBar();
    bar->setValue(bar->maximum());
    drainEvents();
    auto* tray =
        page.findChild<QWidget*>(QStringLiteral("settings-control-interface-tray-enabled"));
    require(tray != nullptr, "scrolling to the bottom loads the last section without navigation");
    require(bar->value() == bar->maximum(),
            "jumping to the bottom must stay at the bottom after deferred layout");
    page.resize(880, 1000);
    drainEvents();
    require(bar->value() <= bar->maximum(), "resize preserves a valid scroll position");
}

void deferredSections(const settings::SettingsRegistry& registry,
                      settings::SettingsRuntimeSession& session) {
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
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
                page.findChildren<QWidget*>().size() == fullCount,
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
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
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
        SettingsPageWidget unopened(registry, QStringLiteral("interface-settings"), session);
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

void unchangedPresentation(const settings::SettingsRegistry& registry,
                           settings::SettingsRuntimeSession& session) {
    CountingStyle style;
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
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
    SettingsPageWidget page(registry, QStringLiteral("interface-settings"), session);
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
    if (application.arguments().contains(QStringLiteral("--selects-only"))) {
        fontPreviewAndFiltering(registry, session);
        multiSettingsSelectsSearch(registry, session);
        storage.shutdown();
        return 0;
    }
    deferredSections(registry, session);
    deferredStateAndKeyboard(registry, session);
    scrollingLoadsSections(registry, session);
    unchangedPresentation(registry, session);
    skinControlsCommitAndRetranslate(registry, session);
    storage.shutdown();
    return 0;
}
