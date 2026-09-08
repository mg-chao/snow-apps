#ifndef SNOW_SHOT_STORAGE_CAPTUREHISTORYPOLICY_P_H
#define SNOW_SHOT_STORAGE_CAPTUREHISTORYPOLICY_P_H

#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/configurationstore.h"

#include <QJsonValue>
#include <QMap>

namespace snow_shot::storage {
inline CaptureHistoryPolicy
captureHistoryPolicyFromConfiguration(const ConfigurationStore& configuration) {
    return {
        configuration.value(QStringLiteral("capture_history/enabled")).toBool(true),
        configuration.value(QStringLiteral("capture_history/retention_days")).toInt(7),
        configuration.value(QStringLiteral("capture_history/max_entries")).toInt(100),
        configuration.value(QStringLiteral("capture_history/max_disk_mib")).toInt(1024),
        configuration.value(QStringLiteral("capture_history/keep_permanently")).toBool(false),
    };
}

inline QMap<QString, QJsonValue>
captureHistoryPolicyConfigurationValues(const CaptureHistoryPolicy& policy) {
    return {
        {QStringLiteral("capture_history/enabled"), policy.enabled},
        {QStringLiteral("capture_history/keep_permanently"), policy.keepPermanently},
        {QStringLiteral("capture_history/retention_days"), policy.retentionDays},
        {QStringLiteral("capture_history/max_entries"), policy.maxEntries},
        {QStringLiteral("capture_history/max_disk_mib"), policy.maxDiskMiB},
    };
}
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CAPTUREHISTORYPOLICY_P_H
