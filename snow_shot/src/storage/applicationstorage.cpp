#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/storage/applicationstorage.h"
#include <QTimer>

#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/storagelogging.h"
#include "snow_shot/storage/storageusagetracker.h"

#include "capturehistorypolicy_p.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QSet>

#include <chrono>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace snow_shot::storage {
namespace {
// A completed usage scan stays authoritative for this long, so settings-page
// show events inside the window reuse the cached snapshot instead of rescanning.
constexpr std::chrono::seconds kUsageRefreshStaleAfter{10};

template <typename Result> std::shared_future<Result> readyFuture(Result result) {
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future().share();
    promise->set_value(std::move(result));
    return future;
}

struct DirectoryCheck {
    bool available = false;
    QString error;
};

DirectoryCheck ensureWritableDirectory(const QString& path) {
    if (path.trimmed().isEmpty()) {
        return {false, QStringLiteral("The storage directory path is empty")};
    }
    QDir directory;
    if (!directory.mkpath(path)) {
        return {false, QStringLiteral("The storage directory could not be created")};
    }
    const QFileInfo info(path);
    if (!info.isDir()) {
        return {false, QStringLiteral("The storage path is not a directory")};
    }
    QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".snow-shot-write-test-XXXXXX")));
    probe.setAutoRemove(true);
    if (!probe.open()) {
        return {false, QStringLiteral("The storage directory is not writable")};
    }
    return {true, {}};
}

QString markerSelection(const QString& executableDirectory, bool* markerPresent,
                        QString* markerError) {
    if (markerPresent != nullptr) {
        *markerPresent = false;
    }
    const QString markerPath =
        QDir(executableDirectory).filePath(QStringLiteral("__data_directory"));
    QFile marker(markerPath);
    if (!marker.exists()) {
        return {};
    }
    if (markerPresent != nullptr) {
        *markerPresent = true;
    }
    if (!marker.open(QIODevice::ReadOnly)) {
        if (markerError != nullptr) {
            *markerError = QStringLiteral("The __data_directory marker could not be read");
        }
        return {};
    }
    QString value = QString::fromUtf8(marker.readAll());
    while (!value.isEmpty() && value.front() == QChar::ByteOrderMark) {
        value.remove(0, 1);
    }
    value = value.trimmed();
    if (value.isEmpty()) {
        return {};
    }
    if (QDir::isAbsolutePath(value)) {
        return QDir::cleanPath(value);
    }
    return QDir::cleanPath(QDir(executableDirectory).absoluteFilePath(value));
}

} // namespace

ApplicationStorage::ApplicationStorage(QObject* parent) : QObject(parent) {
    m_pinnedPreviewPool.setMaxThreadCount(2);
    m_pinnedFullImagePool.setMaxThreadCount(1);
    m_pinnedMaintenancePool.setMaxThreadCount(1);
    qRegisterMetaType<CaptureHistoryUsage>();
    qRegisterMetaType<AppStorageUsage>();
    qRegisterMetaType<StorageStatus>();
    connect(&diagnostics::DiagnosticsService::instance(),
            &diagnostics::DiagnosticsService::statusChanged, this,
            &ApplicationStorage::emitStatusChanged, Qt::QueuedConnection);
}

ApplicationStorage::~ApplicationStorage() {
    shutdown();
}

ApplicationStorage& ApplicationStorage::instance() {
    static ApplicationStorage storage;
    return storage;
}

StorageDirectorySelection
ApplicationStorage::resolveDirectory(const StorageInitializationOptions& options) {
    if (options.resolvedDirectory) {
        return *options.resolvedDirectory;
    }
    QString executableDirectory = options.executableDirectory;
    if (executableDirectory.isEmpty()) {
#ifdef Q_OS_WIN
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
        executableDirectory =
            QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).absolutePath();
#else
        executableDirectory = QCoreApplication::applicationDirPath();
#endif
    }
    executableDirectory = QDir::cleanPath(executableDirectory);
    const QString appDataDirectory =
        QDir::cleanPath(options.appDataDirectory.isEmpty()
                            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                            : options.appDataDirectory);

    bool markerPresent = false;
    QString markerError;
    QString markerDirectory = markerSelection(executableDirectory, &markerPresent, &markerError);
    bool savedSelection = false;
#ifdef Q_OS_WIN
    QString recoveryWarning;
    const QString saved =
        savedStorageDirectory(appDataDirectory, executableDirectory, &recoveryWarning);
    if (!saved.isEmpty()) {
        markerDirectory = saved;
        markerPresent = true;
        markerError.clear();
        savedSelection = true;
    }
