#ifndef SNOW_SHOT_STORAGE_PERSISTEDSELECTIONCODEC_H
#define SNOW_SHOT_STORAGE_PERSISTEDSELECTIONCODEC_H

#include "snow_shot/storage/capturehistorytypes.h"

#include <QJsonObject>
#include <QJsonValue>

namespace snow_shot::storage {
struct PersistedSelectionNormalization {
    PersistedSelection value;
    bool valid = false;
    bool changed = false;
};

[[nodiscard]] QJsonObject persistedSelectionToJson(const PersistedSelection& selection);
[[nodiscard]] PersistedSelectionNormalization normalizePersistedSelection(const QJsonValue& value);
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PERSISTEDSELECTIONCODEC_H
