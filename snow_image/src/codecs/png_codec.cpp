#include "codecs/png_codec.h"
#include "exif_orientation.h"

#include "snow/image/processing.h"

#include <png.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <memory>
#include <vector>

namespace snow::image::internal {
namespace {

constexpr std::array<std::byte, 8> kPngSignature{std::byte{0x89}, std::byte{'P'},  std::byte{'N'},
                                                 std::byte{'G'},  std::byte{0x0D}, std::byte{0x0A},
                                                 std::byte{0x1A}, std::byte{0x0A}};

Status png_error(const png_image& image, ErrorCode code) {
    const std::string detail =
        image.message[0] == '\0' ? "libpng operation failed." : image.message;
    return Status::error(code, detail, "libpng");
}

Result<std::pair<png_uint_32, int>> output_png_format(const ImageView& view) {
    if (view.format.sample_type != SampleType::unsigned_integer ||
        (view.format.bits_per_channel != 8 && view.format.bits_per_channel != 16)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "PNG encoding requires packed 8-bit or 16-bit unsigned pixels.",
                             "libpng");
    }
    switch (view.format.channels) {
    case ChannelLayout::gray:
        return std::pair{static_cast<png_uint_32>(PNG_FORMAT_GRAY), 1};
    case ChannelLayout::gray_alpha:
        return std::pair{static_cast<png_uint_32>(PNG_FORMAT_GA), 2};
    case ChannelLayout::rgb:
        return std::pair{static_cast<png_uint_32>(PNG_FORMAT_RGB), 3};
    case ChannelLayout::rgba:
        return std::pair{static_cast<png_uint_32>(PNG_FORMAT_RGBA), 4};
    case ChannelLayout::bgr:
        return std::pair{static_cast<png_uint_32>(PNG_FORMAT_BGR), 3};
    case ChannelLayout::bgra:
        return std::pair{static_cast<png_uint_32>(PNG_FORMAT_BGRA), 4};
    case ChannelLayout::cmyk:
    case ChannelLayout::indexed:
        return Status::error(ErrorCode::unsupported_feature,
                             "PNG encoding does not accept this channel layout.", "libpng");
    }
    return Status::error(ErrorCode::unsupported_feature, "Unsupported PNG pixel layout.", "libpng");
}

struct PngReadState final {
    const ByteSource* source = nullptr;
    std::uint64_t source_size = 0;
    std::uint64_t offset = 0;
    std::stop_token stop;
    Status failure;
    DocumentInfo document;
    std::vector<std::byte> pixels;
    std::vector<std::byte> row;
    std::vector<png_bytep> rows;
};

void read_png_bytes(png_structp png, png_bytep destination, png_size_t count) {
    auto* state = static_cast<PngReadState*>(png_get_io_ptr(png));
    if (!state || !state->source) {
        png_error(png, "PNG reader state is unavailable.");
        return;
    }
    if (state->stop.stop_requested()) {
        state->failure = cancelled_status();
        png_error(png, "PNG decode cancelled.");
        return;
    }
    if (count > state->source_size - state->offset) {
        state->failure =
            Status::error(ErrorCode::truncated_data, "PNG input ended unexpectedly.", "libpng");
        png_error(png, "PNG input ended unexpectedly.");
        return;
    }
    std::size_t completed = 0;
    while (completed < count) {
        Result<std::size_t> read = state->source->read_at(
            state->offset + completed,
            std::span(reinterpret_cast<std::byte*>(destination + completed), count - completed));
        if (!read) {
            state->failure = read.error();
            png_error(png, "PNG input read failed.");
            return;
        }
        if (read.value() == 0) {
            state->failure =
                Status::error(ErrorCode::truncated_data, "PNG input ended unexpectedly.", "libpng");
            png_error(png, "PNG input ended unexpectedly.");
            return;
        }
        completed += read.value();
    }
    state->offset += count;
}

Result<void> configure_png_output(png_structp png, png_infop info, const PixelFormat& target) {
    if (target.sample_type != SampleType::unsigned_integer ||
        (target.bits_per_channel != 8 && target.bits_per_channel != 16) ||
        (target.channels != ChannelLayout::gray && target.channels != ChannelLayout::gray_alpha &&
         target.channels != ChannelLayout::rgb && target.channels != ChannelLayout::rgba &&
         target.channels != ChannelLayout::bgr && target.channels != ChannelLayout::bgra)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "PNG decode requested an unsupported output layout.", "libpng");
    }
    const int color_type = png_get_color_type(png, info);
    const int bit_depth = png_get_bit_depth(png, info);
    const bool source_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0;
    const bool transparency = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (bit_depth == 16 && target.bits_per_channel == 8)
        png_set_strip_16(png);
    if (bit_depth < 16 && target.bits_per_channel == 16)
        png_set_expand_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (transparency)
        png_set_tRNS_to_alpha(png);
    const bool target_rgb =
        target.channels == ChannelLayout::rgb || target.channels == ChannelLayout::rgba ||
        target.channels == ChannelLayout::bgr || target.channels == ChannelLayout::bgra;
    const bool source_gray =
        color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA;
    if (target_rgb && source_gray) {
        png_set_gray_to_rgb(png);
    }
    if (!target_rgb && !source_gray && color_type != PNG_COLOR_TYPE_PALETTE) {
        png_set_rgb_to_gray_fixed(png, 1, -1, -1);
    }
    const bool target_alpha = target.channels == ChannelLayout::gray_alpha ||
                              target.channels == ChannelLayout::rgba ||
                              target.channels == ChannelLayout::bgra;
    if (target_alpha && !source_alpha && !transparency)
        png_set_add_alpha(png, target.bits_per_channel == 16 ? 0xFFFFU : 0xFFU, PNG_FILLER_AFTER);
    if (!target_alpha && (source_alpha || transparency))
        png_set_strip_alpha(png);
    if (target.channels == ChannelLayout::bgr || target.channels == ChannelLayout::bgra)
        png_set_bgr(png);
    if (target.bits_per_channel == 16 && target.little_endian &&
        std::endian::native == std::endian::little)
        png_set_swap(png);
    return {};
}

