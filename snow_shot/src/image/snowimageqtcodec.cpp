#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snowimageqtcodec.h"
#include "snowimageqtsrgbrowreader.h"
#include "snowimageopacity.h"

#include "snowimagecodecbridge.h"

#include <snow/memory/pixel_buffer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <mutex>

#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QBuffer>
#include <QColorSpace>
#include <QIODevice>
#include <QImageReader>
#include <QPainter>
#include <QTransform>

namespace snow_shot::image_codec {
namespace {

constexpr std::size_t kBackendErrorCapacity = 1024;

void releaseBackendBuffer(void* rawBuffer) {
    auto* buffer = static_cast<SnowShotImageCodecBuffer*>(rawBuffer);
    if (buffer == nullptr) {
        return;
    }
    snow_shot_image_codec_release_buffer(buffer);
    delete buffer;
}

struct StreamingBridgeContext final {
    const ScreenshotImageRowSource* source = nullptr;
    QIODevice* device = nullptr;
    QString ioError;
};

int32_t SNOW_SHOT_IMAGE_CODEC_CALL readRowsCallback(void* rawContext, uint32_t firstRow,
                                                    uint32_t rowCount, uint64_t destinationStride,
                                                    uint8_t* destination,
                                                    uint64_t destinationSize) {
    auto* context = static_cast<StreamingBridgeContext*>(rawContext);
    if (context == nullptr || context->source == nullptr || destination == nullptr ||
        firstRow > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        rowCount > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        destinationStride > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()) ||
        destinationSize > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()) ||
        !context->source->readRows(static_cast<int>(firstRow), static_cast<int>(rowCount),
                                   static_cast<qsizetype>(destinationStride), destination,
                                   static_cast<qsizetype>(destinationSize))) {
        return 0;
    }
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL writeCallback(void* rawContext, const uint8_t* source,
                                                 uint64_t sourceSize) {
    auto* context = static_cast<StreamingBridgeContext*>(rawContext);
    if (context == nullptr || context->device == nullptr ||
        (source == nullptr && sourceSize != 0)) {
        return 0;
    }
    uint64_t written = 0;
    while (written < sourceSize) {
        const uint64_t remaining = sourceSize - written;
        const qsizetype chunk = static_cast<qsizetype>(std::min<uint64_t>(
            remaining, static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())));
        const qint64 result =
            context->device->write(reinterpret_cast<const char*>(source + written), chunk);
        if (result <= 0) {
            context->ioError = context->device->errorString();
            return 0;
        }
        written += static_cast<uint64_t>(result);
    }
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL positionCallback(void* rawContext, uint64_t* position) {
    auto* context = static_cast<StreamingBridgeContext*>(rawContext);
    if (context == nullptr || context->device == nullptr || position == nullptr) {
        return 0;
    }
    const qint64 value = context->device->pos();
    if (value < 0) {
        context->ioError = context->device->errorString();
        return 0;
    }
    *position = static_cast<uint64_t>(value);
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL seekCallback(void* rawContext, uint64_t position) {
    auto* context = static_cast<StreamingBridgeContext*>(rawContext);
    if (context == nullptr || context->device == nullptr ||
        position > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) ||
        !context->device->seek(static_cast<qint64>(position))) {
        if (context != nullptr && context->device != nullptr) {
            context->ioError = context->device->errorString();
        }
        return 0;
    }
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL flushCallback(void* rawContext) {
    auto* context = static_cast<StreamingBridgeContext*>(rawContext);
    if (context == nullptr || context->device == nullptr) {
        return 0;
    }
    if (auto* file = dynamic_cast<QFileDevice*>(context->device);
        file != nullptr && !file->flush()) {
        context->ioError = file->errorString();
        return 0;
    }
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL cancelledCallback(void* rawContext) {
    const auto* context = static_cast<const StreamingBridgeContext*>(rawContext);
    return context != nullptr && context->source != nullptr &&
                   context->source->cancellationRequested &&
                   context->source->cancellationRequested()
               ? 1
               : 0;
}

bool backendAbiIsCompatible() noexcept {
    return snow_shot_image_codec_abi_version() == SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
}

uint32_t bridgeFormat(snow::image::Format format) noexcept {
    switch (format) {
    case snow::image::Format::unknown:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN;
    case snow::image::Format::bmp:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_BMP;
    case snow::image::Format::cur:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_CUR;
    case snow::image::Format::gif:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_GIF;
    case snow::image::Format::ico:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_ICO;
    case snow::image::Format::jpeg:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG;
    case snow::image::Format::pbm:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_PBM;
    case snow::image::Format::pgm:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_PGM;
    case snow::image::Format::png:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG;
    case snow::image::Format::ppm:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_PPM;
    case snow::image::Format::svg:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_SVG;
    case snow::image::Format::svgz:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_SVGZ;
    case snow::image::Format::xbm:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_XBM;
    case snow::image::Format::xpm:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_XPM;
    case snow::image::Format::heif:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_HEIF;
    case snow::image::Format::avif:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_AVIF;
    case snow::image::Format::jxl:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL;
    case snow::image::Format::exr:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_EXR;
    case snow::image::Format::webp:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP;
    }
    return SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN;
}

snow::image::EncoderOptionRange
encoderOptionRange(const SnowShotImageCodecEncoderOptionRange& range) noexcept {
    return {range.minimum, range.maximum, range.default_value};
}

uint8_t bridgeChromaSubsampling(snow::image::ChromaSubsampling value) noexcept {
    switch (value) {
    case snow::image::ChromaSubsampling::none:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_NONE;
    case snow::image::ChromaSubsampling::yuv444:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV444;
    case snow::image::ChromaSubsampling::yuv422:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV422;
    case snow::image::ChromaSubsampling::yuv420:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV420;
    case snow::image::ChromaSubsampling::yuv440:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV440;
    case snow::image::ChromaSubsampling::yuv411:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV411;
    case snow::image::ChromaSubsampling::yuv441:
        return SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV441;
    }
    return SNOW_SHOT_IMAGE_CODEC_CHROMA_NONE;
}

uint8_t bridgeAlphaContent(snow::image::AlphaContent value) noexcept {
    switch (value) {
    case snow::image::AlphaContent::opaque:
        return SNOW_SHOT_IMAGE_CODEC_ALPHA_OPAQUE;
    case snow::image::AlphaContent::non_opaque:
        return SNOW_SHOT_IMAGE_CODEC_ALPHA_NON_OPAQUE;
    }
    return SNOW_SHOT_IMAGE_CODEC_ALPHA_OPAQUE;
}

SnowShotImageCodecEncodeOptions bridgeOptions(snow::image::Format format,
                                              const snow::image::EncodeOptions& options) noexcept {
    SnowShotImageCodecEncodeOptions result{};
    result.struct_size = static_cast<uint32_t>(sizeof(SnowShotImageCodecEncodeOptions));
    result.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    result.format = bridgeFormat(format);
    result.quality = options.quality;
    result.effort = options.effort;
    result.lossless_effort = options.lossless_effort;
    result.compression_level = options.compression_level;
    result.lossless = options.lossless ? uint8_t{1} : uint8_t{0};
    result.preserve_metadata = options.preserve_metadata ? uint8_t{1} : uint8_t{0};
    result.progressive = options.progressive ? uint8_t{1} : uint8_t{0};
    result.interlaced = options.interlaced ? uint8_t{1} : uint8_t{0};
    if (options.chroma_subsampling.has_value()) {
        result.has_chroma_subsampling = 1;
        result.chroma_subsampling = bridgeChromaSubsampling(*options.chroma_subsampling);
    }
    if (options.verified_alpha_content.has_value()) {
        result.has_verified_alpha_content = 1;
        result.verified_alpha_content = bridgeAlphaContent(*options.verified_alpha_content);
    }
    return result;
}

void setError(QString* output, const char* backendError, const char* fallback) {
    if (output == nullptr) {
        return;
    }
    *output = backendError != nullptr && backendError[0] != '\0' ? QString::fromUtf8(backendError)
                                                                 : QString::fromLatin1(fallback);
}

QByteArray encodeImage(const QImage& image, snow::image::Format format,
                       const snow::image::EncodeOptions& options, QString* error) {
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) ||
        !encodeToDevice(image, &buffer, format, options, error)) {
        return {};
    }
    return encoded;
}

QColorSpace decodedColorSpace(const SnowShotImageCodecColorEncoding& color) {
    if (color.icc_profile != nullptr && color.icc_profile_size != 0 &&
        color.icc_profile_size <= static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
        // QColorSpace must own the ICC bytes independently of the native pixel buffer.
        const QByteArray profile(reinterpret_cast<const char*>(color.icc_profile),
                                 static_cast<qsizetype>(color.icc_profile_size));
        const QColorSpace space = QColorSpace::fromIccProfile(profile);
        if (space.isValid())
            return space;
    }

    QColorSpace::Primaries primaries;
    switch (color.primaries) {
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_SRGB:
        primaries = QColorSpace::Primaries::SRgb;
        break;
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_DISPLAY_P3:
        primaries = QColorSpace::Primaries::DciP3D65;
        break;
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_ADOBE_RGB:
        primaries = QColorSpace::Primaries::AdobeRgb;
        break;
    case SNOW_SHOT_IMAGE_CODEC_PRIMARIES_REC2020:
        primaries = QColorSpace::Primaries::Bt2020;
        break;
    default:
        return {};
    }
    QColorSpace::TransferFunction transfer;
    switch (color.transfer) {
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_LINEAR:
        transfer = QColorSpace::TransferFunction::Linear;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_SRGB:
        transfer = QColorSpace::TransferFunction::SRgb;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_GAMMA:
        transfer = QColorSpace::TransferFunction::Gamma;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_PQ:
        transfer = QColorSpace::TransferFunction::St2084;
        break;
    case SNOW_SHOT_IMAGE_CODEC_TRANSFER_HLG:
        transfer = QColorSpace::TransferFunction::Hlg;
        break;
    default:
        return {};
    }
    return QColorSpace(primaries, transfer);
}

using OwnedBackendBuffer =
    std::unique_ptr<SnowShotImageCodecBuffer, decltype(&releaseBackendBuffer)>;

QImage takeDecodedImage(OwnedBackendBuffer output, QImage::Format format) {
    if (!output || output->data == nullptr || output->width == 0 || output->height == 0 ||
        output->width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4) ||
        output->height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const uint64_t rowBytes = static_cast<uint64_t>(output->width) * 4U;
    if (output->row_stride != rowBytes ||
        output->height > std::numeric_limits<uint64_t>::max() / rowBytes) {
        return {};
    }
    const uint64_t requiredSize = rowBytes * output->height;
    if (requiredSize > output->size ||
        requiredSize > static_cast<uint64_t>(std::numeric_limits<qsizetype>::max())) {
        return {};
    }

    const QColorSpace space = decodedColorSpace(output->color);
    QImage result(output->data, static_cast<int>(output->width), static_cast<int>(output->height),
                  static_cast<qsizetype>(rowBytes), format, &releaseBackendBuffer, output.get());
    if (result.isNull())
        return {};
    // Both packed formats share this owner. Attach metadata while the QImage is unique
    // so neither wrapping the pixels nor setting the profile introduces an image copy.
    static_cast<void>(output.release());
    result.setColorSpace(space);
    return result;
}

QImage decodeBytes(const QByteArray& encoded, snow::image::Format expectedFormat,
                   uint32_t preferredIconExtent = 0,
                   QImage::Format pixelFormat = QImage::Format_RGBA8888,
                   bool firstFrameOnly = false) {
    const uint32_t bridgeExpectedFormat = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() ||
        (bridgeExpectedFormat == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN &&
         expectedFormat != snow::image::Format::unknown)) {
        return {};
    }

