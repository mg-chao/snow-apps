#pragma once

#include "editing/edit_export_settings.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>

namespace snow::image_viewer::worker_protocol {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr qsizetype kMaximumPayloadBytes = 16 * 1024 * 1024;

enum class MessageType : std::uint16_t {
    ready = 1,
    encode_job = 2,
    preview_job = 3,
    artifact_ready = 4,
    preview_ready = 5,
    job_failed = 6,
    cancel = 7,
    cancelled = 8,
    shutdown = 9,
};

struct Frame final {
    MessageType type = MessageType::ready;
    QJsonObject payload;
};

[[nodiscard]] QByteArray encodeFrame(MessageType type, const QJsonObject& payload);
[[nodiscard]] bool takeFrame(QByteArray* buffer, Frame* frame, QString* error);
[[nodiscard]] QJsonObject settingsToJson(const EditExportSettings& settings);
[[nodiscard]] bool settingsFromJson(const QJsonValue& value, EditExportSettings* settings,
                                    QString* error);
[[nodiscard]] QJsonObject receiptToJson(const snow::image::EncodedArtifactReceipt& receipt);
[[nodiscard]] bool receiptFromJson(const QJsonValue& value,
                                   snow::image::EncodedArtifactReceipt* receipt, QString* error);

} // namespace snow::image_viewer::worker_protocol
