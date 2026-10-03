#include "codecs/exr_codec.h"

#include <snow/memory/pixel_array.h>

#include <OpenEXR/ImfAttribute.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfDeepScanLineInputPart.h>
#include <OpenEXR/ImfDeepScanLineOutputPart.h>
#include <OpenEXR/ImfDeepTiledInputPart.h>
#include <OpenEXR/ImfDeepTiledOutputPart.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfMultiPartOutputFile.h>
#include <OpenEXR/ImfOpaqueAttribute.h>
#include <OpenEXR/ImfOutputPart.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfPixelType.h>
#include <OpenEXR/ImfTileDescription.h>
#include <OpenEXR/ImfTiledInputPart.h>
#include <OpenEXR/ImfTiledOutputPart.h>
#include <OpenEXR/ImfVersion.h>
#include <Iex.h>
#include <Imath/half.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snow::image::internal {
namespace {

namespace Imf = OPENEXR_IMF_NAMESPACE;
namespace Imath = IMATH_NAMESPACE;

class MemoryIStream final : public Imf::IStream {
  public:
    explicit MemoryIStream(std::span<const std::byte> bytes)
        : Imf::IStream("snow_image memory input"), bytes_(bytes) {}

    bool isMemoryMapped() const override {
        return true;
    }
    bool read(char* destination, int count) override {
        if (count < 0 || static_cast<std::uint64_t>(count) > bytes_.size() - position_) {
            throw IEX_NAMESPACE::InputExc("Unexpected end of OpenEXR input.");
        }
        std::memcpy(destination, bytes_.data() + position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::uint64_t>(count);
        return position_ < bytes_.size();
    }
    char* readMemoryMapped(int count) override {
        if (count < 0 || static_cast<std::uint64_t>(count) > bytes_.size() - position_) {
            throw IEX_NAMESPACE::InputExc("Unexpected end of OpenEXR input.");
        }
        char* result = reinterpret_cast<char*>(const_cast<std::byte*>(bytes_.data() + position_));
        position_ += static_cast<std::uint64_t>(count);
        return result;
    }
    std::uint64_t tellg() override {
        return position_;
    }
    void seekg(std::uint64_t position) override {
        if (position > bytes_.size()) {
            throw IEX_NAMESPACE::InputExc("OpenEXR seek lies outside the input.");
        }
        position_ = position;
    }
    std::int64_t size() override {
        return bytes_.size() > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())
                   ? -1
                   : static_cast<std::int64_t>(bytes_.size());
    }

  private:
    std::span<const std::byte> bytes_;
    std::uint64_t position_ = 0;
};

class MemoryOStream final : public Imf::OStream {
  public:
    MemoryOStream() : Imf::OStream("snow_image memory output") {}

