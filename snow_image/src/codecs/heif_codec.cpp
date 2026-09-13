#include "codecs/heif_codec.h"
#include "planar_raster_io.h"

#include "snow/image/processing.h"

#include <libheif/heif.h>
#include <libheif/heif_properties.h>
#include <libheif/heif_sequences.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snow::image::internal {
namespace {

struct ContextDeleter final {
    void operator()(heif_context* value) const noexcept {
        heif_context_free(value);
    }
};
struct HandleDeleter final {
    void operator()(const heif_image_handle* value) const noexcept {
        heif_image_handle_release(value);
    }
};
struct ImageDeleter final {
    void operator()(const heif_image* value) const noexcept {
        heif_image_release(value);
    }
};
struct EncoderDeleter final {
    void operator()(heif_encoder* value) const noexcept {
        heif_encoder_release(value);
    }
};
struct DecodeOptionsDeleter final {
    void operator()(heif_decoding_options* value) const noexcept {
        heif_decoding_options_free(value);
    }
};
struct EncodeOptionsDeleter final {
    void operator()(heif_encoding_options* value) const noexcept {
        heif_encoding_options_free(value);
    }
};
struct NclxDeleter final {
    void operator()(heif_color_profile_nclx* value) const noexcept {
        heif_nclx_color_profile_free(value);
    }
};
struct TrackDeleter final {
    void operator()(heif_track* value) const noexcept {
        heif_track_release(value);
    }
};
struct TrackOptionsDeleter final {
    void operator()(heif_track_options* value) const noexcept {
        heif_track_options_release(value);
    }
};
struct SequenceOptionsDeleter final {
    void operator()(heif_sequence_encoding_options* value) const noexcept {
        heif_sequence_encoding_options_release(value);
    }
};

using ContextPtr = std::unique_ptr<heif_context, ContextDeleter>;
using HandlePtr = std::unique_ptr<const heif_image_handle, HandleDeleter>;
using ImagePtr = std::unique_ptr<const heif_image, ImageDeleter>;
using MutableImagePtr = std::unique_ptr<heif_image, ImageDeleter>;
using EncoderPtr = std::unique_ptr<heif_encoder, EncoderDeleter>;
using DecodeOptionsPtr = std::unique_ptr<heif_decoding_options, DecodeOptionsDeleter>;
using EncodeOptionsPtr = std::unique_ptr<heif_encoding_options, EncodeOptionsDeleter>;
using NclxPtr = std::unique_ptr<heif_color_profile_nclx, NclxDeleter>;
using TrackPtr = std::unique_ptr<heif_track, TrackDeleter>;
using TrackOptionsPtr = std::unique_ptr<heif_track_options, TrackOptionsDeleter>;
using SequenceOptionsPtr = std::unique_ptr<heif_sequence_encoding_options, SequenceOptionsDeleter>;

Status heif_status(heif_error error, ErrorCode fallback, std::string_view operation) {
    ErrorCode code = fallback;
    switch (error.code) {
    case heif_error_Ok:
        return {};
    case heif_error_Invalid_input:
        code = ErrorCode::corrupt_data;
        break;
    case heif_error_Unsupported_filetype:
        code = ErrorCode::unsupported_format;
        break;
    case heif_error_Unsupported_feature:
        code = ErrorCode::unsupported_feature;
        break;
    case heif_error_Memory_allocation_error:
        code = ErrorCode::out_of_memory;
        break;
    case heif_error_Canceled:
        code = ErrorCode::cancelled;
        break;
    case heif_error_Input_does_not_exist:
    case heif_error_Decoder_plugin_error:
    case heif_error_Encoder_plugin_error:
    case heif_error_Encoding_error:
    case heif_error_Color_profile_does_not_exist:
    case heif_error_Plugin_loading_error:
    case heif_error_End_of_sequence:
    case heif_error_Usage_error:
        break;
    }
    std::string message(operation);
    message += " failed";
    if (error.message && *error.message) {
        message += ": ";
        message += error.message;
    }
    message += ".";
    return Status::error(code, std::move(message), "libheif",
                         static_cast<std::int64_t>(error.subcode));
}

template <typename T> T clamp_unsigned(std::uint64_t value) {
    return static_cast<T>(std::min<std::uint64_t>(value, std::numeric_limits<T>::max()));
}

struct ParsedHeif final {
    std::vector<std::byte> bytes;
    ContextPtr context;
};

Result<ParsedHeif> parse_heif(const Input& input, const DecodeOptions& options,
                              std::stop_token stop) {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    ContextPtr context(heif_context_alloc());
    if (!context) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a HEIF context.",
                             "libheif");
    }
    heif_security_limits* security = heif_context_get_security_limits(context.get());
    if (security) {
        security->max_image_size_pixels = options.limits.maximum_pixels;
        security->max_items = std::max<std::uint32_t>(
            64U, clamp_unsigned<std::uint32_t>(
                     static_cast<std::uint64_t>(options.limits.maximum_frames) * 8U));
        security->max_color_profile_size =
            clamp_unsigned<std::uint32_t>(options.limits.maximum_metadata_bytes);
        security->max_memory_block_size = options.limits.maximum_working_bytes;
        security->max_total_memory = options.limits.maximum_working_bytes;
        security->max_sequence_frames = options.limits.maximum_frames;
    }
    heif_error error = heif_context_read_from_memory_without_copy(
        context.get(), bytes.value().data(), bytes.value().size(), nullptr);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::corrupt_data, "HEIF parsing");
    }
    return ParsedHeif{std::move(bytes).value(), std::move(context)};
}

Result<ContextPtr> reopen_heif(const ParsedHeif& parsed, const DecodeOptions& options) {
    ContextPtr context(heif_context_alloc());
    if (!context) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a HEIF context.",
                             "libheif");
    }
    heif_security_limits* security = heif_context_get_security_limits(context.get());
    if (security) {
        security->max_image_size_pixels = options.limits.maximum_pixels;
        security->max_items = std::max<std::uint32_t>(
            64U, clamp_unsigned<std::uint32_t>(
                     static_cast<std::uint64_t>(options.limits.maximum_frames) * 8U));
        security->max_color_profile_size =
            clamp_unsigned<std::uint32_t>(options.limits.maximum_metadata_bytes);
        security->max_memory_block_size = options.limits.maximum_working_bytes;
        security->max_total_memory = options.limits.maximum_working_bytes;
        security->max_sequence_frames = options.limits.maximum_frames;
    }
    const heif_error error = heif_context_read_from_memory_without_copy(
        context.get(), parsed.bytes.data(), parsed.bytes.size(), nullptr);
    if (error.code != heif_error_Ok)
        return heif_status(error, ErrorCode::corrupt_data, "HEIF parsing");
    return context;
}

ColorPrimaries color_primaries(heif_color_primaries value) noexcept {
    switch (value) {
    case heif_color_primaries_ITU_R_BT_709_5:
        return ColorPrimaries::srgb;
    case heif_color_primaries_ITU_R_BT_2020_2_and_2100_0:
        return ColorPrimaries::rec2020;
    case heif_color_primaries_SMPTE_EG_432_1:
        return ColorPrimaries::display_p3;
    case heif_color_primaries_unspecified:
    case heif_color_primaries_ITU_R_BT_470_6_System_M:
    case heif_color_primaries_ITU_R_BT_470_6_System_B_G:
    case heif_color_primaries_ITU_R_BT_601_6:
    case heif_color_primaries_SMPTE_240M:
    case heif_color_primaries_generic_film:
    case heif_color_primaries_SMPTE_ST_428_1:
    case heif_color_primaries_SMPTE_RP_431_2:
    case heif_color_primaries_EBU_Tech_3213_E:
        return ColorPrimaries::custom;
    }
    return ColorPrimaries::unknown;
}

TransferFunction transfer_function(heif_transfer_characteristics value) noexcept {
    switch (value) {
    case heif_transfer_characteristic_linear:
        return TransferFunction::linear;
    case heif_transfer_characteristic_IEC_61966_2_1:
        return TransferFunction::srgb;
    case heif_transfer_characteristic_ITU_R_BT_2100_0_PQ:
        return TransferFunction::pq;
    case heif_transfer_characteristic_ITU_R_BT_2100_0_HLG:
        return TransferFunction::hlg;
    case heif_transfer_characteristic_unspecified:
        return TransferFunction::unknown;
    case heif_transfer_characteristic_ITU_R_BT_709_5:
    case heif_transfer_characteristic_ITU_R_BT_470_6_System_M:
    case heif_transfer_characteristic_ITU_R_BT_470_6_System_B_G:
    case heif_transfer_characteristic_ITU_R_BT_601_6:
    case heif_transfer_characteristic_SMPTE_240M:
    case heif_transfer_characteristic_logarithmic_100:
    case heif_transfer_characteristic_logarithmic_100_sqrt10:
    case heif_transfer_characteristic_IEC_61966_2_4:
    case heif_transfer_characteristic_ITU_R_BT_1361:
    case heif_transfer_characteristic_ITU_R_BT_2020_2_10bit:
    case heif_transfer_characteristic_ITU_R_BT_2020_2_12bit:
    case heif_transfer_characteristic_SMPTE_ST_428_1:
        return TransferFunction::gamma;
    }
    return TransferFunction::unknown;
}

void apply_nclx(ColorEncoding& color, const heif_color_profile_nclx& nclx) {
    color.primaries = color_primaries(nclx.color_primaries);
    color.transfer = transfer_function(nclx.transfer_characteristics);
    if (color.transfer == TransferFunction::pq || color.transfer == TransferFunction::hlg) {
        color.dynamic_range = DynamicRange::high;
    }
}

template <typename ProfileOwner> void read_nclx(ColorEncoding& color, ProfileOwner&& loader) {
    heif_color_profile_nclx* raw = nullptr;
    if (loader(&raw).code == heif_error_Ok && raw) {
        NclxPtr profile(raw);
        apply_nclx(color, *profile);
    }
}

Result<ColorEncoding> read_color(const heif_image_handle* handle, const DecodeLimits& limits) {
    ColorEncoding color;
    const std::size_t profile_size = heif_image_handle_get_raw_color_profile_size(handle);
    if (profile_size > limits.maximum_metadata_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF ICC profile exceeds the configured metadata limit.", "libheif");
    }
    if (profile_size != 0) {
        color.icc_profile.resize(profile_size);
        heif_error error =
            heif_image_handle_get_raw_color_profile(handle, color.icc_profile.data());
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::decode_failed, "HEIF ICC profile extraction");
        }
    }
    read_nclx(color, [handle](heif_color_profile_nclx** out) {
        return heif_image_handle_get_nclx_color_profile(handle, out);
    });
    heif_content_light_level light{};
    if (heif_image_handle_get_content_light_level(handle, &light)) {
        color.max_content_light_level = light.max_content_light_level;
        color.max_frame_average_light_level = light.max_pic_average_light_level;
        color.source_peak_nits = static_cast<float>(light.max_content_light_level);
        color.dynamic_range = DynamicRange::high;
    }
    heif_mastering_display_colour_volume encoded{};
    if (heif_image_handle_get_mastering_display_colour_volume(handle, &encoded)) {
        heif_decoded_mastering_display_colour_volume decoded{};
        if (heif_mastering_display_colour_volume_decode(&encoded, &decoded).code == heif_error_Ok) {
            color.mastering_primaries_and_white =
                std::array<float, 8>{decoded.display_primaries_x[0], decoded.display_primaries_y[0],
                                     decoded.display_primaries_x[1], decoded.display_primaries_y[1],
                                     decoded.display_primaries_x[2], decoded.display_primaries_y[2],
                                     decoded.white_point_x,          decoded.white_point_y};
            color.source_peak_nits = static_cast<float>(decoded.max_display_mastering_luminance);
            color.dynamic_range = DynamicRange::high;
        }
    }
    return color;
}

