#include "snow_shot/presentation/capture/screenshotcapturepolicy.h"

#include <QGuiApplication>
#include <QVector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
#endif

#include "snow_capture.h"

namespace snow_shot::presentation::capture {

ScreenshotApiMode screenshotApiModeFromValue(const char* value) noexcept {
    if (value != nullptr) {
        const QString mode = QString::fromLatin1(value);
        if (mode == QStringLiteral("dxgi")) return ScreenshotApiMode::Dxgi;
        if (mode == QStringLiteral("wgc")) return ScreenshotApiMode::Wgc;
        if (mode == QStringLiteral("gdi")) return ScreenshotApiMode::Gdi;
    }
    return ScreenshotApiMode::Auto;
}

std::uint8_t nativeBackendForNormalScreenshot(ScreenshotApiMode mode) noexcept {
    switch (mode) {
    case ScreenshotApiMode::Dxgi: return SNOW_CAPTURE_BACKEND_DXGI;
    case ScreenshotApiMode::Wgc: return SNOW_CAPTURE_BACKEND_WGC;
    case ScreenshotApiMode::Gdi: return SNOW_CAPTURE_BACKEND_GDI;
    case ScreenshotApiMode::Auto: break;
    }
    return SNOW_CAPTURE_BACKEND_AUTO;
}

ScreenshotApiMode resolveAutoScreenshotApiMode() noexcept {
    static const ScreenshotApiMode resolved = [] {
#if defined(Q_OS_WIN) || defined(_WIN32)
        UINT pathCount = 0;
        UINT modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
            QVector<DISPLAYCONFIG_PATH_INFO> paths(static_cast<qsizetype>(pathCount));
            QVector<DISPLAYCONFIG_MODE_INFO> modes(static_cast<qsizetype>(modeCount));
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                   modes.data(), nullptr) == ERROR_SUCCESS) {
                for (UINT index = 0; index < pathCount; ++index) {
                    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
                    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                    info.header.size = sizeof(info);
                    info.header.adapterId = paths[index].targetInfo.adapterId;
                    info.header.id = paths[index].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS &&
                        info.advancedColorSupported && info.advancedColorEnabled) {
                        return ScreenshotApiMode::Dxgi;
                    }
                }
            }
        }
        return ScreenshotApiMode::Gdi;
#else
        return ScreenshotApiMode::Auto;
#endif
    }();
    return resolved;
}

} // namespace snow_shot::presentation::capture