void read_png_metadata(png_structp png, png_infop info, Metadata* metadata, ColorEncoding* color) {
    png_charp profile_name = nullptr;
    int compression = 0;
    png_bytep profile = nullptr;
    png_uint_32 profile_size = 0;
    if (png_get_iCCP(png, info, &profile_name, &compression, &profile, &profile_size) != 0 &&
        profile && profile_size != 0) {
        color->icc_profile.assign(reinterpret_cast<const std::byte*>(profile),
                                  reinterpret_cast<const std::byte*>(profile + profile_size));
    }
    int srgb_intent = 0;
    if (png_get_sRGB(png, info, &srgb_intent) != 0) {
        color->primaries = ColorPrimaries::srgb;
        color->transfer = TransferFunction::srgb;
    }
    png_uint_32 pixels_x = 0;
    png_uint_32 pixels_y = 0;
    int unit = PNG_RESOLUTION_UNKNOWN;
    if (png_get_pHYs(png, info, &pixels_x, &pixels_y, &unit) != 0 && unit == PNG_RESOLUTION_METER) {
        metadata->horizontal_dpi = static_cast<double>(pixels_x) * 0.0254;
        metadata->vertical_dpi = static_cast<double>(pixels_y) * 0.0254;
    }
#if defined(PNG_eXIf_SUPPORTED)
    png_bytep exif = nullptr;
    png_uint_32 exif_size = 0;
    if (png_get_eXIf_1(png, info, &exif_size, &exif) != 0 && exif && exif_size != 0) {
        metadata->exif.assign(reinterpret_cast<const std::byte*>(exif),
                              reinterpret_cast<const std::byte*>(exif + exif_size));
    }
#endif
    png_textp text = nullptr;
    int text_count = 0;
    if (png_get_text(png, info, &text, &text_count) > 0 && text) {
        for (int index = 0; index < text_count; ++index) {
            const std::string_view key = text[index].key ? text[index].key : "";
            const char* value = text[index].text ? text[index].text : "";
            const std::size_t size = std::strlen(value);
            if (key == "XML:com.adobe.xmp") {
                metadata->xmp.assign(reinterpret_cast<const std::byte*>(value),
                                     reinterpret_cast<const std::byte*>(value + size));
            } else if (key == "Comment") {
                metadata->comment.assign(value, size);
            }
        }
    }
    if (const auto orientation = parse_exif_orientation(metadata->exif))
        metadata->orientation = *orientation;
}

void apply_png_metadata_policy(DocumentInfo* info, const DecodeOptions& options) {
    const Orientation orientation = info->metadata.orientation;
    if (options.orientation == OrientationPolicy::apply) {
        if (static_cast<std::uint8_t>(orientation) >= 5) {
            std::swap(info->canvas_width, info->canvas_height);
            for (FrameInfo& frame : info->frames)
                std::swap(frame.width, frame.height);
        }
        if (orientation != Orientation::identity) {
            for (FrameInfo& frame : info->frames) {
                frame.native_format = kRgba8;
                frame.has_alpha = true;
            }
        }
        info->metadata.orientation = Orientation::identity;
        (void)rewrite_exif_orientation(&info->metadata.exif, Orientation::identity);
    }
    if (!options.preserve_metadata) {
        info->metadata = {};
        info->color.icc_profile.clear();
    }
    for (FrameInfo& frame : info->frames) {
        frame.metadata = info->metadata;
        frame.color = info->color;
    }
}

Result<void> write_png_metadata(png_structp png, png_infop info, const Document& document,
                                const EncodeOptions& options, std::vector<png_text>* text_chunks,
                                std::vector<std::string>* text_values) {
    if (!options.preserve_metadata)
        return {};
    const Metadata& metadata = document.metadata;
    const ColorEncoding& color = document.color;
    if (!color.icc_profile.empty()) {
        if (color.icc_profile.size() > std::numeric_limits<png_uint_32>::max()) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "PNG ICC profile exceeds libpng limits.", "libpng");
        }
        png_set_iCCP(png, info, "ICC profile", PNG_COMPRESSION_TYPE_BASE,
                     reinterpret_cast<png_const_bytep>(color.icc_profile.data()),
                     static_cast<png_uint_32>(color.icc_profile.size()));
    } else if (color.primaries == ColorPrimaries::srgb &&
               color.transfer == TransferFunction::srgb) {
        png_set_sRGB_gAMA_and_cHRM(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    }
    if (metadata.horizontal_dpi && metadata.vertical_dpi &&
        std::isfinite(*metadata.horizontal_dpi) && std::isfinite(*metadata.vertical_dpi) &&
        *metadata.horizontal_dpi > 0.0 && *metadata.vertical_dpi > 0.0) {
        const double pixels_x = *metadata.horizontal_dpi / 0.0254;
        const double pixels_y = *metadata.vertical_dpi / 0.0254;
        if (pixels_x <= std::numeric_limits<png_uint_32>::max() &&
            pixels_y <= std::numeric_limits<png_uint_32>::max()) {
            png_set_pHYs(png, info, static_cast<png_uint_32>(std::llround(pixels_x)),
                         static_cast<png_uint_32>(std::llround(pixels_y)), PNG_RESOLUTION_METER);
        }
    }
#if defined(PNG_eXIf_SUPPORTED)
    if (!metadata.exif.empty()) {
        if (metadata.exif.size() > std::numeric_limits<png_uint_32>::max()) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "PNG EXIF payload exceeds libpng limits.", "libpng");
        }
        png_set_eXIf_1(png, info, static_cast<png_uint_32>(metadata.exif.size()),
                       reinterpret_cast<png_bytep>(const_cast<std::byte*>(metadata.exif.data())));
    }
