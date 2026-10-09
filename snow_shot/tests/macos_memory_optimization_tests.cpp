#include "snow_shot/runtime/memoryoptimizationcontroller.h"

#include <QCoreApplication>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
namespace runtime = snow_shot::runtime;
constexpr std::size_t kAllocationBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kChecksumSeed = 1469598103934665603ULL;
constexpr std::uint64_t kChecksumPrime = 1099511628211ULL;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void nativeCallbacksAndTrimAreAvailable() {
    const auto options = runtime::nativeMemoryOptimizationOptions();
    require(options.lowMemory && options.sample && options.trim && options.report,
            "the macOS production memory callbacks must be available");
    const auto before = options.sample();
    require(before && before->memoryBytes > 0, "sample the current process's physical footprint");
    const auto result = options.trim();
    // The allocator may already have released every unused page. Reclaiming zero
    // bytes is a successful best-effort operation, not a native error.
    require(result.succeeded && result.error == 0,
            "macOS allocator relief succeeds even when no bytes can be released");
    const auto after = options.sample();
    require(after && after->memoryBytes > 0, "physical-footprint sampling remains available");
    require(options.lowMemory().has_value(), "the native memory-pressure source is available");
}

std::uint64_t checksum(const volatile std::uint64_t* data, std::size_t count) {
    std::uint64_t value = kChecksumSeed;
    for (std::size_t index = 0; index < count; ++index)
        value = (value ^ data[index]) * kChecksumPrime;
    return value;
}

void liveAllocationRemainsIntact() {
    const auto options = runtime::nativeMemoryOptimizationOptions();
    std::unique_ptr<std::uint64_t, decltype(&std::free)> allocation(
        static_cast<std::uint64_t*>(std::malloc(kAllocationBytes)), &std::free);
    require(allocation != nullptr, "allocate the live 16 MiB malloc fixture");
    const std::size_t count = kAllocationBytes / sizeof(*allocation);
    std::uint64_t expectedChecksum = kChecksumSeed;
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL + 17U;
        allocation.get()[index] = value;
        expectedChecksum = (expectedChecksum ^ value) * kChecksumPrime;
    }
    require(checksum(allocation.get(), count) == expectedChecksum,
            "every live allocation page contains the deterministic fixture");
    const auto before = options.sample();
    require(before && before->memoryBytes > 0, "sample the touched live allocation");
    const auto result = options.trim();
    require(result.succeeded && result.error == 0, "relieve allocator pressure with live data");
    require(checksum(allocation.get(), count) == expectedChecksum,
            "allocator relief preserves every byte of a live allocation");
    // Unlike working-set eviction, allocator relief need not lower the footprint
    // of a live allocation. Neither footprint reduction nor reclaimed bytes is
    // deterministic across allocator versions and surrounding process activity.
    const auto after = options.sample();
    require(after && after->memoryBytes > 0, "sample the process after allocator relief");
}

void copiedOptionsRetainBackendAndRepeatedLifetimesAreSafe() {
    for (int iteration = 0; iteration < 32; ++iteration) {
        auto original = runtime::nativeMemoryOptimizationOptions();
        auto copied = original;
        original = {};
        require(copied.lowMemory && copied.sample && copied.trim,
                "copied options retain all native callbacks");
        require(copied.lowMemory().has_value(),
                "copied options keep pressure monitoring available");
        const auto sample = copied.sample();
        require(sample && sample->memoryBytes > 0,
                "copied options keep the backend alive after the original is discarded");
        if (iteration == 0) {
            const auto result = copied.trim();
            require(result.succeeded && result.error == 0,
                    "copied options retain a working allocator-relief callback");
        }
        copied = {};
        QCoreApplication::processEvents();
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    require(runtime::currentProcessOwnsForegroundWindow(),
            "missing GUI state conservatively blocks automatic reclamation");
    nativeCallbacksAndTrimAreAvailable();
    liveAllocationRemainsIntact();
    copiedOptionsRetainBackendAndRepeatedLifetimesAreSafe();
    return 0;
}
