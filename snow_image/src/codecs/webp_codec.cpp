#include "codecs/webp_codec.h"
#include "exif_orientation.h"
#include "planar_raster_io.h"

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace snow::image::internal {
namespace {

struct AnimDecoderDeleter final {
    void operator()(WebPAnimDecoder* decoder) const noexcept {
        WebPAnimDecoderDelete(decoder);
    }
};
struct AnimEncoderDeleter final {
    void operator()(WebPAnimEncoder* encoder) const noexcept {
        WebPAnimEncoderDelete(encoder);
    }
};
using AnimDecoder = std::unique_ptr<WebPAnimDecoder, AnimDecoderDeleter>;
using AnimEncoder = std::unique_ptr<WebPAnimEncoder, AnimEncoderDeleter>;

struct DemuxerDeleter final {
    void operator()(WebPDemuxer* demuxer) const noexcept {
        WebPDemuxDelete(demuxer);
    }
};
using Demuxer = std::unique_ptr<WebPDemuxer, DemuxerDeleter>;

struct MuxDeleter final {
    void operator()(WebPMux* mux) const noexcept {
        WebPMuxDelete(mux);
    }
};
using Mux = std::unique_ptr<WebPMux, MuxDeleter>;

constexpr std::uint64_t kMaximumWebpEncodeMetadataBytes = std::uint64_t{64} << 20U;

Status webp_error(ErrorCode code, const char* message) {
    return Status::error(code, message, "libwebp");
}

Status webp_decode_error(VP8StatusCode status, const char* operation) {
    switch (status) {
    case VP8_STATUS_OK:
        return {};
    case VP8_STATUS_OUT_OF_MEMORY:
        return webp_error(ErrorCode::out_of_memory, "The WebP decoder ran out of memory.");
    case VP8_STATUS_INVALID_PARAM:
        return webp_error(ErrorCode::invalid_argument,
                          "The WebP decoder configuration is invalid.");
    case VP8_STATUS_UNSUPPORTED_FEATURE:
        return webp_error(ErrorCode::unsupported_feature,
                          "The WebP bitstream uses an unsupported feature.");
    case VP8_STATUS_USER_ABORT:
        return webp_error(ErrorCode::cancelled, "The WebP decoder was cancelled.");
    case VP8_STATUS_NOT_ENOUGH_DATA:
        return webp_error(ErrorCode::truncated_data, "The WebP bitstream is truncated.");
    case VP8_STATUS_BITSTREAM_ERROR:
    case VP8_STATUS_SUSPENDED:
        return webp_error(ErrorCode::corrupt_data, operation);
    }
    return webp_error(ErrorCode::decode_failed, operation);
}

struct WebpFeatures final {
    WebPBitstreamFeatures bitstream{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Result<WebpFeatures> read_features(std::span<const std::byte> bytes, const DecodeLimits& limits) {
    WebpFeatures result;
    const VP8StatusCode status = WebPGetFeatures(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &result.bitstream);
    if (status != VP8_STATUS_OK)
        return webp_decode_error(status, "Could not inspect the WebP bitstream.");
    if (result.bitstream.width <= 0 || result.bitstream.height <= 0)
        return webp_error(ErrorCode::corrupt_data, "WebP dimensions are invalid.");
    result.width = static_cast<std::uint32_t>(result.bitstream.width);
    result.height = static_cast<std::uint32_t>(result.bitstream.height);
    Result<void> dimensions = validate_dimensions(result.width, result.height, limits);
    if (!dimensions)
        return dimensions.error();
    return result;
}

std::pair<std::uint32_t, std::uint32_t> scaled_dimensions(const WebpFeatures& features,
                                                          const DecodeOptions& options) {
    if (!options.maximum_extent || *options.maximum_extent == 0 ||
        features.bitstream.has_animation != 0 ||
        (features.width <= *options.maximum_extent && features.height <= *options.maximum_extent))
        return {features.width, features.height};
    const std::uint32_t extent = *options.maximum_extent;
    if (features.width >= features.height) {
        const std::uint32_t height = static_cast<std::uint32_t>(std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(features.height) * extent / features.width));
        return {extent, height};
    }
    const std::uint32_t width = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(features.width) * extent / features.height));
    return {width, extent};
}

bool native_planar_supported(const WebpFeatures& features) noexcept {
    return features.bitstream.has_animation == 0 && features.bitstream.format == 1;
}

Result<WEBP_CSP_MODE> packed_mode(const PixelFormat& format) {
    if (format == kRgba8)
        return MODE_RGBA;
    if (format == kBgra8)
        return MODE_BGRA;
    if (format == kRgb8)
        return MODE_RGB;
    return webp_error(ErrorCode::unsupported_feature,
                      "WebP packed decoding supports RGB8, RGBA8, and BGRA8 output.");
}

DocumentInfo static_document_info(const WebpFeatures& features, const DecodeOptions& options,
                                  const PixelFormat& format) {
    const auto [width, height] = scaled_dimensions(features, options);
    DocumentInfo document;
    document.format = Format::webp;
    document.canvas_width = width;
    document.canvas_height = height;
    document.frames.push_back({width, height, 0, 0, std::chrono::nanoseconds{0}, format,
                               features.bitstream.has_alpha != 0, std::nullopt});
    return document;
}

Result<DocumentDescriptor> native_descriptor(const WebpFeatures& features,
                                             const DecodeOptions& options) {
    if (!native_planar_supported(features))
        return webp_error(ErrorCode::unsupported_feature,
                          "This WebP bitstream has no native planar output.");
    const auto [width, height] = scaled_dimensions(features, options);
    const std::uint32_t chroma_width = (width + 1U) / 2U;
    const std::uint32_t chroma_height = (height + 1U) / 2U;
    DocumentDescriptor document;
    document.format = Format::webp;
    document.canvas_width = width;
    document.canvas_height = height;
    RasterFrameDescriptor frame;
    frame.width = width;
    frame.height = height;
    frame.layout.color_model = ColorModel::ycbcr;
    frame.layout.alpha = features.bitstream.has_alpha != 0 ? AlphaMode::straight : AlphaMode::none;
    frame.layout.chroma_subsampling = ChromaSubsampling::yuv420;
    frame.layout.color_range = ColorRange::limited;
    frame.layout.planes = {{PlaneSemantic::luma, width, height, kGray8, 8},
                           {PlaneSemantic::chroma_blue, chroma_width, chroma_height, kGray8, 8},
                           {PlaneSemantic::chroma_red, chroma_width, chroma_height, kGray8, 8}};
    if (features.bitstream.has_alpha != 0) {
        frame.layout.planes.push_back({PlaneSemantic::alpha, width, height, kGray8, 8});
    }
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

struct DecodeBufferGuard final {
    explicit DecodeBufferGuard(WebPDecBuffer* buffer) : buffer(buffer) {}
    ~DecodeBufferGuard() {
        WebPFreeDecBuffer(buffer);
    }
    WebPDecBuffer* buffer;
};

Result<void> decode_packed(std::span<const std::byte> bytes, const WebpFeatures& features,
                           const DecodeOptions& options, const PixelFormat& format,
                           std::span<std::byte> destination, std::size_t row_stride) {
    Result<WEBP_CSP_MODE> mode = packed_mode(format);
    if (!mode)
        return mode.error();
    const auto [width, height] = scaled_dimensions(features, options);
    if (row_stride > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        height > std::numeric_limits<std::size_t>::max() / row_stride ||
        destination.size() < row_stride * height)
        return webp_error(ErrorCode::invalid_argument, "WebP packed output storage is invalid.");
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config))
        return webp_error(ErrorCode::codec_unavailable, "The libwebp decoder ABI is incompatible.");
    DecodeBufferGuard guard(&config.output);
    config.input = features.bitstream;
    config.output.colorspace = mode.value();
    config.output.is_external_memory = 1;
    config.output.width = static_cast<int>(width);
    config.output.height = static_cast<int>(height);
    config.output.u.RGBA.rgba = reinterpret_cast<std::uint8_t*>(destination.data());
    config.output.u.RGBA.stride = static_cast<int>(row_stride);
    config.output.u.RGBA.size = destination.size();
    config.options.use_threads = 1;
    if (width != features.width || height != features.height) {
        config.options.use_scaling = 1;
        config.options.scaled_width = static_cast<int>(width);
        config.options.scaled_height = static_cast<int>(height);
    }
    const VP8StatusCode status =
        WebPDecode(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &config);
    return status == VP8_STATUS_OK
               ? Result<void>{}
               : webp_decode_error(status, "Could not decode the static WebP frame.");
}

