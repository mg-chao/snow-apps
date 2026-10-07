#include "codecs/jxl_codec.h"

#include <snow/memory/pixel_array.h>

#include "snow/image/processing.h"

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace snow::image::internal {
namespace {

struct DecoderDeleter final {
    void operator()(JxlDecoder* value) const noexcept {
        JxlDecoderDestroy(value);
    }
};
struct EncoderDeleter final {
    void operator()(JxlEncoder* value) const noexcept {
        JxlEncoderDestroy(value);
    }
};
struct RunnerDeleter final {
    void operator()(void* value) const noexcept {
        JxlThreadParallelRunnerDestroy(value);
    }
};

using DecoderPtr = std::unique_ptr<JxlDecoder, DecoderDeleter>;
using EncoderPtr = std::unique_ptr<JxlEncoder, EncoderDeleter>;
using RunnerPtr = std::unique_ptr<void, RunnerDeleter>;

Status jxl_error(ErrorCode code, std::string message) {
    return Status::error(code, std::move(message), "libjxl");
}

Status jxl_encoder_error(JxlEncoder* encoder) {
    switch (JxlEncoderGetError(encoder)) {
    case JXL_ENC_ERR_OOM:
        return jxl_error(ErrorCode::out_of_memory, "JPEG XL encoding ran out of memory.");
    case JXL_ENC_ERR_BAD_INPUT:
        return jxl_error(ErrorCode::encode_failed, "JPEG XL encoding rejected the input image.");
    case JXL_ENC_ERR_NOT_SUPPORTED:
        return jxl_error(ErrorCode::unsupported_feature,
                         "JPEG XL encoding does not support this image configuration.");
    case JXL_ENC_ERR_API_USAGE:
        return jxl_error(ErrorCode::internal_error,
                         "JPEG XL encoding encountered an invalid encoder configuration.");
    case JXL_ENC_ERR_JBRD:
        return jxl_error(ErrorCode::encode_failed,
                         "JPEG XL could not preserve the source JPEG bitstream.");
    case JXL_ENC_ERR_OK:
    case JXL_ENC_ERR_GENERIC:
        return jxl_error(ErrorCode::encode_failed, "JPEG XL encoding failed.");
    }
    return jxl_error(ErrorCode::encode_failed, "JPEG XL encoding failed.");
}

RunnerPtr create_runner(std::size_t maximum_threads = 32U) {
    const unsigned int available = std::thread::hardware_concurrency();
    const std::size_t threads =
        std::clamp<std::size_t>(available == 0 ? 1U : available, 1U, maximum_threads);
    return RunnerPtr(JxlThreadParallelRunnerCreate(nullptr, threads));
}

PixelFormat output_pixel_format(const JxlBasicInfo& basic, const DecodeOptions& options) {
    if (options.output_format)
        return *options.output_format;
    if (basic.exponent_bits_per_sample != 0) {
        return basic.bits_per_sample <= 16 ? kRgba16Float : kRgba32Float;
    }
    return basic.bits_per_sample <= 8 ? kRgba8 : kRgba16;
}

Result<JxlPixelFormat> jxl_pixel_format(const PixelFormat& format) {
    if (format.channels != ChannelLayout::rgba) {
        return jxl_error(ErrorCode::unsupported_feature,
                         "JPEG XL decoding currently emits RGBA pixels.");
    }
    JxlPixelFormat result{};
    result.num_channels = 4;
    result.endianness = format.little_endian ? JXL_LITTLE_ENDIAN : JXL_BIG_ENDIAN;
    result.align = 0;
    if (format.sample_type == SampleType::unsigned_integer && format.bits_per_channel == 8) {
        result.data_type = JXL_TYPE_UINT8;
    } else if (format.sample_type == SampleType::unsigned_integer &&
               format.bits_per_channel == 16) {
        result.data_type = JXL_TYPE_UINT16;
    } else if (format.sample_type == SampleType::floating_point && format.bits_per_channel == 16) {
        result.data_type = JXL_TYPE_FLOAT16;
    } else if (format.sample_type == SampleType::floating_point && format.bits_per_channel == 32) {
        result.data_type = JXL_TYPE_FLOAT;
    } else {
        return jxl_error(ErrorCode::unsupported_feature,
                         "JPEG XL supports RGBA8, RGBA16, RGBA16F, and RGBA32F output.");
    }
    return result;
}

ColorPrimaries color_primaries(JxlPrimaries value) noexcept {
    switch (value) {
    case JXL_PRIMARIES_SRGB:
        return ColorPrimaries::srgb;
    case JXL_PRIMARIES_2100:
        return ColorPrimaries::rec2020;
    case JXL_PRIMARIES_P3:
        return ColorPrimaries::display_p3;
    case JXL_PRIMARIES_CUSTOM:
        return ColorPrimaries::custom;
    }
    return ColorPrimaries::unknown;
}

TransferFunction transfer_function(JxlTransferFunction value) noexcept {
    switch (value) {
    case JXL_TRANSFER_FUNCTION_LINEAR:
        return TransferFunction::linear;
    case JXL_TRANSFER_FUNCTION_SRGB:
        return TransferFunction::srgb;
    case JXL_TRANSFER_FUNCTION_PQ:
        return TransferFunction::pq;
    case JXL_TRANSFER_FUNCTION_HLG:
        return TransferFunction::hlg;
    case JXL_TRANSFER_FUNCTION_709:
    case JXL_TRANSFER_FUNCTION_DCI:
    case JXL_TRANSFER_FUNCTION_GAMMA:
        return TransferFunction::gamma;
    case JXL_TRANSFER_FUNCTION_UNKNOWN:
        return TransferFunction::unknown;
    }
    return TransferFunction::unknown;
}

void read_structured_color(JxlDecoder* decoder, ColorEncoding& color) {
    JxlColorEncoding encoded{};
    if (JxlDecoderGetColorAsEncodedProfile(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &encoded) !=
        JXL_DEC_SUCCESS) {
        return;
    }
    color.primaries = color_primaries(encoded.primaries);
    color.transfer = transfer_function(encoded.transfer_function);
    if (color.transfer == TransferFunction::pq || color.transfer == TransferFunction::hlg) {
        color.dynamic_range = DynamicRange::high;
    }
}

Result<void> read_icc(JxlDecoder* decoder, ColorEncoding& color, const DecodeLimits& limits) {
    std::size_t size = 0;
    if (JxlDecoderGetICCProfileSize(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &size) !=
        JXL_DEC_SUCCESS) {
        return {};
    }
    if (size > limits.maximum_metadata_bytes) {
        return jxl_error(ErrorCode::limit_exceeded,
                         "JPEG XL ICC profile exceeds the configured metadata limit.");
    }
    color.icc_profile.resize(size);
    if (size != 0 &&
        JxlDecoderGetColorAsICCProfile(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL,
                                       reinterpret_cast<std::uint8_t*>(color.icc_profile.data()),
                                       size) != JXL_DEC_SUCCESS) {
        return jxl_error(ErrorCode::decode_failed, "Could not extract the JPEG XL ICC profile.");
    }
    return {};
}

std::pair<std::uint32_t, std::uint32_t> oriented_dimensions(const JxlBasicInfo& basic,
                                                            OrientationPolicy policy) {
    const bool swap =
        policy == OrientationPolicy::apply && (basic.orientation == JXL_ORIENT_TRANSPOSE ||
                                               basic.orientation == JXL_ORIENT_ROTATE_90_CW ||
                                               basic.orientation == JXL_ORIENT_ANTI_TRANSPOSE ||
                                               basic.orientation == JXL_ORIENT_ROTATE_90_CCW);
    return swap ? std::pair{basic.ysize, basic.xsize} : std::pair{basic.xsize, basic.ysize};
}

std::pair<std::uint32_t, std::uint32_t> scaled_dimensions(std::uint32_t width, std::uint32_t height,
                                                          const DecodeOptions& options) {
    if (!options.maximum_extent || *options.maximum_extent == 0 ||
        (width <= *options.maximum_extent && height <= *options.maximum_extent)) {
        return {width, height};
    }
    const double scale = std::min(static_cast<double>(*options.maximum_extent) / width,
                                  static_cast<double>(*options.maximum_extent) / height);
    return {std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(width * scale))),
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::floor(height * scale)))};
}

