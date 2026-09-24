#include "codecs/gif_codec.h"

#include <gif_lib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace snow::image::internal {
namespace {

Status gif_error(int code, ErrorCode category) {
    const char* text = GifErrorString(code);
    return Status::error(category, text && *text ? text : "giflib operation failed.", "giflib",
                         code);
}

struct GifReadState final {
    std::span<const std::byte> bytes;
    std::size_t position = 0;
    std::stop_token stop;
};

int gif_read(GifFileType* file, GifByteType* destination, int requested) {
    auto* state = static_cast<GifReadState*>(file->UserData);
    if (!state || requested < 0 || state->stop.stop_requested())
        return 0;
    const std::size_t available =
        state->bytes.size() - std::min(state->position, state->bytes.size());
    const std::size_t count = std::min(available, static_cast<std::size_t>(requested));
    if (count != 0) {
        std::memcpy(destination, state->bytes.data() + state->position, count);
        state->position += count;
    }
    return static_cast<int>(count);
}

struct GifReadDeleter final {
    void operator()(GifFileType* file) const noexcept {
        if (file) {
            int error = 0;
            DGifCloseFile(file, &error);
        }
    }
};
using GifReadFile = std::unique_ptr<GifFileType, GifReadDeleter>;

struct GifContext final {
    GifReadState state;
    GifReadFile file;