#endif
    const bool customRequested = markerPresent && !markerDirectory.isEmpty();
    StorageDirectorySelection selection;
    selection.executableDirectory = executableDirectory;
    selection.requestedDirectory = customRequested ? markerDirectory : appDataDirectory;

    QString effectiveDirectory;
    StorageMode requestedMode = StorageMode::ApplicationData;
    if (!markerError.isEmpty()) {
        selection.fallbackReason = markerError;
    } else if (customRequested) {
        requestedMode = savedSelection ? StorageMode::Custom : StorageMode::Portable;
        const DirectoryCheck custom = ensureWritableDirectory(markerDirectory);
        if (custom.available) {
            effectiveDirectory = markerDirectory;
        } else {
            selection.fallbackReason = custom.error;
        }
    }

    if (effectiveDirectory.isEmpty()) {
        const DirectoryCheck fallback = ensureWritableDirectory(appDataDirectory);
        if (fallback.available) {
            effectiveDirectory = appDataDirectory;
            requestedMode = StorageMode::ApplicationData;
        } else {
            if (!selection.fallbackReason.isEmpty()) {
                selection.fallbackReason += u' ';
            }
            selection.fallbackReason +=
                QStringLiteral("The AppDataLocation fallback is unavailable: ") + fallback.error;
        }
    }

#ifdef Q_OS_WIN
    if (!recoveryWarning.isEmpty()) {
        if (!selection.fallbackReason.isEmpty())
            selection.fallbackReason += u'\n';
        selection.fallbackReason += recoveryWarning;
    }
#endif
    selection.effectiveDirectory = effectiveDirectory;
    selection.mode = effectiveDirectory.isEmpty() ? StorageMode::Degraded : requestedMode;
    return selection;
}

