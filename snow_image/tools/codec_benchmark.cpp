#include <snow/image/service.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

double median_milliseconds(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t middle = samples.size() / 2U;
    return samples.size() % 2U == 0 ? (samples[middle - 1U] + samples[middle]) * 0.5
                                    : samples[middle];
}

int compare_preview(snow::image::Service& service, const snow::image::Input& input, int iterations,
                    std::uint32_t extent) {
    snow::image::DecodeOptions full_options;
    full_options.output_format = snow::image::kRgba8;
    full_options.frame_index = 0;
    auto preview_options = full_options;
    preview_options.maximum_extent = extent;
    std::vector<double> full_times, preview_times, complete_times;
    std::uint32_t preview_width = 0, preview_height = 0;
    std::uint32_t full_width = 0, full_height = 0;
    const auto milliseconds_since = [](auto started) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    };
    for (int iteration = -1; iteration < iterations; ++iteration) {
        // Alternate the paths; iteration -1 warms decoder and filesystem caches.
        {
            const auto started = std::chrono::steady_clock::now();
            auto full = service.decode(input, full_options);
            if (!full) {
                std::cerr << full.error().message << '\n';
                return 1;
            }
            if (iteration >= 0)
                full_times.push_back(milliseconds_since(started));
            full_width = full.value().canvas_width;
            full_height = full.value().canvas_height;
        }
        const auto started = std::chrono::steady_clock::now();
        auto preview = service.decode(input, preview_options);
        if (!preview) {
            std::cerr << preview.error().message << '\n';
            return 1;
        }
        if (iteration >= 0)
            preview_times.push_back(milliseconds_since(started));
        preview_width = preview.value().canvas_width;
        preview_height = preview.value().canvas_height;
        // Retain the displayed preview while preparing the original for editing.
        auto original = service.decode(input, full_options);
        if (!original) {
            std::cerr << original.error().message << '\n';
            return 1;
        }
        if (iteration >= 0)
            complete_times.push_back(milliseconds_since(started));
    }
    std::cout << std::fixed << std::setprecision(3)
              << "preview comparison: iterations=" << iterations << " full=" << full_width << 'x'
              << full_height << " preview=" << preview_width << 'x' << preview_height
              << " full_ms=" << median_milliseconds(full_times)
              << " first_preview_ms=" << median_milliseconds(preview_times)
              << " preview_then_full_ms=" << median_milliseconds(complete_times) << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: snow_image_benchmark <image> [iterations] "
                     "[--preview-comparison <maximum-extent>]\n";
        return 2;
    }
    const int iterations = argc > 2 ? std::max(1, std::atoi(argv[2])) : 3;
    snow::image::Result<snow::image::Input> input =
        snow::image::file_input(std::filesystem::path(argv[1]));
    if (!input) {
        std::cerr << input.error().message << '\n';
        return 1;
    }
    snow::image::Service service;
    if (argc > 3) {
        if (argc != 5 || std::string_view(argv[3]) != "--preview-comparison")
            return 2;
        const std::string_view argument(argv[4]);
        std::uint32_t extent = 0;
        const auto parsed =
            std::from_chars(argument.data(), argument.data() + argument.size(), extent);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() ||
            extent == 0)
            return 2;
        return compare_preview(service, input.value(), iterations, extent);
    }
    snow::image::Result<snow::image::Document> source = service.decode(input.value());
    if (!source) {
        std::cerr << source.error().message << '\n';
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t decodedBytes = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        snow::image::Result<snow::image::Document> document = service.decode(input.value());
        if (!document) {
            std::cerr << document.error().message << '\n';
            return 1;
        }
        for (const snow::image::Frame& frame : document.value().frames) {
            decodedBytes += frame.image.pixels().size();
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    const double mebibytes = static_cast<double>(decodedBytes) / (1024.0 * 1024.0);
    std::cout << iterations << " decode(s), " << elapsed.count() << " s, "
              << mebibytes / elapsed.count() << " MiB/s\n";

    if (source.value().format == snow::image::Format::jpeg) {
        for (const auto [name, layout] :
             {std::pair{"packed", snow::image::RasterLayoutPolicy::packed},
              std::pair{"native", snow::image::RasterLayoutPolicy::native}}) {
            std::vector<double> samples;
            samples.reserve(static_cast<std::size_t>(iterations));
            std::uint64_t storeBytes = 0;
            for (int iteration = 0; iteration < iterations; ++iteration) {
                const std::filesystem::path path =
                    std::filesystem::temp_directory_path() /
                    ("snow-image-benchmark-" + std::string(name) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".srs");
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                snow::image::DecodeOptions options;
                options.raster_layout = layout;
                const auto decodeStarted = std::chrono::steady_clock::now();
                auto store = service.decode_to_store(input.value(), path, options);
                const auto decodeElapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - decodeStarted);
                if (!store) {
                    std::cerr << store.error().message << '\n';
                    return 1;
                }
                samples.push_back(decodeElapsed.count());
                storeBytes = store.value()->file_bytes();
                store.value().reset();
                std::filesystem::remove(path, ignored);
            }
            std::cout << "JPEG raster decode: layout=" << name << ", iterations=" << iterations
                      << ", median=" << std::fixed << std::setprecision(3)
                      << median_milliseconds(samples) << " ms, store-bytes=" << storeBytes << '\n';
        }
    }

    if (service.encoder_info(snow::image::Format::png)) {
        const std::string_view backend =
            snow::image::compression_backend_version(snow::image::Format::png);
        std::cout << "PNG compression backend: " << backend << '\n';
        for (const int compression : {1, 6, 9}) {
            std::vector<double> samples;
            samples.reserve(static_cast<std::size_t>(iterations));
            std::size_t outputBytes = 0;
            for (int iteration = 0; iteration < iterations; ++iteration) {
                auto encoded = std::make_shared<std::vector<std::byte>>();
                snow::image::EncodeOptions options;
                options.format = snow::image::Format::png;
                options.compression_level = compression;
                const auto encodeStarted = std::chrono::steady_clock::now();
                snow::image::Result<snow::image::EncodeResult> status =
                    service.encode(source.value(), snow::image::memory_output(encoded), options);
                const auto encodeElapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - encodeStarted);
                if (!status) {
                    std::cerr << status.error().message << '\n';
                    return 1;
                }
                samples.push_back(encodeElapsed.count());
                outputBytes = encoded->size();
            }
            std::cout << "PNG encode: compression=" << compression
                      << ", filter=libpng-adaptive, zlib=" << backend
                      << ", iterations=" << iterations << ", median=" << std::fixed
                      << std::setprecision(3) << median_milliseconds(samples)
                      << " ms, bytes=" << outputBytes << '\n';
        }
    }
    return 0;
}