    OwnedBackendBuffer output(new (std::nothrow) SnowShotImageCodecBuffer{}, &releaseBackendBuffer);
    if (!output)
        return {};
    std::array<char, kBackendErrorCapacity> backendError{};
    const auto decoder = preferredIconExtent != 0 ? snow_shot_image_codec_decode_icon_rgba8
                         : firstFrameOnly         ? snow_shot_image_codec_decode_first_rgba8
                                                  : (pixelFormat == QImage::Format_ARGB32
                                                         ? snow_shot_image_codec_decode_bgra8
                                                         : snow_shot_image_codec_decode_rgba8);
    const int32_t succeeded =
        decoder(reinterpret_cast<const uint8_t*>(encoded.constData()),
                static_cast<uint64_t>(encoded.size()),
                preferredIconExtent != 0 ? preferredIconExtent : bridgeExpectedFormat, output.get(),
                backendError.data(), static_cast<uint64_t>(backendError.size()));
    if (succeeded == 0) {
        return {};
    }
    return takeDecodedImage(std::move(output), pixelFormat);
}

QImage decodeBgraBytes(const QByteArray& encoded, snow::image::Format expectedFormat) {
    return decodeBytes(encoded, expectedFormat, 0, QImage::Format_ARGB32);
}

bool inspectBytes(const QByteArray& encoded, snow::image::Format expectedFormat,
                  const QSize& expectedSize) {
    const uint32_t bridgeExpectedFormat = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() || !expectedSize.isValid() ||
        (bridgeExpectedFormat == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN &&
         expectedFormat != snow::image::Format::unknown)) {
        return false;
    }

    SnowShotImageCodecImageInfo information{};
    std::array<char, kBackendErrorCapacity> backendError{};
    const int32_t succeeded = snow_shot_image_codec_inspect(
        reinterpret_cast<const uint8_t*>(encoded.constData()),
        static_cast<uint64_t>(encoded.size()), bridgeExpectedFormat, &information,
        backendError.data(), static_cast<uint64_t>(backendError.size()));
    return succeeded != 0 && information.width == static_cast<uint32_t>(expectedSize.width()) &&
           information.height == static_cast<uint32_t>(expectedSize.height());
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

} // namespace