StorageResult ApplicationStorage::initialize(const StorageInitializationOptions& options) {
    if (m_initialized) {
        shutdown();
    }
    m_usageTracker.reset();
    m_captureHistory.reset();
    m_pinnedWindows.reset();
    m_pinnedChangeQueued.store(false);
    {
        std::lock_guard lock(m_pinnedMaintenanceMutex);
        m_pinnedMaintenancePending = false;
        m_pinnedMaintenanceRunning = false;
    }
    m_lastPinnedNotifiedRevision = 0;
    m_configuration.reset();
    const auto selection = resolveDirectory(options);
    m_executableDirectory = selection.executableDirectory;
    m_bootstrapDirectory = options.appDataDirectory.isEmpty()
                               ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                               : options.appDataDirectory;
    const QString effectiveDirectory = selection.effectiveDirectory;
    m_status = {};
    m_status.requestedDirectory = selection.requestedDirectory;
    m_status.effectiveDirectory = effectiveDirectory;
    m_status.fallbackReason = selection.fallbackReason;
    m_status.readAvailable = !effectiveDirectory.isEmpty();
    m_status.writeAvailable = !effectiveDirectory.isEmpty();
    m_status.effectiveMode = selection.mode;
    const QString configurationFile =
        effectiveDirectory.isEmpty()
            ? QString()
            : QDir(effectiveDirectory).filePath(QStringLiteral("config.json"));
    m_configuration = std::make_unique<ConfigurationStore>(
        configurationFile, m_status.readAvailable, m_status.writeAvailable,
        options.debounceMilliseconds, this);
#ifdef Q_OS_MACOS
    platform::initializeApplicationQoS(
        platform::applicationQoSForValue(
            m_configuration->value(QStringLiteral("system/application_qos")).toString())
            .value_or(platform::ApplicationQoS::Responsive));
#endif
    m_status.configurationCompatibility = m_configuration->compatibility();
    m_status.lastConfigurationError = m_configuration->lastError();
    if (m_configuration->compatibility() == ConfigurationCompatibility::FutureVersion) {
        m_status.writeAvailable = false;
        m_status.effectiveMode = StorageMode::FutureVersionReadOnly;
    }

    CaptureHistoryRepositoryOptions historyOptions;
    historyOptions.writeAvailable = m_status.writeAvailable;
    historyOptions.policy = captureHistoryPolicyFromConfiguration(*m_configuration);
    historyOptions.callbacks.recordsChanged = [this]() {
        QMetaObject::invokeMethod(
            this, [this]() { emit captureHistoryChanged(); }, Qt::QueuedConnection);
    };
    historyOptions.callbacks.usageChanged = [this](const CaptureHistoryUsage& usage) {
        QMetaObject::invokeMethod(
            this, [this, usage]() { updateHistoryUsage(usage); }, Qt::QueuedConnection);
    };
    historyOptions.callbacks.errorChanged = [this](const QString& error) {
        QMetaObject::invokeMethod(
            this, [this, error]() { updateHistoryError(error); }, Qt::QueuedConnection);
    };
    historyOptions.callbacks.policyFinished = [this](bool success, const QString& error) {
        QMetaObject::invokeMethod(
            this, [this, success, error]() { finishHistoryPolicy(success, error); },
            Qt::QueuedConnection);
    };
    historyOptions.callbacks.clearFinished = [this](bool success, const QString& error) {
        QMetaObject::invokeMethod(
            this, [this, success, error]() { finishHistoryClear(success, error); },
            Qt::QueuedConnection);
    };
    m_captureHistory = makeCaptureHistoryRepository(effectiveDirectory, std::move(historyOptions));
    m_pinnedWindows = std::make_unique<PinnedWindowRepository>(
        effectiveDirectory, m_status.writeAvailable, options.debounceMilliseconds);
    m_pinnedWindows->setChangedCallback([this]() {
        if (m_pinnedChangeQueued.exchange(true))
            return;
        QMetaObject::invokeMethod(
            this,
            [this]() {
                m_pinnedChangeQueued.store(false);
                if (m_status.directoryChanging || !m_initialized || !m_pinnedWindows)
                    return;
                const bool recordsChanged =
                    m_pinnedWindows->revision() != m_lastPinnedNotifiedRevision;
                const auto error = m_pinnedWindows->lastError();
                if (m_status.lastPinnedError != error) {
                    m_status.lastPinnedError = error;
                    emitStatusChanged();
                }
                if (recordsChanged) {
                    m_lastPinnedNotifiedRevision = m_pinnedWindows->revision();
                    emit pinnedWindowsChanged();
                }
            },
            Qt::QueuedConnection);
    });
    static_cast<void>(m_pinnedWindows->setPolicy(pinnedWindowPolicy(), false));
    m_pinnedWindows->setCompressionLevel(
        m_configuration->value(QStringLiteral("pinned_history/compression_level")).toString());
    auto* pinnedCleanupTimer = new QTimer(m_configuration.get());
    pinnedCleanupTimer->setInterval(60000);
    connect(pinnedCleanupTimer, &QTimer::timeout, this,
            &ApplicationStorage::requestPinnedWindowRetentionCleanup);
    pinnedCleanupTimer->start();
    connect(
        m_configuration.get(), &ConfigurationStore::valueChanged, this,
        [this](const QString& key, const QJsonValue&) {
            if (key.startsWith(QStringLiteral("pinned_history/")) && m_pinnedWindows) {
                if (key == QStringLiteral("pinned_history/compression_level")) {
                    m_pinnedWindows->setCompressionLevel(
                        m_configuration->value(QStringLiteral("pinned_history/compression_level"))
                            .toString());
                    return;
                }
                const auto requestedPolicy = pinnedWindowPolicy();
                if (m_pinnedWindows->policy() != requestedPolicy) {
                    static_cast<void>(m_pinnedWindows->setPolicy(requestedPolicy, false));
                    requestPinnedWindowRetentionCleanup();
                }
            }
        });
    m_status.historyUsage = m_captureHistory->usage();
    m_status.lastHistoryError = m_captureHistory->lastError();

    createUsageTracker();
    m_initialized = true;

    connect(m_configuration.get(), &ConfigurationStore::errorChanged, this,
            &ApplicationStorage::updateConfigurationError);
    connect(m_configuration.get(), &ConfigurationStore::mutationRejected, this,
            [this](const QString&, const QString& error) { updateConfigurationError(error); });
    connect(m_configuration.get(), &ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue& value) {
                if (key == QStringLiteral("screenshot_selection/smart_selection")) {
                    emit smartSelectionChanged(value.toBool());
                }
            });
    if (QCoreApplication::instance() != nullptr) {
        m_aboutToQuitConnection =
            connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
                // Consumers still hold repository pointers while their destructors run.
                // Keep storage initialized until its owner calls shutdown after them;
                // otherwise a settings read can reinitialize and replace those repositories.
                static_cast<void>(flushNow());
            });
    }

    qCInfo(storageLog) << "Storage initialized at" << effectiveDirectory
                       << "write available:" << m_status.writeAvailable;
    emitStatusChanged();
    requestPinnedWindowRetentionCleanup();
    if (effectiveDirectory.isEmpty()) {
        return StorageResult::failure(m_status.fallbackReason);
    }
    return StorageResult::ok();
}