Result<Image> resize_nearest(const Image& source, std::uint32_t width, std::uint32_t height) {
    if (source.width() == width && source.height() == height) {
        return source;
    }
    Result<std::size_t> bytes_per_pixel = source.format().bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();
    Result<MutableImage> allocated = MutableImage::allocate(width, height, source.format());
    if (!allocated)
        return allocated.error();
    MutableImage output = std::move(allocated).value();
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t source_y =
            static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * source.height()) / height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t source_x = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * source.width()) / width);
            std::memcpy(output.pixels().data() + static_cast<std::size_t>(y) * output.row_stride() +
                            static_cast<std::size_t>(x) * bytes_per_pixel.value(),
                        source.pixels().data() +
                            static_cast<std::size_t>(source_y) * source.row_stride() +
                            static_cast<std::size_t>(source_x) * bytes_per_pixel.value(),
                        bytes_per_pixel.value());
        }
    }
    return std::move(output).freeze();
}

std::chrono::nanoseconds frame_duration(const JxlBasicInfo& basic, const JxlFrameHeader& frame) {
    if (!basic.have_animation || basic.animation.tps_numerator == 0)
        return {};
    const long double nanoseconds = static_cast<long double>(frame.duration) *
                                    basic.animation.tps_denominator * 1'000'000'000.0L /
                                    basic.animation.tps_numerator;
    return std::chrono::nanoseconds(static_cast<std::int64_t>(std::clamp<long double>(
        std::round(nanoseconds), 0.0L,
        static_cast<long double>(std::numeric_limits<std::int64_t>::max()))));
}

bool structural_box(std::string_view type) {
    return type == "JXL " || type == "ftyp" || type == "jxlc" || type == "jxlp" || type == "jxll" ||
           type == "jxli";
}

struct BoxState final {
    bool active = false;
    std::string type;
    std::vector<std::byte> data;
    std::size_t assigned = 0;
};

Result<void> release_box_buffer(JxlDecoder* decoder, BoxState& box, const DecodeLimits& limits) {
    if (!box.active || box.assigned == 0)
        return {};
    const std::size_t unused = JxlDecoderReleaseBoxBuffer(decoder);
    if (unused > box.assigned) {
        return jxl_error(ErrorCode::internal_error,
                         "libjxl returned an invalid box-buffer remainder.");
    }
    box.data.resize(box.data.size() - unused);
    box.assigned = 0;
    if (box.data.size() > limits.maximum_metadata_bytes) {
        return jxl_error(ErrorCode::limit_exceeded,
                         "JPEG XL metadata exceeds the configured byte limit.");
    }
    return {};
}

Result<void> extend_box_buffer(JxlDecoder* decoder, BoxState& box, std::uint64_t metadata_used,
                               const DecodeLimits& limits) {
    constexpr std::size_t kChunk = 64U * 1024U;
    if (metadata_used > limits.maximum_metadata_bytes ||
        box.data.size() > limits.maximum_metadata_bytes - metadata_used) {
        return jxl_error(ErrorCode::limit_exceeded,
                         "JPEG XL metadata exceeds the configured byte limit.");
    }
    const std::uint64_t remaining = limits.maximum_metadata_bytes - metadata_used - box.data.size();
    if (remaining == 0) {
        return jxl_error(ErrorCode::limit_exceeded,
                         "JPEG XL metadata exceeds the configured byte limit.");
    }
    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, remaining));
    const std::size_t start = box.data.size();
    box.data.resize(start + chunk);
    box.assigned = chunk;
    if (JxlDecoderSetBoxBuffer(decoder, reinterpret_cast<std::uint8_t*>(box.data.data() + start),
                               chunk) != JXL_DEC_SUCCESS) {
        return jxl_error(ErrorCode::decode_failed,
                         "Could not configure the JPEG XL metadata buffer.");
    }
    return {};
}

void store_box(Document& document, BoxState& box, std::uint64_t& metadata_used) {
    if (!box.active)
        return;
    metadata_used += box.data.size();
    if (box.type == "Exif") {
        document.metadata.exif = std::move(box.data);
    } else if (box.type == "xml ") {
        document.metadata.xmp = std::move(box.data);
    } else {
        document.metadata.blocks.push_back({box.type, {}, std::move(box.data), true});
    }
    box = {};
}

struct DecodeResult final {
    Document document;
    DocumentInfo info;
};

