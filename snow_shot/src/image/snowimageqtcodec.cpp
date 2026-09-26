#include "snowimageqtcodec.h"

#include "snowimagecodecbridge.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include <QFile>
#include <QFileDevice>
#include <QBuffer>
#include <QColorSpace>
#include <QIODevice>

namespace snow_shot::image_codec {
namespace {

constexpr std::size_t kBackendErrorCapacity = 1024;

class BackendBuffer final {
  public:
    BackendBuffer() = default;
    ~BackendBuffer() {
        snow_shot_image_codec_release_buffer(&value);
    }

    BackendBuffer(const BackendBuffer&) = delete;
    BackendBuffer& operator=(const BackendBuffer&) = delete;

    SnowShotImageCodecBuffer value{};
};

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

QImage decodeBytes(const QByteArray& encoded, snow::image::Format expectedFormat) {
    const uint32_t bridgeExpectedFormat = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() ||
        bridgeExpectedFormat == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN) {
        return {};
    }

    BackendBuffer output;
    std::array<char, kBackendErrorCapacity> backendError{};
    const int32_t succeeded = snow_shot_image_codec_decode_rgba8(
        reinterpret_cast<const uint8_t*>(encoded.constData()),
        static_cast<uint64_t>(encoded.size()), bridgeExpectedFormat, &output.value,
        backendError.data(), static_cast<uint64_t>(backendError.size()));
    if (succeeded == 0 || output.value.data == nullptr || output.value.width == 0 ||
        output.value.height == 0 ||
        output.value.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        output.value.height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    const uint64_t rowBytes = static_cast<uint64_t>(output.value.width) * 4U;
    if (output.value.row_stride != rowBytes ||
        output.value.height > std::numeric_limits<uint64_t>::max() / rowBytes) {
        return {};
    }
    const uint64_t requiredSize = rowBytes * output.value.height;
    if (requiredSize > output.value.size ||
        requiredSize > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return {};
    }

    QImage result(static_cast<int>(output.value.width), static_cast<int>(output.value.height),
                  QImage::Format_RGBA8888);
    if (result.isNull() || static_cast<uint64_t>(result.bytesPerLine()) < rowBytes) {
        return {};
    }
    const std::size_t sourceStride = static_cast<std::size_t>(rowBytes);
    for (uint32_t row = 0; row < output.value.height; ++row) {
        std::memcpy(result.scanLine(static_cast<int>(row)),
                    output.value.data + static_cast<std::size_t>(row) * sourceStride, sourceStride);
    }
    return result;
}

QImage decodeBgraBytes(const QByteArray& encoded, snow::image::Format expectedFormat) {
    const uint32_t bridgeExpectedFormat = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() ||
        bridgeExpectedFormat == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN) {
        return {};
    }

    auto* output = new (std::nothrow) SnowShotImageCodecBuffer{};
    if (output == nullptr) {
        return {};
    }
    std::array<char, kBackendErrorCapacity> backendError{};
    const int32_t succeeded = snow_shot_image_codec_decode_bgra8(
        reinterpret_cast<const uint8_t*>(encoded.constData()),
        static_cast<uint64_t>(encoded.size()), bridgeExpectedFormat, output, backendError.data(),
        static_cast<uint64_t>(backendError.size()));
    if (succeeded == 0 || output->data == nullptr || output->width == 0 || output->height == 0 ||
        output->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        output->height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        releaseBackendBuffer(output);
        return {};
    }

    const uint64_t rowBytes = static_cast<uint64_t>(output->width) * 4U;
    if (output->row_stride != rowBytes ||
        output->height > std::numeric_limits<uint64_t>::max() / rowBytes) {
        releaseBackendBuffer(output);
        return {};
    }
    const uint64_t requiredSize = rowBytes * output->height;
    if (requiredSize > output->size ||
        requiredSize > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
        releaseBackendBuffer(output);
        return {};
    }

    QImage result(output->data, static_cast<int>(output->width), static_cast<int>(output->height),
                  static_cast<int>(output->row_stride), QImage::Format_ARGB32,
                  &releaseBackendBuffer, output);
    if (result.isNull()) {
        releaseBackendBuffer(output);
    }
    return result;
}

bool inspectBytes(const QByteArray& encoded, snow::image::Format expectedFormat,
                  const QSize& expectedSize) {
    const uint32_t bridgeExpectedFormat = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() || !expectedSize.isValid() ||
        bridgeExpectedFormat == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN) {
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

    const SnowShotImageCodecEncodeOptions encodedOptions = bridgeOptions(format, options);
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

QByteArray encodePng(const QImage& image, int compressionLevel) {
    snow::image::EncodeOptions options;
    options.compression_level = qBound(0, compressionLevel, 9);
    return encodeImage(image, snow::image::Format::png, options, nullptr);
}

ScreenshotImageRowSource srgbRowSource(const QImage& image) {
    const QColorSpace srgb(QColorSpace::SRgb);
    QImage rgba = image.colorSpace().isValid() && image.colorSpace() != srgb
                      ? image.convertedToColorSpace(srgb, QImage::Format_RGBA8888)
                      : image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
        return {};
    }
    ScreenshotImageRowSource source;
    source.size = rgba.size();
    source.backingImage = rgba;
    source.readRows = [rgba](int first, int count, qsizetype stride, uchar* destination,
                             qsizetype capacity) {
        const qsizetype rowBytes = static_cast<qsizetype>(rgba.width()) * 4;
        if (first < 0 || count <= 0 || first > rgba.height() || count > rgba.height() - first ||
            destination == nullptr || stride < rowBytes || capacity < rowBytes ||
            count - 1 > (capacity - rowBytes) / stride) {
            return false;
        }
        for (int row = 0; row < count; ++row) {
            std::memcpy(destination + row * stride, rgba.constScanLine(first + row),
                        static_cast<std::size_t>(rowBytes));
        }
        return true;
    };
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
QSize inspectSize(const QByteArray& encoded, snow::image::Format expectedFormat) {
    const auto format = bridgeFormat(expectedFormat);
    if (!backendAbiIsCompatible() || encoded.isEmpty() ||
        format == SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN)
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

QImage decodeFile(const QString& path, snow::image::Format expectedFormat) {
    return decodeBytes(readFile(path), expectedFormat);
}

QImage decodeFileBgra(const QString& path, snow::image::Format expectedFormat) {
    return decodeBgraBytes(readFile(path), expectedFormat);
}

bool inspectFile(const QString& path, snow::image::Format expectedFormat,
                 const QSize& expectedSize) {
    return inspectBytes(readFile(path), expectedFormat, expectedSize);
}

} // namespace snow_shot::image_codec