void ApplicationStorage::createUsageTracker() {
    const auto generation = ++m_usageGeneration;
    StorageUsageTrackerOptions usageOptions;
    usageOptions.appDataDirectory = m_status.effectiveDirectory;
    usageOptions.thumbnailCacheDirectory = StorageUsageTracker::defaultThumbnailCacheDirectory();
    usageOptions.recordingTempDirectory = StorageUsageTracker::defaultRecordingTempDirectory();
    usageOptions.diagnosticsDirectories = diagnostics::DiagnosticsService::instance().directories();
    usageOptions.activeFileCutoff = QDateTime::currentDateTime();
    usageOptions.historyBytesProvider = [this]() {
        return m_captureHistory != nullptr ? m_captureHistory->usage().totalBytes : 0;
    };
    usageOptions.callbacks.usageChanged = [this, generation](const AppStorageUsage& usage) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, usage]() {
                if (generation == m_usageGeneration)
                    updateAppUsage(usage);
            },
            Qt::QueuedConnection);
    };
    usageOptions.callbacks.clearFinished = [this](StorageCacheKind kind,
                                                  const StorageResult& result) {
        QMetaObject::invokeMethod(
            this, [this, kind, result]() { finishCacheClear(kind, result); }, Qt::QueuedConnection);
    };
    m_usageTracker = std::make_unique<StorageUsageTracker>(std::move(usageOptions));
    m_status.appUsage = m_usageTracker->usage();
}

bool ApplicationStorage::isInitialized() const {
    return m_initialized;
}

StorageResult ApplicationStorage::flushNow() {
    if (m_status.directoryChanging || !m_initialized || m_configuration == nullptr) {
        return StorageResult::failure(QStringLiteral("Application storage is not initialized"));
    }
    if (m_captureHistory != nullptr) {
        m_captureHistory->drain();
        updateHistoryError(m_captureHistory->lastError());
        updateHistoryUsage(m_captureHistory->usage());
    }
    if (m_pinnedWindows != nullptr) {
        const auto pinnedResult = m_pinnedWindows->flush();
        if (!pinnedResult.success)
            return pinnedResult;
    }
    if (m_usageTracker != nullptr) {
        m_usageTracker->drain();
        m_status.appUsage = m_usageTracker->usage();
    }
    const StorageResult result = m_configuration->flushNow();
    updateConfigurationError(m_configuration->lastError());
    return result;
}

void ApplicationStorage::shutdown() {
    disconnect(m_aboutToQuitConnection);
    m_aboutToQuitConnection = {};
    if (m_directoryWorker.valid())
        m_directoryWorker.wait();
    if (!m_initialized) {
        return;
    }
    m_pinnedPreviewPool.clear();
    m_pinnedFullImagePool.clear();
    m_pinnedPreviewPool.waitForDone();
    m_pinnedFullImagePool.waitForDone();
    m_pinnedMaintenancePool.waitForDone();
    {
        std::lock_guard lock(m_pinnedMaintenanceMutex);
        m_pinnedMaintenancePending = false;
        m_pinnedMaintenanceRunning = false;
    }
    if (m_captureHistory != nullptr) {
        m_captureHistory->drain();
        m_status.lastHistoryError = m_captureHistory->lastError();
        m_status.historyUsage = m_captureHistory->usage();
    }
    if (m_configuration != nullptr) {
        static_cast<void>(m_configuration->flushNow());
        m_status.lastConfigurationError = m_configuration->lastError();
    }
    if (m_pinnedWindows != nullptr) {
        static_cast<void>(m_pinnedWindows->flush());
    }
    if (m_usageTracker != nullptr) {
        m_usageTracker->drain();
        m_status.appUsage = m_usageTracker->usage();
        m_usageTracker.reset();
    }
    m_initialized = false;
}

ConfigurationStore& ApplicationStorage::configuration() {
    Q_ASSERT(m_configuration != nullptr);
    return *m_configuration;
}

CaptureHistoryRepository& ApplicationStorage::captureHistory() {
    Q_ASSERT(m_captureHistory != nullptr);
    return *m_captureHistory;
}

PinnedWindowRepository& ApplicationStorage::pinnedWindows() {
    Q_ASSERT(m_pinnedWindows != nullptr);
    return *m_pinnedWindows;
}

StorageStatus ApplicationStorage::status() const {
    StorageStatus current = m_status;
    if (!current.directoryChanging)
        current.diagnostics = diagnostics::DiagnosticsService::instance().status();
    if (current.directoryChanging)
        return current;
    if (m_configuration != nullptr) {
        current.lastConfigurationError = m_configuration->lastError();
    }
    if (m_captureHistory != nullptr) {
        current.historyUsage = m_captureHistory->usage();
        current.lastHistoryError = m_captureHistory->lastError();
    }
    if (m_usageTracker != nullptr) {
        current.appUsage = m_usageTracker->usage();
    }
    return current;
}

