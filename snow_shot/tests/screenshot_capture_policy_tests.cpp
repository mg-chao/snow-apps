#include "screenshotcapturepolicy_p.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
using snow_shot::presentation::capture::detail::displayHdrEnabled;

DISPLAYCONFIG_PATH_TARGET_INFO target{};
DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 modernInfo{};
DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacyInfo{};
LONG modernStatus = ERROR_SUCCESS;
LONG legacyStatus = ERROR_SUCCESS;
int modernQueries = 0;
int legacyQueries = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

LONG WINAPI queryColorInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* header) {
    require(header->adapterId.LowPart == target.adapterId.LowPart &&
                header->adapterId.HighPart == target.adapterId.HighPart && header->id == target.id,
            "color queries must use the active display target's adapter and ID");
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2) {
        require(header->size == sizeof(modernInfo), "incorrect modern color query size");
        ++modernQueries;
        *reinterpret_cast<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2*>(header) = modernInfo;
        return modernStatus;
    }
    require(header->type == DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO &&
                header->size == sizeof(legacyInfo),
            "incorrect legacy color query type or size");
    ++legacyQueries;
    *reinterpret_cast<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO*>(header) = legacyInfo;
    return legacyStatus;
}

void reset() {
    target = {};
    target.adapterId.LowPart = 123;
    target.adapterId.HighPart = 456;
    target.id = 7;
    modernInfo = {};
    legacyInfo = {};
    modernStatus = ERROR_SUCCESS;
    legacyStatus = ERROR_SUCCESS;
    modernQueries = 0;
    legacyQueries = 0;
}

void wideColorWithoutHdrUsesGdi() {
    reset();
    // GX321UR with HDR off and automatic color management on: both legacy flags are true.
    modernInfo.activeColorMode = DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG;
    modernInfo.advancedColorSupported = 1;
    modernInfo.advancedColorActive = 1;
    modernInfo.highDynamicRangeSupported = 1;
    modernInfo.wideColorSupported = 1;
    modernInfo.wideColorUserEnabled = 1;
    legacyInfo.advancedColorSupported = 1;
    legacyInfo.advancedColorEnabled = 1;
    legacyInfo.wideColorEnforced = 1;
    require(!displayHdrEnabled(target, queryColorInfo),
            "WCG with automatic color management must not select HDR capture");
    require(modernQueries == 1 && legacyQueries == 0,
            "a successful active color mode query must be authoritative");
}

void onlyActiveHdrSelectsDxgi() {
    for (const auto mode :
         {DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR, DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG,
          DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR}) {
        for (const bool userEnabled : {false, true}) {
            reset();
            modernInfo.activeColorMode = mode;
            modernInfo.highDynamicRangeSupported = 1;
            modernInfo.highDynamicRangeUserEnabled = userEnabled;
            // Do not let legacy flags override a successful active-mode query.
            legacyInfo.advancedColorSupported = 1;
            legacyInfo.advancedColorEnabled = 1;
            require(displayHdrEnabled(target, queryColorInfo) ==
                        (mode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR),
                    "HDR capture must follow the active mode, not capability or user preference");
            require(modernQueries == 1 && legacyQueries == 0,
                    "a successful active color mode query must not use legacy flags");
        }
    }
}

void legacyQueriesDistinguishHdrFromWideColor() {
    for (const LONG status : {ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED}) {
        for (const bool supported : {false, true}) {
            for (const bool enabled : {false, true}) {
                for (const bool wideColor : {false, true}) {
                    reset();
                    modernStatus = status;
                    modernInfo.activeColorMode = DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
                    legacyInfo.advancedColorSupported = supported;
                    legacyInfo.advancedColorEnabled = enabled;
                    legacyInfo.wideColorEnforced = wideColor;
                    require(displayHdrEnabled(target, queryColorInfo) ==
                                (supported && enabled && !wideColor),
                            "legacy query must require enabled HDR and exclude WCG");
                    require(modernQueries == 1 && legacyQueries == 1,
                            "unavailable modern query must fall back to the same display target");
                }
            }
        }
    }
}

void failedQueriesDoNotReportHdr() {
    reset();
    modernStatus = ERROR_NOT_SUPPORTED;
    legacyStatus = ERROR_GEN_FAILURE;
    modernInfo.activeColorMode = DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
    legacyInfo.advancedColorSupported = 1;
    legacyInfo.advancedColorEnabled = 1;
    require(!displayHdrEnabled(target, queryColorInfo),
            "failed queries must not use untrusted HDR output fields");
}
} // namespace

int main() {
    wideColorWithoutHdrUsesGdi();
    onlyActiveHdrSelectsDxgi();
    legacyQueriesDistinguishHdrFromWideColor();
    failedQueriesDoNotReportHdr();
    return 0;
}
