#include "snow_capture_macos.h"
#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            return __LINE__;                                                                       \
    } while (0)
#include <stdio.h>

int main(void) {
    REQUIRE(snow_capture_macos_snapshot(NULL, NULL) == SNOW_MAC_INVALID_ARGUMENT);
    REQUIRE(snow_capture_macos_frame_retain(NULL) == NULL);
    snow_capture_macos_frame_release(NULL);
    snow_capture_macos_stream_destroy(NULL);
    SnowMacCancellation* cancellation = snow_capture_macos_cancellation_create();
    REQUIRE(cancellation != NULL);
    SnowMacCaptureConfig config = {0};
    config.struct_size = sizeof(config);
    config.cancellation = cancellation;
    config.fps = 30;
    config.timeout_ms = 100;
    snow_capture_macos_cancellation_cancel(cancellation);
    SnowMacFrame* frame = NULL;
    REQUIRE(snow_capture_macos_snapshot(&config, &frame) == SNOW_MAC_CANCELED);
    REQUIRE(frame == NULL);
    snow_capture_macos_cancellation_release(cancellation);
    printf("capture-only C ABI is usable; HDR supported=%u\n", snow_capture_macos_hdr_supported());
    return 0;
}
