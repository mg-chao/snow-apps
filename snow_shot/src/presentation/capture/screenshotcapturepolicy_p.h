#ifndef SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTCAPTUREPOLICY_P_H
#define SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTCAPTUREPOLICY_P_H

#include <windows.h>

namespace snow_shot::presentation::capture::detail {

using DisplayColorInfoQuery = LONG(WINAPI*)(DISPLAYCONFIG_DEVICE_INFO_HEADER*);

[[nodiscard]] bool
displayHdrEnabled(const DISPLAYCONFIG_PATH_TARGET_INFO& target,
                  DisplayColorInfoQuery query = DisplayConfigGetDeviceInfo) noexcept;

} // namespace snow_shot::presentation::capture::detail

#endif