ColorEncoding read_color(const heif_image* image) {
    ColorEncoding color;
    const std::size_t profile_size = heif_image_get_raw_color_profile_size(image);
    if (profile_size != 0) {
        color.icc_profile.resize(profile_size);
        if (heif_image_get_raw_color_profile(image, color.icc_profile.data()).code !=
            heif_error_Ok) {
            color.icc_profile.clear();
        }
    }
    read_nclx(color, [image](heif_color_profile_nclx** out) {
        return heif_image_get_nclx_color_profile(image, out);
    });
    heif_content_light_level light{};
    if (heif_image_has_content_light_level(image)) {
        heif_image_get_content_light_level(image, &light);
        color.max_content_light_level = light.max_content_light_level;
        color.max_frame_average_light_level = light.max_pic_average_light_level;
        color.source_peak_nits = static_cast<float>(light.max_content_light_level);
        color.dynamic_range = DynamicRange::high;
    }
    return color;
}

Result<Metadata> read_metadata(const heif_image_handle* handle, const DecodeOptions& options) {
    Metadata metadata;
    if (!options.preserve_metadata)
        return metadata;
    const int count = heif_image_handle_get_number_of_metadata_blocks(handle, nullptr);
    if (count <= 0)
        return metadata;
    if (static_cast<std::uint32_t>(count) > options.limits.maximum_frames) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF metadata block count exceeds the configured limit.", "libheif");
    }
    std::vector<heif_item_id> ids(static_cast<std::size_t>(count));
    const int returned =
        heif_image_handle_get_list_of_metadata_block_IDs(handle, nullptr, ids.data(), count);
    std::uint64_t total = 0;
    for (int index = 0; index < returned; ++index) {
        const heif_item_id id = ids[static_cast<std::size_t>(index)];
        const char* raw_type = heif_image_handle_get_metadata_type(handle, id);
        const std::string type = raw_type ? raw_type : "";
        const char* raw_content = heif_image_handle_get_metadata_content_type(handle, id);
        const std::string content = raw_content ? raw_content : "";
        const std::size_t size = heif_image_handle_get_metadata_size(handle, id);
        if (size > options.limits.maximum_metadata_bytes ||
            total > options.limits.maximum_metadata_bytes - size) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "HEIF metadata exceeds the configured byte limit.", "libheif");
        }
        total += size;
        std::vector<std::byte> data(size);
        heif_error error = heif_image_handle_get_metadata(handle, id, data.data());
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::decode_failed, "HEIF metadata extraction");
        }
        if (type == "Exif") {
            metadata.exif = std::move(data);
        } else if (content == "application/rdf+xml") {
            metadata.xmp = std::move(data);
        } else {
            metadata.blocks.push_back({type, content, std::move(data), false});
        }
    }
    return metadata;
}

Orientation read_orientation(const heif_context* context, const heif_image_handle* handle,
                             OrientationPolicy policy) {
    static_cast<void>(context);
    static_cast<void>(handle);
    static_cast<void>(policy);
    // HEIF transformations include clean-aperture cropping as well as orientation.
    // libheif applies the ordered transform chain atomically, so exposing a residual
    // orientation here would make callers apply rotation or mirroring twice.
    return Orientation::identity;
}

Result<std::vector<heif_item_id>> top_level_ids(heif_context* context, const DecodeLimits& limits) {
    const int count = heif_context_get_number_of_top_level_images(context);
    if (count <= 0) {
        return Status::error(ErrorCode::corrupt_data, "HEIF file has no top-level images.",
                             "libheif");
    }
    if (static_cast<std::uint32_t>(count) > limits.maximum_frames) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF image collection exceeds the configured frame limit.",
                             "libheif");
    }
    std::vector<heif_item_id> ids(static_cast<std::size_t>(count));
    const int returned = heif_context_get_list_of_top_level_image_IDs(context, ids.data(), count);
    ids.resize(static_cast<std::size_t>(std::max(returned, 0)));
    heif_item_id primary = 0;
    if (heif_context_get_primary_image_ID(context, &primary).code == heif_error_Ok) {
        const auto position = std::find(ids.begin(), ids.end(), primary);
        if (position != ids.end())
            std::rotate(ids.begin(), position, position + 1);
    }
    return ids;
}

Result<HandlePtr> image_handle(heif_context* context, heif_item_id id) {
    heif_image_handle* raw = nullptr;
    heif_error error = heif_context_get_image_handle(context, id, &raw);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::decode_failed, "HEIF image handle lookup");
    }
    return HandlePtr(raw);
}

struct ImageDescription final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = kRgba8;
    bool has_alpha = false;
    AlphaMode alpha = AlphaMode::none;
    int bits_per_channel = 8;
    heif_colorspace preferred_colorspace = heif_colorspace_undefined;
    heif_chroma preferred_chroma = heif_chroma_undefined;
    ColorRange color_range = ColorRange::limited;
    Metadata metadata;
    ColorEncoding color;
};

Result<ImageDescription> describe_handle(const heif_context* context,
                                         const heif_image_handle* handle,
                                         const DecodeOptions& options) {
    const int width = heif_image_handle_get_width(handle);
    const int height = heif_image_handle_get_height(handle);
    if (width <= 0 || height <= 0) {
        return Status::error(ErrorCode::corrupt_data, "HEIF image dimensions are invalid.",
                             "libheif");
    }
    ImageDescription description;
    description.width = static_cast<std::uint32_t>(width);
    description.height = static_cast<std::uint32_t>(height);
    Result<void> dimensions =
        validate_dimensions(description.width, description.height, options.limits);
    if (!dimensions)
        return dimensions.error();
    const int bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    description.bits_per_channel = bits;
    description.format = bits > 8 ? kRgba16 : kRgba8;
    description.has_alpha = heif_image_handle_has_alpha_channel(handle) != 0;
    description.alpha = description.has_alpha ? (heif_image_handle_is_premultiplied_alpha(handle)
                                                     ? AlphaMode::premultiplied
                                                     : AlphaMode::straight)
                                              : AlphaMode::none;
    heif_error preferred = heif_image_handle_get_preferred_decoding_colorspace(
        handle, &description.preferred_colorspace, &description.preferred_chroma);
    if (preferred.code != heif_error_Ok) {
        description.preferred_colorspace = heif_colorspace_undefined;
        description.preferred_chroma = heif_chroma_undefined;
    }
    if (options.output_format) {
        if (*options.output_format != kRgba8 && *options.output_format != kRgba16) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "HEIF decoding supports RGBA8 and RGBA16 output.", "libheif");
        }
        description.format = *options.output_format;
    }
    Result<Metadata> metadata = read_metadata(handle, options);
    if (!metadata)
        return metadata.error();
    description.metadata = std::move(metadata).value();
    description.metadata.orientation = read_orientation(context, handle, options.orientation);
    Result<ColorEncoding> color = read_color(handle, options.limits);
    if (!color)
        return color.error();
    description.color = std::move(color).value();
    heif_color_profile_nclx* raw_nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle, &raw_nclx).code == heif_error_Ok &&
        raw_nclx) {
        NclxPtr nclx(raw_nclx);
        description.color_range = nclx->full_range_flag ? ColorRange::full : ColorRange::limited;
    }
    if (bits > 8)
        description.color.dynamic_range = DynamicRange::high;
    return description;
}

std::pair<std::uint32_t, std::uint32_t> scaled_size(std::uint32_t width, std::uint32_t height,
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

Result<ChromaSubsampling> native_subsampling(heif_chroma chroma) {
    switch (chroma) {
    case heif_chroma_420:
        return ChromaSubsampling::yuv420;
    case heif_chroma_422:
        return ChromaSubsampling::yuv422;
    case heif_chroma_444:
        return ChromaSubsampling::yuv444;
    default:
        return Status::error(ErrorCode::unsupported_feature,
                             "HEIF preferred chroma layout is not a supported planar format.",
                             "libheif");
    }
}

bool native_heif_supported(const ImageDescription& description) {
    return description.preferred_colorspace == heif_colorspace_YCbCr &&
           (description.preferred_chroma == heif_chroma_420 ||
            description.preferred_chroma == heif_chroma_422 ||
            description.preferred_chroma == heif_chroma_444) &&
           description.bits_per_channel > 0 && description.bits_per_channel <= 8 &&
           description.alpha != AlphaMode::premultiplied;
}

Result<DocumentDescriptor> native_heif_descriptor(Format format,
                                                  const ImageDescription& description,
                                                  const DecodeOptions& options) {
    if (!native_heif_supported(description))
        return Status::error(ErrorCode::unsupported_feature,
                             "This HEIF image has no supported native SDR layout.", "libheif");
    Result<ChromaSubsampling> subsampling = native_subsampling(description.preferred_chroma);
    if (!subsampling)
        return subsampling.error();
    const auto [width, height] = scaled_size(description.width, description.height, options);
    const std::uint32_t horizontal = subsampling.value() == ChromaSubsampling::yuv444 ? 1U : 2U;
    const std::uint32_t vertical = subsampling.value() == ChromaSubsampling::yuv420 ? 2U : 1U;
    const std::uint32_t chroma_width = (width + horizontal - 1U) / horizontal;
    const std::uint32_t chroma_height = (height + vertical - 1U) / vertical;
    DocumentDescriptor document;
    document.format = format;
    document.canvas_width = width;
    document.canvas_height = height;
    document.metadata = description.metadata;
    document.color = description.color;
    RasterFrameDescriptor frame;
    frame.width = width;
    frame.height = height;
    frame.metadata = description.metadata;
    frame.color = description.color;
    frame.layout.color_model = ColorModel::ycbcr;
    frame.layout.alpha = description.alpha;
    frame.layout.chroma_subsampling = subsampling.value();
    frame.layout.color_range = description.color_range;
    frame.layout.planes = {{PlaneSemantic::luma, width, height, kGray8, 8},
                           {PlaneSemantic::chroma_blue, chroma_width, chroma_height, kGray8, 8},
                           {PlaneSemantic::chroma_red, chroma_width, chroma_height, kGray8, 8}};
    if (description.has_alpha)
        frame.layout.planes.push_back({PlaneSemantic::alpha, width, height, kGray8, 8});
    document.frames.push_back(std::move(frame));
    Result<void> valid = document.validate();
    if (!valid)
        return valid.error();
    return document;
}

bool matching_descriptor(const DocumentDescriptor& left, const DocumentDescriptor& right) {
    return left.canvas_width == right.canvas_width && left.canvas_height == right.canvas_height &&
           left.frames.size() == 1 && right.frames.size() == 1 &&
           left.frames.front().width == right.frames.front().width &&
           left.frames.front().height == right.frames.front().height &&
           left.frames.front().layout == right.frames.front().layout;
}

struct StopContext final {
    std::stop_token stop;
};

int cancel_heif_decode(void* user_data) {
    const auto* context = static_cast<const StopContext*>(user_data);
    return context && context->stop.stop_requested() ? 1 : 0;
}

Result<Image> copy_heif_image(const heif_image* decoded, const PixelFormat& format,
                              std::uint32_t width, std::uint32_t height, std::uint64_t& owned_bytes,
                              const DecodeLimits& limits, std::stop_token stop) {
    Result<std::size_t> bytes_per_pixel = format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(width) * height * bytes_per_pixel.value();
    if (bytes > limits.maximum_owned_output_bytes ||
        owned_bytes > limits.maximum_owned_output_bytes - bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF pixels exceed the configured owning decode limit.", "libheif");
    }
    std::size_t source_stride = 0;
    const std::uint8_t* source =
        heif_image_get_plane_readonly2(decoded, heif_channel_interleaved, &source_stride);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel.value();
    if (!source || source_stride < row_bytes) {
        return Status::error(ErrorCode::decode_failed,
                             "libheif returned an invalid interleaved image plane.", "libheif");
    }
    Result<MutableImage> allocated = MutableImage::allocate(width, height, format);
    if (!allocated)
        return allocated.error();
    MutableImage output = std::move(allocated).value();
    for (std::uint32_t row = 0; row < height; ++row) {
        if (stop.stop_requested())
            return cancelled_status();
        std::memcpy(output.pixels().data() + static_cast<std::size_t>(row) * output.row_stride(),
                    source + static_cast<std::size_t>(row) * source_stride, row_bytes);
    }
    owned_bytes += bytes;
    return std::move(output).freeze();
}

Result<ImagePtr> decode_packed_heif_image(const heif_image_handle* handle,
                                          const ImageDescription& description,
                                          const DecodeOptions& options, std::stop_token stop) {
    StopContext stop_context{stop};
    DecodeOptionsPtr decoding(heif_decoding_options_alloc());
    if (!decoding) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate HEIF decoding options.",
                             "libheif");
    }
    decoding->ignore_transformations = 0;
    decoding->strict_decoding = 1;
    decoding->cancel_decoding = cancel_heif_decode;
    decoding->progress_user_data = &stop_context;
    // Decode one media timeline pass. Edit-list repetition (including infinite looping)
    // belongs in the playback scheduler and must not make a decode operation unbounded.
    decoding->ignore_sequence_editlist = 1;
    const heif_chroma chroma = description.format == kRgba16 ? heif_chroma_interleaved_RRGGBBAA_LE
                                                             : heif_chroma_interleaved_RGBA;
    heif_image* raw = nullptr;
    heif_error error = heif_decode_image(handle, &raw, heif_colorspace_RGB, chroma, decoding.get());
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::decode_failed, "HEIF pixel decoding");
    }
    ImagePtr decoded(raw);
    const auto target = scaled_size(description.width, description.height, options);
    if (target.first != description.width || target.second != description.height) {
        heif_image* scaled_raw = nullptr;
        error = heif_image_scale_image(decoded.get(), &scaled_raw, static_cast<int>(target.first),
                                       static_cast<int>(target.second), nullptr);
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::decode_failed, "HEIF image scaling");
        }
        decoded.reset(scaled_raw);
    }
    return decoded;
}