CaptureHistoryPolicy ApplicationStorage::captureHistoryPolicy() const {
    return m_configuration != nullptr ? captureHistoryPolicyFromConfiguration(*m_configuration)
                                      : CaptureHistoryPolicy{};
}

QString ApplicationStorage::configurationDirectory() const {
    return m_status.effectiveDirectory;
}

bool ApplicationStorage::smartSelectionEnabled() const {
    return m_configuration != nullptr
               ? m_configuration->value(QStringLiteral("screenshot_selection/smart_selection"))
                     .toBool()
               : ConfigurationSchema::defaultValue(
                     QStringLiteral("screenshot_selection/smart_selection"))
                     .toBool();
}

bool ApplicationStorage::requestCaptureHistoryPolicy(const CaptureHistoryPolicy& policy) {
    const auto result = requestCaptureHistoryPolicyAsync(policy);
    return result.valid() &&
           (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready ||
            result.get().success);
}

std::shared_future<StorageResult>
ApplicationStorage::requestCaptureHistoryPolicyAsync(const CaptureHistoryPolicy& policy) {
    if (m_status.directoryChanging || !m_initialized || m_configuration == nullptr ||
        m_captureHistory == nullptr || !policy.isValid()) {
        updateHistoryError(QStringLiteral("The capture-history policy is invalid"));
        return readyFuture(
            StorageResult::failure(QStringLiteral("The capture-history policy is invalid")));
    }
    const QMap<QString, QJsonValue> values = captureHistoryPolicyConfigurationValues(policy);
    if (!m_configuration->setValues(values)) {
        updateConfigurationError(m_configuration->lastError());
        return readyFuture(StorageResult::failure(m_configuration->lastError()));
    }
    m_status.historyPolicyUpdating = true;
    const auto result = m_captureHistory->updatePolicy(policy);
    if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
        !result.get().success) {
        m_status.historyPolicyUpdating = false;
    }
    emitStatusChanged();
    return result;
}

bool ApplicationStorage::requestSmartSelection(bool enabled) {
    const auto result = requestSmartSelectionAsync(enabled);
    return result.valid() && result.get().success;
}

std::shared_future<StorageResult> ApplicationStorage::requestSmartSelectionAsync(bool enabled) {
    if (m_status.directoryChanging || !m_initialized || m_configuration == nullptr ||
        !m_status.writeAvailable) {
        return readyFuture(
            StorageResult::failure(QStringLiteral("Configuration storage is not writable")));
    }
    if (!m_configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                   enabled)) {
        updateConfigurationError(m_configuration->lastError());
        return readyFuture(StorageResult::failure(m_configuration->lastError()));
    }
    return readyFuture(StorageResult::ok());
}

PinnedWindowPolicy ApplicationStorage::pinnedWindowPolicy() const {
    if (!m_configuration)
        return {};
    return {
        m_configuration->value(QStringLiteral("pinned_history/enabled")).toBool(true),
        m_configuration->value(QStringLiteral("pinned_history/retention_days")).toInt(7),
        m_configuration->value(QStringLiteral("pinned_history/max_entries")).toInt(100),
        m_configuration->value(QStringLiteral("pinned_history/max_disk_mib")).toInt(1024),
        m_configuration->value(QStringLiteral("pinned_history/keep_permanently")).toBool(false)};
}

bool ApplicationStorage::requestPinnedWindowPolicy(const PinnedWindowPolicy& policy) {
    if (m_status.directoryChanging || !m_initialized || !m_status.writeAvailable ||
        !policy.isValid())
        return false;
    m_status.pinnedPolicyUpdating = true;
    emitStatusChanged();
    const auto result = m_configuration->setValues(
        {{QStringLiteral("pinned_history/enabled"), policy.enabled},
         {QStringLiteral("pinned_history/keep_permanently"), policy.keepPermanently},
         {QStringLiteral("pinned_history/retention_days"), policy.retentionDays},
         {QStringLiteral("pinned_history/max_entries"), policy.maxEntries},
         {QStringLiteral("pinned_history/max_disk_mib"), policy.maxDiskMiB}});
    m_status.pinnedPolicyUpdating = false;
    m_status.lastPinnedError = result ? QString() : m_configuration->lastError();
    emitStatusChanged();
    return result;
}

