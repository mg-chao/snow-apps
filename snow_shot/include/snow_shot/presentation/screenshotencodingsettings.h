#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTENCODINGSETTINGS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTENCODINGSETTINGS_H

#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/storage/settingsadapters.h"

namespace snow_shot::presentation {
// Snapshot at the output request boundary; asynchronous exports own this value.
[[nodiscard]] inline ScreenshotImageEncodingOptions
screenshotEncodingOptions(const storage::ScreenshotSettings& settings) {
    return {settings.imageQuality(),
            ScreenshotImageFileService::compressionLevelForKey(settings.compressionLevel())};
}
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTENCODINGSETTINGS_H
