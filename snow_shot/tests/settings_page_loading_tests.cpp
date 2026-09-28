#include "snow_shot/presentation/components/settingspagewidget.h"
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
    TestTranslator translator;
    QCoreApplication::installTranslator(&translator);
    drainEvents();
    require(font->model() == model && font->currentValue().toString() == family &&
                model->index(0, 0)
                    .data(Qt::DisplayRole)
                    .toString()
                    .startsWith(QStringLiteral("Translated: ")),
            "language changes retain the model and selected family and translate System default");
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
    storage.shutdown();
    return 0;
}
