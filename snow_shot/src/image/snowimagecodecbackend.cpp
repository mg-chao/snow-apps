#include "snowimagecodecbridge.h"

#include <snow/image/codec.h>
#include <snow/image/image.h>
#include <snow/image/io.h>
#include <snow/image/service.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

const snow::image::Service& service() {
    static const snow::image::Service instance;
    return instance;
}

void clearError(char* error, uint64_t capacity) noexcept {
    if (error != nullptr && capacity > 0) {
        error[0] = '\0';
    }
}

void setError(char* error, uint64_t capacity, std::string_view message) noexcept {
    if (error == nullptr || capacity == 0) {
        return;
    }
    const uint64_t maximum = capacity - 1;
    const std::size_t count = static_cast<std::size_t>(
        std::min<uint64_t>(maximum, static_cast<uint64_t>(message.size())));
    if (count > 0) {
        std::memcpy(error, message.data(), count);
    }
    error[count] = '\0';
}

bool prepareBuffer(SnowShotImageCodecBuffer* buffer, char* error, uint64_t errorCapacity) noexcept {
    if (buffer == nullptr) {
        setError(error, errorCapacity, "The image output buffer is invalid.");
        return false;
    }
    if (buffer->data != nullptr || buffer->size != 0 || buffer->width != 0 || buffer->height != 0 ||
        buffer->row_stride != 0) {
        setError(error, errorCapacity, "The image output buffer must be released before reuse.");
        return false;
    }
    *buffer = {};
    return true;
}