    void write(const char* source, int count) override {
        if (count < 0)
            throw IEX_NAMESPACE::ArgExc("Negative OpenEXR write size.");
        const std::size_t amount = static_cast<std::size_t>(count);
        if (position_ > std::numeric_limits<std::size_t>::max() - amount) {
            throw IEX_NAMESPACE::ArgExc("OpenEXR output position overflow.");
        }
        const std::size_t end = position_ + amount;
        if (end > bytes_.size())
            bytes_.resize(end);
        std::memcpy(bytes_.data() + position_, source, amount);
        position_ = end;
    }
    std::uint64_t tellp() override {
        return position_;
    }
    void seekp(std::uint64_t position) override {
        if (position > std::numeric_limits<std::size_t>::max()) {
            throw IEX_NAMESPACE::ArgExc("OpenEXR output seek overflow.");
        }
        position_ = static_cast<std::size_t>(position);
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

  private:
    std::vector<std::byte> bytes_;
    std::size_t position_ = 0;
};

Status exr_error(ErrorCode code, std::string message) {
    return Status::error(code, std::move(message), "OpenEXR");
}

template <typename Function>
auto guard_exr(ErrorCode code, std::string_view operation, Function&& function)
    -> decltype(function()) {
    using Return = decltype(function());
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return Return(
            exr_error(ErrorCode::out_of_memory, std::string(operation) + " ran out of memory."));
    } catch (const std::exception& exception) {
        std::string message(operation);
        message += " failed: ";
        message += exception.what();
        return Return(exr_error(code, std::move(message)));
    } catch (...) {
        return Return(exr_error(code, std::string(operation) + " failed unexpectedly."));
    }
}

Result<std::pair<std::uint32_t, std::uint32_t>> window_dimensions(const Imath::Box2i& window,
                                                                  const DecodeLimits& limits) {
    const std::int64_t width = static_cast<std::int64_t>(window.max.x) - window.min.x + 1;
    const std::int64_t height = static_cast<std::int64_t>(window.max.y) - window.min.y + 1;
    if (width <= 0 || height <= 0 || width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max()) {
        return exr_error(ErrorCode::corrupt_data, "OpenEXR data window is invalid.");
    }
    Result<void> dimensions = validate_dimensions(static_cast<std::uint32_t>(width),
                                                  static_cast<std::uint32_t>(height), limits);
    if (!dimensions)
        return dimensions.error();
    return std::pair{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
}

std::array<std::int32_t, 4> window_array(const Imath::Box2i& window) noexcept {
    return {window.min.x, window.min.y, window.max.x, window.max.y};
}

Imath::Box2i box_from_array(const std::array<std::int32_t, 4>& window) {
    return {{window[0], window[1]}, {window[2], window[3]}};
}

std::size_t sample_size(Imf::PixelType type) {
    switch (type) {
    case Imf::UINT:
        return 4;
    case Imf::HALF:
        return 2;
    case Imf::FLOAT:
        return 4;
    case Imf::NUM_PIXELTYPES:
        break;
    }
    throw IEX_NAMESPACE::ArgExc("Invalid OpenEXR pixel type.");
}

Imf::PixelType exr_pixel_type(const ExrChannel& channel) {
    if (channel.sample_type == SampleType::unsigned_integer && channel.bits_per_sample == 32) {
        return Imf::UINT;
    }
    if (channel.sample_type == SampleType::floating_point && channel.bits_per_sample == 16) {
        return Imf::HALF;
    }
    if (channel.sample_type == SampleType::floating_point && channel.bits_per_sample == 32) {
        return Imf::FLOAT;
    }
    throw IEX_NAMESPACE::ArgExc("Unsupported snow_image OpenEXR channel type.");
}

ExrChannel channel_description(const char* name, const Imf::Channel& channel) {
    ExrChannel output;
    output.name = name;
    output.sample_type =
        channel.type == Imf::UINT ? SampleType::unsigned_integer : SampleType::floating_point;
    output.bits_per_sample = channel.type == Imf::HALF ? 16 : 32;
    output.x_sampling = static_cast<std::uint32_t>(std::max(channel.xSampling, 1));
    output.y_sampling = static_cast<std::uint32_t>(std::max(channel.ySampling, 1));
    return output;
}

char* offset_base(void* data, const Imath::Box2i& window, std::size_t x_stride,
                  std::size_t y_stride) {
    const std::int64_t offset =
        static_cast<std::int64_t>(window.min.x) * static_cast<std::int64_t>(x_stride) +
        static_cast<std::int64_t>(window.min.y) * static_cast<std::int64_t>(y_stride);
    return static_cast<char*>(data) - offset;
}

bool reserved_attribute(std::string_view name) {
    return name == "channels" || name == "compression" || name == "dataWindow" ||
           name == "displayWindow" || name == "lineOrder" || name == "pixelAspectRatio" ||
           name == "screenWindowCenter" || name == "screenWindowWidth" || name == "name" ||
           name == "type" || name == "version" || name == "chunkCount" || name == "tiles";
}

Result<std::vector<MetadataBlock>> read_attributes(const Imf::Header& header, int version,
                                                   const DecodeOptions& options,
                                                   std::uint64_t& metadata_bytes) {
    std::vector<MetadataBlock> blocks;
    if (!options.preserve_metadata)
        return blocks;
    for (auto iterator = header.begin(); iterator != header.end(); ++iterator) {
        if (reserved_attribute(iterator.name()))
            continue;
        MemoryOStream output;
        iterator.attribute().writeValueTo(output, version);
        if (output.bytes().size() > options.limits.maximum_metadata_bytes ||
            metadata_bytes > options.limits.maximum_metadata_bytes - output.bytes().size()) {
            return exr_error(ErrorCode::limit_exceeded,
                             "OpenEXR attributes exceed the configured metadata limit.");
        }
        metadata_bytes += output.bytes().size();
        MetadataBlock block;
        block.type = iterator.name();
        block.content_type = iterator.attribute().typeName();
        block.data.assign(output.bytes().begin(), output.bytes().end());
        block.safe_to_copy = true;
        blocks.push_back(std::move(block));
    }
    return blocks;
}

ExrPartType part_type(const Imf::Header& header) {
    const std::string type =
        header.hasType() ? header.type()
                         : (header.hasTileDescription() ? Imf::TILEDIMAGE : Imf::SCANLINEIMAGE);
    if (type == Imf::DEEPSCANLINE)
        return ExrPartType::deep_scanline;
    if (type == Imf::DEEPTILE)
        return ExrPartType::deep_tiled;
    if (type == Imf::TILEDIMAGE)
        return ExrPartType::tiled;
    return ExrPartType::scanline;
}

ExrLevelMode level_mode(Imf::LevelMode mode) noexcept {
    switch (mode) {
    case Imf::MIPMAP_LEVELS:
        return ExrLevelMode::mipmap;
    case Imf::RIPMAP_LEVELS:
        return ExrLevelMode::ripmap;
    case Imf::ONE_LEVEL:
        return ExrLevelMode::one_level;
    case Imf::NUM_LEVELMODES:
        break;
    }
    return ExrLevelMode::one_level;
}

Imf::LevelMode level_mode(ExrLevelMode mode) noexcept {
    switch (mode) {
    case ExrLevelMode::mipmap:
        return Imf::MIPMAP_LEVELS;
    case ExrLevelMode::ripmap:
        return Imf::RIPMAP_LEVELS;
    case ExrLevelMode::one_level:
        return Imf::ONE_LEVEL;
    }
    return Imf::ONE_LEVEL;
}

Result<ExrPart> describe_part(const Imf::Header& header, int version, const DecodeOptions& options,
                              std::uint64_t& metadata_bytes) {
    Result<std::pair<std::uint32_t, std::uint32_t>> dimensions =
        window_dimensions(header.dataWindow(), options.limits);
    if (!dimensions)
        return dimensions.error();
    ExrPart part;
    part.name = header.hasName() ? header.name() : std::string{};
    part.type = part_type(header);
    part.data_window = window_array(header.dataWindow());
    part.display_window = window_array(header.displayWindow());
    if (header.hasTileDescription()) {
        part.tile_width = header.tileDescription().xSize;
        part.tile_height = header.tileDescription().ySize;
        part.level_mode = level_mode(header.tileDescription().mode);
    }
    for (auto iterator = header.channels().begin(); iterator != header.channels().end();
         ++iterator) {
        part.channels.push_back(channel_description(iterator.name(), iterator.channel()));
    }
    Result<std::vector<MetadataBlock>> attributes =
        read_attributes(header, version, options, metadata_bytes);
    if (!attributes)
        return attributes.error();
    part.attributes = std::move(attributes).value();
    return part;
}

using FlatReader = std::function<void(Imf::FrameBuffer&)>;

Result<std::vector<ExrChannel>> read_flat_channels(const Imf::Header& header,
                                                   const Imath::Box2i& window,
                                                   const DecodeOptions& options,
                                                   std::uint64_t& owned_bytes,
                                                   const FlatReader& reader) {
    Result<std::pair<std::uint32_t, std::uint32_t>> dimensions =
        window_dimensions(window, options.limits);
    if (!dimensions)
        return dimensions.error();
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(dimensions.value().first) * dimensions.value().second;
    std::vector<ExrChannel> channels;
    for (auto iterator = header.channels().begin(); iterator != header.channels().end();
         ++iterator) {
        ExrChannel channel = channel_description(iterator.name(), iterator.channel());
        const std::uint64_t bytes = pixels * sample_size(iterator.channel().type);
        if (bytes > options.limits.maximum_owned_output_bytes ||
            owned_bytes > options.limits.maximum_owned_output_bytes - bytes) {
            return exr_error(ErrorCode::limit_exceeded,
                             "OpenEXR channels exceed the configured owning decode limit.");
        }
        channel.samples.resize(static_cast<std::size_t>(bytes));
        owned_bytes += bytes;
        channels.push_back(std::move(channel));
    }
    Imf::FrameBuffer frame_buffer;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        ExrChannel& channel = channels[index];
        const Imf::Channel& source = header.channels()[channel.name];
        const std::size_t bytes = sample_size(source.type);
        const std::size_t y_stride = static_cast<std::size_t>(dimensions.value().first) * bytes;
        frame_buffer.insert(channel.name,
                            Imf::Slice(source.type,
                                       offset_base(channel.samples.data(), window, bytes, y_stride),
                                       bytes, y_stride, source.xSampling, source.ySampling, 0.0));
    }
    reader(frame_buffer);
    return channels;
}

using DeepReader = std::function<void(Imf::DeepFrameBuffer&, bool)>;

Result<DeepSamples> read_deep_channels(const Imf::Header& header, const Imath::Box2i& window,
                                       const DecodeOptions& options, std::uint64_t& owned_bytes,
                                       const DeepReader& reader) {
    Result<std::pair<std::uint32_t, std::uint32_t>> dimensions =
        window_dimensions(window, options.limits);
    if (!dimensions)
        return dimensions.error();
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(dimensions.value().first) * dimensions.value().second;
    const std::uint64_t count_bytes = pixel_count * sizeof(std::uint32_t);
    if (count_bytes > options.limits.maximum_owned_output_bytes ||
        owned_bytes > options.limits.maximum_owned_output_bytes - count_bytes) {
        return exr_error(ErrorCode::limit_exceeded,
                         "OpenEXR deep counts exceed the configured owning decode limit.");
    }
    DeepSamples deep;
    deep.counts.resize(static_cast<std::size_t>(pixel_count));
    owned_bytes += count_bytes;
    const std::size_t count_y_stride =
        static_cast<std::size_t>(dimensions.value().first) * sizeof(std::uint32_t);
    Imf::DeepFrameBuffer count_buffer;
    count_buffer.insertSampleCountSlice(Imf::Slice(
        Imf::UINT, offset_base(deep.counts.data(), window, sizeof(std::uint32_t), count_y_stride),
        sizeof(std::uint32_t), count_y_stride));
    reader(count_buffer, true);
    std::uint64_t total_samples = 0;
    for (std::uint32_t count : deep.counts) {
        if (count > options.limits.maximum_deep_samples - total_samples) {
            return exr_error(ErrorCode::limit_exceeded,
                             "OpenEXR deep sample count exceeds the configured limit.");
        }
        total_samples += count;
    }
    std::vector<snow::memory::PixelArray<char*>> pointers;
    std::size_t channel_count = 0;
    for (auto iterator = header.channels().begin(); iterator != header.channels().end();
         ++iterator) {
        ++channel_count;
    }
    pointers.reserve(channel_count);
    for (auto iterator = header.channels().begin(); iterator != header.channels().end();
         ++iterator) {
        ExrChannel channel = channel_description(iterator.name(), iterator.channel());
        const std::size_t bytes_per_sample = sample_size(iterator.channel().type);
        const std::uint64_t bytes = total_samples * bytes_per_sample;
        if (bytes > options.limits.maximum_owned_output_bytes ||
            owned_bytes > options.limits.maximum_owned_output_bytes - bytes) {
            return exr_error(ErrorCode::limit_exceeded,
                             "OpenEXR deep samples exceed the configured owning decode limit.");
        }
        channel.samples.resize(static_cast<std::size_t>(bytes));
        owned_bytes += bytes;
        deep.channels.push_back(std::move(channel));
        pointers.emplace_back(static_cast<std::size_t>(pixel_count), nullptr);
    }
    Imf::DeepFrameBuffer frame_buffer;
    frame_buffer.insertSampleCountSlice(Imf::Slice(
        Imf::UINT, offset_base(deep.counts.data(), window, sizeof(std::uint32_t), count_y_stride),
        sizeof(std::uint32_t), count_y_stride));
    for (std::size_t channel_index = 0; channel_index < deep.channels.size(); ++channel_index) {
        ExrChannel& channel = deep.channels[channel_index];
        const Imf::Channel& source = header.channels()[channel.name];
        const std::size_t bytes_per_sample = sample_size(source.type);
        std::uint64_t sample_offset = 0;
        for (std::size_t pixel = 0; pixel < deep.counts.size(); ++pixel) {
            if (deep.counts[pixel] != 0) {
                pointers[channel_index][pixel] = reinterpret_cast<char*>(
                    channel.samples.data() +
                    static_cast<std::size_t>(sample_offset) * bytes_per_sample);
            }
            sample_offset += deep.counts[pixel];
        }
        const std::size_t pointer_y_stride =
            static_cast<std::size_t>(dimensions.value().first) * sizeof(char*);
        frame_buffer.insert(
            channel.name,
            Imf::DeepSlice(source.type,
                           offset_base(static_cast<void*>(pointers[channel_index].data()), window,
                                       sizeof(char*), pointer_y_stride),
                           sizeof(char*), pointer_y_stride, bytes_per_sample, source.xSampling,
                           source.ySampling));
    }
    reader(frame_buffer, false);
    return deep;
}

float channel_value(const ExrChannel& channel, std::size_t sample) {
    const std::size_t bytes = channel.bits_per_sample / 8U;
    if (bytes == 0 || sample >= channel.samples.size() / bytes) {
        return 0.0F;
    }
    if (channel.sample_type == SampleType::unsigned_integer) {
        std::uint32_t value = 0;
        std::memcpy(&value, channel.samples.data() + sample * bytes, sizeof(value));
        return static_cast<float>(value) /
               static_cast<float>(std::numeric_limits<std::uint32_t>::max());
    }
    if (channel.bits_per_sample == 16) {
        Imath::half value;
        std::memcpy(&value, channel.samples.data() + sample * bytes, sizeof(value));
        return static_cast<float>(value);
    }
    float value = 0.0F;
    std::memcpy(&value, channel.samples.data() + sample * bytes, sizeof(value));
    return value;
}

const ExrChannel* find_channel(const std::vector<ExrChannel>& channels, std::string_view name) {
    const auto exact = std::find_if(channels.begin(), channels.end(),
                                    [name](const ExrChannel& c) { return c.name == name; });
    return exact == channels.end() ? nullptr : &*exact;
}

struct DisplayChannels final {
    const ExrChannel* red = nullptr;
    const ExrChannel* green = nullptr;
    const ExrChannel* blue = nullptr;
    const ExrChannel* alpha = nullptr;
    const ExrChannel* luminance = nullptr;
    const ExrChannel* depth = nullptr;
};

DisplayChannels display_channels(const std::vector<ExrChannel>& channels) {
    DisplayChannels result;
    result.red = find_channel(channels, "R");
    result.green = find_channel(channels, "G");
    result.blue = find_channel(channels, "B");
    result.alpha = find_channel(channels, "A");
    result.luminance = find_channel(channels, "Y");
    result.depth = find_channel(channels, "Z");
    if (!result.red) {
        const auto layered =
            std::find_if(channels.begin(), channels.end(), [](const ExrChannel& c) {
                return std::string_view(c.name).ends_with(".R");
            });
        if (layered != channels.end()) {
            result.red = &*layered;
            const std::string prefix = layered->name.substr(0, layered->name.size() - 1);
            result.green = find_channel(channels, prefix + "G");
            result.blue = find_channel(channels, prefix + "B");
            result.alpha = find_channel(channels, prefix + "A");
        }
    }
    return result;
}

void write_float_pixel(MutableImage& image, std::size_t pixel, const std::array<float, 4>& rgba) {
    std::memcpy(image.pixels().data() + pixel * sizeof(float) * 4U, rgba.data(),
                sizeof(float) * 4U);
}

Result<Image> flatten_part(const ExrPart& part, const DecodeOptions& options,
                           std::uint64_t& owned_bytes, std::stop_token stop) {
    const Imath::Box2i window = box_from_array(part.data_window);
    Result<std::pair<std::uint32_t, std::uint32_t>> dimensions =
        window_dimensions(window, options.limits);
    if (!dimensions)
        return dimensions.error();
    const std::uint64_t bytes = static_cast<std::uint64_t>(dimensions.value().first) *
                                dimensions.value().second * sizeof(float) * 4U;
    if (bytes > options.limits.maximum_owned_output_bytes ||
        owned_bytes > options.limits.maximum_owned_output_bytes - bytes) {
        return exr_error(ErrorCode::limit_exceeded,
                         "OpenEXR display pixels exceed the configured owning decode limit.");
    }
    Result<MutableImage> allocated =
        MutableImage::allocate(dimensions.value().first, dimensions.value().second, kRgba32Float);
    if (!allocated)
        return allocated.error();
    MutableImage image = std::move(allocated).value();
    const std::vector<ExrChannel>& channels =
        part.deep_samples ? part.deep_samples->channels : part.channels;
    const DisplayChannels selected = display_channels(channels);
    if (part.deep_samples) {
        const DeepSamples& deep = *part.deep_samples;
        std::uint64_t sample_start = 0;
        for (std::size_t pixel = 0; pixel < deep.counts.size(); ++pixel) {
            if (stop.stop_requested())
                return cancelled_status();
            std::vector<std::uint32_t> order(deep.counts[pixel]);
            std::iota(order.begin(), order.end(), 0U);
            if (selected.depth) {
                std::sort(order.begin(), order.end(), [&](std::uint32_t left, std::uint32_t right) {
                    return channel_value(*selected.depth,
                                         static_cast<std::size_t>(sample_start + left)) <
                           channel_value(*selected.depth,
                                         static_cast<std::size_t>(sample_start + right));
                });
            }
            std::array<float, 4> rgba{0.0F, 0.0F, 0.0F, 0.0F};
            for (std::uint32_t relative : order) {
                const std::size_t sample = static_cast<std::size_t>(sample_start + relative);
                const float alpha = selected.alpha ? channel_value(*selected.alpha, sample) : 1.0F;
                const float remain = 1.0F - rgba[3];
                const float luminance =
                    selected.luminance ? channel_value(*selected.luminance, sample) : 0.0F;
                rgba[0] += (selected.red ? channel_value(*selected.red, sample) : luminance) *
                           alpha * remain;
                rgba[1] += (selected.green ? channel_value(*selected.green, sample) : luminance) *
                           alpha * remain;
                rgba[2] += (selected.blue ? channel_value(*selected.blue, sample) : luminance) *
                           alpha * remain;
                rgba[3] += alpha * remain;
            }
            write_float_pixel(image, pixel, rgba);
            sample_start += deep.counts[pixel];
        }
    } else {
        const std::size_t pixels =
            static_cast<std::size_t>(dimensions.value().first) * dimensions.value().second;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            if (stop.stop_requested())
                return cancelled_status();
            const float luminance =
                selected.luminance ? channel_value(*selected.luminance, pixel) : 0.0F;
            write_float_pixel(image, pixel,
                              {selected.red ? channel_value(*selected.red, pixel) : luminance,
                               selected.green ? channel_value(*selected.green, pixel) : luminance,
                               selected.blue ? channel_value(*selected.blue, pixel) : luminance,
                               selected.alpha ? channel_value(*selected.alpha, pixel) : 1.0F});
        }
    }
    owned_bytes += bytes;
    return std::move(image).freeze();
}

