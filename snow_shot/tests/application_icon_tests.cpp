#include "snow_shot/presentation/components/icons/snowshoticons.h"

#include "icon_renderer.h"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPixmap>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool containsGreenNearBorder(const QImage& image) {
    const int border = std::max(1, image.width() / 5);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (x >= border && x < image.width() - border && y >= border &&
                y < image.height() - border) {
                continue;
            }
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() >= 180 && color.green() > color.red() + 25 &&
                color.green() > color.blue() + 50) {
                return true;
            }
        }
    }
    return false;
}

void applicationIconScalesWithoutEmbeddedRasterImages() {
    const auto icon = snow_shot::presentation::icons::custom::app::ApplicationIcon();
    require(icon.isValid(), "application icon must have a valid descriptor");
    const auto svg = icon.descriptor()->svg;
    require(svg.find("<image") == std::string_view::npos &&
                svg.find("data:image") == std::string_view::npos,
            "the small caption icon must rasterize vector paths directly at the target DPI");

    for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
        adqt::icons::IconRenderRequest request;
        request.logicalSize = QSize(16, 16);
        request.devicePixelRatio = scale;
        const QPixmap pixmap = adqt::icons::renderIconPixmap(icon, request);
        require(pixmap.size() == QSize(qRound(16 * scale), qRound(16 * scale)) &&
                    pixmap.devicePixelRatio() == scale,
                "caption icon must provide physical pixels for the window's display scale");
        const QImage image = pixmap.toImage();
        require(containsGreenNearBorder(image), "caption icon must retain its green scan frame");
        bool containsPurple = false;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                containsPurple |= color.alpha() >= 180 && color.red() > color.green() + 25 &&
                                  color.blue() > color.red() + 25;
            }
        }
        require(containsPurple, "caption icon must retain its purple lightning mark");
    }
}

void installedApplicationIconPreservesItsGreenTaskbarBorder() {
    namespace icons = snow_shot::presentation::icons::custom;
    QApplication::setWindowIcon(adqt::icons::makeIcon(icons::app::ApplicationIcon()));
    const QIcon installedIcon = QApplication::windowIcon();
    require(!installedIcon.isNull(), "QApplication did not retain the Snow Shot icon");

    for (const int size : {16, 20, 24, 32, 40, 48, 64}) {
        const QPixmap pixmap = installedIcon.pixmap(QSize(size, size));
        require(!pixmap.isNull() && pixmap.size() == QSize(size, size),
                "application icon should provide every taskbar raster size");
        require(containsGreenNearBorder(pixmap.toImage()),
                "application icon should preserve its green border at taskbar sizes");
    }
}

#ifdef Q_OS_WIN
void executableIconResourcePreservesItsGreenTaskbarBorder() {
    HMODULE module = GetModuleHandleW(nullptr);
    require(module != nullptr, "application icon test could not resolve its module");
    for (const int size : {16, 32, 48}) {
        HICON icon = static_cast<HICON>(
            LoadImageW(module, MAKEINTRESOURCEW(101), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        require(icon != nullptr, "Snow Shot executable did not contain its application icon");
        const QImage image = QImage::fromHICON(icon);
        DestroyIcon(icon);
        require(!image.isNull() && containsGreenNearBorder(image),
                "embedded Snow Shot application icon should preserve its green border");
    }
}
#endif

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        applicationIconScalesWithoutEmbeddedRasterImages();
        installedApplicationIconPreservesItsGreenTaskbarBorder();
#ifdef Q_OS_WIN
        executableIconResourcePreservesItsGreenTaskbarBorder();
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
