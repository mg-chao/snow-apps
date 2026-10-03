// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_TEST_SUPPORT_MEMORY_SNAPSHOT_H
#define SNOW_TEST_SUPPORT_MEMORY_SNAPSHOT_H

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace snow::test_support {
struct MemorySnapshot {
    std::uint64_t residentBytes = 0;
    // macOS physical footprint; Windows private committed bytes. These measure
    // different resources and should only be compared on the same platform.
    std::uint64_t footprintBytes = 0;
    std::uint64_t peakResidentBytes = 0;
};

inline MemorySnapshot memorySnapshot() {
    MemorySnapshot snapshot;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = static_cast<DWORD>(sizeof(info));
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),
                             static_cast<DWORD>(sizeof(info)))) {
        snapshot.residentBytes = info.WorkingSetSize;
        snapshot.footprintBytes = info.PrivateUsage;
        snapshot.peakResidentBytes = info.PeakWorkingSetSize;
    }
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        snapshot.residentBytes = info.resident_size;
        snapshot.footprintBytes = info.phys_footprint;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        snapshot.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t virtualPages = 0;
    std::uint64_t residentPages = 0;
    if (statm >> virtualPages >> residentPages)
        snapshot.residentBytes = residentPages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        snapshot.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    return snapshot;
}
} // namespace snow::test_support
#endif