void add_extra_levels(Imf::TiledInputPart& input, const Imf::Header& header, ExrPart& part,
                      const DecodeOptions& options, std::uint64_t& owned_bytes,
                      std::stop_token stop) {
    for (int y_level = 0; y_level < input.numYLevels(); ++y_level) {
        for (int x_level = 0; x_level < input.numXLevels(); ++x_level) {
            if (!input.isValidLevel(x_level, y_level) || (x_level == 0 && y_level == 0))
                continue;
            if (stop.stop_requested())
                throw IEX_NAMESPACE::InputExc("OpenEXR decode cancelled.");
            const Imath::Box2i window = input.dataWindowForLevel(x_level, y_level);
            Result<std::vector<ExrChannel>> channels = read_flat_channels(
                header, window, options, owned_bytes, [&](Imf::FrameBuffer& frame_buffer) {
                    input.setFrameBuffer(frame_buffer);
                    input.readTiles(0, input.numXTiles(x_level) - 1, 0,
                                    input.numYTiles(y_level) - 1, x_level, y_level);
                });
            if (!channels)
                throw IEX_NAMESPACE::InputExc(channels.error().message.c_str());
            part.levels.push_back({x_level, y_level, window_array(window),
                                   std::move(channels).value(), std::nullopt});
        }
    }
}

