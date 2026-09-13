#ifndef SNOW_SHOT_STORAGE_STORAGERESULT_H
#define SNOW_SHOT_STORAGE_STORAGERESULT_H

#include <QString>

#include <utility>

namespace snow_shot::storage {
struct StorageResult {
    bool success = false;
    QString error;

    [[nodiscard]] static StorageResult ok() {
        return {true, {}};
    }
    [[nodiscard]] static StorageResult failure(QString message) {
        return {false, std::move(message)};
    }
};
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_STORAGERESULT_H