Result<Image> decode_handle(const heif_image_handle* handle, const ImageDescription& description,
                            const DecodeOptions& options, std::uint64_t& owned_bytes,
                            std::stop_token stop) {
    Result<ImagePtr> decoded = decode_packed_heif_image(handle, description, options, stop);
    if (!decoded)
        return decoded.error();
    const auto [width, height] = scaled_size(description.width, description.height, options);
    return copy_heif_image(decoded.value().get(), description.format, width, height, owned_bytes,
                           options.limits, stop);
}

Result<ImagePtr> decode_native_heif_image(const heif_image_handle* handle,
                                          const ImageDescription& description,
                                          const DecodeOptions& options, std::stop_token stop) {
    StopContext stop_context{stop};
    DecodeOptionsPtr decoding(heif_decoding_options_alloc());
    if (!decoding)
        return Status::error(ErrorCode::out_of_memory, "Could not allocate HEIF decoding options.",
                             "libheif");
    decoding->ignore_transformations = 0;
    decoding->strict_decoding = 1;
    decoding->cancel_decoding = cancel_heif_decode;
    decoding->progress_user_data = &stop_context;
    decoding->ignore_sequence_editlist = 1;
    heif_image* raw = nullptr;
    heif_error error = heif_decode_image(handle, &raw, description.preferred_colorspace,
                                         description.preferred_chroma, decoding.get());
    if (error.code != heif_error_Ok)
        return heif_status(error, ErrorCode::decode_failed, "HEIF native plane decoding");
    ImagePtr decoded(raw);
    const auto target = scaled_size(description.width, description.height, options);
    if (target.first != description.width || target.second != description.height) {
        heif_image* scaled_raw = nullptr;
        error = heif_image_scale_image(decoded.get(), &scaled_raw, static_cast<int>(target.first),
                                       static_cast<int>(target.second), nullptr);
        if (error.code != heif_error_Ok)
            return heif_status(error, ErrorCode::decode_failed, "HEIF native plane scaling");
        decoded.reset(scaled_raw);
    }
    return decoded;
}

Result<void> publish_packed_heif_image(const heif_image* decoded, const PixelFormat& format,
                                       std::uint32_t width, std::uint32_t height,
                                       std::uint32_t sink_frame_index, PixelSink& sink,
                                       const DecodeLimits& limits, std::stop_token stop) {
    Result<std::size_t> bytes_per_pixel = format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();
    if (width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel.value()) {
        return Status::error(ErrorCode::limit_exceeded, "HEIF output row size overflows.",
                             "libheif");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel.value();
    if (height != 0 && row_bytes > std::numeric_limits<std::size_t>::max() / height) {
        return Status::error(ErrorCode::limit_exceeded, "HEIF output size overflows.", "libheif");
    }
    const std::size_t byte_size = row_bytes * height;
    if (byte_size > limits.maximum_working_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "One decoded HEIF frame exceeds the configured working-memory limit.",
                             "libheif");
    }
    std::size_t source_stride = 0;
    const std::uint8_t* source =
        heif_image_get_plane_readonly2(decoded, heif_channel_interleaved, &source_stride);
    if (!source || source_stride < row_bytes) {
        return Status::error(ErrorCode::decode_failed,
                             "libheif returned an invalid interleaved image plane.", "libheif");
    }
    std::span<std::byte> storage = sink.frame_storage(sink_frame_index, row_bytes, byte_size);
    if (storage.size() == byte_size) {
        for (std::uint32_t row = 0; row < height; ++row) {
            if (stop.stop_requested())
                return cancelled_status();
            std::memcpy(storage.data() + static_cast<std::size_t>(row) * row_bytes,
                        source + static_cast<std::size_t>(row) * source_stride, row_bytes);
        }
        return {};
    }
    if (stop.stop_requested())
        return cancelled_status();
    if (height > 1U &&
        source_stride > (std::numeric_limits<std::size_t>::max() - row_bytes) / (height - 1U)) {
        return Status::error(ErrorCode::limit_exceeded, "HEIF source plane size overflows.",
                             "libheif");
    }
    const std::size_t source_size =
        height == 0 ? 0 : static_cast<std::size_t>(height - 1U) * source_stride + row_bytes;
    return sink.write_rows(0, height, source_stride,
                           std::span(reinterpret_cast<const std::byte*>(source), source_size));
}

Result<DocumentInfo> inspect_collection(ParsedHeif& parsed, Format format,
                                        const DecodeOptions& options) {
    Result<std::vector<heif_item_id>> ids = top_level_ids(parsed.context.get(), options.limits);
    if (!ids)
        return ids.error();
    DocumentInfo info;
    info.format = format;
    for (heif_item_id id : ids.value()) {
        Result<HandlePtr> handle = image_handle(parsed.context.get(), id);
        if (!handle)
            return handle.error();
        Result<ImageDescription> description =
            describe_handle(parsed.context.get(), handle.value().get(), options);
        if (!description)
            return description.error();
        const auto target =
            scaled_size(description.value().width, description.value().height, options);
        info.canvas_width = std::max(info.canvas_width, target.first);
        info.canvas_height = std::max(info.canvas_height, target.second);
        info.frames.push_back({target.first, target.second, 0, 0, std::chrono::nanoseconds{0},
                               description.value().format, description.value().has_alpha,
                               std::nullopt});
        if (info.frames.size() == 1) {
            info.metadata = description.value().metadata;
            info.color = description.value().color;
        }
    }
    return info;
}

Result<Document> decode_collection(ParsedHeif& parsed, Format format, const DecodeOptions& options,
                                   std::stop_token stop) {
    Result<std::vector<heif_item_id>> ids = top_level_ids(parsed.context.get(), options.limits);
    if (!ids)
        return ids.error();
    if (options.frame_index && *options.frame_index >= ids.value().size()) {
        return Status::error(ErrorCode::invalid_argument,
                             "Requested HEIF collection image index is out of range.", "libheif");
    }
    const std::size_t first = options.frame_index ? *options.frame_index : 0;
    const std::size_t end = options.frame_index ? first + 1 : ids.value().size();
    Document document;
    document.format = format;
    std::uint64_t owned_bytes = 0;
    for (std::size_t index = first; index < end; ++index) {
        if (stop.stop_requested())
            return cancelled_status();
        Result<HandlePtr> handle = image_handle(parsed.context.get(), ids.value()[index]);
        if (!handle)
            return handle.error();
        Result<ImageDescription> description =
            describe_handle(parsed.context.get(), handle.value().get(), options);
        if (!description)
            return description.error();
        Result<Image> image =
            decode_handle(handle.value().get(), description.value(), options, owned_bytes, stop);
        if (!image)
            return image.error();
        Frame frame;
        frame.image = std::move(image).value();
        frame.metadata = std::move(description.value().metadata);
        frame.color = std::move(description.value().color);
        document.canvas_width = std::max(document.canvas_width, frame.image.width());
        document.canvas_height = std::max(document.canvas_height, frame.image.height());
        if (document.frames.empty()) {
            document.metadata = frame.metadata;
            document.color = frame.color;
        }
        document.frames.push_back(std::move(frame));
    }
    return document;
}

Result<TrackPtr> first_visual_track(heif_context* context) {
    TrackPtr track(heif_context_get_track(context, 0));
    if (!track) {
        return Status::error(ErrorCode::corrupt_data, "HEIF sequence has no visual track.",
                             "libheif");
    }
    return track;
}

struct SequenceTiming final {
    std::uint32_t loop_count = 1;
    std::vector<std::chrono::nanoseconds> durations;
};

constexpr std::array<std::byte, 8> kTimingMagic{std::byte{'S'}, std::byte{'N'}, std::byte{'O'},
                                                std::byte{'W'}, std::byte{'T'}, std::byte{'I'},
                                                std::byte{'M'}, std::byte{'E'}};

std::uint32_t read_u32_le(const std::byte* bytes) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[3])) << 24U);
}

std::uint64_t read_u64_le(const std::byte* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[index]))
                 << (index * 8U);
    }
    return value;
}