    Result<void> open(std::span<const std::byte> bytes, std::stop_token stop, bool slurp = true) {
        state = {bytes, 0, stop};
        int error = 0;
        file.reset(DGifOpen(&state, &gif_read, &error));
        if (!file)
            return gif_error(error, ErrorCode::corrupt_data);
        if (slurp && DGifSlurp(file.get()) == GIF_ERROR) {
            return stop.stop_requested()
                       ? Result<void>(cancelled_status())
                       : Result<void>(gif_error(file->Error, ErrorCode::corrupt_data));
        }
        return {};
    }
};

std::uint32_t loop_count(const GifFileType& file) {
    const auto scan = [](const ExtensionBlock* blocks, int count) -> std::optional<std::uint32_t> {
        for (int index = 0; index + 1 < count; ++index) {
            const ExtensionBlock& application = blocks[index];
            const ExtensionBlock& data = blocks[index + 1];
            if (application.Function == APPLICATION_EXT_FUNC_CODE && application.ByteCount == 11 &&
                std::memcmp(application.Bytes, "NETSCAPE2.0", 11) == 0 &&
                data.Function == CONTINUE_EXT_FUNC_CODE && data.ByteCount >= 3 &&
                data.Bytes[0] == 1) {
                return static_cast<std::uint32_t>(data.Bytes[1]) |
                       static_cast<std::uint32_t>(data.Bytes[2]) << 8U;
            }
        }
        return std::nullopt;
    };
    for (int image = 0; image < file.ImageCount; ++image) {
        const SavedImage& saved = file.SavedImages[image];
        if (const auto value = scan(saved.ExtensionBlocks, saved.ExtensionBlockCount))
            return *value;
    }
    if (const auto value = scan(file.ExtensionBlocks, file.ExtensionBlockCount))
        return *value;
    return 1;
}

FrameDisposal disposal_mode(int mode) {
    if (mode == DISPOSE_BACKGROUND)
        return FrameDisposal::background;
    if (mode == DISPOSE_PREVIOUS)
        return FrameDisposal::previous;
    return FrameDisposal::keep;
}

Result<DocumentInfo> gif_info(const GifFileType& file, const DecodeOptions& options) {
    if (file.SWidth <= 0 || file.SHeight <= 0 || file.ImageCount <= 0) {
        return Status::error(ErrorCode::corrupt_data, "GIF canvas or frame count is invalid.",
                             "giflib");
    }
    Result<void> dimensions =
        validate_dimensions(static_cast<std::uint32_t>(file.SWidth),
                            static_cast<std::uint32_t>(file.SHeight), options.limits);
    if (!dimensions)
        return dimensions.error();
    if (static_cast<std::uint32_t>(file.ImageCount) > options.limits.maximum_frames) {
        return Status::error(ErrorCode::limit_exceeded, "GIF frame count exceeds the decode limit.",
                             "giflib");
    }
    DocumentInfo info;
    info.format = Format::gif;
    info.canvas_width = static_cast<std::uint32_t>(file.SWidth);
    info.canvas_height = static_cast<std::uint32_t>(file.SHeight);
    info.loop_count = loop_count(file);
    info.frames.reserve(static_cast<std::size_t>(file.ImageCount));
    for (int index = 0; index < file.ImageCount; ++index) {
        const SavedImage& saved = file.SavedImages[index];
        const GifImageDesc& descriptor = saved.ImageDesc;
        if (descriptor.Width <= 0 || descriptor.Height <= 0 || descriptor.Left < 0 ||
            descriptor.Top < 0 || descriptor.Left + descriptor.Width > file.SWidth ||
            descriptor.Top + descriptor.Height > file.SHeight) {
            return Status::error(ErrorCode::corrupt_data, "GIF frame rectangle is invalid.",
                                 "giflib");
        }
        GraphicsControlBlock control{};
        const bool has_control =
            DGifSavedExtensionToGCB(const_cast<GifFileType*>(&file), index, &control) == GIF_OK;
        info.frames.push_back({static_cast<std::uint32_t>(descriptor.Width),
                               static_cast<std::uint32_t>(descriptor.Height),
                               static_cast<std::uint32_t>(descriptor.Left),
                               static_cast<std::uint32_t>(descriptor.Top),
                               std::chrono::milliseconds(has_control ? control.DelayTime * 10 : 0),
                               kRgba8, has_control && control.TransparentColor >= 0, std::nullopt});
    }
    return info;
}

struct GifStreamInspection final {
    DocumentInfo info;
    std::vector<bool> interlaced;
};

struct GifExtensionState final {
    GraphicsControlBlock control{};
    bool has_control = false;
    std::uint32_t loop_count = 1;
};

Result<void> read_extensions(GifFileType* file, GifExtensionState* state, std::stop_token stop) {
    int code = 0;
    GifByteType* block = nullptr;
    if (DGifGetExtension(file, &code, &block) == GIF_ERROR)
        return stop.stop_requested()
                   ? Result<void>(cancelled_status())
                   : Result<void>(gif_error(file->Error, ErrorCode::corrupt_data));
    bool netscape = false;
    bool first = true;
    while (block) {
        if (stop.stop_requested())
            return cancelled_status();
        const std::size_t size = block[0];
        const GifByteType* data = block + 1;
        if (first && code == GRAPHICS_EXT_FUNC_CODE) {
            GraphicsControlBlock control{};
            if (DGifExtensionToGCB(size, data, &control) == GIF_OK) {
                state->control = control;
                state->has_control = true;
            }
        } else if (first && code == APPLICATION_EXT_FUNC_CODE && size == 11 &&
                   std::memcmp(data, "NETSCAPE2.0", 11) == 0) {
            netscape = true;
        } else if (!first && netscape && size >= 3 && data[0] == 1) {
            state->loop_count =
                static_cast<std::uint32_t>(data[1]) | static_cast<std::uint32_t>(data[2]) << 8U;
        }
        first = false;
        if (DGifGetExtensionNext(file, &block) == GIF_ERROR)
            return stop.stop_requested()
                       ? Result<void>(cancelled_status())
                       : Result<void>(gif_error(file->Error, ErrorCode::corrupt_data));
    }
    return {};
}

Result<void> validate_stream_frame(const GifFileType& file, const GifImageDesc& descriptor) {
    if (descriptor.Width <= 0 || descriptor.Height <= 0 || descriptor.Left < 0 ||
        descriptor.Top < 0 || descriptor.Left + descriptor.Width > file.SWidth ||
        descriptor.Top + descriptor.Height > file.SHeight)
        return Status::error(ErrorCode::corrupt_data, "GIF frame rectangle is invalid.", "giflib");
    const ColorMapObject* palette = descriptor.ColorMap ? descriptor.ColorMap : file.SColorMap;
    if (!palette || palette->ColorCount <= 0 || !palette->Colors)
        return Status::error(ErrorCode::corrupt_data, "GIF frame has no usable color table.",
                             "giflib");
    return {};
}

Result<void> consume_gif_rows(GifFileType* file, std::span<GifByteType> row, std::stop_token stop) {
    for (int y = 0; y < file->Image.Height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        if (DGifGetLine(file, row.data(), file->Image.Width) == GIF_ERROR)
            return stop.stop_requested()
                       ? Result<void>(cancelled_status())
                       : Result<void>(gif_error(file->Error, ErrorCode::corrupt_data));
    }
    return {};
}

Result<GifStreamInspection> inspect_gif_stream(std::span<const std::byte> bytes,
                                               const DecodeOptions& options, std::stop_token stop) {
    if (options.output_format && *options.output_format != kRgba8)
        return Status::error(ErrorCode::unsupported_feature, "GIF decoding supports RGBA8 output.",
                             "giflib");
    GifContext context;
    Result<void> opened = context.open(bytes, stop, false);
    if (!opened)
        return opened.error();
    if (context.file->SWidth <= 0 || context.file->SHeight <= 0)
        return Status::error(ErrorCode::corrupt_data, "GIF canvas dimensions are invalid.",
                             "giflib");
    Result<void> dimensions =
        validate_dimensions(static_cast<std::uint32_t>(context.file->SWidth),
                            static_cast<std::uint32_t>(context.file->SHeight), options.limits);
    if (!dimensions)
        return dimensions.error();

    GifStreamInspection result;
    result.info.format = Format::gif;
    result.info.canvas_width = static_cast<std::uint32_t>(context.file->SWidth);
    result.info.canvas_height = static_cast<std::uint32_t>(context.file->SHeight);
    GifExtensionState extensions;
    std::vector<GifByteType> row(static_cast<std::size_t>(context.file->SWidth));
    std::uint32_t source_index = 0;
    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        GifRecordType record = UNDEFINED_RECORD_TYPE;
        if (DGifGetRecordType(context.file.get(), &record) == GIF_ERROR)
            return gif_error(context.file->Error, ErrorCode::corrupt_data);
        if (record == TERMINATE_RECORD_TYPE)
            break;
        if (record == EXTENSION_RECORD_TYPE) {
            Result<void> extension = read_extensions(context.file.get(), &extensions, stop);
            if (!extension)
                return extension.error();
            continue;
        }
        if (record != IMAGE_DESC_RECORD_TYPE)
            continue;
        if (source_index >= options.limits.maximum_frames)
            return Status::error(ErrorCode::limit_exceeded,
                                 "GIF frame count exceeds the decode limit.", "giflib");
        if (DGifGetImageDesc(context.file.get()) == GIF_ERROR)
            return gif_error(context.file->Error, ErrorCode::corrupt_data);
        const GifImageDesc& descriptor = context.file->Image;
        Result<void> valid = validate_stream_frame(*context.file, descriptor);
        if (!valid)
            return valid.error();
        const std::size_t width = static_cast<std::size_t>(descriptor.Width);
        if (width > row.size())
            row.resize(width);
        const bool selected = !options.frame_index || *options.frame_index == source_index;
        if (selected) {
            const bool transparent =
                extensions.has_control && extensions.control.TransparentColor >= 0;
            FrameInfo frame{static_cast<std::uint32_t>(descriptor.Width),
                            static_cast<std::uint32_t>(descriptor.Height),
                            static_cast<std::uint32_t>(descriptor.Left),
                            static_cast<std::uint32_t>(descriptor.Top),
                            std::chrono::milliseconds(
                                extensions.has_control ? extensions.control.DelayTime * 10 : 0),
                            kRgba8,
                            transparent,
                            std::nullopt};
            frame.blend = transparent ? FrameBlend::over : FrameBlend::source;
            frame.disposal = disposal_mode(extensions.has_control ? extensions.control.DisposalMode
                                                                  : DISPOSAL_UNSPECIFIED);
            result.info.frames.push_back(std::move(frame));
            result.interlaced.push_back(descriptor.Interlace != 0);
        }
        Result<void> consumed =
            consume_gif_rows(context.file.get(), std::span(row.data(), width), stop);
        if (!consumed)
            return consumed.error();
        extensions.has_control = false;
        ++source_index;
    }
    result.info.loop_count = extensions.loop_count;
    if (options.frame_index && result.info.frames.empty())
        return Status::error(ErrorCode::invalid_argument,
                             "Requested GIF frame index is out of range.", "giflib");
    if (result.info.frames.empty())
        return Status::error(ErrorCode::corrupt_data, "GIF has no frames.", "giflib");
    return result;
}

