#include "snow_shot/presentation/windowgroupswitchercontroller.h"
#include "snow_shot/presentation/windowgroupswitcherpopup.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "widgets/detail/popup_shadow.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTemporaryDir>
#include <QTranslator>
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::presentation;
namespace storage = snow_shot::storage;
namespace shortcuts = snow_shot::shortcuts;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
class Input final : public GlobalShortcutBackend {
  public:
    ActivationHandler handler;
    QHash<int, shortcuts::ShortcutBinding> registrations;
    QSet<int> held;
    bool escape = false;
    bool available = true;
    mutable int queries = 0;
    void setActivationHandler(ActivationHandler value) override {
        handler = std::move(value);
    }
    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        return {binding.portableText, true, GlobalShortcutFailureReason::None, binding};
    }
    GlobalShortcutBackendResult
    registerShortcut(int id, const shortcuts::ShortcutBinding& binding) override {
        registrations.insert(id, binding);
        return {true, GlobalShortcutFailureReason::None, 0};
    }
    void unregisterShortcut(int id) override {
        registrations.remove(id);
    }
    std::optional<GlobalShortcutInputState> inputState(int id) const override {
        ++queries;
        if (!available || (id != 0 && !registrations.contains(id)))
            return std::nullopt;
        return GlobalShortcutInputState{
            held.contains(id), escape,
            registrations.value(id).portableText.contains(QStringLiteral("Esc"))};
    }
    int id(const QString& text) const {
        for (auto it = registrations.cbegin(); it != registrations.cend(); ++it)
            if (it->portableText == text)
                return it.key();
        return 0;
    }
};
struct Fixture {
    QTemporaryDir dir;
    storage::PinnedWindowRepository repository{dir.path()};
    PinnedWindowGroupManager groups{&repository};
    Input* input = new Input;
    GlobalShortcutManager shortcuts{std::unique_ptr<GlobalShortcutBackend>(input), nullptr,
                                    [] { return false; }};
    WindowGroupSwitcherController picker{shortcuts, groups};
    QString alpha;
    QString beta;
    int first = 0;
    int second = 0;
    int commits = 0;
    Fixture(bool addGroups = true) {
        if (addGroups) {
            beta = groups.createGroup(QStringLiteral("Beta")).value();
            alpha = groups.createGroup(QStringLiteral("Alpha")).value();
        }
        shortcuts.initialize();
        require(shortcuts.setShortcuts(GlobalShortcutAction::SwitchWindowGroup,
                                       {QStringLiteral("Ctrl+Alt+F8"), QStringLiteral("Shift+F9")}),
                "assign switcher bindings");
        first = input->id(QStringLiteral("Ctrl+Alt+F8"));
        second = input->id(QStringLiteral("Shift+F9"));
        require(first != 0 && second != 0, "both bindings registered");
        QObject::connect(&shortcuts, &GlobalShortcutManager::bindingActivated, &picker,
                         [this](GlobalShortcutAction action, int id) {
                             if (action == GlobalShortcutAction::SwitchWindowGroup)
                                 picker.activateShortcut(id);
                         });
        QObject::connect(&groups, &PinnedWindowGroupManager::activeGroupChanged, &picker,
                         [this] { ++commits; });
        QCoreApplication::processEvents();
    }
    void press(int id) {
        input->held.insert(id);
        input->handler(id);
    }
    void release() {
        input->held.clear();
        picker.pollInput();
    }
};
QByteArray manifest(Fixture& f) {
    require(f.repository.flush().success, "flush test manifest");
    QFile file(f.dir.filePath(QStringLiteral("pinned_windows_v2/index.json")));
    require(file.open(QIODevice::ReadOnly), "open test manifest");
    return file.readAll();
}
void previewAndCommit() {
    Fixture f;
    const auto before = manifest(f);
    f.press(f.first);
    require(f.picker.isVisible() && f.picker.selectedGroupId() == f.alpha,
            "first press previews next display-sorted group");
    require(f.groups.activeGroupId() == QStringLiteral("default") && f.commits == 0,
            "preview never activates");
    require(manifest(f) == before, "preview never persists group state");
    f.picker.pollInput();
    require(f.picker.isVisible(), "held chord keeps popup open");
    f.press(f.first);
    require(f.picker.selectedGroupId() == f.beta, "fresh native press cycles once");
    f.press(f.first);
    require(f.picker.selectedGroupId() == QStringLiteral("default"), "cycle wraps");
    f.press(f.first);
    f.release();
    require(!f.picker.isVisible() && f.groups.activeGroupId() == f.alpha && f.commits == 1,
            "release commits exactly once");
    f.picker.pollInput();
    require(f.commits == 1 && !f.picker.isTrackingInput(), "idle does not track or recommit");
}
void multipleBindingsAndQuickTap() {
    Fixture f;
    f.press(f.first);
    f.press(f.second);
    require(f.picker.selectedGroupId() == f.beta, "alternate binding advances same session");
    f.input->held.remove(f.first);
    f.picker.pollInput();
    require(f.picker.isVisible(), "second participating chord delays commit");
    f.release();
    require(f.groups.activeGroupId() == f.beta, "last participating chord release commits");
    f.input->handler(f.first); // The key was released before the activation reached the GUI thread.
    require(f.picker.isVisible(), "quick tap still opens preview");
    f.picker.pollInput();
    require(f.groups.activeGroupId() == QStringLiteral("default"), "quick tap commits next group");
}
void cancellationAndDraining() {
    Fixture f;
    f.press(f.first);
    f.input->escape = true;
    f.picker.pollInput();
    require(!f.picker.isVisible() && f.commits == 0, "Escape cancels without activation");
    f.input->handler(f.first);
    require(!f.picker.isVisible(), "held chord cannot reopen cancelled popup");
    f.release();
    f.input->escape = false;
    f.press(f.first);
    f.picker.selectGroup(f.beta);
    require(!f.picker.isVisible() && f.commits == 1 && f.groups.activeGroupId() == f.beta,
            "click commits immediately while keys held");
    f.input->handler(f.first);
    require(!f.picker.isVisible(), "held chord cannot reopen clicked popup");
    f.release();
    require(f.commits == 1 && !f.picker.isTrackingInput(), "release after click never recommits");
}
void mouseModeAndActualClick() {
    Fixture f;
    f.picker.openPicker();
    f.picker.pollInput();
    require(f.picker.isVisible() && f.picker.selectedGroupId() == f.alpha,
            "settings opens persistent picker");
    auto* list = f.picker.popup()->findChild<QListView*>();
    require(list, "picker list exists");
    require(!f.picker.popup()->contentBody()->testAttribute(Qt::WA_TransparentForMouseEvents),
            "floating surface body accepts native mouse clicks");
    require(list->model()->rowCount() == 3, "every group shown, including empty groups");
    QCoreApplication::processEvents();
    const QPointF point = list->visualRect(list->model()->index(2, 0)).center();
    QMouseEvent down(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton,
                     Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &down);
    QApplication::sendEvent(list->viewport(), &up);
    require(f.groups.activeGroupId() == f.beta && !f.picker.isVisible(),
            "real row click confirms target");
    f.picker.openPicker();
    f.press(f.first);
    f.release();
    require(f.groups.activeGroupId() == f.alpha,
            "shortcut converts mouse picker to release confirmation");
}
void invalidation() {
    {
        Fixture f;
        f.press(f.first);
        f.shortcuts.setGlobalHotkeysEnabled(false);
        require(!f.picker.isVisible() && f.commits == 0, "disabled hotkeys cancel");
        f.picker.pollInput();
        require(!f.picker.isTrackingInput(), "disabled session stops polling");
    }
    {
        Fixture f;
        f.press(f.first);
        const auto handle = f.shortcuts.suspendRegistrations();
        require(!f.picker.isVisible(), "shortcut recorder cancels");
        f.picker.pollInput();
        f.shortcuts.resumeRegistrations(handle);
        require(f.commits == 0, "resuming does not commit");
    }
    {
        Fixture f;
        f.press(f.first);
        require(f.shortcuts.setShortcuts(GlobalShortcutAction::SwitchWindowGroup, {}),
                "remove bindings");
        f.picker.pollInput();
        require(!f.picker.isVisible() && f.commits == 0, "registration removal cancels");
    }
    {
        Fixture f;
        f.press(f.first);
        f.input->available = false;
        f.picker.pollInput();
        require(!f.picker.isVisible() && f.commits == 0 && !f.picker.isTrackingInput(),
                "unavailable native state cancels safely");
    }
    {
        Fixture f;
        f.press(f.first);
        require(f.groups.deleteSpecifiedGroup(f.alpha), "delete selected group");
        QCoreApplication::processEvents();
        require(!f.picker.isVisible() && f.commits == 0, "deleted selection cancels");
    }
    {
        Fixture f;
        f.press(f.first);
        require(f.groups.setActiveGroup(f.beta), "external group change");
        require(!f.picker.isVisible() && f.commits == 1,
                "external change cancels pending selection");
        f.release();
        require(f.groups.activeGroupId() == f.beta, "old session cannot override external change");
    }
}
void singleGroupAndEscapeShortcut() {
    Fixture f(false);
    f.press(f.first);
    require(f.picker.selectedGroupId() == QStringLiteral("default"),
            "single group remains selectable");
    f.release();
    require(f.commits == 0, "single group commits no-op");
    require(f.shortcuts.setShortcuts(GlobalShortcutAction::SwitchWindowGroup,
                                     {QStringLiteral("Ctrl+Esc")}),
            "assign Escape shortcut");
    const int id = f.input->id(QStringLiteral("Ctrl+Esc"));
    f.input->escape = true;
    f.press(id);
    f.picker.pollInput();
    require(f.picker.isVisible(), "Escape within binding does not cancel");
    f.input->escape = false;
    f.release();
    require(!f.picker.isVisible(), "Escape binding releases normally");
}
void snapshotsAndLiveUpdates() {
    Fixture f;
    f.groups.registerPendingPin(QStringLiteral("pending"), f.alpha);
    const auto entries = f.groups.displaySnapshot();
    const auto sorted = f.groups.groupsSortedForDisplay();
    for (int i = 0; i < entries.size(); ++i) {
        const auto counts = f.groups.windowCounts(entries[i].id);
        require(entries[i].id == sorted[i].id &&
                    entries[i].name == f.groups.displayName(sorted[i].id) &&
                    entries[i].counts.nonIgnored == counts.nonIgnored &&
                    entries[i].counts.total == counts.total,
                "bulk snapshot matches menu data");
    }
    f.press(f.first);
    int resets = 0;
    auto* model = f.picker.popup()->findChild<QListView*>()->model();
    QObject::connect(model, &QAbstractItemModel::modelReset, &f.picker, [&] { ++resets; });
    f.press(f.first);
    require(resets == 0, "cycling does not rebuild list");
    const auto selected = f.picker.selectedGroupId();
    f.groups.registerPendingPin(QStringLiteral("second"), f.beta);
    QCoreApplication::processEvents();
    require(f.picker.selectedGroupId() == selected && resets > 0,
            "count updates preserve selected ID");
    f.release();
}
void readOnlyFailureAndManyGroups() {
    Fixture f;
    require(f.repository.flush().success, "flush groups for read-only fixture");
    storage::PinnedWindowRepository readOnly(f.dir.path(), false);
    PinnedWindowGroupManager readOnlyGroups(&readOnly);
    WindowGroupSwitcherController picker(f.shortcuts, readOnlyGroups);
    int errors = 0;
    QObject::connect(&picker, &WindowGroupSwitcherController::errorOccurred, &picker,
                     [&](const QString& error) {
                         require(!error.isEmpty(), "failure explains switch error");
                         ++errors;
                     });
    picker.openPicker();
    picker.selectGroup(f.alpha);
    require(errors == 1 && !picker.isVisible() &&
                readOnlyGroups.activeGroupId() == QStringLiteral("default"),
            "failed activation leaves original group active");
    for (int i = 3; i < storage::PinnedWindowRepository::maximumGroupCount(); ++i)
        require(f.groups.createGroup(QStringLiteral("Group %1").arg(i)).has_value(),
                "create maximum groups");
    f.picker.openPicker();
    auto* list = f.picker.popup()->findChild<QListView*>();
    require(list->model()->rowCount() == 128, "picker includes all current groups");
    const auto last = f.groups.groupsSortedForDisplay().last().id;
    f.picker.popup()->setSelectedGroup(last);
    QCoreApplication::processEvents();
    require(list->viewport()->rect().intersects(list->visualRect(list->currentIndex())),
            "selected row is scrolled into view");
    require(f.picker.popup()->height() <=
                static_cast<int>(QApplication::primaryScreen()->availableGeometry().height() * 0.6),
            "many groups respect screen height cap");
    f.picker.cancel();
}

