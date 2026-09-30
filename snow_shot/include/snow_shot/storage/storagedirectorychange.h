#ifndef SNOW_SHOT_STORAGE_STORAGEDIRECTORYCHANGE_H
#define SNOW_SHOT_STORAGE_STORAGEDIRECTORYCHANGE_H

#include "snow_shot/storage/storageresult.h"
#include <QStringList>
#include <QMetaType>
#include <functional>

namespace snow_shot::storage {
struct StorageDirectoryProgress {
    enum class Stage { Preparing, Copying, Verifying, Switching, Cleaning };
    Stage stage = Stage::Preparing;
    QString category;
    qint64 completed = 0;
    qint64 total = 0;
};
struct StorageDirectoryChangeResult {
    bool success = false;
    QString error;
    QString warning;
};

// The caller suspends producers and drains repositories before running this transaction.
// Hooks run on the transaction worker. Activation must preserve repository identities.
struct StorageDirectoryChangeOptions {
    QString source;
    QString destination;
    QString bootstrapDirectory;
    QString executableDirectory;
    bool migrate = true;
    QStringList preparedFiles;
    std::function<bool(const QString&)> includePath;
    std::function<void(const StorageDirectoryProgress&)> progress;
    std::function<StorageResult(const QString&)> prepare;
    std::function<StorageResult(const QString&)> activate;
    std::function<void()> rollback;
    // Deterministic fault injection for transaction tests; empty in production.
    std::function<StorageResult(const QString&)> checkpoint;
};

QString storageBootstrapPath(const QString& directory, const QString& executableDirectory);
QString savedStorageDirectory(const QString& directory, const QString& executableDirectory,
                              QString* recoveryWarning = nullptr);
StorageResult validateStorageDirectory(const QString& source, const QString& destination);
StorageDirectoryChangeResult changeStorageDirectory(const StorageDirectoryChangeOptions& options);
} // namespace snow_shot::storage
Q_DECLARE_METATYPE(snow_shot::storage::StorageDirectoryProgress)
Q_DECLARE_METATYPE(snow_shot::storage::StorageDirectoryChangeResult)
#endif