int interlaced_row(int encoded_row, int height) {
    constexpr std::array starts{0, 4, 2, 1};
    constexpr std::array steps{8, 8, 4, 2};
    for (std::size_t pass = 0; pass < starts.size(); ++pass) {
        if (starts[pass] >= height)
            continue;
        const int count = (height - starts[pass] + steps[pass] - 1) / steps[pass];
        if (encoded_row < count)
            return starts[pass] + encoded_row * steps[pass];
        encoded_row -= count;
    }
    return height - 1;
}

Result<void> convert_gif_row(const ColorMapObject& palette, const GraphicsControlBlock* control,
                             std::span<const GifByteType> indices, std::span<std::byte> rgba) {
    if (rgba.size() < indices.size() * 4U)
        return Status::error(ErrorCode::internal_error, "GIF row storage is too small.", "giflib");
    for (std::size_t x = 0; x < indices.size(); ++x) {
        const int color_index = indices[x];
        if (color_index < 0 || color_index >= palette.ColorCount)
            return Status::error(ErrorCode::corrupt_data, "GIF palette index is out of range.",
                                 "giflib");
        const GifColorType color = palette.Colors[color_index];
        const std::size_t destination = x * 4U;
        rgba[destination] = static_cast<std::byte>(color.Red);
        rgba[destination + 1U] = static_cast<std::byte>(color.Green);
        rgba[destination + 2U] = static_cast<std::byte>(color.Blue);
        rgba[destination + 3U] =
            control && color_index == control->TransparentColor ? std::byte{0} : std::byte{0xFF};
    }
    return {};
}