void popupShadowMatchesMessage() {
    const styles::ThemeStyleConfig original;
    for (bool dark : {false, true}) {
        auto config = original;
        config.appearance = dark ? styles::ThemeAppearance::Dark : styles::ThemeAppearance::Light;
        styles::ThemeManager::instance().setThemeStyleConfig(config);
        WindowGroupSwitcherPopup popup;
        popup.resize(436, 260);
        require(popup.shadowMargins() == adqt::widgets::detail::antPopupShadowSecondaryMargins(),
                "group popup reserves the same shadow space as messages");
        for (qreal dpr : {1.0, 1.5, 2.0}) {
            QImage actual(QSize(qRound(popup.width() * dpr), qRound(popup.height() * dpr)),
                          QImage::Format_ARGB32_Premultiplied);
            actual.setDevicePixelRatio(dpr);
            actual.fill(Qt::transparent);
            QPainter actualPainter(&actual);
            popup.render(&actualPainter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
            actualPainter.end();
            QImage expected(actual.size(), actual.format());
            expected.setDevicePixelRatio(dpr);
            expected.fill(Qt::transparent);
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(popup.bodyRect()), popup.cornerRadius(),
                                popup.cornerRadius());
            adqt::widgets::detail::paintAntPopupBoxShadowSecondary(painter, path);
            painter.end();
            int shadowPixels = 0;
            const QRectF body = QRectF(popup.bodyRect()).adjusted(-1, -1, 1, 1);
            for (int y = 0; y < actual.height(); ++y) {
                for (int x = 0; x < actual.width(); ++x) {
                    if (body.contains(QPointF((x + 0.5) / dpr, (y + 0.5) / dpr)))
                        continue;
                    require(actual.pixel(x, y) == expected.pixel(x, y),
                            "popup shadow pixels match the message renderer at every scale");
                    shadowPixels += qAlpha(actual.pixel(x, y)) > 0 ? 1 : 0;
                }
            }
            require(shadowPixels > 0, "popup has a visible shadow outside its body");
        }
    }
    styles::ThemeManager::instance().setThemeStyleConfig(original);
}

