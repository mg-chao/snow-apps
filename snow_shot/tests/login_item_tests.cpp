#include "snow_shot/platform/macos/loginitemservice.h"
#include <QCoreApplication>
#include <QSettings>
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
    int changes = 0;
    int opened = 0;
    LoginItemService service{operations()};
    bool initialized() {
        QSettings settings(directory.filePath(QStringLiteral("state.ini")), QSettings::IniFormat);
        return settings.value(QStringLiteral("initialized"), false).toBool();
    }
    LoginItemOperations operations() {
        return {[this] { return native; },
                [this](bool enabled) -> LoginItemResult {
                    ++changes;
                    require(service.pending(), "operation must expose pending state");
                    require(!service.setEnabled(!enabled).success,
                            "reentrant mutations must be rejected");
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
                    preference = value;
                    return preferenceWritable;
                },
                [this] { ++opened; }};
    }
};
void registration() {
    Fixture f;
    require(f.service.initialize(true).success && f.changes == 1 && f.preference,
            "first eligible launch must enable automatically");
    require(f.initialized(), "automatic attempt must persist the local marker");
    require(f.service.initialize(true).success && f.changes == 1, "initialize is idempotent");
    f.native.status = LoginItemStatus::ApprovalRequired;
    f.service.refresh();
    require(f.service.snapshot().requested() && !f.service.hint().isEmpty(),
            "revoked approval needs guidance");
    require(f.service.initialize(true).success && f.changes == 1,
            "startup respects revoked consent");
    f.service.openSettings();
    require(f.opened == 1, "settings action delegates to native API");
    require(f.service.setEnabled(false).success && !f.preference,
            "disable unregisters approval-pending item");
    require(f.service.setEnabled(false).success && f.changes == 2, "disable is idempotent");
    f.approval = true;
    require(f.service.setEnabled(true).success &&
                f.service.snapshot().status == LoginItemStatus::ApprovalRequired,
            "registration awaiting consent is not an active login permission");
    f.native.status = LoginItemStatus::Unregistered;
    require(f.service.initialize(true).success && f.changes == 3,
            "external removal must not be undone");
    require(!f.service.snapshot().requested(), "refresh reflects external removal");
    f.approval = false;
    require(f.service.setEnabled(true).success && f.changes == 4,
            "explicit reset can restore enabled default");
}
void failuresAndEligibility() {
    Fixture disabled;
    require(disabled.service.initialize(false).success && disabled.initialized() &&
                disabled.changes == 0,
            "existing opt-out must be preserved");
    Fixture unavailable;
    unavailable.native = {LoginItemStatus::Unavailable, QStringLiteral("install app")};
    require(unavailable.service.initialize(true).success && !unavailable.initialized(),
            "ineligible app must not consume initialization");
    require(!unavailable.service.setEnabled(true).success && unavailable.changes == 0,
            "ineligible app cannot register");
    unavailable.native = {LoginItemStatus::Unregistered, {}};
    require(unavailable.service.initialize(true).success && unavailable.changes == 1,
            "moving to Applications allows initial registration");
    Fixture marker;
    marker.markerWritable = false;
    require(!marker.service.initialize(true).success && marker.changes == 0,
            "marker failure prevents automatic registration");
    require(!marker.service.setEnabled(true).success && marker.changes == 0,
            "marker failure prevents explicit registration");
    Fixture native;
    native.failNative = true;
    require(!native.service.initialize(true).success && native.initialized(),
            "failed native attempt is remembered");
    require(native.service.initialize(true).success && native.changes == 1,
            "do not repeat failed automatic requests");
    native.failNative = false;
    require(native.service.setEnabled(true).success, "explicit retry recovers native failure");
    native.failNative = true;
    require(!native.service.setEnabled(false).success && native.service.snapshot().requested(),
            "failed disable retains actual native status");
    Fixture persistence;
    persistence.preferenceWritable = false;
    require(!persistence.service.setEnabled(true).success &&
                persistence.service.snapshot().requested() && !persistence.service.pending(),
            "preference failure retains successful native status and clears pending");
    Fixture approved;
    approved.native.status = LoginItemStatus::ApprovalRequired;
    require(approved.service.initialize(true).success && approved.changes == 0,
            "pre-existing revoked approval must not trigger registration");
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
    registration();
    failuresAndEligibility();
    launchAndPaths();
    std::cout << "Login item tests passed\n";
}