bool checkedSize(uint64_t value, std::size_t* output) noexcept {
    if (output == nullptr || value > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    *output = static_cast<std::size_t>(value);
    return true;
}

bool checkedProduct(uint64_t left, uint64_t right, uint64_t* output) noexcept {
    if (output == nullptr || (right != 0 && left > std::numeric_limits<uint64_t>::max() / right)) {
        return false;
    }
    *output = left * right;
    return true;
}

bool formatFromBridge(uint32_t value, snow::image::Format* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    switch (value) {
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_BMP:
        *output = snow::image::Format::bmp;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_CUR:
        *output = snow::image::Format::cur;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_GIF:
        *output = snow::image::Format::gif;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_ICO:
        *output = snow::image::Format::ico;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG:
        *output = snow::image::Format::jpeg;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_PBM:
        *output = snow::image::Format::pbm;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_PGM:
        *output = snow::image::Format::pgm;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG:
        *output = snow::image::Format::png;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_PPM:
        *output = snow::image::Format::ppm;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_SVG:
        *output = snow::image::Format::svg;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_SVGZ:
        *output = snow::image::Format::svgz;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_XBM:
        *output = snow::image::Format::xbm;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_XPM:
        *output = snow::image::Format::xpm;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_HEIF:
        *output = snow::image::Format::heif;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_AVIF:
        *output = snow::image::Format::avif;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL:
        *output = snow::image::Format::jxl;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_EXR:
        *output = snow::image::Format::exr;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP:
        *output = snow::image::Format::webp;
        return true;
    default:
        return false;
    }
}

bool chromaSubsamplingFromBridge(uint8_t value, snow::image::ChromaSubsampling* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    switch (value) {
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_NONE:
        *output = snow::image::ChromaSubsampling::none;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV444:
        *output = snow::image::ChromaSubsampling::yuv444;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV422:
        *output = snow::image::ChromaSubsampling::yuv422;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV420:
        *output = snow::image::ChromaSubsampling::yuv420;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV440:
        *output = snow::image::ChromaSubsampling::yuv440;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV411:
        *output = snow::image::ChromaSubsampling::yuv411;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV441:
        *output = snow::image::ChromaSubsampling::yuv441;
        return true;
    default:
        return false;
    }
}

bool alphaContentFromBridge(uint8_t value, snow::image::AlphaContent* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    switch (value) {
    case SNOW_SHOT_IMAGE_CODEC_ALPHA_OPAQUE:
        *output = snow::image::AlphaContent::opaque;
        return true;
    case SNOW_SHOT_IMAGE_CODEC_ALPHA_NON_OPAQUE:
        *output = snow::image::AlphaContent::non_opaque;
        return true;
    default:
        return false;
    }
}

const char* nameHint(snow::image::Format format) noexcept {
    switch (format) {
    case snow::image::Format::png:
        return "snow-shot.png";
    case snow::image::Format::jpeg:
        return "snow-shot.jpg";
    case snow::image::Format::webp:
        return "snow-shot.webp";
    case snow::image::Format::jxl:
        return "snow-shot.jxl";
    case snow::image::Format::avif:
        return "snow-shot.avif";
    default:
        return "snow-shot.image";
    }
}

bool optionsFromBridge(const SnowShotImageCodecEncodeOptions* source,
                       snow::image::EncodeOptions* output, char* error,
                       uint64_t errorCapacity) noexcept {
    constexpr uint32_t expectedSize =
        static_cast<uint32_t>(sizeof(SnowShotImageCodecEncodeOptions));
    if (source == nullptr || output == nullptr || source->struct_size != expectedSize ||
        source->abi_version != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION) {
        setError(error, errorCapacity, "The image encoding options are invalid.");
        return false;
    }
    snow::image::Format format = snow::image::Format::unknown;
    if (!formatFromBridge(source->format, &format)) {
        setError(error, errorCapacity, "The requested image format is invalid.");
        return false;
    }
    output->format = format;
    output->quality = source->quality;
    output->effort = source->effort;
    output->lossless_effort = source->lossless_effort;
    output->compression_level = source->compression_level;
    output->lossless = source->lossless != 0;
    output->preserve_metadata = source->preserve_metadata != 0;
    output->progressive = source->progressive != 0;
    output->interlaced = source->interlaced != 0;
    if (source->has_chroma_subsampling != 0) {
        snow::image::ChromaSubsampling chromaSubsampling{};
        if (!chromaSubsamplingFromBridge(source->chroma_subsampling, &chromaSubsampling)) {
            setError(error, errorCapacity, "The chroma subsampling option is invalid.");
            return false;
        }
        output->chroma_subsampling = chromaSubsampling;
    }
    if (source->has_verified_alpha_content != 0) {
        snow::image::AlphaContent alphaContent{};
        if (!alphaContentFromBridge(source->verified_alpha_content, &alphaContent)) {
            setError(error, errorCapacity, "The verified alpha option is invalid.");
            return false;
        }
        output->verified_alpha_content = alphaContent;
    }
    return true;
}

std::shared_ptr<const std::vector<std::byte>> ownedInput(const uint8_t* encoded, std::size_t size) {
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    if (size > 0) {
        std::memcpy(bytes->data(), encoded, size);
    }
    return bytes;
}

bool publishBytes(std::span<const std::byte> source, SnowShotImageCodecBuffer* output, char* error,
                  uint64_t errorCapacity) noexcept {
    if (source.empty()) {
        setError(error, errorCapacity, "The image encoder produced no data.");
        return false;
    }
    auto* bytes = new (std::nothrow) uint8_t[source.size()];
    if (bytes == nullptr) {
        setError(error, errorCapacity, "The image output could not be allocated.");
        return false;
    }
    std::memcpy(bytes, source.data(), source.size());
    output->data = bytes;
    output->size = static_cast<uint64_t>(source.size());
    return true;
}

bool callbackCancelled(void* context, SnowShotImageCodecCancelCallback callback) noexcept {
    return callback != nullptr && callback(context) != 0;
}

snow::image::Status callbackError(snow::image::ErrorCode code, std::string_view message) {
    return snow::image::Status::error(code, std::string(message), "snow-shot codec bridge");
}

class CallbackRasterSource final : public snow::image::RasterSource {
  public:
    CallbackRasterSource(const SnowShotImageCodecRgba8Source& source, snow::image::Format format)
        : source_(source) {
        descriptor_.format = format;
        descriptor_.canvas_width = source.width;
        descriptor_.canvas_height = source.height;
        snow::image::RasterFrameDescriptor frame;
        frame.width = source.width;
        frame.height = source.height;
        frame.layout.color_model = snow::image::ColorModel::rgb;
        frame.layout.alpha = snow::image::AlphaMode::straight;
        frame.layout.planes.push_back({snow::image::PlaneSemantic::packed, source.width,
                                       source.height, snow::image::kRgba8, 8});
        descriptor_.frames.push_back(std::move(frame));
    }

    const snow::image::DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    snow::image::RasterAccess access() const noexcept override {
        return snow::image::RasterAccess::sequential_rows | snow::image::RasterAccess::random_rows;
    }

    snow::image::Result<void> read_rows(std::uint32_t frameIndex, std::uint32_t planeIndex,
                                        std::uint32_t firstRow, std::uint32_t rowCount,
                                        std::size_t destinationStride,
                                        std::span<std::byte> destination,
                                        std::stop_token stop) const override {
        if (stop.stop_requested() || cancelled()) {
            return callbackError(snow::image::ErrorCode::cancelled,
                                 "Image encoding was cancelled.");
        }
        const std::uint64_t rowBytes = static_cast<std::uint64_t>(source_.width) * 4U;
        const std::uint64_t required =
            rowCount == 0
                ? 0
                : static_cast<std::uint64_t>(destinationStride) * (rowCount - 1U) + rowBytes;
        if (frameIndex != 0 || planeIndex != 0 || rowCount == 0 || firstRow > source_.height ||
            rowCount > source_.height - firstRow || destinationStride < rowBytes ||
            required > destination.size()) {
            return callbackError(snow::image::ErrorCode::invalid_argument,
                                 "The requested image row range is invalid.");
        }
        if (source_.read_rows(source_.context, firstRow, rowCount, destinationStride,
                              reinterpret_cast<std::uint8_t*>(destination.data()),
                              destination.size()) == 0) {
            return callbackError(snow::image::ErrorCode::io_error,
                                 cancelled() ? "Image encoding was cancelled."
                                             : "The image row provider failed.");
        }
        return {};
    }

  private:
    bool cancelled() const noexcept {
        return callbackCancelled(source_.context, source_.is_cancelled);
    }

    SnowShotImageCodecRgba8Source source_{};
    snow::image::DocumentDescriptor descriptor_;
};

class CallbackByteSink final : public snow::image::ByteSink {
  public:
    explicit CallbackByteSink(const SnowShotImageCodecByteSink& sink) : sink_(sink) {}

    snow::image::Result<void> write(std::span<const std::byte> source) override {
        if (cancelled()) {
            return callbackError(snow::image::ErrorCode::cancelled,
                                 "Image encoding was cancelled.");
        }
        if (!source.empty() &&
            sink_.write(sink_.context, reinterpret_cast<const std::uint8_t*>(source.data()),
                        source.size()) == 0) {
            return callbackError(snow::image::ErrorCode::io_error,
                                 cancelled() ? "Image encoding was cancelled."
                                             : "The image output writer failed.");
        }
        return {};
    }

    snow::image::Result<std::uint64_t> position() const override {
        std::uint64_t result = 0;
        if (sink_.position(sink_.context, &result) == 0) {
            return callbackError(snow::image::ErrorCode::io_error,
                                 "The image output position is unavailable.");
        }
        return result;
    }

    snow::image::Result<void> seek(std::uint64_t position) override {
        if (!seekable() || sink_.seek == nullptr || sink_.seek(sink_.context, position) == 0) {
            return callbackError(snow::image::ErrorCode::io_error,
                                 "The image output could not seek.");
        }
        return {};
    }

    snow::image::Result<void> flush() override {
        if (cancelled()) {
            return callbackError(snow::image::ErrorCode::cancelled,
                                 "Image encoding was cancelled.");
        }
        if (sink_.flush(sink_.context) == 0) {
            return callbackError(snow::image::ErrorCode::io_error,
                                 "The image output could not be flushed.");
        }
        return {};
    }

    bool seekable() const noexcept override {
        return sink_.seekable != 0;
    }

  private:
    bool cancelled() const noexcept {
        return callbackCancelled(sink_.context, sink_.is_cancelled);
    }

    SnowShotImageCodecByteSink sink_{};
};

} // namespace

