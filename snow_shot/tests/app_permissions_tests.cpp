#include "snow_shot/presentation/apppermissionservice.h"
#include "snow_shot/app/featureavailability.h"
#include "snow_shot/presentation/components/sectionheaderwidget.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/popconfirm.h"
#include "widgets/scroll_area.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QFile>
#include <QJsonDocument>
#include <QApplication>
#include <algorithm>
#include <QLabel>
#include <QLayout>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::presentation;
namespace {
using P = AppPermission;
using S = AppPermissionStatus;
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void flush() {
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}
class FakePermissions final : public AppPermissionBackend {
  public:
    FakePermissions() {
        value.statuses.fill(S::Granted);
    }
    AppPermissionSnapshot value;
    int queries = 0;
    int requests = 0;
    int opens = 0;
    bool canOpen = true;
    bool openNativeSettings = false;
    P lastOpen = P::ScreenRecording;
    std::function<void()> completion;
    AppPermissionSnapshot query() override {
        ++queries;
        return value;
    }
    void request(P, std::function<void()> done) override {
        ++requests;
        completion = std::move(done);
    }
    bool openSettings(P permission) override {
        ++opens;
        lastOpen = permission;
        return openNativeSettings ? createAppPermissionBackend()->openSettings(permission)
                                  : canOpen;
    }
};
void permissionDiagnostics() {
    using namespace snow_shot::diagnostics;
    QTemporaryDir directory(QDir(QDir::tempPath()).canonicalPath() +
                            QStringLiteral("/snow-permissions-XXXXXX"));
    DiagnosticsOptions options;
    options.directories = {directory.path()};
    options.enableCrashCapture = false;
    options.installMessageHandler = false;
    options.mirrorToConsole = false;
    auto& diagnostics = DiagnosticsService::instance();
    require(diagnostics.initialize(options), "permission diagnostics initialize");
    auto fake = std::make_unique<FakePermissions>();
    auto* native = fake.get();
    native->value.statuses.fill(S::Granted);
    AppPermissionService service(std::move(fake));
    service.refresh();
    flush();
    service.refresh();
    flush();
    native->value.statuses[0] = S::Denied;
    service.refresh();
    flush();
    auto exported = diagnostics.exportDay(QDate::currentDate()).get();
    require(exported.success, "permission diagnostics export");
    QFile file(exported.path);
    require(file.open(QIODevice::ReadOnly), "permission diagnostics readable");
    int changes = 0;
    bool denied = false;
    for (const auto& line : file.readAll().split('\n')) {
        const auto record = QJsonDocument::fromJson(line).object();
        if (record.value(QStringLiteral("event")) != QStringLiteral("permission.changed"))
            continue;
        ++changes;
        const auto fields = record.value(QStringLiteral("fields")).toObject();
        denied |= fields.value(QStringLiteral("operation")) == QStringLiteral("screen-recording") &&
                  fields.value(QStringLiteral("status")) == QStringLiteral("denied");
    }
    require(changes == 5 && denied, "only permission transitions are logged with stable IDs");
    diagnostics.shutdown();
}
void snapshotsAndLifecycle() {
    auto fake = std::make_unique<FakePermissions>();
    auto* native = fake.get();
    AppPermissionService service(std::move(fake));
    require(service.takeStartupMissing().isEmpty(), "startup waits for the first snapshot");
    int changes = 0;
    int refreshes = 0;
    QObject::connect(&service, &AppPermissionService::changed, [&] { ++changes; });
    QObject::connect(&service, &AppPermissionService::refreshed, [&] { ++refreshes; });
    require(native->queries == 0 && !service.polling(),
            "construction must not prompt, query, or poll");
    for (int i = 0; i < 100; ++i)
        service.refresh();
    flush();
    require(native->queries == 1 && changes == 1 && refreshes == 1, "refreshes must coalesce");
    service.refresh();
    flush();
    require(changes == 1 && refreshes == 2, "unchanged snapshots must not repaint");
    native->value.statuses[3] = S::NotDetermined;
    service.refresh();
    flush();
    require(service.startupMissing().isEmpty(), "optional microphone must not interrupt startup");
    require(service.takeStartupMissing().isEmpty(), "first completed startup check can be quiet");
    service.setMicrophoneEnabled(true);
    require(service.startupMissing() == AppPermissions{P::Microphone},
            "enabled microphone must affect startup");
    require(service.takeStartupMissing().isEmpty(),
            "later changes must not steal focus after the startup check");
    service.setMicrophoneEnabled(false);
    for (auto core : {P::ScreenRecording, P::Accessibility, P::InputMonitoring}) {
        native->value.statuses[static_cast<size_t>(core)] = S::Missing;
        service.refresh();
        flush();
        require(service.startupMissing().contains(core), "all core permissions affect startup");
        native->value.statuses[static_cast<size_t>(core)] = S::Granted;
    }
    native->value.statuses[0] = S::Error;
    service.refresh();
    flush();
    require(service.snapshot().status(P::ScreenRecording) == S::Error,
            "query failures must remain distinguishable");
    service.request(P::Microphone);
    service.request(P::Microphone);
    service.request(P::Accessibility);
    require(native->requests == 1 && service.requestPending(),
            "only one permission request may be pending");
    native->completion();
    flush();
    require(!service.requestPending() && !service.snapshot().granted(P::Microphone),
            "request completion alone must never grant access");
    native->value.statuses[3] = S::Restricted;
    service.refresh();
    flush();
    service.request(P::Microphone);
    require(native->requests == 1, "restricted permissions must not prompt again");
    for (auto status : {S::Denied, S::Error, S::Granted, S::Missing}) {
        native->value.statuses[3] = status;
        const int previousChanges = changes;
        service.refresh();
        flush();
        require(service.snapshot().status(P::Microphone) == status &&
                    changes == previousChanges + 1,
                "denial, failure, grant, and revocation must publish distinct transitions");
        if (status == S::Denied || status == S::Error) {
            service.request(P::Microphone);
            require(native->requests == 1,
                    "denied or unqueryable microphone access cannot prompt again");
        }
    }
    auto owner = std::make_unique<QObject>();
    QObject second;
    service.observe(owner.get(), true);
    service.observe(&second, true);
    flush();
    require(service.polling(), "visible permission surfaces activate polling");
    owner.reset();
    require(service.polling(), "one remaining surface keeps observation active");
    service.observe(&second, false);
    require(!service.polling(), "no background polling after surfaces disappear");
    for (int i = 0; i < 10; ++i) {
        service.observe(&second, true);
        service.observe(&second, false);
    }
    require(!service.polling(), "repeated show/hide must not leak observation leases");
    std::function<void()> late;
    {
        auto backend = std::make_unique<FakePermissions>();
        auto* raw = backend.get();
        raw->value.statuses[3] = S::NotDetermined;
        AppPermissionService disposable(std::move(backend));
        disposable.refresh();
        flush();
        disposable.request(P::Microphone);
        late = raw->completion;
    }
    late();
    flush(); // Must not call through a destroyed service or backend.
}
void routingPolicy() {
    using A = GlobalShortcutAction;
    for (auto action : {A::Screenshot, A::ScreenshotDelay, A::ScreenshotFixed, A::ScreenshotOcr,
                        A::ScreenshotTranslation, A::ScreenshotCopy, A::ScreenshotFullScreen,
                        A::ScreenshotFocusedWindow})
        require(requiredPermissions(action, true) == AppPermissions{P::ScreenRecording},
                "capture must not require optional smart-selection access");
    for (auto action : {A::ScreenRecord, A::ScreenRecordCopy}) {
        require(requiredPermissions(action, false) == AppPermissions{P::ScreenRecording},
                "recording without microphone");
        require(requiredPermissions(action, true) == AppPermissions({P::ScreenRecording}),
                "recording selection defers microphone access until actual recording");
    }
    require(requiredPermissions(A::TranslateSelectedText, false) ==
                AppPermissions{P::Accessibility},
            "selected text needs Accessibility");
    for (auto action :
         {A::OpenScreenRecordingFolder, A::OpenCaptureHistory, A::OpenPinToScreenManagement,
          A::OpenSettings, A::PinClipboardContent, A::PinSelectedFiles, A::FullscreenCanvas})
        require(requiredPermissions(action, true).isEmpty(),
                "permission-free actions must remain usable");
    using M = settings::SettingsGlobalMouseAction;
    for (auto action :
         {M::ScreenshotCopy, M::ScreenshotFixed, M::ScreenshotOcr, M::ScreenshotTranslation,
          M::ScreenshotQuickSave, M::ScreenshotSave, M::ScreenRecording}) {
        const auto required = requiredPermissions(action, true);
        require(required.contains(P::ScreenRecording) && required.contains(P::Accessibility) &&
                    required.contains(P::InputMonitoring),
                "mouse action requirements");
        require(!required.contains(P::Microphone),
                "mouse selection defers microphone access until actual recording");
    }
    require(!pagePermissions(false, false, false).contains(P::InputMonitoring),
            "Carbon hotkeys do not require Input Monitoring");
    require(pagePermissions(false, false, true).contains(P::Accessibility),
            "enabled selected text is relevant to hotkeys");
    auto fake = std::make_unique<FakePermissions>();
    auto* native = fake.get();
    native->value.statuses[0] = S::Missing;
    AppPermissionService service(std::move(fake));
    service.refresh();
    flush();
    require(service.takeStartupMissing() == AppPermissions{P::ScreenRecording},
            "startup surfaces missing core access");
    require(service.takeStartupMissing().isEmpty(),
            "startup navigation happens once per service lifetime");
    int blocked = 0;
    int dispatched = 0;
    int unavailable = 0;
    snow_shot::app::FeatureActionRouter featureRouter(
        [&](snow_shot::app::FeatureFamily) { ++unavailable; });
    for (int i = 0; i < 100; ++i) {
        if (service.allow(requiredPermissions(A::Screenshot, false),
                          [&](const AppPermissions& permissions) {
                              require(permissions == AppPermissions{P::ScreenRecording},
                                      "redirect identifies the missing permission");
                              ++blocked;
                          }))
            static_cast<void>(featureRouter.dispatch(snow_shot::app::FeatureFamily::Screenshot,
                                                     [&] { ++dispatched; }));
    }
    require(blocked == 100 && dispatched == 0 && unavailable == 0 && native->queries == 1,
            "routing reads only cached state and blocks feature dispatch");
    native->value.statuses[0] = S::Granted;
    service.refresh();
    flush();
    require(dispatched == 0, "granting access must not replay an action");
    require(service.allow({P::ScreenRecording}, {}),
            "a fresh user action can proceed to the existing availability gate");
    static_cast<void>(
        featureRouter.dispatch(snow_shot::app::FeatureFamily::Screenshot, [&] { ++dispatched; }));
    require(unavailable == 0 && dispatched == 1,
            "granted screen access must allow capture on every supported platform");
}
class PermissionTranslator final : public QTranslator {
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QString::fromUtf8(context) == u"SettingsPageWidget" &&
            QString::fromUtf8(source) == u"Authorized")
            return QStringLiteral("Localized authorized");
        return {};
    }
};
void pageAndAlerts() {
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(
        storage.initialize({temporary.filePath(QStringLiteral("bin")), temporary.path()}));
    require(storage.isInitialized(), "isolated storage must initialize");
    styles::ThemeManager::instance().initialize(*qApp);
    {
        auto fake = std::make_unique<FakePermissions>();
#ifdef Q_OS_MACOS
        auto* native = fake.get();
#endif
        AppPermissionService service(std::move(fake));
        service.refresh();
        flush();
        GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts, nullptr, nullptr, &service);
        const auto& registry = settings::builtInSettingsRegistry();
        require(registry.catalog().validationErrors().isEmpty(),
                "permission catalog must validate");
        settings::SettingsRuntimeSession runtime(registry, backend);
#ifdef Q_OS_MACOS
        const auto* pageDefinition = registry.catalog().page(QStringLiteral("app-permissions"));
        require(pageDefinition && pageDefinition->route == u"/settings/appPermissions",
                "macOS permissions route");
        bool followsSystem = false;
        for (const auto& node : registry.catalog().navigation()) {
            if (const auto* group = std::get_if<settings::SettingsNavigationGroupDefinition>(&node))
                for (int i = 1; i < group->pages.size(); ++i)
                    if (group->pages.at(i).pageId == u"app-permissions")
                        followsSystem = group->pages.at(i - 1).pageId == u"system-settings";
        }
        require(followsSystem, "App Permissions must immediately follow System settings");
        settings::SettingsSearchIndex search(registry);
        const auto hits = search.search(QStringLiteral("Input Monitoring"));
        require(
            std::any_of(hits.begin(), hits.end(),
                        [](const auto& hit) { return hit.location.pageId == u"app-permissions"; }),
            "permissions must be searchable");
        {
            SettingsPageWidget page(registry, QStringLiteral("app-permissions"), runtime);
            page.resize(920, 850);
            page.show();
            flush();
            require(service.polling(), "visible page watches permissions");
            auto* microphone = page.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("appPermission-microphone-settings"));
            auto* screen = page.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("appPermission-screen-recording-settings"));
            auto* accessibility = page.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("appPermission-accessibility-settings"));
            auto* input = page.findChild<adqt::widgets::AdButton*>(
                QStringLiteral("appPermission-input-monitoring-settings"));
            require(screen && accessibility && input && microphone,
                    "all permission rows use one standard action button");
            const QVector<QStringList> expectedRows{
                {QStringLiteral("screen-recording"),
                 QStringLiteral("Screen & System Audio Recording"),
                 QStringLiteral("Capture screenshots and record your screen and system audio.")},
                {QStringLiteral("accessibility"), QStringLiteral("Accessibility"),
                 QStringLiteral("Use global mouse gestures, select individual window elements, "
                                "and translate selected text.")},
                {QStringLiteral("input-monitoring"), QStringLiteral("Input Monitoring"),
                 QStringLiteral("Recognize global mouse gestures while you use other apps.")},
                {QStringLiteral("microphone"), QStringLiteral("Microphone"),
                 QStringLiteral("Optional. Record your microphone when microphone audio is "
                                "enabled for recording.")}};
            for (const QStringList& expected : expectedRows) {
                auto* row =
                    page.findChild<QWidget*>(QStringLiteral("settings-item-") + expected.at(0));
                const auto labels =
                    row != nullptr ? row->findChildren<QLabel*>() : QList<QLabel*>();
                require(row && labels.size() == 2 && labels.at(0)->text() == expected.at(1) &&
                            labels.at(1)->text() == expected.at(2) &&
                            row->findChildren<adqt::widgets::AdButton*>().size() == 1,
                        "permission rows use catalog copy and exactly one standard action");
            }
            require(page.findChildren<adqt::widgets::AdAlert*>().isEmpty(),
                    "permission page has no inline status or error bars");
            for (auto* button : {screen, accessibility, input, microphone}) {
                require(button->text() == u"Authorized" &&
                            button->accentRole() == adqt::widgets::AdButton::AccentRole::Success,
                        "granted permissions use the Authorized success button");
                button->click();
            }
            require(native->opens == 0, "Authorized buttons are status-only");
            auto* refresh =
                page.findChild<adqt::widgets::AdButton*>(QStringLiteral("sectionRefreshButton"));
            require(refresh && refresh->toolTip() == u"Refresh" &&
                        refresh->accessibleName() == u"Refresh",
                    "permission category uses the standard header refresh action");
            auto* confirmation = page.findChild<adqt::widgets::AdPopconfirm*>(
                QStringLiteral("sectionResetPopconfirm"));
            require(confirmation && !confirmation->isEnabled(),
                    "refresh does not use reset confirmation");
            const int queriesBeforeRefresh = native->queries;
            refresh->click();
            flush();
            require(native->queries == queriesBeforeRefresh + 1,
                    "header refresh requests a fresh permission snapshot");

            native->value.statuses = {S::Missing, S::Denied, S::Restricted, S::NotDetermined};
            service.refresh();
            flush();
            for (auto* button : {screen, accessibility, input}) {
                require(button->text() == u"Go to Settings" &&
                            button->accentRole() == adqt::widgets::AdButton::AccentRole::Danger,
                        "unavailable core permissions use error actions");
            }
            require(microphone->text() == u"Go to Settings" &&
                        microphone->accentRole() == adqt::widgets::AdButton::AccentRole::Orange,
                    "unavailable microphone uses a warning action");
            screen->click();
            require(native->opens == 1 && native->lastOpen == P::ScreenRecording,
                    "core action opens the matching System Settings pane");
            microphone->click();
            require(native->opens == 2 && native->lastOpen == P::Microphone,
                    "microphone action opens the matching System Settings pane");
            native->canOpen = false;
            accessibility->click();
            flush();
            require(native->opens == 3 && page.findChildren<adqt::widgets::AdAlert*>().isEmpty(),
                    "settings failures use transient messaging instead of an inline alert");
            native->canOpen = true;

            native->value.statuses[0] = S::Checking;
            service.refresh();
            flush();
            require(screen->text() == u"Checking…" &&
                        screen->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
                    "checking permissions use a neutral status button");
            screen->click();
            require(native->opens == 3, "checking buttons are non-interactive");

            native->value.statuses.fill(S::Granted);
            service.refresh();
            flush();
            PermissionTranslator translator;
            QApplication::installTranslator(&translator);
            page.retranslateUi();
            require(microphone->text() == u"Localized authorized",
                    "permission action labels retranslate");
            QApplication::removeTranslator(&translator);
            page.retranslateUi();
            page.reveal({QStringLiteral("app-permissions"), QStringLiteral("permissions"),
                         QStringLiteral("microphone")});
            require(QApplication::focusWidget() != nullptr, "deep links focus a permission row");
            flush();
            if (QApplication::arguments().contains(QStringLiteral("--render"))) {
                native->value.statuses.fill(S::Missing);
                service.refresh();
                flush();
                page.grab().save(QStringLiteral("build/app-permissions-light.png"));
                styles::ThemeManager::instance().setThemeMode(styles::ThemeMode::Dark);
                flush();
                page.grab().save(QStringLiteral("build/app-permissions-dark.png"));
                styles::ThemeManager::instance().setThemeMode(styles::ThemeMode::Light);
                flush();
                native->value.statuses.fill(S::Granted);
                service.refresh();
                flush();
            }
            page.resize(520, 900);
            flush();
            require(microphone->isVisible(), "permission actions remain visible in narrow layouts");
            page.hide();
            flush();
            require(!service.polling(), "hidden page stops polling");
        }
        require(!service.polling(), "destroyed page releases all observers");
        {
            const auto previous = service.snapshot();
            SettingsPageWidget recreated(registry, QStringLiteral("app-permissions"), runtime);
            recreated.show();
            flush();
            require(service.snapshot() == previous && service.polling() && native->requests == 0,
                    "recreating the window preserves the application snapshot without prompting");
        }
        require(!service.polling(), "recreated window releases observation on destruction");
        SettingsPageWidget hotkeys(registry, QStringLiteral("global-hotkeys"), runtime);
        SettingsPageWidget mouse(registry, QStringLiteral("global-mouse"), runtime);
        hotkeys.show();
        mouse.show();
        flush();
        auto* hotkeyAlert =
            hotkeys.findChild<adqt::widgets::AdAlert*>(QStringLiteral("appPermissionsAlert"));
        auto* mouseAlert =
            mouse.findChild<adqt::widgets::AdAlert*>(QStringLiteral("appPermissionsAlert"));
        require(hotkeyAlert && mouseAlert && !hotkeyAlert->isVisible() && !mouseAlert->isVisible(),
                "granted permissions hide both alerts");
        const auto metric = styles::ThemeManager::instance().themeColorScheme().metricAlias;
        for (SettingsPageWidget* currentPage : {&hotkeys, &mouse}) {
            auto* content = currentPage->findChild<QWidget*>(QStringLiteral("settings-content-") +
                                                             currentPage->pageId());
            require(content != nullptr, "permission alert page content must be available");
            const QMargins pageMargins = content->layout()->contentsMargins();
            require(pageMargins.top() == metric.paddingXXS &&
                        pageMargins.left() == metric.paddingLG &&
                        pageMargins.right() == metric.paddingLG,
                    "hidden permission alerts must retain the standard page top spacing");
        }
        native->value.statuses[2] = S::Missing;
        service.refresh();
        flush();
        require(!hotkeyAlert->isVisible() && mouseAlert->isVisible(),
                "input monitoring warning belongs to mouse page only");
        native->value.statuses[0] = S::Missing;
        service.refresh();
        flush();
        require(hotkeyAlert->isVisible() && mouseAlert->isVisible(),
                "screen capture access affects both pages");
        for (SettingsPageWidget* currentPage : {&hotkeys, &mouse}) {
            auto* content = currentPage->findChild<QWidget*>(QStringLiteral("settings-content-") +
                                                             currentPage->pageId());
            auto* alert = currentPage->findChild<adqt::widgets::AdAlert*>(
                QStringLiteral("appPermissionsAlert"));
            require(content && alert, "permission alert geometry must be available");
            const QMargins pageMargins = content->layout()->contentsMargins();
            require(pageMargins.top() == pageMargins.left() &&
                        pageMargins.left() == pageMargins.right() && pageMargins.top() > 0,
                    "permission alerts must have balanced top and horizontal page spacing");
        }
        auto* mouseScroll = mouse.findChild<adqt::widgets::AdScrollArea*>(
            QStringLiteral("settings-scroll-global-mouse"));
        require(mouseScroll && mouseScroll->verticalScrollBar()->maximum() > 0,
                "global mouse settings must be scrollable in the test viewport");
        mouseScroll->verticalScrollBar()->setValue(mouseScroll->verticalScrollBar()->maximum());
        mouse.reveal({QStringLiteral("global-mouse"), {}, {}});
        require(mouseScroll->verticalScrollBar()->value() ==
                    mouseScroll->verticalScrollBar()->minimum(),
                "opening a settings page must reveal introductory content above its first section");
        mouse.reveal({QStringLiteral("global-mouse"), QStringLiteral("screenshot"), {}});
        require(mouseScroll->verticalScrollBar()->value() >
                    mouseScroll->verticalScrollBar()->minimum(),
                "explicit section navigation must still align the requested section");
        settings::SettingsLocation destination;
        QObject::connect(
            &mouse, &SettingsPageWidget::commandRequested,
            [&](const settings::SettingsCommand& command) { destination = command.location; });
        auto* action = qobject_cast<adqt::widgets::AdButton*>(mouseAlert->actionsWidget());
        require(action != nullptr, "alert has a review action");
        action->click();
        require(destination.pageId == u"app-permissions" &&
                    destination.itemId == u"screen-recording",
                "alert navigates to first relevant permission");
        native->value.statuses.fill(S::Granted);
        service.refresh();
        flush();
        for (SettingsPageWidget* currentPage : {&hotkeys, &mouse}) {
            auto* content = currentPage->findChild<QWidget*>(QStringLiteral("settings-content-") +
                                                             currentPage->pageId());
            auto* alert = currentPage->findChild<adqt::widgets::AdAlert*>(
                QStringLiteral("appPermissionsAlert"));
            require(content && alert && !alert->isVisible() &&
                        content->layout()->contentsMargins().top() == metric.paddingXXS,
                    "hiding permission alerts must restore the standard page top spacing");
        }
        require(native->requests == 0, "page navigation and refresh must never prompt");
        SettingsPageWidget functions(registry, QStringLiteral("function-settings"), runtime);
        require(!functions.findChild<QWidget*>(QStringLiteral("smartSelectionPermission")),
                "old permission widget must be removed");
#else
        require(!registry.catalog().page(QStringLiteral("app-permissions")),
                "other platforms must not expose macOS permissions");
#endif
    }
    storage.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("app-permissions-tests"));
    permissionDiagnostics();
    snapshotsAndLifecycle();
    routingPolicy();
    pageAndAlerts();
    std::cout << "App permission tests passed\n";
}