void append_u32_le(std::vector<std::byte>& output, std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void append_u64_le(std::vector<std::byte>& output, std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8; ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

Result<std::optional<SequenceTiming>> read_sequence_timing(heif_context* context,
                                                           const DecodeLimits& limits) {
    heif_image_handle* raw_handle = nullptr;
    const heif_error handle_error = heif_context_get_primary_image_handle(context, &raw_handle);
    if (handle_error.code != heif_error_Ok)
        return std::optional<SequenceTiming>{};
    HandlePtr handle(raw_handle);
    const int count = heif_image_handle_get_number_of_metadata_blocks(handle.get(), "snim");
    if (count <= 0)
        return std::optional<SequenceTiming>{};
    heif_item_id id = 0;
    if (heif_image_handle_get_list_of_metadata_block_IDs(handle.get(), "snim", &id, 1) != 1) {
        return std::optional<SequenceTiming>{};
    }
    const std::size_t size = heif_image_handle_get_metadata_size(handle.get(), id);
    if (size > limits.maximum_metadata_bytes || size < 24) {
        return Status::error(size > limits.maximum_metadata_bytes ? ErrorCode::limit_exceeded
                                                                  : ErrorCode::corrupt_data,
                             "HEIF sequence timing metadata has an invalid size.", "libheif");
    }
    std::vector<std::byte> bytes(size);
    const heif_error metadata_error =
        heif_image_handle_get_metadata(handle.get(), id, bytes.data());
    if (metadata_error.code != heif_error_Ok) {
        return heif_status(metadata_error, ErrorCode::decode_failed,
                           "HEIF sequence timing extraction");
    }
    if (!std::equal(kTimingMagic.begin(), kTimingMagic.end(), bytes.begin()) ||
        read_u32_le(bytes.data() + 8) != 1U) {
        return std::optional<SequenceTiming>{};
    }
    const std::uint32_t frame_count = read_u32_le(bytes.data() + 16);
    if (frame_count > limits.maximum_frames ||
        size != 24U + static_cast<std::size_t>(frame_count) * 8U) {
        return Status::error(frame_count > limits.maximum_frames ? ErrorCode::limit_exceeded
                                                                 : ErrorCode::corrupt_data,
                             "HEIF sequence timing metadata is inconsistent.", "libheif");
    }
    SequenceTiming timing;
    timing.loop_count = read_u32_le(bytes.data() + 12);
    timing.durations.reserve(frame_count);
    for (std::uint32_t index = 0; index < frame_count; ++index) {
        const std::uint64_t duration =
            read_u64_le(bytes.data() + 24U + static_cast<std::size_t>(index) * 8U);
        timing.durations.emplace_back(
            duration > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ? std::numeric_limits<std::int64_t>::max()
                : static_cast<std::int64_t>(duration));
    }
    return std::optional<SequenceTiming>{std::move(timing)};
}

struct SequenceInspection final {
    DocumentInfo info;
    bool consumed_track = false;
};

Result<SequenceInspection> inspect_sequence_frames(ParsedHeif& parsed, Format format,
                                                   const DecodeOptions& options,
                                                   std::stop_token stop) {
    PixelFormat output_format = options.output_format.value_or(kRgba8);
    if (output_format != kRgba8 && output_format != kRgba16) {
        return Status::error(ErrorCode::unsupported_feature,
                             "HEIF sequence decoding supports RGBA8 and RGBA16 output.", "libheif");
    }
    Result<std::optional<SequenceTiming>> timing =
        read_sequence_timing(parsed.context.get(), options.limits);
    if (!timing)
        return timing.error();
    Result<TrackPtr> track = first_visual_track(parsed.context.get());
    if (!track)
        return track.error();
    const std::uint32_t timescale = heif_track_get_timescale(track.value().get());
    if (timescale == 0) {
        return Status::error(ErrorCode::corrupt_data,
                             "HEIF sequence track has an invalid timescale.", "libheif");
    }

    DocumentInfo info;
    info.format = format;
    if (timing.value())
        info.loop_count = timing.value()->loop_count;

    heif_image_handle* primary_raw = nullptr;
    if (heif_context_get_primary_image_handle(parsed.context.get(), &primary_raw).code ==
            heif_error_Ok &&
        primary_raw) {
        HandlePtr primary(primary_raw);
        Result<ImageDescription> description =
            describe_handle(parsed.context.get(), primary.get(), options);
        if (!description)
            return description.error();
        info.metadata = description.value().metadata;
        info.color = description.value().color;
    }

    // snow_image sequence encodes carry an exact timing table. Together with
    // the fixed visual-track resolution it lets the sink path begin immediately
    // and decode every frame exactly once.
    if (timing.value() && !timing.value()->durations.empty()) {
        const std::size_t frame_count = timing.value()->durations.size();
        if (options.frame_index && *options.frame_index >= frame_count) {
            return Status::error(ErrorCode::invalid_argument,
                                 "Requested HEIF sequence frame index is out of range.", "libheif");
        }
        std::uint16_t track_width = 0;
        std::uint16_t track_height = 0;
        const heif_error resolution =
            heif_track_get_image_resolution(track.value().get(), &track_width, &track_height);
        if (resolution.code == heif_error_Ok && track_width != 0 && track_height != 0) {
            Result<void> dimensions =
                validate_dimensions(track_width, track_height, options.limits);
            if (!dimensions)
                return dimensions.error();
            const auto target = scaled_size(track_width, track_height, options);
            info.canvas_width = target.first;
            info.canvas_height = target.second;
            const std::size_t first = options.frame_index ? *options.frame_index : 0;
            const std::size_t end = options.frame_index ? first + 1 : frame_count;
            info.frames.reserve(end - first);
            const bool has_alpha = heif_track_has_alpha_channel(track.value().get()) != 0;
            for (std::size_t index = first; index < end; ++index) {
                FrameInfo frame{
                    target.first,  target.second, 0,           0, timing.value()->durations[index],
                    output_format, has_alpha,     std::nullopt};
                frame.color = info.color;
                if (index == first)
                    frame.metadata = info.metadata;
                info.frames.push_back(std::move(frame));
            }
            return SequenceInspection{std::move(info), false};
        }
    }

    StopContext stop_context{stop};
    DecodeOptionsPtr decoding(heif_decoding_options_alloc());
    if (!decoding) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate HEIF sequence decoding options.", "libheif");
    }
    decoding->strict_decoding = 1;
    decoding->cancel_decoding = cancel_heif_decode;
    decoding->progress_user_data = &stop_context;
    decoding->ignore_sequence_editlist = 1;
    const heif_chroma chroma = output_format == kRgba16 ? heif_chroma_interleaved_RRGGBBAA_LE
                                                        : heif_chroma_interleaved_RGBA;
    std::uint32_t decoded_index = 0;
    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        heif_image* raw = nullptr;
        const heif_error error = heif_track_decode_next_image(
            track.value().get(), &raw, heif_colorspace_RGB, chroma, decoding.get());
        if (error.code == heif_error_End_of_sequence)
            break;
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::decode_failed, "HEIF sequence frame inspection");
        }
        ImagePtr decoded(raw);
        if (decoded_index >= options.limits.maximum_frames) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "HEIF sequence exceeds the configured frame limit.", "libheif");
        }
        const bool selected = !options.frame_index || *options.frame_index == decoded_index;
        if (selected) {
            const int raw_width = heif_image_get_primary_width(decoded.get());
            const int raw_height = heif_image_get_primary_height(decoded.get());
            if (raw_width <= 0 || raw_height <= 0) {
                return Status::error(ErrorCode::corrupt_data,
                                     "HEIF sequence frame dimensions are invalid.", "libheif");
            }
            const auto width = static_cast<std::uint32_t>(raw_width);
            const auto height = static_cast<std::uint32_t>(raw_height);
            Result<void> dimensions = validate_dimensions(width, height, options.limits);
            if (!dimensions)
                return dimensions.error();
            const auto target = scaled_size(width, height, options);
            const std::uint32_t ticks = heif_image_get_duration(decoded.get());
            std::chrono::nanoseconds duration(static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(ticks) * 1'000'000'000ULL) / timescale));
            if (timing.value() && decoded_index < timing.value()->durations.size()) {
                duration = timing.value()->durations[decoded_index];
            }
            FrameInfo frame{target.first,
                            target.second,
                            0,
                            0,
                            duration,
                            output_format,
                            heif_track_has_alpha_channel(track.value().get()) != 0,
                            std::nullopt};
            frame.color = read_color(decoded.get());
            if (info.frames.empty()) {
                if (info.color.icc_profile.empty() &&
                    info.color.primaries == ColorPrimaries::unknown)
                    info.color = frame.color;
                frame.metadata = info.metadata;
            }
            info.canvas_width = std::max(info.canvas_width, target.first);
            info.canvas_height = std::max(info.canvas_height, target.second);
            info.frames.push_back(std::move(frame));
        }
        ++decoded_index;
        if (options.frame_index && decoded_index > *options.frame_index)
            break;
    }
    if (options.frame_index && info.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument,
                             "Requested HEIF sequence frame index is out of range.", "libheif");
    }
    if (info.frames.empty()) {
        return Status::error(ErrorCode::corrupt_data, "HEIF sequence has no frames.", "libheif");
    }
    return SequenceInspection{std::move(info), true};
}

Result<Document> decode_sequence(ParsedHeif& parsed, Format format, const DecodeOptions& options,
                                 std::stop_token stop) {
    Result<std::optional<SequenceTiming>> timing =
        read_sequence_timing(parsed.context.get(), options.limits);
    if (!timing)
        return timing.error();
    Result<TrackPtr> track = first_visual_track(parsed.context.get());
    if (!track)
        return track.error();
    const std::uint32_t timescale = heif_track_get_timescale(track.value().get());
    if (timescale == 0) {
        return Status::error(ErrorCode::corrupt_data,
                             "HEIF sequence track has an invalid timescale.", "libheif");
    }
    PixelFormat output_format = options.output_format.value_or(kRgba8);
    if (output_format != kRgba8 && output_format != kRgba16) {
        return Status::error(ErrorCode::unsupported_feature,
                             "HEIF sequence decoding supports RGBA8 and RGBA16 output.", "libheif");
    }
    StopContext stop_context{stop};
    DecodeOptionsPtr decoding(heif_decoding_options_alloc());
    if (!decoding) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate HEIF sequence decoding options.", "libheif");
    }
    decoding->strict_decoding = 1;
    decoding->cancel_decoding = cancel_heif_decode;
    decoding->progress_user_data = &stop_context;
    decoding->ignore_sequence_editlist = 1;
    const heif_chroma chroma = output_format == kRgba16 ? heif_chroma_interleaved_RRGGBBAA_LE
                                                        : heif_chroma_interleaved_RGBA;
    Document document;
    document.format = format;
    const std::optional<SequenceTiming>& sequence_timing = timing.value();
    if (sequence_timing)
        document.loop_count = sequence_timing->loop_count;
    std::uint64_t owned_bytes = 0;
    std::uint32_t decoded_index = 0;
    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        heif_image* raw = nullptr;
        const heif_error error = heif_track_decode_next_image(
            track.value().get(), &raw, heif_colorspace_RGB, chroma, decoding.get());
        if (error.code == heif_error_End_of_sequence)
            break;
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::decode_failed, "HEIF sequence frame decoding");
        }
        ImagePtr decoded(raw);
        if (decoded_index >= options.limits.maximum_frames) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "HEIF sequence exceeds the configured frame limit.", "libheif");
        }
        const bool selected = !options.frame_index || *options.frame_index == decoded_index;
        if (selected) {
            const int raw_width = heif_image_get_primary_width(decoded.get());
            const int raw_height = heif_image_get_primary_height(decoded.get());
            if (raw_width <= 0 || raw_height <= 0) {
                return Status::error(ErrorCode::corrupt_data,
                                     "HEIF sequence frame dimensions are invalid.", "libheif");
            }
            std::uint32_t width = static_cast<std::uint32_t>(raw_width);
            std::uint32_t height = static_cast<std::uint32_t>(raw_height);
            Result<void> dimensions = validate_dimensions(width, height, options.limits);
            if (!dimensions)
                return dimensions.error();
            const auto target = scaled_size(width, height, options);
            if (target.first != width || target.second != height) {
                heif_image* scaled_raw = nullptr;
                const heif_error scale_error = heif_image_scale_image(
                    decoded.get(), &scaled_raw, static_cast<int>(target.first),
                    static_cast<int>(target.second), nullptr);
                if (scale_error.code != heif_error_Ok) {
                    return heif_status(scale_error, ErrorCode::decode_failed,
                                       "HEIF sequence frame scaling");
                }
                decoded.reset(scaled_raw);
                width = target.first;
                height = target.second;
            }
            Result<Image> image = copy_heif_image(decoded.get(), output_format, width, height,
                                                  owned_bytes, options.limits, stop);
            if (!image)
                return image.error();
            Frame frame;
            frame.image = std::move(image).value();
            if (sequence_timing && decoded_index < sequence_timing->durations.size()) {
                frame.duration = sequence_timing->durations[decoded_index];
            } else {
                const std::uint32_t ticks = heif_image_get_duration(decoded.get());
                frame.duration = std::chrono::nanoseconds(static_cast<std::int64_t>(
                    (static_cast<std::uint64_t>(ticks) * 1'000'000'000ULL) / timescale));
            }
            frame.color = read_color(decoded.get());
            document.canvas_width = std::max(document.canvas_width, width);
            document.canvas_height = std::max(document.canvas_height, height);
            if (document.frames.empty())
                document.color = frame.color;
            document.frames.push_back(std::move(frame));
        }
        ++decoded_index;
        if (options.frame_index && decoded_index > *options.frame_index)
            break;
    }
    if (options.frame_index && document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument,
                             "Requested HEIF sequence frame index is out of range.", "libheif");
    }
    if (document.frames.empty()) {
        return Status::error(ErrorCode::corrupt_data, "HEIF sequence has no frames.", "libheif");
    }
    return document;
}