void add_extra_levels(Imf::DeepTiledInputPart& input, const Imf::Header& header, ExrPart& part,
                      const DecodeOptions& options, std::uint64_t& owned_bytes,
                      std::stop_token stop) {
    for (int y_level = 0; y_level < input.numYLevels(); ++y_level) {
        for (int x_level = 0; x_level < input.numXLevels(); ++x_level) {
            if (!input.isValidLevel(x_level, y_level) || (x_level == 0 && y_level == 0))
                continue;
            if (stop.stop_requested())
                throw IEX_NAMESPACE::InputExc("OpenEXR decode cancelled.");
            const Imath::Box2i window = input.dataWindowForLevel(x_level, y_level);
            Result<DeepSamples> deep = read_deep_channels(
                header, window, options, owned_bytes,
                [&](Imf::DeepFrameBuffer& frame_buffer, bool counts) {
                    input.setFrameBuffer(frame_buffer);
                    if (counts) {
                        input.readPixelSampleCounts(0, input.numXTiles(x_level) - 1, 0,
                                                    input.numYTiles(y_level) - 1, x_level, y_level);
                    } else {
                        input.readTiles(0, input.numXTiles(x_level) - 1, 0,
                                        input.numYTiles(y_level) - 1, x_level, y_level);
                    }
                });
            if (!deep)
                throw IEX_NAMESPACE::InputExc(deep.error().message.c_str());
            part.levels.push_back(
                {x_level, y_level, window_array(window), {}, std::move(deep).value()});
        }
    }
}