void renderFixtures(const QString& directory) {
    QDir().mkpath(directory);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        for (const QString& file : {QStringLiteral("segoeui.ttf"), QStringLiteral("segoeuib.ttf"),
                                    QStringLiteral("msyh.ttc")})
            require(QFontDatabase::addApplicationFont(
                        QDir(qEnvironmentVariable("WINDIR"))
                            .filePath(QStringLiteral("Fonts/") + file)) >= 0,
                    "load system fonts into the offscreen font database");
    }
#endif
    for (const QString& scenario :
         {QStringLiteral("compact"), QStringLiteral("many"), QStringLiteral("large-font")}) {
        Fixture f;
        if (scenario == QStringLiteral("many")) {
            for (int i = 3; i < 128; ++i)
                require(f.groups.createGroup(QStringLiteral("Research %1").arg(i)).has_value(),
                        "create render group");
        }
        f.groups.registerPendingPin(QStringLiteral("render"), f.alpha);
        for (const QString& language :
             {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
            require(LanguageManager::instance().setLanguage(language), "set render language");
            for (bool dark : {false, true}) {
                styles::ThemeStyleConfig config;
#ifdef Q_OS_WIN
                if (QGuiApplication::platformName() == QStringLiteral("offscreen"))
                    config.appFont = QFont(QStringLiteral("Segoe UI"));
#endif
                config.appearance =
                    dark ? styles::ThemeAppearance::Dark : styles::ThemeAppearance::Light;
                config.fontSize = scenario == QStringLiteral("large-font") ? 22 : 14;
                styles::ThemeManager::instance().setThemeStyleConfig(config);
                for (bool keyboard : {false, true}) {
                    if (keyboard)
                        f.press(f.first);
                    else
                        f.picker.openPicker();
                    QCoreApplication::processEvents();
                    auto* popup = f.picker.popup();
                    require(popup->windowFlags().testFlag(Qt::WindowStaysOnTopHint) &&
                                popup->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus),
                            "popup is topmost and nonactivating");
                    require(QApplication::primaryScreen()->availableGeometry().contains(
                                popup->geometry()),
                            "popup fits monitor");
                    auto* list = popup->findChild<QListView*>();
                    require(
                        list->viewport()->rect().contains(list->visualRect(list->currentIndex())),
                        "selected row remains fully visible with enlarged fonts");
                    require(
                        popup->grab().save(QDir(directory).filePath(
                            scenario +
                            (keyboard ? QStringLiteral("-keyboard-") : QStringLiteral("-")) +
                            language +
                            (dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png")))),
                        "save visual fixture");
                    f.picker.cancel();
                    f.release();
                }
            }
        }
    }
}

} // namespace
#include "window_group_switcher_native_test.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-host"))) {
        QWidget host(nullptr, Qt::Window | Qt::WindowStaysOnTopHint);
        host.resize(260, 160);
        host.setWindowTitle(QStringLiteral("Window group switcher native test host"));
        host.show();
        host.activateWindow();
        app.processEvents();
        std::cout << static_cast<qulonglong>(host.winId()) << std::endl;
        return app.exec();
    }
#endif
    QTemporaryDir directory;
    auto& storage = storage::ApplicationStorage::instance();
    require(
        storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path()}).success,
        "initialize test storage");
    styles::ThemeManager::instance().initialize(app);
    LanguageManager::instance().initialize();
    const auto args = app.arguments();
    const qsizetype render = args.indexOf(QStringLiteral("--render"));
#ifdef Q_OS_WIN
    if (args.contains(QStringLiteral("--native"))) {
        try {
            nativeSwitcherChecks();
        } catch (const NativeGroupDesktopUnavailable& error) {
            std::cerr << error.what() << '\n';
            storage.shutdown();
            return 77;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            storage.shutdown();
            return 1;
        }
    } else
#endif
        if (render >= 0 && render + 1 < args.size())
        renderFixtures(args.at(render + 1));
    else {
        popupShadowMatchesMessage();
        previewAndCommit();
        multipleBindingsAndQuickTap();
        cancellationAndDraining();
        mouseModeAndActualClick();
        invalidation();
        singleGroupAndEscapeShortcut();
        snapshotsAndLiveUpdates();
        readOnlyFailureAndManyGroups();
    }
    storage.shutdown();
    std::cout << "Window group switcher checks passed\n";
    return 0;
}