Result<void> decode_sequence_to_sink(ParsedHeif& parsed, Format format, PixelSink& sink,
                                     const DecodeOptions& options, std::stop_token stop) {
    Result<SequenceInspection> inspected = inspect_sequence_frames(parsed, format, options, stop);
    if (!inspected)
        return inspected.error();
    SequenceInspection sequence = std::move(inspected).value();

    for (const FrameInfo& frame : sequence.info.frames) {
        Result<std::size_t> bytes_per_pixel = frame.native_format.bytes_per_pixel();
        if (!bytes_per_pixel)
            return bytes_per_pixel.error();
        const std::uint64_t frame_bytes =
            static_cast<std::uint64_t>(frame.width) * frame.height * bytes_per_pixel.value();
        if (frame_bytes > options.limits.maximum_working_bytes) {
            return Status::error(
                ErrorCode::limit_exceeded,
                "One decoded HEIF frame exceeds the configured working-memory limit.", "libheif");
        }
    }

    ContextPtr reopened;
    heif_context* context = parsed.context.get();
    if (sequence.consumed_track) {
        Result<ContextPtr> fresh = reopen_heif(parsed, options);
        if (!fresh)
            return fresh.error();
        reopened = std::move(fresh).value();
        context = reopened.get();
    }
    Result<TrackPtr> track = first_visual_track(context);
    if (!track)
        return track.error();
    const PixelFormat output_format = options.output_format.value_or(kRgba8);
    const heif_chroma chroma = output_format == kRgba16 ? heif_chroma_interleaved_RRGGBBAA_LE
                                                        : heif_chroma_interleaved_RGBA;
    StopContext stop_context{stop};
    DecodeOptionsPtr decoding(heif_decoding_options_alloc());
    if (!decoding) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate HEIF sequence decoding options.", "libheif");
    }
    decoding->strict_decoding = 1;
    decoding->cancel_decoding = cancel_heif_decode;
    decoding->progress_user_data = &stop_context;
    decoding->ignore_sequence_editlist = 1;

    Result<void> status = sink.begin(sequence.info);
    if (!status)
        return status;
    std::uint32_t decoded_index = 0;
    std::uint32_t sink_index = 0;
    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        heif_image* raw = nullptr;
        const heif_error error = heif_track_decode_next_image(
            track.value().get(), &raw, heif_colorspace_RGB, chroma, decoding.get());
        if (error.code == heif_error_End_of_sequence)
            break;
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::decode_failed, "HEIF sequence frame decoding");
        }
        ImagePtr decoded(raw);
        if (decoded_index >= options.limits.maximum_frames) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "HEIF sequence exceeds the configured frame limit.", "libheif");
        }
        const bool selected = !options.frame_index || *options.frame_index == decoded_index;
        if (selected) {
            if (sink_index >= sequence.info.frames.size()) {
                return Status::error(ErrorCode::corrupt_data,
                                     "HEIF sequence contains more frames than its timing metadata.",
                                     "libheif");
            }
            const int raw_width = heif_image_get_primary_width(decoded.get());
            const int raw_height = heif_image_get_primary_height(decoded.get());
            if (raw_width <= 0 || raw_height <= 0) {
                return Status::error(ErrorCode::corrupt_data,
                                     "HEIF sequence frame dimensions are invalid.", "libheif");
            }
            std::uint32_t width = static_cast<std::uint32_t>(raw_width);
            std::uint32_t height = static_cast<std::uint32_t>(raw_height);
            Result<void> dimensions = validate_dimensions(width, height, options.limits);
            if (!dimensions)
                return dimensions.error();
            const auto target = scaled_size(width, height, options);
            if (target.first != width || target.second != height) {
                heif_image* scaled_raw = nullptr;
                const heif_error scale_error = heif_image_scale_image(
                    decoded.get(), &scaled_raw, static_cast<int>(target.first),
                    static_cast<int>(target.second), nullptr);
                if (scale_error.code != heif_error_Ok) {
                    return heif_status(scale_error, ErrorCode::decode_failed,
                                       "HEIF sequence frame scaling");
                }
                decoded.reset(scaled_raw);
                width = target.first;
                height = target.second;
            }
            const FrameInfo& frame = sequence.info.frames[sink_index];
            if (frame.width != width || frame.height != height ||
                frame.native_format != output_format) {
                return Status::error(
                    ErrorCode::corrupt_data,
                    "HEIF sequence frame geometry disagrees with its track metadata.", "libheif");
            }
            status = sink.begin_frame(sink_index, frame);
            if (!status)
                return status;
            status = publish_packed_heif_image(decoded.get(), output_format, width, height,
                                               sink_index, sink, options.limits, stop);
            if (!status)
                return status;
            status = sink.end_frame(sink_index);
            if (!status)
                return status;
            ++sink_index;
        }
        ++decoded_index;
        if (options.frame_index && decoded_index > *options.frame_index)
            break;
    }
    if (sink_index != sequence.info.frames.size()) {
        return Status::error(
            options.frame_index ? ErrorCode::invalid_argument : ErrorCode::corrupt_data,
            options.frame_index ? "Requested HEIF sequence frame index is out of range."
                                : "HEIF sequence contains fewer frames than its timing metadata.",
            "libheif");
    }
    return sink.end();
}

bool is_sequence_document(const Document& document) {
    return document.frames.size() > 1 &&
           std::any_of(document.frames.begin(), document.frames.end(),
                       [](const Frame& frame) { return frame.duration.count() > 0; });
}

Result<void> configure_heif_image(heif_image* image, const ColorEncoding& color, bool include_alpha,
                                  AlphaMode alpha_mode, bool lossless_rgb) {
    heif_image_set_premultiplied_alpha(
        image, include_alpha && alpha_mode == AlphaMode::premultiplied ? 1 : 0);
    if (!color.icc_profile.empty()) {
        const heif_error error = heif_image_set_raw_color_profile(
            image, "prof", color.icc_profile.data(), color.icc_profile.size());
        if (error.code != heif_error_Ok)
            return heif_status(error, ErrorCode::encode_failed, "HEIF ICC profile assignment");
    }
    NclxPtr nclx(heif_nclx_color_profile_alloc());
    if (nclx && (lossless_rgb || color.primaries != ColorPrimaries::unknown ||
                 color.transfer != TransferFunction::unknown)) {
        nclx->color_primaries = color.primaries == ColorPrimaries::rec2020
                                    ? heif_color_primaries_ITU_R_BT_2020_2_and_2100_0
                                : color.primaries == ColorPrimaries::display_p3
                                    ? heif_color_primaries_SMPTE_EG_432_1
                                    : heif_color_primaries_ITU_R_BT_709_5;
        nclx->transfer_characteristics = color.transfer == TransferFunction::linear
                                             ? heif_transfer_characteristic_linear
                                         : color.transfer == TransferFunction::pq
                                             ? heif_transfer_characteristic_ITU_R_BT_2100_0_PQ
                                         : color.transfer == TransferFunction::hlg
                                             ? heif_transfer_characteristic_ITU_R_BT_2100_0_HLG
                                             : heif_transfer_characteristic_IEC_61966_2_1;
        nclx->matrix_coefficients =
            lossless_rgb ? heif_matrix_coefficients_RGB_GBR
            : color.primaries == ColorPrimaries::rec2020
                ? heif_matrix_coefficients_ITU_R_BT_2020_2_non_constant_luminance
                : heif_matrix_coefficients_ITU_R_BT_709_5;
        nclx->full_range_flag = 1;
        const heif_error error = heif_image_set_nclx_color_profile(image, nclx.get());
        if (error.code != heif_error_Ok)
            return heif_status(error, ErrorCode::encode_failed, "HEIF NCLX profile assignment");
    }
    return {};
}

Result<MutableImagePtr> make_heif_image(const ImageView& view, const ColorEncoding& color,
                                        bool include_alpha, bool lossless_rgb = false) {
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        view.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF input dimensions exceed libheif limits.", "libheif");
    }
    const bool sixteen_bit = view.format.sample_type == SampleType::unsigned_integer &&
                             view.format.bits_per_channel == 16;
    if (sixteen_bit && view.format.channels != ChannelLayout::rgba) {
        return Status::error(ErrorCode::unsupported_feature,
                             "16-bit HEIF encoding currently requires RGBA pixels.", "libheif");
    }
    if (!sixteen_bit && (view.format.sample_type != SampleType::unsigned_integer ||
                         view.format.bits_per_channel != 8)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "HEIF encoding supports packed 8-bit pixels or RGBA16 pixels.",
                             "libheif");
    }
    const heif_chroma chroma = sixteen_bit     ? include_alpha ? heif_chroma_interleaved_RRGGBBAA_LE
                                                               : heif_chroma_interleaved_RRGGBB_LE
                               : include_alpha ? heif_chroma_interleaved_RGBA
                                               : heif_chroma_interleaved_RGB;
    heif_image* raw = nullptr;
    heif_error error =
        heif_image_create(static_cast<int>(view.width), static_cast<int>(view.height),
                          heif_colorspace_RGB, chroma, &raw);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF image allocation");
    }
    MutableImagePtr image(raw);
    error =
        heif_image_add_plane(image.get(), heif_channel_interleaved, static_cast<int>(view.width),
                             static_cast<int>(view.height), sixteen_bit ? 16 : 8);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF image-plane allocation");
    }
    std::size_t destination_stride = 0;
    std::uint8_t* destination =
        heif_image_get_plane2(image.get(), heif_channel_interleaved, &destination_stride);
    const std::size_t output_channels = include_alpha ? 4U : 3U;
    const std::size_t row_bytes =
        static_cast<std::size_t>(view.width) * output_channels * (sixteen_bit ? 2U : 1U);
    if (!destination || destination_stride < row_bytes) {
        return Status::error(ErrorCode::encode_failed,
                             "libheif returned an invalid encoding image plane.", "libheif");
    }
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const std::byte* source =
            view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride;
        std::uint8_t* row = destination + static_cast<std::size_t>(y) * destination_stride;
        if (sixteen_bit) {
            if (include_alpha && view.format.little_endian) {
                std::memcpy(row, source, row_bytes);
            } else {
                for (std::uint32_t x = 0; x < view.width; ++x) {
                    for (std::size_t channel = 0; channel < output_channels; ++channel) {
                        const std::size_t input = (static_cast<std::size_t>(x) * 4U + channel) * 2U;
                        const std::size_t output =
                            (static_cast<std::size_t>(x) * output_channels + channel) * 2U;
                        row[output] = static_cast<std::uint8_t>(
                            source[input + (view.format.little_endian ? 0U : 1U)]);
                        row[output + 1U] = static_cast<std::uint8_t>(
                            source[input + (view.format.little_endian ? 1U : 0U)]);
                    }
                }
            }
            continue;
        }
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t output = static_cast<std::size_t>(x) * output_channels;
            switch (view.format.channels) {
            case ChannelLayout::gray: {
                const std::uint8_t value = static_cast<std::uint8_t>(source[x]);
                row[output] = value;
                row[output + 1] = value;
                row[output + 2] = value;
                if (include_alpha)
                    row[output + 3] = 255;
                break;
            }
            case ChannelLayout::gray_alpha: {
                const std::size_t input = static_cast<std::size_t>(x) * 2U;
                const std::uint8_t value = static_cast<std::uint8_t>(source[input]);
                row[output] = value;
                row[output + 1] = value;
                row[output + 2] = value;
                if (include_alpha)
                    row[output + 3] = static_cast<std::uint8_t>(source[input + 1]);
                break;
            }
            case ChannelLayout::rgb:
            case ChannelLayout::bgr: {
                const std::size_t input = static_cast<std::size_t>(x) * 3U;
                const bool bgr = view.format.channels == ChannelLayout::bgr;
                row[output] = static_cast<std::uint8_t>(source[input + (bgr ? 2U : 0U)]);
                row[output + 1] = static_cast<std::uint8_t>(source[input + 1]);
                row[output + 2] = static_cast<std::uint8_t>(source[input + (bgr ? 0U : 2U)]);
                if (include_alpha)
                    row[output + 3] = 255;
                break;
            }
            case ChannelLayout::rgba:
            case ChannelLayout::bgra: {
                const std::size_t input = static_cast<std::size_t>(x) * 4U;
                const bool bgra = view.format.channels == ChannelLayout::bgra;
                row[output] = static_cast<std::uint8_t>(source[input + (bgra ? 2U : 0U)]);
                row[output + 1] = static_cast<std::uint8_t>(source[input + 1]);
                row[output + 2] = static_cast<std::uint8_t>(source[input + (bgra ? 0U : 2U)]);
                if (include_alpha)
                    row[output + 3] = static_cast<std::uint8_t>(source[input + 3]);
                break;
            }
            case ChannelLayout::cmyk:
            case ChannelLayout::indexed:
                return Status::error(ErrorCode::unsupported_feature,
                                     "HEIF encoding does not accept CMYK or indexed pixels.",
                                     "libheif");
            }
        }
    }
    Result<void> configured =
        configure_heif_image(image.get(), color, include_alpha, view.format.alpha, lossless_rgb);
    if (!configured)
        return configured.error();
    return image;
}