Result<DecodeResult> decode_jxl(const Input& input, const DecodeOptions& options,
                                bool decode_pixels, std::stop_token stop,
                                PixelSink* storage_sink = nullptr) {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    DecoderPtr decoder(JxlDecoderCreate(nullptr));
    RunnerPtr runner = create_runner();
    if (!decoder || !runner) {
        return jxl_error(ErrorCode::out_of_memory,
                         "Could not allocate the JPEG XL decoder or thread runner.");
    }
    if (JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get()) !=
            JXL_DEC_SUCCESS ||
        JxlDecoderSetKeepOrientation(decoder.get(),
                                     options.orientation == OrientationPolicy::preserve
                                         ? JXL_TRUE
                                         : JXL_FALSE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS) {
        return jxl_error(ErrorCode::decode_failed, "Could not configure the JPEG XL decoder.");
    }
    int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_BOX |
                 JXL_DEC_BOX_COMPLETE;
    if (decode_pixels)
        events |= JXL_DEC_FULL_IMAGE;
    if (JxlDecoderSubscribeEvents(decoder.get(), events) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(decoder.get(),
                           reinterpret_cast<const std::uint8_t*>(bytes.value().data()),
                           bytes.value().size()) != JXL_DEC_SUCCESS) {
        return jxl_error(ErrorCode::decode_failed,
                         "Could not initialize JPEG XL input processing.");
    }
    JxlDecoderCloseInput(decoder.get());

    DecodeResult result;
    result.document.format = Format::jxl;
    result.info.format = Format::jxl;
    JxlBasicInfo basic{};
    JxlFrameHeader current_header{};
    PixelFormat native_format = kRgba8;
    JxlPixelFormat pixel_format{};
    MutableImage current_image;
    std::span<std::byte> current_storage;
    std::size_t current_row_stride = 0;
    BoxState box;
    std::uint64_t metadata_used = 0;
    std::uint64_t owned_bytes = 0;
    std::uint32_t frame_index = 0;
    std::uint32_t sink_frame_index = 0;
    bool sink_frame_active = false;
    bool have_basic = false;
    bool selected_frame_found = false;

    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_ERROR) {
            return jxl_error(ErrorCode::corrupt_data, "JPEG XL decoding failed.");
        }
        if (status == JXL_DEC_NEED_MORE_INPUT) {
            return jxl_error(ErrorCode::truncated_data, "JPEG XL input ended prematurely.");
        }
        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS) {
                return jxl_error(ErrorCode::decode_failed,
                                 "Could not read JPEG XL basic image information.");
            }
            const auto dimensions = oriented_dimensions(basic, options.orientation);
            Result<void> valid_dimensions =
                validate_dimensions(dimensions.first, dimensions.second, options.limits);
            if (!valid_dimensions)
                return valid_dimensions.error();
            native_format = output_pixel_format(basic, options);
            Result<JxlPixelFormat> converted = jxl_pixel_format(native_format);
            if (!converted)
                return converted.error();
            pixel_format = converted.value();
            const auto scaled = scaled_dimensions(dimensions.first, dimensions.second, options);
            result.document.canvas_width = scaled.first;
            result.document.canvas_height = scaled.second;
            result.info.canvas_width = scaled.first;
            result.info.canvas_height = scaled.second;
            result.document.loop_count = basic.have_animation ? basic.animation.num_loops : 1;
            result.info.loop_count = result.document.loop_count;
            result.document.metadata.orientation =
                options.orientation == OrientationPolicy::preserve
                    ? static_cast<Orientation>(basic.orientation)
                    : Orientation::identity;
            result.info.metadata.orientation = result.document.metadata.orientation;
            result.document.color.source_peak_nits = basic.intensity_target;
            if (basic.exponent_bits_per_sample != 0 || basic.bits_per_sample > 8) {
                result.document.color.dynamic_range = DynamicRange::high;
            }
            result.info.color = result.document.color;
            have_basic = true;
            continue;
        }
        if (status == JXL_DEC_COLOR_ENCODING) {
            read_structured_color(decoder.get(), result.document.color);
            if (options.preserve_metadata) {
                Result<void> icc = read_icc(decoder.get(), result.document.color, options.limits);
                if (!icc)
                    return icc.error();
            }
            result.info.color = result.document.color;
            continue;
        }
        if (status == JXL_DEC_FRAME) {
            if (!have_basic ||
                JxlDecoderGetFrameHeader(decoder.get(), &current_header) != JXL_DEC_SUCCESS) {
                return jxl_error(ErrorCode::decode_failed,
                                 "Could not read the JPEG XL frame header.");
            }
            if (frame_index >= options.limits.maximum_frames) {
                return jxl_error(ErrorCode::limit_exceeded,
                                 "JPEG XL frame count exceeds the configured limit.");
            }
            FrameInfo frame_info;
            frame_info.width = result.info.canvas_width;
            frame_info.height = result.info.canvas_height;
            frame_info.duration = frame_duration(basic, current_header);
            frame_info.native_format = native_format;
            frame_info.has_alpha = basic.alpha_bits != 0;
            if (!options.frame_index || *options.frame_index == frame_index) {
                result.info.frames.push_back(frame_info);
                if (decode_pixels && storage_sink) {
                    Result<void> begun =
                        storage_sink->begin_frame(sink_frame_index, result.info.frames.back());
                    if (!begun)
                        return begun.error();
                    sink_frame_active = true;
                }
            }
            if (!decode_pixels)
                ++frame_index;
            continue;
        }
        if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            std::size_t required = 0;
            if (JxlDecoderImageOutBufferSize(decoder.get(), &pixel_format, &required) !=
                JXL_DEC_SUCCESS) {
                return jxl_error(ErrorCode::decode_failed,
                                 "Could not determine the JPEG XL frame buffer size.");
            }
            const auto raw_dimensions = oriented_dimensions(basic, options.orientation);
            Result<std::size_t> bytes_per_pixel = native_format.bytes_per_pixel();
            if (!bytes_per_pixel)
                return bytes_per_pixel.error();
            if (raw_dimensions.first >
                std::numeric_limits<std::size_t>::max() / bytes_per_pixel.value()) {
                return jxl_error(ErrorCode::limit_exceeded, "JPEG XL frame row size overflows.");
            }
            current_row_stride =
                static_cast<std::size_t>(raw_dimensions.first) * bytes_per_pixel.value();
            const auto scaled =
                scaled_dimensions(raw_dimensions.first, raw_dimensions.second, options);
            const bool selected = !options.frame_index || *options.frame_index == frame_index;
            if (storage_sink && selected && scaled == raw_dimensions) {
                current_storage =
                    storage_sink->frame_storage(sink_frame_index, current_row_stride, required);
            }
            std::byte* target = nullptr;
            if (current_storage.size() == required) {
                target = current_storage.data();
            } else {
                const std::uint64_t allocation_limit =
                    storage_sink ? options.limits.maximum_working_bytes
                                 : options.limits.maximum_owned_output_bytes;
                if (required > allocation_limit)
                    return jxl_error(ErrorCode::limit_exceeded,
                                     storage_sink
                                         ? "JPEG XL frame buffer exceeds the working-memory limit."
                                         : "JPEG XL frame buffer exceeds the owning limit.");
                Result<MutableImage> allocated = MutableImage::allocate(
                    raw_dimensions.first, raw_dimensions.second, native_format);
                if (!allocated)
                    return allocated.error();
                current_image = std::move(allocated).value();
                if (required != current_image.pixels().size())
                    return jxl_error(ErrorCode::decode_failed,
                                     "JPEG XL frame buffer size is invalid.");
                target = current_image.pixels().data();
            }
            if (JxlDecoderSetImageOutBuffer(decoder.get(), &pixel_format,
                                            reinterpret_cast<std::uint8_t*>(target),
                                            required) != JXL_DEC_SUCCESS) {
                return jxl_error(ErrorCode::decode_failed,
                                 "Could not configure the JPEG XL frame output buffer.");
            }
            continue;
        }
        if (status == JXL_DEC_FULL_IMAGE) {
            const bool selected = !options.frame_index || *options.frame_index == frame_index;
            if (selected) {
                if (current_storage.empty()) {
                    const auto scaled =
                        scaled_dimensions(current_image.width(), current_image.height(), options);
                    Image decoded_image = std::move(current_image).freeze();
                    Result<Image> scaled_image =
                        resize_nearest(decoded_image, scaled.first, scaled.second);
                    if (!scaled_image)
                        return scaled_image.error();
                    if (storage_sink) {
                        Result<void> written = storage_sink->write_rows(
                            0, scaled_image.value().height(), scaled_image.value().row_stride(),
                            scaled_image.value().pixels());
                        if (!written)
                            return written.error();
                    } else {
                        if (scaled_image.value().pixels().size() >
                                options.limits.maximum_owned_output_bytes ||
                            owned_bytes > options.limits.maximum_owned_output_bytes -
                                              scaled_image.value().pixels().size()) {
                            return jxl_error(
                                ErrorCode::limit_exceeded,
                                "JPEG XL frames exceed the configured owning decode limit.");
                        }
                        owned_bytes += scaled_image.value().pixels().size();
                        Frame frame;
                        frame.image = std::move(scaled_image).value();
                        frame.duration = frame_duration(basic, current_header);
                        frame.metadata = result.document.metadata;
                        frame.color = result.document.color;
                        result.document.frames.push_back(std::move(frame));
                    }
                }
                selected_frame_found = true;
                if (storage_sink) {
                    if (!sink_frame_active)
                        return jxl_error(ErrorCode::internal_error,
                                         "JPEG XL sink frame was not started.");
                    Result<void> ended = storage_sink->end_frame(sink_frame_index);
                    if (!ended)
                        return ended.error();
                    sink_frame_active = false;
                    ++sink_frame_index;
                }
            }
            current_image = {};
            current_storage = {};
            ++frame_index;
            continue;
        }
        if (status == JXL_DEC_BOX) {
            Result<void> released = release_box_buffer(decoder.get(), box, options.limits);
            if (!released)
                return released.error();
            store_box(result.document, box, metadata_used);
            JxlBoxType type{};
            if (JxlDecoderGetBoxType(decoder.get(), type, JXL_TRUE) != JXL_DEC_SUCCESS) {
                return jxl_error(ErrorCode::decode_failed,
                                 "Could not read a JPEG XL metadata box type.");
            }
            const std::string box_type(type, type + 4);
            if (options.preserve_metadata && !structural_box(box_type)) {
                box.active = true;
                box.type = box_type;
                Result<void> extended =
                    extend_box_buffer(decoder.get(), box, metadata_used, options.limits);
                if (!extended)
                    return extended.error();
            }
            continue;
        }
        if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            Result<void> released = release_box_buffer(decoder.get(), box, options.limits);
            if (!released)
                return released.error();
            Result<void> extended =
                extend_box_buffer(decoder.get(), box, metadata_used, options.limits);
            if (!extended)
                return extended.error();
            continue;
        }
        if (status == JXL_DEC_BOX_COMPLETE) {
            Result<void> released = release_box_buffer(decoder.get(), box, options.limits);
            if (!released)
                return released.error();
            store_box(result.document, box, metadata_used);
            continue;
        }
        if (status == JXL_DEC_SUCCESS) {
            Result<void> released = release_box_buffer(decoder.get(), box, options.limits);
            if (!released)
                return released.error();
            store_box(result.document, box, metadata_used);
            break;
        }
        if (status == JXL_DEC_NEED_PREVIEW_OUT_BUFFER || status == JXL_DEC_JPEG_NEED_MORE_OUTPUT) {
            return jxl_error(ErrorCode::unsupported_feature,
                             "Unexpected JPEG XL auxiliary output request.");
        }
    }
    result.info.metadata = result.document.metadata;
    result.info.color = result.document.color;
    if (decode_pixels && options.frame_index && !selected_frame_found) {
        return jxl_error(ErrorCode::invalid_argument,
                         "Requested JPEG XL frame index is out of range.");
    }
    if (decode_pixels && !storage_sink && result.document.frames.empty()) {
        return jxl_error(ErrorCode::corrupt_data, "JPEG XL image has no displayed frames.");
    }
    if (sink_frame_active)
        return jxl_error(ErrorCode::truncated_data, "JPEG XL ended with an incomplete sink frame.");
    return result;
}

