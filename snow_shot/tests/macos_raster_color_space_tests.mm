#include "snow_shot/platform/macos/rastercolorspace.h"

#include <QApplication>
#include <QColorSpace>
#include <QImage>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void rasterPresentationRetainsCaptureColors() {
    const QColorSpace srgb(QColorSpace::SRgb);
    QImage capture(4, 1, QImage::Format_ARGB32_Premultiplied);
    capture.setColorSpace(srgb);
    const QColor colors[] = {QColor(200, 100, 50), QColor(40, 180, 110), QColor(60, 80, 220),
                             QColor(128, 128, 128)};
    for (int x = 0; x < capture.width(); ++x)
        capture.setPixelColor(x, 0, colors[x]);

    // Model Cocoa's interpretation of raster bytes using the declared surface
    // profile. QPainter itself does not convert tagged source images.
    for (int cycle = 0; cycle < 4; ++cycle) {
        QImage surface(capture.size(), capture.format());
        surface.setColorSpace(QSurfaceFormat::defaultFormat().colorSpace());
        surface.fill(Qt::transparent);
        {
            QPainter painter(&surface);
            painter.drawImage(0, 0, capture);
        }
        capture = surface.convertedToColorSpace(srgb);
        require(!capture.isNull(), "raster surface has no usable color profile");
        for (int x = 0; x < capture.width(); ++x)
            require(capture.pixelColor(x, 0) == colors[x],
                    "presenting and recapturing an sRGB raster introduced a color cast");
    }
}

void nativeSurfacesUseSrgb() {
    const QColorSpace srgb(QColorSpace::SRgb);
    for (const auto flags : {Qt::Window, Qt::Tool, Qt::Popup}) {
        for (const bool translucent : {false, true}) {
            QWidget widget(nullptr, flags | Qt::FramelessWindowHint);
            widget.setAttribute(Qt::WA_TranslucentBackground, translucent);
            for (int generation = 0; generation < 2; ++generation) {
                auto* view = reinterpret_cast<NSView*>(widget.winId());
                // This is the profile Qt 6.11.1's Cocoa backing store reads from
                // QNSView, rather than NSWindow's monitor-dependent color space.
                NSColorSpace* nativeSpace = [view valueForKey:@"colorSpace"];
                const auto profile =
                    QColorSpace::fromIccProfile(QByteArray::fromNSData(nativeSpace.ICCProfileData));
                require(profile == srgb,
                        "Cocoa raster surface uses the monitor profile instead of sRGB");
                require(widget.windowHandle()->format().colorSpace() == srgb,
                        "native window did not inherit the raster working space");
                widget.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    auto initialFormat = QSurfaceFormat::defaultFormat();
    initialFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(initialFormat);
    snow_shot::platform::macos::configureRasterColorSpace();
    require(QSurfaceFormat::defaultFormat().swapBehavior() == initialFormat.swapBehavior(),
            "configuring raster color space changed unrelated surface options");
    QApplication app(argc, argv);
    rasterPresentationRetainsCaptureColors();
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        nativeSurfacesUseSrgb();
    std::cout << "macOS raster color space tests passed\n";
    return EXIT_SUCCESS;
}