Result<std::pair<AnimDecoder, WebPAnimInfo>> open_decoder(std::span<const std::byte> bytes,
                                                          const DecodeLimits& limits,
                                                          bool owning_output = true) {
    WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    WebPAnimDecoderOptions decoder_options;
    if (!WebPAnimDecoderOptionsInit(&decoder_options)) {
        return webp_error(ErrorCode::codec_unavailable,
                          "The libwebp animation decoder ABI is incompatible.");
    }
    decoder_options.color_mode = MODE_RGBA;
    decoder_options.use_threads = 1;
    AnimDecoder decoder(WebPAnimDecoderNew(&data, &decoder_options));
    if (!decoder)
        return webp_error(ErrorCode::corrupt_data, "Could not open WebP input.");
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(decoder.get(), &info)) {
        return webp_error(ErrorCode::corrupt_data, "Could not inspect WebP input.");
    }
    Result<void> dimensions = validate_dimensions(info.canvas_width, info.canvas_height, limits);
    if (!dimensions)
        return dimensions.error();
    if (info.frame_count == 0 || info.frame_count > limits.maximum_frames) {
        return webp_error(ErrorCode::limit_exceeded, "WebP frame count exceeds decode limits.");
    }
    const std::uint64_t frame_bytes =
        static_cast<std::uint64_t>(info.canvas_width) * info.canvas_height * 4U;
    if (frame_bytes > limits.maximum_working_bytes)
        return webp_error(ErrorCode::limit_exceeded,
                          "WebP animation canvas exceeds the decode working-memory limit.");
    const std::uint64_t bytes_required = frame_bytes * info.frame_count;
    if (owning_output && bytes_required > limits.maximum_owned_output_bytes) {
        return webp_error(ErrorCode::limit_exceeded, "WebP frames exceed the owning decode limit.");
    }
    return std::pair{std::move(decoder), info};
}

Result<DocumentInfo> animation_document_info(WebPAnimDecoder* decoder, const WebPAnimInfo& info) {
    const WebPDemuxer* demuxer = WebPAnimDecoderGetDemuxer(decoder);
    if (!demuxer)
        return webp_error(ErrorCode::corrupt_data, "Could not inspect WebP animation frames.");
    WebPIterator iterator{};
    if (!WebPDemuxGetFrame(demuxer, 1, &iterator))
        return webp_error(ErrorCode::corrupt_data, "WebP animation has no readable frames.");
    struct IteratorGuard final {
        explicit IteratorGuard(WebPIterator* iterator) : iterator(iterator) {}
        ~IteratorGuard() {
            WebPDemuxReleaseIterator(iterator);
        }
        WebPIterator* iterator;
    } guard(&iterator);
    DocumentInfo document;
    document.format = Format::webp;
    document.canvas_width = info.canvas_width;
    document.canvas_height = info.canvas_height;
    document.loop_count = info.loop_count;
    document.frames.reserve(info.frame_count);
    do {
        FrameInfo frame{info.canvas_width,
                        info.canvas_height,
                        0,
                        0,
                        std::chrono::milliseconds(std::max(1, iterator.duration)),
                        kRgba8,
                        true,
                        std::nullopt};
        frame.blend = FrameBlend::source;
        frame.disposal = FrameDisposal::keep;
        document.frames.push_back(std::move(frame));
    } while (WebPDemuxNextFrame(&iterator));
    if (document.frames.size() != info.frame_count)
        return webp_error(ErrorCode::truncated_data,
                          "WebP animation frame metadata is incomplete.");
    return document;
}

Result<std::vector<std::byte>> input_bytes(const Input& input, const DecodeOptions& options) {
    if (!input.source)
        return webp_error(ErrorCode::invalid_argument, "WebP input is empty.");
    return read_all(*input.source, options.limits.maximum_input_bytes);
}

Result<std::vector<std::uint8_t>> rgba_pixels(const ImageView& view) {
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.format.sample_type != SampleType::unsigned_integer ||
        view.format.bits_per_channel != 8) {
        return webp_error(ErrorCode::unsupported_feature,
                          "WebP encoding requires packed 8-bit pixels.");
    }
    const std::uint32_t channels = view.format.channel_count();
    if (view.format.channels != ChannelLayout::gray && view.format.channels != ChannelLayout::rgb &&
        view.format.channels != ChannelLayout::rgba && view.format.channels != ChannelLayout::bgr &&
        view.format.channels != ChannelLayout::bgra) {
        return webp_error(ErrorCode::unsupported_feature,
                          "WebP encoding does not support this pixel layout.");
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(view.width) * view.height * 4U);
    const bool bgr =
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const std::byte* source =
            view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride;
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t source_offset = static_cast<std::size_t>(x) * channels;
            const std::size_t target_offset = (static_cast<std::size_t>(y) * view.width + x) * 4U;
            if (channels == 1U) {
                const auto gray = std::to_integer<std::uint8_t>(source[source_offset]);
                pixels[target_offset] = gray;
                pixels[target_offset + 1U] = gray;
                pixels[target_offset + 2U] = gray;
                pixels[target_offset + 3U] = 255U;
            } else {
                pixels[target_offset] =
                    std::to_integer<std::uint8_t>(source[source_offset + (bgr ? 2U : 0U)]);
                pixels[target_offset + 1U] =
                    std::to_integer<std::uint8_t>(source[source_offset + 1U]);
                pixels[target_offset + 2U] =
                    std::to_integer<std::uint8_t>(source[source_offset + (bgr ? 0U : 2U)]);
                pixels[target_offset + 3U] =
                    channels == 4U ? std::to_integer<std::uint8_t>(source[source_offset + 3U])
                                   : 255U;
            }
        }
    }
    return pixels;
}