Result<void> stream_gif_frames(std::span<const std::byte> bytes,
                               const GifStreamInspection& inspection, PixelSink& sink,
                               const DecodeOptions& options, std::stop_token stop) {
    Result<void> status = sink.begin(inspection.info);
    if (!status)
        return status;
    GifContext context;
    status = context.open(bytes, stop, false);
    if (!status)
        return status;
    GifExtensionState extensions;
    std::vector<GifByteType> discard_row;
    std::uint32_t source_index = 0;
    std::uint32_t sink_index = 0;
    for (;;) {
        if (stop.stop_requested())
            return cancelled_status();
        GifRecordType record = UNDEFINED_RECORD_TYPE;
        if (DGifGetRecordType(context.file.get(), &record) == GIF_ERROR)
            return gif_error(context.file->Error, ErrorCode::corrupt_data);
        if (record == TERMINATE_RECORD_TYPE)
            break;
        if (record == EXTENSION_RECORD_TYPE) {
            status = read_extensions(context.file.get(), &extensions, stop);
            if (!status)
                return status;
            continue;
        }
        if (record != IMAGE_DESC_RECORD_TYPE)
            continue;
        if (source_index >= options.limits.maximum_frames)
            return Status::error(ErrorCode::limit_exceeded,
                                 "GIF frame count exceeds the decode limit.", "giflib");
        if (DGifGetImageDesc(context.file.get()) == GIF_ERROR)
            return gif_error(context.file->Error, ErrorCode::corrupt_data);
        const GifImageDesc& descriptor = context.file->Image;
        status = validate_stream_frame(*context.file, descriptor);
        if (!status)
            return status;
        const std::size_t width = static_cast<std::size_t>(descriptor.Width);
        const std::size_t height = static_cast<std::size_t>(descriptor.Height);
        const bool selected = !options.frame_index || *options.frame_index == source_index;
        if (!selected) {
            try {
                discard_row.resize(width);
            } catch (const std::bad_alloc&) {
                return Status::error(ErrorCode::out_of_memory, "Could not allocate a GIF scanline.",
                                     "giflib");
            }
            status =
                consume_gif_rows(context.file.get(), std::span(discard_row.data(), width), stop);
            if (!status)
                return status;
            extensions.has_control = false;
            ++source_index;
            continue;
        }
        if (sink_index >= inspection.info.frames.size())
            return Status::error(ErrorCode::corrupt_data,
                                 "GIF frame count changed between decode passes.", "giflib");
        const FrameInfo& frame = inspection.info.frames[sink_index];
        if (frame.width != width || frame.height != height)
            return Status::error(ErrorCode::corrupt_data,
                                 "GIF frame geometry changed between decode passes.", "giflib");
        const ColorMapObject* palette =
            descriptor.ColorMap ? descriptor.ColorMap : context.file->SColorMap;
        const GraphicsControlBlock* control =
            extensions.has_control ? &extensions.control : nullptr;
        const std::uint64_t rgba_row_bytes64 = static_cast<std::uint64_t>(width) * 4U;
        const std::uint64_t frame_bytes64 = rgba_row_bytes64 * height;
        if (rgba_row_bytes64 > std::numeric_limits<std::size_t>::max() ||
            frame_bytes64 > std::numeric_limits<std::size_t>::max())
            return Status::error(ErrorCode::limit_exceeded, "GIF frame byte count overflows.",
                                 "giflib");
        const std::size_t rgba_row_bytes = static_cast<std::size_t>(rgba_row_bytes64);
        const std::size_t frame_bytes = static_cast<std::size_t>(frame_bytes64);
        status = sink.begin_frame(sink_index, frame);
        if (!status)
            return status;
        std::span<std::byte> storage = sink.frame_storage(sink_index, rgba_row_bytes, frame_bytes);
        const bool direct_storage = storage.size() == frame_bytes;
        const bool interlaced = descriptor.Interlace != 0;
        const std::uint64_t working_bytes =
            direct_storage ? width
            : interlaced   ? static_cast<std::uint64_t>(width) * height + rgba_row_bytes64
                           : static_cast<std::uint64_t>(width) + rgba_row_bytes64;
        if (working_bytes > options.limits.maximum_working_bytes)
            return Status::error(
                ErrorCode::limit_exceeded,
                "One GIF frame exceeds the configured streaming working-memory limit.", "giflib");
        try {
            if (direct_storage) {
                std::vector<GifByteType> indices(width);
                for (int encoded_y = 0; encoded_y < descriptor.Height; ++encoded_y) {
                    if (stop.stop_requested())
                        return cancelled_status();
                    if (DGifGetLine(context.file.get(), indices.data(), descriptor.Width) ==
                        GIF_ERROR)
                        return gif_error(context.file->Error, ErrorCode::corrupt_data);
                    const int output_y =
                        interlaced ? interlaced_row(encoded_y, descriptor.Height) : encoded_y;
                    status = convert_gif_row(
                        *palette, control, indices,
                        storage.subspan(static_cast<std::size_t>(output_y) * rgba_row_bytes,
                                        rgba_row_bytes));
                    if (!status)
                        return status;
                }
            } else if (interlaced) {
                std::vector<GifByteType> indices(width * height);
                std::vector<std::byte> rgba(rgba_row_bytes);
                for (int encoded_y = 0; encoded_y < descriptor.Height; ++encoded_y) {
                    if (stop.stop_requested())
                        return cancelled_status();
                    const int output_y = interlaced_row(encoded_y, descriptor.Height);
                    GifByteType* row = indices.data() + static_cast<std::size_t>(output_y) * width;
                    if (DGifGetLine(context.file.get(), row, descriptor.Width) == GIF_ERROR)
                        return gif_error(context.file->Error, ErrorCode::corrupt_data);
                }
                for (std::uint32_t y = 0; y < frame.height; ++y) {
                    if (stop.stop_requested())
                        return cancelled_status();
                    status = convert_gif_row(
                        *palette, control,
                        std::span(indices.data() + static_cast<std::size_t>(y) * width, width),
                        rgba);
                    if (!status)
                        return status;
                    status = sink.write_rows(y, 1, rgba_row_bytes, rgba);
                    if (!status)
                        return status;
                }
            } else {
                std::vector<GifByteType> indices(width);
                std::vector<std::byte> rgba(rgba_row_bytes);
                for (std::uint32_t y = 0; y < frame.height; ++y) {
                    if (stop.stop_requested())
                        return cancelled_status();
                    if (DGifGetLine(context.file.get(), indices.data(), descriptor.Width) ==
                        GIF_ERROR)
                        return gif_error(context.file->Error, ErrorCode::corrupt_data);
                    status = convert_gif_row(*palette, control, indices, rgba);
                    if (!status)
                        return status;
                    status = sink.write_rows(y, 1, rgba_row_bytes, rgba);
                    if (!status)
                        return status;
                }
            }
        } catch (const std::bad_alloc&) {
            return Status::error(ErrorCode::out_of_memory,
                                 "Could not allocate bounded GIF frame storage.", "giflib");
        }
        status = sink.end_frame(sink_index);
        if (!status)
            return status;
        ++sink_index;
        extensions.has_control = false;
        ++source_index;
        if (options.frame_index)
            break;
    }
    if (sink_index != inspection.info.frames.size())
        return Status::error(ErrorCode::corrupt_data,
                             "GIF frame count changed between decode passes.", "giflib");
    return sink.end();
}