bool ApplicationStorage::requestPinnedWindowClear() {
    if (m_status.directoryChanging || !m_initialized || !m_status.writeAvailable)
        return false;
    m_status.pinnedClearing = true;
    emitStatusChanged();
    const auto result = m_pinnedWindows->clearClosed();
    m_status.pinnedClearing = false;
    m_status.lastPinnedError = result.error;
    emitStatusChanged();
    return result.success;
}

void ApplicationStorage::requestPinnedWindowRetentionCleanup() {
    if (m_status.directoryChanging || !m_initialized || !m_pinnedWindows)
        return;
    // Preserve requests that arrive during a sweep, including late source commits.
    {
        std::lock_guard lock(m_pinnedMaintenanceMutex);
        m_pinnedMaintenancePending = true;
        if (m_pinnedMaintenanceRunning)
            return;
        m_pinnedMaintenanceRunning = true;
    }
    auto* repository = m_pinnedWindows.get();
    m_pinnedMaintenancePool.start([this, repository]() {
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        for (;;) {
            {
                std::lock_guard lock(m_pinnedMaintenanceMutex);
                if (!m_pinnedMaintenancePending) {
                    m_pinnedMaintenanceRunning = false;
                    return;
                }
                m_pinnedMaintenancePending = false;
            }
            static_cast<void>(repository->enforcePolicy());
        }
    });
}

bool ApplicationStorage::requestCaptureHistoryClear() {
    const auto result = requestCaptureHistoryClearAsync();
    return result.valid() &&
           (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready ||
            result.get().success);
}

std::shared_future<StorageResult> ApplicationStorage::requestCaptureHistoryClearAsync() {
    if (m_status.directoryChanging || !m_initialized || m_captureHistory == nullptr ||
        m_status.historyClearing || !m_status.writeAvailable) {
        return readyFuture(
            StorageResult::failure(QStringLiteral("Capture-history storage is not writable")));
    }
    m_status.historyClearing = true;
    emitStatusChanged();
    const auto result = m_captureHistory->requestClear();
    if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
        !result.get().success) {
        m_status.historyClearing = false;
        emitStatusChanged();
    }
    return result;
}

void ApplicationStorage::requestStorageUsageRefresh() {
    if (m_status.directoryChanging)
        return;
    diagnostics::DiagnosticsService::instance().requestMaintenance();
    if (m_usageTracker != nullptr) {
        m_usageTracker->requestRefresh();
    }
}

void ApplicationStorage::requestStorageUsageRefreshIfStale() {
    if (m_status.directoryChanging)
        return;
    if (m_usageTracker != nullptr) {
        m_usageTracker->requestRefreshIfStale(kUsageRefreshStaleAfter);
    }
}

bool ApplicationStorage::requestThumbnailCacheClear() {
    const auto result = requestThumbnailCacheClearAsync();
    return result.valid() && result.get().success;
}

std::shared_future<StorageResult> ApplicationStorage::requestThumbnailCacheClearAsync() {
    if (m_status.directoryChanging || !m_initialized || m_usageTracker == nullptr ||
        m_status.cacheClearing) {
        return readyFuture(StorageResult::failure(
            QStringLiteral("A cache cleanup is already running or storage is unavailable")));
    }
    m_status.cacheClearing = true;
    emitStatusChanged();
    const auto result = m_usageTracker->requestClear(StorageCacheKind::ThumbnailCache);
    if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
        !result.get().success) {
        m_status.cacheClearing = false;
        emitStatusChanged();
    }
    return result;
}

bool ApplicationStorage::requestRecordingTempClear() {
    const auto result = requestRecordingTempClearAsync();
    return result.valid() && result.get().success;
}

std::shared_future<StorageResult> ApplicationStorage::requestRecordingTempClearAsync() {
    if (m_status.directoryChanging || !m_initialized || m_usageTracker == nullptr ||
        m_status.cacheClearing) {
        return readyFuture(StorageResult::failure(
            QStringLiteral("A cache cleanup is already running or storage is unavailable")));
    }
    m_status.cacheClearing = true;
    emitStatusChanged();
    const auto result = m_usageTracker->requestClear(StorageCacheKind::RecordingTemp);
    if (result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
        !result.get().success) {
        m_status.cacheClearing = false;
        emitStatusChanged();
    }
    return result;
}

void ApplicationStorage::updateConfigurationError(const QString& error) {
    if (m_status.lastConfigurationError == error) {
        return;
    }
    m_status.lastConfigurationError = error;
    emitStatusChanged();
}

void ApplicationStorage::updateHistoryError(const QString& error) {
    if (m_status.lastHistoryError == error) {
        return;
    }
    m_status.lastHistoryError = error;
    emitStatusChanged();
}

