#include "snow_shot/storage/applicationstorage.h"

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

#include <chrono>

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
    qRegisterMetaType<CaptureHistoryUsage>();
    qRegisterMetaType<AppStorageUsage>();
    qRegisterMetaType<StorageStatus>();
}

ApplicationStorage::~ApplicationStorage() {
    shutdown();
}

ApplicationStorage& ApplicationStorage::instance() {
    static ApplicationStorage storage;
    return storage;
}

StorageResult ApplicationStorage::initialize(const StorageInitializationOptions& options) {
    if (m_initialized) {
        shutdown();
    }
    // The usage tracker's history provider reads the capture-history repository,
    // so the tracker worker must be joined before the repository is destroyed.
    m_usageTracker.reset();
    m_captureHistory.reset();
    m_pinnedWindows.reset();
    m_configuration.reset();

    const QString executableDirectory = QDir::cleanPath(options.executableDirectory.isEmpty()
                                                            ? QCoreApplication::applicationDirPath()
                                                            : options.executableDirectory);
    const QString appDataDirectory =
        QDir::cleanPath(options.appDataDirectory.isEmpty()
                            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                            : options.appDataDirectory);

    bool markerPresent = false;
    QString markerError;
    const QString markerDirectory =
        markerSelection(executableDirectory, &markerPresent, &markerError);
    const bool customRequested = markerPresent && !markerDirectory.isEmpty();
    m_status = {};
    m_status.requestedDirectory = customRequested ? markerDirectory : appDataDirectory;

    QString effectiveDirectory;
    StorageMode requestedMode = StorageMode::ApplicationData;
    if (!markerError.isEmpty()) {
        m_status.fallbackReason = markerError;
    } else if (customRequested) {
        requestedMode = StorageMode::Portable;
        const DirectoryCheck custom = ensureWritableDirectory(markerDirectory);
        if (custom.available) {
            effectiveDirectory = markerDirectory;
        } else {
            m_status.fallbackReason = custom.error;
            qCWarning(storageLog) << "Portable storage unavailable:" << custom.error;
        }
    }

    if (effectiveDirectory.isEmpty()) {
        const DirectoryCheck fallback = ensureWritableDirectory(appDataDirectory);
        if (fallback.available) {
            effectiveDirectory = appDataDirectory;
            requestedMode = StorageMode::ApplicationData;
        } else {
            if (!m_status.fallbackReason.isEmpty()) {
                m_status.fallbackReason += u' ';
            }
            m_status.fallbackReason +=
                QStringLiteral("The AppDataLocation fallback is unavailable: ") + fallback.error;
        }
    }

    m_status.effectiveDirectory = effectiveDirectory;
    m_status.readAvailable = !effectiveDirectory.isEmpty();
    m_status.writeAvailable = !effectiveDirectory.isEmpty();
    m_status.effectiveMode = effectiveDirectory.isEmpty() ? StorageMode::Degraded : requestedMode;
    const QString configurationFile = effectiveDirectory.isEmpty()
                                          ? QString()
                                          : QDir(effectiveDirectory).filePath(
                                              QStringLiteral("config.json"));
    m_configuration = std::make_unique<ConfigurationStore>(
        configurationFile, m_status.readAvailable, m_status.writeAvailable,
        options.debounceMilliseconds, this);
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
    m_pinnedWindows = std::make_unique<PinnedWindowRepository>(effectiveDirectory,
                                                                m_status.writeAvailable,
                                                                options.debounceMilliseconds);
    m_status.historyUsage = m_captureHistory->usage();
    m_status.lastHistoryError = m_captureHistory->lastError();

    StorageUsageTrackerOptions usageOptions;
    usageOptions.appDataDirectory = effectiveDirectory;
    usageOptions.thumbnailCacheDirectory = StorageUsageTracker::defaultThumbnailCacheDirectory();
    usageOptions.recordingTempDirectory = StorageUsageTracker::defaultRecordingTempDirectory();
    usageOptions.activeFileCutoff = QDateTime::currentDateTime();
    usageOptions.historyBytesProvider = [this]() {
        return m_captureHistory != nullptr ? m_captureHistory->usage().totalBytes : 0;
    };
    usageOptions.callbacks.usageChanged = [this](const AppStorageUsage& usage) {
        QMetaObject::invokeMethod(
            this, [this, usage]() { updateAppUsage(usage); }, Qt::QueuedConnection);
    };
    usageOptions.callbacks.clearFinished = [this](StorageCacheKind kind,
                                                  const StorageResult& result) {
        QMetaObject::invokeMethod(
            this, [this, kind, result]() { finishCacheClear(kind, result); },
            Qt::QueuedConnection);
    };
    m_usageTracker = std::make_unique<StorageUsageTracker>(std::move(usageOptions));
    m_status.appUsage = m_usageTracker->usage();
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
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                [this]() { shutdown(); });
    }

    qCInfo(storageLog) << "Storage initialized at" << effectiveDirectory
                       << "write available:" << m_status.writeAvailable;
    emitStatusChanged();
    if (effectiveDirectory.isEmpty()) {
        return StorageResult::failure(m_status.fallbackReason);
    }
    return StorageResult::ok();
}

