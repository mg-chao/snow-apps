#include "ui/app_icons.h"

#include "icon_registry.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QSet>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QRect alphaBounds(const QImage& image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                bounds |= QRect(x, y, 1, 1);
            }
        }
    }
    return bounds;
}

QPixmap render(const adqt::icons::IconRef& ref, const QSize& size, qreal dpr = 1.0) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = size;
    request.devicePixelRatio = dpr;
    return adqt::icons::renderIconPixmap(ref, request);
}

void viewerPackRendersAndPreservesColorModels() {
    namespace icons = snow::image_viewer::icons;
    adqt::icons::IconRegistry registry;
    const auto registered = icons::registerWith(registry);
    const adqt::icons::IconPack* staticPack = icons::pack().staticPack();
    require(registered.ok() && staticPack != nullptr && staticPack->entryCount == 2,
            "viewer pack should register its two project-owned assets");

    for (std::size_t index = 0; index < staticPack->entryCount; ++index) {
        const auto ref = icons::pack().icon(index);
        adqt::icons::IconRenderRequest request;
        request.logicalSize = QSize(32, 32);
        request.devicePixelRatio = 1.5;
        const QPixmap pixmap = registry.renderIconPixmap(ref, request);
        require(ref.isValid() && !pixmap.isNull() && pixmap.size() == QSize(48, 48) &&
                    !alphaBounds(pixmap.toImage()).isEmpty(),
                "every viewer pack entry should render at fractional DPR");
    }

    const QColor primary(12, 132, 96);
    const auto resizeRef = icons::outlined::ResizeImage(adqt::icons::IconColors::primary(primary));
    const QImage resize = render(resizeRef, QSize(32, 32)).toImage();
    bool hasPrimary = false;
    for (int y = 0; y < resize.height(); ++y) {
        for (int x = 0; x < resize.width(); ++x) {
            const QColor color = resize.pixelColor(x, y);
            hasPrimary = hasPrimary || (color.alpha() == 255 && color.rgb() == primary.rgb());
        }
    }
    require(adqt::icons::describeIcon(resizeRef).colorModel ==
                    adqt::icons::IconColorModel::Monochrome &&
                hasPrimary,
            "resize-image should use the viewer pack primary color slot");

    const auto appRef = icons::app::ApplicationIcon();
    const auto metadata = adqt::icons::describeIcon(appRef);
    const QImage appIcon = render(appRef, QSize(64, 64)).toImage();
    QSet<QRgb> opaqueColors;
    for (int y = 0; y < appIcon.height(); ++y) {
        for (int x = 0; x < appIcon.width(); ++x) {
            const QColor color = appIcon.pixelColor(x, y);
            if (color.alpha() == 255) {
                opaqueColors.insert(color.rgb());
            }
        }
    }
    require(metadata.key.pack == QStringLiteral("snow-image-viewer") &&
                metadata.colorModel == adqt::icons::IconColorModel::FullColor &&
                opaqueColors.size() > 4,
            "viewer application icon should originate from its full-color generated reference");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        viewerPackRendersAndPreservesColorModels();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