struct NormalizedFrame final {
    JxlPixelFormat format{};
    PixelFormat snow_format;
    std::span<const std::byte> pixels;
    snow::memory::PixelArray<std::byte> storage;
};

Result<NormalizedFrame> normalize_frame(const ImageView& view) {
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    NormalizedFrame normalized;
    normalized.format.num_channels = 4;
    normalized.format.align = 0;
    normalized.format.endianness = view.format.little_endian ? JXL_LITTLE_ENDIAN : JXL_BIG_ENDIAN;
    if (view.format.sample_type == SampleType::unsigned_integer &&
        view.format.bits_per_channel == 16 && view.format.channels == ChannelLayout::rgba) {
        normalized.format.data_type = JXL_TYPE_UINT16;
        normalized.snow_format = view.format;
    } else if (view.format.sample_type == SampleType::floating_point &&
               view.format.bits_per_channel == 16 && view.format.channels == ChannelLayout::rgba) {
        normalized.format.data_type = JXL_TYPE_FLOAT16;
        normalized.snow_format = view.format;
    } else if (view.format.sample_type == SampleType::floating_point &&
               view.format.bits_per_channel == 32 && view.format.channels == ChannelLayout::rgba) {
        normalized.format.data_type = JXL_TYPE_FLOAT;
        normalized.snow_format = view.format;
    } else if (view.format.sample_type == SampleType::unsigned_integer &&
               view.format.bits_per_channel == 8) {
        normalized.format.data_type = JXL_TYPE_UINT8;
        // Single-byte samples have no byte order. Keep every packed layout
        // consistent with the direct RGB8 path when comparing animation frames.
        normalized.format.endianness = JXL_NATIVE_ENDIAN;
        normalized.snow_format = kRgba8;
    } else {
        return jxl_error(
            ErrorCode::unsupported_feature,
            "JPEG XL encoding supports packed 8-bit images and RGBA16/16F/32F images.");
    }
    Result<std::size_t> output_bpp = normalized.snow_format.bytes_per_pixel();
    if (!output_bpp)
        return output_bpp.error();
    const std::size_t output_row = static_cast<std::size_t>(view.width) * output_bpp.value();
    const bool direct = view.format == normalized.snow_format && view.row_stride == output_row;
    if (direct) {
        normalized.pixels = view.pixels.first(output_row * view.height);
        return normalized;
    }
    if (view.format.bits_per_channel != 8) {
        return jxl_error(ErrorCode::unsupported_feature,
                         "JPEG XL high-bit-depth encoding requires tightly packed RGBA pixels.");
    }
    normalized.storage.resize(output_row * view.height);
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const std::byte* source =
            view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride;
        std::byte* destination =
            normalized.storage.data() + static_cast<std::size_t>(y) * output_row;
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t output = static_cast<std::size_t>(x) * 4U;
            switch (view.format.channels) {
            case ChannelLayout::gray: {
                destination[output] = source[x];
                destination[output + 1] = source[x];
                destination[output + 2] = source[x];
                destination[output + 3] = std::byte{0xFF};
                break;
            }
            case ChannelLayout::gray_alpha: {
                const std::size_t input = static_cast<std::size_t>(x) * 2U;
                destination[output] = source[input];
                destination[output + 1] = source[input];
                destination[output + 2] = source[input];
                destination[output + 3] = source[input + 1];
                break;
            }
            case ChannelLayout::rgb:
            case ChannelLayout::bgr: {
                const std::size_t input = static_cast<std::size_t>(x) * 3U;
                const bool bgr = view.format.channels == ChannelLayout::bgr;
                destination[output] = source[input + (bgr ? 2U : 0U)];
                destination[output + 1] = source[input + 1];
                destination[output + 2] = source[input + (bgr ? 0U : 2U)];
                destination[output + 3] = std::byte{0xFF};
                break;
            }
            case ChannelLayout::rgba:
            case ChannelLayout::bgra: {
                const std::size_t input = static_cast<std::size_t>(x) * 4U;
                const bool bgra = view.format.channels == ChannelLayout::bgra;
                destination[output] = source[input + (bgra ? 2U : 0U)];
                destination[output + 1] = source[input + 1];
                destination[output + 2] = source[input + (bgra ? 0U : 2U)];
                destination[output + 3] = source[input + 3];
                break;
            }
            case ChannelLayout::cmyk:
            case ChannelLayout::indexed:
                return jxl_error(ErrorCode::unsupported_feature,
                                 "JPEG XL encoding does not accept CMYK or indexed pixels.");
            }
        }
    }
    normalized.pixels = normalized.storage;
    return normalized;
}

