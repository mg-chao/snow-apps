#include "codec_registry.h"

#include "codecs/bmp_codec.h"
#include "codecs/pnm_codec.h"
#if defined(SNOW_IMAGE_HAS_GIF)
#include "codecs/gif_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_ICON)
#include "codecs/icon_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_XBITMAP)
#include "codecs/xbitmap_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_SVG)
#include "codecs/svg_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_HEIF)
#include "codecs/heif_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_JXL)
#include "codecs/jxl_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_EXR)
#include "codecs/exr_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_JPEG)
#include "codecs/jpeg_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_PNG)
#include "codecs/png_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_WEBP)
#include "codecs/webp_codec.h"
#endif
#if defined(SNOW_IMAGE_HAS_TIFF)
#include "codecs/tiff_codec.h"
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace snow::image::internal {
namespace {

EncoderInfo describe_encoder(Format format) {
    EncoderInfo info;
    info.format = format;
    switch (format) {
    case Format::png:
        info.features = EncoderFeature::alpha | EncoderFeature::indexed |
                        EncoderFeature::compression_level | EncoderFeature::interlaced |
                        EncoderFeature::pixel_exact | EncoderFeature::metadata;
        info.compression_level = {0, 9, 6};
        info.cancellation = CodecCancellation::cooperative;
        break;
    case Format::jpeg:
        info.features = EncoderFeature::quality | EncoderFeature::progressive |
                        EncoderFeature::chroma_subsampling;
        info.quality = {1, 100, 75};
        info.cancellation = CodecCancellation::cooperative;
        break;
    case Format::tiff:
        info.features = EncoderFeature::alpha | EncoderFeature::compression_level |
                        EncoderFeature::pixel_exact | EncoderFeature::metadata;
        info.compression_level = {0, 9, 6};
        info.cancellation = CodecCancellation::cooperative;
        break;
    case Format::webp:
        info.features = EncoderFeature::alpha | EncoderFeature::animation |
                        EncoderFeature::quality | EncoderFeature::lossless |
                        EncoderFeature::effort | EncoderFeature::metadata;
        info.quality = {0, 100, 75};
        info.effort = {0, 6, 4};
        info.lossless_effort = {0, 9, 6};
        info.limits = {16'383, 16'383, static_cast<std::uint32_t>(std::numeric_limits<int>::max())};
        break;
    case Format::avif:
        info.features = EncoderFeature::alpha | EncoderFeature::animation |
                        EncoderFeature::quality | EncoderFeature::lossless |
                        EncoderFeature::effort | EncoderFeature::metadata;
        info.quality = {0, 100, 50};
        info.effort = {1, 9, 6};
        break;
    case Format::jxl:
        info.features = EncoderFeature::alpha | EncoderFeature::animation |
                        EncoderFeature::quality | EncoderFeature::lossless |
                        EncoderFeature::effort | EncoderFeature::progressive |
                        EncoderFeature::metadata;
        info.quality = {0, 100, 75};
        info.effort = {1, 10, 7};
        break;
    case Format::gif:
        info.features = EncoderFeature::animation | EncoderFeature::indexed;
        break;
    case Format::bmp:
    case Format::cur:
    case Format::ico:
        info.features = EncoderFeature::alpha;
        info.cancellation = CodecCancellation::cooperative;
        break;
    case Format::heif:
        info.features = EncoderFeature::alpha | EncoderFeature::animation |
                        EncoderFeature::quality | EncoderFeature::lossless |
                        EncoderFeature::effort | EncoderFeature::metadata;
        info.quality = {0, 100, 75};
        info.effort = {1, 9, 6};
        break;
    case Format::svgz:
        info.features = EncoderFeature::compression_level;
        info.compression_level = {0, 9, 6};
        info.cancellation = CodecCancellation::cooperative;
        break;
    case Format::exr:
        info.features = EncoderFeature::compression_level | EncoderFeature::metadata;
        info.compression_level = {0, 9, 6};
        info.cancellation = CodecCancellation::cooperative;
        break;
    case Format::pbm:
    case Format::pgm:
    case Format::ppm:
    case Format::svg:
    case Format::xbm:
    case Format::xpm:
    case Format::unknown:
        info.cancellation = CodecCancellation::cooperative;
        break;
    }
    return info;
}

bool compatible_writer_descriptor(const DocumentDescriptor& expected,
                                  const DocumentDescriptor& actual) {
    if (expected.canvas_width != actual.canvas_width ||
        expected.canvas_height != actual.canvas_height ||
        expected.frames.size() != actual.frames.size())
        return false;
    for (std::size_t index = 0; index < expected.frames.size(); ++index) {
        const RasterFrameDescriptor& left = expected.frames[index];
        const RasterFrameDescriptor& right = actual.frames[index];
        if (left.width != right.width || left.height != right.height || left.x != right.x ||
            left.y != right.y || left.layout.planes.size() != 1 ||
            right.layout.planes.size() != 1 ||
            left.layout.planes.front() != right.layout.planes.front())
            return false;
    }
    return true;
}

bool first_frame_only(Format format) noexcept {
    switch (format) {
    case Format::bmp:
    case Format::jpeg:
    case Format::pbm:
    case Format::pgm:
    case Format::png:
    case Format::ppm:
    case Format::xbm:
    case Format::xpm:
        return true;
    case Format::tiff:
        return true;
    default:
        return false;
    }
}

EncodedArtifactReceipt receipt_for_descriptor_impl(const DocumentDescriptor& descriptor,
                                                   Format format) {
    EncodedArtifactReceipt receipt;
    receipt.format = format;
    receipt.document_kind = descriptor.kind;
    receipt.canvas_width = descriptor.canvas_width;
    receipt.canvas_height = descriptor.canvas_height;
    if (descriptor.frames.empty()) {
        return receipt;
    }
    const std::size_t frame_count = first_frame_only(format)
                                        ? std::min<std::size_t>(1, descriptor.frames.size())
                                        : descriptor.frames.size();
    receipt.emitted_frame_extents.reserve(frame_count);
    for (std::size_t index = 0; index < frame_count; ++index) {
        const RasterFrameDescriptor& frame = descriptor.frames[index];
        EncodedFrameExtent extent{frame.x, frame.y, frame.width, frame.height};
        if (first_frame_only(format) || format == Format::ico || format == Format::cur) {
            extent.x = 0;
            extent.y = 0;
        }
        receipt.emitted_frame_extents.push_back(extent);
    }
    if (format == Format::ico || format == Format::cur) {
        receipt.canvas_width = 0;
        receipt.canvas_height = 0;
        for (const EncodedFrameExtent& extent : receipt.emitted_frame_extents) {
            receipt.canvas_width = std::max(receipt.canvas_width, extent.width);
            receipt.canvas_height = std::max(receipt.canvas_height, extent.height);
        }
    } else if (first_frame_only(format) && !receipt.emitted_frame_extents.empty()) {
        receipt.canvas_width = receipt.emitted_frame_extents.front().width;
        receipt.canvas_height = receipt.emitted_frame_extents.front().height;
    }
    receipt.emitted_frame_count = static_cast<std::uint32_t>(receipt.emitted_frame_extents.size());
    return receipt;
}

EncodedArtifactReceipt receipt_for_document_impl(const Document& document, Format format) {
    DocumentDescriptor descriptor;
    descriptor.format = document.format;
    descriptor.kind = document.vector              ? DocumentKind::vector
                      : document.exr_parts.empty() ? DocumentKind::raster
                                                   : DocumentKind::deep;
    descriptor.canvas_width = document.canvas_width;
    descriptor.canvas_height = document.canvas_height;
    descriptor.frames.reserve(document.frames.size());
    for (const Frame& source : document.frames) {
        RasterFrameDescriptor frame;
        frame.width = source.image.width();
        frame.height = source.image.height();
        frame.x = source.x;
        frame.y = source.y;
        descriptor.frames.push_back(std::move(frame));
    }
    return receipt_for_descriptor_impl(descriptor, format);
}

Result<void> validate_receipt_shape(const EncodedArtifactReceipt& receipt, Format format) {
    if (receipt.format != format ||
        static_cast<std::uint8_t>(receipt.document_kind) >
            static_cast<std::uint8_t>(DocumentKind::deep) ||
        receipt.emitted_frame_count != receipt.emitted_frame_extents.size()) {
        return Status::error(ErrorCode::encode_failed,
                             "The encoder returned an invalid artifact receipt.");
    }
    if (receipt.document_kind == DocumentKind::raster && receipt.emitted_frame_count == 0) {
        return Status::error(ErrorCode::encode_failed,
                             "The raster encoder returned no emitted frames.");
    }
    const bool icon = format == Format::ico || format == Format::cur;
    if (!icon && receipt.emitted_frame_count > 0 &&
        (receipt.canvas_width == 0 || receipt.canvas_height == 0)) {
        return Status::error(ErrorCode::encode_failed,
                             "The encoder returned an empty artifact canvas.");
    }
    for (const EncodedFrameExtent& extent : receipt.emitted_frame_extents) {
        if (extent.width == 0 || extent.height == 0 ||
            extent.x > std::numeric_limits<std::uint32_t>::max() - extent.width ||
            extent.y > std::numeric_limits<std::uint32_t>::max() - extent.height) {
            return Status::error(ErrorCode::encode_failed,
                                 "The encoder returned an out-of-range frame extent.");
        }
        if (!icon &&
            (extent.x > receipt.canvas_width || extent.width > receipt.canvas_width - extent.x ||
             extent.y > receipt.canvas_height ||
             extent.height > receipt.canvas_height - extent.y)) {
            return Status::error(ErrorCode::encode_failed,
                                 "The encoder returned a frame outside its canvas.");
        }
    }
    if (icon) {
        for (const EncodedFrameExtent& extent : receipt.emitted_frame_extents) {
            if (extent.width > receipt.canvas_width || extent.height > receipt.canvas_height) {
                return Status::error(ErrorCode::encode_failed,
                                     "The icon receipt canvas is smaller than an emitted frame.");
            }
        }
    }
    return {};
}

class RasterWriterSink final : public PixelSink {
  public:
    explicit RasterWriterSink(RasterWriter& writer, std::stop_token stop)
        : writer_(writer), stop_(stop) {}

    Result<void> begin(const DocumentInfo& document) override {
        Result<DocumentDescriptor> descriptor = describe_document(document);
        if (!descriptor)
            return descriptor.error();
        if (!compatible_writer_descriptor(writer_.descriptor(), descriptor.value()))
            return Status::error(ErrorCode::invalid_argument,
                                 "Raster writer descriptor does not match decoded output.");
        begun_ = true;
        return {};
    }

    Result<void> begin_frame(std::uint32_t frame_index, const FrameInfo& frame) override {
        if (!begun_ || active_frame_ != kNoFrame ||
            frame_index >= writer_.descriptor().frames.size())
            return Status::error(ErrorCode::invalid_argument,
                                 "Decoder began an invalid raster frame.");
        const RasterFrameDescriptor& target = writer_.descriptor().frames[frame_index];
        if (target.width != frame.width || target.height != frame.height ||
            target.layout.planes.size() != 1 ||
            target.layout.planes.front().format != frame.native_format)
            return Status::error(ErrorCode::invalid_argument,
                                 "Decoded frame layout does not match the raster writer.");
        active_frame_ = frame_index;
        expected_row_ = 0;
        return {};
    }

    Result<void> write_rows(std::uint32_t first_row, std::uint32_t row_count,
                            std::size_t row_stride, std::span<const std::byte> pixels) override {
        if (active_frame_ == kNoFrame || first_row != expected_row_)
            return Status::error(ErrorCode::corrupt_data,
                                 "Decoder emitted non-sequential raster rows.");
        Result<void> status =
            writer_.write_rows(active_frame_, 0, first_row, row_count, row_stride, pixels, stop_);
        if (status)
            expected_row_ += row_count;
        return status;
    }

    Result<void> end_frame(std::uint32_t frame_index) override {
        if (frame_index != active_frame_ ||
            expected_row_ != writer_.descriptor().frames[frame_index].height)
            return Status::error(ErrorCode::truncated_data,
                                 "Decoder ended an incomplete raster frame.");
        active_frame_ = kNoFrame;
        return {};
    }

    Result<void> end() override {
        if (!begun_ || active_frame_ != kNoFrame)
            return Status::error(ErrorCode::truncated_data,
                                 "Decoder ended an incomplete raster document.");
        ended_ = true;
        return writer_.commit();
    }

    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

  private:
    static constexpr std::uint32_t kNoFrame = std::numeric_limits<std::uint32_t>::max();
    RasterWriter& writer_;
    std::stop_token stop_;
    std::uint32_t active_frame_ = kNoFrame;
    std::uint32_t expected_row_ = 0;
    bool begun_ = false;
    bool ended_ = false;
};

Result<Document> materialize_raster(const RasterSource& source, std::stop_token stop) {
    const DocumentDescriptor& descriptor = source.descriptor();
    Result<void> valid = descriptor.validate();
    if (!valid)
        return valid.error();
    try {
        Document document;
        document.format = descriptor.format;
        document.canvas_width = descriptor.canvas_width;
        document.canvas_height = descriptor.canvas_height;
        document.loop_count = descriptor.loop_count;
        document.metadata = descriptor.metadata;
        document.color = descriptor.color;
        document.frames.reserve(descriptor.frames.size());
        for (std::uint32_t index = 0; index < descriptor.frames.size(); ++index) {
            if (stop.stop_requested())
                return cancelled_status();
            const RasterFrameDescriptor& source_frame = descriptor.frames[index];
            if (source_frame.layout.planes.size() != 1 ||
                source_frame.layout.planes.front().semantic != PlaneSemantic::packed) {
                return Status::error(
                    ErrorCode::unsupported_feature,
                    "The compatibility encoder accepts one packed plane per frame.");
            }
            const PlaneDescriptor& plane = source_frame.layout.planes.front();
            Result<MutableImage> allocated =
                MutableImage::allocate(source_frame.width, source_frame.height, plane.format, 64);
            if (!allocated)
                return allocated.error();
            MutableImage image = std::move(allocated).value();
            Result<void> read = source.read_rows(index, 0, 0, source_frame.height,
                                                 image.row_stride(), image.pixels(), stop);
            if (!read)
                return read.error();
            Frame frame;
            frame.image = std::move(image).freeze();
            frame.x = source_frame.x;
            frame.y = source_frame.y;
            frame.duration = source_frame.duration;
            frame.blend = source_frame.blend;
            frame.disposal = source_frame.disposal;
            frame.metadata = source_frame.metadata;
            frame.color = source_frame.color;
            frame.cursor_hotspot = source_frame.cursor_hotspot;
            document.frames.push_back(std::move(frame));
        }
        return document;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not materialize compatibility encoder frames.");
    }
}

Result<Document> map_raster(const RasterSource& source, std::stop_token stop) {
    const DocumentDescriptor& descriptor = source.descriptor();
    try {
        Document document;
        document.format = descriptor.format;
        document.canvas_width = descriptor.canvas_width;
        document.canvas_height = descriptor.canvas_height;
        document.loop_count = descriptor.loop_count;
        document.metadata = descriptor.metadata;
        document.color = descriptor.color;
        document.frames.reserve(descriptor.frames.size());
        for (std::uint32_t index = 0; index < descriptor.frames.size(); ++index) {
            if (stop.stop_requested())
                return cancelled_status();
            const RasterFrameDescriptor& source_frame = descriptor.frames[index];
            if (source_frame.layout.planes.size() != 1 ||
                source_frame.layout.planes.front().semantic != PlaneSemantic::packed) {
                return Status::error(
                    ErrorCode::unsupported_feature,
                    "The mapped compatibility encoder accepts one packed plane per frame.");
            }
            Result<MappedPlane> mapped = source.map_plane(index, 0);
            if (!mapped)
                return mapped.error();
            Result<SharedPixelBuffer> buffer =
                SharedPixelBuffer::adopt(mapped.value().owner, mapped.value().pixels);
            if (!buffer)
                return buffer.error();
            Result<Image> image = Image::adopt(
                source_frame.width, source_frame.height, source_frame.layout.planes.front().format,
                mapped.value().row_stride, std::move(buffer).value());
            if (!image)
                return image.error();
            Frame frame;
            frame.image = std::move(image).value();
            frame.x = source_frame.x;
            frame.y = source_frame.y;
            frame.duration = source_frame.duration;
            frame.blend = source_frame.blend;
            frame.disposal = source_frame.disposal;
            frame.metadata = source_frame.metadata;
            frame.color = source_frame.color;
            frame.cursor_hotspot = source_frame.cursor_hotspot;
            document.frames.push_back(std::move(frame));
        }
        return document;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate mapped compatibility encoder views.");
    }
}

} // namespace

EncodedArtifactReceipt receipt_for_descriptor(const DocumentDescriptor& descriptor, Format format) {
    return receipt_for_descriptor_impl(descriptor, format);
}

EncodedArtifactReceipt receipt_for_document(const Document& document, Format format) {
    return receipt_for_document_impl(document, format);
}

Result<DocumentDescriptor> Codec::inspect_raster(const Input& input, const DecodeOptions& options,
                                                 std::stop_token stop) const {
    Result<DocumentInfo> inspected = inspect(input, options, stop);
    if (!inspected)
        return inspected.error();
    Result<DocumentDescriptor> descriptor = describe_document(inspected.value());
    if (!descriptor)
        return descriptor.error();
    if (options.output_format) {
        for (RasterFrameDescriptor& frame : descriptor.value().frames) {
            PlaneDescriptor& plane = frame.layout.planes.front();
            plane.format = *options.output_format;
            plane.significant_bits = plane.format.bits_per_channel;
            frame.layout.alpha = plane.format.alpha;
        }
    }
    return descriptor;
}

Result<void> Codec::decode_into(const Input& input, RasterWriter& writer,
                                const DecodeOptions& options, std::stop_token stop) const {
    RasterWriterSink sink(writer, stop);
    Result<void> status = decode_to_sink(input, sink, options, stop);
    if (!status || !sink.ended()) {
        writer.abort();
        return status ? Status::error(ErrorCode::truncated_data,
                                      "Decoder did not commit its raster output.")
                      : status.error();
    }
    return {};
}

Result<EncodeResult> Codec::encode(const Document& document, const Output& output,
                                   const EncodeOptions& options, std::stop_token stop) const {
    if (!output.sink) {
        return Status::error(ErrorCode::invalid_argument, "Output has no byte sink.");
    }
    Result<std::uint64_t> start = output.sink->position();
    if (!start)
        return start.error();
    Result<EncodedArtifactReceipt> encoded = encode_to_sink(document, output, options, stop);
    if (!encoded)
        return encoded.error();
    Result<void> flushed = output.sink->flush();
    if (!flushed)
        return flushed.error();
    Result<std::uint64_t> end = output.sink->position();
    if (!end)
        return end.error();
    if (end.value() < start.value()) {
        return Status::error(ErrorCode::io_error,
                             "The output sink position moved backwards while encoding.");
    }
    if (end.value() == start.value()) {
        return Status::error(ErrorCode::encode_failed, "The encoder finalized an empty artifact.");
    }
    const bool canonical_webp =
        format() == Format::webp && options.lossless &&
        std::all_of(document.frames.begin(), document.frames.end(),
                    [](const Frame& frame) { return frame.image.format() == kRgba8; });
    PixelRoundTrip round_trip = PixelRoundTrip::codec_artifact;
    if (format() == Format::png || format() == Format::tiff || canonical_webp ||
        (format() == Format::jxl && options.lossless)) {
        round_trip = PixelRoundTrip::exact;
    }
    EncodedArtifactReceipt receipt = std::move(encoded).value();
    Result<void> receipt_status = validate_receipt_shape(receipt, format());
    if (!receipt_status)
        return receipt_status.error();
    receipt.encoder_finalized_and_sink_flushed = true;
    return EncodeResult{end.value() - start.value(), round_trip, std::move(receipt)};
}

Result<EncodeResult> Codec::encode(const RasterSource& source, const Output& output,
                                   const EncodeOptions& options, std::stop_token stop) const {
    if (!output.sink) {
        return Status::error(ErrorCode::invalid_argument, "Output has no byte sink.");
    }
    Result<std::uint64_t> start = output.sink->position();
    if (!start)
        return start.error();
    Result<EncodedArtifactReceipt> encoded = encode_raster_to_sink(source, output, options, stop);
    if (!encoded)
        return encoded.error();
    Result<void> flushed = output.sink->flush();
    if (!flushed)
        return flushed.error();
    Result<std::uint64_t> end = output.sink->position();
    if (!end)
        return end.error();
    if (end.value() < start.value()) {
        return Status::error(ErrorCode::io_error,
                             "The output sink position moved backwards while encoding.");
    }
    if (end.value() == start.value()) {
        return Status::error(ErrorCode::encode_failed, "The encoder finalized an empty artifact.");
    }
    const bool canonical_webp =
        format() == Format::webp && options.lossless &&
        std::all_of(source.descriptor().frames.begin(), source.descriptor().frames.end(),
                    [](const RasterFrameDescriptor& frame) {
                        return frame.layout.planes.size() == 1 &&
                               frame.layout.planes.front().format == kRgba8;
                    });
    const PixelRoundTrip round_trip = format() == Format::png || format() == Format::tiff ||
                                              canonical_webp ||
                                              (format() == Format::jxl && options.lossless)
                                          ? PixelRoundTrip::exact
                                          : PixelRoundTrip::codec_artifact;
    EncodedArtifactReceipt receipt = std::move(encoded).value();
    Result<void> receipt_status = validate_receipt_shape(receipt, format());
    if (!receipt_status)
        return receipt_status.error();
    receipt.encoder_finalized_and_sink_flushed = true;
    return EncodeResult{end.value() - start.value(), round_trip, std::move(receipt)};
}

Result<EncodedArtifactReceipt> Codec::encode_raster_to_sink(const RasterSource& source,
                                                            const Output& output,
                                                            const EncodeOptions& options,
                                                            std::stop_token stop) const {
    Result<Document> document = has_access(source.access(), RasterAccess::mapped_planes)
                                    ? map_raster(source, stop)
                                    : materialize_raster(source, stop);
    if (!document)
        return document.error();
    return encode_to_sink(document.value(), output, options, stop);
}

RasterEncodeRoute Codec::raster_encode_route(const DocumentDescriptor&,
                                             const EncodeOptions&) const noexcept {
    return RasterEncodeRoute::materialized;
}

Result<void> Codec::decode_to_sink(const Input& input, PixelSink& sink,
                                   const DecodeOptions& options, std::stop_token stop) const {
    Result<Document> decoded = decode(input, options, stop);
    if (!decoded) {
        return decoded.error();
    }
    Document& document = decoded.value();
    DocumentInfo info;
    info.format = document.format;
    info.canvas_width = document.canvas_width;
    info.canvas_height = document.canvas_height;
    info.loop_count = document.loop_count;
    info.metadata = document.metadata;
    info.color = document.color;
    info.is_vector = document.vector.has_value();
    for (const Frame& frame : document.frames) {
        info.frames.push_back({frame.image.width(), frame.image.height(), frame.x, frame.y,
                               frame.duration, frame.image.format(),
                               frame.image.format().alpha != AlphaMode::none, frame.cursor_hotspot,
                               frame.color, frame.metadata, frame.blend, frame.disposal});
    }
    Result<void> status = sink.begin(info);
    if (!status) {
        return status;
    }
    for (std::uint32_t index = 0; index < document.frames.size(); ++index) {
        if (stop.stop_requested()) {
            return cancelled_status();
        }
        const Frame& frame = document.frames[index];
        status = sink.begin_frame(index, info.frames[index]);
        if (!status) {
            return status;
        }
        status = sink.write_rows(0, frame.image.height(), frame.image.row_stride(),
                                 frame.image.pixels());
        if (!status) {
            return status;
        }
        status = sink.end_frame(index);
        if (!status) {
            return status;
        }
    }
    return sink.end();
}

CodecRegistry::CodecRegistry() {
    codecs_.push_back(std::make_shared<BmpCodec>());
#if defined(SNOW_IMAGE_HAS_GIF)
    codecs_.push_back(std::make_shared<GifCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_ICON)
    codecs_.push_back(std::make_shared<IconCodec>(false));
    codecs_.push_back(std::make_shared<IconCodec>(true));
#endif
#if defined(SNOW_IMAGE_HAS_XBITMAP)
    codecs_.push_back(std::make_shared<XbmCodec>());
    codecs_.push_back(std::make_shared<XpmCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_SVG)
    codecs_.push_back(std::make_shared<SvgCodec>(false));
    codecs_.push_back(std::make_shared<SvgCodec>(true));
#endif
#if defined(SNOW_IMAGE_HAS_HEIF)
    codecs_.push_back(std::make_shared<HeifCodec>(Format::heif));
    codecs_.push_back(std::make_shared<HeifCodec>(Format::avif));
#endif
#if defined(SNOW_IMAGE_HAS_JXL)
    codecs_.push_back(std::make_shared<JxlCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_EXR)
    codecs_.push_back(std::make_shared<ExrCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_JPEG)
    codecs_.push_back(std::make_shared<JpegCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_PNG)
    codecs_.push_back(std::make_shared<PngCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_WEBP)
    codecs_.push_back(std::make_shared<WebpCodec>());
#endif
#if defined(SNOW_IMAGE_HAS_TIFF)
    codecs_.push_back(std::make_shared<TiffCodec>());
#endif
    codecs_.push_back(std::make_shared<PnmCodec>(Format::pbm));
    codecs_.push_back(std::make_shared<PnmCodec>(Format::pgm));
    codecs_.push_back(std::make_shared<PnmCodec>(Format::ppm));
    formats_.reserve(codecs_.size());
    for (const auto& codec : codecs_) {
        formats_.push_back({codec->format(), codec->capabilities(), codec->name(),
                            format_extensions(codec->format())});
        if (has_capability(codec->capabilities(), CodecCapability::encode)) {
            encoders_.push_back(describe_encoder(codec->format()));
        }
    }
}

Result<std::shared_ptr<const Codec>> CodecRegistry::detect(const Input& input,
                                                           std::stop_token stop) const {
    if (!input.source) {
        return Status::error(ErrorCode::invalid_argument, "Input has no byte source.");
    }
    if (stop.stop_requested()) {
        return cancelled_status();
    }
    Result<std::uint64_t> input_size = input.source->size();
    if (!input_size) {
        return input_size.error();
    }
    constexpr std::size_t kProbeBytes = 64U * 1024U;
    const std::size_t header_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(input_size.value(), kProbeBytes));
    std::vector<std::byte> header;
    try {
        header.resize(header_size);
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate the format probe buffer.");
    }
    if (!header.empty()) {
        Result<std::size_t> read = input.source->read_at(0, header);
        if (!read) {
            return read.error();
        }
        header.resize(read.value());
    }

    int best_score = 0;
    std::shared_ptr<const Codec> best;
    for (const auto& codec : codecs_) {
        const int score = codec->probe(header, input.name_hint);
        if (score > best_score) {
            best_score = score;
            best = codec;
        }
    }
    if (!best) {
        return Status::error(ErrorCode::unsupported_format,
                             "No registered codec recognized the input.");
    }
    return best;
}

std::shared_ptr<const Codec> CodecRegistry::encoder(Format format) const noexcept {
    for (const auto& codec : codecs_) {
        if (codec->format() == format &&
            has_capability(codec->capabilities(), CodecCapability::encode)) {
            return codec;
        }
    }
    return {};
}

const EncoderInfo* CodecRegistry::encoder_info(Format format) const noexcept {
    const auto found =
        std::find_if(encoders_.cbegin(), encoders_.cend(),
                     [format](const EncoderInfo& info) { return info.format == format; });
    return found == encoders_.cend() ? nullptr : &*found;
}

Status cancelled_status() {
    return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
}

Result<void> validate_dimensions(std::uint32_t width, std::uint32_t height,
                                 const DecodeLimits& limits) {
    if (width == 0 || height == 0) {
        return Status::error(ErrorCode::corrupt_data, "Image dimensions are zero.");
    }
    if (width > limits.maximum_width || height > limits.maximum_height ||
        static_cast<std::uint64_t>(width) * height > limits.maximum_pixels) {
        return Status::error(ErrorCode::limit_exceeded,
                             "Image dimensions exceed the configured decode limits.");
    }
    return {};
}

} // namespace snow::image::internal