#endif
    text_chunks->clear();
    text_values->clear();
    text_chunks->reserve((metadata.comment.empty() ? 0U : 1U) + (metadata.xmp.empty() ? 0U : 1U));
    text_values->reserve(text_chunks->capacity());
    if (!metadata.comment.empty()) {
        text_values->push_back(metadata.comment);
        png_text text{};
        text.compression = PNG_TEXT_COMPRESSION_zTXt;
        text.key = const_cast<png_charp>("Comment");
        text.text = text_values->back().data();
        text.text_length = text_values->back().size();
        text_chunks->push_back(text);
    }
    if (!metadata.xmp.empty()) {
        text_values->emplace_back(reinterpret_cast<const char*>(metadata.xmp.data()),
                                  metadata.xmp.size());
        png_text text{};
        text.compression = PNG_ITXT_COMPRESSION_zTXt;
        text.key = const_cast<png_charp>("XML:com.adobe.xmp");
        text.text = text_values->back().data();
        text.itxt_length = text_values->back().size();
        text.lang = const_cast<png_charp>("");
        text.lang_key = const_cast<png_charp>("");
        text_chunks->push_back(text);
    }
    if (!text_chunks->empty())
        png_set_text(png, info, text_chunks->data(), static_cast<int>(text_chunks->size()));
    return {};
}

struct PngWriteState final {
    ByteSink* sink = nullptr;
    std::stop_token stop;
    Status failure;
};

class OwningPngSink final : public PixelSink {
  public:
    explicit OwningPngSink(std::uint64_t maximum_bytes) : maximum_bytes_(maximum_bytes) {}

    Result<void> begin(const DocumentInfo& info) override {
        document_.format = info.format;
        document_.canvas_width = info.canvas_width;
        document_.canvas_height = info.canvas_height;
        document_.loop_count = info.loop_count;
        document_.metadata = info.metadata;
        document_.color = info.color;
        document_.frames.reserve(info.frames.size());
        return {};
    }

    Result<void> begin_frame(std::uint32_t frame_index, const FrameInfo& info) override {
        if (frame_index != document_.frames.size() || active_.has_value())
            return Status::error(ErrorCode::corrupt_data,
                                 "PNG decoder emitted an invalid frame order.", "libpng");
        Result<std::size_t> bytes_per_pixel = info.native_format.bytes_per_pixel();
        if (!bytes_per_pixel)
            return bytes_per_pixel.error();
        const std::uint64_t required =
            static_cast<std::uint64_t>(info.width) * info.height * bytes_per_pixel.value();
        if (required > maximum_bytes_ || required > maximum_bytes_ - used_bytes_)
            return Status::error(ErrorCode::limit_exceeded,
                                 "PNG output exceeds the owning decode limit; use decode_into().",
                                 "libpng");
        Result<MutableImage> allocated =
            MutableImage::allocate(info.width, info.height, info.native_format);
        if (!allocated)
            return allocated.error();
        active_ = std::move(allocated).value();
        active_info_ = info;
        next_row_ = 0;
        used_bytes_ += required;
        return {};
    }

    std::span<std::byte> frame_storage(std::uint32_t frame_index, std::size_t row_stride,
                                       std::size_t byte_size) override {
        if (!active_ || frame_index != document_.frames.size() ||
            row_stride != active_->row_stride() || byte_size > active_->pixels().size())
            return {};
        return active_->pixels().first(byte_size);
    }

    Result<void> write_rows(std::uint32_t first_row, std::uint32_t row_count,
                            std::size_t source_stride, std::span<const std::byte> pixels) override {
        if (!active_ || first_row != next_row_)
            return Status::error(ErrorCode::corrupt_data,
                                 "PNG decoder emitted non-sequential rows.", "libpng");
        Result<std::size_t> bytes_per_pixel = active_->format().bytes_per_pixel();
        if (!bytes_per_pixel)
            return bytes_per_pixel.error();
        const std::size_t row_bytes =
            static_cast<std::size_t>(active_->width()) * bytes_per_pixel.value();
        if (source_stride < row_bytes || row_count > active_->height() - first_row ||
            (row_count > 1 && source_stride > (pixels.size() - std::min(pixels.size(), row_bytes)) /
                                                  (row_count - 1U)))
            return Status::error(ErrorCode::corrupt_data, "PNG decoder row storage is invalid.",
                                 "libpng");
        for (std::uint32_t row = 0; row < row_count; ++row) {
            std::memcpy(active_->pixels().data() +
                            static_cast<std::size_t>(first_row + row) * active_->row_stride(),
                        pixels.data() + static_cast<std::size_t>(row) * source_stride, row_bytes);
        }
        next_row_ += row_count;
        return {};
    }

    Result<void> end_frame(std::uint32_t frame_index) override {
        if (!active_ || frame_index != document_.frames.size() || next_row_ != active_->height())
            return Status::error(ErrorCode::truncated_data,
                                 "PNG decoder ended an incomplete frame.", "libpng");
        Frame frame;
        frame.image = std::move(*active_).freeze();
        frame.x = active_info_.x;
        frame.y = active_info_.y;
        frame.duration = active_info_.duration;
        frame.blend = active_info_.blend;
        frame.disposal = active_info_.disposal;
        frame.metadata = active_info_.metadata;
        frame.color = active_info_.color;
        frame.cursor_hotspot = active_info_.cursor_hotspot;
        document_.frames.push_back(std::move(frame));
        active_.reset();
        return {};
    }

