#include "snow_recording_macos.h"
#include "snow_recording.h"
#define REQUIRE(condition) do { if (!(condition)) return __LINE__; } while (0)
#include <stddef.h>
int main(void) {
    REQUIRE(snow_recording_macos_create(NULL, NULL) == SNOW_MAC_RECORDING_INVALID_ARGUMENT);
    REQUIRE(snow_recording_macos_pause(NULL, 0) == SNOW_MAC_RECORDING_INVALID_ARGUMENT);
    REQUIRE(snow_recording_macos_enumerate_inputs(NULL, NULL) == SNOW_MAC_RECORDING_INVALID_ARGUMENT);
    REQUIRE(snow_recording_macos_microphone_permission_request(NULL, NULL) == SNOW_MAC_RECORDING_INVALID_ARGUMENT);
    SnowMacRecordingCancellation* cancellation = snow_recording_macos_cancellation_create();
    REQUIRE(cancellation != NULL);
    SnowMacRecordingConfig config = {0};
    config.struct_size = sizeof(config);
    config.cancellation = cancellation;
    config.fps = 30;
    config.output_width = 64;
    config.output_height = 64;
    config.output_path = "/nonexistent/canceled-recording.mp4";
    snow_recording_macos_cancellation_cancel(cancellation);
    SnowMacRecording* recording = NULL;
    REQUIRE(snow_recording_macos_create(&config, &recording) == SNOW_MAC_RECORDING_CANCELED);
    REQUIRE(recording == NULL);
    snow_recording_macos_cancellation_release(cancellation);
    snow_recording_macos_destroy(NULL);
    REQUIRE(snow_recording_last_error_message() != NULL);
    return 0;
}
