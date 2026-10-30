#include "shell_context_menu_test_registry.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/platform/windows/autostartregistration.h"
#include "snow_shot/platform/windows/packagedcontextmenupreference.h"
#include "snow_shot/storage/storagedirectorychange.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace snow_shot::platform::windows;
    require(snow_shot::app::distribution::isStoreManaged(), "test must compile the Store policy");
    for (const auto state : {AutoStartState::Unavailable, AutoStartState::DisabledByUser,
                             AutoStartState::DisabledByPolicy, AutoStartState::EnabledByPolicy})
        require(!AutoStartRegistration::canChangeState(state),
                "Windows user and policy decisions remain authoritative");
    require(AutoStartRegistration::canChangeState(AutoStartState::Disabled) &&
                AutoStartRegistration::canChangeState(AutoStartState::Enabled),
            "ordinary startup states can be changed explicitly");
#ifdef Q_OS_WIN
    require(snow_shot::app::distribution::runtimePackageFamilyName().isEmpty(),
            "the native Store-policy fixture runs without registered package identity");
    for (int query = 0; query < 3; ++query) {
        const auto startup = AutoStartRegistration::snapshot();
        require(!startup.valid && !startup.exists && startup.state == AutoStartState::Unavailable &&
                    !startup.error.isEmpty() && AutoStartRegistration::hint() == startup.error,
                "repeated unpackaged startup reads report unavailable without invoking WinRT");
    }
    for (const bool enabled : {false, true}) {
        QString startupError;
        require(!AutoStartRegistration::setEnabled(enabled, &startupError) &&
                    !startupError.isEmpty(),
                "unpackaged Store policy cannot change native startup registration");
    }