Result<Document> decode_exr(Imf::MultiPartInputFile& input, const DecodeOptions& options,
                            std::stop_token stop) {
    if (input.parts() <= 0) {
        return exr_error(ErrorCode::corrupt_data, "OpenEXR file contains no parts.");
    }
    if (static_cast<std::uint32_t>(input.parts()) > options.limits.maximum_frames) {
        return exr_error(ErrorCode::limit_exceeded,
                         "OpenEXR part count exceeds the configured limit.");
    }
    Document document;
    document.format = Format::exr;
    document.color.dynamic_range = DynamicRange::high;
    document.color.transfer = TransferFunction::linear;
    std::uint64_t owned_bytes = 0;
    std::uint64_t metadata_bytes = 0;
    for (int part_index = 0; part_index < input.parts(); ++part_index) {
        if (stop.stop_requested())
            return cancelled_status();
        const Imf::Header& header = input.header(part_index);
        Result<ExrPart> described = describe_part(header, input.version(), options, metadata_bytes);
        if (!described)
            return described.error();
        ExrPart part = std::move(described).value();
        const Imath::Box2i window = header.dataWindow();
        switch (part.type) {
        case ExrPartType::scanline: {
            Imf::InputPart source(input, part_index);
            Result<std::vector<ExrChannel>> channels = read_flat_channels(
                header, window, options, owned_bytes, [&](Imf::FrameBuffer& frame_buffer) {
                    source.setFrameBuffer(frame_buffer);
                    source.readPixels(window.min.y, window.max.y);
                });
            if (!channels)
                return channels.error();
            part.channels = std::move(channels).value();
            break;
        }
        case ExrPartType::tiled: {
            Imf::TiledInputPart source(input, part_index);
            Result<std::vector<ExrChannel>> channels = read_flat_channels(
                header, source.dataWindowForLevel(0, 0), options, owned_bytes,
                [&](Imf::FrameBuffer& frame_buffer) {
                    source.setFrameBuffer(frame_buffer);
                    source.readTiles(0, source.numXTiles(0) - 1, 0, source.numYTiles(0) - 1, 0, 0);
                });
            if (!channels)
                return channels.error();
            part.channels = std::move(channels).value();
            add_extra_levels(source, header, part, options, owned_bytes, stop);
            break;
        }
        case ExrPartType::deep_scanline: {
            Imf::DeepScanLineInputPart source(input, part_index);
            Result<DeepSamples> deep =
                read_deep_channels(header, window, options, owned_bytes,
                                   [&](Imf::DeepFrameBuffer& frame_buffer, bool counts) {
                                       source.setFrameBuffer(frame_buffer);
                                       if (counts)
                                           source.readPixelSampleCounts(window.min.y, window.max.y);
                                       else
                                           source.readPixels(window.min.y, window.max.y);
                                   });
            if (!deep)
                return deep.error();
            part.deep_samples = std::move(deep).value();
            break;
        }
        case ExrPartType::deep_tiled: {
            Imf::DeepTiledInputPart source(input, part_index);
            Result<DeepSamples> deep = read_deep_channels(
                header, source.dataWindowForLevel(0, 0), options, owned_bytes,
                [&](Imf::DeepFrameBuffer& frame_buffer, bool counts) {
                    source.setFrameBuffer(frame_buffer);
                    if (counts) {
                        source.readPixelSampleCounts(0, source.numXTiles(0) - 1, 0,
                                                     source.numYTiles(0) - 1, 0, 0);
                    } else {
                        source.readTiles(0, source.numXTiles(0) - 1, 0, source.numYTiles(0) - 1, 0,
                                         0);
                    }
                });
            if (!deep)
                return deep.error();
            part.deep_samples = std::move(deep).value();
            add_extra_levels(source, header, part, options, owned_bytes, stop);
            break;
        }
        }
        Result<Image> flattened = flatten_part(part, options, owned_bytes, stop);
        if (!flattened)
            return flattened.error();
        Frame frame;
        frame.image = std::move(flattened).value();
        frame.color = document.color;
        const Imath::Box2i& display = header.displayWindow();
        const auto display_dimensions = window_dimensions(display, options.limits);
        if (!display_dimensions)
            return display_dimensions.error();
        frame.x = window.min.x > display.min.x
                      ? static_cast<std::uint32_t>(window.min.x - display.min.x)
                      : 0;
        frame.y = window.min.y > display.min.y
                      ? static_cast<std::uint32_t>(window.min.y - display.min.y)
                      : 0;
        document.canvas_width = std::max(document.canvas_width, display_dimensions.value().first);
        document.canvas_height =
            std::max(document.canvas_height, display_dimensions.value().second);
        document.frames.push_back(std::move(frame));
        document.exr_parts.push_back(std::move(part));
    }
    return document;
}

