#include "scan.h"

#include <cstdio>

#if defined(_WIN32)
#include <Windows.h>
#endif

int main() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    // Repeat initialization to cover both startup and reinitialization after
    // libheif has released and registered its built-in decoder again.
    for (int iteration = 0; iteration < 2; ++iteration) {
        init_scan_orders();
        for (int log2size = 2; log2size <= 5; ++log2size) {
            const int size = 1 << log2size;
            for (int scanIndex = 0; scanIndex < 3; ++scanIndex) {
                const position* subblocks = get_scan_order(log2size - 2, scanIndex);
                const position* positions = get_scan_order(2, scanIndex);
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const scan_position inverse = get_scan_position(x, y, scanIndex, log2size);
                        if (inverse.subBlock >= size * size / 16 || inverse.scanPos >= 16 ||
                            (subblocks[inverse.subBlock].x << 2) + positions[inverse.scanPos].x !=
                                x ||
                            (subblocks[inverse.subBlock].y << 2) + positions[inverse.scanPos].y !=
                                y) {
                            std::fprintf(stderr,
                                         "Invalid inverse scan: size=%d scan=%d x=%d y=%d\n", size,
                                         scanIndex, x, y);
                            return 1;
                        }
                    }
                }
            }
        }
    }
    std::puts("All libde265 inverse scan tables verified.");
    return 0;
}