std::optional<snow::image::EncoderInfo> encoderInfo(snow::image::Format format) {
    const uint32_t requestedFormat = bridgeFormat(format);
    if (!backendAbiIsCompatible() || requestedFormat == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN)
        return std::nullopt;
    SnowShotImageCodecEncoderInfo bridge{};
    bridge.struct_size = sizeof(bridge);
    bridge.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    if (snow_shot_image_codec_encoder_info(requestedFormat, &bridge) == 0 ||
        bridge.format != requestedFormat) {
        return std::nullopt;
    }
    snow::image::EncoderInfo result;
    result.format = format;
    result.features = static_cast<snow::image::EncoderFeature>(bridge.features);
    result.quality = encoderOptionRange(bridge.quality);
    result.effort = encoderOptionRange(bridge.effort);
    result.lossless_effort = encoderOptionRange(bridge.lossless_effort);
    result.compression_level = encoderOptionRange(bridge.compression_level);
    return result;
}

std::optional<qint64> encodeWorkingBytes(QSize size, snow::image::Format format,
                                         const snow::image::EncodeOptions& options) {
    if (size.isEmpty() || !backendAbiIsCompatible())
        return std::nullopt;
    const auto encodedOptions = bridgeOptions(format, options);
    uint64_t working = 0;
    constexpr uint64_t rowCache = 2 * 1024 * 1024;
    if (!snow_shot_image_codec_encode_resources(static_cast<uint32_t>(size.width()),
                                                static_cast<uint32_t>(size.height()),
                                                &encodedOptions, &working) ||
        working > static_cast<uint64_t>(std::numeric_limits<qint64>::max()) - rowCache)
        return std::nullopt;
    return static_cast<qint64>(working + rowCache);
}