    Result<void> end() override {
        if (active_ || document_.frames.empty())
            return Status::error(ErrorCode::truncated_data,
                                 "PNG decoder ended an incomplete document.", "libpng");
        ended_ = true;
        return {};
    }

    Result<Document> take() {
        if (!ended_)
            return Status::error(ErrorCode::internal_error, "PNG owning decode did not finish.",
                                 "libpng");
        return std::move(document_);
    }

  private:
    std::uint64_t maximum_bytes_ = 0;
    std::uint64_t used_bytes_ = 0;
    Document document_;
    FrameInfo active_info_;
    std::optional<MutableImage> active_;
    std::uint32_t next_row_ = 0;
    bool ended_ = false;
};

void write_png_bytes(png_structp png, png_bytep data, png_size_t size) {
    auto* state = static_cast<PngWriteState*>(png_get_io_ptr(png));
    if (!state || !state->sink) {
        png_error(png, "PNG writer state is unavailable.");
        return;
    }
    if (!state->failure.ok())
        return;
    if (state->stop.stop_requested()) {
        state->failure = cancelled_status();
        return;
    }
    Result<void> written =
        state->sink->write(std::span(reinterpret_cast<const std::byte*>(data), size));
    if (!written) {
        state->failure = written.error();
    }
}

void flush_png_bytes(png_structp png) {
    auto* state = static_cast<PngWriteState*>(png_get_io_ptr(png));
    if (!state || !state->sink) {
        png_error(png, "PNG writer state is unavailable.");
        return;
    }
    if (!state->failure.ok())
        return;
    if (state->stop.stop_requested()) {
        state->failure = cancelled_status();
        return;
    }
    // The common codec wrapper owns the final sink flush.  libpng invokes this
    // callback during finalization, so it only needs to observe cancellation
    // here and must not make a second sink-level commit.
}

struct IndexedPng final {
    std::vector<png_color> palette;
    std::vector<png_byte> alpha;
    std::unique_ptr<png_byte[]> indices;
    std::size_t index_count = 0;
    AlphaContent alpha_content = AlphaContent::opaque;
};

struct PaletteSlot final {
    std::uint32_t key = 0;
    png_byte index = 0;
    bool occupied = false;
};

constexpr std::size_t kPaletteSlotCount = 512;

std::size_t palette_hash(std::uint32_t key) noexcept {
    key ^= key >> 16U;
    key *= 0x7FEB352DU;
    key ^= key >> 15U;
    key *= 0x846CA68BU;
    key ^= key >> 16U;
    return static_cast<std::size_t>(key) & (kPaletteSlotCount - 1U);
}

Result<std::optional<IndexedPng>> make_indexed_png(const ImageView& view, std::stop_token stop) {
    if (view.format.sample_type != SampleType::unsigned_integer ||
        view.format.bits_per_channel != 8 ||
        (view.format.channels != ChannelLayout::rgb &&
         view.format.channels != ChannelLayout::rgba &&
         view.format.channels != ChannelLayout::bgr &&
         view.format.channels != ChannelLayout::bgra)) {
        return std::optional<IndexedPng>{};
    }
    if (stop.stop_requested())
        return cancelled_status();
    const std::size_t channels = view.format.channel_count();
    const bool bgr =
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    if (view.width != 0 && view.height > std::numeric_limits<std::size_t>::max() / view.width) {
        return Status::error(ErrorCode::limit_exceeded, "PNG palette index storage size overflows.",
                             "libpng");
    }
    const std::size_t pixel_count = static_cast<std::size_t>(view.width) * view.height;
    IndexedPng indexed;
    indexed.index_count = pixel_count;
    indexed.indices.reset(new png_byte[pixel_count]);
    std::array<PaletteSlot, kPaletteSlotCount> lookup{};
    bool has_alpha = false;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        const std::byte* row = view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride;
        for (std::uint32_t x = 0; x < view.width; ++x) {
            if ((x & 4095U) == 0U && stop.stop_requested())
                return cancelled_status();
            const std::size_t offset = static_cast<std::size_t>(x) * channels;
            const std::uint8_t red = std::to_integer<std::uint8_t>(row[offset + (bgr ? 2U : 0U)]);
            const std::uint8_t green = std::to_integer<std::uint8_t>(row[offset + 1U]);
            const std::uint8_t blue = std::to_integer<std::uint8_t>(row[offset + (bgr ? 0U : 2U)]);
            const std::uint8_t alpha =
                channels == 4U ? std::to_integer<std::uint8_t>(row[offset + 3U]) : 255U;
            const std::uint32_t key = static_cast<std::uint32_t>(red) << 24U |
                                      static_cast<std::uint32_t>(green) << 16U |
                                      static_cast<std::uint32_t>(blue) << 8U | alpha;
            std::size_t slot = palette_hash(key);
            std::size_t probes = 0;
            while (probes < kPaletteSlotCount && lookup[slot].occupied && lookup[slot].key != key) {
                slot = (slot + 1U) & (kPaletteSlotCount - 1U);
                ++probes;
            }
            if (probes == kPaletteSlotCount) {
                return Status::error(ErrorCode::internal_error,
                                     "PNG palette lookup table overflowed.", "libpng");
            }
            if (!lookup[slot].occupied) {
                if (indexed.palette.size() >= 256U)
                    return std::optional<IndexedPng>{};
                const auto index = static_cast<png_byte>(indexed.palette.size());
                lookup[slot] = PaletteSlot{key, index, true};
                indexed.palette.push_back({red, green, blue});
                indexed.alpha.push_back(alpha);
                has_alpha = has_alpha || alpha != 255U;
            }
            indexed.indices[static_cast<std::size_t>(y) * view.width + x] = lookup[slot].index;
        }
    }
    if (!has_alpha)
        indexed.alpha.clear();
    indexed.alpha_content = has_alpha ? AlphaContent::non_opaque : AlphaContent::opaque;
    return std::optional<IndexedPng>(std::move(indexed));
}

} // namespace

