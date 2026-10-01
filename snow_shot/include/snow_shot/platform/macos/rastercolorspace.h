#ifndef SNOW_SHOT_PLATFORM_MACOS_RASTERCOLORSPACE_H
#define SNOW_SHOT_PLATFORM_MACOS_RASTERCOLORSPACE_H

#include <QColorSpace>
#include <QSurfaceFormat>

namespace snow_shot::platform::macos {
inline void configureRasterColorSpace() {
    // Cocoa otherwise tags Qt raster backing stores with the display's ICC profile.
    // QPainter copies sRGB screenshot pixels without a color-space conversion, so
    // every raster window must declare the same working space before it is created.
    auto format = QSurfaceFormat::defaultFormat();
    format.setColorSpace(QColorSpace::SRgb);
    QSurfaceFormat::setDefaultFormat(format);
}
} // namespace snow_shot::platform::macos

#endif
