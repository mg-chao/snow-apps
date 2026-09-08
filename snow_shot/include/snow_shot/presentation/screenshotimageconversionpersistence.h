#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONPERSISTENCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONPERSISTENCE_H

#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace snow_shot::presentation {
inline constexpr quint32 kImageConversionPayloadMarker = 0x53494356;
inline constexpr quint8 kImageConversionPayloadVersion = 1;
inline constexpr qsizetype kMaximumImageConversionPayload = 16 * 1024 * 1024;

[[nodiscard]] inline QByteArray
encodeImageConversions(const ScreenshotRecognitionResults& results) {
    QJsonArray entries;
    QSet<int> formats;
    for (const auto& entry : results.conversions) {
        const int format = static_cast<int>(entry.format);
        if (!entry.isValid() || formats.contains(format)) {
            continue;
        }
        formats.insert(format);
        entries.push_back(QJsonObject{{QStringLiteral("format"), format},
                                      {QStringLiteral("model"), entry.model},
                                      {QStringLiteral("source"), entry.source},
                                      {QStringLiteral("prompt_version"), entry.promptVersion},
                                      {QStringLiteral("image"), entry.imageFingerprint}});
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
    if (entries.size() > 2) {
        return;
    }
    QVector<ScreenshotImageConversionEntry> parsed;
    QSet<int> formats;
    for (const auto& value : entries) {
        const auto item = value.toObject();
        const int format = item.value(QStringLiteral("format")).toInt(-1);
        if (format < 0 || format > 1 || formats.contains(format)) {
            continue;
        }
        ScreenshotImageConversionEntry entry{static_cast<SnowShotImageConversionFormat>(format),
                                             item.value(QStringLiteral("model")).toString(),
                                             item.value(QStringLiteral("source")).toString(),
                                             item.value(QStringLiteral("prompt_version")).toInt(-1),
                                             item.value(QStringLiteral("image")).toString()};
        if (entry.isValid()) {
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
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSIONPERSISTENCE_H