uint32_t snow_shot_image_codec_abi_version(void) {
    return SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
}

int32_t snow_shot_image_codec_encode_rgba8(const uint8_t* pixels, uint64_t pixelsSize,
                                           uint32_t width, uint32_t height, uint64_t rowStride,
                                           const SnowShotImageCodecEncodeOptions* bridgeOptions,
                                           SnowShotImageCodecBuffer* output, char* error,
                                           uint64_t errorCapacity) {
    clearError(error, errorCapacity);
    if (!prepareBuffer(output, error, errorCapacity)) {
        return 0;
    }
    try {
        if (pixels == nullptr || width == 0 || height == 0) {
            setError(error, errorCapacity, "The source image is empty.");
            return 0;
        }
        uint64_t rowBytes = 0;
        uint64_t requiredSize = 0;
        if (!checkedProduct(width, 4, &rowBytes) || rowStride < rowBytes ||
            !checkedProduct(rowStride, height, &requiredSize) || pixelsSize < requiredSize) {
            setError(error, errorCapacity, "The source image layout is invalid.");
            return 0;
        }
        std::size_t sourceStride = 0;
        std::size_t sourceRowBytes = 0;
        if (!checkedSize(rowStride, &sourceStride) || !checkedSize(rowBytes, &sourceRowBytes) ||
            requiredSize > std::numeric_limits<std::size_t>::max()) {
            setError(error, errorCapacity, "The source image is too large.");
            return 0;
        }

        snow::image::EncodeOptions options;
        if (!optionsFromBridge(bridgeOptions, &options, error, errorCapacity)) {
            return 0;
        }
        snow::image::Result<snow::image::MutableImage> allocated =
            snow::image::MutableImage::allocate(width, height, snow::image::kRgba8);
        if (!allocated) {
            setError(error, errorCapacity, allocated.error().message);
            return 0;
        }
        snow::image::MutableImage image = std::move(allocated).value();
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(image.pixels().data() + static_cast<std::size_t>(row) * image.row_stride(),
                        pixels + static_cast<std::size_t>(row) * sourceStride, sourceRowBytes);
        }

        snow::image::Document document;
        document.format = options.format;
        document.canvas_width = width;
        document.canvas_height = height;
        snow::image::Frame frame;
        frame.image = std::move(image).freeze();
        document.frames.push_back(std::move(frame));

        auto encoded = std::make_shared<std::vector<std::byte>>();
        snow::image::Result<snow::image::EncodeResult> result = service().encode(
            document, snow::image::memory_output(encoded, nameHint(options.format)), options);
        if (!result) {
            setError(error, errorCapacity, result.error().message);
            return 0;
        }
        return publishBytes(*encoded, output, error, errorCapacity) ? 1 : 0;
    } catch (const std::exception& exception) {
        setError(error, errorCapacity, exception.what());
    } catch (...) {
        setError(error, errorCapacity, "Image encoding failed unexpectedly.");
    }
    return 0;
}

