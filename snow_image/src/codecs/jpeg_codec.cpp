#include "codecs/jpeg_codec.h"
#include "planar_raster_io.h"

#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#include <turbojpeg.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324 4611)
#endif

namespace snow::image::internal {
namespace {

struct TjDeleter final {
    void operator()(void* handle) const noexcept {
        tj3Destroy(handle);
    }
};
using TjHandle = std::unique_ptr<void, TjDeleter>;

Status tj_error(void* handle, ErrorCode code) {
    const char* message = tj3GetErrorStr(handle);
    return Status::error(code, message && *message ? message : "libjpeg-turbo operation failed.",
                         "libjpeg-turbo");
}

struct JpegInfo final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int colorspace = TJCS_RGB;
    PixelFormat pixel_format = kRgb8;
    int turbo_pixel_format = TJPF_RGB;
    int subsampling = TJSAMP_UNKNOWN;
};

Result<ChromaSubsampling> chroma_subsampling(int value) {
    switch (value) {
    case TJSAMP_444:
        return ChromaSubsampling::yuv444;
    case TJSAMP_422:
        return ChromaSubsampling::yuv422;
    case TJSAMP_420:
        return ChromaSubsampling::yuv420;
    case TJSAMP_440:
        return ChromaSubsampling::yuv440;
    case TJSAMP_411:
        return ChromaSubsampling::yuv411;
    case TJSAMP_441:
        return ChromaSubsampling::yuv441;
    default:
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG chroma subsampling is not representable as planar YCbCr.",
                             "libjpeg-turbo");
    }
}

Result<int> turbo_subsampling(ChromaSubsampling value) {
    switch (value) {
    case ChromaSubsampling::yuv444:
        return TJSAMP_444;
    case ChromaSubsampling::yuv422:
        return TJSAMP_422;
    case ChromaSubsampling::yuv420:
        return TJSAMP_420;
    case ChromaSubsampling::yuv440:
        return TJSAMP_440;
    case ChromaSubsampling::yuv411:
        return TJSAMP_411;
    case ChromaSubsampling::yuv441:
        return TJSAMP_441;
    case ChromaSubsampling::none:
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG planar input requires chroma subsampling.", "libjpeg-turbo");
    }
    return Status::error(ErrorCode::unsupported_feature,
                         "JPEG planar input has unknown chroma subsampling.", "libjpeg-turbo");
}

bool native_planar_supported(const JpegInfo& info) {
    return info.colorspace == TJCS_GRAY ||
           (info.colorspace == TJCS_YCbCr && info.subsampling != TJSAMP_UNKNOWN);
}

Result<DocumentDescriptor> native_descriptor(const JpegInfo& info) {
    if (!native_planar_supported(info)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "This JPEG colorspace cannot be exposed as native planar data.",
                             "libjpeg-turbo");
    }
    DocumentDescriptor document;
    document.format = Format::jpeg;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    RasterFrameDescriptor frame;
    frame.width = info.width;
    frame.height = info.height;
    frame.layout.alpha = AlphaMode::none;
    frame.layout.color_range = ColorRange::full;
    if (info.colorspace == TJCS_GRAY || info.subsampling == TJSAMP_GRAY) {
        frame.layout.color_model = ColorModel::gray;
        frame.layout.planes.push_back({PlaneSemantic::gray, info.width, info.height, kGray8, 8});
    } else {
        Result<ChromaSubsampling> subsampling = chroma_subsampling(info.subsampling);
        if (!subsampling)
            return subsampling.error();
        frame.layout.color_model = ColorModel::ycbcr;
        frame.layout.chroma_subsampling = subsampling.value();
        constexpr std::array semantics{PlaneSemantic::luma, PlaneSemantic::chroma_blue,
                                       PlaneSemantic::chroma_red};
        for (int component = 0; component < 3; ++component) {
            const int width =
                tj3YUVPlaneWidth(component, static_cast<int>(info.width), info.subsampling);
            const int height =
                tj3YUVPlaneHeight(component, static_cast<int>(info.height), info.subsampling);
            if (width <= 0 || height <= 0) {
                return Status::error(ErrorCode::unsupported_feature,
                                     "JPEG planar dimensions are invalid.", "libjpeg-turbo");
            }
            frame.layout.planes.push_back({semantics[static_cast<std::size_t>(component)],
                                           static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height), kGray8, 8});
        }
    }
    document.frames.push_back(std::move(frame));
    Result<void> valid = document.validate();
    if (!valid)
        return valid.error();
    return document;
}

bool matching_descriptor(const DocumentDescriptor& left, const DocumentDescriptor& right) {
    if (left.canvas_width != right.canvas_width || left.canvas_height != right.canvas_height ||
        left.frames.size() != right.frames.size())
        return false;
    for (std::size_t index = 0; index < left.frames.size(); ++index) {
        if (left.frames[index].width != right.frames[index].width ||
            left.frames[index].height != right.frames[index].height ||
            left.frames[index].layout != right.frames[index].layout)
            return false;
    }
    return true;
}