Result<MutableImagePtr> make_heif_raster_image(const RasterSource& source,
                                               const RasterFrameDescriptor& frame,
                                               const ColorEncoding& color, bool include_alpha,
                                               bool lossless_rgb, std::stop_token stop) {
    const PlaneDescriptor& plane = frame.layout.planes.front();
    if (plane.format != kRgba8 ||
        frame.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        frame.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Native HEIF raster encoding requires RGBA8 pixels.", "libheif");
    }
    const heif_chroma chroma =
        include_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
    heif_image* raw = nullptr;
    heif_error error =
        heif_image_create(static_cast<int>(frame.width), static_cast<int>(frame.height),
                          heif_colorspace_RGB, chroma, &raw);
    if (error.code != heif_error_Ok)
        return heif_status(error, ErrorCode::encode_failed, "HEIF image allocation");
    MutableImagePtr image(raw);
    error = heif_image_add_plane(image.get(), heif_channel_interleaved,
                                 static_cast<int>(frame.width), static_cast<int>(frame.height), 8);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF image-plane allocation");
    }
    std::size_t destination_stride = 0;
    std::uint8_t* destination =
        heif_image_get_plane2(image.get(), heif_channel_interleaved, &destination_stride);
    const std::size_t source_row_bytes = static_cast<std::size_t>(frame.width) * 4U;
    const std::size_t destination_row_bytes =
        static_cast<std::size_t>(frame.width) * (include_alpha ? 4U : 3U);
    if (destination == nullptr || destination_stride < destination_row_bytes) {
        return Status::error(ErrorCode::encode_failed,
                             "libheif returned an invalid encoding image plane.", "libheif");
    }
    try {
        std::vector<std::byte> source_row(source_row_bytes);
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            if (stop.stop_requested())
                return cancelled_status();
            Result<void> read = source.read_rows(0, 0, y, 1, source_row_bytes, source_row, stop);
            if (!read)
                return read.error();
            std::uint8_t* output = destination + static_cast<std::size_t>(y) * destination_stride;
            if (include_alpha) {
                std::memcpy(output, source_row.data(), source_row_bytes);
                continue;
            }
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                std::memcpy(output + static_cast<std::size_t>(x) * 3U,
                            source_row.data() + static_cast<std::size_t>(x) * 4U, 3U);
            }
        }
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate a HEIF raster input row.", "libheif");
    }
    Result<void> configured =
        configure_heif_image(image.get(), color, include_alpha, plane.format.alpha, lossless_rgb);
    if (!configured)
        return configured.error();
    return image;
}

Result<void> add_metadata(heif_context* context, const heif_image_handle* handle,
                          const Metadata& metadata, const EncodeOptions& options) {
    if (!options.preserve_metadata)
        return {};
    const auto checked_size = [](std::size_t size) -> Result<int> {
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "HEIF metadata block exceeds libheif limits.", "libheif");
        }
        return static_cast<int>(size);
    };
    if (!metadata.exif.empty()) {
        Result<int> size = checked_size(metadata.exif.size());
        if (!size)
            return size.error();
        const heif_error error =
            heif_context_add_exif_metadata(context, handle, metadata.exif.data(), size.value());
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::encode_failed, "HEIF Exif assignment");
        }
    }
    if (!metadata.xmp.empty()) {
        Result<int> size = checked_size(metadata.xmp.size());
        if (!size)
            return size.error();
        const heif_error error =
            heif_context_add_XMP_metadata(context, handle, metadata.xmp.data(), size.value());
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::encode_failed, "HEIF XMP assignment");
        }
    }
    for (const MetadataBlock& block : metadata.blocks) {
        if (block.type.size() != 4) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "Generic HEIF metadata types must be four-character item types.",
                                 "libheif");
        }
        Result<int> size = checked_size(block.data.size());
        if (!size)
            return size.error();
        const char* content = block.content_type.empty() ? nullptr : block.content_type.c_str();
        const heif_error error = heif_context_add_generic_metadata(
            context, handle, block.data.data(), size.value(), block.type.c_str(), content);
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::encode_failed, "Generic HEIF metadata assignment");
        }
    }
    return {};
}

Result<EncoderPtr> make_encoder(heif_context* context, Format format,
                                const EncodeOptions& options) {
    heif_encoder* raw = nullptr;
    const heif_compression_format compression =
        format == Format::avif ? heif_compression_AV1 : heif_compression_HEVC;
    heif_error error = heif_context_get_encoder_for_format(context, compression, &raw);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::codec_unavailable, "HEIF encoder selection");
    }
    EncoderPtr encoder(raw);
    error = heif_encoder_set_lossless(encoder.get(), options.lossless ? 1 : 0);
    if (error.code == heif_error_Ok && !options.lossless) {
        error = heif_encoder_set_lossy_quality(encoder.get(), std::clamp(options.quality, 0, 100));
    }
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF encoder configuration");
    }
    // Lossless AV1 quantization also needs an identity color transform and no chroma
    // subsampling to preserve RGB values, as in libheif's lossless encoder example.
    if (format == Format::avif && options.lossless) {
        error = heif_encoder_set_parameter_string(encoder.get(), "chroma", "444");
        if (error.code != heif_error_Ok)
            return heif_status(error, ErrorCode::encode_failed,
                               "AVIF lossless chroma configuration");
    }
    const heif_encoder_parameter* const* parameters = heif_encoder_list_parameters(encoder.get());
    if (parameters) {
        for (std::size_t index = 0; parameters[index] != nullptr; ++index) {
            const char* parameter_name = heif_encoder_parameter_get_name(parameters[index]);
            if (!parameter_name)
                continue;
            const std::string_view name(parameter_name);
            const heif_encoder_parameter_type type =
                heif_encoder_parameter_get_type(parameters[index]);
            if (name == "speed" && type == heif_encoder_parameter_type_integer) {
                error = heif_encoder_set_parameter_integer(encoder.get(), "speed",
                                                           std::clamp(9 - options.effort, 0, 9));
                if (error.code != heif_error_Ok) {
                    return heif_status(error, ErrorCode::encode_failed,
                                       "HEIF encoder speed configuration");
                }
            } else if (format == Format::avif && name == "enable-intrabc" &&
                       type == heif_encoder_parameter_type_boolean) {
                // libheif's AOM plugin enables intrablock-copy by default. Its full-frame
                // hash table can exhaust a bounded encoder process before AVIF encoding begins.
                error = heif_encoder_set_parameter_boolean(encoder.get(), "enable-intrabc", 0);
                if (error.code != heif_error_Ok) {
                    return heif_status(error, ErrorCode::encode_failed,
                                       "AVIF encoder intrablock-copy configuration");
                }
            }
        }
    }
    return encoder;
}

const ColorEncoding& effective_color(const Document& document, const Frame& frame) {
    return frame.color.icc_profile.empty() && frame.color.primaries == ColorPrimaries::unknown &&
                   frame.color.transfer == TransferFunction::unknown
               ? document.color
               : frame.color;
}

const Metadata& effective_metadata(const Document& document, const Frame& frame) {
    return frame.metadata.exif.empty() && frame.metadata.xmp.empty() &&
                   frame.metadata.blocks.empty() && frame.metadata.comment.empty()
               ? document.metadata
               : frame.metadata;
}

const ColorEncoding& effective_color(const DocumentDescriptor& document,
                                     const RasterFrameDescriptor& frame) {
    return frame.color.icc_profile.empty() && frame.color.primaries == ColorPrimaries::unknown &&
                   frame.color.transfer == TransferFunction::unknown
               ? document.color
               : frame.color;
}

const Metadata& effective_metadata(const DocumentDescriptor& document,
                                   const RasterFrameDescriptor& frame) {
    return frame.metadata.exif.empty() && frame.metadata.xmp.empty() &&
                   frame.metadata.blocks.empty() && frame.metadata.comment.empty()
               ? document.metadata
               : frame.metadata;
}

Result<void> encode_still(heif_context* context, heif_encoder* encoder, const Document& document,
                          const Frame& frame, const EncodeOptions& options, bool preserve_metadata,
                          std::stop_token stop, HandlePtr* output_handle = nullptr) {
    Result<AlphaContent> alpha = options.verified_alpha_content
                                     ? Result<AlphaContent>(*options.verified_alpha_content)
                                     : classify_alpha(frame.image, stop);
    if (!alpha)
        return alpha.error();
    const bool include_alpha = alpha.value() == AlphaContent::non_opaque;
    const bool lossless_rgb = options.format == Format::avif && options.lossless;
    Result<MutableImagePtr> image = make_heif_image(
        frame.image.view(), effective_color(document, frame), include_alpha, lossless_rgb);
    if (!image)
        return image.error();
    EncodeOptionsPtr encoding(heif_encoding_options_alloc());
    if (!encoding) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate HEIF encoding options.",
                             "libheif");
    }
    encoding->save_alpha_channel = include_alpha ? 1 : 0;
    encoding->save_two_colr_boxes_when_ICC_and_nclx_available = 1;
    NclxPtr lossless_profile;
    if (lossless_rgb) {
        heif_color_profile_nclx* profile = nullptr;
        const auto error = heif_image_get_nclx_color_profile(image.value().get(), &profile);
        if (error.code != heif_error_Ok)
            return heif_status(error, ErrorCode::encode_failed, "AVIF lossless color profile");
        lossless_profile.reset(profile);
        encoding->output_nclx_profile = lossless_profile.get();
    }
    encoding->image_orientation = static_cast<heif_orientation>(
        preserve_metadata ? effective_metadata(document, frame).orientation
                          : Orientation::identity);
    heif_image_handle* raw_handle = nullptr;
    heif_error error = heif_context_encode_image(context, image.value().get(), encoder,
                                                 encoding.get(), &raw_handle);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF image encoding");
    }
    HandlePtr handle(raw_handle);
    if (preserve_metadata) {
        Result<void> metadata =
            add_metadata(context, handle.get(), effective_metadata(document, frame), options);
        if (!metadata)
            return metadata;
    }
    if (output_handle)
        *output_handle = std::move(handle);
    return {};
}