struct PictureDeleter final {
    void operator()(WebPPicture* picture) const noexcept {
        if (picture) {
            WebPPictureFree(picture);
            delete picture;
        }
    }
};
using Picture = std::unique_ptr<WebPPicture, PictureDeleter>;

Result<Picture> import_picture(const ImageView& view) {
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        view.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        view.row_stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return webp_error(ErrorCode::limit_exceeded, "WebP image dimensions are too large.");
    }
    Picture picture(new WebPPicture{});
    if (!WebPPictureInit(picture.get())) {
        return webp_error(ErrorCode::codec_unavailable, "The libwebp picture ABI is incompatible.");
    }
    picture->use_argb = 1;
    picture->width = static_cast<int>(view.width);
    picture->height = static_cast<int>(view.height);

    if (view.format == kRgba8) {
        if (!WebPPictureImportRGBA(picture.get(),
                                   reinterpret_cast<const std::uint8_t*>(view.pixels.data()),
                                   static_cast<int>(view.row_stride))) {
            return webp_error(ErrorCode::out_of_memory, "Could not import WebP pixels.");
        }
        return picture;
    }

    Result<std::vector<std::uint8_t>> converted = rgba_pixels(view);
    if (!converted)
        return converted.error();
    if (!WebPPictureImportRGBA(picture.get(), converted.value().data(),
                               static_cast<int>(view.width * 4U))) {
        return webp_error(ErrorCode::out_of_memory, "Could not import WebP pixels.");
    }
    return picture;
}

struct WriteContext final {
    ByteSink* sink = nullptr;
    std::stop_token stop;
    Status status;
};

int write_picture(const std::uint8_t* data, std::size_t size, const WebPPicture* picture) {
    auto* context = static_cast<WriteContext*>(picture->custom_ptr);
    if (!context || !context->sink)
        return 0;
    if (context->stop.stop_requested()) {
        context->status = cancelled_status();
        return 0;
    }
    Result<void> written = context->sink->write(std::as_bytes(std::span(data, size)));
    if (!written) {
        context->status = written.error();
        return 0;
    }
    return 1;
}

int report_progress(int, const WebPPicture* picture) {
    const auto* context = static_cast<const WriteContext*>(picture->user_data);
    return context && !context->stop.stop_requested();
}

bool native_webp_source(const RasterFrameDescriptor& frame) {
    const RasterLayout& layout = frame.layout;
    const bool has_alpha = layout.alpha == AlphaMode::straight;
    if (layout.color_model != ColorModel::ycbcr ||
        layout.chroma_subsampling != ChromaSubsampling::yuv420 ||
        layout.color_range != ColorRange::limited ||
        (layout.alpha != AlphaMode::none && !has_alpha) ||
        layout.planes.size() != (has_alpha ? 4U : 3U))
        return false;
    const std::uint32_t chroma_width = (frame.width + 1U) / 2U;
    const std::uint32_t chroma_height = (frame.height + 1U) / 2U;
    constexpr std::array semantics{PlaneSemantic::luma, PlaneSemantic::chroma_blue,
                                   PlaneSemantic::chroma_red};
    for (std::size_t index = 0; index < semantics.size(); ++index) {
        const PlaneDescriptor& plane = layout.planes[index];
        const std::uint32_t width = index == 0 ? frame.width : chroma_width;
        const std::uint32_t height = index == 0 ? frame.height : chroma_height;
        if (plane.semantic != semantics[index] || plane.format != kGray8 ||
            plane.significant_bits != 8 || plane.width != width || plane.height != height)
            return false;
    }
    if (has_alpha) {
        const PlaneDescriptor& alpha = layout.planes[3];
        if (alpha.semantic != PlaneSemantic::alpha || alpha.format != kGray8 ||
            alpha.significant_bits != 8 || alpha.width != frame.width ||
            alpha.height != frame.height)
            return false;
    }
    return true;
}

struct WebpMetadata final {
    Metadata metadata;
    ColorEncoding color;
    Orientation source_orientation = Orientation::identity;
};

Result<WebpMetadata> read_webp_metadata(std::span<const std::byte> bytes,
                                        const DecodeOptions& options) {
    WebpMetadata result;
    WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    Demuxer demuxer(WebPDemux(&data));
    if (!demuxer)
        return webp_error(ErrorCode::corrupt_data,
                          "Could not inspect the WebP container metadata.");
    std::uint64_t used = 0;
    const auto read_chunk = [&](const char fourcc[4], std::vector<std::byte>* destination,
                                bool retain) -> Result<void> {
        WebPChunkIterator iterator{};
        if (!WebPDemuxGetChunk(demuxer.get(), fourcc, 1, &iterator))
            return {};
        struct Guard final {
            explicit Guard(WebPChunkIterator* value) : value(value) {}
            ~Guard() {
                WebPDemuxReleaseChunkIterator(value);
            }
            WebPChunkIterator* value;
        } guard(&iterator);
        if (iterator.chunk.size > options.limits.maximum_metadata_bytes ||
            used > options.limits.maximum_metadata_bytes - iterator.chunk.size) {
            return webp_error(ErrorCode::limit_exceeded,
                              "WebP metadata exceeds the configured byte limit.");
        }
        used += iterator.chunk.size;
        if (retain) {
            destination->assign(
                reinterpret_cast<const std::byte*>(iterator.chunk.bytes),
                reinterpret_cast<const std::byte*>(iterator.chunk.bytes + iterator.chunk.size));
        }
        return {};
    };

    Result<void> status = read_chunk("ICCP", &result.color.icc_profile, options.preserve_metadata);
    if (!status)
        return status.error();
    std::vector<std::byte> orientation_exif;
    status = read_chunk(
        "EXIF", options.preserve_metadata ? &result.metadata.exif : &orientation_exif, true);
    if (!status)
        return status.error();
    status = read_chunk("XMP ", &result.metadata.xmp, options.preserve_metadata);
    if (!status)
        return status.error();
    const std::span<const std::byte> exif = options.preserve_metadata
                                                ? std::span<const std::byte>(result.metadata.exif)
                                                : std::span<const std::byte>(orientation_exif);
    if (const auto orientation = parse_exif_orientation(exif))
        result.source_orientation = *orientation;
    if (options.preserve_metadata && options.orientation == OrientationPolicy::preserve)
        result.metadata.orientation = result.source_orientation;
    return result;
}

