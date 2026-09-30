#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDPLACEMENTGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDPLACEMENTGEOMETRY_H
#include "snow_shot/presentation/screenshotclipboardplacement.h"
#include "snow_shot/presentation/pinnedgeometry.h"
#include <QGuiApplication>

inline QList<QScreen*> screenshotClipboardScreens() {
    auto screens = QGuiApplication::screens();
    const auto index = screens.indexOf(QGuiApplication::primaryScreen());
    if (index > 0)
        screens.move(index, 0);
    return screens;
}
inline QList<ScreenshotClipboardDisplay> screenshotClipboardDisplays() {
    QList<ScreenshotClipboardDisplay> result;
    for (const auto* screen : screenshotClipboardScreens())
        result.append({screen->name(), screen->serialNumber(), screen->geometry(),
                       snow_shot::presentation::pinnedScreenGeometry(*screen),
                       screen->availableGeometry(),
                       std::max(qreal(1), screen->devicePixelRatio())});
    return result;
}
struct ScreenshotClipboardPinGeometry {
    QScreen* screen = nullptr;
    ScreenshotPinnedImageFit fit;
};
inline ScreenshotClipboardPinGeometry
screenshotClipboardPinGeometry(const std::optional<ScreenshotClipboardPlacement>& placement,
                               QSize rasterSize, QSize defaultWindowSize, QScreen* fallback,
                               bool autoResizeWindow) {
    const auto screens = screenshotClipboardScreens();
    if (placement && placement->rasterSize == rasterSize) {
        const auto resolved = resolveScreenshotClipboardPlacement(
            *placement, screenshotClipboardDisplays(), autoResizeWindow);
        if (resolved.isValid() && resolved.displayIndex < screens.size())
            return {screens[resolved.displayIndex],
                    {resolved.windowRect, resolved.initialWindowSize, 100.0, true}};
    }
    if (!fallback)
        fallback = QGuiApplication::primaryScreen();
    return {fallback, fallback ? snow_shot::presentation::fitPinnedImageOnScreen(
                                     *fallback, defaultWindowSize, autoResizeWindow)
                               : ScreenshotPinnedImageFit{}};
}
inline std::optional<ScreenshotClipboardPlacement>
screenshotClipboardSelectionPlacement(const QRect& windowRect, const QSize& initialSize,
                                      QScreen* screen) {
    if (!screen || windowRect.isEmpty())
        return {};
    ScreenshotClipboardPlacement result;
    result.displays = screenshotClipboardDisplays();
    result.anchorDisplayIndex = screenshotClipboardScreens().indexOf(screen);
    const qreal scale = snow_shot::storage::pinnedGeometryScale(screen->devicePixelRatio());
    result.placement = {
        screen->name(), screen->serialNumber(),
        (QPointF(windowRect.topLeft()) -
         QPointF(snow_shot::presentation::pinnedScreenGeometry(*screen).topLeft())) /
            scale,
        initialSize};
    result.windowRect = windowRect;
    // Replaced with the actual export dimensions before publishing.
    result.rasterSize = initialSize;
    return result.isValid() ? std::optional(result) : std::nullopt;
}
#endif