void ApplicationStorage::updateHistoryUsage(const CaptureHistoryUsage& usage) {
    if (m_status.historyUsage == usage) {
        return;
    }
    m_status.historyUsage = usage;
    emitStatusChanged();
}

void ApplicationStorage::updateAppUsage(const AppStorageUsage& usage) {
    if (m_status.appUsage == usage) {
        return;
    }
    m_status.appUsage = usage;
    emitStatusChanged();
}

void ApplicationStorage::finishHistoryClear(bool success, const QString& error) {
    m_status.historyClearing = false;
    m_status.lastHistoryError = success ? QString() : error;
    if (m_captureHistory != nullptr) {
        m_status.historyUsage = m_captureHistory->usage();
    }
    if (m_usageTracker != nullptr) {
        m_usageTracker->requestRefresh();
    }
    emit captureHistoryClearFinished(success, error);
    emit captureHistoryChanged();
    emitStatusChanged();
}

void ApplicationStorage::finishCacheClear(StorageCacheKind kind, const StorageResult& result) {
    qCInfo(storageLog) << "Cache clear finished, kind:" << static_cast<int>(kind)
                       << "success:" << result.success;
    m_status.cacheClearing = false;
    if (m_usageTracker != nullptr) {
        m_status.appUsage = m_usageTracker->usage();
    }
    emit cacheClearFinished(kind, result.success);
    emitStatusChanged();
}

void ApplicationStorage::finishHistoryPolicy(bool success, const QString& error) {
    m_status.historyPolicyUpdating = false;
    if (!success) {
        updateHistoryError(error);
    }
    emitStatusChanged();
}

void ApplicationStorage::emitStatusChanged() {
    emit storageStatusChanged(status());
}
void ApplicationStorage::setDirectoryChangeHooks(std::function<StorageResult()> suspend,
                                                 std::function<void(const QString&)> resume,
                                                 std::function<void()> drain) {
    m_suspendForDirectoryChange = std::move(suspend);
    m_drainForDirectoryChange = std::move(drain);
    m_resumeAfterDirectoryChange = std::move(resume);
}