bool encodeToDevice(const ScreenshotImageRowSource& source, QIODevice* device,
                    snow::image::Format format, const snow::image::EncodeOptions& options,
                    QString* error, EncodeResult* result) {
    if (error != nullptr) {
        error->clear();
    }
    if (result != nullptr)
        *result = {};
    if (!backendAbiIsCompatible()) {
        setError(error, nullptr, "The image codec backend is incompatible.");
        return false;
    }
    if (!source.isValid() || device == nullptr || !device->isOpen() || !device->isWritable() ||
        bridgeFormat(format) == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN) {
        setError(error, nullptr, "The image source, format, or output device is invalid.");
        return false;
    }

    StreamingBridgeContext context{&source, device, {}};
    SnowShotImageCodecRgba8Source bridgeSource{};
    bridgeSource.struct_size = sizeof(bridgeSource);
    bridgeSource.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    bridgeSource.context = &context;
    bridgeSource.width = static_cast<uint32_t>(source.size.width());
    bridgeSource.height = static_cast<uint32_t>(source.size.height());
    bridgeSource.read_rows = &readRowsCallback;
    bridgeSource.is_cancelled = &cancelledCallback;

    SnowShotImageCodecByteSink bridgeSink{};
    bridgeSink.struct_size = sizeof(bridgeSink);
    bridgeSink.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    bridgeSink.context = &context;
    bridgeSink.write = &writeCallback;
    bridgeSink.position = &positionCallback;
    bridgeSink.seek = &seekCallback;
    bridgeSink.flush = &flushCallback;
    bridgeSink.is_cancelled = &cancelledCallback;
    bridgeSink.seekable = device->isSequential() ? 0 : 1;

    auto verifiedOptions = options;
    if (!verifiedOptions.verified_alpha_content) {
        switch (source.verifiedAlphaContent()) {
        case ScreenshotImageAlphaContent::Opaque:
            verifiedOptions.verified_alpha_content = snow::image::AlphaContent::opaque;
            break;
        case ScreenshotImageAlphaContent::NonOpaque:
            verifiedOptions.verified_alpha_content = snow::image::AlphaContent::non_opaque;
            break;
        case ScreenshotImageAlphaContent::Unknown:
            break;
        }
    }
    const SnowShotImageCodecEncodeOptions encodedOptions = bridgeOptions(format, verifiedOptions);
    std::array<char, kBackendErrorCapacity> backendError{};
    SnowShotImageCodecEncodeResult bridgeResult{};
    bridgeResult.struct_size = sizeof(bridgeResult);
    bridgeResult.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    const int32_t succeeded = snow_shot_image_codec_encode_rgba8_stream(
        &bridgeSource, &bridgeSink, &encodedOptions, &bridgeResult, backendError.data(),
        static_cast<uint64_t>(backendError.size()));
    if (succeeded == 0 || bridgeResult.bytes_written == 0 ||
        bridgeResult.encoded_format != bridgeFormat(format) ||
        bridgeResult.canvas_width != static_cast<uint32_t>(source.size.width()) ||
        bridgeResult.canvas_height != static_cast<uint32_t>(source.size.height()) ||
        bridgeResult.emitted_frame_count == 0 ||
        bridgeResult.pixel_round_trip > SNOW_SHOT_IMAGE_CODEC_PIXEL_ROUND_TRIP_CODEC_ARTIFACT ||
        bridgeResult.encoder_finalized_and_sink_flushed == 0) {
        if (!context.ioError.isEmpty() && error != nullptr) {
            *error = context.ioError;
        } else {
            setError(error, backendError.data(), "Image encoding failed.");
        }
        return false;
    }
    if (result != nullptr) {
        result->bytesWritten = bridgeResult.bytes_written;
        result->format = format;
        result->size = source.size;
        result->emittedFrameCount = bridgeResult.emitted_frame_count;
        result->roundTrip =
            bridgeResult.pixel_round_trip == SNOW_SHOT_IMAGE_CODEC_PIXEL_ROUND_TRIP_EXACT
                ? snow::image::PixelRoundTrip::exact
                : snow::image::PixelRoundTrip::codec_artifact;
        result->finalizedAndFlushed = true;
    }
    return true;
}

bool encodeToDevice(const QImage& image, QIODevice* device, snow::image::Format format,
                    const snow::image::EncodeOptions& options, QString* error,
                    EncodeResult* result) {
    const ScreenshotImageRowSource source = srgbRowSource(image);
    if (!source.isValid()) {
        setError(error, nullptr, "The image could not be converted to RGBA pixels.");
        return false;
    }
    return encodeToDevice(source, device, format, options, error, result);
}

bool resizeToRgba8(const ScreenshotImageRowSource& source, const QSize& outputSize,
                   uchar* destination, qsizetype destinationStride, qsizetype destinationSize,
                   QString* error) {
    if (error != nullptr)
        error->clear();
    const qsizetype outputRowBytes = static_cast<qsizetype>(outputSize.width()) * 4;
    if (!backendAbiIsCompatible() || !source.isValid() || outputSize.isEmpty() ||
        outputSize.width() > std::numeric_limits<int>::max() / 4 || destination == nullptr ||
        destinationStride < outputRowBytes || destinationSize < outputRowBytes ||
        outputSize.height() - 1 > (destinationSize - outputRowBytes) / destinationStride) {
        setError(error, nullptr, "The resize source or destination is invalid.");
        return false;
    }

    StreamingBridgeContext context{&source, nullptr, {}};
    SnowShotImageCodecRgba8Source bridgeSource{};
    bridgeSource.struct_size = sizeof(bridgeSource);
    bridgeSource.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    bridgeSource.context = &context;
    bridgeSource.width = static_cast<uint32_t>(source.size.width());
    bridgeSource.height = static_cast<uint32_t>(source.size.height());
    bridgeSource.read_rows = &readRowsCallback;
    bridgeSource.is_cancelled = &cancelledCallback;

    std::array<char, kBackendErrorCapacity> backendError{};
    const int32_t succeeded = snow_shot_image_codec_resize_rgba8(
        &bridgeSource, destination, static_cast<uint64_t>(destinationStride),
        static_cast<uint64_t>(destinationSize), static_cast<uint32_t>(outputSize.width()),
        static_cast<uint32_t>(outputSize.height()), backendError.data(),
        static_cast<uint64_t>(backendError.size()));
    if (succeeded == 0) {
        setError(error, backendError.data(), "Image resizing failed.");
        return false;
    }
    return true;
}