Result<void> encode_sequence(heif_context* context, heif_encoder* encoder, const Document& document,
                             const EncodeOptions& options, std::stop_token stop) {
    const Frame& first = document.frames.front();
    if (first.image.width() > std::numeric_limits<std::uint16_t>::max() ||
        first.image.height() > std::numeric_limits<std::uint16_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF sequence dimensions exceed the track format limit.", "libheif");
    }
    for (const Frame& frame : document.frames) {
        if (frame.image.width() != first.image.width() ||
            frame.image.height() != first.image.height() || frame.x != 0 || frame.y != 0) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "HEIF sequence frames must be full-canvas images of equal size.",
                                 "libheif");
        }
    }
    bool encode_alpha = false;
    if (options.verified_alpha_content) {
        encode_alpha = *options.verified_alpha_content == AlphaContent::non_opaque;
    } else {
        for (const Frame& frame : document.frames) {
            Result<AlphaContent> alpha = classify_alpha(frame.image, stop);
            if (!alpha)
                return alpha.error();
            if (alpha.value() == AlphaContent::non_opaque) {
                encode_alpha = true;
                break;
            }
        }
    }
    TrackOptionsPtr track_options(heif_track_options_alloc());
    SequenceOptionsPtr sequence_options(heif_sequence_encoding_options_alloc());
    if (!track_options || !sequence_options) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate HEIF sequence options.",
                             "libheif");
    }
    constexpr std::uint32_t kTimescale = 1'000'000;
    heif_track_options_set_timescale(track_options.get(), kTimescale);
    sequence_options->save_alpha_channel = encode_alpha ? 1 : 0;
    // Intra-only keeps image-sequence sample order and per-sample timing exact across
    // libheif plugins. Inter-frame GOPs can reorder frames and libheif 1.21 associates
    // the final decoded sample with the preceding timing entry in that mode.
    sequence_options->gop_structure = heif_sequence_gop_structure_intra_only;
    heif_track* raw_track = nullptr;
    heif_error error = heif_context_add_visual_sequence_track(
        context, static_cast<std::uint16_t>(first.image.width()),
        static_cast<std::uint16_t>(first.image.height()), heif_track_type_image_sequence,
        track_options.get(), sequence_options.get(), &raw_track);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF sequence track creation");
    }
    TrackPtr track(raw_track);
    heif_context_set_sequence_timescale(context, kTimescale);
    heif_context_set_number_of_sequence_repetitions(context, document.loop_count);
    for (const Frame& frame : document.frames) {
        if (stop.stop_requested())
            return cancelled_status();
        Result<MutableImagePtr> image =
            make_heif_image(frame.image.view(), effective_color(document, frame), encode_alpha,
                            options.format == Format::avif && options.lossless);
        if (!image)
            return image.error();
        const std::int64_t duration_ns = std::max<std::int64_t>(0, frame.duration.count());
        const std::uint64_t ticks =
            (static_cast<std::uint64_t>(duration_ns) * kTimescale + 500'000'000ULL) /
            1'000'000'000ULL;
        heif_image_set_duration(image.value().get(),
                                clamp_unsigned<std::uint32_t>(std::max<std::uint64_t>(1, ticks)));
        error = heif_track_encode_sequence_image(track.get(), image.value().get(), encoder,
                                                 sequence_options.get());
        if (error.code != heif_error_Ok) {
            return heif_status(error, ErrorCode::encode_failed, "HEIF sequence frame encoding");
        }
    }
    error = heif_track_encode_end_of_sequence(track.get(), encoder);
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF sequence finalization");
    }
    return {};
}

Result<void> add_sequence_timing(heif_context* context, const heif_image_handle* poster,
                                 const Document& document) {
    if (document.frames.size() >
        (static_cast<std::size_t>(std::numeric_limits<int>::max()) - 24U) / 8U) {
        return Status::error(ErrorCode::limit_exceeded,
                             "HEIF sequence timing metadata is too large.", "libheif");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(24U + document.frames.size() * 8U);
    bytes.insert(bytes.end(), kTimingMagic.begin(), kTimingMagic.end());
    append_u32_le(bytes, 1);
    append_u32_le(bytes, document.loop_count);
    append_u32_le(bytes, static_cast<std::uint32_t>(document.frames.size()));
    append_u32_le(bytes, 0);
    for (const Frame& frame : document.frames) {
        append_u64_le(
            bytes, static_cast<std::uint64_t>(std::max<std::int64_t>(0, frame.duration.count())));
    }
    const heif_error error = heif_context_add_generic_metadata(
        context, poster, bytes.data(), static_cast<int>(bytes.size()), "snim",
        "application/x-snow-image-sequence-timing");
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF sequence timing assignment");
    }
    return {};
}

struct WriterContext final {
    ByteSink* sink = nullptr;
    std::stop_token stop;
    Status failure;
};

heif_error write_heif_chunk(heif_context*, const void* data, std::size_t size, void* user_data) {
    auto* writer = static_cast<WriterContext*>(user_data);
    if (writer->stop.stop_requested()) {
        writer->failure = cancelled_status();
        return {heif_error_Canceled, heif_suberror_Unspecified, "Encoding cancelled"};
    }
    try {
        Result<void> written = writer->sink->write(
            std::as_bytes(std::span(static_cast<const std::uint8_t*>(data), size)));
        if (!written) {
            writer->failure = written.error();
            return {heif_error_Encoding_error, heif_suberror_Unspecified, "Output write failed"};
        }
    } catch (...) {
        writer->failure = Status::error(ErrorCode::io_error,
                                        "Output sink threw while writing HEIF data.", "libheif");
        return {heif_error_Encoding_error, heif_suberror_Unspecified, "Output write threw"};
    }
    return heif_error_success;
}

} // namespace

CodecCapability HeifCodec::capabilities() const noexcept {
    CodecCapability result = CodecCapability::inspect | CodecCapability::decode |
                             CodecCapability::animation | CodecCapability::multiple_images |
                             CodecCapability::streaming_decode | CodecCapability::metadata_decode |
                             CodecCapability::hdr;
    const heif_compression_format compression =
        format_ == Format::avif ? heif_compression_AV1 : heif_compression_HEVC;
    if (heif_have_encoder_for_format(compression))
        result = result | CodecCapability::encode;
    return result;
}

int HeifCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 12 &&
        header.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        const char* mime = heif_get_file_mime_type(
            reinterpret_cast<const std::uint8_t*>(header.data()), static_cast<int>(header.size()));
        const std::string_view type = mime ? mime : "";
        const bool avif = type == "image/avif" || type == "image/avif-sequence";
        const bool heif = type == "image/heic" || type == "image/heif" ||
                          type == "image/heic-sequence" || type == "image/heif-sequence";
        if ((format_ == Format::avif && avif) || (format_ == Format::heif && heif))
            return 95;
        if (avif || heif)
            return 0;
    }
    return format_from_extension(name_hint) == format_ ? 10 : 0;
}

Result<DocumentInfo> HeifCodec::inspect(const Input& input, const DecodeOptions& options,
                                        std::stop_token stop) const {
    Result<ParsedHeif> parsed = parse_heif(input, options, stop);
    if (!parsed)
        return parsed.error();
    if (heif_context_has_sequence(parsed.value().context.get())) {
        Result<SequenceInspection> sequence =
            inspect_sequence_frames(parsed.value(), format_, options, stop);
        if (!sequence)
            return sequence.error();
        return std::move(sequence).value().info;
    }
    return inspect_collection(parsed.value(), format_, options);
}

Result<DocumentDescriptor> HeifCodec::inspect_raster(const Input& input,
                                                     const DecodeOptions& options,
                                                     std::stop_token stop) const {
    if (options.raster_layout != RasterLayoutPolicy::native || options.output_format)
        return Codec::inspect_raster(input, options, stop);
    Result<ParsedHeif> parsed = parse_heif(input, options, stop);
    if (!parsed)
        return parsed.error();
    if (heif_context_has_sequence(parsed.value().context.get()))
        return Codec::inspect_raster(input, options, stop);
    Result<std::vector<heif_item_id>> ids =
        top_level_ids(parsed.value().context.get(), options.limits);
    if (!ids)
        return ids.error();
    if (options.frame_index && *options.frame_index >= ids.value().size())
        return Status::error(ErrorCode::invalid_argument,
                             "Requested HEIF collection image index is out of range.", "libheif");
    if (!options.frame_index && ids.value().size() != 1)
        return Codec::inspect_raster(input, options, stop);
    const std::size_t selected = options.frame_index ? *options.frame_index : 0;
    Result<HandlePtr> handle = image_handle(parsed.value().context.get(), ids.value()[selected]);
    if (!handle)
        return handle.error();
    Result<ImageDescription> description =
        describe_handle(parsed.value().context.get(), handle.value().get(), options);
    if (!description)
        return description.error();
    if (!native_heif_supported(description.value()))
        return Codec::inspect_raster(input, options, stop);
    return native_heif_descriptor(format_, description.value(), options);
}

Result<Document> HeifCodec::decode(const Input& input, const DecodeOptions& options,
                                   std::stop_token stop) const {
    Result<ParsedHeif> parsed = parse_heif(input, options, stop);
    if (!parsed)
        return parsed.error();
    if (heif_context_has_sequence(parsed.value().context.get())) {
        return decode_sequence(parsed.value(), format_, options, stop);
    }
    return decode_collection(parsed.value(), format_, options, stop);
}