Result<std::pair<TjHandle, JpegInfo>> read_header(std::span<const std::byte> bytes,
                                                  const DecodeLimits& limits) {
    if (bytes.size() > std::numeric_limits<std::size_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "JPEG input is too large.",
                             "libjpeg-turbo");
    }
    TjHandle handle(tj3Init(TJINIT_DECOMPRESS));
    if (!handle) {
        return Status::error(ErrorCode::out_of_memory, "Could not create the JPEG decoder.",
                             "libjpeg-turbo");
    }
    if (tj3DecompressHeader(handle.get(), reinterpret_cast<const unsigned char*>(bytes.data()),
                            bytes.size()) != 0) {
        return tj_error(handle.get(), ErrorCode::corrupt_data);
    }
    const int width = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
    const int height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
    if (width <= 0 || height <= 0) {
        return Status::error(ErrorCode::corrupt_data, "JPEG dimensions are invalid.",
                             "libjpeg-turbo");
    }
    Result<void> dimensions = validate_dimensions(static_cast<std::uint32_t>(width),
                                                  static_cast<std::uint32_t>(height), limits);
    if (!dimensions)
        return dimensions.error();
    JpegInfo info;
    info.width = static_cast<std::uint32_t>(width);
    info.height = static_cast<std::uint32_t>(height);
    info.colorspace = tj3Get(handle.get(), TJPARAM_COLORSPACE);
    info.subsampling = tj3Get(handle.get(), TJPARAM_SUBSAMP);
    if (info.colorspace == TJCS_GRAY) {
        info.pixel_format = kGray8;
        info.turbo_pixel_format = TJPF_GRAY;
    } else if (info.colorspace == TJCS_CMYK || info.colorspace == TJCS_YCCK) {
        info.pixel_format = {SampleType::unsigned_integer, ChannelLayout::cmyk, AlphaMode::none, 8,
                             true};
        info.turbo_pixel_format = TJPF_CMYK;
    }
    return std::pair{std::move(handle), info};
}

Result<JpegInfo> scaled_info(void* handle, JpegInfo info, const DecodeOptions& options) {
    if (!options.maximum_extent || *options.maximum_extent == 0 ||
        (info.width <= *options.maximum_extent && info.height <= *options.maximum_extent))
        return info;
    int count = 0;
    const tjscalingfactor* factors = tj3GetScalingFactors(&count);
    if (!factors || count <= 0) {
        return Status::error(ErrorCode::unsupported_feature,
                             "The JPEG decoder exposes no scaling factors.", "libjpeg-turbo");
    }
    const tjscalingfactor* selected = nullptr;
    std::uint64_t selected_pixels = 0;
    for (int index = 0; index < count; ++index) {
        const int width = TJSCALED(static_cast<int>(info.width), factors[index]);
        const int height = TJSCALED(static_cast<int>(info.height), factors[index]);
        if (width <= 0 || height <= 0 || width > static_cast<int>(*options.maximum_extent) ||
            height > static_cast<int>(*options.maximum_extent))
            continue;
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
        if (!selected || pixels > selected_pixels) {
            selected = &factors[index];
            selected_pixels = pixels;
        }
    }
    if (!selected) {
        return Status::error(
            ErrorCode::limit_exceeded,
            "The requested JPEG preview is smaller than the codec's minimum scaled output.",
            "libjpeg-turbo");
    }
    const tjscalingfactor selected_factor = *selected;
    if (tj3SetScalingFactor(handle, selected_factor) != 0)
        return tj_error(handle, ErrorCode::decode_failed);
    info.width =
        static_cast<std::uint32_t>(TJSCALED(static_cast<int>(info.width), selected_factor));
    info.height =
        static_cast<std::uint32_t>(TJSCALED(static_cast<int>(info.height), selected_factor));
    return info;
}

DocumentInfo document_info(const JpegInfo& info) {
    DocumentInfo document;
    document.format = Format::jpeg;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    document.frames.push_back({info.width, info.height, 0, 0, std::chrono::nanoseconds{0},
                               info.pixel_format, false, std::nullopt});
    return document;
}

Result<int> turbo_pixel_format(const PixelFormat& format) {
    if (format.sample_type != SampleType::unsigned_integer || format.bits_per_channel != 8) {
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG encoding currently requires packed 8-bit unsigned pixels.",
                             "libjpeg-turbo");
    }
    switch (format.channels) {
    case ChannelLayout::gray:
        return TJPF_GRAY;
    case ChannelLayout::rgb:
        return TJPF_RGB;
    case ChannelLayout::rgba:
        return TJPF_RGBA;
    case ChannelLayout::bgr:
        return TJPF_BGR;
    case ChannelLayout::bgra:
        return TJPF_BGRA;
    case ChannelLayout::cmyk:
        return TJPF_CMYK;
    case ChannelLayout::gray_alpha:
    case ChannelLayout::indexed:
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG encoding does not accept this channel layout.", "libjpeg-turbo");
    }
    return Status::error(ErrorCode::unsupported_feature, "Unsupported JPEG pixel layout.",
                         "libjpeg-turbo");
}

constexpr std::size_t kJpegDestinationBytes = std::size_t{256} << 10U;

struct JpegErrorManager final {
    jpeg_error_mgr base{};
    std::jmp_buf jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
};

struct JpegDestinationManager final {
    jpeg_destination_mgr base{};
    ByteSink* sink = nullptr;
    JpegErrorManager* error = nullptr;
    std::array<JOCTET, kJpegDestinationBytes> buffer{};
    Status sink_error;
    bool has_sink_error = false;
};