Result<NormalizedFrame> normalize_frame_without_alpha(const ImageView& view) {
    if (view.format.sample_type == SampleType::unsigned_integer &&
        view.format.bits_per_channel == 8 && view.format.channels == ChannelLayout::rgb) {
        Result<void> valid = view.validate();
        if (!valid)
            return valid.error();
        NormalizedFrame normalized;
        normalized.format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        normalized.snow_format = kRgb8;
        const std::size_t row_bytes = static_cast<std::size_t>(view.width) * 3U;
        if (view.row_stride == row_bytes) {
            normalized.pixels = view.pixels.first(row_bytes * view.height);
        } else {
            normalized.storage.resize(row_bytes * view.height);
            for (std::uint32_t y = 0; y < view.height; ++y)
                std::memcpy(normalized.storage.data() + static_cast<std::size_t>(y) * row_bytes,
                            view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride,
                            row_bytes);
            normalized.pixels = normalized.storage;
        }
        return normalized;
    }
    Result<NormalizedFrame> source = normalize_frame(view);
    if (!source)
        return source.error();
    NormalizedFrame normalized = std::move(source).value();
    const std::size_t sample_bytes = normalized.format.data_type == JXL_TYPE_UINT8   ? 1U
                                     : normalized.format.data_type == JXL_TYPE_FLOAT ? 4U
                                                                                     : 2U;
    const std::size_t pixel_count = static_cast<std::size_t>(view.width) * view.height;
    try {
        snow::memory::PixelArray<std::byte> rgb(pixel_count * 3U * sample_bytes);
        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
            std::memcpy(rgb.data() + pixel * 3U * sample_bytes,
                        normalized.pixels.data() + pixel * 4U * sample_bytes, 3U * sample_bytes);
        }
        normalized.storage = std::move(rgb);
    } catch (const std::bad_alloc&) {
        return jxl_error(ErrorCode::out_of_memory,
                         "Could not allocate opaque JPEG XL input pixels.");
    }
    normalized.pixels = normalized.storage;
    normalized.format.num_channels = 3;
    normalized.snow_format.channels = ChannelLayout::rgb;
    normalized.snow_format.alpha = AlphaMode::none;
    return normalized;
}

struct AnimationClock final {
    std::uint64_t tick_nanoseconds = 1'000'000;
    std::uint32_t numerator = 1'000;
    std::uint32_t denominator = 1;
};