CodecCapability PngCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::streaming_decode | CodecCapability::metadata_decode;
}

int PngCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= kPngSignature.size() &&
        std::equal(kPngSignature.begin(), kPngSignature.end(), header.begin())) {
        return 100;
    }
    return format_from_extension(name_hint) == Format::png ? 10 : 0;
}

Result<DocumentInfo> PngCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::uint64_t> size = input.source->size();
    if (!size)
        return size.error();
    if (size.value() > options.limits.maximum_input_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "PNG input exceeds the configured byte limit.", "libpng");
    }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
        return Status::error(ErrorCode::out_of_memory, "Could not allocate the libpng inspector.",
                             "libpng");
    png_infop png_info = png_create_info_struct(png);
    if (!png_info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate PNG inspection metadata.", "libpng");
    }
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
    auto state = std::make_unique<PngReadState>();
    state->source = input.source.get();
    state->source_size = size.value();
    state->stop = stop;
    if (setjmp(png_jmpbuf(png)) != 0) {
        const Status failure =
            state->failure.ok()
                ? Status::error(ErrorCode::corrupt_data, "libpng could not inspect the PNG stream.",
                                "libpng")
                : state->failure;
        png_destroy_read_struct(&png, &png_info, nullptr);
        return failure;
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    png_set_read_fn(png, state.get(), read_png_bytes);
    png_set_user_limits(png, options.limits.maximum_width, options.limits.maximum_height);
    png_set_chunk_malloc_max(png, options.limits.maximum_metadata_bytes);
    png_read_info(png, png_info);
    const std::uint32_t width = png_get_image_width(png, png_info);
    const std::uint32_t height = png_get_image_height(png, png_info);
    const Result<void> dimensions = validate_dimensions(width, height, options.limits);
    if (!dimensions) {
        png_destroy_read_struct(&png, &png_info, nullptr);
        return dimensions.error();
    }
    const int bit_depth = png_get_bit_depth(png, png_info);
    const int color_type = png_get_color_type(png, png_info);
    const bool transparency = png_get_valid(png, png_info, PNG_INFO_tRNS) != 0;
    DocumentInfo info;
    info.format = Format::png;
    info.canvas_width = width;
    info.canvas_height = height;
    read_png_metadata(png, png_info, &info.metadata, &info.color);
    info.frames.push_back({width, height, 0, 0, std::chrono::nanoseconds{0},
                           bit_depth == 16 ? kRgba16 : kRgba8,
                           color_type == PNG_COLOR_TYPE_GRAY_ALPHA ||
                               color_type == PNG_COLOR_TYPE_RGBA || transparency,
                           std::nullopt, info.color, info.metadata});
    apply_png_metadata_policy(&info, options);
    png_destroy_read_struct(&png, &png_info, nullptr);
    return info;
}

Result<Document> PngCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    DecodeOptions normalized = options;
    // The legacy owning API retains its RGBA8 contract. RasterWriter callers use
    // decode_into() to retain the source's 16-bit samples.
    normalized.output_format = kRgba8;
    normalized.orientation = OrientationPolicy::preserve;
    normalized.preserve_metadata = true;
    OwningPngSink sink(options.limits.maximum_owned_output_bytes);
    Result<void> status = decode_to_sink(input, sink, normalized, stop);
    if (!status)
        return status.error();
    Result<Document> taken = sink.take();
    if (!taken)
        return taken.error();
    Document document = std::move(taken).value();
    const Orientation orientation = document.metadata.orientation;
    if (options.orientation == OrientationPolicy::apply) {
        status = apply_orientation(&document, orientation, stop);
        if (!status)
            return status.error();
    }
    if (!options.preserve_metadata) {
        document.metadata = {};
        document.color.icc_profile.clear();
        for (Frame& frame : document.frames) {
            frame.metadata = {};
            frame.color.icc_profile.clear();
        }
    }
    return document;
}

