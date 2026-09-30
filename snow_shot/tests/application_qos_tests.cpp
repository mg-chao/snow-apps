#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/platform/macos/loginitemservice.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/applicationpriority.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThreadPool>
#include <pthread/qos.h>

#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace platform = snow_shot::platform;
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

unsigned int workerClass() {
    platform::applyApplicationQoSToCurrentThread();
    return qos_class_self();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool reload = argc == 4 && QString::fromUtf8(argv[1]) == u"--reload";
    require(argc == 2 || reload, "one initial QoS profile or a restart fixture");
    const QString profile = QString::fromUtf8(argv[reload ? 3 : 1]);
    const auto selected =
        platform::applicationQoSForValue(profile).value_or(platform::ApplicationQoS::Responsive);
    const unsigned int classes[] = {QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_USER_INITIATED,
                                    QOS_CLASS_DEFAULT, QOS_CLASS_UTILITY, QOS_CLASS_BACKGROUND};
    const unsigned int expected = classes[static_cast<unsigned int>(selected)];
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated storage directory");
    const QString path = temporary.filePath(QStringLiteral("config.json"));
    auto document = storage::ConfigurationSchema::completeDefaultDocument();
    auto system = document.value(QStringLiteral("system")).toObject();
    if (profile != u"missing")
        system.insert(QStringLiteral("application_qos"), profile);
    else
        system.remove(QStringLiteral("application_qos"));
    system.insert(QStringLiteral("application_priority"), QStringLiteral("real_time"));
    document.insert(QStringLiteral("system"), system);
    if (reload) {
        QFile source(QString::fromUtf8(argv[2]));
        require(source.open(QIODevice::ReadOnly), "read persisted restart fixture");
        document = QJsonDocument::fromJson(source.readAll()).object();
    }
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "seed saved profile");
    require(file.write(QJsonDocument(document).toJson()) > 0, "write saved profile");
    file.close();
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated application storage");
    require(qos_class_self() == expected, "startup applies native QoS to main thread");
    require(platform::snow_application_qos_active() == static_cast<unsigned int>(selected),
            "startup selects shared Rust policy");
    require(applicationStorage.configuration()
                    .value(QStringLiteral("system/application_qos"))
                    .toString() == ((profile == u"invalid" || profile == u"missing")
                                        ? QStringLiteral("user_interactive")
                                        : profile),
            "missing and invalid startup preferences normalize to Responsive");
    require(std::async(std::launch::async, workerClass).get() == expected,
            "native standard worker applies selected QoS");

    QThread thread;
    platform::configureApplicationQoSThread(&thread);
    unsigned int qtClass = 0;
    QObject::connect(
        &thread, &QThread::started, &thread,
        [&] {
            qtClass = qos_class_self();
            thread.quit();
        },
        Qt::DirectConnection);
    thread.start();
    require(thread.wait(10000) && qtClass == expected, "QObject worker starts with selected QoS");
    QThreadPool pool;
    std::promise<unsigned int> promise;
    auto result = promise.get_future();
    pool.start([&] { promise.set_value(workerClass()); });
    require(result.get() == expected && pool.waitForDone(10000), "pool work applies native QoS");

    if (reload) {
        applicationStorage.shutdown();
        return 0;
    }

    snow_shot::presentation::GlobalShortcutManager shortcuts;
    platform::macos::LoginItemService loginItems(
        {[] {
             return platform::macos::LoginItemSnapshot{platform::macos::LoginItemStatus::Enabled,
                                                       {}};
         },
         [](bool) { return platform::macos::LoginItemResult{true, {}}; }, [] { return true; },
         [] { return true; }, [](bool) { return true; }, [] {}});
    settings::BuiltInSettingsBackend backend(shortcuts, nullptr, nullptr, nullptr, &loginItems);
    const QString key = QStringLiteral("system/application_qos");
    const QString next = selected == platform::ApplicationQoS::Utility
                             ? QStringLiteral("background")
                             : QStringLiteral("utility");
    require(backend.applySelectValue(settings::SettingsSelectBinding::ApplicationQoS, next),
            "save changed preference");
    require(backend.selectValue(settings::SettingsSelectBinding::ApplicationQoS).toString() == next,
            "settings reflect saved preference");
    require(!backend.applySelectValue(settings::SettingsSelectBinding::ApplicationQoS,
                                      QStringLiteral("invalid")),
            "reject invalid preference");
    require(qos_class_self() == expected &&
                std::async(std::launch::async, workerClass).get() == expected,
            "changed preference affects neither current nor new workers before restart");
    require(applicationStorage.configuration().flushNow().success, "persist preference");
    storage::ConfigurationStore persisted(path, true, false);
    require(persisted.value(key).toString() == next, "preference survives reload");
    QProcess restarted;
    restarted.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--reload"), path, next});
    require(restarted.waitForFinished(20000) && restarted.exitStatus() == QProcess::NormalExit &&
                restarted.exitCode() == 0,
            "restarting applies the newly persisted preference to main and workers");

    auto imported = applicationStorage.configuration().snapshot();
    imported.insert(key, QStringLiteral("user_initiated"));
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()) &&
                applicationStorage.configuration().value(key).toString() == u"user_initiated" &&
                qos_class_self() == expected &&
                std::async(std::launch::async, workerClass).get() == expected,
            "import changes only the saved preference, including for newly started workers");
    imported.remove(key);
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()),
            "older archive without QoS imports successfully");
    require(applicationStorage.configuration().value(key).toString() == u"user_interactive" &&
                qos_class_self() == expected,
            "old archive uses Responsive default without applying it until restart");
    require(backend.applySelectValue(settings::SettingsSelectBinding::ApplicationQoS, next) &&
                backend.resetSection(settings::SettingsSectionReset::SystemSettings),
            "Core reset succeeds");
    require(applicationStorage.configuration().value(key).toString() == u"user_interactive" &&
                applicationStorage.configuration()
                        .value(QStringLiteral("system/application_priority"))
                        .toString() == u"real_time" &&
                qos_class_self() == expected,
            "Core reset restores saved QoS and preserves legacy priority and active policy");
    require(!settings::applyApplicationPriority(settings::ApplicationPriority::High),
            "macOS legacy priority is unsupported");
    applicationStorage.shutdown();
    return 0;
}
