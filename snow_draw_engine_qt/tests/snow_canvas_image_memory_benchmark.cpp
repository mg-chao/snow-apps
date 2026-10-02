#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {
void reportMemory(const char* stage) {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        std::cout << stage
                  << " footprint_mib=" << static_cast<double>(info.phys_footprint) / 1048576.0
                  << " resident_mib=" << static_cast<double>(info.resident_size) / 1048576.0
                  << '\n';
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = static_cast<DWORD>(sizeof(info));
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),
                             static_cast<DWORD>(sizeof(info))))
        std::cout << stage << " private_mib=" << static_cast<double>(info.PrivateUsage) / 1048576.0
                  << " working_set_mib=" << static_cast<double>(info.WorkingSetSize) / 1048576.0
                  << '\n';
#else
    static_cast<void>(stage);
#endif
}
} // namespace

// Run each mode in a fresh process: a preceding heap workload would leave its
// cached allocations in the process and contaminate the mapped mode's baseline.
int main(int argc, char** argv) {
    if (argc != 2 ||
        (std::string_view(argv[1]) != "--qt" && std::string_view(argv[1]) != "--pages")) {
        std::cerr << "Usage: snow-canvas-image-memory-benchmark --qt|--pages\n";
        return 1;
    }
    const bool pages = std::string_view(argv[1]) == "--pages";
    std::vector<double> samples;
    samples.reserve(60);
    reportMemory("baseline");
    for (int cycle = 0; cycle < 60; ++cycle) {
        QElapsedTimer timer;
        timer.start();
        {
            std::array<QImage, 3> buffers;
            const std::array<QSize, 3> sizes{QSize(3840, 2160), QSize(1600, 900), QSize(1664, 964)};
            for (std::size_t index = 0; index < sizes.size(); ++index) {
                buffers[index] = pages
                                     ? snowCanvasAllocateImage(sizes[index], QImage::Format_ARGB32)
                                     : QImage(sizes[index], QImage::Format_ARGB32);
                if (buffers[index].isNull())
                    return 2;
                buffers[index].fill(qRgb(cycle, 80, 120));
            }
        }
        samples.push_back(static_cast<double>(timer.nsecsElapsed()) / 1000000.0);
    }
    reportMemory("after_60_released_cycles");
    std::sort(samples.begin(), samples.end());
    std::cout << (pages ? "pages" : "qt") << " p50_ms=" << samples[samples.size() / 2]
              << " p95_ms=" << samples[samples.size() * 95 / 100] << '\n';
    return 0;
}