void apply_metadata(DocumentInfo* document, WebpMetadata metadata, const DecodeOptions& options) {
    if (options.orientation == OrientationPolicy::apply) {
        metadata.metadata.orientation = Orientation::identity;
        (void)rewrite_exif_orientation(&metadata.metadata.exif, Orientation::identity);
        if (static_cast<std::uint8_t>(metadata.source_orientation) >= 5) {
            std::swap(document->canvas_width, document->canvas_height);
            for (FrameInfo& frame : document->frames)
                std::swap(frame.width, frame.height);
        }
    }
    document->metadata = std::move(metadata.metadata);
    document->color = std::move(metadata.color);
    for (FrameInfo& frame : document->frames) {
        frame.metadata = document->metadata;
        frame.color = document->color;
    }
}

void apply_metadata(DocumentDescriptor* document, WebpMetadata metadata,
                    const DecodeOptions& options) {
    if (options.orientation == OrientationPolicy::apply) {
        metadata.metadata.orientation = Orientation::identity;
        (void)rewrite_exif_orientation(&metadata.metadata.exif, Orientation::identity);
    }
    document->metadata = std::move(metadata.metadata);
    document->color = std::move(metadata.color);
    for (RasterFrameDescriptor& frame : document->frames) {
        frame.metadata = document->metadata;
        frame.color = document->color;
    }
}

bool sdr_srgb(const ColorEncoding& color) noexcept {
    return color.dynamic_range == DynamicRange::standard &&
           (color.primaries == ColorPrimaries::unknown ||
            color.primaries == ColorPrimaries::srgb) &&
           (color.transfer == TransferFunction::unknown ||
            color.transfer == TransferFunction::srgb);
}

Result<WebPConfig> encoder_config(const EncodeOptions& options) {
    WebPConfig config;
    if (!WebPConfigInit(&config))
        return webp_error(ErrorCode::codec_unavailable, "The libwebp encoder ABI is incompatible.");
    if (options.lossless) {
        if (!WebPConfigLosslessPreset(&config, std::clamp(options.lossless_effort, 0, 9))) {
            return webp_error(ErrorCode::invalid_argument, "WebP lossless compression is invalid.");
        }
        config.exact = 1;
    } else {
        config.lossless = 0;
        config.quality = static_cast<float>(std::clamp(options.quality, 0, 100));
        config.method = std::clamp(options.effort, 0, 6);
    }
    config.thread_level = 1;
    if (!WebPValidateConfig(&config))
        return webp_error(ErrorCode::invalid_argument, "WebP encoder settings are invalid.");
    return config;
}

Result<void> encode_picture(WebPPicture& picture, const Output& output, const WebPConfig& config,
                            std::stop_token stop) {
    WriteContext context{output.sink.get(), stop, {}};
    picture.writer = write_picture;
    picture.custom_ptr = &context;
    picture.progress_hook = report_progress;
    picture.user_data = &context;
    if (!WebPEncode(&config, &picture)) {
        if (!context.status.ok())
            return context.status;
        if (stop.stop_requested())
            return cancelled_status();
        return webp_error(ErrorCode::encode_failed, "Could not encode WebP pixels.");
    }
    return {};
}

} // namespace

Result<void> validate_webp_document(const Document& document, const Output& output);
Result<void> validate_webp_metadata(const Metadata& metadata, const ColorEncoding& color,
                                    const EncodeOptions& options);
Result<void> install_animation_metadata(WebPAnimEncoder* encoder, const Metadata& metadata,
                                        const ColorEncoding& color, const EncodeOptions& options);
Result<void> encode_static_picture(WebPPicture& picture, const Output& output,
                                   const WebPConfig& config, const Metadata& metadata,
                                   const ColorEncoding& color, const EncodeOptions& options,
                                   std::stop_token stop);

CodecCapability WebpCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::animation | CodecCapability::streaming_decode |
           CodecCapability::metadata_decode;
}

int WebpCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 12U && std::memcmp(header.data(), "RIFF", 4U) == 0 &&
        std::memcmp(header.data() + 8U, "WEBP", 4U) == 0) {
        return 100;
    }
    return format_from_extension(name_hint) == Format::webp ? 10 : 0;
}

Result<DocumentInfo> WebpCodec::inspect(const Input& input, const DecodeOptions& options,
                                        std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes = input_bytes(input, options);
    if (!bytes)
        return bytes.error();
    Result<WebpFeatures> features = read_features(bytes.value(), options.limits);
    if (!features)
        return features.error();
    Result<WebpMetadata> container_metadata = read_webp_metadata(bytes.value(), options);
    if (!container_metadata)
        return container_metadata.error();
    const PixelFormat format = options.output_format.value_or(kRgba8);
    Result<WEBP_CSP_MODE> mode = packed_mode(format);
    if (!mode)
        return mode.error();
    if (features.value().bitstream.has_animation == 0) {
        DocumentInfo document = static_document_info(features.value(), options, format);
        apply_metadata(&document, std::move(container_metadata).value(), options);
        return document;
    }
    Result<std::pair<AnimDecoder, WebPAnimInfo>> opened =
        open_decoder(bytes.value(), options.limits, false);
    if (!opened)
        return opened.error();
    Result<DocumentInfo> document =
        animation_document_info(opened.value().first.get(), opened.value().second);
    if (!document)
        return document.error();
    apply_metadata(&document.value(), std::move(container_metadata).value(), options);
    return document;
}

Result<DocumentDescriptor> WebpCodec::inspect_raster(const Input& input,
                                                     const DecodeOptions& options,
                                                     std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes = input_bytes(input, options);
    if (!bytes)
        return bytes.error();
    Result<WebpFeatures> features = read_features(bytes.value(), options.limits);
    if (!features)
        return features.error();
    if (options.raster_layout == RasterLayoutPolicy::native && !options.output_format &&
        native_planar_supported(features.value())) {
        Result<WebpMetadata> metadata = read_webp_metadata(bytes.value(), options);
        if (!metadata)
            return metadata.error();
        if (options.orientation == OrientationPolicy::apply &&
            metadata.value().source_orientation != Orientation::identity)
            return Codec::inspect_raster(input, options, stop);
        Result<DocumentDescriptor> descriptor = native_descriptor(features.value(), options);
        if (!descriptor)
            return descriptor.error();
        apply_metadata(&descriptor.value(), std::move(metadata).value(), options);
        return descriptor;
    }
    return Codec::inspect_raster(input, options, stop);
}