namespace {
bool outsideImage(const QImage& image, const uchar* destination, qsizetype bytes) {
    if (image.isNull())
        return true;
    const auto source = reinterpret_cast<std::uintptr_t>(image.constBits());
    const auto output = reinterpret_cast<std::uintptr_t>(destination);
    // Borrowed crops retain the original row stride. Padding after the final pixel row can
    // extend beyond that source storage and must not falsely overlap an adjacent destination.
    const auto sourceBytes =
        static_cast<std::uintptr_t>(image.sizeInBytes() - image.bytesPerLine() +
                                    (qsizetype(image.width()) * image.depth() + 7) / 8);
    const auto outputBytes = static_cast<std::uintptr_t>(bytes);
    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    return sourceBytes <= maximum - source && outputBytes <= maximum - output &&
           !(source < output + outputBytes && output < source + sourceBytes);
}

bool validRgbaRegion(QSize size, QRect region, qsizetype stride, const uchar* destination,
                     qsizetype capacity, const QImage& owner = {}) {
    if (region.x() < 0 || region.y() < 0 || region.width() <= 0 || region.height() <= 0 ||
        region.x() >= size.width() || region.y() >= size.height() ||
        region.width() > size.width() - region.x() ||
        region.height() > size.height() - region.y() || destination == nullptr)
        return false;
    const qsizetype rowBytes = qsizetype(region.width()) * 4;
    const auto maximum = std::numeric_limits<qsizetype>::max();
    if (stride < rowBytes || region.height() - 1 > (maximum - rowBytes) / stride)
        return false;
    const qsizetype extent = qsizetype(region.height() - 1) * stride + rowBytes;
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    return capacity >= extent &&
           static_cast<std::uintptr_t>(extent) <=
               std::numeric_limits<std::uintptr_t>::max() - address &&
           outsideImage(owner, destination, extent);
}

int resizeCacheRows(QSize output) {
    constexpr qsizetype cacheBytes = 2 * 1024 * 1024;
    const qsizetype rowBytes = qsizetype(output.width()) * 4;
    return std::min(output.height(), static_cast<int>(std::min<qsizetype>(
                                         64, std::max<qsizetype>(1, cacheBytes / rowBytes))));
}

uint64_t resizeCacheBytes(QSize output) {
    const auto rows = resizeCacheRows(output);
    return rows <= 1
               ? uint64_t{0}
               : uint64_t{static_cast<uint32_t>(output.width())} * static_cast<uint32_t>(rows) * 4U;
}

class ResizeRowReader final {
  public:
    ResizeRowReader(ScreenshotImageRowSource source, QSize output)
        : m_source(std::move(source)), m_context{&m_source, nullptr, {}}, m_output(output),
          m_cacheRows(resizeCacheRows(output)),
          m_plan(nullptr, &snow_shot_image_codec_destroy_resize) {}

    bool prepare(QString* error) {
        SnowShotImageCodecRgba8Source source{};
        source.struct_size = sizeof(source);
        source.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
        source.context = &m_context;
        source.width = static_cast<uint32_t>(m_source.size.width());
        source.height = static_cast<uint32_t>(m_source.size.height());
        source.read_rows = &readRowsCallback;
        source.is_cancelled = m_source.cancellationRequested ? &cancelledCallback : nullptr;
        std::array<char, kBackendErrorCapacity> detail{};
        m_plan.reset(snow_shot_image_codec_prepare_resize(
            &source, static_cast<uint32_t>(m_output.width()),
            static_cast<uint32_t>(m_output.height()), detail.data(), detail.size()));
        if (!m_plan)
            setError(error, detail.data(), "Image resizing failed.");
        return static_cast<bool>(m_plan);
    }

    bool operator()(int first, int count, qsizetype stride, uchar* destination,
                    qsizetype capacity) {
        const std::lock_guard lock(m_mutex);
        const qsizetype rowBytes = qsizetype(m_output.width()) * 4;
        if (first < 0 || count <= 0 || first >= m_output.height() ||
            count > m_output.height() - first || destination == nullptr || stride < rowBytes ||
            capacity < rowBytes || count - 1 > (capacity - rowBytes) / stride ||
            (m_source.cancellationRequested && m_source.cancellationRequested()))
            return false;
        const qsizetype extent = qsizetype(count - 1) * stride + rowBytes;
        const auto address = reinterpret_cast<std::uintptr_t>(destination);
        if (static_cast<std::uintptr_t>(extent) >
                std::numeric_limits<std::uintptr_t>::max() - address ||
            !outsideImage(m_source.backingImage, destination, extent) ||
            !outsideImage(m_cache, destination, extent))
            return false;
        if (m_cacheRows <= 1 || count >= m_cacheRows)
            return read(first, count, stride, destination, capacity);
        int copied = 0;
        while (copied < count) {
            if (m_source.cancellationRequested && m_source.cancellationRequested())
                return false;
            const int sourceRow = first + copied;
            if (sourceRow < m_cachedFirst || sourceRow >= m_cachedFirst + m_cachedCount) {
                if (m_cache.isNull())
                    m_cache = snowCanvasAllocateImage(QSize(m_output.width(), m_cacheRows),
                                                      QImage::Format_RGBA8888);
                if (m_cache.isNull())
                    return false;
                m_cachedCount = 0;
                m_cachedFirst = sourceRow / m_cacheRows * m_cacheRows;
                const int rows = std::min(m_cacheRows, m_output.height() - m_cachedFirst);
                if (!read(m_cachedFirst, rows, m_cache.bytesPerLine(), m_cache.bits(),
                          m_cache.sizeInBytes()))
                    return false;
                m_cachedCount = rows;
            }
            const int local = sourceRow - m_cachedFirst;
            const int rows = std::min(count - copied, m_cachedCount - local);
            for (int row = 0; row < rows; ++row)
                std::memcpy(destination + qsizetype(copied + row) * stride,
                            m_cache.constScanLine(local + row), static_cast<size_t>(rowBytes));
            copied += rows;
        }
        return true;
    }

    bool region(QRect region, qsizetype stride, uchar* destination, qsizetype capacity) {
        if (!validRgbaRegion(m_output, region, stride, destination, capacity,
                             m_source.backingImage))
            return false;
        QImage row = snowCanvasAllocateImage(QSize(m_output.width(), 1), QImage::Format_RGBA8888);
        if (row.isNull())
            return false;
        const qsizetype bytes = qsizetype(region.width()) * 4;
        for (int y = 0; y < region.height(); ++y) {
            if (!(*this)(region.y() + y, 1, row.bytesPerLine(), row.bits(), row.sizeInBytes()))
                return false;
            std::memcpy(destination + qsizetype(y) * stride,
                        row.constBits() + qsizetype(region.x()) * 4, static_cast<size_t>(bytes));
        }
        return true;
    }

