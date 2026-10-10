#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONPERSISTENCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONPERSISTENCE_H

#include "snow_shot/presentation/screenshotrecognitionresults.h"
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#endif

namespace snow_shot::presentation {
inline constexpr quint32 kImageConversionPayloadMarker = 0x53494356;
inline constexpr quint8 kImageConversionPayloadVersion = 1;
inline constexpr qsizetype kMaximumImageConversionPayload = kMaximumImageConversionCacheBytes;

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
[[nodiscard]] inline QByteArray
encodeImageConversions(const ScreenshotRecognitionResults& results) {
    QJsonArray entries;
    QSet<int> formats;
    QVector<ScreenshotImageConversionEntry> saved;
    // Reserve the envelope and retain the newest results if JSON exceeds the payload budget.
    qsizetype payloadBytes = 128;
    for (qsizetype index = results.conversions.size(); index > 0; --index) {
        const auto& entry = results.conversions.at(index - 1);
        const int format = static_cast<int>(entry.format);
        if (!entry.isValid() || saved.size() >= kMaximumImageConversionEntries ||
            std::any_of(saved.cbegin(), saved.cend(), [&entry](const auto& previous) {
                return sameImageConversionRequest(previous, entry);
            })) {
            continue;
        }
        const QJsonObject item{{QStringLiteral("format"), format},
                               {QStringLiteral("model"), entry.model},
                               {QStringLiteral("source"), entry.source},
                               {QStringLiteral("prompt_version"), entry.promptVersion},
                               {QStringLiteral("model_fingerprint"), entry.modelFingerprint}};
        const qsizetype itemBytes = QJsonDocument(item).toJson(QJsonDocument::Compact).size() + 1;
        if (itemBytes > kMaximumImageConversionPayload - payloadBytes)
            continue;
        payloadBytes += itemBytes;
        saved.prepend(entry);
        formats.insert(format);
        entries.prepend(item);
    }
    if (entries.isEmpty()) {
        return {};
    }
    QJsonObject root{{QStringLiteral("entries"), entries}};
    if (results.visibleConversion &&
        formats.contains(static_cast<int>(*results.visibleConversion))) {
        root.insert(QStringLiteral("visible"), static_cast<int>(*results.visibleConversion));
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return payload.size() <= kMaximumImageConversionPayload ? payload : QByteArray{};
}

inline void decodeImageConversions(const QByteArray& bytes, ScreenshotRecognitionResults& results) {
    if (bytes.size() > kMaximumImageConversionPayload) {
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) {
        return;
    }
    const auto root = document.object();
    if (!root.value(QStringLiteral("entries")).isArray()) {
        return;
    }
    const auto entries = root.value(QStringLiteral("entries")).toArray();
    if (entries.size() > kMaximumImageConversionEntries) {
        return;
    }
    QVector<ScreenshotImageConversionEntry> parsed;
    QSet<int> formats;
    for (const auto& value : entries) {
        const auto item = value.toObject();
        const int format = item.value(QStringLiteral("format")).toInt(-1);
        if (format < 0 || format > 1) {
            continue;
        }
        ScreenshotImageConversionEntry entry{
            static_cast<SnowShotImageConversionFormat>(format),
            item.value(QStringLiteral("model")).toString(),
            item.value(QStringLiteral("source")).toString(),
            item.value(QStringLiteral("prompt_version")).toInt(-1),
            item.value(QStringLiteral("model_fingerprint")).toString()};
        if (entry.isValid() &&
            std::none_of(parsed.cbegin(), parsed.cend(), [&entry](const auto& previous) {
                return sameImageConversionRequest(previous, entry);
            })) {
            parsed.push_back(std::move(entry));
            formats.insert(format);
        }
    }
    results.conversions = std::move(parsed);
    results.visibleConversion.reset();
    const int visible = root.value(QStringLiteral("visible")).toInt(-1);
    if (formats.contains(visible)) {
        results.visibleConversion = static_cast<SnowShotImageConversionFormat>(visible);
    }
}
#endif // SNOW_SHOT_ENABLE_IMAGE_CONVERSION
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONPERSISTENCE_H
