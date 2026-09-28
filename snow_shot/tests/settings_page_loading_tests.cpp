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
    snow_shot::presentation::LanguageManager::instance().initialize();
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    deferredSections(registry, session);
    deferredStateAndKeyboard(registry, session);
    scrollingLoadsSections(registry, session);
    unchangedPresentation(registry, session);
    storage.shutdown();
    return 0;
}