  private:
    bool read(int first, int count, qsizetype stride, uchar* destination, qsizetype capacity) {
        std::array<char, kBackendErrorCapacity> detail{};
        return snow_shot_image_codec_resize_rows(
                   m_plan.get(), static_cast<uint32_t>(first), static_cast<uint32_t>(count),
                   destination, static_cast<uint64_t>(stride), static_cast<uint64_t>(capacity),
                   detail.data(), detail.size()) != 0;
    }

    ScreenshotImageRowSource m_source;
    StreamingBridgeContext m_context;
    QSize m_output;
    int m_cacheRows;
    std::mutex m_mutex;
    QImage m_cache;
    int m_cachedFirst = -1;
    int m_cachedCount = 0;
    std::unique_ptr<SnowShotImageCodecResizePlan, decltype(&snow_shot_image_codec_destroy_resize)>
        m_plan;
};
} // namespace

std::optional<ResizeResourceEstimate> resizeResources(QSize source, QSize output) {
    if (!backendAbiIsCompatible() || source.isEmpty() || output.isEmpty())
        return std::nullopt;
    if (source == output)
        return ResizeResourceEstimate{};
    uint64_t retained = 0;
    uint64_t working = 0;
    if (!snow_shot_image_codec_resize_resources(
            static_cast<uint32_t>(source.width()), static_cast<uint32_t>(source.height()),
            static_cast<uint32_t>(output.width()), static_cast<uint32_t>(output.height()),
            &retained, &working))
        return std::nullopt;
    const auto cache = resizeCacheBytes(output);
    const auto maximum = static_cast<uint64_t>(std::numeric_limits<qint64>::max());
    if (retained > maximum - cache || working > maximum)
        return std::nullopt;
    return ResizeResourceEstimate{static_cast<qint64>(retained + cache),
                                  static_cast<qint64>(working)};
}

std::optional<qint64> resizeWorkingBytes(QSize source, QSize output) {
    const auto estimate = resizeResources(source, output);
    if (!estimate ||
        estimate->retainedBytes > std::numeric_limits<qint64>::max() - estimate->scratchBytes)
        return std::nullopt;
    return estimate->retainedBytes + estimate->scratchBytes -
           (source == output ? 0 : static_cast<qint64>(resizeCacheBytes(output)));
}

ScreenshotImageRowSource resizedRowSource(ScreenshotImageRowSource source, QSize output,
                                          QString* error) {
    if (error)
        error->clear();
    if (!source.isValid() || !resizeResources(source.size, output)) {
        setError(error, nullptr, "The resize source or destination is invalid.");
        return {};
    }
    if (source.size == output)
        return source;
    ScreenshotImageRowSource result;
    result.size = output;
    result.cancellationRequested = source.cancellationRequested;
    result.reserveResources = source.reserveResources;
    result.isKnownOpaque = source.isKnownOpaque;
    if (source.classifyAlpha) {
        // Resampling can discard transparent samples or round their alpha to 255. Only a
        // proof of opacity survives without examining the resized pixels themselves.
        result.classifyAlpha = [classify = source.classifyAlpha] {
            return classify() == ScreenshotImageAlphaContent::Opaque
                       ? ScreenshotImageAlphaContent::Opaque
                       : ScreenshotImageAlphaContent::Unknown;
        };
    }
    result.previewImage = source.previewImage;
    auto reader = std::make_shared<ResizeRowReader>(std::move(source), output);
    if (!reader->prepare(error))
        return {};
    result.readRows = [reader](int first, int count, qsizetype stride, uchar* destination,
                               qsizetype capacity) {
        return (*reader)(first, count, stride, destination, capacity);
    };
    result.readRegion = [reader = std::move(reader)](QRect region, qsizetype stride,
                                                     uchar* destination, qsizetype capacity) {
        return reader->region(region, stride, destination, capacity);
    };
    return result;
}

QByteArray encodePng(const QImage& image, int compressionLevel) {
    snow::image::EncodeOptions options;
    options.compression_level = qBound(0, compressionLevel, 9);
    return encodeImage(image, snow::image::Format::png, options, nullptr);
}

ScreenshotImageRowSource srgbRowSource(const QImage& image) {
    if (image.isNull())
        return {};
    const QColorSpace srgb(QColorSpace::SRgb);
    const bool convertColor = image.colorSpace().isValid() && image.colorSpace() != srgb;
    ScreenshotImageRowSource source;
    source.size = image.size();
    source.isKnownOpaque = !image.hasAlphaChannel();
    if (!source.isKnownOpaque) {
        struct Opacity final {
            std::once_flag once;
            ScreenshotImageAlphaContent content = ScreenshotImageAlphaContent::Unknown;
        };
        auto opacity = std::make_shared<Opacity>();
        source.classifyAlpha = [image, opacity] {
            std::call_once(opacity->once,
                           [&] { opacity->content = detail::imageAlphaContent(image); });
            return opacity->content;
        };
    }
    // Untagged images retain the existing assumption that their pixels are
    // sRGB. Retagging a shared image here would detach its complete raster.
    if (!convertColor && image.format() == QImage::Format_RGBA8888) {
        source.backingImage = image;
        source.readRows = [image](int first, int count, qsizetype stride, uchar* destination,
                                  qsizetype capacity) {
            return snowCanvasCopyRgba8888Rows(image, first, count, destination, capacity, stride);
        };
        source.readRegion = [image](QRect region, qsizetype stride, uchar* destination,
                                    qsizetype capacity) {
            if (!validRgbaRegion(image.size(), region, stride, destination, capacity, image))
                return false;
            for (int row = 0; row < region.height(); ++row)
                std::memcpy(destination + qsizetype(row) * stride,
                            image.constScanLine(region.y() + row) + qsizetype(region.x()) * 4,
                            static_cast<size_t>(region.width()) * 4U);
            return true;
        };
    } else {
        auto reader = std::make_shared<detail::BoundedSrgbRowReader>(image);
        source.readRows = [reader](int first, int count, qsizetype stride, uchar* destination,
                                   qsizetype capacity) {
            return (*reader)(first, count, stride, destination, capacity);
        };
        source.readRegion = [reader = std::move(reader), image](QRect region, qsizetype stride,
                                                                uchar* destination,
                                                                qsizetype capacity) {
            if (!validRgbaRegion(image.size(), region, stride, destination, capacity, image))
                return false;
            QImage row = snowCanvasAllocateImage(QSize(image.width(), 1), QImage::Format_RGBA8888);
            if (row.isNull())
                return false;
            for (int y = 0; y < region.height(); ++y) {
                if (!(*reader)(region.y() + y, 1, row.bytesPerLine(), row.bits(),
                               row.sizeInBytes()))
                    return false;
                std::memcpy(destination + qsizetype(y) * stride,
                            row.constBits() + qsizetype(region.x()) * 4,
                            static_cast<size_t>(region.width()) * 4U);
            }
            return true;
        };
    }
    return source;
}