struct JpegEncoderContext final {
    jpeg_compress_struct compressor{};
    JpegErrorManager error;
    JpegDestinationManager destination;
};

[[noreturn]] void jpeg_error_exit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegErrorManager*>(common->err);
    (*common->err->format_message)(common, error->message.data());
    std::longjmp(error->jump, 1);
}

void destination_failure(JpegDestinationManager* destination, Status status) {
    destination->sink_error = std::move(status);
    destination->has_sink_error = true;
    std::longjmp(destination->error->jump, 1);
}

void jpeg_init_destination(j_compress_ptr compressor) {
    auto* destination = reinterpret_cast<JpegDestinationManager*>(compressor->dest);
    destination->base.next_output_byte = destination->buffer.data();
    destination->base.free_in_buffer = destination->buffer.size();
}

boolean jpeg_empty_output_buffer(j_compress_ptr compressor) {
    auto* destination = reinterpret_cast<JpegDestinationManager*>(compressor->dest);
    Status failure;
    bool failed = false;
    {
        Result<void> written = destination->sink->write(
            std::as_bytes(std::span(destination->buffer.data(), destination->buffer.size())));
        if (!written) {
            failure = std::move(written).error();
            failed = true;
        }
    }
    if (failed)
        destination_failure(destination, std::move(failure));
    destination->base.next_output_byte = destination->buffer.data();
    destination->base.free_in_buffer = destination->buffer.size();
    return TRUE;
}

void jpeg_term_destination(j_compress_ptr compressor) {
    auto* destination = reinterpret_cast<JpegDestinationManager*>(compressor->dest);
    const std::size_t remaining = destination->buffer.size() - destination->base.free_in_buffer;
    if (remaining == 0)
        return;
    Status failure;
    bool failed = false;
    {
        Result<void> written = destination->sink->write(
            std::as_bytes(std::span(destination->buffer.data(), remaining)));
        if (!written) {
            failure = std::move(written).error();
            failed = true;
        }
    }
    if (failed)
        destination_failure(destination, std::move(failure));
}

void initialize_destination(JpegEncoderContext* context, ByteSink* sink) {
    context->destination.sink = sink;
    context->destination.error = &context->error;
    context->destination.base.init_destination = jpeg_init_destination;
    context->destination.base.empty_output_buffer = jpeg_empty_output_buffer;
    context->destination.base.term_destination = jpeg_term_destination;
    context->compressor.dest = &context->destination.base;
}

void configure_sampling(jpeg_compress_struct* compressor, ChromaSubsampling sampling) {
    if (sampling == ChromaSubsampling::none)
        return;
    compressor->comp_info[0].h_samp_factor = sampling == ChromaSubsampling::yuv444 ? 1 : 2;
    compressor->comp_info[0].v_samp_factor = sampling == ChromaSubsampling::yuv420 ? 2 : 1;
    compressor->comp_info[1].h_samp_factor = 1;
    compressor->comp_info[1].v_samp_factor = 1;
    compressor->comp_info[2].h_samp_factor = 1;
    compressor->comp_info[2].v_samp_factor = 1;
}

Result<J_COLOR_SPACE> jpeg_input_color_space(const PixelFormat& format) {
    if (format.sample_type != SampleType::unsigned_integer || format.bits_per_channel != 8) {
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG encoding requires packed 8-bit unsigned pixels.",
                             "libjpeg-turbo");
    }
    switch (format.channels) {
    case ChannelLayout::gray:
        return JCS_GRAYSCALE;
    case ChannelLayout::rgb:
        return JCS_EXT_RGB;
    case ChannelLayout::rgba:
        return JCS_EXT_RGBA;
    case ChannelLayout::bgr:
        return JCS_EXT_BGR;
    case ChannelLayout::bgra:
        return JCS_EXT_BGRA;
    case ChannelLayout::gray_alpha:
    case ChannelLayout::cmyk:
    case ChannelLayout::indexed:
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG encoding does not accept this channel layout.", "libjpeg-turbo");
    }
    return Status::error(ErrorCode::unsupported_feature, "Unsupported JPEG pixel layout.",
                         "libjpeg-turbo");
}

Result<void> begin_compression(JpegEncoderContext* context, std::uint32_t width,
                               std::uint32_t height, int components, J_COLOR_SPACE color_space,
                               ChromaSubsampling sampling, const EncodeOptions& options,
                               ByteSink* sink, bool raw_data) {
    context->compressor.err = jpeg_std_error(&context->error.base);
    context->error.base.error_exit = jpeg_error_exit;
    jpeg_create_compress(&context->compressor);
    initialize_destination(context, sink);
    context->compressor.image_width = width;
    context->compressor.image_height = height;
    context->compressor.input_components = components;
    context->compressor.in_color_space = color_space;
    jpeg_set_defaults(&context->compressor);
    configure_sampling(&context->compressor, sampling);
    jpeg_set_quality(&context->compressor, options.quality, TRUE);
    if (options.progressive)
        jpeg_simple_progression(&context->compressor);
    context->compressor.raw_data_in = raw_data ? TRUE : FALSE;
    jpeg_start_compress(&context->compressor, TRUE);
    return {};
}