Result<Document> WebpCodec::decode(const Input& input, const DecodeOptions& options,
                                   std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes = input_bytes(input, options);
    if (!bytes)
        return bytes.error();
    Result<WebpFeatures> features = read_features(bytes.value(), options.limits);
    if (!features)
        return features.error();
    Result<WebpMetadata> container_metadata = read_webp_metadata(bytes.value(), options);
    if (!container_metadata)
        return container_metadata.error();
    if (features.value().bitstream.has_animation == 0) {
        const PixelFormat format = options.output_format.value_or(kRgba8);
        Result<WEBP_CSP_MODE> mode = packed_mode(format);
        if (!mode)
            return mode.error();
        const auto [width, height] = scaled_dimensions(features.value(), options);
        Result<std::size_t> pixel_bytes = format.bytes_per_pixel();
        if (!pixel_bytes)
            return pixel_bytes.error();
        const std::uint64_t output_bytes =
            static_cast<std::uint64_t>(width) * height * pixel_bytes.value();
        if (output_bytes > options.limits.maximum_owned_output_bytes)
            return webp_error(ErrorCode::limit_exceeded,
                              "WebP output exceeds the owning decode limit.");
        Result<MutableImage> allocated = MutableImage::allocate(width, height, format);
        if (!allocated)
            return allocated.error();
        MutableImage pixels = std::move(allocated).value();
        if (stop.stop_requested())
            return cancelled_status();
        Result<void> decoded = decode_packed(bytes.value(), features.value(), options, format,
                                             pixels.pixels(), pixels.row_stride());
        if (!decoded)
            return decoded.error();
        if (stop.stop_requested())
            return cancelled_status();
        Document document;
        document.format = Format::webp;
        document.canvas_width = width;
        document.canvas_height = height;
        Frame frame;
        frame.image = std::move(pixels).freeze();
        document.frames.push_back(std::move(frame));
        document.metadata = container_metadata.value().metadata;
        document.color = container_metadata.value().color;
        document.frames.front().metadata = document.metadata;
        document.frames.front().color = document.color;
        if (options.orientation == OrientationPolicy::apply) {
            Result<void> oriented =
                apply_orientation(&document, container_metadata.value().source_orientation, stop);
            if (!oriented)
                return oriented.error();
        }
        return document;
    }
    if (options.output_format && *options.output_format != kRgba8)
        return webp_error(ErrorCode::unsupported_feature,
                          "Animated WebP decoding currently requires RGBA8 output.");
    Result<std::pair<AnimDecoder, WebPAnimInfo>> opened =
        open_decoder(bytes.value(), options.limits);
    if (!opened)
        return opened.error();
    AnimDecoder decoder = std::move(opened.value().first);
    const WebPAnimInfo webp = opened.value().second;
    Document document;
    document.format = Format::webp;
    document.canvas_width = webp.canvas_width;
    document.canvas_height = webp.canvas_height;
    document.loop_count = webp.loop_count;
    int previous_timestamp = 0;
    while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
        if (stop.stop_requested())
            return cancelled_status();
        std::uint8_t* decoded = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(decoder.get(), &decoded, &timestamp) || !decoded) {
            return webp_error(ErrorCode::decode_failed, "Could not decode a WebP frame.");
        }
        Result<MutableImage> allocated =
            MutableImage::allocate(webp.canvas_width, webp.canvas_height, kRgba8);
        if (!allocated)
            return allocated.error();
        MutableImage pixels = std::move(allocated).value();
        const std::size_t row_bytes = static_cast<std::size_t>(webp.canvas_width) * 4U;
        for (std::uint32_t y = 0; y < webp.canvas_height; ++y) {
            std::memcpy(pixels.pixels().data() + static_cast<std::size_t>(y) * pixels.row_stride(),
                        decoded + static_cast<std::size_t>(y) * row_bytes, row_bytes);
        }
        Frame frame;
        frame.image = std::move(pixels).freeze();
        frame.duration = std::chrono::milliseconds(std::max(1, timestamp - previous_timestamp));
        frame.blend = FrameBlend::source;
        frame.disposal = FrameDisposal::keep;
        document.frames.push_back(std::move(frame));
        previous_timestamp = timestamp;
    }
    document.metadata = container_metadata.value().metadata;
    document.color = container_metadata.value().color;
    for (Frame& frame : document.frames) {
        frame.metadata = document.metadata;
        frame.color = document.color;
    }
    if (options.orientation == OrientationPolicy::apply) {
        Result<void> oriented =
            apply_orientation(&document, container_metadata.value().source_orientation, stop);
        if (!oriented)
            return oriented.error();
    }
    return document;
}