Result<DocumentInfo> inspect_exr(Imf::MultiPartInputFile& input, const DecodeOptions& options) {
    if (input.parts() <= 0 ||
        static_cast<std::uint32_t>(input.parts()) > options.limits.maximum_frames) {
        return exr_error(input.parts() <= 0 ? ErrorCode::corrupt_data : ErrorCode::limit_exceeded,
                         "OpenEXR part count is invalid or exceeds the configured limit.");
    }
    DocumentInfo info;
    info.format = Format::exr;
    info.color.dynamic_range = DynamicRange::high;
    info.color.transfer = TransferFunction::linear;
    std::uint64_t metadata_bytes = 0;
    for (int index = 0; index < input.parts(); ++index) {
        const Imf::Header& header = input.header(index);
        Result<ExrPart> part = describe_part(header, input.version(), options, metadata_bytes);
        if (!part)
            return part.error();
        const auto dimensions = window_dimensions(header.dataWindow(), options.limits);
        if (!dimensions)
            return dimensions.error();
        info.frames.push_back({dimensions.value().first,
                               dimensions.value().second,
                               0,
                               0,
                               {},
                               kRgba32Float,
                               true,
                               std::nullopt});
        const Imath::Box2i& display = header.displayWindow();
        const auto display_dimensions = window_dimensions(display, options.limits);
        if (!display_dimensions)
            return display_dimensions.error();
        info.canvas_width = std::max(info.canvas_width, display_dimensions.value().first);
        info.canvas_height = std::max(info.canvas_height, display_dimensions.value().second);
        info.exr_parts.push_back(std::move(part).value());
    }
    return info;
}

float raster_sample(const ImageView& view, std::uint32_t x, std::uint32_t y,
                    std::uint32_t channel) {
    Result<std::size_t> bytes_per_pixel = view.format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return 0.0F;
    const std::byte* pixel = view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride +
                             static_cast<std::size_t>(x) * bytes_per_pixel.value();
    const std::uint32_t channels = view.format.channel_count();
    if (view.format.sample_type == SampleType::floating_point &&
        view.format.bits_per_channel == 32) {
        float value = channel == 3 && channels < 4 ? 1.0F : 0.0F;
        if (channel < channels)
            std::memcpy(&value, pixel + channel * sizeof(float), sizeof(value));
        return value;
    }
    if (view.format.sample_type == SampleType::unsigned_integer &&
        view.format.bits_per_channel == 8) {
        if (view.format.channels == ChannelLayout::gray) {
            return channel == 3 ? 1.0F
                                : static_cast<float>(static_cast<std::uint8_t>(pixel[0])) / 255.0F;
        }
        if (channel == 3 && channels < 4)
            return 1.0F;
        if (channel >= channels)
            return 0.0F;
        std::uint32_t input_channel = channel;
        if ((view.format.channels == ChannelLayout::bgr ||
             view.format.channels == ChannelLayout::bgra) &&
            (channel == 0 || channel == 2)) {
            input_channel = 2U - channel;
        }
        return static_cast<float>(static_cast<std::uint8_t>(pixel[input_channel])) / 255.0F;
    }
    return channel == 3 ? 1.0F : 0.0F;
}

Result<std::vector<ExrPart>> encoding_parts(const Document& document) {
    if (!document.exr_parts.empty())
        return document.exr_parts;
    if (document.frames.empty()) {
        return exr_error(ErrorCode::invalid_argument,
                         "OpenEXR encoding requires EXR parts or a raster frame.");
    }
    const ImageView view = document.frames.front().image.view();
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        view.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return exr_error(ErrorCode::limit_exceeded,
                         "OpenEXR raster dimensions exceed the format coordinate range.");
    }
    ExrPart part;
    part.name = "image";
    part.type = ExrPartType::scanline;
    part.data_window = {0, 0, static_cast<std::int32_t>(view.width - 1),
                        static_cast<std::int32_t>(view.height - 1)};
    part.display_window = part.data_window;
    constexpr std::array<std::string_view, 4> names{"R", "G", "B", "A"};
    for (std::uint32_t component = 0; component < 4; ++component) {
        ExrChannel channel;
        channel.name = names[component];
        channel.sample_type = SampleType::floating_point;
        channel.bits_per_sample = 32;
        channel.samples.resize(static_cast<std::size_t>(view.width) * view.height * sizeof(float));
        for (std::uint32_t y = 0; y < view.height; ++y) {
            for (std::uint32_t x = 0; x < view.width; ++x) {
                const float value = raster_sample(view, x, y, component);
                const std::size_t index = static_cast<std::size_t>(y) * view.width + x;
                std::memcpy(channel.samples.data() + index * sizeof(float), &value, sizeof(value));
            }
        }
        part.channels.push_back(std::move(channel));
    }
    part.attributes = document.metadata.blocks;
    return std::vector<ExrPart>{std::move(part)};
}

void restore_attributes(Imf::Header& header, const std::vector<MetadataBlock>& blocks) {
    for (const MetadataBlock& block : blocks) {
        if (block.type.empty() || block.content_type.empty() || reserved_attribute(block.type) ||
            block.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            continue;
        }
        std::unique_ptr<Imf::Attribute> attribute;
        if (Imf::Attribute::knownType(block.content_type.c_str())) {
            attribute.reset(Imf::Attribute::newAttribute(block.content_type.c_str()));
            MemoryIStream input(block.data);
            attribute->readValueFrom(input, static_cast<int>(block.data.size()), Imf::EXR_VERSION);
        } else {
            attribute = std::make_unique<Imf::OpaqueAttribute>(block.content_type.c_str(),
                                                               static_cast<long>(block.data.size()),
                                                               block.data.data());
        }
        header.insert(block.type, *attribute);
    }
}