Status compression_error(const JpegEncoderContext& context) {
    if (context.destination.has_sink_error)
        return context.destination.sink_error;
    return Status::error(ErrorCode::encode_failed,
                         context.error.message.front() != '\0'
                             ? std::string(context.error.message.data())
                             : std::string("libjpeg-turbo encoding failed."),
                         "libjpeg-turbo");
}

EncodedArtifactReceipt jpeg_receipt(const Document& document, ChromaSubsampling sampling) {
    EncodedArtifactReceipt receipt = receipt_for_document(document, Format::jpeg);
    receipt.jpeg_chroma_subsampling = sampling;
    return receipt;
}

EncodedArtifactReceipt jpeg_receipt(const DocumentDescriptor& descriptor,
                                    ChromaSubsampling sampling) {
    EncodedArtifactReceipt receipt = receipt_for_descriptor(descriptor, Format::jpeg);
    receipt.jpeg_chroma_subsampling = sampling;
    return receipt;
}

} // namespace

CodecCapability JpegCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::streaming_decode | CodecCapability::metadata_decode;
}

int JpegCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 3 && header[0] == std::byte{0xFF} && header[1] == std::byte{0xD8} &&
        header[2] == std::byte{0xFF}) {
        return 100;
    }
    return format_from_extension(name_hint) == Format::jpeg ? 10 : 0;
}

Result<DocumentInfo> JpegCodec::inspect(const Input& input, const DecodeOptions& options,
                                        std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<std::pair<TjHandle, JpegInfo>> header = read_header(bytes.value(), options.limits);
    if (!header)
        return header.error();
    Result<JpegInfo> info = scaled_info(header.value().first.get(), header.value().second, options);
    if (!info)
        return info.error();
    return document_info(info.value());
}

Result<DocumentDescriptor> JpegCodec::inspect_raster(const Input& input,
                                                     const DecodeOptions& options,
                                                     std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<std::pair<TjHandle, JpegInfo>> header = read_header(bytes.value(), options.limits);
    if (!header)
        return header.error();
    Result<JpegInfo> info = scaled_info(header.value().first.get(), header.value().second, options);
    if (!info)
        return info.error();
    if (options.raster_layout == RasterLayoutPolicy::native && !options.output_format &&
        native_planar_supported(info.value()))
        return native_descriptor(info.value());
    JpegInfo packed = info.value();
    packed.pixel_format = options.output_format.value_or(kRgba8);
    Result<int> pixel_format = turbo_pixel_format(packed.pixel_format);
    if (!pixel_format)
        return pixel_format.error();
    packed.turbo_pixel_format = pixel_format.value();
    return describe_document(document_info(packed));
}

Result<Document> JpegCodec::decode(const Input& input, const DecodeOptions& options,
                                   std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<std::pair<TjHandle, JpegInfo>> header = read_header(bytes.value(), options.limits);
    if (!header)
        return header.error();
    TjHandle handle = std::move(header.value().first);
    Result<JpegInfo> scaled = scaled_info(handle.get(), header.value().second, options);
    if (!scaled)
        return scaled.error();
    JpegInfo info = scaled.value();
    if (options.output_format) {
        Result<int> output = turbo_pixel_format(*options.output_format);
        if (!output)
            return output.error();
        info.pixel_format = *options.output_format;
        info.turbo_pixel_format = output.value();
    }
    Result<std::size_t> pixel_bytes = info.pixel_format.bytes_per_pixel();
    if (!pixel_bytes)
        return pixel_bytes.error();
    const std::uint64_t output_bytes =
        static_cast<std::uint64_t>(info.width) * info.height * pixel_bytes.value();
    if (output_bytes > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "JPEG output exceeds the owning decode limit.", "libjpeg-turbo");
    }
    Result<MutableImage> allocated =
        MutableImage::allocate(info.width, info.height, info.pixel_format);
    if (!allocated)
        return allocated.error();
    MutableImage pixels = std::move(allocated).value();
    if (pixels.row_stride() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status::error(ErrorCode::limit_exceeded, "JPEG output row is too large.",
                             "libjpeg-turbo");
    }
    if (stop.stop_requested())
        return cancelled_status();
    if (tj3Decompress8(handle.get(), reinterpret_cast<const unsigned char*>(bytes.value().data()),
                       bytes.value().size(),
                       reinterpret_cast<unsigned char*>(pixels.pixels().data()),
                       static_cast<int>(pixels.row_stride()), info.turbo_pixel_format) != 0) {
        return tj_error(handle.get(), ErrorCode::decode_failed);
    }
    if (stop.stop_requested())
        return cancelled_status();
    Document document;
    document.format = Format::jpeg;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    Frame frame;
    frame.image = std::move(pixels).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

bool native_jpeg_source(const RasterFrameDescriptor& frame) {
    if (frame.layout.alpha != AlphaMode::none)
        return false;
    if (frame.layout.color_model == ColorModel::gray) {
        return frame.layout.planes.size() == 1 &&
               frame.layout.planes.front().semantic == PlaneSemantic::gray &&
               frame.layout.planes.front().format == kGray8;
    }
    if (frame.layout.color_model != ColorModel::ycbcr || frame.layout.planes.size() != 3)
        return false;
    constexpr std::array semantics{PlaneSemantic::luma, PlaneSemantic::chroma_blue,
                                   PlaneSemantic::chroma_red};
    for (std::size_t index = 0; index < semantics.size(); ++index) {
        if (frame.layout.planes[index].semantic != semantics[index] ||
            frame.layout.planes[index].format != kGray8)
            return false;
    }
    return true;
}