Result<void> WebpCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                       const DecodeOptions& options, std::stop_token stop) const {
    if (options.orientation == OrientationPolicy::apply)
        return Codec::decode_to_sink(input, sink, options, stop);
    Result<std::vector<std::byte>> bytes = input_bytes(input, options);
    if (!bytes)
        return bytes.error();
    Result<WebpFeatures> features = read_features(bytes.value(), options.limits);
    if (!features)
        return features.error();
    Result<WebpMetadata> container_metadata = read_webp_metadata(bytes.value(), options);
    if (!container_metadata)
        return container_metadata.error();
    if (features.value().bitstream.has_animation != 0) {
        if (options.output_format && *options.output_format != kRgba8)
            return webp_error(ErrorCode::unsupported_feature,
                              "Animated WebP decoding currently requires RGBA8 output.");
        Result<std::pair<AnimDecoder, WebPAnimInfo>> opened =
            open_decoder(bytes.value(), options.limits, false);
        if (!opened)
            return opened.error();
        AnimDecoder decoder = std::move(opened.value().first);
        const WebPAnimInfo info = opened.value().second;
        Result<DocumentInfo> described = animation_document_info(decoder.get(), info);
        if (!described)
            return described.error();
        apply_metadata(&described.value(), std::move(container_metadata).value(), options);
        Result<void> status = sink.begin(described.value());
        if (!status)
            return status;
        const std::size_t row_stride = static_cast<std::size_t>(info.canvas_width) * 4U;
        const std::size_t frame_bytes = row_stride * info.canvas_height;
        std::uint32_t frame_index = 0;
        while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
            if (stop.stop_requested())
                return cancelled_status();
            if (frame_index >= described.value().frames.size())
                return webp_error(ErrorCode::corrupt_data,
                                  "WebP animation contains unexpected frames.");
            status = sink.begin_frame(frame_index, described.value().frames[frame_index]);
            if (!status)
                return status;
            std::uint8_t* decoded = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(decoder.get(), &decoded, &timestamp) || !decoded)
                return webp_error(ErrorCode::decode_failed,
                                  "Could not decode a WebP animation frame.");
            std::span<std::byte> storage = sink.frame_storage(frame_index, row_stride, frame_bytes);
            if (storage.size() == frame_bytes) {
                std::memcpy(storage.data(), decoded, frame_bytes);
            } else {
                status = sink.write_rows(0, info.canvas_height, row_stride,
                                         std::as_bytes(std::span(decoded, frame_bytes)));
                if (!status)
                    return status;
            }
            status = sink.end_frame(frame_index);
            if (!status)
                return status;
            ++frame_index;
        }
        if (frame_index != described.value().frames.size())
            return webp_error(ErrorCode::truncated_data,
                              "WebP animation ended before all frames were decoded.");
        return sink.end();
    }
    const PixelFormat format = options.output_format.value_or(kRgba8);
    Result<WEBP_CSP_MODE> mode = packed_mode(format);
    if (!mode)
        return mode.error();
    Result<std::size_t> pixel_bytes = format.bytes_per_pixel();
    if (!pixel_bytes)
        return pixel_bytes.error();
    const auto [width, height] = scaled_dimensions(features.value(), options);
    if (width > std::numeric_limits<std::size_t>::max() / pixel_bytes.value()) {
        return webp_error(ErrorCode::limit_exceeded, "WebP output size overflows.");
    }
    DocumentInfo document_info = static_document_info(features.value(), options, format);
    apply_metadata(&document_info, std::move(container_metadata).value(), options);
    Result<void> status = sink.begin(document_info);
    if (!status)
        return status;
    status = sink.begin_frame(0, document_info.frames.front());
    if (!status)
        return status;
    const std::size_t row_stride = static_cast<std::size_t>(width) * pixel_bytes.value();
    if (height > std::numeric_limits<std::size_t>::max() / row_stride)
        return webp_error(ErrorCode::limit_exceeded, "WebP output size overflows.");
    const std::size_t output_bytes = row_stride * height;
    std::span<std::byte> target = sink.frame_storage(0, row_stride, output_bytes);
    std::vector<std::byte> owned;
    if (target.size() != output_bytes) {
        if (output_bytes > options.limits.maximum_owned_output_bytes) {
            return webp_error(
                ErrorCode::limit_exceeded,
                "WebP output exceeds the owning decode limit; use a storage-backed sink.");
        }
        try {
            owned.resize(output_bytes);
        } catch (const std::bad_alloc&) {
            return webp_error(ErrorCode::out_of_memory, "Could not allocate WebP output pixels.");
        }
        target = owned;
    }
    if (stop.stop_requested())
        return cancelled_status();
    Result<void> decoded =
        decode_packed(bytes.value(), features.value(), options, format, target, row_stride);
    if (!decoded)
        return decoded.error();
    if (stop.stop_requested())
        return cancelled_status();
    if (!owned.empty()) {
        status = sink.write_rows(0, height, row_stride, owned);
        if (!status)
            return status;
    }
    status = sink.end_frame(0);
    if (!status)
        return status;
    return sink.end();
}

Result<void> WebpCodec::decode_into(const Input& input, RasterWriter& writer,
                                    const DecodeOptions& options, std::stop_token stop) const {
    if (options.raster_layout != RasterLayoutPolicy::native || options.output_format)
        return Codec::decode_into(input, writer, options, stop);
    Result<std::vector<std::byte>> bytes = input_bytes(input, options);
    if (!bytes)
        return bytes.error();
    Result<WebpFeatures> features = read_features(bytes.value(), options.limits);
    if (!features)
        return features.error();
    if (!native_planar_supported(features.value()))
        return Codec::decode_into(input, writer, options, stop);
    Result<DocumentDescriptor> descriptor = native_descriptor(features.value(), options);
    if (!descriptor)
        return descriptor.error();
    Result<WebpMetadata> metadata = read_webp_metadata(bytes.value(), options);
    if (!metadata)
        return metadata.error();
    if (options.orientation == OrientationPolicy::apply &&
        metadata.value().source_orientation != Orientation::identity)
        return Codec::decode_into(input, writer, options, stop);
    apply_metadata(&descriptor.value(), std::move(metadata).value(), options);
    if (!matching_descriptor(writer.descriptor(), descriptor.value()))
        return webp_error(ErrorCode::invalid_argument,
                          "WebP native raster writer does not match the decoded plane layout.");
    const RasterFrameDescriptor& frame = descriptor.value().frames.front();
    Result<WritablePlaneSet> prepared =
        prepare_writable_planes(writer, 0, frame, options.limits, "libwebp");
    if (!prepared)
        return prepared.error();
    WritablePlaneSet planes = std::move(prepared).value();
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config))
        return webp_error(ErrorCode::codec_unavailable, "The libwebp decoder ABI is incompatible.");
    DecodeBufferGuard guard(&config.output);
    config.input = features.value().bitstream;
    config.output.colorspace = frame.layout.alpha == AlphaMode::none ? MODE_YUV : MODE_YUVA;
    config.output.is_external_memory = 1;
    config.output.width = static_cast<int>(frame.width);
    config.output.height = static_cast<int>(frame.height);
    WebPYUVABuffer& output = config.output.u.YUVA;
    output.y = planes.pointers[0];
    output.u = planes.pointers[1];
    output.v = planes.pointers[2];
    output.a = planes.pointers.size() == 4 ? planes.pointers[3] : nullptr;
    output.y_stride = planes.strides[0];
    output.u_stride = planes.strides[1];
    output.v_stride = planes.strides[2];
    output.a_stride = planes.pointers.size() == 4 ? planes.strides[3] : 0;
    output.y_size = static_cast<std::size_t>(planes.strides[0]) * frame.layout.planes[0].height;
    output.u_size = static_cast<std::size_t>(planes.strides[1]) * frame.layout.planes[1].height;
    output.v_size = static_cast<std::size_t>(planes.strides[2]) * frame.layout.planes[2].height;
    output.a_size = planes.pointers.size() == 4 ? static_cast<std::size_t>(planes.strides[3]) *
                                                      frame.layout.planes[3].height
                                                : 0;
    config.options.use_threads = 1;
    if (frame.width != features.value().width || frame.height != features.value().height) {
        config.options.use_scaling = 1;
        config.options.scaled_width = static_cast<int>(frame.width);
        config.options.scaled_height = static_cast<int>(frame.height);
    }
    if (stop.stop_requested())
        return cancelled_status();
    const VP8StatusCode decoded = WebPDecode(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size(), &config);
    if (decoded != VP8_STATUS_OK)
        return webp_decode_error(decoded, "Could not decode native WebP planes.");
    if (stop.stop_requested())
        return cancelled_status();
    return publish_writable_planes(writer, 0, frame, planes, stop);
}