int32_t snow_shot_image_codec_encode_rgba8_stream(
    const SnowShotImageCodecRgba8Source* source, const SnowShotImageCodecByteSink* sink,
    const SnowShotImageCodecEncodeOptions* bridgeOptions, uint64_t* bytesWritten, char* error,
    uint64_t errorCapacity) {
    clearError(error, errorCapacity);
    if (bytesWritten != nullptr) {
        *bytesWritten = 0;
    }
    try {
        if (source == nullptr || sink == nullptr || bytesWritten == nullptr ||
            source->struct_size != sizeof(SnowShotImageCodecRgba8Source) ||
            sink->struct_size != sizeof(SnowShotImageCodecByteSink) ||
            source->abi_version != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION ||
            sink->abi_version != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION || source->width == 0 ||
            source->height == 0 || source->read_rows == nullptr || sink->write == nullptr ||
            sink->position == nullptr || sink->flush == nullptr ||
            (sink->seekable != 0 && sink->seek == nullptr)) {
            setError(error, errorCapacity, "The streaming image callbacks are invalid.");
            return 0;
        }
        snow::image::EncodeOptions options;
        if (!optionsFromBridge(bridgeOptions, &options, error, errorCapacity)) {
            return 0;
        }
        CallbackRasterSource raster(*source, options.format);
        auto outputSink = std::make_shared<CallbackByteSink>(*sink);
        snow::image::Output output{std::move(outputSink), nameHint(options.format)};
        snow::image::Result<snow::image::EncodeResult> result =
            service().encode(raster, output, options);
        if (!result) {
            setError(error, errorCapacity, result.error().message);
            return 0;
        }
        *bytesWritten = result.value().bytes_written;
        return *bytesWritten > 0 ? 1 : 0;
    } catch (const std::bad_alloc&) {
        setError(error, errorCapacity, "Streaming image encoding ran out of memory.");
        return 0;
    } catch (const std::exception& exception) {
        setError(error, errorCapacity, exception.what());
        return 0;
    } catch (...) {
        setError(error, errorCapacity, "Streaming image encoding failed unexpectedly.");
        return 0;
    }
}