Result<Frame> decode_frame(const GifFileType& file, int index, std::stop_token stop) {
    if (stop.stop_requested())
        return cancelled_status();
    const SavedImage& saved = file.SavedImages[index];
    const GifImageDesc& descriptor = saved.ImageDesc;
    const ColorMapObject* palette = descriptor.ColorMap ? descriptor.ColorMap : file.SColorMap;
    if (!palette || palette->ColorCount <= 0 || !saved.RasterBits) {
        return Status::error(ErrorCode::corrupt_data, "GIF frame has no usable color table.",
                             "giflib");
    }
    GraphicsControlBlock control{};
    const bool has_control =
        DGifSavedExtensionToGCB(const_cast<GifFileType*>(&file), index, &control) == GIF_OK;
    Result<MutableImage> allocated =
        MutableImage::allocate(static_cast<std::uint32_t>(descriptor.Width),
                               static_cast<std::uint32_t>(descriptor.Height), kRgba8);
    if (!allocated)
        return allocated.error();
    MutableImage pixels = std::move(allocated).value();
    for (int y = 0; y < descriptor.Height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        for (int x = 0; x < descriptor.Width; ++x) {
            const std::size_t source =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(descriptor.Width) +
                static_cast<std::size_t>(x);
            const int color_index = saved.RasterBits[source];
            if (color_index < 0 || color_index >= palette->ColorCount) {
                return Status::error(ErrorCode::corrupt_data, "GIF palette index is out of range.",
                                     "giflib");
            }
            const GifColorType color = palette->Colors[color_index];
            const std::size_t destination = static_cast<std::size_t>(y) * pixels.row_stride() +
                                            static_cast<std::size_t>(x) * 4U;
            pixels.pixels()[destination] = static_cast<std::byte>(color.Red);
            pixels.pixels()[destination + 1U] = static_cast<std::byte>(color.Green);
            pixels.pixels()[destination + 2U] = static_cast<std::byte>(color.Blue);
            pixels.pixels()[destination + 3U] =
                has_control && color_index == control.TransparentColor ? std::byte{0}
                                                                       : std::byte{0xFF};
        }
    }
    Frame frame;
    frame.image = std::move(pixels).freeze();
    frame.x = static_cast<std::uint32_t>(descriptor.Left);
    frame.y = static_cast<std::uint32_t>(descriptor.Top);
    frame.duration = std::chrono::milliseconds(has_control ? control.DelayTime * 10 : 0);
    frame.blend =
        has_control && control.TransparentColor >= 0 ? FrameBlend::over : FrameBlend::source;
    frame.disposal = disposal_mode(has_control ? control.DisposalMode : DISPOSAL_UNSPECIFIED);
    return frame;
}