Result<EncodedArtifactReceipt> WebpCodec::encode_to_sink(const Document& document,
                                                         const Output& output,
                                                         const EncodeOptions& options,
                                                         std::stop_token stop) const {
    Result<void> valid = validate_webp_document(document, output);
    if (!valid)
        return valid.error();
    valid = validate_webp_metadata(document.metadata, document.color, options);
    if (!valid)
        return valid.error();
    Result<WebPConfig> configured = encoder_config(options);
    if (!configured)
        return configured.error();
    const WebPConfig config = configured.value();
    if (document.frames.size() == 1U) {
        Result<Picture> imported = import_picture(document.frames.front().image.view());
        if (!imported)
            return imported.error();
        Result<void> encoded = encode_static_picture(
            *imported.value(), output, config, document.metadata, document.color, options, stop);
        if (!encoded)
            return encoded.error();
        return receipt_for_document(document, format());
    }

    WebPAnimEncoderOptions animation_options;
    if (!WebPAnimEncoderOptionsInit(&animation_options)) {
        return webp_error(ErrorCode::codec_unavailable,
                          "The libwebp animation encoder ABI is incompatible.");
    }
    animation_options.anim_params.loop_count = static_cast<int>(std::min<std::uint32_t>(
        document.loop_count, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    AnimEncoder encoder(WebPAnimEncoderNew(static_cast<int>(document.canvas_width),
                                           static_cast<int>(document.canvas_height),
                                           &animation_options));
    if (!encoder)
        return webp_error(ErrorCode::out_of_memory, "Could not create WebP encoder.");
    valid = install_animation_metadata(encoder.get(), document.metadata, document.color, options);
    if (!valid)
        return valid.error();

    int timestamp = 0;
    for (const Frame& frame : document.frames) {
        if (stop.stop_requested())
            return cancelled_status();
        Result<Picture> picture = import_picture(frame.image.view());
        if (!picture)
            return picture.error();
        const int added =
            WebPAnimEncoderAdd(encoder.get(), picture.value().get(), timestamp, &config);
        if (!added)
            return webp_error(ErrorCode::encode_failed, "Could not add WebP frame.");
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(frame.duration).count();
        timestamp += static_cast<int>(std::max<std::int64_t>(1, milliseconds));
    }
    if (!WebPAnimEncoderAdd(encoder.get(), nullptr, timestamp, nullptr)) {
        return webp_error(ErrorCode::encode_failed, "Could not finalize WebP frames.");
    }
    WebPData encoded;
    WebPDataInit(&encoded);
    if (!WebPAnimEncoderAssemble(encoder.get(), &encoded)) {
        return webp_error(ErrorCode::encode_failed, "Could not assemble WebP output.");
    }
    Result<void> written =
        output.sink->write(std::as_bytes(std::span(encoded.bytes, encoded.size)));
    WebPDataClear(&encoded);
    if (!written)
        return written.error();
    return receipt_for_document(document, format());
}

Result<EncodedArtifactReceipt> WebpCodec::encode_raster_to_sink(const RasterSource& source,
                                                                const Output& output,
                                                                const EncodeOptions& options,
                                                                std::stop_token stop) const {
    const DocumentDescriptor& descriptor = source.descriptor();
    if (options.lossless || descriptor.frames.size() != 1 ||
        !native_webp_source(descriptor.frames.front()))
        return Codec::encode_raster_to_sink(source, output, options, stop);
    if (!output.sink)
        return webp_error(ErrorCode::invalid_argument, "WebP output is empty.");
    Result<void> metadata_status =
        validate_webp_metadata(descriptor.metadata, descriptor.color, options);
    if (!metadata_status)
        return metadata_status.error();
    const RasterFrameDescriptor& frame = descriptor.frames.front();
    if (frame.width > WEBP_MAX_DIMENSION || frame.height > WEBP_MAX_DIMENSION)
        return webp_error(ErrorCode::limit_exceeded, "WebP image dimensions exceed codec limits.");
    Result<ReadablePlaneSet> prepared = prepare_readable_planes(
        source, 0, frame, stop, std::numeric_limits<std::uint64_t>::max(), "libwebp");
    if (!prepared)
        return prepared.error();
    ReadablePlaneSet planes = std::move(prepared).value();

    Picture picture(new WebPPicture{});
    if (!WebPPictureInit(picture.get()))
        return webp_error(ErrorCode::codec_unavailable, "The libwebp picture ABI is incompatible.");
    const bool has_alpha = frame.layout.alpha == AlphaMode::straight;
    picture->use_argb = 0;
    picture->colorspace = has_alpha ? WEBP_YUV420A : WEBP_YUV420;
    picture->width = static_cast<int>(frame.width);
    picture->height = static_cast<int>(frame.height);
    if (has_access(source.access(), RasterAccess::mapped_planes)) {
        // libwebp's SIMD readers are not safe against a page-bounded mapped view.
        // Its own YUVA allocation provides the storage contract those readers
        // require while still avoiding RGB conversion and RGBA materialization.
        if (!WebPPictureAlloc(picture.get()))
            return webp_error(ErrorCode::out_of_memory,
                              "Could not allocate WebP native input planes.");
        const std::array<unsigned char*, 4> destinations{picture->y, picture->u, picture->v,
                                                         picture->a};
        const std::array<int, 4> destination_strides{picture->y_stride, picture->uv_stride,
                                                     picture->uv_stride, picture->a_stride};
        for (std::size_t index = 0; index < planes.pointers.size(); ++index) {
            const PlaneDescriptor& plane = frame.layout.planes[index];
            for (std::uint32_t row = 0; row < plane.height; ++row) {
                if (stop.stop_requested())
                    return cancelled_status();
                std::memcpy(
                    destinations[index] + static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(destination_strides[index]),
                    planes.pointers[index] + static_cast<std::size_t>(row) *
                                                 static_cast<std::size_t>(planes.strides[index]),
                    plane.width);
            }
        }
    } else {
        if (planes.strides[1] != planes.strides[2])
            return webp_error(ErrorCode::invalid_argument,
                              "WebP chroma planes require a common row stride.");
        picture->y = const_cast<unsigned char*>(planes.pointers[0]);
        picture->u = const_cast<unsigned char*>(planes.pointers[1]);
        picture->v = const_cast<unsigned char*>(planes.pointers[2]);
        picture->a = has_alpha ? const_cast<unsigned char*>(planes.pointers[3]) : nullptr;
        picture->y_stride = planes.strides[0];
        picture->uv_stride = planes.strides[1];
        picture->a_stride = has_alpha ? planes.strides[3] : 0;
    }
    Result<WebPConfig> configured = encoder_config(options);
    if (!configured)
        return configured.error();
    Result<void> encoded = encode_static_picture(
        *picture, output, configured.value(), descriptor.metadata, descriptor.color, options, stop);
    if (!encoded)
        return encoded.error();
    return receipt_for_descriptor(descriptor, format());
}

RasterEncodeRoute WebpCodec::raster_encode_route(const DocumentDescriptor& descriptor,
                                                 const EncodeOptions& options) const noexcept {
    if (options.lossless || descriptor.frames.size() != 1 || !sdr_srgb(descriptor.color))
        return RasterEncodeRoute::materialized;
    const RasterFrameDescriptor& frame = descriptor.frames.front();
    if (frame.x != 0 || frame.y != 0 || frame.width != descriptor.canvas_width ||
        frame.height != descriptor.canvas_height || !sdr_srgb(frame.color) ||
        !native_webp_source(frame))
        return RasterEncodeRoute::materialized;
    return RasterEncodeRoute::native;
}

Result<void> validate_webp_document(const Document& document, const Output& output) {
    if (!output.sink || document.frames.empty() || document.canvas_width == 0 ||
        document.canvas_height == 0)
        return webp_error(ErrorCode::invalid_argument, "WebP document is invalid.");
    if (document.canvas_width > WEBP_MAX_DIMENSION || document.canvas_height > WEBP_MAX_DIMENSION)
        return webp_error(ErrorCode::limit_exceeded, "WebP image dimensions exceed codec limits.");
    if (document.frames.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return webp_error(ErrorCode::limit_exceeded, "WebP frame count exceeds codec limits.");
    std::int64_t timestamp = 0;
    for (const Frame& frame : document.frames) {
        if (frame.x != 0 || frame.y != 0 || frame.image.width() != document.canvas_width ||
            frame.image.height() != document.canvas_height) {
            return webp_error(ErrorCode::unsupported_feature,
                              "WebP encoding requires full-canvas composed frames.");
        }
        if (document.frames.size() > 1U) {
            const std::int64_t milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(frame.duration).count();
            const std::int64_t duration = std::max<std::int64_t>(1, milliseconds);
            if (duration > std::numeric_limits<int>::max() - timestamp)
                return webp_error(ErrorCode::limit_exceeded,
                                  "WebP animation duration exceeds the signed millisecond limit.");
            timestamp += duration;
        }
    }
    return {};
}

bool has_webp_metadata(const Metadata& metadata, const ColorEncoding& color,
                       const EncodeOptions& options) noexcept {
    return options.preserve_metadata &&
           (!color.icc_profile.empty() || !metadata.exif.empty() || !metadata.xmp.empty());
}

Result<void> validate_webp_metadata(const Metadata& metadata, const ColorEncoding& color,
                                    const EncodeOptions& options) {
    if (!options.preserve_metadata)
        return {};
    if (color.icc_profile.size() > kMaximumWebpEncodeMetadataBytes)
        return webp_error(ErrorCode::limit_exceeded,
                          "WebP metadata exceeds the aggregate byte limit.");
    std::uint64_t total = color.icc_profile.size();
    if (metadata.exif.size() > kMaximumWebpEncodeMetadataBytes - total)
        return webp_error(ErrorCode::limit_exceeded,
                          "WebP metadata exceeds the aggregate byte limit.");
    total += metadata.exif.size();
    if (metadata.xmp.size() > kMaximumWebpEncodeMetadataBytes - total)
        return webp_error(ErrorCode::limit_exceeded,
                          "WebP metadata exceeds the aggregate byte limit.");
    return {};
}

WebPData webp_data(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

Result<void> set_mux_chunk(WebPMux* mux, const char fourcc[4], std::span<const std::byte> bytes) {
    if (bytes.empty())
        return {};
    const WebPData data = webp_data(bytes);
    const WebPMuxError status = WebPMuxSetChunk(mux, fourcc, &data, 1);
    if (status == WEBP_MUX_MEMORY_ERROR)
        return webp_error(ErrorCode::out_of_memory, "Could not allocate WebP metadata.");
    if (status != WEBP_MUX_OK)
        return webp_error(ErrorCode::encode_failed, "Could not add WebP metadata.");
    return {};
}

Result<void> set_animation_chunk(WebPAnimEncoder* encoder, const char fourcc[4],
                                 std::span<const std::byte> bytes) {
    if (bytes.empty())
        return {};
    const WebPData data = webp_data(bytes);
    const WebPMuxError status = WebPAnimEncoderSetChunk(encoder, fourcc, &data, 1);
    if (status == WEBP_MUX_MEMORY_ERROR)
        return webp_error(ErrorCode::out_of_memory, "Could not allocate WebP animation metadata.");
    if (status != WEBP_MUX_OK)
        return webp_error(ErrorCode::encode_failed, "Could not add WebP animation metadata.");
    return {};
}

Result<void> install_animation_metadata(WebPAnimEncoder* encoder, const Metadata& metadata,
                                        const ColorEncoding& color, const EncodeOptions& options) {
    if (!options.preserve_metadata)
        return {};
    Result<void> status = set_animation_chunk(encoder, "ICCP", color.icc_profile);
    if (!status)
        return status;
    status = set_animation_chunk(encoder, "EXIF", metadata.exif);
    if (!status)
        return status;
    return set_animation_chunk(encoder, "XMP ", metadata.xmp);
}

Result<void> encode_static_picture(WebPPicture& picture, const Output& output,
                                   const WebPConfig& config, const Metadata& metadata,
                                   const ColorEncoding& color, const EncodeOptions& options,
                                   std::stop_token stop) {
    if (!has_webp_metadata(metadata, color, options))
        return encode_picture(picture, output, config, stop);

    auto encoded = std::make_shared<std::vector<std::byte>>();
    Output memory = memory_output(encoded, "static.webp");
    Result<void> status = encode_picture(picture, memory, config, stop);
    if (!status)
        return status;
    const WebPData bitstream = webp_data(*encoded);
    Mux mux(WebPMuxCreate(&bitstream, 1));
    if (!mux)
        return webp_error(ErrorCode::out_of_memory, "Could not allocate the WebP metadata muxer.");
    status = set_mux_chunk(mux.get(), "ICCP", color.icc_profile);
    if (!status)
        return status;
    status = set_mux_chunk(mux.get(), "EXIF", metadata.exif);
    if (!status)
        return status;
    status = set_mux_chunk(mux.get(), "XMP ", metadata.xmp);
    if (!status)
        return status;
    WebPData assembled;
    WebPDataInit(&assembled);
    const WebPMuxError assembled_status = WebPMuxAssemble(mux.get(), &assembled);
    if (assembled_status != WEBP_MUX_OK) {
        WebPDataClear(&assembled);
        return webp_error(assembled_status == WEBP_MUX_MEMORY_ERROR ? ErrorCode::out_of_memory
                                                                    : ErrorCode::encode_failed,
                          "Could not assemble the metadata-bearing WebP output.");
    }
    status = output.sink->write(std::as_bytes(std::span(assembled.bytes, assembled.size)));
    WebPDataClear(&assembled);
    return status;
}

} // namespace snow::image::internal
