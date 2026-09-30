#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snowimageqtcodec.h"

namespace {
int compressionValue(const snow::image::EncoderOptionRange& range,
                     ScreenshotCompressionLevel level) {
    switch (level) {
    case ScreenshotCompressionLevel::Low:
        return range.minimum;
    case ScreenshotCompressionLevel::Medium:
        return range.default_value;
    case ScreenshotCompressionLevel::High:
        return range.maximum;
    }
    return range.minimum;
}
} // namespace

snow::image::Format ScreenshotImageFileService::snowImageFormat(ScreenshotImageFileFormat format) {
    switch (format) {
    case ScreenshotImageFileFormat::Pdf:
        return snow::image::Format::unknown;
    case ScreenshotImageFileFormat::Png:
        return snow::image::Format::png;
    case ScreenshotImageFileFormat::Jpeg:
        return snow::image::Format::jpeg;
    case ScreenshotImageFileFormat::Bmp:
        return snow::image::Format::bmp;
    case ScreenshotImageFileFormat::Webp:
        return snow::image::Format::webp;
    case ScreenshotImageFileFormat::Jxl:
        return snow::image::Format::jxl;
    case ScreenshotImageFileFormat::Avif:
        return snow::image::Format::avif;
    }
    return snow::image::Format::unknown;
}

bool ScreenshotImageFileService::supportsQuality(ScreenshotImageFileFormat format) {
    if (format == ScreenshotImageFileFormat::Pdf)
        return true;
    const auto encoder = snow_shot::image_codec::encoderInfo(snowImageFormat(format));
    return encoder.has_value() &&
           snow::image::has_feature(encoder->features, snow::image::EncoderFeature::quality);
}

bool ScreenshotImageFileService::supportsCompressionLevel(ScreenshotImageFileFormat format) {
    if (format == ScreenshotImageFileFormat::Pdf)
        return false;
    const auto encoder = snow_shot::image_codec::encoderInfo(snowImageFormat(format));
    if (!encoder.has_value())
        return false;
    return snow::image::has_feature(encoder->features,
                                    snow::image::EncoderFeature::compression_level) ||
           snow::image::has_feature(encoder->features, snow::image::EncoderFeature::effort) ||
           (snow::image::has_feature(encoder->features, snow::image::EncoderFeature::lossless) &&
            encoder->lossless_effort.maximum > encoder->lossless_effort.minimum);
}

QString ScreenshotImageFileService::compressionLevelKey(ScreenshotCompressionLevel level) {
    switch (level) {
    case ScreenshotCompressionLevel::Low:
        return QStringLiteral("low");
    case ScreenshotCompressionLevel::Medium:
        return QStringLiteral("medium");
    case ScreenshotCompressionLevel::High:
        return QStringLiteral("high");
    }
    return QStringLiteral("low");
}

ScreenshotCompressionLevel ScreenshotImageFileService::compressionLevelForKey(const QString& key) {
    const QString normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("medium"))
        return ScreenshotCompressionLevel::Medium;
    if (normalized == QStringLiteral("high"))
        return ScreenshotCompressionLevel::High;
    if (normalized == QStringLiteral("low"))
        return ScreenshotCompressionLevel::Low;
    return ScreenshotCompressionLevel::Medium;
}

snow::image::EncodeOptions
ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat format,
                                          ScreenshotImageEncodingOptions encoding) {
    snow::image::EncodeOptions options;
    options.format = snowImageFormat(format);
    options.preserve_metadata = false;
    options.quality = qBound(0, encoding.quality, 100);
    const auto encoder = snow_shot::image_codec::encoderInfo(options.format);
    switch (format) {
    case ScreenshotImageFileFormat::Png:
        if (encoder.has_value())
            options.compression_level =
                compressionValue(encoder->compression_level, encoding.compressionLevel);
        break;
    case ScreenshotImageFileFormat::Pdf:
    case ScreenshotImageFileFormat::Jpeg:
    case ScreenshotImageFileFormat::Bmp:
        break;
    case ScreenshotImageFileFormat::Webp:
        options.lossless = options.quality == 100;
        if (encoder.has_value()) {
            if (options.lossless) {
                options.lossless_effort =
                    compressionValue(encoder->lossless_effort, encoding.compressionLevel);
            } else {
                options.effort = compressionValue(encoder->effort, encoding.compressionLevel);
            }
        }
        break;
    case ScreenshotImageFileFormat::Jxl:
    case ScreenshotImageFileFormat::Avif:
        options.lossless = options.quality == 100;
        if (encoder.has_value())
            options.effort = compressionValue(encoder->effort, encoding.compressionLevel);
        break;
    }
    return options;
}

snow::image::EncodeOptions
ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat format, int quality) {
    return encodeOptions(format, ScreenshotImageEncodingOptions{quality});
}