struct GifWriteState final {
    ByteSink* sink = nullptr;
    Status error;
};

int gif_write(GifFileType* file, const GifByteType* source, int count) {
    auto* state = static_cast<GifWriteState*>(file->UserData);
    if (!state || !state->sink || count < 0)
        return 0;
    Result<void> written =
        state->sink->write(std::as_bytes(std::span(source, static_cast<std::size_t>(count))));
    if (!written) {
        state->error = written.error();
        return 0;
    }
    return count;
}

struct GifWriteDeleter final {
    void operator()(GifFileType* file) const noexcept {
        if (file) {
            int error = 0;
            EGifCloseFile(file, &error);
        }
    }
};
using GifWriteFile = std::unique_ptr<GifFileType, GifWriteDeleter>;

std::array<GifColorType, 256> fixed_palette() {
    std::array<GifColorType, 256> palette{};
    for (std::size_t index = 0; index < palette.size(); ++index) {
        const std::uint8_t code = static_cast<std::uint8_t>(index);
        palette[index] = {static_cast<GifByteType>(((code >> 5U) * 255U) / 7U),
                          static_cast<GifByteType>((((code >> 2U) & 7U) * 255U) / 7U),
                          static_cast<GifByteType>(((code & 3U) * 255U) / 3U)};
    }
    return palette;
}

Result<std::vector<GifByteType>> indexed_frame(const ImageView& view, bool* has_transparency) {
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.format.sample_type != SampleType::unsigned_integer ||
        view.format.bits_per_channel != 8 ||
        (view.format.channels != ChannelLayout::rgb &&
         view.format.channels != ChannelLayout::rgba &&
         view.format.channels != ChannelLayout::bgr &&
         view.format.channels != ChannelLayout::bgra &&
         view.format.channels != ChannelLayout::gray)) {
        return Status::error(
            ErrorCode::unsupported_feature,
            "GIF encoding requires packed 8-bit gray, RGB, BGR, RGBA, or BGRA pixels.", "giflib");
    }
    std::vector<GifByteType> indices(static_cast<std::size_t>(view.width) * view.height);
    *has_transparency = false;
    const std::size_t channels = view.format.channel_count();
    const bool bgr =
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const auto row =
            view.pixels.subspan(static_cast<std::size_t>(y) * view.row_stride, view.row_stride);
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t source = static_cast<std::size_t>(x) * channels;
            if (channels == 4U && std::to_integer<std::uint8_t>(row[source + 3U]) < 128U) {
                indices[static_cast<std::size_t>(y) * view.width + x] = 0;
                *has_transparency = true;
                continue;
            }
            const std::uint8_t red = std::to_integer<std::uint8_t>(row[source + (bgr ? 2U : 0U)]);
            const std::uint8_t green =
                channels == 1U ? red : std::to_integer<std::uint8_t>(row[source + 1U]);
            const std::uint8_t blue =
                channels == 1U ? red : std::to_integer<std::uint8_t>(row[source + (bgr ? 0U : 2U)]);
            std::uint8_t value =
                static_cast<std::uint8_t>((red & 0xE0U) | ((green & 0xE0U) >> 3U) | (blue >> 6U));
            if (*has_transparency || channels == 4U) {
                value = static_cast<std::uint8_t>(1U +
                                                  (static_cast<unsigned int>(value) * 254U) / 255U);
            }
            indices[static_cast<std::size_t>(y) * view.width + x] = value;
        }
    }
    return indices;
}