#endif
    for (const bool elevated : {false, true}) {
        const auto presentation = administratorPresentation({true, true, elevated}, true, false);
        require(!presentation.launchEnabled && !presentation.restartEnabled,
                "Store controls never offer elevation");
    }
    bool persisted = false;
    require(!changeStartupMode(
                 StartupMode::ElevatedTask,
                 [&] {
                     persisted = true;
                     return true;
                 }).success &&
                !persisted,
            "Store startup refuses elevation before changing state");
    require(!restartAsAdministrator([] { return true; }).success,
            "Store restart never launches an elevation helper");
    require(dispatchAdministratorHelper(
                {QStringLiteral("snow_shot.exe"), QStringLiteral("--administrator-helper")}) == 20,
            "Store rejects helper command lines");
    require(reconcileStartupMode(StartupMode::Registry).success,
            "launch reconciliation does not require or modify a startup registration");
    require(removeInstallationStartup(QStringLiteral("C:/unused-store-installation")).success &&
                migrateInstallationStartup(QStringLiteral("C:/unused-store-installation"),
                                           QStringLiteral("D:/unused-store-installation"))
                    .success,
            "Store installation maintenance never repairs native startup registrations");

    QTemporaryDir fixture;
    require(fixture.isValid(), "storage checks use an isolated writable fixture");
    shell_context_menu_tests::Registry registry;
    auto options = registry.options();
    options.packagedPreferencePath = fixture.filePath(QStringLiteral("context-menu-enabled"));
    const auto preferencePath = options.packagedPreferencePath.toStdWString();
    ShellContextMenuRegistration shell(options);
    bool preferenceChanged = false;
    require(!ShellContextMenuRegistration::isSupported(),
            "Store shell registration still belongs to the manifest");
    require(packaged_context_menu::isEnabled(preferencePath),
            "a new installation enables the command before the app first runs");
    require(!shell.changeMode(ShellContextMenuMode::Disabled, [] { return false; }).success &&
                !QFile::exists(options.packagedPreferencePath),
            "a rejected first change restores the absent preference");
    require(shell.changeMode(ShellContextMenuMode::Disabled, QString(),
                             [&] {
                                 preferenceChanged = true;
                                 return true;
                             })
                    .success &&
                preferenceChanged && !packaged_context_menu::isEnabled(preferencePath),
            "disabling persists a preference readable by the Qt-free Explorer command");
    require(!shell.changeMode(ShellContextMenuMode::ImageAndDocumentFiles, [] { return false; })
                    .success &&
                !packaged_context_menu::isEnabled(preferencePath),
            "configuration save failure rolls back the Explorer preference");
    require(shell.setMode(ShellContextMenuMode::ImageAndDocumentFiles) &&
                packaged_context_menu::isEnabled(preferencePath) && shell.refreshLabels(),
            "reenabling and startup reconciliation restore the command");
    require(registry.writes == 0 && registry.notifications == 2 && registry.keys.isEmpty(),
            "packaged changes notify Explorer without writing classic registry entries");
    require(shell.setMode(ShellContextMenuMode::ImageAndDocumentFiles, QStringLiteral("e")) &&
                packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::ImageAndDocumentFiles,
                                                      L'E'},
            "packaged preferences persist a canonical classic menu access key");
    preferenceChanged = false;
    require(!shell.changeMode(ShellContextMenuMode::ImageAndDocumentFiles, QStringLiteral("Ctrl+E"),
                              [&] {
                                  preferenceChanged = true;
                                  return true;
                              })
                    .success &&
                !preferenceChanged &&
                packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::ImageAndDocumentFiles,
                                                      L'E'},
            "invalid access keys cannot change either packaged preference");
    require(
        !shell.changeMode(ShellContextMenuMode::Disabled, QStringLiteral("7"), [] { return false; })
                .success &&
            packaged_context_menu::readPreference(preferencePath) ==
                packaged_context_menu::Preference{ShellContextMenuMode::ImageAndDocumentFiles,
                                                  L'E'},
        "failed commits restore the menu mode and access key together");
    require(shell.setMode(ShellContextMenuMode::Disabled, QStringLiteral("7")) &&
                packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::Disabled, L'7'} &&
                shell.setMode(ShellContextMenuMode::ImageAndDocumentFiles, QStringLiteral("7")) &&
                packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::ImageAndDocumentFiles,
                                                      L'7'},
            "disabled menus retain their assigned access keys when reenabled");
    require(shell.setMode(ShellContextMenuMode::ImageAndDocumentFiles) &&
                packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::ImageAndDocumentFiles},
            "clearing the assignment leaves the menu enabled without an access key");
    require(shell.setMode(ShellContextMenuMode::ImageFiles, QStringLiteral("E")) &&
                packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::ImageFiles, L'E'},
            "image-only scope and its mnemonic round-trip through the packaged preference");
    require(!shell.changeMode(ShellContextMenuMode::ImageAndDocumentFiles, [] { return false; })
                    .success &&
                packaged_context_menu::readPreference(preferencePath).mode ==
                    ShellContextMenuMode::ImageFiles,
            "failed scope commits restore the image-only packaged preference");
    QFile malformed(options.packagedPreferencePath);
    for (const QByteArray bytes : {QByteArray("0"), QByteArray("1"), QByteArray("2"),
                                   QByteArray("0E"), QByteArray("1E"), QByteArray("2E")}) {
        require(malformed.open(QIODevice::WriteOnly) && malformed.write(bytes) == bytes.size(),
                "create legacy or scoped packaged preferences");
        malformed.close();
        const auto preference = packaged_context_menu::readPreference(preferencePath);
        const auto expected = bytes[0] == '0'   ? ShellContextMenuMode::Disabled
                              : bytes[0] == '2' ? ShellContextMenuMode::ImageFiles
                                                : ShellContextMenuMode::ImageAndDocumentFiles;
        require(preference.mode == expected &&
                    preference.accessKey == (bytes.size() == 2 ? L'E' : L'\0'),
                "legacy and image-only payloads preserve scope and optional access keys");
    }
    for (const QByteArray bytes : {QByteArray(), QByteArray("invalid"), QByteArray("1Eextra"),
                                   QByteArray("1&"), QByteArray("1e"), QByteArray("3E")}) {
        require(malformed.open(QIODevice::WriteOnly) && malformed.write(bytes) == bytes.size(),
                "create an invalid preference");
        malformed.close();
        require(packaged_context_menu::readPreference(preferencePath) ==
                    packaged_context_menu::Preference{ShellContextMenuMode::Disabled},
                "invalid data cannot enable the command or supply a menu mnemonic");
    }
    options.packagedPreferencePath += QStringLiteral("/blocked");
    preferenceChanged = false;
    require(!ShellContextMenuRegistration(options)
                    .changeMode(ShellContextMenuMode::Disabled,
                                [&] {
                                    preferenceChanged = true;
                                    return true;
                                })
                    .success &&
                !preferenceChanged,
            "a native preference write failure never commits application settings");
    const auto first = snow_shot::storage::storageBootstrapPath(
        fixture.path(),
        QStringLiteral("C:/Program Files/WindowsApps/SnowApps.SnowShot_1.0.0.0_x64/bin"));
    const auto updated = snow_shot::storage::storageBootstrapPath(
        fixture.path(), QStringLiteral("D:/WindowsApps/SnowApps.SnowShot_2.0.0.0_arm64/bin"));
    require(first == updated,
            "package version, volume and architecture do not change a saved storage selection");
    const auto custom = fixture.filePath(QStringLiteral("custom-data"));
    QFile selection(first);
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("directory"), custom}}).toJson();
    require(selection.open(QIODevice::WriteOnly) && selection.write(bytes) == bytes.size(),
            "persist custom directory fixture");
    selection.close();
    require(snow_shot::storage::savedStorageDirectory(
                fixture.path(),
                QStringLiteral("D:/WindowsApps/SnowApps.SnowShot_2.0.0.0_arm64/bin")) == custom,
            "an update restores the same custom data directory");
    return 0;
}