Imf::Header make_header(const ExrPart& part, int compression_level) {
    const Imath::Box2i display = box_from_array(part.display_window);
    const Imath::Box2i data = box_from_array(part.data_window);
    const Imf::Compression compression =
        part.type == ExrPartType::deep_scanline || part.type == ExrPartType::deep_tiled
            ? Imf::ZIPS_COMPRESSION
            : Imf::ZIP_COMPRESSION;
    Imf::Header header(display, data, 1.0F, {0.0F, 0.0F}, 1.0F, Imf::INCREASING_Y, compression);
    header.setName(part.name.empty() ? "part" : part.name);
    switch (part.type) {
    case ExrPartType::scanline:
        header.setType(Imf::SCANLINEIMAGE);
        break;
    case ExrPartType::tiled:
        header.setType(Imf::TILEDIMAGE);
        break;
    case ExrPartType::deep_scanline:
        header.setType(Imf::DEEPSCANLINE);
        break;
    case ExrPartType::deep_tiled:
        header.setType(Imf::DEEPTILE);
        break;
    }
    if (part.type == ExrPartType::tiled || part.type == ExrPartType::deep_tiled) {
        header.setTileDescription(Imf::TileDescription(
            part.tile_width == 0 ? 64U : part.tile_width,
            part.tile_height == 0 ? 64U : part.tile_height, level_mode(part.level_mode)));
    }
    const std::vector<ExrChannel>& channels =
        part.deep_samples ? part.deep_samples->channels : part.channels;
    if (channels.empty())
        throw IEX_NAMESPACE::ArgExc("OpenEXR part has no channels.");
    for (const ExrChannel& channel : channels) {
        header.channels().insert(channel.name, Imf::Channel(exr_pixel_type(channel),
                                                            static_cast<int>(channel.x_sampling),
                                                            static_cast<int>(channel.y_sampling)));
    }
    header.zipCompressionLevel() = std::clamp(compression_level, 0, 9);
    restore_attributes(header, part.attributes);
    return header;
}

Imf::FrameBuffer flat_frame_buffer(const std::vector<ExrChannel>& channels,
                                   const Imath::Box2i& window) {
    const std::size_t width =
        static_cast<std::size_t>(static_cast<std::int64_t>(window.max.x) - window.min.x + 1);
    Imf::FrameBuffer buffer;
    for (const ExrChannel& channel : channels) {
        const Imf::PixelType type = exr_pixel_type(channel);
        const std::size_t bytes = sample_size(type);
        const std::size_t y_stride = width * bytes;
        if (channel.samples.size() <
            y_stride * static_cast<std::size_t>(static_cast<std::int64_t>(window.max.y) -
                                                window.min.y + 1)) {
            throw IEX_NAMESPACE::ArgExc("OpenEXR channel sample buffer is too small.");
        }
        buffer.insert(channel.name,
                      Imf::Slice(type,
                                 offset_base(const_cast<std::byte*>(channel.samples.data()), window,
                                             bytes, y_stride),
                                 bytes, y_stride, static_cast<int>(channel.x_sampling),
                                 static_cast<int>(channel.y_sampling)));
    }
    return buffer;
}

struct DeepBufferStorage final {
    Imf::DeepFrameBuffer buffer;
    std::vector<snow::memory::PixelArray<char*>> pointers;
};

DeepBufferStorage deep_frame_buffer(const DeepSamples& deep, const Imath::Box2i& window) {
    const std::size_t width =
        static_cast<std::size_t>(static_cast<std::int64_t>(window.max.x) - window.min.x + 1);
    const std::size_t height =
        static_cast<std::size_t>(static_cast<std::int64_t>(window.max.y) - window.min.y + 1);
    if (deep.counts.size() != width * height) {
        throw IEX_NAMESPACE::ArgExc("OpenEXR deep sample count buffer has the wrong size.");
    }
    DeepBufferStorage storage;
    const std::size_t count_y_stride = width * sizeof(std::uint32_t);
    storage.buffer.insertSampleCountSlice(
        Imf::Slice(Imf::UINT,
                   offset_base(const_cast<std::uint32_t*>(deep.counts.data()), window,
                               sizeof(std::uint32_t), count_y_stride),
                   sizeof(std::uint32_t), count_y_stride));
    const std::uint64_t total =
        std::accumulate(deep.counts.begin(), deep.counts.end(), std::uint64_t{0});
    storage.pointers.resize(deep.channels.size());
    for (std::size_t channel_index = 0; channel_index < deep.channels.size(); ++channel_index) {
        const ExrChannel& channel = deep.channels[channel_index];
        const Imf::PixelType type = exr_pixel_type(channel);
        const std::size_t bytes = sample_size(type);
        if (channel.samples.size() < total * bytes) {
            throw IEX_NAMESPACE::ArgExc("OpenEXR deep channel sample buffer is too small.");
        }
        snow::memory::PixelArray<char*>& pointers = storage.pointers[channel_index];
        pointers.resize(deep.counts.size(), nullptr);
        std::uint64_t offset = 0;
        for (std::size_t pixel = 0; pixel < deep.counts.size(); ++pixel) {
            if (deep.counts[pixel] != 0) {
                pointers[pixel] = reinterpret_cast<char*>(
                    const_cast<std::byte*>(channel.samples.data()) + offset * bytes);
            }
            offset += deep.counts[pixel];
        }
        const std::size_t pointer_y_stride = width * sizeof(char*);
        storage.buffer.insert(channel.name,
                              Imf::DeepSlice(type,
                                             offset_base(static_cast<void*>(pointers.data()),
                                                         window, sizeof(char*), pointer_y_stride),
                                             sizeof(char*), pointer_y_stride, bytes,
                                             static_cast<int>(channel.x_sampling),
                                             static_cast<int>(channel.y_sampling)));
    }
    return storage;
}

const ExrLevel* find_level(const ExrPart& part, int x_level, int y_level) {
    const auto level =
        std::find_if(part.levels.begin(), part.levels.end(), [=](const ExrLevel& candidate) {
            return candidate.x_level == x_level && candidate.y_level == y_level;
        });
    return level == part.levels.end() ? nullptr : &*level;
}