bool packed_jpeg_source(const RasterFrameDescriptor& frame) {
    if (frame.layout.planes.size() != 1 ||
        frame.layout.planes.front().semantic != PlaneSemantic::packed) {
        return false;
    }
    const PixelFormat& format = frame.layout.planes.front().format;
    if (format.sample_type != SampleType::unsigned_integer || format.bits_per_channel != 8) {
        return false;
    }
    return format.channels == ChannelLayout::gray || format.channels == ChannelLayout::rgb ||
           format.channels == ChannelLayout::rgba || format.channels == ChannelLayout::bgr ||
           format.channels == ChannelLayout::bgra;
}

bool sdr_srgb(const ColorEncoding& color) noexcept {
    return color.dynamic_range == DynamicRange::standard &&
           (color.primaries == ColorPrimaries::unknown ||
            color.primaries == ColorPrimaries::srgb) &&
           (color.transfer == TransferFunction::unknown ||
            color.transfer == TransferFunction::srgb);
}

ChromaSubsampling resolved_sampling(const EncodeOptions& options) noexcept {
    if (options.chroma_subsampling)
        return *options.chroma_subsampling;
    if (options.quality >= 90)
        return ChromaSubsampling::yuv444;
    if (options.quality >= 80)
        return ChromaSubsampling::yuv422;
    return ChromaSubsampling::yuv420;
}

Result<void> JpegCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                       const DecodeOptions& options, std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<std::pair<TjHandle, JpegInfo>> header = read_header(bytes.value(), options.limits);
    if (!header)
        return header.error();
    TjHandle handle = std::move(header.value().first);
    Result<JpegInfo> scaled = scaled_info(handle.get(), header.value().second, options);
    if (!scaled)
        return scaled.error();
    const JpegInfo info = scaled.value();
    JpegInfo sinkInfo = info;
    sinkInfo.pixel_format = options.output_format.value_or(kRgba8);
    Result<int> sink_pixel_format = turbo_pixel_format(sinkInfo.pixel_format);
    if (!sink_pixel_format)
        return sink_pixel_format.error();
    sinkInfo.turbo_pixel_format = sink_pixel_format.value();
    Result<std::size_t> pixel_bytes = sinkInfo.pixel_format.bytes_per_pixel();
    if (!pixel_bytes)
        return pixel_bytes.error();
    if (info.width > std::numeric_limits<std::size_t>::max() / pixel_bytes.value()) {
        return Status::error(ErrorCode::limit_exceeded, "JPEG row size overflows.",
                             "libjpeg-turbo");
    }
    const std::size_t row_stride = static_cast<std::size_t>(info.width) * pixel_bytes.value();
    if (row_stride > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        info.height > std::numeric_limits<std::size_t>::max() / row_stride) {
        return Status::error(ErrorCode::limit_exceeded, "JPEG output size overflows.",
                             "libjpeg-turbo");
    }
    const std::size_t output_bytes = row_stride * info.height;
    const DocumentInfo metadata = document_info(sinkInfo);
    Result<void> status = sink.begin(metadata);
    if (!status)
        return status;
    status = sink.begin_frame(0, metadata.frames.front());
    if (!status)
        return status;

    std::span<std::byte> target = sink.frame_storage(0, row_stride, output_bytes);
    std::vector<std::byte> owned;
    if (target.size() != output_bytes) {
        if (output_bytes > options.limits.maximum_owned_output_bytes) {
            return Status::error(
                ErrorCode::limit_exceeded,
                "JPEG output exceeds the owning decode limit; use a storage-backed sink.",
                "libjpeg-turbo");
        }
        try {
            owned.resize(output_bytes);
        } catch (const std::bad_alloc&) {
            return Status::error(ErrorCode::out_of_memory, "Could not allocate JPEG output pixels.",
                                 "libjpeg-turbo");
        }
        target = owned;
    }
    if (stop.stop_requested())
        return cancelled_status();
    if (tj3Decompress8(handle.get(), reinterpret_cast<const unsigned char*>(bytes.value().data()),
                       bytes.value().size(), reinterpret_cast<unsigned char*>(target.data()),
                       static_cast<int>(row_stride), sinkInfo.turbo_pixel_format) != 0) {
        return tj_error(handle.get(), ErrorCode::decode_failed);
    }
    if (stop.stop_requested())
        return cancelled_status();
    if (!owned.empty()) {
        status = sink.write_rows(0, info.height, row_stride, owned);
        if (!status)
            return status;
    }
    status = sink.end_frame(0);
    if (!status)
        return status;
    return sink.end();
}

