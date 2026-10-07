#include "snow_stitch_images.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {
constexpr std::uint32_t width = 3840;
constexpr std::uint32_t height = 2160;
constexpr std::size_t bytes = std::size_t(width) * height * 4;
constexpr int warmups = 3;
constexpr int rounds = 31;
using Clock = std::chrono::steady_clock;
using Pixels = std::shared_ptr<const std::vector<std::uint8_t>>;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void releasePixels(void* context) {
    delete static_cast<Pixels*>(context);
}

std::string outputPath() {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    require(_dupenv_s(&value, &length, "SNOW_SCROLLING_PERF_OUTPUT") == 0,
            "could not read output path");
    std::unique_ptr<char, decltype(&std::free)> owner(value, &std::free);
    return value ? value : "";
#else
    const auto* value = std::getenv("SNOW_SCROLLING_PERF_OUTPUT");
    return value ? value : "";
#endif
}

std::uint64_t privateBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    require(GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters)) != 0,
            "could not query process memory");
    return counters.PrivateUsage;
#else
    return 0;
#endif
}

double median(std::vector<std::uint64_t> values) {
    std::sort(values.begin(), values.end());
    return static_cast<double>(values[values.size() / 2]) / 1'000'000.0;
}

struct Measurement {
    double pushMilliseconds = 0;
    double handoffMilliseconds = 0;
    std::uint64_t livePrivateBytes = 0;
};

Measurement measure(const Pixels& source, bool external) {
    SnowStitchConfig config{};
    require(snow_stitch_config_default(&config) != 0, "could not create config");
    const auto destroySession = [](SnowStitchSession* value) {
        snow_stitch_session_destroy(value);
    };
    std::unique_ptr<SnowStitchSession, decltype(destroySession)> session(
        snow_stitch_session_create(&config), destroySession);
    const auto destroyPool = [](SnowStitchFramePool* value) {
        snow_stitch_frame_pool_destroy(value);
    };
    std::unique_ptr<SnowStitchFramePool, decltype(destroyPool)> pool(
        external ? nullptr : snow_stitch_frame_pool_create(width, height, 6), destroyPool);
    require(session && (external || pool), "could not create handoff state");
    std::vector<std::uint64_t> pushTimes;
    std::vector<std::uint64_t> handoffTimes;
    for (int round = -warmups - 1; round < rounds; ++round) {
        SnowStitchFrameBuffer* frame = nullptr;
        std::uint64_t copiedNs = 0;
        const auto started = Clock::now();
        if (!external) {
            frame = snow_stitch_frame_pool_acquire_for_overwrite(pool.get());
            SnowStitchMutableImageInfo info{};
            require(frame && snow_stitch_frame_buffer_info(frame, &info),
                    "could not acquire frame");
            const auto copyStarted = Clock::now();
            std::memcpy(info.rgba_bytes, source->data(), bytes);
            copiedNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - copyStarted)
                    .count();
        }
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        snow_stitch_perf_reset_thread();
#endif
        SnowStitchFrameOutcome outcome{};
        const auto pushed =
            external ? snow_stitch_session_push_external_rgba(session.get(), width, height,
                                                              source->data(), bytes, &releasePixels,
                                                              new Pixels(source), &outcome)
                     : snow_stitch_session_push_owned(session.get(), &frame, &outcome);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
        require(pushed && frame == nullptr, "handoff failed");
        require(outcome.event == (round == -warmups - 1 ? SNOW_STITCH_FRAME_EVENT_INITIAL
                                                        : SNOW_STITCH_FRAME_EVENT_DUPLICATE),
                "handoff changed duplicate decisions");
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        SnowStitchPerfSnapshot stages{};
        require(snow_stitch_perf_read_thread(&stages, sizeof(stages)) &&
                    stages.calls[SNOW_STITCH_PERF_FRAME_FREEZE] == 1,
                "handoff timing was not recorded");
        if (round >= 0)
            handoffTimes.push_back(copiedNs + stages.elapsed_ns[SNOW_STITCH_PERF_FRAME_FREEZE]);
#else
        static_cast<void>(copiedNs);
#endif
        if (round >= 0)
            pushTimes.push_back(static_cast<std::uint64_t>(elapsed));
    }
    const auto liveBytes = privateBytes();
    const auto destroyImage = [](SnowStitchOwnedImage* value) {
        snow_stitch_owned_image_destroy(value);
    };
    std::unique_ptr<SnowStitchOwnedImage, decltype(destroyImage)> result(
        snow_stitch_session_materialize_axis(session.get(), 0, height), destroyImage);
    SnowStitchImageInfo info{};
    require(result && snow_stitch_owned_image_info(result.get(), &info) && info.rgba_len == bytes &&
                std::memcmp(info.rgba_bytes, source->data(), bytes) == 0,
            "handoff changed result pixels");
    return {median(pushTimes), handoffTimes.empty() ? 0 : median(handoffTimes), liveBytes};
}
} // namespace

int main() {
    try {
        require(std::string(SNOW_SCROLLING_BUILD_CONFIG) == "Release",
                "benchmark requires Release");
        auto source = std::make_shared<std::vector<std::uint8_t>>(bytes);
        for (std::size_t i = 0; i < bytes; ++i)
            (*source)[i] = static_cast<std::uint8_t>(i * 71 + i / 97);
        const auto legacy = measure(source, false);
        const auto external = measure(source, true);
        const auto pushReduction =
            100.0 * (1.0 - external.pushMilliseconds / legacy.pushMilliseconds);
        const auto handoffReduction =
            legacy.handoffMilliseconds == 0
                ? 0
                : 100.0 * (1.0 - external.handoffMilliseconds / legacy.handoffMilliseconds);
        std::ostringstream report;
        report << "{\"rounds\":" << rounds << ",\"width\":" << width << ",\"height\":" << height
               << ",\"legacy_push_median_ms\":" << legacy.pushMilliseconds
               << ",\"external_push_median_ms\":" << external.pushMilliseconds
               << ",\"push_reduction_percent\":" << pushReduction
               << ",\"legacy_handoff_median_ms\":" << legacy.handoffMilliseconds
               << ",\"external_handoff_median_ms\":" << external.handoffMilliseconds
               << ",\"handoff_reduction_percent\":" << handoffReduction
               << ",\"legacy_live_private_bytes\":" << legacy.livePrivateBytes
               << ",\"external_live_private_bytes\":" << external.livePrivateBytes
               << ",\"pixels_match\":true,\"decisions_match\":true}";
        std::cout << report.str() << '\n';
        if (const auto path = outputPath(); !path.empty()) {
            std::ofstream output(path);
            output << report.str() << '\n';
            require(output.good(), "could not write handoff artifact");
        }
        require(pushReduction >= 20, "duplicate push improvement is below 20 percent");
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        require(handoffReduction >= 90, "handoff improvement is below 90 percent");
#endif
#ifdef _WIN32
        require(legacy.livePrivateBytes >= external.livePrivateBytes + bytes * 2,
                "retained memory reduction is less than two viewport buffers");
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