void write_part(Imf::MultiPartOutputFile& output, int index, const ExrPart& part,
                std::stop_token stop) {
    const Imath::Box2i base_window = box_from_array(part.data_window);
    switch (part.type) {
    case ExrPartType::scanline: {
        Imf::OutputPart destination(output, index);
        Imf::FrameBuffer buffer = flat_frame_buffer(part.channels, base_window);
        destination.setFrameBuffer(buffer);
        destination.writePixels(base_window.max.y - base_window.min.y + 1);
        break;
    }
    case ExrPartType::tiled: {
        Imf::TiledOutputPart destination(output, index);
        for (int y_level = 0; y_level < destination.numYLevels(); ++y_level) {
            for (int x_level = 0; x_level < destination.numXLevels(); ++x_level) {
                if (!destination.isValidLevel(x_level, y_level))
                    continue;
                if (stop.stop_requested())
                    throw IEX_NAMESPACE::InputExc("OpenEXR encode cancelled.");
                const ExrLevel* level =
                    (x_level == 0 && y_level == 0) ? nullptr : find_level(part, x_level, y_level);
                if ((x_level != 0 || y_level != 0) && !level) {
                    throw IEX_NAMESPACE::ArgExc("OpenEXR tiled part is missing a level.");
                }
                const Imath::Box2i window =
                    level ? box_from_array(level->data_window) : base_window;
                const std::vector<ExrChannel>& channels = level ? level->channels : part.channels;
                Imf::FrameBuffer buffer = flat_frame_buffer(channels, window);
                destination.setFrameBuffer(buffer);
                destination.writeTiles(0, destination.numXTiles(x_level) - 1, 0,
                                       destination.numYTiles(y_level) - 1, x_level, y_level);
            }
        }
        break;
    }
    case ExrPartType::deep_scanline: {
        if (!part.deep_samples)
            throw IEX_NAMESPACE::ArgExc("Missing deep OpenEXR samples.");
        Imf::DeepScanLineOutputPart destination(output, index);
        DeepBufferStorage storage = deep_frame_buffer(*part.deep_samples, base_window);
        destination.setFrameBuffer(storage.buffer);
        destination.writePixels(base_window.max.y - base_window.min.y + 1);
        break;
    }
    case ExrPartType::deep_tiled: {
        if (!part.deep_samples)
            throw IEX_NAMESPACE::ArgExc("Missing deep OpenEXR samples.");
        Imf::DeepTiledOutputPart destination(output, index);
        for (int y_level = 0; y_level < destination.numYLevels(); ++y_level) {
            for (int x_level = 0; x_level < destination.numXLevels(); ++x_level) {
                if (!destination.isValidLevel(x_level, y_level))
                    continue;
                if (stop.stop_requested())
                    throw IEX_NAMESPACE::InputExc("OpenEXR encode cancelled.");
                const ExrLevel* level =
                    (x_level == 0 && y_level == 0) ? nullptr : find_level(part, x_level, y_level);
                if ((x_level != 0 || y_level != 0) && !level) {
                    throw IEX_NAMESPACE::ArgExc("OpenEXR deep tiled part is missing a level.");
                }
                const DeepSamples* deep =
                    level ? (level->deep_samples ? &*level->deep_samples : nullptr)
                          : &*part.deep_samples;
                if (!deep)
                    throw IEX_NAMESPACE::ArgExc("OpenEXR deep tiled level is missing samples.");
                const Imath::Box2i window =
                    level ? box_from_array(level->data_window) : base_window;
                DeepBufferStorage storage = deep_frame_buffer(*deep, window);
                destination.setFrameBuffer(storage.buffer);
                destination.writeTiles(0, destination.numXTiles(x_level) - 1, 0,
                                       destination.numYTiles(y_level) - 1, x_level, y_level);
            }
        }
        break;
    }
    }
}

} // namespace

CodecCapability ExrCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::multiple_images | CodecCapability::metadata_decode |
           CodecCapability::hdr | CodecCapability::deep_data;
}

int ExrCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 4 && header[0] == std::byte{0x76} && header[1] == std::byte{0x2F} &&
        header[2] == std::byte{0x31} && header[3] == std::byte{0x01}) {
        return 100;
    }
    return format_from_extension(name_hint) == Format::exr ? 10 : 0;
}

Result<DocumentInfo> ExrCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    return guard_exr(ErrorCode::corrupt_data, "OpenEXR inspection", [&]() -> Result<DocumentInfo> {
        if (stop.stop_requested())
            return cancelled_status();
        Result<std::vector<std::byte>> bytes =
            read_all(*input.source, options.limits.maximum_input_bytes);
        if (!bytes)
            return bytes.error();
        MemoryIStream stream(bytes.value());
        Imf::MultiPartInputFile file(stream);
        return inspect_exr(file, options);
    });
}

Result<Document> ExrCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    return guard_exr(ErrorCode::decode_failed, "OpenEXR decoding", [&]() -> Result<Document> {
        Result<std::vector<std::byte>> bytes =
            read_all(*input.source, options.limits.maximum_input_bytes);
        if (!bytes)
            return bytes.error();
        MemoryIStream stream(bytes.value());
        Imf::MultiPartInputFile file(stream);
        return decode_exr(file, options, stop);
    });
}

Result<EncodedArtifactReceipt> ExrCodec::encode_to_sink(const Document& document,
                                                        const Output& output,
                                                        const EncodeOptions& options,
                                                        std::stop_token stop) const {
    return guard_exr(
        ErrorCode::encode_failed, "OpenEXR encoding", [&]() -> Result<EncodedArtifactReceipt> {
            if (stop.stop_requested())
                return cancelled_status();
            Result<std::vector<ExrPart>> parts = encoding_parts(document);
            if (!parts)
                return parts.error();
            if (!options.preserve_metadata) {
                for (ExrPart& part : parts.value())
                    part.attributes.clear();
            }
            if (parts.value().size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                return exr_error(ErrorCode::limit_exceeded, "OpenEXR part count is too large.");
            }
            std::vector<Imf::Header> headers;
            headers.reserve(parts.value().size());
            for (std::size_t index = 0; index < parts.value().size(); ++index) {
                ExrPart& part = parts.value()[index];
                if (part.name.empty())
                    part.name = "part" + std::to_string(index);
                headers.push_back(make_header(part, options.compression_level));
            }
            MemoryOStream stream;
            {
                Imf::MultiPartOutputFile file(stream, headers.data(),
                                              static_cast<int>(headers.size()));
                for (std::size_t index = 0; index < parts.value().size(); ++index) {
                    write_part(file, static_cast<int>(index), parts.value()[index], stop);
                }
            }
            Result<void> written = output.sink->write(stream.bytes());
            if (!written)
                return written.error();
            return receipt_for_document(document, format());
        });
}

} // namespace snow::image::internal
