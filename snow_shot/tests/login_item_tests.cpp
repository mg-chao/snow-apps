#include "login_item_test_support.h"
#include "snow_shot/platform/macos/loginitemservice.h"
#include <QCoreApplication>
#include <QSettings>
#include <QSemaphore>
#include <QTimer>
#include <atomic>
#include <memory>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::platform::macos;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
struct Fixture {
    QTemporaryDir directory;
    LoginItemSnapshot native{LoginItemStatus::Unregistered, {}};
    bool markerWritable = true;
    bool preferenceWritable = true;
    bool preference = false;
    bool failNative = false;
    bool approval = false;
    int queries = 0;
    int changes = 0;
    int opened = 0;
    LoginItemService service{operations()};
    Fixture() {
        login_item_test::idle(service);
    }
    bool initialized() {
        QSettings settings(directory.filePath(QStringLiteral("state.ini")), QSettings::IniFormat);
        return settings.value(QStringLiteral("initialized"), false).toBool();
    }
    LoginItemOperations operations() {
        return {[this] {
                    require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                            "status must run off the GUI thread");
                    ++queries;
                    return native;
                },
                [this](bool enabled) -> LoginItemResult {
                    ++changes;
                    require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                            "native mutation must run off the GUI thread");
                    if (failNative)
                        return {false, QStringLiteral("native failure")};
                    native.status = enabled ? (approval ? LoginItemStatus::ApprovalRequired
                                                        : LoginItemStatus::Enabled)
                                            : LoginItemStatus::Unregistered;
                    return {};
                },
                [this] { return initialized(); },
                [this] {
                    if (!markerWritable)
                        return false;
                    QSettings settings(directory.filePath(QStringLiteral("state.ini")),
                                       QSettings::IniFormat);
                    settings.setValue(QStringLiteral("initialized"), true);
                    settings.sync();
                    return settings.status() == QSettings::NoError;
                },
                [this](bool value) {
                    require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                            "preference publication must run on the GUI thread");
                    preference = value;
                    return preferenceWritable;
                },
                [this] { ++opened; }};
    }
};
void refreshNotifications() {
    Fixture f;
    int notifications = 0;
    int busyNotifications = 0;
    QObject::connect(&f.service, &LoginItemService::changed, [&] {
        ++notifications;
        if (f.service.pending())
            ++busyNotifications;
    });
    const int beforeRefresh = f.queries;
    login_item_test::refresh(f.service);
    login_item_test::refresh(f.service);
    require(f.queries == beforeRefresh + 2, "refresh must still observe current native state");
    require(notifications == 0, "unchanged refresh must not notify settings consumers");
    f.native.status = LoginItemStatus::Unavailable;
    login_item_test::refresh(f.service);
    require(notifications == 1 && !f.service.available(), "status change must notify consumers");
    f.native.error = QStringLiteral("install app");
    login_item_test::refresh(f.service);
    require(notifications == 2 && f.service.hint() == f.native.error,
            "hint change must notify even when status is unchanged");
    login_item_test::refresh(f.service);
    require(notifications == 2, "unchanged status and hint must not notify twice");
    f.native = {LoginItemStatus::Unregistered, {}};
    login_item_test::refresh(f.service);
    require(notifications == 3 && f.service.hint().isEmpty(), "cleared hint must notify consumers");
    notifications = 0;
    require(login_item_test::change(f.service, true).success, "registration succeeds");
    require(notifications == 2 && busyNotifications == 1 && !f.service.pending(),
            "registration must still notify both busy and idle transitions");
    notifications = 0;
    busyNotifications = 0;
    f.failNative = true;
    require(!login_item_test::change(f.service, false).success, "native mutation fails");
    require(notifications == 2 && busyNotifications == 1 && !f.service.pending() &&
                f.service.snapshot().requested(),
            "failed mutation must notify busy and idle even with an unchanged snapshot");
}
void registration() {
    Fixture f;
    require(login_item_test::change(f.service, true, true).success && f.changes == 1 &&
                f.preference,
            "first eligible launch must enable automatically");
    require(f.initialized(), "automatic attempt must persist the local marker");
    require(login_item_test::change(f.service, true, true).success && f.changes == 1,
            "initialize is idempotent");
    f.native.status = LoginItemStatus::ApprovalRequired;
    login_item_test::refresh(f.service);
    require(f.service.snapshot().requested() && !f.service.hint().isEmpty(),
            "revoked approval needs guidance");
    require(login_item_test::change(f.service, true, true).success && f.changes == 1,
            "startup respects revoked consent");
    f.service.openSettings();
    require(f.opened == 1, "settings action delegates to native API");
    require(login_item_test::change(f.service, false).success && !f.preference,
            "disable unregisters approval-pending item");
    require(login_item_test::change(f.service, false).success && f.changes == 2,
            "disable is idempotent");
    f.approval = true;
    require(login_item_test::change(f.service, true).success &&
                f.service.snapshot().status == LoginItemStatus::ApprovalRequired,
            "registration awaiting consent is not an active login permission");
    f.native.status = LoginItemStatus::Unregistered;
    require(login_item_test::change(f.service, true, true).success && f.changes == 3,
            "external removal must not be undone");
    require(!f.service.snapshot().requested(), "refresh reflects external removal");
    f.approval = false;
    require(login_item_test::change(f.service, true).success && f.changes == 4,
            "explicit reset can restore enabled default");
}
void failuresAndEligibility() {
    Fixture disabled;
    require(login_item_test::change(disabled.service, false, true).success &&
                disabled.initialized() && disabled.changes == 0,
            "existing opt-out must be preserved");
    Fixture unavailable;
    unavailable.native = {LoginItemStatus::Unavailable, QStringLiteral("install app")};
    require(login_item_test::change(unavailable.service, true, true).success &&
                !unavailable.initialized(),
            "ineligible app must not consume initialization");
    require(!login_item_test::change(unavailable.service, true).success && unavailable.changes == 0,
            "ineligible app cannot register");
    unavailable.native = {LoginItemStatus::Unregistered, {}};
    require(login_item_test::change(unavailable.service, true, true).success &&
                unavailable.changes == 1,
            "moving to Applications allows initial registration");
    Fixture marker;
    marker.markerWritable = false;
    require(!login_item_test::change(marker.service, true, true).success && marker.changes == 0,
            "marker failure prevents automatic registration");
    require(!login_item_test::change(marker.service, true).success && marker.changes == 0,
            "marker failure prevents explicit registration");
    Fixture native;
    native.failNative = true;
    require(!login_item_test::change(native.service, true, true).success && native.initialized(),
            "failed native attempt is remembered");
    require(login_item_test::change(native.service, true, true).success && native.changes == 1,
            "do not repeat failed automatic requests");
    native.failNative = false;
    require(login_item_test::change(native.service, true).success,
            "explicit retry recovers native failure");
    native.failNative = true;
    require(!login_item_test::change(native.service, false).success &&
                native.service.snapshot().requested(),
            "failed disable retains actual native status");
    Fixture persistence;
    persistence.preferenceWritable = false;
    require(!login_item_test::change(persistence.service, true).success &&
                persistence.service.snapshot().requested() && !persistence.service.pending(),
            "preference failure retains successful native status and clears pending");
    Fixture approved;
    approved.native.status = LoginItemStatus::ApprovalRequired;
    require(login_item_test::change(approved.service, true, true).success && approved.changes == 0,
            "pre-existing revoked approval must not trigger registration");
}
struct BlockingState {
    std::atomic_int queries{0};
    std::atomic_int changes{0};
    std::atomic_int queryExits{0};
    std::atomic_bool blockQuery{true};
    std::atomic_bool blockChange{false};
    std::atomic_bool enabled{false};
    std::atomic_bool initialized{false};
    QSemaphore entered;
    QSemaphore release;
    int saved = 0;
    LoginItemOperations operations() {
        return {[this] {
                    require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                            "blocked status query is off the GUI thread");
                    ++queries;
                    if (blockQuery.exchange(false)) {
                        entered.release();
                        release.acquire();
                    }
                    const LoginItemSnapshot snapshot{
                        enabled ? LoginItemStatus::Enabled : LoginItemStatus::Unregistered, {}};
                    ++queryExits;
                    return snapshot;
                },
                [this](bool value) {
                    require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                            "blocked registration is off the GUI thread");
                    ++changes;
                    if (blockChange.exchange(false)) {
                        entered.release();
                        release.acquire();
                    }
                    enabled = value;
                    return LoginItemResult{};
                },
                [this] { return initialized.load(); },
                [this] {
                    initialized = true;
                    return true;
                },
                [this](bool) {
                    require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                            "completion persists preferences on the GUI thread");
                    ++saved;
                    return true;
                },
                [] {}};
    }
    void waitUntilBlocked() {
        login_item_test::waitFor([&] { return entered.tryAcquire(); });
        bool delivered = false;
        QTimer::singleShot(0, QCoreApplication::instance(), [&] { delivered = true; });
        login_item_test::waitFor([&] { return delivered; });
    }
};
void blockedRefreshAndChanges() {
    BlockingState state;
    LoginItemService service(state.operations());
    require(state.queries == 0, "construction never queries native status inline");
    state.waitUntilBlocked();
    for (int i = 0; i < 100; ++i)
        service.refresh();
    require(state.queries == 1, "activation bursts cannot overlap a blocked query");
    state.release.release();
    login_item_test::idle(service);
    require(state.queries == 2, "one trailing refresh observes changes during the first query");
    for (int i = 0; i < 100; ++i)
        service.refresh();
    login_item_test::idle(service);
    require(state.queries == 3, "same-event refresh requests are coalesced");

    state.blockQuery = true;
    service.refresh();
    state.waitUntilBlocked();
    state.blockChange = true;
    bool completed = false;
    require(service.setEnabled(true,
                               [&](LoginItemResult result) {
                                   require(QThread::currentThread() == service.thread(),
                                           "GUI completion callback");
                                   completed = result.success;
                               })
                    .success &&
                service.pending(),
            "a mutation can queue behind a refresh");
    const auto future = service.completionFuture();
    require(!service.setEnabled(false).success, "overlapping mutations are rejected");
    for (int i = 0; i < 100; ++i)
        service.refresh();
    require(state.changes == 0 && !completed && state.saved == 0,
            "no registration or persistence may overtake the query");
    state.release.release();
    state.waitUntilBlocked();
    require(service.pending() && !completed && !service.snapshot().requested(),
            "blocked registration retains the old snapshot and pending state");
    state.release.release();
    login_item_test::idle(service);
    require(completed && future.get().success && service.snapshot().requested() && state.saved == 1,
            "native completion publishes state and persists once");

    state.blockChange = true;
    require(service.setEnabled(false).success, "unregistration admitted");
    state.waitUntilBlocked();
    require(service.pending() && service.snapshot().requested(), "unregister remains responsive");
    state.release.release();
    login_item_test::idle(service);
    require(!service.snapshot().requested() && state.changes == 2 && state.saved == 2,
            "unregistration completes asynchronously");
}
void asynchronousInitializationAndDestruction() {
    BlockingState state;
    {
        LoginItemService service(state.operations());
        require(service.initialize(true).success, "startup initialization admitted");
        state.waitUntilBlocked();
        require(!state.initialized && state.changes == 0, "startup cannot overtake its query");
        state.release.release();
        login_item_test::idle(service);
        require(service.completionFuture().get().success && state.initialized &&
                    state.changes == 1 && state.saved == 1,
                "first startup registers and persists asynchronously");
    }
    state.blockQuery = true;
    auto service = std::make_unique<LoginItemService>(state.operations());
    state.waitUntilBlocked();
    bool completed = false;
    require(service->setEnabled(false, [&](LoginItemResult) { completed = true; }).success,
            "queued mutation admitted before destruction");
    const auto future = service->completionFuture();
    const int before = state.queryExits;
    service.reset();
    require(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
                !future.get().success,
            "destruction resolves outstanding completion without waiting for native work");
    state.release.release();
    login_item_test::waitFor([&] { return state.queryExits > before; });
    QCoreApplication::processEvents();
    require(!completed && state.changes == 1 && state.saved == 1,
            "destroyed receiver cannot apply queued registration, publish, or persist");
}
void launchAndPaths() {
    const QString user = QStringLiteral("/Users/example/Applications");
    require(loginItemLocationAllowed(QStringLiteral("/Applications/Snow Shot.app"), user),
            "system Applications allowed");
    require(loginItemLocationAllowed(user + QStringLiteral("/Snow Shot.app"), user),
            "user Applications allowed");
    for (const auto& path :
         {QStringLiteral("/Applications-other/Snow Shot.app"),
          QStringLiteral("/Volumes/Snow Shot/Snow Shot.app"),
          QStringLiteral("/private/var/AppTranslocation/Snow Shot.app"),
          QStringLiteral("/build/snow_shot.app"), QStringLiteral("/Applications/snow_shot")})
        require(!loginItemLocationAllowed(path, user), "non-installed bundle rejected");
    const QStringList manual{QStringLiteral("snow_shot"), QStringLiteral("--show-main-window")};
    require(loginItemLaunchArguments(manual, false) == manual, "manual launch preserved");
    const auto login = loginItemLaunchArguments(manual, true);
    require(login.contains(QStringLiteral("--autostart")) &&
                loginItemLaunchArguments(login, true) == login,
            "native login maps idempotently to quiet forwarding");
    require(loginItemAutomaticRegistrationAllowed(login), "ordinary login may initialize");
    for (const auto& flag :
         {QStringLiteral("--e2e-instance-id=test"), QStringLiteral("--update-probe"),
          QStringLiteral("--recording-macos-probe"), QStringLiteral("--self-test")})
        require(!loginItemAutomaticRegistrationAllowed({QStringLiteral("snow_shot"), flag}),
                "test/probe excluded");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    refreshNotifications();
    registration();
    failuresAndEligibility();
    blockedRefreshAndChanges();
    asynchronousInitializationAndDestruction();
    launchAndPaths();
    std::cout << "Login item tests passed\n";
}