bool ApplicationStorage::isInitialized() const {
    return m_initialized;
}

StorageResult ApplicationStorage::flushNow() {
    if (!m_initialized || m_configuration == nullptr) {
        return StorageResult::failure(QStringLiteral("Application storage is not initialized"));
    }
    if (m_captureHistory != nullptr) {
        m_captureHistory->drain();
        updateHistoryError(m_captureHistory->lastError());
        updateHistoryUsage(m_captureHistory->usage());
    }
    if (m_pinnedWindows != nullptr) {
        static_cast<void>(m_pinnedWindows->flush());
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
    if (!m_initialized) {
        return;
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
    if (!m_initialized || m_configuration == nullptr || m_captureHistory == nullptr ||
        !policy.isValid()) {
        updateHistoryError(QStringLiteral("The capture-history policy is invalid"));
        return readyFuture(StorageResult::failure(QStringLiteral(
            "The capture-history policy is invalid")));
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
    if (!m_initialized || m_configuration == nullptr || !m_status.writeAvailable) {
        return readyFuture(StorageResult::failure(
            QStringLiteral("Configuration storage is not writable")));
    }
    if (!m_configuration->setValue(QStringLiteral("screenshot_selection/smart_selection"),
                                   enabled)) {
        updateConfigurationError(m_configuration->lastError());
        return readyFuture(StorageResult::failure(m_configuration->lastError()));
    }
    return readyFuture(StorageResult::ok());
}

bool ApplicationStorage::requestCaptureHistoryClear() {
    const auto result = requestCaptureHistoryClearAsync();
    return result.valid() &&
           (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready ||
            result.get().success);
}

std::shared_future<StorageResult> ApplicationStorage::requestCaptureHistoryClearAsync() {
    if (!m_initialized || m_captureHistory == nullptr || m_status.historyClearing ||
        !m_status.writeAvailable) {
        return readyFuture(StorageResult::failure(
            QStringLiteral("Capture-history storage is not writable")));
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
    if (m_usageTracker != nullptr) {
        m_usageTracker->requestRefresh();
    }
}

void ApplicationStorage::requestStorageUsageRefreshIfStale() {
    if (m_usageTracker != nullptr) {
        m_usageTracker->requestRefreshIfStale(kUsageRefreshStaleAfter);
    }
}

bool ApplicationStorage::requestThumbnailCacheClear() {
    const auto result = requestThumbnailCacheClearAsync();
    return result.valid() && result.get().success;
}

std::shared_future<StorageResult> ApplicationStorage::requestThumbnailCacheClearAsync() {
    if (!m_initialized || m_usageTracker == nullptr || m_status.cacheClearing) {
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
    if (!m_initialized || m_usageTracker == nullptr || m_status.cacheClearing) {
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
} // namespace snow_shot::storage