int32_t snow_shot_image_codec_decode_rgba8(const uint8_t* encoded, uint64_t encodedSize,
                                           uint32_t expectedFormat,
                                           SnowShotImageCodecBuffer* output, char* error,
                                           uint64_t errorCapacity) {
    clearError(error, errorCapacity);
    if (!prepareBuffer(output, error, errorCapacity)) {
        return 0;
    }
    try {
        snow::image::Format format = snow::image::Format::unknown;
        std::size_t inputSize = 0;
        if (encoded == nullptr || encodedSize == 0 || !formatFromBridge(expectedFormat, &format) ||
            !checkedSize(encodedSize, &inputSize)) {
            setError(error, errorCapacity, "The encoded image input is invalid.");
            return 0;
        }
        const auto bytes = ownedInput(encoded, inputSize);
        snow::image::DecodeOptions options;
        options.output_format = snow::image::kRgba8;
        options.raster_layout = snow::image::RasterLayoutPolicy::packed;
        snow::image::Result<snow::image::Document> decoded =
            service().decode(snow::image::memory_input(bytes, nameHint(format)), options);
        if (!decoded) {
            setError(error, errorCapacity, decoded.error().message);
            return 0;
        }
        if (decoded.value().format != format || decoded.value().frames.empty()) {
            setError(error, errorCapacity, "The decoded image format is not the expected format.");
            return 0;
        }
        const snow::image::Image& image = decoded.value().frames.front().image;
        if (image.format() != snow::image::kRgba8 || image.width() == 0 || image.height() == 0) {
            setError(error, errorCapacity, "The decoded image does not contain RGBA pixels.");
            return 0;
        }
        uint64_t rowBytes = 0;
        uint64_t outputSize = 0;
        if (!checkedProduct(image.width(), 4, &rowBytes) ||
            !checkedProduct(rowBytes, image.height(), &outputSize) ||
            outputSize > std::numeric_limits<std::size_t>::max()) {
            setError(error, errorCapacity, "The decoded image is too large.");
            return 0;
        }
        auto* pixels = new (std::nothrow) uint8_t[static_cast<std::size_t>(outputSize)];
        if (pixels == nullptr) {
            setError(error, errorCapacity, "The decoded image could not be allocated.");
            return 0;
        }
        const std::size_t destinationStride = static_cast<std::size_t>(rowBytes);
        for (uint32_t row = 0; row < image.height(); ++row) {
            std::memcpy(pixels + static_cast<std::size_t>(row) * destinationStride,
                        image.pixels().data() + static_cast<std::size_t>(row) * image.row_stride(),
                        destinationStride);
        }
        output->data = pixels;
        output->size = outputSize;
        output->width = image.width();
        output->height = image.height();
        output->row_stride = rowBytes;
        return 1;
    } catch (const std::exception& exception) {
        setError(error, errorCapacity, exception.what());
    } catch (...) {
        setError(error, errorCapacity, "Image decoding failed unexpectedly.");
    }
    return 0;
}

int32_t snow_shot_image_codec_inspect(const uint8_t* encoded, uint64_t encodedSize,
                                      uint32_t expectedFormat, SnowShotImageCodecImageInfo* output,
                                      char* error, uint64_t errorCapacity) {
    clearError(error, errorCapacity);
    if (output != nullptr) {
        *output = {};
    }
    try {
        snow::image::Format format = snow::image::Format::unknown;
        std::size_t inputSize = 0;
        if (encoded == nullptr || encodedSize == 0 || output == nullptr ||
            !formatFromBridge(expectedFormat, &format) || !checkedSize(encodedSize, &inputSize)) {
            setError(error, errorCapacity, "The encoded image input is invalid.");
            return 0;
        }
        const auto bytes = ownedInput(encoded, inputSize);
        snow::image::Result<snow::image::DocumentInfo> information =
            service().inspect(snow::image::memory_input(bytes, nameHint(format)));
        if (!information) {
            setError(error, errorCapacity, information.error().message);
            return 0;
        }
        if (information.value().format != format || information.value().frames.size() != 1) {
            setError(error, errorCapacity, "The image does not match the expected format.");
            return 0;
        }
        output->width = information.value().frames.front().width;
        output->height = information.value().frames.front().height;
        return 1;
    } catch (const std::exception& exception) {
        setError(error, errorCapacity, exception.what());
    } catch (...) {
        setError(error, errorCapacity, "Image inspection failed unexpectedly.");
    }
    if (output != nullptr) {
        *output = {};
    }
    return 0;
}

void snow_shot_image_codec_release_buffer(SnowShotImageCodecBuffer* buffer) {
    if (buffer == nullptr) {
        return;
    }
    delete[] buffer->data;
    *buffer = {};
}
