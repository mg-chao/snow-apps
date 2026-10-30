#include "screenshotpinnedshakedetector.h"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
bool trackAllocations = false;
std::size_t allocationCount = 0;
} // namespace

void* operator new(std::size_t size) {
    if (trackAllocations)
        ++allocationCount;
    if (void* result = std::malloc(std::max(std::size_t(1), size)))
        return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

int main() {
    constexpr int iterations = 2000000;
    constexpr int trials = 11;
    std::array<double, trials> timings{};
    int shakes = 0;
    // Warm up the clock before tracking the allocation-free sampling path.
    QElapsedTimer timer;
    timer.start();
    for (int trial = 0; trial < trials; ++trial) {
        ScreenshotPinnedShakeDetector detector;
        detector.begin({});
        allocationCount = 0;
        timer.restart();
        trackAllocations = true;
        for (int i = 1; i <= iterations; ++i) {
            // A mix of jitter, straight drags and rapid shakes. Reset at every new drag.
            if (i % 40 == 0)
                detector.begin({});
            const int position = ((i / 4) % 2) ? 64 : 0;
            shakes += detector.sample({qreal(position), qreal(i % 3)}, i % 40 * 8) ? 1 : 0;
        }
        trackAllocations = false;
        timings[trial] = double(timer.nsecsElapsed()) / iterations;
        if (allocationCount != 0) {
            std::cerr << "Detector sampling allocated " << allocationCount << " times\n";
            return 1;
        }
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "Detector state bytes: " << sizeof(ScreenshotPinnedShakeDetector)
              << "\nSamples per trial: " << iterations
              << "\nMedian ns/sample: " << timings[trials / 2]
              << "\nWorst trial ns/sample: " << timings.back()
              << "\nSampling C++ allocations: 0\nRecognized shakes: " << shakes << '\n';
    return shakes > 0 ? 0 : 1;
}