QByteArray encodePng(const ScreenshotImageRowSource& source, int compressionLevel) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    snow::image::EncodeOptions options;
    options.compression_level = qBound(0, compressionLevel, 9);
    if (!buffer.open(QIODevice::WriteOnly) ||
        !encodeToDevice(source, &buffer, snow::image::Format::png, options)) {
        return {};
    }
    return bytes;
}

QByteArray encodeWebp(const QImage& image, int quality) {
    snow::image::EncodeOptions options;
    options.quality = quality;
    return encodeImage(image, snow::image::Format::webp, options, nullptr);
}

QImage decode(const QByteArray& encoded, snow::image::Format expectedFormat,
              const char* /*nameHint*/) {
    return decodeBytes(encoded, expectedFormat);
}

QImage decodeFirstFrame(const QByteArray& encoded, snow::image::Format expectedFormat) {
    return decodeBytes(encoded, expectedFormat, 0, QImage::Format_RGBA8888, true);
}

QImage decodeBgra(const QByteArray& encoded, snow::image::Format expectedFormat) {
    return decodeBgraBytes(encoded, expectedFormat);
}

QImage readManagedImage(QImageReader& reader) {
    const auto fallback = [&reader] {
        QImage image = reader.read();
        return image.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
                   ? image
                   : snowCanvasCopyImage(image);
    };
    const QSize size = reader.size();
    const QImage::Format format = reader.imageFormat();
    if (size.isEmpty() || format <= QImage::Format_Invalid || format >= QImage::NImageFormats)
        return fallback();

    // Qt checks this limit only when its handler allocates a new image. Reusing
    // our allocation must enforce the same effective depth and aligned stride.
    const quint64 depth =
        std::max(quint64{32}, quint64{QImage::toPixelFormat(format).bitsPerPixel()});
    const quint64 stride = ((static_cast<quint64>(size.width()) * depth + 31) / 32) * 4;
    if (stride == 0 || static_cast<quint64>(size.height()) >
                           static_cast<quint64>(std::numeric_limits<qsizetype>::max()) / stride)
        return fallback();
    const quint64 bytes = stride * static_cast<quint64>(size.height());
    const int limit = QImageReader::allocationLimit();
    if (limit > 0 && bytes > static_cast<quint64>(limit) * 1024 * 1024)
        return fallback();

    QImage result = snowCanvasAllocateImage(size, format);
    if (result.isNull())
        return fallback();
    const uchar* pixels = result.constBits();
    if (!reader.read(&result))
        return {};
    // Some plugins cannot reuse storage, and automatic orientation or reader
    // scaling can replace it after decoding. Preserve those results exactly.
    return result.constBits() == pixels ||
                   result.sizeInBytes() < qsizetype(snow::memory::kMappedPixelBufferMinimum)
               ? result
               : snowCanvasCopyImage(result);
}

QImage decodeIconFile(const QString& path, uint32_t preferredExtent) {
    if (preferredExtent == 0)
        return {};
    return decodeBytes(readFile(path), snow::image::Format::ico, preferredExtent);
}

QSize inspectSize(const QByteArray& encoded, snow::image::Format expectedFormat) {
    const auto format = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() ||
        (format == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN &&
         expectedFormat != snow::image::Format::unknown))
        return {};
    SnowShotImageCodecImageInfo information{};
    std::array<char, kBackendErrorCapacity> backendError{};
    if (snow_shot_image_codec_inspect(reinterpret_cast<const uint8_t*>(encoded.constData()),
                                      static_cast<uint64_t>(encoded.size()), format, &information,
                                      backendError.data(),
                                      static_cast<uint64_t>(backendError.size())) == 0 ||
        information.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        information.height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        return {};
    return QSize(static_cast<int>(information.width), static_cast<int>(information.height));
}

QSize inspectCanvasSize(const QByteArray& encoded, snow::image::Format expectedFormat) {
    const auto format = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() ||
        (format == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN &&
         expectedFormat != snow::image::Format::unknown))
        return {};
    SnowShotImageCodecImageInfo information{};
    std::array<char, kBackendErrorCapacity> error{};
    if (!snow_shot_image_codec_inspect_canvas(reinterpret_cast<const uint8_t*>(encoded.constData()),
                                              static_cast<uint64_t>(encoded.size()), format,
                                              &information, error.data(), error.size()) ||
        information.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        information.height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
        return {};
    return QSize(static_cast<int>(information.width), static_cast<int>(information.height));
}

QImage decodeFile(const QString& path, snow::image::Format expectedFormat) {
    return decodeBytes(readFile(path), expectedFormat);
}

QImage decodeFileBgra(const QString& path, snow::image::Format expectedFormat) {
    return decodeBgraBytes(readFile(path), expectedFormat);
}