StorageResult ApplicationStorage::requestDirectoryChange(const QString& directory, bool migrate) {
#ifndef Q_OS_WIN
    Q_UNUSED(directory);
    Q_UNUSED(migrate);
    return StorageResult::failure(tr("Custom storage directories are only supported on Windows."));
#else
    if (!m_initialized || !m_status.writeAvailable || m_status.directoryChanging ||
        m_status.historyClearing || m_status.cacheClearing || m_status.pinnedClearing ||
        m_status.historyPolicyUpdating || m_status.pinnedPolicyUpdating)
        return StorageResult::failure(
            tr("Storage is busy or unavailable. Try again when current operations finish."));
    const QString destination = QDir::cleanPath(QDir::fromNativeSeparators(directory.trimmed()));
    const auto validation = validateStorageDirectory(m_status.effectiveDirectory, destination);
    if (!validation.success)
        return validation;
    const StorageStatus before = status();
    if (m_suspendForDirectoryChange) {
        const auto suspended = m_suspendForDirectoryChange();
        if (!suspended.success)
            return suspended;
    }
    m_status = before;
    m_status.directoryChanging = true;
    m_status.writeAvailable = false;
    m_configuration->suspendWrites(true);
    m_captureHistory->suspendWrites(true);
    emitStatusChanged();
    const auto diagnosticOptions = diagnostics::DiagnosticsService::instance().options();
    m_directoryWorker =
        std::async(std::launch::async, [this, before, destination, migrate, diagnosticOptions] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            StorageDirectoryChangeResult outcome;
            if (m_drainForDirectoryChange)
                m_drainForDirectoryChange();
            m_pinnedPreviewPool.waitForDone();
            m_pinnedFullImagePool.waitForDone();
            m_pinnedMaintenancePool.waitForDone();
            m_pinnedWindows->suspendWrites(true);
            m_captureHistory->drain();
            if (m_usageTracker)
                m_usageTracker->drain();
            auto flush = m_configuration->flushNow();
            if (flush.success)
                flush = m_pinnedWindows->flush();
            if (!flush.success)
                return StorageDirectoryChangeResult{false, flush.error, {}};
            auto& diagnostics = diagnostics::DiagnosticsService::instance();
            const bool hadDiagnostics = !diagnosticOptions.directories.isEmpty();
            const QString captureDatabase = diagnostics.crashCaptureDirectory();
            if (hadDiagnostics)
                diagnostics.shutdown();
            std::unique_ptr<PinnedWindowRepository> prepared;
            bool exchanged = false;
            StorageDirectoryChangeOptions options;
            options.source = before.effectiveDirectory;
            options.destination = destination;
            options.bootstrapDirectory = m_bootstrapDirectory;
            options.executableDirectory = m_executableDirectory;
            options.migrate = migrate;
            QSet<QString> expectedPins;
            for (const auto& pin : m_pinnedWindows->summaries()) {
                if (migrate || !pin.ignored)
                    expectedPins.insert(pin.id);
            }
            // The native handler keeps writing to its process-lifetime database even
            // while the log writer is stopped. Never copy or remove that live database.
            options.includePath = [expectedPins, migrate, source = before.effectiveDirectory,
                                   captureDatabase](const QString& path) {
                const QString absolute = QDir::cleanPath(QDir(source).filePath(path));
                if (!captureDatabase.isEmpty() &&
                    (absolute.compare(captureDatabase, Qt::CaseInsensitive) == 0 ||
                     absolute.startsWith(captureDatabase + u'/', Qt::CaseInsensitive)))
                    return false;
                const QString prefix = QStringLiteral("pinned_windows_v2/pins/");
                return migrate || !path.startsWith(prefix) ||
                       expectedPins.contains(path.mid(prefix.size()).section(u'/', 0, 0));
            };
            options.preparedFiles = {QStringLiteral("pinned_windows_v2/index.json")};
            options.progress = [this](const StorageDirectoryProgress& progress) {
                QMetaObject::invokeMethod(
                    this, [this, progress] { emit directoryChangeProgress(progress); },
                    Qt::QueuedConnection);
            };
            options.prepare = [&](const QString& root) {
                prepared = std::make_unique<PinnedWindowRepository>(root, true, 60000);
                if (!prepared->lastError().isEmpty())
                    return StorageResult::failure(prepared->lastError());
                if (!migrate) {
                    const auto cleared = prepared->clearClosed();
                    if (!cleared.success)
                        return cleared;
                }
                QSet<QString> actualPins;
                for (const auto& pin : prepared->summaries())
                    actualPins.insert(pin.id);
                if (actualPins != expectedPins)
                    return StorageResult::failure(
                        tr("Some pinned windows could not be prepared in the new directory."));
                return prepared->flush();
            };
            options.activate = [&](const QString& root) {
                const auto history = m_captureHistory->relocate(root);
                if (!history.success)
                    return history;
                m_pinnedWindows->exchangeStorage(*prepared);
                exchanged = true;
                m_configuration->relocate(root);
                return StorageResult::ok();
            };
            options.rollback = [&] {
                m_configuration->relocate(before.effectiveDirectory);
                static_cast<void>(m_captureHistory->relocate(before.effectiveDirectory));
                if (exchanged) {
                    m_pinnedWindows->exchangeStorage(*prepared);
                    exchanged = false;
                }
                prepared.reset();
            };
            outcome = changeStorageDirectory(options);
            prepared.reset();
            if (hadDiagnostics) {
                auto next = diagnosticOptions;
                if (outcome.success) {
                    const QString oldRoot = QDir::cleanPath(before.effectiveDirectory) + u'/';
                    for (auto& path : next.directories) {
                        if (QDir::cleanPath(path).startsWith(oldRoot, Qt::CaseInsensitive))
                            path = QDir(destination)
                                       .filePath(
                                           QDir(before.effectiveDirectory).relativeFilePath(path));
                    }
                }
                if (!diagnostics.initialize(next))
                    outcome.warning += tr("File logging could not be restarted.");
            }
            return outcome;
        });
    auto* poll = new QTimer(this);
    poll->setInterval(25);
    connect(poll, &QTimer::timeout, this, [this, poll, before, destination] {
        if (m_directoryWorker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return;
        poll->stop();
        poll->deleteLater();
        const auto result = m_directoryWorker.get();
        m_status = before;
        if (result.success) {
            m_status.requestedDirectory = destination;
            m_status.effectiveDirectory = destination;
            m_status.effectiveMode = StorageMode::Custom;
            m_status.fallbackReason.clear();
        }
        m_usageTracker.reset();
        createUsageTracker();
        m_configuration->suspendWrites(false);
        m_captureHistory->suspendWrites(false);
        m_pinnedWindows->suspendWrites(false);
        if (m_resumeAfterDirectoryChange)
            m_resumeAfterDirectoryChange(m_status.effectiveDirectory);
        emit captureHistoryChanged();
        emit pinnedWindowsChanged();
        emitStatusChanged();
        requestStorageUsageRefresh();
        emit directoryChangeFinished(result);
    });
    poll->start();
    return StorageResult::ok();
#endif
}

} // namespace snow_shot::storage
