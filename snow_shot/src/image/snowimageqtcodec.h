#pragma once

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <snow/image/codec.h>
#include <snow/image/format.h>

#include <optional>

class QIODevice;

namespace snow_shot::image_codec {

struct EncodeResult final {
    quint64 bytesWritten = 0;
    snow::image::Format format = snow::image::Format::unknown;
    QSize size;
    quint32 emittedFrameCount = 0;
    snow::image::PixelRoundTrip roundTrip = snow::image::PixelRoundTrip::codec_artifact;
    bool finalizedAndFlushed = false;
};

[[nodiscard]] std::optional<snow::image::EncoderInfo> encoderInfo(snow::image::Format format);

[[nodiscard]] bool encodeToDevice(const ScreenshotImageRowSource& source, QIODevice* device,
                                  snow::image::Format format,
                                  const snow::image::EncodeOptions& options,
                                  QString* error = nullptr, EncodeResult* result = nullptr);
[[nodiscard]] bool encodeToDevice(const QImage& image, QIODevice* device,
                                  snow::image::Format format,
                                  const snow::image::EncodeOptions& options,
                                  QString* error = nullptr, EncodeResult* result = nullptr);

[[nodiscard]] bool resizeToRgba8(const ScreenshotImageRowSource& source, const QSize& outputSize,
                                 uchar* destination, qsizetype destinationStride,
                                 qsizetype destinationSize, QString* error = nullptr);

[[nodiscard]] QByteArray encodePng(const QImage& image, int compressionLevel = 0);
[[nodiscard]] QByteArray encodePng(const ScreenshotImageRowSource& source,
                                   int compressionLevel = 0);
[[nodiscard]] ScreenshotImageRowSource srgbRowSource(const QImage& image);
[[nodiscard]] QByteArray encodeWebp(const QImage& image, int quality = 75);
[[nodiscard]] QImage decode(const QByteArray& encoded, snow::image::Format expectedFormat,
                            const char* nameHint);
[[nodiscard]] QSize inspectSize(const QByteArray& encoded, snow::image::Format expectedFormat);
[[nodiscard]] QImage decodeFile(const QString& path, snow::image::Format expectedFormat);
[[nodiscard]] QImage decodeFileBgra(const QString& path, snow::image::Format expectedFormat);
[[nodiscard]] bool inspectFile(const QString& path, snow::image::Format expectedFormat,
                               const QSize& expectedSize);

} // namespace snow_shot::image_codec
