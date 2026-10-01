#include "snowimagecodecbridge.h"

#include <snow/image/codec.h>
#include <snow/image/image.h>
#include <snow/image/io.h>
#include <snow/image/service.h>
#include <snow/image/processing.h>

#include <algorithm>
#include <array>
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
#include <utility>
#include <filesystem>
#include <thread>
#include <chrono>

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
        buffer->row_stride != 0 || buffer->color.icc_profile != nullptr ||
        buffer->color.icc_profile_size != 0 || buffer->color.primaries != 0 ||
        buffer->color.transfer != 0) {
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

uint32_t formatToBridge(snow::image::Format format) noexcept {
    switch (format) {
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
    case snow::image::Format::unknown:
        return SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN;
    }
    return SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN;
}

bool prepareEncodeResult(SnowShotImageCodecEncodeResult* result, char* error,
                         uint64_t errorCapacity) noexcept {
    if (result == nullptr || result->struct_size != sizeof(SnowShotImageCodecEncodeResult) ||
        result->abi_version != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION) {
        setError(error, errorCapacity, "The image encoding result is invalid.");
        return false;
    }
    *result = {};
    result->struct_size = sizeof(SnowShotImageCodecEncodeResult);
    result->abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    return true;
}

void publishEncodeResult(const snow::image::EncodeResult& source,
                         SnowShotImageCodecEncodeResult* destination) noexcept {
    destination->bytes_written = source.bytes_written;
    destination->encoded_format = formatToBridge(source.receipt.format);
    destination->canvas_width = source.receipt.canvas_width;
    destination->canvas_height = source.receipt.canvas_height;
    destination->emitted_frame_count = source.receipt.emitted_frame_count;
    destination->pixel_round_trip =
        static_cast<uint8_t>(source.round_trip == snow::image::PixelRoundTrip::exact
                                 ? SNOW_SHOT_IMAGE_CODEC_PIXEL_ROUND_TRIP_EXACT
                                 : SNOW_SHOT_IMAGE_CODEC_PIXEL_ROUND_TRIP_CODEC_ARTIFACT);
    destination->encoder_finalized_and_sink_flushed =
        source.receipt.encoder_finalized_and_sink_flushed ? 1 : 0;
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

snow::image::ColorEncoding srgbEncoding() {
    snow::image::ColorEncoding color;
    color.primaries = snow::image::ColorPrimaries::srgb;
    color.transfer = snow::image::TransferFunction::srgb;
    return color;
}

class CallbackRasterSource final : public snow::image::RasterSource {
  public:
    CallbackRasterSource(const SnowShotImageCodecRgba8Source& source, snow::image::Format format)
        : source_(source) {
        descriptor_.format = format;
        descriptor_.canvas_width = source.width;
        descriptor_.canvas_height = source.height;
        descriptor_.color = srgbEncoding();
        snow::image::RasterFrameDescriptor frame;
        frame.color = descriptor_.color;
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

SnowShotImageCodecColorEncoding bridgeColorEncoding(const snow::image::ColorEncoding& color) {
    SnowShotImageCodecColorEncoding result{};
    switch (color.primaries) {
    case snow::image::ColorPrimaries::unknown:
        result.primaries = SNOW_SHOT_IMAGE_CODEC_PRIMARIES_UNKNOWN;
        break;
    case snow::image::ColorPrimaries::srgb:
        result.primaries = SNOW_SHOT_IMAGE_CODEC_PRIMARIES_SRGB;
        break;
    case snow::image::ColorPrimaries::display_p3:
        result.primaries = SNOW_SHOT_IMAGE_CODEC_PRIMARIES_DISPLAY_P3;
        break;
    case snow::image::ColorPrimaries::adobe_rgb:
        result.primaries = SNOW_SHOT_IMAGE_CODEC_PRIMARIES_ADOBE_RGB;
        break;
    case snow::image::ColorPrimaries::rec2020:
        result.primaries = SNOW_SHOT_IMAGE_CODEC_PRIMARIES_REC2020;
        break;
    case snow::image::ColorPrimaries::custom:
        result.primaries = SNOW_SHOT_IMAGE_CODEC_PRIMARIES_CUSTOM;
        break;
    }
    switch (color.transfer) {
    case snow::image::TransferFunction::unknown:
        result.transfer = SNOW_SHOT_IMAGE_CODEC_TRANSFER_UNKNOWN;
        break;
    case snow::image::TransferFunction::linear:
        result.transfer = SNOW_SHOT_IMAGE_CODEC_TRANSFER_LINEAR;
        break;
    case snow::image::TransferFunction::srgb:
        result.transfer = SNOW_SHOT_IMAGE_CODEC_TRANSFER_SRGB;
        break;
    case snow::image::TransferFunction::gamma:
        result.transfer = SNOW_SHOT_IMAGE_CODEC_TRANSFER_GAMMA;
        break;
    case snow::image::TransferFunction::pq:
        result.transfer = SNOW_SHOT_IMAGE_CODEC_TRANSFER_PQ;
        break;
    case snow::image::TransferFunction::hlg:
        result.transfer = SNOW_SHOT_IMAGE_CODEC_TRANSFER_HLG;
        break;
    }
    return result;
}

class PackedDecodeSink final : public snow::image::PixelSink {
  public:
    PackedDecodeSink(snow::image::Format expectedDocumentFormat,
                     snow::image::PixelFormat expectedPixelFormat, bool firstFrameOnly = false,
                     std::uint64_t maximumOutputBytes = std::numeric_limits<std::uint64_t>::max())
        : expectedDocumentFormat_(expectedDocumentFormat),
          expectedPixelFormat_(expectedPixelFormat), firstFrameOnly_(firstFrameOnly),
          maximumOutputBytes_(maximumOutputBytes) {}

    snow::image::Result<void> begin(const snow::image::DocumentInfo& document) override {
        if (document.format != expectedDocumentFormat_) {
            return snow::image::Status::error(
                snow::image::ErrorCode::decode_failed,
                "The decoded image format is not the expected format.");
        }
        if (document.frames.empty()) {
            return snow::image::Status::error(snow::image::ErrorCode::decode_failed,
                                              "The decoded image has no frames.");
        }
        return {};
    }

    snow::image::Result<void> begin_frame(std::uint32_t frameIndex,
                                          const snow::image::FrameInfo& frame) override {
        if (activeFrame_ != kNoFrame) {
            return snow::image::Status::error(
                snow::image::ErrorCode::corrupt_data,
                "The decoder began a frame before ending the prior frame.");
        }
        activeFrame_ = frameIndex;
        expectedRow_ = 0;
        storageUsed_ = false;
        if (frameIndex != 0) {
            return {};
        }
        if (frame.native_format != expectedPixelFormat_ || frame.width == 0 || frame.height == 0) {
            return snow::image::Status::error(
                snow::image::ErrorCode::unsupported_feature,
                "The decoder did not produce the requested packed pixel format.");
        }
        const snow::image::Result<std::size_t> bytesPerPixel =
            expectedPixelFormat_.bytes_per_pixel();
        if (!bytesPerPixel || bytesPerPixel.value() != 4 ||
            frame.width > std::numeric_limits<std::size_t>::max() / bytesPerPixel.value()) {
            return snow::image::Status::error(snow::image::ErrorCode::limit_exceeded,
                                              "The decoded image row size overflows.");
        }
        rowStride_ = static_cast<std::size_t>(frame.width) * bytesPerPixel.value();
        if (frame.height > std::numeric_limits<std::size_t>::max() / rowStride_) {
            return snow::image::Status::error(snow::image::ErrorCode::limit_exceeded,
                                              "The decoded image size overflows.");
        }
        const std::size_t outputSize = rowStride_ * static_cast<std::size_t>(frame.height);
        if (outputSize > maximumOutputBytes_) {
            return snow::image::Status::error(snow::image::ErrorCode::limit_exceeded,
                                              "The decoded image exceeds its output limit.");
        }
        pixels_.reset(new (std::nothrow) std::uint8_t[outputSize]);
        if (!pixels_) {
            return snow::image::Status::error(snow::image::ErrorCode::out_of_memory,
                                              "The decoded image could not be allocated.");
        }
        width_ = frame.width;
        height_ = frame.height;
        color_ = bridgeColorEncoding(frame.color);
        if (!frame.color.icc_profile.empty()) {
            const std::size_t size = frame.color.icc_profile.size();
            iccProfile_.reset(new (std::nothrow) std::uint8_t[size]);
            if (!iccProfile_) {
                return snow::image::Status::error(
                    snow::image::ErrorCode::out_of_memory,
                    "The decoded color profile could not be allocated.");
            }
            std::memcpy(iccProfile_.get(), frame.color.icc_profile.data(), size);
            color_.icc_profile_size = static_cast<std::uint64_t>(size);
        }
        return {};
    }

    std::span<std::byte> frame_storage(std::uint32_t frameIndex, std::size_t rowStride,
                                       std::size_t byteSize) override {
        if (activeFrame_ != 0 || frameIndex != 0 || !pixels_ || rowStride != rowStride_ ||
            byteSize != rowStride_ * static_cast<std::size_t>(height_)) {
            return {};
        }
        storageUsed_ = true;
        return {reinterpret_cast<std::byte*>(pixels_.get()), byteSize};
    }

    snow::image::Result<void> write_rows(std::uint32_t firstRow, std::uint32_t rowCount,
                                         std::size_t sourceStride,
                                         std::span<const std::byte> sourcePixels) override {
        if (activeFrame_ == kNoFrame) {
            return snow::image::Status::error(snow::image::ErrorCode::corrupt_data,
                                              "The decoder wrote rows outside a frame.");
        }
        if (activeFrame_ != 0) {
            return {};
        }
        if (!pixels_ || rowCount == 0 || firstRow != expectedRow_ || firstRow > height_ ||
            rowCount > height_ - firstRow || sourceStride < rowStride_ ||
            rowCount > std::numeric_limits<std::uint32_t>::max() - expectedRow_) {
            return snow::image::Status::error(snow::image::ErrorCode::corrupt_data,
                                              "The decoder produced invalid packed rows.");
        }
        const std::size_t sourceRowCount = static_cast<std::size_t>(rowCount - 1U);
        if (sourceRowCount > std::numeric_limits<std::size_t>::max() / sourceStride) {
            return snow::image::Status::error(snow::image::ErrorCode::limit_exceeded,
                                              "The decoder row stride overflows.");
        }
        const std::size_t lastSourceOffset = sourceRowCount * sourceStride;
        if (lastSourceOffset > sourcePixels.size() ||
            sourcePixels.size() - lastSourceOffset < rowStride_) {
            return snow::image::Status::error(snow::image::ErrorCode::corrupt_data,
                                              "The decoder produced invalid packed rows.");
        }
        for (std::uint32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            std::memcpy(pixels_.get() + static_cast<std::size_t>(firstRow + rowIndex) * rowStride_,
                        sourcePixels.data() + static_cast<std::size_t>(rowIndex) * sourceStride,
                        rowStride_);
        }
        expectedRow_ += rowCount;
        return {};
    }

    snow::image::Result<void> end_frame(std::uint32_t frameIndex) override {
        if (activeFrame_ != frameIndex) {
            return snow::image::Status::error(snow::image::ErrorCode::corrupt_data,
                                              "The decoder ended an unexpected frame.");
        }
        if (frameIndex == 0 && !storageUsed_ && expectedRow_ != height_) {
            return snow::image::Status::error(snow::image::ErrorCode::truncated_data,
                                              "The decoder ended an incomplete packed frame.");
        }
        if (frameIndex == 0) {
            completed_ = true;
        }
        activeFrame_ = kNoFrame;
        if (frameIndex == 0 && firstFrameOnly_) {
            // The skin needs one composited frame. Stop streaming here rather
            // than decoding every frame or materializing an animated document.
            return snow::image::Status::error(snow::image::ErrorCode::cancelled,
                                              "The first preview frame is complete.");
        }
        return {};
    }

    snow::image::Result<void> end() override {
        if (activeFrame_ != kNoFrame || !completed_) {
            return snow::image::Status::error(snow::image::ErrorCode::truncated_data,
                                              "The decoder ended without a packed frame.");
        }
        return {};
    }

    [[nodiscard]] bool hasImage() const noexcept {
        return completed_ && pixels_ && width_ != 0 && height_ != 0 && rowStride_ != 0;
    }

    [[nodiscard]] std::uint8_t* releasePixels() noexcept {
        return pixels_.release();
    }

    [[nodiscard]] SnowShotImageCodecColorEncoding releaseColor() noexcept {
        color_.icc_profile = iccProfile_.release();
        return std::exchange(color_, {});
    }

    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }

    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }

    [[nodiscard]] std::size_t rowStride() const noexcept {
        return rowStride_;
    }

  private:
    static constexpr std::uint32_t kNoFrame = std::numeric_limits<std::uint32_t>::max();

    snow::image::Format expectedDocumentFormat_;
    snow::image::PixelFormat expectedPixelFormat_;
    bool firstFrameOnly_ = false;
    std::uint64_t maximumOutputBytes_ = std::numeric_limits<std::uint64_t>::max();
    std::unique_ptr<std::uint8_t[]> pixels_;
    std::unique_ptr<std::uint8_t[]> iccProfile_;
    SnowShotImageCodecColorEncoding color_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::size_t rowStride_ = 0;
    std::uint32_t activeFrame_ = kNoFrame;
    std::uint32_t expectedRow_ = 0;
    bool storageUsed_ = false;
    bool completed_ = false;
};

constexpr std::uint64_t kSkinInputLimit = 64ULL << 20U;
constexpr std::uint64_t kSkinPixelLimit = 64ULL * 1000 * 1000;
constexpr std::uint64_t kSkinMetadataLimit = 8ULL << 20U;
constexpr std::uint64_t kSkinOutputLimit = 256ULL << 20U;
constexpr std::uint32_t kSkinDimensionLimit = 16384;
constexpr std::uint32_t kSkinPreviewExtent = 4096;

// The adapter keeps its QByteArray alive throughout the call. Avoid the extra
// ownedInput/memory_input copy used by the general screenshot bridge.
class SkinByteSource final : public snow::image::ByteSource {
  public:
    explicit SkinByteSource(std::span<const std::byte> bytes) : m_bytes(bytes) {}

    snow::image::Result<std::uint64_t> size() const override {
        return static_cast<std::uint64_t>(m_bytes.size());
    }

    snow::image::Result<std::size_t> read_at(std::uint64_t offset,
                                             std::span<std::byte> destination) const override {
        if (offset > m_bytes.size()) {
            return snow::image::Status::error(snow::image::ErrorCode::io_error,
                                              "The preview read offset is invalid.");
        }
        const std::size_t begin = static_cast<std::size_t>(offset);
        const std::size_t count = std::min(destination.size(), m_bytes.size() - begin);
        if (count != 0)
            std::memcpy(destination.data(), m_bytes.data() + begin, count);
        return count;
    }

  private:
    std::span<const std::byte> m_bytes;
};

snow::image::Input skinInput(std::span<const std::byte> bytes) {
    return {std::make_shared<SkinByteSource>(bytes), {}};
}

std::uint32_t skinFailure(const snow::image::Status& status) {
    switch (status.code) {
    case snow::image::ErrorCode::limit_exceeded:
    case snow::image::ErrorCode::out_of_memory:
        return SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_RESOURCE_LIMIT;
    case snow::image::ErrorCode::unsupported_format:
    case snow::image::ErrorCode::codec_unavailable:
        return SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_UNSUPPORTED_FORMAT;
    default:
        return SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
    }
}

snow::image::DecodeOptions skinOptions() {
    snow::image::DecodeOptions options;
    options.limits.maximum_width = kSkinDimensionLimit;
    options.limits.maximum_height = kSkinDimensionLimit;
    options.limits.maximum_pixels = kSkinPixelLimit;
    options.limits.maximum_input_bytes = kSkinInputLimit;
    options.limits.maximum_metadata_bytes = kSkinMetadataLimit;
    options.limits.maximum_owned_output_bytes = kSkinOutputLimit;
    options.limits.maximum_working_bytes = 512ULL << 20U;
    options.output_format = snow::image::kRgba8;
    options.frame_index = 0;
    // Apply orientation after reducing the preview. Native orientation fallback
    // can otherwise materialize every frame of an animated WebP.
    options.orientation = snow::image::OrientationPolicy::preserve;
    return options;
}

std::uint32_t bigEndian32(std::span<const std::byte> bytes) {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

std::uint32_t exifOrientation(std::span<const std::byte> exif) {
    if (exif.size() >= 6 && std::memcmp(exif.data(), "Exif\0\0", 6) == 0)
        exif = exif.subspan(6);
    if (exif.size() < 8)
        return 1;
    const bool little = exif[0] == std::byte{'I'} && exif[1] == std::byte{'I'};
    const bool big = exif[0] == std::byte{'M'} && exif[1] == std::byte{'M'};
    if (!little && !big)
        return 1;
    const auto read16 = [exif, little](std::size_t offset) -> std::uint16_t {
        const auto a = std::to_integer<std::uint16_t>(exif[offset]);
        const auto b = std::to_integer<std::uint16_t>(exif[offset + 1]);
        return static_cast<std::uint16_t>(little ? a | (b << 8U) : (a << 8U) | b);
    };
    const auto read32 = [exif, little](std::size_t offset) -> std::uint32_t {
        if (!little)
            return bigEndian32(exif.subspan(offset, 4));
        return std::to_integer<std::uint32_t>(exif[offset]) |
               (std::to_integer<std::uint32_t>(exif[offset + 1]) << 8U) |
               (std::to_integer<std::uint32_t>(exif[offset + 2]) << 16U) |
               (std::to_integer<std::uint32_t>(exif[offset + 3]) << 24U);
    };
    if (read16(2) != 42)
        return 1;
    const std::size_t directory = read32(4);
    if (directory > exif.size() - 2)
        return 1;
    const std::size_t count = read16(directory);
    if (count > (exif.size() - directory - 2) / 12)
        return 1;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t entry = directory + 2 + index * 12;
        if (read16(entry) == 0x0112 && read16(entry + 2) == 3 && read32(entry + 4) == 1) {
            const std::uint32_t value = read16(entry + 8);
            return value >= 1 && value <= 8 ? value : 1;
        }
    }
    return 1;
}

std::uint32_t skinJpegOrientation(std::span<const std::byte> bytes, bool* metadataTooLarge) {
    std::size_t offset = 2;
    std::uint64_t metadataBytes = 0;
    std::uint32_t orientation = 1;
    while (offset + 4 <= bytes.size() && bytes[offset] == std::byte{0xff}) {
        while (offset < bytes.size() && bytes[offset] == std::byte{0xff})
            ++offset;
        if (offset >= bytes.size())
            break;
        const auto marker = std::to_integer<unsigned>(bytes[offset++]);
        if (marker == 0xda || marker == 0xd9)
            break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (offset + 2 > bytes.size())
            break;
        const std::size_t length = (std::to_integer<std::size_t>(bytes[offset]) << 8U) |
                                   std::to_integer<std::size_t>(bytes[offset + 1]);
        if (length < 2 || length > bytes.size() - offset)
            break;
        if ((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe) {
            metadataBytes += length - 2;
            if (metadataBytes > kSkinMetadataLimit) {
                *metadataTooLarge = true;
                return 1;
            }
        }
        if (marker == 0xe1 && length >= 8 &&
            std::memcmp(bytes.data() + offset + 2, "Exif\0\0", 6) == 0) {
            orientation = exifOrientation(bytes.subspan(offset + 2, length - 2));
        }
        offset += length;
    }
    return orientation;
}

std::uint32_t pngCrc(std::span<const std::byte> bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < values.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
            values[index] = value;
        }
        return values;
    }();
    std::uint32_t value = 0xffffffffU;
    for (const std::byte byte : bytes)
        value = table[(value ^ std::to_integer<unsigned>(byte)) & 0xffU] ^ (value >> 8U);
    return value ^ 0xffffffffU;
}

void appendBigEndian32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (const unsigned shift : {24U, 16U, 8U, 0U})
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

void appendPngChunk(std::vector<std::byte>& output, const char* type,
                    std::span<const std::byte> payload) {
    appendBigEndian32(output, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crcBegin = output.size();
    const auto* typeBytes = reinterpret_cast<const std::byte*>(type);
    output.insert(output.end(), typeBytes, typeBytes + 4);
    output.insert(output.end(), payload.begin(), payload.end());
    const std::uint32_t crc = pngCrc(std::span(output).subspan(crcBegin));
    appendBigEndian32(output, crc);
}

struct SkinPngFrame final {
    std::vector<std::byte> encoded;
    std::uint32_t failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_NONE;
};

// libpng decodes an APNG's default image, which may be a separate poster.
// Repackage only the first animation frame as a normal PNG, retaining palette,
// transparency and color metadata. The adapter places it on the original canvas.
SkinPngFrame skinPngFirstFrame(std::span<const std::byte> bytes, SnowShotImageCodecSkinInfo* info) {
    SkinPngFrame result;
    if (bytes.size() < 33 || bigEndian32(bytes.subspan(8, 4)) != 13 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
        result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
        return result;
    }
    info->canvas_width = bigEndian32(bytes.subspan(16, 4));
    info->canvas_height = bigEndian32(bytes.subspan(20, 4));
    if (info->canvas_width == 0 || info->canvas_height == 0 ||
        info->canvas_width > kSkinDimensionLimit || info->canvas_height > kSkinDimensionLimit ||
        static_cast<std::uint64_t>(info->canvas_width) * info->canvas_height > kSkinPixelLimit) {
        result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_RESOURCE_LIMIT;
        return result;
    }
    std::vector<std::byte> common;
    std::vector<std::span<const std::byte>> imageData;
    std::uint64_t metadataBytes = 0;
    std::uint32_t frameWidth = info->canvas_width;
    std::uint32_t frameHeight = info->canvas_height;
    bool animated = false;
    bool imageSeen = false;
    bool selected = false;
    bool usesDefault = false;
    std::uint32_t sequence = 0;
    for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
        const std::size_t length = bigEndian32(bytes.subspan(offset, 4));
        if (length > bytes.size() - offset - 12) {
            result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
            return result;
        }
        const auto type = bytes.subspan(offset + 4, 4);
        const auto data = bytes.subspan(offset + 8, length);
        const auto is = [type](const char* name) { return std::memcmp(type.data(), name, 4) == 0; };
        if ((is("IHDR") || is("acTL") || is("fcTL") || (is("IDAT") && selected && usesDefault)) &&
            pngCrc(bytes.subspan(offset + 4, length + 4)) !=
                bigEndian32(bytes.subspan(offset + 8 + length, 4))) {
            result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
            return result;
        }
        if (is("acTL")) {
            animated = true;
            if (length != 8 || bigEndian32(data.first(4)) == 0 ||
                bigEndian32(data.first(4)) > 10000) {
                result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
                return result;
            }
        } else if (is("fcTL")) {
            if (selected)
                break;
            if (length != 26 || !animated || bigEndian32(data.first(4)) != sequence++ ||
                std::to_integer<unsigned>(data[24]) > 2 ||
                std::to_integer<unsigned>(data[25]) > 1) {
                result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
                return result;
            }
            selected = true;
            usesDefault = !imageSeen;
            frameWidth = bigEndian32(data.subspan(4, 4));
            frameHeight = bigEndian32(data.subspan(8, 4));
            info->frame_x = bigEndian32(data.subspan(12, 4));
            info->frame_y = bigEndian32(data.subspan(16, 4));
            if (frameWidth == 0 || frameHeight == 0 || frameWidth > info->canvas_width ||
                frameHeight > info->canvas_height ||
                info->frame_x > info->canvas_width - frameWidth ||
                info->frame_y > info->canvas_height - frameHeight) {
                result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
                return result;
            }
            if (usesDefault &&
                (frameWidth != info->canvas_width || frameHeight != info->canvas_height ||
                 info->frame_x != 0 || info->frame_y != 0)) {
                result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
                return result;
            }
        } else if (is("IDAT")) {
            imageSeen = true;
            if (selected && usesDefault)
                imageData.push_back(data);
        } else if (is("fdAT")) {
            if (selected && !usesDefault) {
                if (length < 4 || bigEndian32(data.first(4)) != sequence++) {
                    result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
                    return result;
                }
                if (pngCrc(bytes.subspan(offset + 4, length + 4)) !=
                    bigEndian32(bytes.subspan(offset + 8 + length, 4))) {
                    result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
                    return result;
                }
                imageData.push_back(data.subspan(4));
            }
        } else if (is("IEND")) {
            break;
        } else {
            metadataBytes += length;
            if (metadataBytes > kSkinMetadataLimit) {
                result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_RESOURCE_LIMIT;
                return result;
            }
            if (!imageSeen && !is("IHDR")) {
                common.insert(common.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + length + 12));
            }
        }
        offset += length + 12;
    }
    if (!animated)
        return result;
    if (!selected || imageData.empty()) {
        result.failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
        return result;
    }
    result.encoded.insert(result.encoded.end(), bytes.begin(), bytes.begin() + 8);
    std::array<std::byte, 13> header{};
    std::copy_n(bytes.begin() + 16, header.size(), header.begin());
    for (unsigned index = 0; index < 4; ++index) {
        header[index] = static_cast<std::byte>((frameWidth >> (24U - index * 8U)) & 0xffU);
        header[index + 4] = static_cast<std::byte>((frameHeight >> (24U - index * 8U)) & 0xffU);
    }
    appendPngChunk(result.encoded, "IHDR", header);
    result.encoded.insert(result.encoded.end(), common.begin(), common.end());
    for (const auto data : imageData)
        appendPngChunk(result.encoded, "IDAT", data);
    appendPngChunk(result.encoded, "IEND", {});
    return result;
}

} // namespace