Result<void> JpegCodec::decode_into(const Input& input, RasterWriter& writer,
                                    const DecodeOptions& options, std::stop_token stop) const {
    if (options.raster_layout != RasterLayoutPolicy::native || options.output_format) {
        return Codec::decode_into(input, writer, options, stop);
    }
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<std::pair<TjHandle, JpegInfo>> header = read_header(bytes.value(), options.limits);
    if (!header)
        return header.error();
    TjHandle handle = std::move(header.value().first);
    Result<JpegInfo> scaled = scaled_info(handle.get(), header.value().second, options);
    if (!scaled)
        return scaled.error();
    const JpegInfo info = scaled.value();
    if (!native_planar_supported(info))
        return Codec::decode_into(input, writer, options, stop);
    Result<DocumentDescriptor> descriptor = native_descriptor(info);
    if (!descriptor)
        return descriptor.error();
    if (!matching_descriptor(writer.descriptor(), descriptor.value())) {
        return Status::error(ErrorCode::invalid_argument,
                             "JPEG native raster writer does not match the decoded plane layout.",
                             "libjpeg-turbo");
    }
    const RasterFrameDescriptor& frame = descriptor.value().frames.front();
    Result<WritablePlaneSet> prepared =
        prepare_writable_planes(writer, 0, frame, options.limits, "libjpeg-turbo");
    if (!prepared)
        return prepared.error();
    WritablePlaneSet planes = std::move(prepared).value();
    if (stop.stop_requested())
        return cancelled_status();
    if (tj3DecompressToYUVPlanes8(
            handle.get(), reinterpret_cast<const unsigned char*>(bytes.value().data()),
            bytes.value().size(), planes.pointers.data(), planes.strides.data()) != 0) {
        return tj_error(handle.get(), ErrorCode::decode_failed);
    }
    if (stop.stop_requested())
        return cancelled_status();
    return publish_writable_planes(writer, 0, frame, planes, stop);
}

Result<EncodedArtifactReceipt> JpegCodec::encode_to_sink(const Document& document,
                                                         const Output& output,
                                                         const EncodeOptions& options,
                                                         std::stop_token stop) const {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "JPEG encoding requires one frame.",
                             "libjpeg-turbo");
    }
    const ImageView view = document.frames.front().image.view();
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    Result<J_COLOR_SPACE> color_space = jpeg_input_color_space(view.format);
    if (!color_space)
        return color_space.error();
    if (view.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        view.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        view.row_stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status::error(ErrorCode::limit_exceeded,
                             "JPEG dimensions or stride exceed codec limits.", "libjpeg-turbo");
    }
    const bool grayscale = view.format.channels == ChannelLayout::gray;
    Result<ChromaSubsampling> resolved = resolve_jpeg_chroma_subsampling(options, grayscale);
    if (!resolved)
        return resolved.error();
    const ChromaSubsampling sampling = resolved.value();
    auto context = std::make_unique<JpegEncoderContext>();
    context->compressor.err = jpeg_std_error(&context->error.base);
    context->error.base.error_exit = jpeg_error_exit;
    if (setjmp(context->error.jump) != 0) {
        jpeg_destroy_compress(&context->compressor);
        return compression_error(*context);
    }
    if (stop.stop_requested())
        return cancelled_status();
    Result<void> begun =
        begin_compression(context.get(), view.width, view.height,
                          grayscale ? 1
                                    : (view.format.channels == ChannelLayout::rgba ||
                                               view.format.channels == ChannelLayout::bgra
                                           ? 4
                                           : 3),
                          color_space.value(), sampling, options, output.sink.get(), false);
    if (!begun)
        return begun.error();
    while (context->compressor.next_scanline < context->compressor.image_height) {
        if (stop.stop_requested()) {
            jpeg_destroy_compress(&context->compressor);
            return cancelled_status();
        }
        const std::size_t offset =
            static_cast<std::size_t>(context->compressor.next_scanline) * view.row_stride;
        JSAMPROW row =
            reinterpret_cast<JSAMPROW>(const_cast<std::byte*>(view.pixels.data() + offset));
        if (jpeg_write_scanlines(&context->compressor, &row, 1) != 1) {
            jpeg_destroy_compress(&context->compressor);
            return Status::error(ErrorCode::encode_failed,
                                 "libjpeg-turbo did not consume a JPEG scanline.", "libjpeg-turbo");
        }
    }
    if (stop.stop_requested()) {
        jpeg_destroy_compress(&context->compressor);
        return cancelled_status();
    }
    jpeg_finish_compress(&context->compressor);
    jpeg_destroy_compress(&context->compressor);
    return jpeg_receipt(document, sampling);
}

