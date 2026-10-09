#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/runtime/memoryoptimizationcontroller.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

const QString kPolicyKey = QStringLiteral("system/memory_optimization_policy");
const QString kPolicyId = QStringLiteral("system.memory-optimization-policy");

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void missingAndInvalidValuesUseSmartControl(const QString& configurationPath) {
    QFile file(configurationPath);
    require(file.open(QIODevice::WriteOnly),
            "create an existing configuration without memory policy");
    const QJsonObject document{
        {QStringLiteral("storage"), QJsonObject{{QStringLiteral("schema_version"),
                                                 storage::ConfigurationSchema::currentVersion()}}},
        {QStringLiteral("system"),
         QJsonObject{{QStringLiteral("application_priority"), QStringLiteral("normal")}}},
    };
    const QByteArray bytes = QJsonDocument(document).toJson();
    require(file.write(bytes) == bytes.size(), "write an existing configuration fixture");
    file.close();
    storage::ConfigurationStore store(configurationPath, true, true, 60000);
    require(
        store.value(kPolicyKey) == QStringLiteral("smart_control") &&
            store.value(QStringLiteral("system/application_priority")) == QStringLiteral("normal"),
        "upgrading an existing configuration defaults the memory policy and preserves priority");
    require(store.applySnapshot({{kPolicyKey, QStringLiteral("disabled")}}) &&
                store.value(kPolicyKey) == QStringLiteral("disabled"),
            "snapshots accept the Disabled memory policy");
    require(store.applySnapshot({}) && store.value(kPolicyKey) == QStringLiteral("smart_control"),
            "omitted memory policy in an import returns to Smart Control");
    require(store.applySnapshot({{kPolicyKey, QStringLiteral("unknown")}}) &&
                store.value(kPolicyKey) == QStringLiteral("smart_control"),
            "invalid imported memory policy is repaired to Smart Control");
    const quint64 revision = store.revision();
    require(!store.setValue(kPolicyKey, QStringLiteral("unknown")) &&
                !store.setValue(kPolicyKey, false) && store.revision() == revision &&
                store.value(kPolicyKey) == QStringLiteral("smart_control"),
            "invalid direct writes cannot change memory policy or configuration revision");
}

void policyPersistsResetsAndImports(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSelectBinding::MemoryOptimizationPolicy;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
#ifdef Q_OS_WIN
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    require(backend.selectValue(binding) == QStringLiteral("smart_control") &&
                session.state(kPolicyId).acceptedValue == QStringLiteral("smart_control"),
            "the backend and settings session start with Smart Control");
    snow_shot::runtime::RuntimeActivityTracker::TimePoint now{};
    snow_shot::runtime::RuntimeActivityTracker activity([&now] { return now; });
    snow_shot::runtime::MemoryOptimizationController::Options options;
    options.clock = [&now] { return now; };
    options.enabled = [&configuration] {
        return configuration.value(kPolicyKey).toString() == QStringLiteral("smart_control");
    };
    options.safe = [] { return true; };
    int trims = 0;
    options.trim = [&trims] {
        ++trims;
        return snow_shot::runtime::MemoryTrimResult{true, 0};
    };
    snow_shot::runtime::MemoryOptimizationController controller(activity, std::move(options));
    require(controller.isScheduling(), "Smart Control starts the real controller scheduler");
    int policyChanges = 0;
    QObject connectionContext;
    QObject::connect(&configuration, &storage::ConfigurationStore::valueChanged, &connectionContext,
                     [&](const QString& key, const QJsonValue& value) {
                         if (key == kPolicyKey) {
                             // Match ApplicationController's synchronous runtime configuration
                             // hook.
                             controller.synchronizePolicy();
                             ++policyChanges;
                             require(
                                 QThread::currentThread() == configuration.thread() &&
                                     value == configuration.value(kPolicyKey) &&
                                     controller.isScheduling() ==
                                         (value.toString() == QStringLiteral("smart_control")),
                                 "policy changes notify runtime consumers on the owning thread");
                         }
                     });
    require(session.applySelectValue(binding, QStringLiteral("disabled")) && policyChanges == 1 &&
                backend.selectValue(binding) == QStringLiteral("disabled") &&
                session.state(kPolicyId).acceptedValue == QStringLiteral("disabled") &&
                !session.hasDirtyFields() && !controller.isScheduling(),
            "selecting Disabled immediately commits the policy and stops the controller scheduler");
    const quint64 revision = configuration.revision();
    require(!backend.applySelectValue(binding, QStringLiteral("unknown")) &&
                !backend.applySelectValue(binding, false) && configuration.revision() == revision &&
                policyChanges == 1 && backend.selectValue(binding) == QStringLiteral("disabled") &&
                !controller.isScheduling(),
            "the backend rejects invalid memory policies without a runtime notification");
    require(configuration.flushNow().success, "flush the Disabled memory policy");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(kPolicyKey) == QStringLiteral("disabled"),
            "the Disabled memory policy survives a configuration reload");
    require(
        session.reset(settings::SettingsSectionReset::SystemSettings) && policyChanges == 2 &&
            configuration.value(kPolicyKey) == QStringLiteral("smart_control") &&
            session.state(kPolicyId).acceptedValue == QStringLiteral("smart_control") &&
            configuration.value(QStringLiteral("system/application_priority")) ==
                storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("system/application_priority")) &&
            controller.isScheduling(),
        "Application performance reset restores preferences and immediately restarts scheduling");
    auto imported = configuration.snapshot();
    imported.insert(kPolicyKey, QStringLiteral("disabled"));
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()) &&
                backend.selectValue(binding) == QStringLiteral("disabled") && policyChanges == 3 &&
                !controller.isScheduling(),
            "configuration import immediately applies Disabled and stops the controller scheduler");
    imported.remove(kPolicyKey);
    require(
        backend.importConfigurationSnapshot(imported,
                                            storage::ConfigurationSchema::currentVersion()) &&
            backend.selectValue(binding) == QStringLiteral("smart_control") && policyChanges == 4 &&
            controller.isScheduling(),
        "configuration import defaults omitted memory policy and immediately restarts scheduling");
    require(backend.applySelectValue(binding, QStringLiteral("disabled")) &&
                !controller.isScheduling(),
            "prepare the memory policy before a rejected reset");
    const auto before = configuration.snapshot();
    configuration.suspendWrites(true);
    require(!backend.resetSection(settings::SettingsSectionReset::SystemSettings) &&
                configuration.snapshot() == before && !controller.isScheduling(),
            "a read-only Application performance reset leaves both stored values untouched");
    configuration.suspendWrites(false);
    require(backend.applySelectValue(binding, QStringLiteral("smart_control")) &&
                controller.isScheduling(),
            "selecting Smart Control immediately restarts the real scheduler");
    require(trims == 0, "policy notifications alone never execute a native memory trim");
#else
    Q_UNUSED(configurationPath);
    const auto before = configuration.snapshot();
    require(!backend.applySelectValue(binding, QStringLiteral("disabled")) &&
                !backend.applySelectValue(binding, QStringLiteral("smart_control")) &&
                configuration.snapshot() == before &&
                settings::builtInSettingsRegistry().field(kPolicyId) == nullptr,
            "non-Windows backends reject direct memory policy writes and omit the field");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated memory policy settings storage");
    missingAndInvalidValuesUseSmartControl(temporary.filePath(QStringLiteral("legacy.json")));
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize isolated application storage");
    policyPersistsResetsAndImports(temporary.filePath(QStringLiteral("data/config.json")));
    applicationStorage.shutdown();
    return 0;
}