SkinDecodeResult decodeSkinFile(const QString& path) {
    constexpr qint64 inputLimit = 64LL << 20U;
    constexpr int previewExtent = 4096;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg") &&
        suffix != QStringLiteral("jpeg") && suffix != QStringLiteral("webp"))
        return {{}, SkinDecodeError::UnsupportedFormat};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
        return {{}, SkinDecodeError::UnreadableFile};
    if (file.size() > inputLimit)
        return {{}, SkinDecodeError::InputTooLarge};
    try {
        // One extra byte detects a file growing after size() without allowing
        // readAll() to allocate an unbounded buffer.
        QByteArray encoded = file.read(inputLimit + 1);
        if (encoded.size() > inputLimit)
            return {{}, SkinDecodeError::InputTooLarge};
        if (file.error() != QFileDevice::NoError)
            return {{}, SkinDecodeError::UnreadableFile};
        if (encoded.isEmpty())
            return {{}, SkinDecodeError::InvalidImage};
        if (!backendAbiIsCompatible())
            return {{}, SkinDecodeError::InvalidImage};
        OwnedBackendBuffer output(new (std::nothrow) SnowShotImageCodecBuffer{},
                                  &releaseBackendBuffer);
        if (!output)
            return {{}, SkinDecodeError::ResourceLimit};
        SnowShotImageCodecSkinInfo info{};
        uint32_t failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
        if (snow_shot_image_codec_decode_skin_rgba8(
                reinterpret_cast<const uint8_t*>(encoded.constData()),
                static_cast<uint64_t>(encoded.size()), output.get(), &info, &failure) == 0) {
            switch (failure) {
            case SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_UNSUPPORTED_FORMAT:
                return {{}, SkinDecodeError::UnsupportedFormat};
            case SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INPUT_TOO_LARGE:
                return {{}, SkinDecodeError::InputTooLarge};
            case SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_RESOURCE_LIMIT:
                return {{}, SkinDecodeError::ResourceLimit};
            default:
                return {{}, SkinDecodeError::InvalidImage};
            }
        }
        // The backend no longer needs the encoded file. Release it before
        // scaling, color conversion and orientation allocate their output.
        encoded = {};
        QImage image = takeDecodedImage(std::move(output), QImage::Format_RGBA8888);
        if (image.isNull())
            return {{}, SkinDecodeError::ResourceLimit};
        const QSize decodedSize = image.size();
        const QSize canvasSize(static_cast<int>(info.canvas_width),
                               static_cast<int>(info.canvas_height));
        if (canvasSize.isEmpty())
            return {{}, SkinDecodeError::InvalidImage};
        QSize previewCanvas = canvasSize;
        if (previewCanvas.width() > previewExtent || previewCanvas.height() > previewExtent) {
            const qreal scale =
                static_cast<qreal>(previewExtent) / qMax(canvasSize.width(), canvasSize.height());
            previewCanvas = QSize(qMax(1, qRound(canvasSize.width() * scale)),
                                  qMax(1, qRound(canvasSize.height() * scale)));
        }
        const qreal scaleX = static_cast<qreal>(previewCanvas.width()) / canvasSize.width();
        const qreal scaleY = static_cast<qreal>(previewCanvas.height()) / canvasSize.height();
        const QSize framePreview(qMax(1, qRound(decodedSize.width() * scaleX)),
                                 qMax(1, qRound(decodedSize.height() * scaleY)));
        if (framePreview != image.size())
            image = snowCanvasScaleImage(image, framePreview, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
        if (image.isNull())
            return {{}, SkinDecodeError::ResourceLimit};
        const QColorSpace srgb(QColorSpace::SRgb);
        image =
            image.colorSpace().isValid() && image.colorSpace() != srgb
                ? snowCanvasColorConvertedImage(image, srgb, QImage::Format_ARGB32_Premultiplied)
                : snowCanvasConvertImage(image, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return {{}, SkinDecodeError::ResourceLimit};
        image.setColorSpace(srgb);
        if (info.frame_x != 0 || info.frame_y != 0 || decodedSize != canvasSize) {
            QImage canvas =
                snowCanvasAllocateZeroedImage(previewCanvas, QImage::Format_ARGB32_Premultiplied);
            if (canvas.isNull())
                return {{}, SkinDecodeError::ResourceLimit};
            canvas.setColorSpace(srgb);
            QPainter painter(&canvas);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(QRectF(info.frame_x * scaleX, info.frame_y * scaleY,
                                     decodedSize.width() * scaleX, decodedSize.height() * scaleY),
                              image);
            painter.end();
            image = std::move(canvas);
        }
        QTransform orientation;
        switch (info.orientation) {
        case 2:
            orientation = QTransform(-1, 0, 0, 1, 0, 0);
            break;
        case 3:
            orientation = QTransform(-1, 0, 0, -1, 0, 0);
            break;
        case 4:
            orientation = QTransform(1, 0, 0, -1, 0, 0);
            break;
        case 5:
            orientation = QTransform(0, 1, 1, 0, 0, 0);
            break;
        case 6:
            orientation = QTransform(0, 1, -1, 0, 0, 0);
            break;
        case 7:
            orientation = QTransform(0, -1, -1, 0, 0, 0);
            break;
        case 8:
            orientation = QTransform(0, -1, 1, 0, 0, 0);
            break;
        default:
            break;
        }
        if (!orientation.isIdentity())
            image = snowCanvasTransformImage(image, orientation, Qt::FastTransformation);
        if (image.isNull())
            return {{}, SkinDecodeError::ResourceLimit};
        image.setDevicePixelRatio(1.0);
        return {std::move(image), SkinDecodeError::None};
    } catch (const std::bad_alloc&) {
        return {{}, SkinDecodeError::ResourceLimit};
    } catch (...) {
        return {{}, SkinDecodeError::InvalidImage};
    }
}

bool inspectFile(const QString& path, snow::image::Format expectedFormat,
                 const QSize& expectedSize) {
    return inspectBytes(readFile(path), expectedFormat, expectedSize);
}

} // namespace snow_shot::image_codec