Result<void> PngCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                      const DecodeOptions& options, std::stop_token stop) const {
    Result<std::uint64_t> source_size = input.source->size();
    if (!source_size)
        return source_size.error();
    if (source_size.value() > options.limits.maximum_input_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "PNG input exceeds the configured byte limit.", "libpng");
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate the libpng decoder.",
                             "libpng");
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return Status::error(ErrorCode::out_of_memory, "Could not allocate PNG metadata.",
                             "libpng");
    }

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
    auto state = std::make_unique<PngReadState>();
    state->source = input.source.get();
    state->source_size = source_size.value();
    state->stop = stop;
    if (setjmp(png_jmpbuf(png)) != 0) {
        const Status failure =
            state->failure.ok() ? Status::error(ErrorCode::decode_failed,
                                                "libpng could not decode the PNG stream.", "libpng")
                                : state->failure;
        png_destroy_read_struct(&png, &info, nullptr);
        return failure;
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    png_set_read_fn(png, state.get(), read_png_bytes);
    png_set_user_limits(png, options.limits.maximum_width, options.limits.maximum_height);
    png_set_chunk_malloc_max(png, options.limits.maximum_metadata_bytes);
    png_read_info(png, info);

    const png_uint_32 width = png_get_image_width(png, info);
    const png_uint_32 height = png_get_image_height(png, info);
    {
        Result<void> dimensions = validate_dimensions(width, height, options.limits);
        if (!dimensions) {
            png_destroy_read_struct(&png, &info, nullptr);
            return dimensions;
        }
    }
    const bool interlaced = png_get_interlace_type(png, info) != PNG_INTERLACE_NONE;
    const PixelFormat output_format =
        options.output_format.value_or(png_get_bit_depth(png, info) == 16 ? kRgba16 : kRgba8);
    read_png_metadata(png, info, &state->document.metadata, &state->document.color);
    if (options.orientation == OrientationPolicy::apply &&
        state->document.metadata.orientation != Orientation::identity) {
        png_destroy_read_struct(&png, &info, nullptr);
        return Codec::decode_to_sink(input, sink, options, stop);
    }
    Result<void> configured = configure_png_output(png, info, output_format);
    if (!configured) {
        png_destroy_read_struct(&png, &info, nullptr);
        return configured;
    }
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const png_size_t row_bytes = png_get_rowbytes(png, info);
    Result<std::size_t> bytes_per_pixel = output_format.bytes_per_pixel();
    if (!bytes_per_pixel) {
        png_destroy_read_struct(&png, &info, nullptr);
        return bytes_per_pixel.error();
    }
    const std::uint64_t expected_row_bytes =
        static_cast<std::uint64_t>(width) * bytes_per_pixel.value();
    if (row_bytes != expected_row_bytes) {
        png_destroy_read_struct(&png, &info, nullptr);
        return Status::error(ErrorCode::decode_failed,
                             "libpng produced an unexpected RGBA row layout.", "libpng");
    }

    state->document.format = Format::png;
    state->document.canvas_width = width;
    state->document.canvas_height = height;
    state->document.frames.push_back({width, height, 0, 0, std::chrono::nanoseconds{0},
                                      output_format, output_format.alpha != AlphaMode::none,
                                      std::nullopt, state->document.color,
                                      state->document.metadata});
    apply_png_metadata_policy(&state->document, options);
    {
        Result<void> status = sink.begin(state->document);
        if (!status) {
            png_destroy_read_struct(&png, &info, nullptr);
            return status;
        }
    }
    {
        Result<void> status = sink.begin_frame(0, state->document.frames.front());
        if (!status) {
            png_destroy_read_struct(&png, &info, nullptr);
            return status;
        }
    }

    if (interlaced) {
        const std::uint64_t image_bytes = expected_row_bytes * height;
        if (image_bytes > options.limits.maximum_working_bytes ||
            image_bytes > std::numeric_limits<std::size_t>::max()) {
            png_destroy_read_struct(&png, &info, nullptr);
            return Status::error(
                ErrorCode::limit_exceeded,
                "Interlaced PNG streaming exceeds the configured working-memory limit.", "libpng");
        }
        state->pixels.resize(static_cast<std::size_t>(image_bytes));
        state->rows.resize(height);
        for (png_uint_32 y = 0; y < height; ++y) {
            state->rows[y] = reinterpret_cast<png_bytep>(state->pixels.data() +
                                                         static_cast<std::size_t>(y) * row_bytes);
        }
        for (int pass = 0; pass < passes; ++pass) {
            if (stop.stop_requested()) {
                png_destroy_read_struct(&png, &info, nullptr);
                return cancelled_status();
            }
            png_read_rows(png, state->rows.data(), nullptr, height);
        }
        {
            Result<void> status = sink.write_rows(0, height, row_bytes, state->pixels);
            if (!status) {
                png_destroy_read_struct(&png, &info, nullptr);
                return status;
            }
        }
    } else {
        state->row.resize(row_bytes);
        for (png_uint_32 y = 0; y < height; ++y) {
            if (stop.stop_requested()) {
                png_destroy_read_struct(&png, &info, nullptr);
                return cancelled_status();
            }
            png_read_row(png, reinterpret_cast<png_bytep>(state->row.data()), nullptr);
            {
                Result<void> status = sink.write_rows(y, 1, row_bytes, state->row);
                if (!status) {
                    png_destroy_read_struct(&png, &info, nullptr);
                    return status;
                }
            }
        }
    }
    png_read_end(png, info);
    png_destroy_read_struct(&png, &info, nullptr);
    Result<void> status = sink.end_frame(0);
    if (!status)
        return status;
    return sink.end();
}