uint32_t snow_shot_image_codec_abi_version(void) {
    return SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
}

int32_t snow_shot_image_codec_encoder_info(uint32_t bridgeFormat,
                                           SnowShotImageCodecEncoderInfo* output) {
    try {
        snow::image::Format format;
        if (output == nullptr || output->struct_size != sizeof(*output) ||
            output->abi_version != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION ||
            !formatFromBridge(bridgeFormat, &format)) {
            return 0;
        }
        const auto* info = service().encoder_info(format);
        if (info == nullptr)
            return 0;
        const auto range = [](const snow::image::EncoderOptionRange& source) {
            return SnowShotImageCodecEncoderOptionRange{source.minimum, source.maximum,
                                                        source.default_value};
        };
        const uint32_t structSize = output->struct_size;
        const uint32_t abiVersion = output->abi_version;
        *output = {};
        output->struct_size = structSize;
        output->abi_version = abiVersion;
        output->format = formatToBridge(info->format);
        output->features = static_cast<uint32_t>(info->features);
        output->quality = range(info->quality);
        output->effort = range(info->effort);
        output->lossless_effort = range(info->lossless_effort);
        output->compression_level = range(info->compression_level);
        return 1;
    } catch (...) {
        return 0;
    }
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
        document.color = srgbEncoding();
        snow::image::Frame frame;
        frame.color = document.color;
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
    const SnowShotImageCodecEncodeOptions* bridgeOptions,
    SnowShotImageCodecEncodeResult* outputResult, char* error, uint64_t errorCapacity) {
    clearError(error, errorCapacity);
    if (!prepareEncodeResult(outputResult, error, errorCapacity))
        return 0;
    try {
        if (source == nullptr || sink == nullptr ||
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
        snow::image::Result<snow::image::EncodeResult> encodedResult =
            service().encode(raster, output, options);
        if (!encodedResult) {
            setError(error, errorCapacity, encodedResult.error().message);
            return 0;
        }
        publishEncodeResult(encodedResult.value(), outputResult);
        return outputResult->bytes_written > 0 ? 1 : 0;
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

int32_t decodePacked8(const uint8_t* encoded, uint64_t encodedSize, uint32_t expectedFormat,
                      snow::image::PixelFormat outputFormat, const char* pixelDescription,
                      SnowShotImageCodecBuffer* output, char* error, uint64_t errorCapacity,
                      uint32_t preferredIconExtent = 0) {
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
        options.output_format = outputFormat;
        options.raster_layout = snow::image::RasterLayoutPolicy::packed;
        if (preferredIconExtent != 0) {
            const auto information =
                service().inspect(snow::image::memory_input(bytes, nameHint(format)));
            if (!information || information.value().format != snow::image::Format::ico ||
                information.value().frames.empty()) {
                setError(error, errorCapacity, "The icon directory is invalid.");
                return 0;
            }
            uint64_t bestScore = std::numeric_limits<uint64_t>::max();
            const auto& frames = information.value().frames;
            for (std::size_t index = 0; index < frames.size(); ++index) {
                const auto& frame = frames[index];
                const auto distance = [preferredIconExtent](uint32_t extent) -> uint64_t {
                    return extent > preferredIconExtent ? extent - preferredIconExtent
                                                        : preferredIconExtent - extent;
                };
                const uint64_t score = distance(frame.width) + distance(frame.height);
                if (score < bestScore) {
                    bestScore = score;
                    options.frame_index = static_cast<uint32_t>(index);
                }
            }
            options.limits.maximum_width = 16384;
            options.limits.maximum_height = 16384;
            options.limits.maximum_pixels = 64ULL * 1024 * 1024;
        }
        PackedDecodeSink sink(format, outputFormat);
        snow::image::Result<void> decoded = service().decode_to_sink(
            snow::image::memory_input(bytes, nameHint(format)), sink, options);
        if (!decoded) {
            setError(error, errorCapacity, decoded.error().message);
            return 0;
        }
        if (!sink.hasImage()) {
            setError(error, errorCapacity, pixelDescription);
            return 0;
        }
        output->color = sink.releaseColor();
        output->data = sink.releasePixels();
        output->width = sink.width();
        output->height = sink.height();
        output->row_stride = sink.rowStride();
        output->size = static_cast<uint64_t>(output->row_stride) * output->height;
        return 1;
    } catch (const std::exception& exception) {
        setError(error, errorCapacity, exception.what());
    } catch (...) {
        setError(error, errorCapacity, "Image decoding failed unexpectedly.");
    }
    return 0;
}

int32_t snow_shot_image_codec_decode_rgba8(const uint8_t* encoded, uint64_t encodedSize,
                                           uint32_t expectedFormat,
                                           SnowShotImageCodecBuffer* output, char* error,
                                           uint64_t errorCapacity) {
    return decodePacked8(encoded, encodedSize, expectedFormat, snow::image::kRgba8,
                         "The decoded image does not contain RGBA pixels.", output, error,
                         errorCapacity);
}

int32_t snow_shot_image_codec_decode_skin_rgba8(const uint8_t* encoded, uint64_t encodedSize,
                                                SnowShotImageCodecBuffer* output,
                                                SnowShotImageCodecSkinInfo* info,
                                                uint32_t* failure) {
    if (failure == nullptr)
        return 0;
    *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
    if (info != nullptr)
        *info = {};
    if (encoded == nullptr || encodedSize == 0 || info == nullptr ||
        !prepareBuffer(output, nullptr, 0))
        return 0;
    if (encodedSize > kSkinInputLimit) {
        *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INPUT_TOO_LARGE;
        return 0;
    }
    try {
        const auto original = std::span(reinterpret_cast<const std::byte*>(encoded),
                                        static_cast<std::size_t>(encodedSize));
        const bool png =
            original.size() >= 8 && std::memcmp(original.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
        const bool jpeg = original.size() >= 2 && original[0] == std::byte{0xff} &&
                          original[1] == std::byte{0xd8};
        const bool webp = original.size() >= 12 && std::memcmp(original.data(), "RIFF", 4) == 0 &&
                          std::memcmp(original.data() + 8, "WEBP", 4) == 0;
        if (!png && !jpeg && !webp) {
            *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_UNSUPPORTED_FORMAT;
            return 0;
        }
        const auto format = png ? snow::image::Format::png
                                : (jpeg ? snow::image::Format::jpeg : snow::image::Format::webp);
        SkinPngFrame firstPng;
        std::span<const std::byte> bytes = original;
        if (png) {
            firstPng = skinPngFirstFrame(original, info);
            if (firstPng.failure != SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_NONE) {
                *failure = firstPng.failure;
                return 0;
            }
            if (!firstPng.encoded.empty())
                bytes = firstPng.encoded;
        }
        bool metadataTooLarge = false;
        const std::uint32_t jpegOrientation =
            jpeg ? skinJpegOrientation(original, &metadataTooLarge) : 1;
        if (metadataTooLarge) {
            *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_RESOURCE_LIMIT;
            return 0;
        }
        const auto input = skinInput(bytes);
        auto options = skinOptions();
        const auto information = service().inspect(input, options);
        if (!information) {
            *failure = skinFailure(information.error());
            return 0;
        }
        if (information.value().format != format || information.value().frames.empty())
            return 0;
        info->orientation =
            jpeg ? jpegOrientation
                 : static_cast<std::uint32_t>(information.value().metadata.orientation);
        options.maximum_extent = kSkinPreviewExtent;
        PackedDecodeSink sink(format, snow::image::kRgba8, true, kSkinOutputLimit);
        auto decoded = service().decode_to_sink(input, sink, options);
        // JPEG has a finite native scaling range. A bounded full decode remains
        // valid when no supported factor fits the requested preview extent.
        if (!decoded && !sink.hasImage() && jpeg &&
            decoded.error().code == snow::image::ErrorCode::limit_exceeded) {
            options.maximum_extent.reset();
            decoded = service().decode_to_sink(input, sink, options);
        }
        if (!sink.hasImage() ||
            (!decoded && decoded.error().code != snow::image::ErrorCode::cancelled)) {
            *failure = decoded ? SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE
                               : skinFailure(decoded.error());
            return 0;
        }
        output->color = sink.releaseColor();
        output->data = sink.releasePixels();
        output->width = sink.width();
        output->height = sink.height();
        output->row_stride = sink.rowStride();
        output->size = static_cast<uint64_t>(output->row_stride) * output->height;
        if (!png) {
            info->canvas_width = output->width;
            info->canvas_height = output->height;
        }
        *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_NONE;
        return 1;
    } catch (const std::bad_alloc&) {
        *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_RESOURCE_LIMIT;
    } catch (...) {
        *failure = SNOW_SHOT_IMAGE_CODEC_SKIN_ERROR_INVALID_IMAGE;
    }
    return 0;
}

int32_t snow_shot_image_codec_decode_icon_rgba8(const uint8_t* encoded, uint64_t encodedSize,
                                                uint32_t preferredExtent,
                                                SnowShotImageCodecBuffer* output, char* error,
                                                uint64_t errorCapacity) {
    return decodePacked8(encoded, encodedSize, SNOW_SHOT_IMAGE_CODEC_FORMAT_ICO,
                         snow::image::kRgba8, "The decoded icon does not contain RGBA pixels.",
                         output, error, errorCapacity, preferredExtent);
}

int32_t snow_shot_image_codec_decode_bgra8(const uint8_t* encoded, uint64_t encodedSize,
                                           uint32_t expectedFormat,
                                           SnowShotImageCodecBuffer* output, char* error,
                                           uint64_t errorCapacity) {
    return decodePacked8(encoded, encodedSize, expectedFormat, snow::image::kBgra8,
                         "The decoded image does not contain BGRA pixels.", output, error,
                         errorCapacity);
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
    delete[] buffer->color.icc_profile;
    *buffer = {};
}

namespace {
bool validExportDimensions(uint32_t width, uint32_t height) {
    return width && height && uint64_t(width) * height <= uint64_t{2} * 1024 * 1024 * 1024 &&
           uint64_t(width) * height <= std::numeric_limits<size_t>::max() / 4;
}
class ExportPixelSink final : public snow::image::PixelSink {
  public:
    ExportPixelSink(uint8_t* pixels, uint32_t width, uint32_t height)
        : m_pixels(pixels), m_width(width), m_height(height) {}
    snow::image::Result<void> begin(const snow::image::DocumentInfo&) override {
        return {};
    }
    snow::image::Result<void> begin_frame(uint32_t index,
                                          const snow::image::FrameInfo& info) override {
        if (index != 0 || info.width != m_width || info.height != m_height ||
            info.native_format != snow::image::kRgba8) {
            return snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                              "Unexpected export raster dimensions or format.");
        }
        return {};
    }
    std::span<std::byte> frame_storage(uint32_t index, size_t stride, size_t bytes) override {
        if (index != 0 || stride != size_t(m_width) * 4 || bytes != stride * m_height)
            return {};
        return {reinterpret_cast<std::byte*>(m_pixels), bytes};
    }
    snow::image::Result<void> write_rows(uint32_t first, uint32_t count, size_t stride,
                                         std::span<const std::byte> bytes) override {
        const size_t rowBytes = size_t(m_width) * 4;
        if (first > m_height || count > m_height - first || stride < rowBytes ||
            (count &&
             (bytes.size() < rowBytes || size_t(count - 1) > (bytes.size() - rowBytes) / stride))) {
            return snow::image::Status::error(snow::image::ErrorCode::invalid_argument,
                                              "Invalid export pixel rows.");
        }
        for (uint32_t row = 0; row < count; ++row)
            std::memcpy(m_pixels + size_t(first + row) * rowBytes,
                        bytes.data() + size_t(row) * stride, rowBytes);
        return {};
    }
    snow::image::Result<void> end_frame(uint32_t) override {
        return {};
    }
    snow::image::Result<void> end() override {
        return {};
    }

  private:
    uint8_t* m_pixels;
    uint32_t m_width;
    uint32_t m_height;
};

// Propagate the existing C callback cancellation contract into snow_image's stop tokens.
class ExportStop final {
  public:
    ExportStop(void* context, SnowShotImageCodecCancelCallback cancelled)
        : m_poll([this, context, cancelled](std::stop_token stop) {
              while (!stop.stop_requested() && cancelled != nullptr) {
                  if (cancelled(context)) {
                      m_stop.request_stop();
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          }) {}
    std::stop_token token() const {
        return m_stop.get_token();
    }

  private:
    std::stop_source m_stop;
    std::jthread m_poll;
};
} // namespace

int32_t snow_shot_image_codec_resize_rgba8(const SnowShotImageCodecRgba8Source* source,
                                           uint8_t* destination, uint64_t destinationStride,
                                           uint64_t destinationSize, uint32_t outputWidth,
                                           uint32_t outputHeight, char* error, uint64_t capacity) {
    clearError(error, capacity);
    try {
        std::size_t outputStride = 0;
        std::size_t outputSize = 0;
        if (source == nullptr || destination == nullptr ||
            source->struct_size != sizeof(SnowShotImageCodecRgba8Source) ||
            source->abi_version != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION ||
            source->read_rows == nullptr || !validExportDimensions(source->width, source->height) ||
            !validExportDimensions(outputWidth, outputHeight) ||
            !checkedSize(destinationStride, &outputStride) ||
            !checkedSize(destinationSize, &outputSize)) {
            setError(error, capacity, "Invalid export raster.");
            return 0;
        }
        CallbackRasterSource raster(*source, snow::image::Format::unknown);
        snow::image::ResizeOptions options{outputWidth, outputHeight};
        options.method = snow::image::ResamplingMethod::linear;
        options.maximum_threads = 2;
        ExportStop stop(source->context, source->is_cancelled);
        snow::image::MutablePlaneView output{
            outputWidth,
            outputHeight,
            snow::image::kRgba8,
            outputStride,
            {reinterpret_cast<std::byte*>(destination), outputSize}};
        auto resized = snow::image::resize_raster_into(raster, options, output, stop.token());
        if (!resized) {
            setError(error, capacity, resized.error().message);
            return 0;
        }
        return 1;
    } catch (const std::exception& exception) {
        setError(error, capacity, exception.what());
    } catch (...) {
        setError(error, capacity, "Export resizing failed.");
    }
    return 0;
}

int32_t snow_shot_image_codec_decode_file_into(const char* path, uint8_t* destination,
                                               uint32_t width, uint32_t height, void* context,
                                               SnowShotImageCodecCancelCallback cancelled,
                                               char* error, uint64_t capacity) {
    clearError(error, capacity);
    try {
        if (!path || !destination || !validExportDimensions(width, height)) {
            setError(error, capacity, "Invalid export raster.");
            return 0;
        }
        const auto length = std::strlen(path);
        const std::u8string filename(reinterpret_cast<const char8_t*>(path), length);
        auto input = snow::image::file_input(std::filesystem::path(filename));
        if (!input) {
            setError(error, capacity, input.error().message);
            return 0;
        }
        ExportPixelSink sink(destination, width, height);
        snow::image::DecodeOptions options;
        options.output_format = snow::image::kRgba8;
        options.frame_index = 0;
        options.preserve_metadata = false;
        ExportStop stop(context, cancelled);
        auto result = service().decode_to_sink(input.value(), sink, options, stop.token());
        if (!result) {
            setError(error, capacity, result.error().message);
            return 0;
        }
        return 1;
    } catch (const std::exception& exception) {
        setError(error, capacity, exception.what());
    } catch (...) {
        setError(error, capacity, "Export preview decoding failed.");
    }
    return 0;
}

int32_t snow_shot_image_codec_export_limits(uint32_t bridgeFormat, uint32_t* width,
                                            uint32_t* height) {
    try {
        snow::image::Format format;
        if (!width || !height || !formatFromBridge(bridgeFormat, &format))
            return 0;
        const auto* info = service().encoder_info(format);
        if (!info)
            return 0;
        *width = info->limits.maximum_width;
        *height = info->limits.maximum_height;
        return 1;
    } catch (...) {
        return 0;
    }
}