Result<EncodedArtifactReceipt> JpegCodec::encode_raster_to_sink(const RasterSource& source,
                                                                const Output& output,
                                                                const EncodeOptions& options,
                                                                std::stop_token stop) const {
    const DocumentDescriptor& descriptor = source.descriptor();
    if (descriptor.frames.size() != 1) {
        return Codec::encode_raster_to_sink(source, output, options, stop);
    }
    const RasterFrameDescriptor& frame = descriptor.frames.front();
    if (packed_jpeg_source(frame) && frame.x == 0 && frame.y == 0 &&
        frame.width == descriptor.canvas_width && frame.height == descriptor.canvas_height) {
        Result<void> descriptor_status = descriptor.validate();
        if (!descriptor_status)
            return descriptor_status.error();
        const PlaneDescriptor& plane = frame.layout.planes.front();
        Result<J_COLOR_SPACE> color_space = jpeg_input_color_space(plane.format);
        if (!color_space)
            return color_space.error();
        Result<std::size_t> row_bytes_result = plane.row_bytes();
        if (!row_bytes_result)
            return row_bytes_result.error();
        const std::size_t row_bytes = row_bytes_result.value();
        if (frame.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            frame.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            row_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "JPEG dimensions or stride exceed codec limits.", "libjpeg-turbo");
        }
        const bool grayscale = plane.format.channels == ChannelLayout::gray;
        Result<ChromaSubsampling> selected = resolve_jpeg_chroma_subsampling(options, grayscale);
        if (!selected)
            return selected.error();
        const ChromaSubsampling sampling = selected.value();
        std::vector<std::byte> row(row_bytes);
        auto context = std::make_unique<JpegEncoderContext>();
        context->compressor.err = jpeg_std_error(&context->error.base);
        context->error.base.error_exit = jpeg_error_exit;
        if (setjmp(context->error.jump) != 0) {
            jpeg_destroy_compress(&context->compressor);
            return compression_error(*context);
        }
        if (stop.stop_requested())
            return cancelled_status();
        const bool alpha = plane.format.channels == ChannelLayout::rgba ||
                           plane.format.channels == ChannelLayout::bgra;
        Result<void> begun = begin_compression(context.get(), frame.width, frame.height,
                                               grayscale ? 1 : (alpha ? 4 : 3), color_space.value(),
                                               sampling, options, output.sink.get(), false);
        if (!begun)
            return begun.error();
        while (context->compressor.next_scanline < context->compressor.image_height) {
            if (stop.stop_requested()) {
                jpeg_destroy_compress(&context->compressor);
                return cancelled_status();
            }
            const std::uint32_t y = context->compressor.next_scanline;
            Result<void> read = source.read_rows(0, 0, y, 1, row_bytes, row, stop);
            if (!read) {
                jpeg_destroy_compress(&context->compressor);
                return read.error();
            }
            if (alpha) {
                auto* pixels = reinterpret_cast<std::uint8_t*>(row.data());
                for (std::uint32_t x = 0; x < frame.width; ++x) {
                    std::uint8_t* pixel = pixels + static_cast<std::size_t>(x) * 4U;
                    const unsigned int opacity = pixel[3];
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        if (plane.format.alpha == AlphaMode::premultiplied) {
                            pixel[channel] = static_cast<std::uint8_t>(
                                (std::min)(255U, static_cast<unsigned int>(pixel[channel]) + 255U -
                                                     opacity));
                        } else {
                            pixel[channel] = static_cast<std::uint8_t>(
                                (static_cast<unsigned int>(pixel[channel]) * opacity +
                                 255U * (255U - opacity) + 127U) /
                                255U);
                        }
                    }
                    pixel[3] = 255;
                }
            }
            JSAMPROW scanline = reinterpret_cast<JSAMPROW>(row.data());
            if (jpeg_write_scanlines(&context->compressor, &scanline, 1) != 1) {
                jpeg_destroy_compress(&context->compressor);
                return Status::error(ErrorCode::encode_failed,
                                     "libjpeg-turbo did not consume a JPEG scanline.",
                                     "libjpeg-turbo");
            }
        }
        if (stop.stop_requested()) {
            jpeg_destroy_compress(&context->compressor);
            return cancelled_status();
        }
        jpeg_finish_compress(&context->compressor);
        jpeg_destroy_compress(&context->compressor);
        return jpeg_receipt(descriptor, sampling);
    }
    if (!native_jpeg_source(frame)) {
        return Codec::encode_raster_to_sink(source, output, options, stop);
    }
    if (frame.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        frame.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        frame.layout.color_range != ColorRange::full) {
        return Status::error(ErrorCode::unsupported_feature,
                             "JPEG native planar encoding requires full-range codec-sized planes.",
                             "libjpeg-turbo");
    }
    const bool grayscale = frame.layout.color_model == ColorModel::gray;
    Result<ChromaSubsampling> selected = resolve_jpeg_chroma_subsampling(options, grayscale);
    if (!selected)
        return selected.error();
    const ChromaSubsampling resolved = selected.value();
    if (!grayscale && resolved != frame.layout.chroma_subsampling) {
        return Status::error(
            ErrorCode::unsupported_feature,
            "JPEG native planar input sampling does not match the requested output sampling.",
            "libjpeg-turbo");
    }
    Result<int> subsampling = grayscale ? Result<int>(TJSAMP_GRAY) : turbo_subsampling(resolved);
    if (!subsampling)
        return subsampling.error();
    const std::size_t expected_planes = subsampling.value() == TJSAMP_GRAY ? 1U : 3U;
    if (frame.layout.planes.size() != expected_planes) {
        return Status::error(ErrorCode::invalid_argument,
                             "JPEG native planar input has the wrong plane count.",
                             "libjpeg-turbo");
    }
    for (std::size_t index = 0; index < expected_planes; ++index) {
        const int width = tj3YUVPlaneWidth(static_cast<int>(index), static_cast<int>(frame.width),
                                           subsampling.value());
        const int height = tj3YUVPlaneHeight(static_cast<int>(index),
                                             static_cast<int>(frame.height), subsampling.value());
        if (width <= 0 || height <= 0 ||
            frame.layout.planes[index].width != static_cast<std::uint32_t>(width) ||
            frame.layout.planes[index].height != static_cast<std::uint32_t>(height)) {
            return Status::error(ErrorCode::invalid_argument,
                                 "JPEG native planar dimensions do not match chroma subsampling.",
                                 "libjpeg-turbo");
        }
    }
    const int y_vertical = resolved == ChromaSubsampling::yuv420 ? 2 : 1;
    const std::array<int, 3> vertical_factors{y_vertical, 1, 1};
    const std::size_t plane_count = grayscale ? 1U : 3U;
    std::array<std::vector<JSAMPLE>, 3> buffers;
    std::array<std::vector<JSAMPROW>, 3> rows;
    std::array<std::size_t, 3> padded_widths{};
    std::array<JSAMPARRAY, 3> image_rows{};
    for (std::size_t index = 0; index < plane_count; ++index) {
        const PlaneDescriptor& plane = frame.layout.planes[index];
        padded_widths[index] =
            (static_cast<std::size_t>(plane.width) + DCTSIZE - 1U) / DCTSIZE * DCTSIZE;
        const std::size_t line_count = static_cast<std::size_t>(vertical_factors[index]) * DCTSIZE;
        buffers[index].resize(padded_widths[index] * line_count);
        rows[index].resize(line_count);
        for (std::size_t row = 0; row < line_count; ++row)
            rows[index][row] = buffers[index].data() + row * padded_widths[index];
        image_rows[index] = rows[index].data();
    }
    auto context = std::make_unique<JpegEncoderContext>();
    context->compressor.err = jpeg_std_error(&context->error.base);
    context->error.base.error_exit = jpeg_error_exit;
    if (setjmp(context->error.jump) != 0) {
        jpeg_destroy_compress(&context->compressor);
        return compression_error(*context);
    }
    if (stop.stop_requested())
        return cancelled_status();
    Result<void> begun = begin_compression(context.get(), frame.width, frame.height,
                                           grayscale ? 1 : 3, grayscale ? JCS_GRAYSCALE : JCS_YCbCr,
                                           resolved, options, output.sink.get(), true);
    if (!begun)
        return begun.error();
    while (context->compressor.next_scanline < context->compressor.image_height) {
        if (stop.stop_requested()) {
            jpeg_destroy_compress(&context->compressor);
            return cancelled_status();
        }
        const JDIMENSION luma_row = context->compressor.next_scanline;
        for (std::size_t index = 0; index < plane_count; ++index) {
            const PlaneDescriptor& plane = frame.layout.planes[index];
            const std::uint32_t first_row = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(luma_row) * vertical_factors[index]) / y_vertical);
            const std::uint32_t line_count =
                static_cast<std::uint32_t>(vertical_factors[index] * DCTSIZE);
            const std::uint32_t available =
                first_row < plane.height ? std::min(line_count, plane.height - first_row) : 0U;
            if (available == 0) {
                jpeg_destroy_compress(&context->compressor);
                return Status::error(ErrorCode::invalid_argument,
                                     "JPEG native planar input ended before an iMCU row.",
                                     "libjpeg-turbo");
            }
            Result<void> read = source.read_rows(
                0, static_cast<std::uint32_t>(index), first_row, available, padded_widths[index],
                std::as_writable_bytes(std::span(buffers[index])), stop);
            if (!read) {
                jpeg_destroy_compress(&context->compressor);
                return read.error();
            }
            for (std::uint32_t row = 0; row < available; ++row) {
                JSAMPROW pixels = rows[index][row];
                std::fill(pixels + plane.width, pixels + padded_widths[index],
                          pixels[plane.width - 1U]);
            }
            for (std::uint32_t row = available; row < line_count; ++row) {
                std::copy_n(rows[index][available - 1U], padded_widths[index], rows[index][row]);
            }
        }
        const JDIMENSION submitted = static_cast<JDIMENSION>(y_vertical * DCTSIZE);
        if (jpeg_write_raw_data(&context->compressor, image_rows.data(), submitted) != submitted) {
            jpeg_destroy_compress(&context->compressor);
            return Status::error(ErrorCode::encode_failed,
                                 "libjpeg-turbo did not consume a JPEG iMCU row.", "libjpeg-turbo");
        }
    }
    if (stop.stop_requested()) {
        jpeg_destroy_compress(&context->compressor);
        return cancelled_status();
    }
    jpeg_finish_compress(&context->compressor);
    jpeg_destroy_compress(&context->compressor);
    return jpeg_receipt(descriptor, resolved);
}

RasterEncodeRoute JpegCodec::raster_encode_route(const DocumentDescriptor& descriptor,
                                                 const EncodeOptions& options) const noexcept {
    if (descriptor.frames.size() != 1 || !sdr_srgb(descriptor.color))
        return RasterEncodeRoute::materialized;
    const RasterFrameDescriptor& frame = descriptor.frames.front();
    if (frame.x != 0 || frame.y != 0 || frame.width != descriptor.canvas_width ||
        frame.height != descriptor.canvas_height || !sdr_srgb(frame.color))
        return RasterEncodeRoute::materialized;
    if (packed_jpeg_source(frame))
        return RasterEncodeRoute::native;
    if (frame.layout.color_range != ColorRange::full || !native_jpeg_source(frame))
        return RasterEncodeRoute::materialized;
    if (frame.layout.color_model != ColorModel::gray &&
        frame.layout.chroma_subsampling != resolved_sampling(options))
        return RasterEncodeRoute::materialized;
    return RasterEncodeRoute::native;
}

} // namespace snow::image::internal

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