AnimationClock animation_clock(const Document& document) {
    std::uint64_t divisor = 0;
    std::uint64_t maximum = 0;
    for (const Frame& frame : document.frames) {
        const std::uint64_t duration =
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, frame.duration.count()));
        if (duration != 0)
            divisor = divisor == 0 ? duration : std::gcd(divisor, duration);
        maximum = std::max(maximum, duration);
    }
    if (divisor == 0)
        divisor = 1'000'000;
    if (maximum / divisor > std::numeric_limits<std::uint32_t>::max()) {
        divisor = maximum / std::numeric_limits<std::uint32_t>::max() + 1;
    }
    const std::uint64_t common = std::gcd<std::uint64_t>(divisor, 1'000'000'000ULL);
    AnimationClock clock;
    clock.tick_nanoseconds = divisor;
    clock.numerator = static_cast<std::uint32_t>(1'000'000'000ULL / common);
    clock.denominator = static_cast<std::uint32_t>(divisor / common);
    return clock;
}

Result<void> add_box(JxlEncoder* encoder, std::string_view type, std::span<const std::byte> data,
                     bool compress) {
    if (type.size() != 4) {
        return jxl_error(ErrorCode::unsupported_feature,
                         "JPEG XL metadata box types must contain four characters.");
    }
    JxlBoxType box_type{};
    std::memcpy(box_type, type.data(), 4);
    if (JxlEncoderAddBox(encoder, box_type, reinterpret_cast<const std::uint8_t*>(data.data()),
                         data.size(), compress ? JXL_TRUE : JXL_FALSE) != JXL_ENC_SUCCESS) {
        return jxl_error(ErrorCode::encode_failed, "Could not add a JPEG XL metadata box.");
    }
    return {};
}

JxlColorEncoding structured_color(const ColorEncoding& source) {
    JxlColorEncoding result{};
    JxlColorEncodingSetToSRGB(&result, JXL_FALSE);
    if (source.primaries == ColorPrimaries::display_p3) {
        result.primaries = JXL_PRIMARIES_P3;
        result.white_point = JXL_WHITE_POINT_D65;
    } else if (source.primaries == ColorPrimaries::rec2020) {
        result.primaries = JXL_PRIMARIES_2100;
        result.white_point = JXL_WHITE_POINT_D65;
    }
    if (source.transfer == TransferFunction::linear) {
        result.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
    } else if (source.transfer == TransferFunction::pq) {
        result.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
    } else if (source.transfer == TransferFunction::hlg) {
        result.transfer_function = JXL_TRANSFER_FUNCTION_HLG;
    }
    return result;
}

} // namespace

CodecCapability JxlCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::animation | CodecCapability::multiple_images |
           CodecCapability::streaming_decode | CodecCapability::metadata_decode |
           CodecCapability::hdr;
}

int JxlCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 2 && header[0] == std::byte{0xFF} && header[1] == std::byte{0x0A}) {
        return 100;
    }
    constexpr std::array<std::byte, 12> signature{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0C},
        std::byte{0x4A}, std::byte{0x58}, std::byte{0x4C}, std::byte{0x20},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x87}, std::byte{0x0A}};
    if (header.size() >= signature.size() &&
        std::equal(signature.begin(), signature.end(), header.begin())) {
        return 100;
    }
    return format_from_extension(name_hint) == Format::jxl ? 10 : 0;
}

Result<DocumentInfo> JxlCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    Result<DecodeResult> decoded = decode_jxl(input, options, false, stop);
    if (!decoded)
        return decoded.error();
    return std::move(decoded).value().info;
}

Result<Document> JxlCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    Result<DecodeResult> decoded = decode_jxl(input, options, true, stop);
    if (!decoded)
        return decoded.error();
    return std::move(decoded).value().document;
}

Result<void> JxlCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                      const DecodeOptions& options, std::stop_token stop) const {
    Result<DocumentInfo> info = inspect(input, options, stop);
    if (!info)
        return info.error();
    Result<void> status = sink.begin(info.value());
    if (!status)
        return status;
    Result<DecodeResult> decoded = decode_jxl(input, options, true, stop, &sink);
    if (!decoded)
        return decoded.error();
    return sink.end();
}