Result<void> write_loop_extension(GifFileType* file, std::uint32_t loop_count_value) {
    const std::uint16_t loops = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(loop_count_value, std::numeric_limits<std::uint16_t>::max()));
    const std::array<GifByteType, 3> data{1, static_cast<GifByteType>(loops & 0xFFU),
                                          static_cast<GifByteType>(loops >> 8U)};
    if (EGifPutExtensionLeader(file, APPLICATION_EXT_FUNC_CODE) == GIF_ERROR ||
        EGifPutExtensionBlock(file, 11, "NETSCAPE2.0") == GIF_ERROR ||
        EGifPutExtensionBlock(file, static_cast<int>(data.size()), data.data()) == GIF_ERROR ||
        EGifPutExtensionTrailer(file) == GIF_ERROR) {
        return gif_error(file->Error, ErrorCode::encode_failed);
    }
    return {};
}

} // namespace

CodecCapability GifCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::animation | CodecCapability::streaming_decode |
           CodecCapability::metadata_decode;
}

int GifCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 6 && (std::memcmp(header.data(), "GIF87a", 6) == 0 ||
                               std::memcmp(header.data(), "GIF89a", 6) == 0))
        return 100;
    return format_from_extension(name_hint) == Format::gif ? 10 : 0;
}

Result<DocumentInfo> GifCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<GifStreamInspection> inspected = inspect_gif_stream(bytes.value(), options, stop);
    if (!inspected)
        return inspected.error();
    return std::move(inspected).value().info;
}

Result<Document> GifCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    if (options.output_format && *options.output_format != kRgba8)
        return Status::error(ErrorCode::unsupported_feature, "GIF decoding supports RGBA8 output.",
                             "giflib");
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    GifContext context;
    Result<void> opened = context.open(bytes.value(), stop);
    if (!opened)
        return opened.error();
    Result<DocumentInfo> info = gif_info(*context.file, options);
    if (!info)
        return info.error();
    if (options.frame_index && *options.frame_index >= info.value().frames.size())
        return Status::error(ErrorCode::invalid_argument,
                             "Requested GIF frame index is out of range.", "giflib");
    const std::size_t first = options.frame_index ? *options.frame_index : 0;
    const std::size_t end = options.frame_index ? first + 1 : info.value().frames.size();
    std::uint64_t output_bytes = 0;
    for (std::size_t index = first; index < end; ++index) {
        const FrameInfo& frame = info.value().frames[index];
        output_bytes += static_cast<std::uint64_t>(frame.width) * frame.height * 4U;
        if (output_bytes > options.limits.maximum_owned_output_bytes) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "GIF frames exceed the owning decode limit.", "giflib");
        }
    }
    Document document;
    document.format = Format::gif;
    document.canvas_width = info.value().canvas_width;
    document.canvas_height = info.value().canvas_height;
    document.loop_count = info.value().loop_count;
    document.frames.reserve(end - first);
    for (std::size_t index = first; index < end; ++index) {
        Result<Frame> frame = decode_frame(*context.file, static_cast<int>(index), stop);
        if (!frame)
            return frame.error();
        document.frames.push_back(std::move(frame).value());
    }
    return document;
}

Result<void> GifCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                      const DecodeOptions& options, std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<GifStreamInspection> inspected = inspect_gif_stream(bytes.value(), options, stop);
    if (!inspected)
        return inspected.error();
    return stream_gif_frames(bytes.value(), inspected.value(), sink, options, stop);
}

