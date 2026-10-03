// SPDX-License-Identifier: GPL-3.0-or-later
// Public APIs and fixtures are identical in the baseline and optimized build.
#include "snow/image/processing.h"
#include "snow/image/raster_conversion.h"
#include "snow/image/service.h"
#include "../../test-support/memorysnapshot.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace snow::image;
using Clock = std::chrono::steady_clock;
std::uint64_t sink = 0;

struct Options {
    std::string scenario = "allocation";
    std::string input;
    std::uint32_t width = 3840;
    std::uint32_t height = 2160;
    int warmup = 3;
    int count = 30;
};

int number(std::string_view text, bool zeroAllowed = false) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value < (zeroAllowed ? 0 : 1))
        throw std::runtime_error("invalid numeric argument");
    return value;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 == argc)
            throw std::runtime_error("missing argument value");
        const std::string_view value(argv[++index]);
        if (argument == "--scenario")
            options.scenario = value;
        else if (argument == "--input")
            options.input = value;
        else if (argument == "--width")
            options.width = static_cast<std::uint32_t>(number(value));
        else if (argument == "--height")
            options.height = static_cast<std::uint32_t>(number(value));
        else if (argument == "--warmup")
            options.warmup = number(value, true);
        else if (argument == "--count" || argument == "--repeat")
            options.count = number(value);
        else
            throw std::runtime_error("unknown argument");
    }
    return options;
}

template <typename T> T unwrap(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

void requireResult(Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}

Image fixture(std::uint32_t width, std::uint32_t height) {
    auto image = unwrap(MutableImage::allocate(width, height, kRgba8));
    for (std::uint32_t y = 0; y < height; ++y) {
        auto* row = image.pixels().data() + static_cast<std::size_t>(y) * image.row_stride();
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto value = x * 1664525U + y * 1013904223U;
            const auto offset = static_cast<std::size_t>(x) * 4;
            row[offset] = static_cast<std::byte>(value >> 3);
            row[offset + 1] = static_cast<std::byte>(value >> 11);
            row[offset + 2] = static_cast<std::byte>(value >> 19);
            row[offset + 3] = static_cast<std::byte>(32 + value % 224);
        }
    }
    return std::move(image).freeze();
}

// Generate rows directly so planar scratch is measured without retaining a
// second full-sized source raster. No file cache or codec setup enters these cases.
class GeneratedRaster final : public RasterSource {
  public:
    GeneratedRaster(std::uint32_t width, std::uint32_t height, const std::string& scenario) {
        descriptor_.canvas_width = width;
        descriptor_.canvas_height = height;
        RasterFrameDescriptor frame;
        frame.width = width;
        frame.height = height;
        if (scenario == "yuv420-region") {
            frame.layout.color_model = ColorModel::ycbcr;
            frame.layout.alpha = AlphaMode::none;
            frame.layout.chroma_subsampling = ChromaSubsampling::yuv420;
            frame.layout.planes = {
                {PlaneSemantic::luma, width, height, kGray8, 8},
                {PlaneSemantic::chroma_blue, (width + 1) / 2, (height + 1) / 2, kGray8, 8},
                {PlaneSemantic::chroma_red, (width + 1) / 2, (height + 1) / 2, kGray8, 8}};
        } else if (scenario == "gray-region") {
            frame.layout.color_model = ColorModel::gray;
            frame.layout.alpha = AlphaMode::none;
            frame.layout.planes = {{PlaneSemantic::gray, width, height, kGray8, 8}};
        } else {
            frame.layout.planes = {{PlaneSemantic::packed, width, height, kRgba8, 8}};
        }
        descriptor_.frames.push_back(std::move(frame));
        requireResult(descriptor_.validate());
    }