Result<EncodedArtifactReceipt> JxlCodec::encode_to_sink(const Document& document,
                                                        const Output& output,
                                                        const EncodeOptions& options,
                                                        std::stop_token stop) const {
    if (document.frames.empty()) {
        return jxl_error(ErrorCode::invalid_argument,
                         "JPEG XL encoding requires at least one frame.");
    }
    const std::uint32_t canvas_width =
        document.canvas_width == 0 ? document.frames.front().image.width() : document.canvas_width;
    const std::uint32_t canvas_height = document.canvas_height == 0
                                            ? document.frames.front().image.height()
                                            : document.canvas_height;
    Result<void> canvas = validate_dimensions(canvas_width, canvas_height, DecodeLimits{});
    if (!canvas)
        return canvas.error();
    bool encode_alpha = false;
    if (options.verified_alpha_content) {
        encode_alpha = *options.verified_alpha_content == AlphaContent::non_opaque;
    } else {
        for (const Frame& frame : document.frames) {
            Result<AlphaContent> content = classify_alpha(frame.image, stop);
            if (!content)
                return content.error();
            if (content.value() == AlphaContent::non_opaque) {
                encode_alpha = true;
                break;
            }
        }
    }
    EncoderPtr encoder(JxlEncoderCreate(nullptr));
    // libjxl's encoder working set grows sharply with its thread pool.
    RunnerPtr runner = create_runner(8U);
    if (!encoder || !runner) {
        return jxl_error(ErrorCode::out_of_memory,
                         "Could not allocate the JPEG XL encoder or thread runner.");
    }
    if (JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get()) !=
        JXL_ENC_SUCCESS) {
        return jxl_error(ErrorCode::encode_failed,
                         "Could not configure the JPEG XL encoder thread runner.");
    }
    Result<NormalizedFrame> first =
        encode_alpha ? normalize_frame(document.frames.front().image.view())
                     : normalize_frame_without_alpha(document.frames.front().image.view());
    if (!first)
        return first.error();
    const JxlDataType expected_data_type = first.value().format.data_type;
    const JxlEndianness expected_endianness = first.value().format.endianness;
    const bool animation = document.frames.size() > 1;
    const AnimationClock clock = animation_clock(document);
    JxlBasicInfo basic{};
    JxlEncoderInitBasicInfo(&basic);
    basic.xsize = canvas_width;
    basic.ysize = canvas_height;
    basic.num_color_channels = 3;
    basic.num_extra_channels = encode_alpha ? 1 : 0;
    basic.uses_original_profile = options.lossless ? JXL_TRUE : JXL_FALSE;
    basic.alpha_premultiplied =
        encode_alpha && first.value().snow_format.alpha == AlphaMode::premultiplied ? JXL_TRUE
                                                                                    : JXL_FALSE;
    basic.orientation = static_cast<JxlOrientation>(document.metadata.orientation);
    basic.have_animation = animation ? JXL_TRUE : JXL_FALSE;
    if (first.value().format.data_type == JXL_TYPE_UINT8) {
        basic.bits_per_sample = 8;
        basic.alpha_bits = encode_alpha ? 8 : 0;
    } else if (first.value().format.data_type == JXL_TYPE_UINT16) {
        basic.bits_per_sample = 16;
        basic.alpha_bits = encode_alpha ? 16 : 0;
    } else if (first.value().format.data_type == JXL_TYPE_FLOAT16) {
        basic.bits_per_sample = 16;
        basic.exponent_bits_per_sample = 5;
        basic.alpha_bits = encode_alpha ? 16 : 0;
        basic.alpha_exponent_bits = encode_alpha ? 5 : 0;
    } else {
        basic.bits_per_sample = 32;
        basic.exponent_bits_per_sample = 8;
        basic.alpha_bits = encode_alpha ? 32 : 0;
        basic.alpha_exponent_bits = encode_alpha ? 8 : 0;
    }
    basic.intensity_target = document.color.source_peak_nits;
    if (animation) {
        basic.animation.tps_numerator = clock.numerator;
        basic.animation.tps_denominator = clock.denominator;
        basic.animation.num_loops = document.loop_count;
    }
    if (JxlEncoderSetBasicInfo(encoder.get(), &basic) != JXL_ENC_SUCCESS) {
        return jxl_error(ErrorCode::encode_failed,
                         "Could not configure JPEG XL basic image information.");
    }
    if (!document.color.icc_profile.empty()) {
        if (JxlEncoderSetICCProfile(
                encoder.get(),
                reinterpret_cast<const std::uint8_t*>(document.color.icc_profile.data()),
                document.color.icc_profile.size()) != JXL_ENC_SUCCESS) {
            return jxl_error(ErrorCode::encode_failed, "Could not assign the JPEG XL ICC profile.");
        }
    } else {
        const JxlColorEncoding color = structured_color(document.color);
        if (JxlEncoderSetColorEncoding(encoder.get(), &color) != JXL_ENC_SUCCESS) {
            return jxl_error(ErrorCode::encode_failed,
                             "Could not assign the JPEG XL structured color profile.");
        }
    }
    const bool write_boxes = options.preserve_metadata &&
                             (!document.metadata.exif.empty() || !document.metadata.xmp.empty() ||
                              !document.metadata.blocks.empty());
    if (write_boxes && JxlEncoderUseBoxes(encoder.get()) != JXL_ENC_SUCCESS) {
        return jxl_error(ErrorCode::encode_failed,
                         "Could not enable the JPEG XL metadata container.");
    }

    for (std::size_t index = 0; index < document.frames.size(); ++index) {
        if (stop.stop_requested())
            return cancelled_status();
        const Frame& frame = document.frames[index];
        Result<NormalizedFrame> normalized =
            // The monotonic loop reaches index zero exactly once.
            // NOLINTNEXTLINE(bugprone-use-after-move)
            index == 0     ? std::move(first)
            : encode_alpha ? normalize_frame(frame.image.view())
                           : normalize_frame_without_alpha(frame.image.view());
        if (!normalized)
            return normalized.error();
        if (normalized.value().format.data_type != expected_data_type ||
            normalized.value().format.endianness != expected_endianness) {
            return jxl_error(ErrorCode::unsupported_feature,
                             "All JPEG XL frames must use the same sample type and endianness.");
        }
        if (frame.x > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            frame.y > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            frame.x + frame.image.width() > canvas_width ||
            frame.y + frame.image.height() > canvas_height) {
            return jxl_error(ErrorCode::invalid_argument,
                             "JPEG XL frame rectangle lies outside the canvas.");
        }
        if (frame.disposal != FrameDisposal::keep) {
            return jxl_error(ErrorCode::unsupported_feature,
                             "JPEG XL encoding does not directly represent GIF disposal modes.");
        }
        JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
        if (!settings ||
            JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT,
                                             std::clamp(options.effort, 1, 10)) !=
                JXL_ENC_SUCCESS ||
            JxlEncoderSetFrameLossless(settings, options.lossless ? JXL_TRUE : JXL_FALSE) !=
                JXL_ENC_SUCCESS) {
            return jxl_error(ErrorCode::encode_failed,
                             "Could not configure JPEG XL frame encoding options.");
        }
        if (!options.lossless &&
            JxlEncoderSetFrameDistance(settings, JxlEncoderDistanceFromQuality(static_cast<float>(
                                                     std::clamp(options.quality, 0, 100)))) !=
                JXL_ENC_SUCCESS) {
            return jxl_error(ErrorCode::encode_failed,
                             "Could not configure JPEG XL frame quality.");
        }
        if (options.progressive) {
            static_cast<void>(
                JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_RESPONSIVE, 1));
            static_cast<void>(JxlEncoderFrameSettingsSetOption(
                settings, JXL_ENC_FRAME_SETTING_PROGRESSIVE_AC, 1));
        }
        JxlFrameHeader header{};
        JxlEncoderInitFrameHeader(&header);
        if (animation) {
            const std::uint64_t duration =
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, frame.duration.count()));
            header.duration = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max(),
                (duration + clock.tick_nanoseconds / 2U) / clock.tick_nanoseconds));
        }
        if (frame.x != 0 || frame.y != 0 || frame.image.width() != canvas_width ||
            frame.image.height() != canvas_height) {
            header.layer_info.have_crop = JXL_TRUE;
            header.layer_info.crop_x0 = static_cast<std::int32_t>(frame.x);
            header.layer_info.crop_y0 = static_cast<std::int32_t>(frame.y);
            header.layer_info.xsize = frame.image.width();
            header.layer_info.ysize = frame.image.height();
        }
        header.layer_info.blend_info.blendmode =
            frame.blend == FrameBlend::over ? JXL_BLEND_BLEND : JXL_BLEND_REPLACE;
        if (JxlEncoderSetFrameHeader(settings, &header) != JXL_ENC_SUCCESS ||
            JxlEncoderAddImageFrame(
                settings, &normalized.value().format,
                reinterpret_cast<const std::uint8_t*>(normalized.value().pixels.data()),
                normalized.value().pixels.size()) != JXL_ENC_SUCCESS) {
            return jxl_error(ErrorCode::encode_failed, "Could not add a JPEG XL image frame.");
        }
    }

    if (write_boxes) {
        if (!document.metadata.exif.empty()) {
            Result<void> status = add_box(encoder.get(), "Exif", document.metadata.exif, true);
            if (!status)
                return status.error();
        }
        if (!document.metadata.xmp.empty()) {
            Result<void> status = add_box(encoder.get(), "xml ", document.metadata.xmp, true);
            if (!status)
                return status.error();
        }
        for (const MetadataBlock& block : document.metadata.blocks) {
            Result<void> status =
                add_box(encoder.get(), block.type, block.data, !block.safe_to_copy);
            if (!status)
                return status.error();
        }
    }
    JxlEncoderCloseInput(encoder.get());
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        std::uint8_t* next = buffer.data();
        std::size_t available = buffer.size();
        const JxlEncoderStatus status = JxlEncoderProcessOutput(encoder.get(), &next, &available);
        const std::size_t produced = buffer.size() - available;
        if (produced != 0) {
            Result<void> written =
                output.sink->write(std::as_bytes(std::span(buffer.data(), produced)));
            if (!written)
                return written.error();
        }
        if (status == JXL_ENC_SUCCESS)
            break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT) {
            return jxl_encoder_error(encoder.get());
        }
    }
    return receipt_for_document(document, format());
}