Result<EncodedArtifactReceipt> PngCodec::encode_to_sink(const Document& document,
                                                        const Output& output,
                                                        const EncodeOptions& options,
                                                        std::stop_token stop) const {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "PNG encoding requires one frame.",
                             "libpng");
    }
    const ImageView view = document.frames.front().image.view();
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    Result<std::pair<png_uint_32, int>> mapped = output_png_format(view);
    if (!mapped)
        return mapped.error();
    if (view.width > std::numeric_limits<png_uint_32>::max() ||
        view.height > std::numeric_limits<png_uint_32>::max() ||
        view.row_stride > static_cast<std::size_t>(std::numeric_limits<png_int_32>::max())) {
        return Status::error(ErrorCode::limit_exceeded,
                             "PNG dimensions or stride exceed libpng limits.", "libpng");
    }
    // Palette discovery is cancellable and may allocate an index plane. Run it
    // before allocating libpng state so a pre-cancelled request does no codec
    // setup work and a photographic image can reject after its first 257th color.
    Result<std::optional<IndexedPng>> indexedResult = make_indexed_png(view, stop);
    if (!indexedResult)
        return indexedResult.error();
    std::optional<IndexedPng> indexed = std::move(indexedResult).value();
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        return Status::error(ErrorCode::out_of_memory, "Could not create PNG writer.", "libpng");
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return Status::error(ErrorCode::out_of_memory, "Could not create PNG metadata.", "libpng");
    }
    PngWriteState state;
    state.sink = output.sink.get();
    state.stop = stop;
    std::vector<png_text> text_chunks;
    std::vector<std::string> text_values;
    Result<AlphaContent> alpha = indexed ? Result<AlphaContent>(indexed->alpha_content)
                                 : options.verified_alpha_content
                                     ? Result<AlphaContent>(*options.verified_alpha_content)
                                     : classify_alpha(document.frames.front().image, stop);
    if (!alpha) {
        png_destroy_write_struct(&png, &info);
        return alpha.error();
    }
    if (indexed && options.verified_alpha_content &&
        *options.verified_alpha_content != indexed->alpha_content) {
        png_destroy_write_struct(&png, &info);
        return Status::error(ErrorCode::invalid_argument,
                             "Verified PNG alpha metadata contradicts palette discovery.",
                             "libpng");
    }
    const bool strip_opaque_alpha = !indexed && alpha.value() == AlphaContent::opaque &&
                                    (view.format.channels == ChannelLayout::gray_alpha ||
                                     view.format.channels == ChannelLayout::rgba ||
                                     view.format.channels == ChannelLayout::bgra);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        return state.failure.ok() ? Status::error(ErrorCode::encode_failed,
                                                  "libpng could not encode the image.", "libpng")
                                  : state.failure;
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    png_set_write_fn(png, &state, &write_png_bytes, &flush_png_bytes);
    png_set_compression_level(png, std::clamp(options.compression_level, 0, 9));
    png_set_compression_buffer_size(png, 256U * 1024U);
    int color_type = indexed ? PNG_COLOR_TYPE_PALETTE : PNG_COLOR_TYPE_RGBA;
    if (!indexed)
        switch (view.format.channels) {
        case ChannelLayout::gray:
            color_type = PNG_COLOR_TYPE_GRAY;
            break;
        case ChannelLayout::gray_alpha:
            color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
            break;
        case ChannelLayout::rgb:
        case ChannelLayout::bgr:
            color_type = PNG_COLOR_TYPE_RGB;
            break;
        case ChannelLayout::rgba:
        case ChannelLayout::bgra:
            color_type = PNG_COLOR_TYPE_RGBA;
            break;
        case ChannelLayout::cmyk:
        case ChannelLayout::indexed:
            break;
        }
    if (strip_opaque_alpha) {
        color_type = view.format.channels == ChannelLayout::gray_alpha ? PNG_COLOR_TYPE_GRAY
                                                                       : PNG_COLOR_TYPE_RGB;
    }
    png_set_IHDR(png, info, view.width, view.height, view.format.bits_per_channel, color_type,
                 options.interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (indexed) {
        png_set_PLTE(png, info, indexed->palette.data(), static_cast<int>(indexed->palette.size()));
        if (!indexed->alpha.empty()) {
            png_set_tRNS(png, info, indexed->alpha.data(), static_cast<int>(indexed->alpha.size()),
                         nullptr);
        }
    } else if (view.format.channels == ChannelLayout::bgr ||
               view.format.channels == ChannelLayout::bgra) {
        png_set_bgr(png);
    }
    Result<void> metadata_status =
        write_png_metadata(png, info, document, options, &text_chunks, &text_values);
    if (!metadata_status) {
        png_destroy_write_struct(&png, &info);
        return metadata_status.error();
    }
    png_write_info(png, info);
    if (view.format.bits_per_channel == 16 && view.format.little_endian &&
        std::endian::native == std::endian::little)
        png_set_swap(png);
    if (strip_opaque_alpha)
        png_set_filler(png, 0, PNG_FILLER_AFTER);
    const int passes = options.interlaced ? png_set_interlace_handling(png) : 1;
    for (int pass = 0; pass < passes; ++pass) {
        for (std::uint32_t y = 0; y < view.height; ++y) {
            if (stop.stop_requested()) {
                state.failure = cancelled_status();
                png_error(png, "PNG encode cancelled.");
            }
            png_bytep row =
                indexed ? indexed->indices.get() + static_cast<std::size_t>(y) * view.width
                        : reinterpret_cast<png_bytep>(const_cast<std::byte*>(
                              view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride));
            png_write_row(png, row);
        }
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    if (!state.failure.ok())
        return state.failure;
    return receipt_for_document(document, format());
}

Result<EncodedArtifactReceipt> PngCodec::encode_raster_to_sink(const RasterSource& source,
                                                               const Output& output,
                                                               const EncodeOptions& options,
                                                               std::stop_token stop) const {
    const DocumentDescriptor& descriptor = source.descriptor();
    if (descriptor.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "PNG encoding requires one frame.",
                             "libpng");
    }
    const RasterFrameDescriptor& source_frame = descriptor.frames.front();
    if (descriptor.frames.size() != 1 || source_frame.layout.planes.size() != 1 ||
        source_frame.layout.planes.front().semantic != PlaneSemantic::packed) {
        return Codec::encode_raster_to_sink(source, output, options, stop);
    }
    if (!has_access(source.access(), RasterAccess::mapped_planes)) {
        if (options.interlaced && !has_access(source.access(), RasterAccess::random_rows)) {
            return Codec::encode_raster_to_sink(source, output, options, stop);
        }
        Result<void> descriptor_status = descriptor.validate();
        if (!descriptor_status)
            return descriptor_status.error();
        const PlaneDescriptor& plane = source_frame.layout.planes.front();
        const ImageView layout{source_frame.width, source_frame.height, plane.format, 0, {}};
        Result<std::pair<png_uint_32, int>> mapped_format = output_png_format(layout);
        if (!mapped_format)
            return Codec::encode_raster_to_sink(source, output, options, stop);
        Result<std::size_t> row_bytes_result = plane.row_bytes();
        if (!row_bytes_result)
            return row_bytes_result.error();
        const std::size_t row_bytes = row_bytes_result.value();
        if (source_frame.width > std::numeric_limits<png_uint_32>::max() ||
            source_frame.height > std::numeric_limits<png_uint_32>::max() ||
            row_bytes > static_cast<std::size_t>(std::numeric_limits<png_int_32>::max())) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "PNG dimensions or stride exceed libpng limits.", "libpng");
        }

        std::vector<std::byte> row(row_bytes);
        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
        if (!png) {
            return Status::error(ErrorCode::out_of_memory, "Could not create PNG writer.",
                                 "libpng");
        }
        png_infop info = png_create_info_struct(png);
        if (!info) {
            png_destroy_write_struct(&png, nullptr);
            return Status::error(ErrorCode::out_of_memory, "Could not create PNG metadata.",
                                 "libpng");
        }
        PngWriteState state;
        state.sink = output.sink.get();
        state.stop = stop;
        std::vector<png_text> text_chunks;
        std::vector<std::string> text_values;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
        if (setjmp(png_jmpbuf(png)) != 0) {
            png_destroy_write_struct(&png, &info);
            return state.failure.ok()
                       ? Status::error(ErrorCode::encode_failed,
                                       "libpng could not encode the image.", "libpng")
                       : state.failure;
        }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        png_set_write_fn(png, &state, &write_png_bytes, &flush_png_bytes);
        png_set_compression_level(png, std::clamp(options.compression_level, 0, 9));
        png_set_compression_buffer_size(png, 256U * 1024U);
        int color_type = PNG_COLOR_TYPE_RGBA;
        switch (plane.format.channels) {
        case ChannelLayout::gray:
            color_type = PNG_COLOR_TYPE_GRAY;
            break;
        case ChannelLayout::gray_alpha:
            color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
            break;
        case ChannelLayout::rgb:
        case ChannelLayout::bgr:
            color_type = PNG_COLOR_TYPE_RGB;
            break;
        case ChannelLayout::rgba:
        case ChannelLayout::bgra:
            color_type = PNG_COLOR_TYPE_RGBA;
            break;
        case ChannelLayout::cmyk:
        case ChannelLayout::indexed:
            png_destroy_write_struct(&png, &info);
            return Codec::encode_raster_to_sink(source, output, options, stop);
        }
        png_set_IHDR(png, info, source_frame.width, source_frame.height,
                     plane.format.bits_per_channel, color_type,
                     options.interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        if (plane.format.channels == ChannelLayout::bgr ||
            plane.format.channels == ChannelLayout::bgra) {
            png_set_bgr(png);
        }
        Document metadata_document;
        metadata_document.format = descriptor.format;
        metadata_document.canvas_width = descriptor.canvas_width;
        metadata_document.canvas_height = descriptor.canvas_height;
        metadata_document.loop_count = descriptor.loop_count;
        metadata_document.metadata = descriptor.metadata;
        metadata_document.color = descriptor.color;
        Result<void> metadata_status =
            write_png_metadata(png, info, metadata_document, options, &text_chunks, &text_values);
        if (!metadata_status) {
            png_destroy_write_struct(&png, &info);
            return metadata_status.error();
        }
        png_write_info(png, info);
        if (plane.format.bits_per_channel == 16 && plane.format.little_endian &&
            std::endian::native == std::endian::little) {
            png_set_swap(png);
        }
        const int passes = options.interlaced ? png_set_interlace_handling(png) : 1;
        for (int pass = 0; pass < passes; ++pass) {
            for (std::uint32_t y = 0; y < source_frame.height; ++y) {
                if (stop.stop_requested()) {
                    state.failure = cancelled_status();
                    png_error(png, "PNG encode cancelled.");
                }
                Result<void> read = source.read_rows(0, 0, y, 1, row_bytes, row, stop);
                if (!read) {
                    state.failure = read.error();
                    png_error(png, "PNG row source failed.");
                }
                png_write_row(png, reinterpret_cast<png_bytep>(row.data()));
            }
        }
        png_write_end(png, info);
        png_destroy_write_struct(&png, &info);
        if (!state.failure.ok())
            return state.failure;
        return receipt_for_descriptor(descriptor, format());
    }
    Result<MappedPlane> mapped = source.map_plane(0, 0);
    if (!mapped) {
        return Codec::encode_raster_to_sink(source, output, options, stop);
    }
    Result<SharedPixelBuffer> buffer =
        SharedPixelBuffer::adopt(mapped.value().owner, mapped.value().pixels);
    if (!buffer)
        return buffer.error();
    Result<Image> image = Image::adopt(source_frame.width, source_frame.height,
                                       source_frame.layout.planes.front().format,
                                       mapped.value().row_stride, std::move(buffer).value());
    if (!image)
        return image.error();
    Document document;
    document.format = descriptor.format;
    document.canvas_width = descriptor.canvas_width;
    document.canvas_height = descriptor.canvas_height;
    document.loop_count = descriptor.loop_count;
    document.metadata = descriptor.metadata;
    document.color = descriptor.color;
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
    return encode_to_sink(document, output, options, stop);
}

} // namespace snow::image::internal