Result<void> HeifCodec::decode_into(const Input& input, RasterWriter& writer,
                                    const DecodeOptions& options, std::stop_token stop) const {
    if (options.raster_layout != RasterLayoutPolicy::native || options.output_format)
        return Codec::decode_into(input, writer, options, stop);
    Result<ParsedHeif> parsed = parse_heif(input, options, stop);
    if (!parsed)
        return parsed.error();
    if (heif_context_has_sequence(parsed.value().context.get()))
        return Codec::decode_into(input, writer, options, stop);
    Result<std::vector<heif_item_id>> ids =
        top_level_ids(parsed.value().context.get(), options.limits);
    if (!ids)
        return ids.error();
    if (options.frame_index && *options.frame_index >= ids.value().size())
        return Status::error(ErrorCode::invalid_argument,
                             "Requested HEIF collection image index is out of range.", "libheif");
    if (!options.frame_index && ids.value().size() != 1)
        return Codec::decode_into(input, writer, options, stop);
    const std::size_t selected = options.frame_index ? *options.frame_index : 0;
    Result<HandlePtr> handle = image_handle(parsed.value().context.get(), ids.value()[selected]);
    if (!handle)
        return handle.error();
    Result<ImageDescription> description =
        describe_handle(parsed.value().context.get(), handle.value().get(), options);
    if (!description)
        return description.error();
    if (!native_heif_supported(description.value()))
        return Codec::decode_into(input, writer, options, stop);
    Result<DocumentDescriptor> descriptor =
        native_heif_descriptor(format_, description.value(), options);
    if (!descriptor)
        return descriptor.error();
    if (!matching_descriptor(writer.descriptor(), descriptor.value()))
        return Status::error(ErrorCode::invalid_argument,
                             "HEIF native raster writer does not match the decoded plane layout.",
                             "libheif");
    Result<ImagePtr> decoded =
        decode_native_heif_image(handle.value().get(), description.value(), options, stop);
    if (!decoded)
        return decoded.error();
    if (heif_image_get_colorspace(decoded.value().get()) !=
            description.value().preferred_colorspace ||
        heif_image_get_chroma_format(decoded.value().get()) != description.value().preferred_chroma)
        return Status::error(ErrorCode::decode_failed,
                             "libheif changed the requested native plane layout.", "libheif");
    const RasterFrameDescriptor& frame = descriptor.value().frames.front();
    constexpr std::array channels{heif_channel_Y, heif_channel_Cb, heif_channel_Cr,
                                  heif_channel_Alpha};
    std::vector<ReadablePlaneView> planes;
    planes.reserve(frame.layout.planes.size());
    for (std::size_t index = 0; index < frame.layout.planes.size(); ++index) {
        const PlaneDescriptor& plane = frame.layout.planes[index];
        const heif_channel channel = channels[index];
        const int width = heif_image_get_width(decoded.value().get(), channel);
        const int height = heif_image_get_height(decoded.value().get(), channel);
        const int bits = heif_image_get_bits_per_pixel_range(decoded.value().get(), channel);
        std::size_t stride = 0;
        const std::uint8_t* pixels =
            heif_image_get_plane_readonly2(decoded.value().get(), channel, &stride);
        if (!pixels || width <= 0 || height <= 0 || bits <= 0 || bits > 8 ||
            static_cast<std::uint32_t>(width) != plane.width ||
            static_cast<std::uint32_t>(height) != plane.height || stride < plane.width ||
            plane.height > std::numeric_limits<std::size_t>::max() / stride)
            return Status::error(ErrorCode::decode_failed,
                                 "libheif returned an invalid native plane.", "libheif");
        planes.push_back(
            {std::span(reinterpret_cast<const std::byte*>(pixels), stride * plane.height), stride});
    }
    return publish_readable_plane_views(writer, 0, frame, planes, stop, true, "libheif");
}

Result<void> HeifCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                       const DecodeOptions& options, std::stop_token stop) const {
    Result<ParsedHeif> parsed = parse_heif(input, options, stop);
    if (!parsed)
        return parsed.error();
    if (heif_context_has_sequence(parsed.value().context.get())) {
        return decode_sequence_to_sink(parsed.value(), format_, sink, options, stop);
    }
    Result<std::vector<heif_item_id>> ids =
        top_level_ids(parsed.value().context.get(), options.limits);
    if (!ids)
        return ids.error();
    if (options.frame_index && *options.frame_index >= ids.value().size()) {
        return Status::error(ErrorCode::invalid_argument,
                             "Requested HEIF collection image index is out of range.", "libheif");
    }
    const std::size_t first = options.frame_index ? *options.frame_index : 0;
    const std::size_t end = options.frame_index ? first + 1 : ids.value().size();
    std::vector<HandlePtr> handles;
    std::vector<ImageDescription> descriptions;
    handles.reserve(end - first);
    descriptions.reserve(end - first);
    DocumentInfo info;
    info.format = format_;
    for (std::size_t index = first; index < end; ++index) {
        if (stop.stop_requested())
            return cancelled_status();
        Result<HandlePtr> handle = image_handle(parsed.value().context.get(), ids.value()[index]);
        if (!handle)
            return handle.error();
        Result<ImageDescription> description =
            describe_handle(parsed.value().context.get(), handle.value().get(), options);
        if (!description)
            return description.error();
        const auto target =
            scaled_size(description.value().width, description.value().height, options);
        Result<std::size_t> bytes_per_pixel = description.value().format.bytes_per_pixel();
        if (!bytes_per_pixel)
            return bytes_per_pixel.error();
        const std::uint64_t frame_bytes =
            static_cast<std::uint64_t>(target.first) * target.second * bytes_per_pixel.value();
        if (frame_bytes > options.limits.maximum_working_bytes) {
            return Status::error(
                ErrorCode::limit_exceeded,
                "One decoded HEIF image exceeds the configured working-memory limit.", "libheif");
        }
        FrameInfo frame{target.first,
                        target.second,
                        0,
                        0,
                        std::chrono::nanoseconds{0},
                        description.value().format,
                        description.value().has_alpha,
                        std::nullopt};
        frame.color = description.value().color;
        frame.metadata = description.value().metadata;
        info.canvas_width = std::max(info.canvas_width, target.first);
        info.canvas_height = std::max(info.canvas_height, target.second);
        if (info.frames.empty()) {
            info.metadata = description.value().metadata;
            info.color = description.value().color;
        }
        info.frames.push_back(std::move(frame));
        handles.push_back(std::move(handle).value());
        descriptions.push_back(std::move(description).value());
    }
    Result<void> status = sink.begin(info);
    if (!status)
        return status;
    for (std::size_t index = 0; index < handles.size(); ++index) {
        const auto sink_index = static_cast<std::uint32_t>(index);
        status = sink.begin_frame(sink_index, info.frames[sink_index]);
        if (!status)
            return status;
        Result<ImagePtr> decoded = decode_packed_heif_image(
            handles[sink_index].get(), descriptions[sink_index], options, stop);
        if (!decoded)
            return decoded.error();
        status = publish_packed_heif_image(
            decoded.value().get(), descriptions[sink_index].format, info.frames[sink_index].width,
            info.frames[sink_index].height, sink_index, sink, options.limits, stop);
        if (!status)
            return status;
        status = sink.end_frame(sink_index);
        if (!status)
            return status;
    }
    return sink.end();
}

Result<EncodedArtifactReceipt> HeifCodec::encode_to_sink(const Document& document,
                                                         const Output& output,
                                                         const EncodeOptions& options,
                                                         std::stop_token stop) const {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument,
                             "HEIF encoding requires at least one image.", "libheif");
    }
    if (stop.stop_requested())
        return cancelled_status();
    ContextPtr context(heif_context_alloc());
    if (!context) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a HEIF context.",
                             "libheif");
    }
    Result<EncoderPtr> encoder = make_encoder(context.get(), format_, options);
    if (!encoder)
        return encoder.error();
    const bool sequence = is_sequence_document(document);
    heif_context_set_major_brand(
        context.get(), format_ == Format::avif ? (sequence ? heif_brand2_avis : heif_brand2_avif)
                                               : (sequence ? heif_brand2_hevc : heif_brand2_heic));
    if (sequence) {
        HandlePtr poster;
        Result<void> poster_status =
            encode_still(context.get(), encoder.value().get(), document, document.frames.front(),
                         options, options.preserve_metadata, stop, &poster);
        if (!poster_status)
            return poster_status.error();
        Result<void> timing_status = add_sequence_timing(context.get(), poster.get(), document);
        if (!timing_status)
            return timing_status.error();
        Result<void> sequence_status =
            encode_sequence(context.get(), encoder.value().get(), document, options, stop);
        if (!sequence_status)
            return sequence_status.error();
    } else {
        for (const Frame& frame : document.frames) {
            if (stop.stop_requested())
                return cancelled_status();
            Result<void> encoded = encode_still(context.get(), encoder.value().get(), document,
                                                frame, options, options.preserve_metadata, stop);
            if (!encoded)
                return encoded.error();
        }
    }
    heif_writer writer{};
    writer.writer_api_version = 1;
    writer.write = write_heif_chunk;
    WriterContext writer_context{output.sink.get(), stop, {}};
    const heif_error error = heif_context_write(context.get(), &writer, &writer_context);
    if (!writer_context.failure.ok())
        return writer_context.failure;
    if (error.code != heif_error_Ok) {
        return heif_status(error, ErrorCode::encode_failed, "HEIF file writing");
    }
    return receipt_for_document(document, format());
}

Result<EncodedArtifactReceipt> HeifCodec::encode_raster_to_sink(const RasterSource& source,
                                                                const Output& output,
                                                                const EncodeOptions& options,
                                                                std::stop_token stop) const {
    const DocumentDescriptor& descriptor = source.descriptor();
    if (raster_encode_route(descriptor, options) != RasterEncodeRoute::native)
        return Codec::encode_raster_to_sink(source, output, options, stop);
    Result<void> descriptor_status = descriptor.validate();
    if (!descriptor_status)
        return descriptor_status.error();
    if (stop.stop_requested())
        return cancelled_status();

    ContextPtr context(heif_context_alloc());
    if (!context) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a HEIF context.",
                             "libheif");
    }
    Result<EncoderPtr> encoder = make_encoder(context.get(), format_, options);
    if (!encoder)
        return encoder.error();
    heif_context_set_major_brand(context.get(),
                                 format_ == Format::avif ? heif_brand2_avif : heif_brand2_heic);

    const RasterFrameDescriptor& frame = descriptor.frames.front();
    const bool include_alpha = *options.verified_alpha_content == AlphaContent::non_opaque;
    const bool lossless_rgb = format_ == Format::avif && options.lossless;
    const ColorEncoding& color = effective_color(descriptor, frame);
    const Metadata& metadata = effective_metadata(descriptor, frame);
    Result<MutableImagePtr> image =
        make_heif_raster_image(source, frame, color, include_alpha, lossless_rgb, stop);
    if (!image)
        return image.error();

    EncodeOptionsPtr encoding(heif_encoding_options_alloc());
    if (!encoding) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate HEIF encoding options.",
                             "libheif");
    }
    encoding->save_alpha_channel = include_alpha ? 1 : 0;
    encoding->save_two_colr_boxes_when_ICC_and_nclx_available = 1;
    NclxPtr lossless_profile;
    if (lossless_rgb) {
        heif_color_profile_nclx* profile = nullptr;
        const heif_error profile_error =
            heif_image_get_nclx_color_profile(image.value().get(), &profile);
        if (profile_error.code != heif_error_Ok) {
            return heif_status(profile_error, ErrorCode::encode_failed,
                               "AVIF lossless color profile");
        }
        lossless_profile.reset(profile);
        encoding->output_nclx_profile = lossless_profile.get();
    }
    encoding->image_orientation = static_cast<heif_orientation>(
        options.preserve_metadata ? metadata.orientation : Orientation::identity);
    heif_image_handle* raw_handle = nullptr;
    const heif_error encoded = heif_context_encode_image(
        context.get(), image.value().get(), encoder.value().get(), encoding.get(), &raw_handle);
    if (encoded.code != heif_error_Ok)
        return heif_status(encoded, ErrorCode::encode_failed, "HEIF image encoding");
    HandlePtr handle(raw_handle);
    Result<void> metadata_status = add_metadata(context.get(), handle.get(), metadata, options);
    if (!metadata_status)
        return metadata_status.error();

    heif_writer writer{};
    writer.writer_api_version = 1;
    writer.write = write_heif_chunk;
    WriterContext writer_context{output.sink.get(), stop, {}};
    const heif_error written = heif_context_write(context.get(), &writer, &writer_context);
    if (!writer_context.failure.ok())
        return writer_context.failure;
    if (written.code != heif_error_Ok)
        return heif_status(written, ErrorCode::encode_failed, "HEIF file writing");
    return receipt_for_descriptor(descriptor, format());
}

RasterEncodeRoute
HeifCodec::raster_encode_route(const DocumentDescriptor& descriptor,
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