Result<EncodedArtifactReceipt> JxlCodec::encode_raster_to_sink(const RasterSource& source,
                                                               const Output& output,
                                                               const EncodeOptions& options,
                                                               std::stop_token stop) const {
    const DocumentDescriptor& descriptor = source.descriptor();
    if (raster_encode_route(descriptor, options) != RasterEncodeRoute::native)
        return Codec::encode_raster_to_sink(source, output, options, stop);
    Result<void> descriptor_status = descriptor.validate();
    if (!descriptor_status)
        return descriptor_status.error();

    const RasterFrameDescriptor& source_frame = descriptor.frames.front();
    const std::size_t row_bytes = static_cast<std::size_t>(source_frame.width) * 4U;
    if (source_frame.height > std::numeric_limits<std::size_t>::max() / row_bytes) {
        return jxl_error(ErrorCode::limit_exceeded,
                         "JPEG XL raster input size overflows addressable memory.");
    }
    try {
        const bool opaque = options.verified_alpha_content == AlphaContent::opaque;
        const PixelFormat input_format = opaque ? kRgb8 : kRgba8;
        Result<MutableImage> allocated =
            MutableImage::allocate(source_frame.width, source_frame.height, input_format);
        if (!allocated)
            return allocated.error();
        MutableImage pixels = std::move(allocated).value();
        constexpr std::uint32_t kRowsPerRead = 64;
        snow::memory::PixelArray<std::byte> rgba_rows;
        if (opaque)
            rgba_rows.resize(row_bytes * std::min(kRowsPerRead, source_frame.height));
        for (std::uint32_t first = 0; first < source_frame.height; first += kRowsPerRead) {
            if (stop.stop_requested())
                return cancelled_status();
            const std::uint32_t count = std::min(kRowsPerRead, source_frame.height - first);
            const std::size_t offset = static_cast<std::size_t>(first) * pixels.row_stride();
            Result<void> read = source.read_rows(
                0, 0, first, count, row_bytes,
                opaque
                    ? std::span(rgba_rows).first(static_cast<std::size_t>(count) * row_bytes)
                    : pixels.pixels().subspan(offset, static_cast<std::size_t>(count) * row_bytes),
                stop);
            if (!read)
                return read.error();
            if (opaque) {
                for (std::uint32_t row = 0; row < count; ++row) {
                    const std::byte* input =
                        rgba_rows.data() + static_cast<std::size_t>(row) * row_bytes;
                    std::byte* destination = pixels.pixels().data() + offset +
                                             static_cast<std::size_t>(row) * pixels.row_stride();
                    for (std::uint32_t x = 0; x < source_frame.width; ++x)
                        std::memcpy(destination + static_cast<std::size_t>(x) * 3U,
                                    input + static_cast<std::size_t>(x) * 4U, 3U);
                }
            }
        }
        Document document;
        document.format = descriptor.format;
        document.canvas_width = descriptor.canvas_width;
        document.canvas_height = descriptor.canvas_height;
        document.loop_count = descriptor.loop_count;
        document.metadata = descriptor.metadata;
        document.color = descriptor.color;
        Frame frame;
        frame.image = std::move(pixels).freeze();
        frame.x = source_frame.x;
        frame.y = source_frame.y;
        frame.duration = source_frame.duration;
        frame.blend = source_frame.blend;
        frame.disposal = source_frame.disposal;
        frame.metadata = source_frame.metadata;
        frame.color = source_frame.color;
        frame.cursor_hotspot = source_frame.cursor_hotspot;
        document.frames.push_back(std::move(frame));
        return encode_to_sink(document, output, options, stop);
    } catch (const std::bad_alloc&) {
        return jxl_error(ErrorCode::out_of_memory, "Could not allocate the JPEG XL input frame.");
    }
}

RasterEncodeRoute
JxlCodec::raster_encode_route(const DocumentDescriptor& descriptor,
                              const EncodeOptions& normalized_options) const noexcept {
    if (!normalized_options.verified_alpha_content || descriptor.kind != DocumentKind::raster ||
        descriptor.frames.size() != 1U)
        return RasterEncodeRoute::materialized;
    const RasterFrameDescriptor& frame = descriptor.frames.front();
    if (frame.x != 0 || frame.y != 0 || frame.width != descriptor.canvas_width ||
        frame.height != descriptor.canvas_height || frame.layout.planes.size() != 1U)
        return RasterEncodeRoute::materialized;
    const PlaneDescriptor& plane = frame.layout.planes.front();
    return plane.semantic == PlaneSemantic::packed && plane.width == frame.width &&
                   plane.height == frame.height && plane.format == kRgba8
               ? RasterEncodeRoute::native
               : RasterEncodeRoute::materialized;
}

} // namespace snow::image::internal