    const DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    RasterAccess access() const noexcept override {
        return RasterAccess::random_rows | RasterAccess::random_regions;
    }
    Result<void> read_rows(std::uint32_t frame, std::uint32_t plane, std::uint32_t first,
                           std::uint32_t count, std::size_t stride, std::span<std::byte> pixels,
                           std::stop_token stop) const override {
        const auto& layout = descriptor_.frames.at(frame).layout.planes.at(plane);
        return read_region(frame, plane, {0, first, layout.width, count},
                           {layout.width, count, layout.format, stride, pixels}, stop);
    }
    Result<void> read_region(std::uint32_t frame, std::uint32_t plane, RasterRect region,
                             MutablePlaneView destination, std::stop_token stop) const override {
        const auto& layout = descriptor_.frames.at(frame).layout.planes.at(plane);
        auto valid = destination.validate();
        if (!valid)
            return valid;
        const bool packed = layout.format == kRgba8;
        for (std::uint32_t y = 0; y < region.height; ++y) {
            if (stop.stop_requested())
                return Status::error(ErrorCode::cancelled, "Generated raster read cancelled.");
            auto* row =
                destination.pixels.data() + static_cast<std::size_t>(y) * destination.row_stride;
            for (std::uint32_t x = 0; x < region.width; ++x) {
                const auto value = (region.x + x) * 1664525U + (region.y + y) * 1013904223U;
                if (packed) {
                    const auto offset = static_cast<std::size_t>(x) * 4;
                    row[offset] = static_cast<std::byte>(value >> 3);
                    row[offset + 1] = static_cast<std::byte>(value >> 11);
                    row[offset + 2] = static_cast<std::byte>(value >> 19);
                    row[offset + 3] = static_cast<std::byte>(32 + value % 224);
                } else {
                    row[x] = static_cast<std::byte>(plane == 0 ? (value >> 11) : 96 + value % 64);
                }
            }
        }
        return {};
    }

  private:
    DocumentDescriptor descriptor_;
};

Document makeFixture(const Options& options) {
    Document document;
    document.canvas_width = options.width;
    document.canvas_height = options.height;
    Frame frame;
    if (options.scenario == "flatten-partial") {
        frame.image = fixture(std::max(1U, options.width / 2), std::max(1U, options.height / 2));
        frame.x = options.width / 4;
        frame.y = options.height / 4;
        frame.blend = FrameBlend::over;
        frame.disposal = FrameDisposal::previous;
    } else {
        frame.image = fixture(options.width, options.height);
    }
    document.frames.push_back(std::move(frame));
    if (options.scenario == "flatten" || options.scenario == "flatten-partial" ||
        options.scenario == "animation-compose") {
        // Flatten needs only the first composed frame. Full replacement and
        // partial-first fixtures distinguish its zero-copy and bounded-frame
        // paths from preservation, which needs a float canvas and all outputs.
        for (std::uint32_t index = 1; index < 4; ++index) {
            Frame partial;
            partial.image =
                fixture(std::max(1U, options.width / 2), std::max(1U, options.height / 2));
            partial.x = options.width / 8 * index;
            partial.y = options.height / 8 * index;
            partial.blend = FrameBlend::over;
            partial.disposal = index >= 2 ? FrameDisposal::previous : FrameDisposal::keep;
            document.frames.push_back(std::move(partial));
        }
    }
    return document;
}

Document oneImage(Image image) {
    Document result;
    result.canvas_width = image.width();
    result.canvas_height = image.height();
    Frame frame;
    frame.image = std::move(image);
    result.frames.push_back(std::move(frame));
    return result;
}

Document apply(const Options& options, const Document& source, const GeneratedRaster& generated,
               const Input& input, const Service& service) {
    const auto& scenario = options.scenario;
    if (scenario == "allocation") {
        auto image = unwrap(MutableImage::allocate(options.width, options.height, kRgba8));
        std::memset(image.pixels().data(), 127, image.pixels().size());
        return oneImage(std::move(image).freeze());
    }
    if (scenario == "copy")
        return oneImage(
            std::move(unwrap(MutableImage::copy(source.frames.front().image.view()))).freeze());
    if (scenario == "resize" || scenario == "palette") {
        TransformOptions transformOptions;
        if (scenario == "resize") {
            ResizeOptions resize;
            resize.width = std::max(1U, options.width / 2);
            resize.height = std::max(1U, options.height / 2);
            resize.linear_rgb = false;
            resize.maximum_threads = 1;
            transformOptions.resize = resize;
        } else {
            transformOptions.palette = PaletteOptions{32, 1.0F};
        }
        return unwrap(transform(source, transformOptions));
    }
    if (scenario == "resize-stream") {
        ResizeOptions resize;
        resize.width = std::max(1U, options.width / 4);
        resize.height = std::max(1U, options.height / 4);
        resize.maximum_threads = 1;
        resize.linear_rgb = false;
        // Select the bounded source-major accumulator instead of the row ring.
        resize.maximum_worker_cache_bytes = 1;
        auto image = unwrap(MutableImage::allocate(resize.width, resize.height, kRgba8));
        requireResult(resize_raster_into(
            generated, resize,
            {image.width(), image.height(), image.format(), image.row_stride(), image.pixels()}));
        return oneImage(std::move(image).freeze());
    }
    if (scenario == "flatten" || scenario == "flatten-partial")
        return unwrap(flatten_animation(source));
    if (scenario == "animation-compose")
        return unwrap(transform(source, TransformOptions{}));
    if (scenario == "gray-region" || scenario == "yuv420-region") {
        auto image = unwrap(MutableImage::allocate(options.width, options.height, kRgba8));
        requireResult(read_rgba8_region(
            generated, 0, {0, 0, options.width, options.height},
            {image.width(), image.height(), image.format(), image.row_stride(), image.pixels()}));
        return oneImage(std::move(image).freeze());
    }
    if (scenario == "decode")
        return unwrap(service.decode(input));
    throw std::runtime_error("unknown scenario: " + scenario);
}

