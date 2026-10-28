#include "snow_shot/presentation/capture/screenshotcapturepolicy.h"

#include <QGuiApplication>
#include <QVector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include "screenshotcapturepolicy_p.h"
#endif

#include "snow_capture.h"

namespace snow_shot::presentation::capture {

#if defined(Q_OS_WIN) || defined(_WIN32)
bool detail::displayHdrEnabled(const DISPLAYCONFIG_PATH_TARGET_INFO& target,
                               DisplayColorInfoQuery query) noexcept {
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 current{};
    current.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    current.header.size = sizeof(current);
    current.header.adapterId = target.adapterId;
    current.header.id = target.id;
    if (query(&current.header) == ERROR_SUCCESS) {
        // Advanced Color also covers SDR automatic color management. Only active HDR needs DXGI.
        return current.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
    }

    // Older Windows versions expose only the legacy flags; WCG is not HDR there either.
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info.header.size = sizeof(info);
    info.header.adapterId = target.adapterId;
    info.header.id = target.id;
    return query(&info.header) == ERROR_SUCCESS && info.advancedColorSupported &&
           info.advancedColorEnabled && !info.wideColorEnforced;
}
#endif

ScreenshotApiMode screenshotApiModeFromValue(const char* value) noexcept {
    if (value != nullptr) {
        const QString mode = QString::fromLatin1(value);
        if (mode == QStringLiteral("dxgi"))
            return ScreenshotApiMode::Dxgi;
        if (mode == QStringLiteral("wgc"))
            return ScreenshotApiMode::Wgc;
        if (mode == QStringLiteral("gdi"))
            return ScreenshotApiMode::Gdi;
    }
    return ScreenshotApiMode::Auto;
}

std::uint8_t nativeBackendForNormalScreenshot(ScreenshotApiMode mode) noexcept {
#ifdef Q_OS_MACOS
    Q_UNUSED(mode);
    return SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT;
#else
    switch (mode) {
    case ScreenshotApiMode::Dxgi:
        return SNOW_CAPTURE_BACKEND_DXGI;
    case ScreenshotApiMode::Wgc:
        return SNOW_CAPTURE_BACKEND_WGC;
    case ScreenshotApiMode::Gdi:
        return SNOW_CAPTURE_BACKEND_GDI;
    case ScreenshotApiMode::Auto:
        break;
    }
    return SNOW_CAPTURE_BACKEND_AUTO;
#endif
}

ScreenshotApiMode resolveAutoScreenshotApiMode() noexcept {
    static const ScreenshotApiMode resolved = [] {
#if defined(Q_OS_WIN) || defined(_WIN32)
        UINT pathCount = 0;
        UINT modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) ==
            ERROR_SUCCESS) {
            QVector<DISPLAYCONFIG_PATH_INFO> paths(static_cast<qsizetype>(pathCount));
            QVector<DISPLAYCONFIG_MODE_INFO> modes(static_cast<qsizetype>(modeCount));
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                   modes.data(), nullptr) == ERROR_SUCCESS) {
                for (UINT index = 0; index < pathCount; ++index) {
                    if (detail::displayHdrEnabled(paths[index].targetInfo)) {
                        return ScreenshotApiMode::Dxgi;
                    }
                }
            }
        }
#endif
#ifdef Q_OS_MACOS
        return ScreenshotApiMode::Auto;
#else
        return ScreenshotApiMode::Gdi;
#endif
    }();
    return resolved;
}

} // namespace snow_shot::presentation::capture
