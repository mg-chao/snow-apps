#ifndef SNOW_SHOT_PRESENTATION_DIRECTCAPTUREHISTORY_H
#define SNOW_SHOT_PRESENTATION_DIRECTCAPTUREHISTORY_H

#include "snow_shot/presentation/directcaptureworkflow.h"
#include "snow_shot/storage/capturehistorytypes.h"

namespace snow_shot::presentation {
[[nodiscard]] storage::CaptureHistoryDraft
directCaptureHistoryDraft(const DirectCaptureRequest& request, const DirectCaptureFrame& frame,
                          std::optional<storage::PreparedPngImage> png = {});
}
#endif