std::uint64_t logicalBytes(const Document& document) {
    std::uint64_t bytes = 0;
    for (const auto& frame : document.frames)
        bytes += frame.image.pixels().size();
    return bytes;
}

std::uint64_t checksum(const Document& document) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& frame : document.frames) {
        const auto view = frame.image.view();
        const auto rowBytes =
            static_cast<std::size_t>(view.width) * unwrap(view.format.bytes_per_pixel());
        for (std::uint32_t y = 0; y < view.height; ++y) {
            const auto* row = view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride;
            for (std::size_t x = 0; x < rowBytes; ++x) {
                hash ^= std::to_integer<std::uint8_t>(row[x]);
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

void touch(const Document& document) {
    if (document.frames.empty())
        throw std::runtime_error("operation returned no frames");
    for (const auto& frame : document.frames) {
        if (frame.image.empty())
            throw std::runtime_error("operation returned an empty frame");
        sink += frame.image.pixels().size() +
                std::to_integer<std::uint8_t>(frame.image.pixels().front());
    }
}

void report(const char* record, const Options& options, int iteration, std::int64_t elapsed = 0,
            std::uint64_t hash = 0, std::uint64_t bytes = 0) {
    const auto memory = snow::test_support::memorySnapshot();
    std::cout << record << ',' << options.scenario << ',' << options.width << ',' << options.height
              << ',' << iteration << ',' << elapsed << ',' << hash << ',' << bytes << ','
              << memory.residentBytes << ',' << memory.footprintBytes << ','
              << memory.peakResidentBytes << '\n';
}
} // namespace

// Each invocation measures exactly one scenario and size. Run baseline/head in
// alternating fresh processes; allocator caches from another scenario are noise.
int main(int argc, char** argv) {
    try {
        auto options = parseOptions(argc, argv);
        std::cout << "record,scenario,width,height,iteration,elapsed_ns,checksum,logical_bytes,"
                     "rss_bytes,footprint_bytes,peak_rss_bytes\n";
        report("process_baseline", options, -1);
        Service service;
        Input input;
        if (options.scenario == "decode") {
            if (options.input.empty())
                throw std::runtime_error("decode requires --input <file>");
            input = unwrap(file_input(options.input));
            const auto info = unwrap(service.inspect(input));
            options.width = info.canvas_width;
            options.height = info.canvas_height;
        }
        GeneratedRaster generated(options.width, options.height, options.scenario);
        Document source;
        if (options.scenario == "copy" || options.scenario == "resize" ||
            options.scenario == "palette" || options.scenario == "flatten" ||
            options.scenario == "flatten-partial" || options.scenario == "animation-compose")
            source = makeFixture(options);
        report("fixture_baseline", options, -1, 0, 0, logicalBytes(source));
        std::vector<std::int64_t> samples;
        samples.reserve(static_cast<std::size_t>(options.count));
        for (int iteration = -options.warmup; iteration < options.count; ++iteration) {
            const auto started = Clock::now();
            {
                const auto output = apply(options, source, generated, input, service);
                touch(output);
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
                    .count();
            if (iteration >= 0) {
                samples.push_back(elapsed);
                report("sample", options, iteration, elapsed);
            }
        }
        report("after_cycles_released", options, -1);
        {
            const auto output = apply(options, source, generated, input, service);
            report("output_live", options, -1, 0, checksum(output), logicalBytes(output));
        }
        report("output_released", options, -1);
        source = {};
        report("all_released", options, -1);
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&samples](std::size_t percent) {
            return samples[(samples.size() * percent + 99) / 100 - 1];
        };
        std::cerr << options.scenario << ",p50_ns=" << percentile(50)
                  << ",p95_ns=" << percentile(95) << ",sink=" << sink << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