Result<EncodedArtifactReceipt> GifCodec::encode_to_sink(const Document& document,
                                                        const Output& output,
                                                        const EncodeOptions& options,
                                                        std::stop_token stop) const {
    if (document.frames.empty() || document.canvas_width == 0 || document.canvas_height == 0 ||
        document.canvas_width > static_cast<std::uint32_t>(std::numeric_limits<GifWord>::max()) ||
        document.canvas_height > static_cast<std::uint32_t>(std::numeric_limits<GifWord>::max())) {
        return Status::error(ErrorCode::invalid_argument, "GIF document or canvas is invalid.",
                             "giflib");
    }
    GifWriteState state{output.sink.get(), {}};
    int error = 0;
    GifWriteFile file(EGifOpen(&state, &gif_write, &error));
    if (!file)
        return gif_error(error, ErrorCode::encode_failed);
    EGifSetGifVersion(file.get(), true);
    const auto palette_values = fixed_palette();
    std::unique_ptr<ColorMapObject, decltype(&GifFreeMapObject)> palette(
        GifMakeMapObject(static_cast<int>(palette_values.size()), palette_values.data()),
        &GifFreeMapObject);
    if (!palette) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate the GIF palette.",
                             "giflib");
    }
    if (EGifPutScreenDesc(file.get(), static_cast<int>(document.canvas_width),
                          static_cast<int>(document.canvas_height), 8, 0,
                          palette.get()) == GIF_ERROR) {
        return gif_error(file->Error, ErrorCode::encode_failed);
    }
    Result<void> extension = write_loop_extension(file.get(), document.loop_count);
    if (!extension)
        return extension.error();
    for (const Frame& frame : document.frames) {
        if (stop.stop_requested())
            return cancelled_status();
        const ImageView view = frame.image.view();
        if (frame.x + view.width > document.canvas_width ||
            frame.y + view.height > document.canvas_height ||
            view.width > static_cast<std::uint32_t>(std::numeric_limits<GifWord>::max()) ||
            view.height > static_cast<std::uint32_t>(std::numeric_limits<GifWord>::max())) {
            return Status::error(ErrorCode::invalid_argument, "GIF frame rectangle is invalid.",
                                 "giflib");
        }
        bool transparent = false;
        Result<std::vector<GifByteType>> indexed = indexed_frame(view, &transparent);
        if (!indexed)
            return indexed.error();
        GraphicsControlBlock control{};
        control.DisposalMode =
            frame.disposal == FrameDisposal::background
                ? DISPOSE_BACKGROUND
                : (frame.disposal == FrameDisposal::previous ? DISPOSE_PREVIOUS : DISPOSE_DO_NOT);
        control.DelayTime = static_cast<int>(std::clamp<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(frame.duration).count() / 10, 0,
            std::numeric_limits<std::uint16_t>::max()));
        control.TransparentColor = transparent ? 0 : NO_TRANSPARENT_COLOR;
        std::array<GifByteType, 4> control_bytes{};
        const std::size_t control_size = EGifGCBToExtension(&control, control_bytes.data());
        if (EGifPutExtension(file.get(), GRAPHICS_EXT_FUNC_CODE, static_cast<int>(control_size),
                             control_bytes.data()) == GIF_ERROR ||
            EGifPutImageDesc(file.get(), static_cast<int>(frame.x), static_cast<int>(frame.y),
                             static_cast<int>(view.width), static_cast<int>(view.height),
                             options.interlaced, nullptr) == GIF_ERROR) {
            return gif_error(file->Error, ErrorCode::encode_failed);
        }
        for (std::uint32_t encoded_y = 0; encoded_y < view.height; ++encoded_y) {
            const std::uint32_t source_y =
                options.interlaced
                    ? static_cast<std::uint32_t>(interlaced_row(static_cast<int>(encoded_y),
                                                                static_cast<int>(view.height)))
                    : encoded_y;
            GifByteType* row =
                indexed.value().data() + static_cast<std::size_t>(source_y) * view.width;
            if (EGifPutLine(file.get(), row, static_cast<int>(view.width)) == GIF_ERROR) {
                return state.error.ok() ? gif_error(file->Error, ErrorCode::encode_failed)
                                        : state.error;
            }
        }
    }
    GifFileType* raw = file.release();
    if (EGifCloseFile(raw, &error) == GIF_ERROR) {
        return state.error.ok() ? gif_error(error, ErrorCode::encode_failed) : state.error;
    }
    return receipt_for_document(document, format());
}

} // namespace snow::image::internal
