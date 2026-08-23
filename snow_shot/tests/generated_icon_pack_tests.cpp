#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"

#include "icon_registry.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QSet>

#include <cstdlib>
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

bool containsOpaqueColor(const QImage& image, const QColor& expected) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor actual = image.pixelColor(x, y);
            if (actual.alpha() == 255 && actual.rgb() == expected.rgb()) {
                return true;
            }
        }
    }
    return false;
}

bool containsOpaqueColorInRect(const QImage& image, const QColor& expected, const QRect& bounds) {
    const QRect clippedBounds = bounds.intersected(image.rect());
    for (int y = clippedBounds.top(); y <= clippedBounds.bottom(); ++y) {
        for (int x = clippedBounds.left(); x <= clippedBounds.right(); ++x) {
            const QColor actual = image.pixelColor(x, y);
            if (actual.alpha() == 255 && actual.rgb() == expected.rgb()) {
                return true;
            }
        }
    }
    return false;
}

QPixmap render(const adqt::icons::IconRef& ref, const QSize& size, qreal dpr = 1.0) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = size;
    request.devicePixelRatio = dpr;
    return adqt::icons::renderIconPixmap(ref, request);
}

void everySnowShotEntryRenders() {
    namespace icons = snow_shot::presentation::icons::custom;
    adqt::icons::IconRegistry registry;
    const auto registered = icons::registerWith(registry);
    require(registered.ok(), "Snow Shot pack registration should succeed");
    const adqt::icons::IconPack* staticPack = icons::pack().staticPack();
    require(staticPack != nullptr && staticPack->entryCount == 76,
            "Snow Shot pack should contain all 76 project-owned assets");

    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(32, 32);
    request.devicePixelRatio = 1.25;
    for (std::size_t index = 0; index < staticPack->entryCount; ++index) {
        const auto ref = icons::pack().icon(index);
        require(ref.isValid(), "every Snow Shot pack entry should create a reference");
        const QPixmap pixmap = registry.renderIconPixmap(ref, request);
        require(!pixmap.isNull() && pixmap.size() == QSize(40, 40) &&
                    qFuzzyCompare(pixmap.devicePixelRatio(), 1.25),
                "every Snow Shot pack entry should render at fractional DPR");
        require(!alphaBounds(pixmap.toImage()).isEmpty(),
                "every Snow Shot pack entry should have nonblank alpha bounds");
    }
}

void projectIconColorsAndModelsArePreserved() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QColor primary(0, 166, 90);
    const QColor brandPurple(0x92, 0x54, 0xde);
    const auto logoRef = icons::brand::SnowShotLogo(adqt::icons::IconColors::primary(primary));
    const auto logoMetadata = adqt::icons::describeIcon(logoRef);
    require(logoMetadata.key.pack == QStringLiteral("snow-shot") &&
                logoMetadata.key.variant == QStringLiteral("brand") &&
                logoMetadata.colorModel == adqt::icons::IconColorModel::Monochrome,
            "Snow Shot logo should be a project-owned hybrid monochrome reference");
    const QImage logo = render(logoRef, QSize(190, 34)).toImage();
    require(containsOpaqueColor(logo, brandPurple) && containsOpaqueColor(logo, primary),
            "Snow Shot logo should preserve its fixed purple mark and themed text slot");

    const auto opacityRef = icons::outlined::Opacity(adqt::icons::IconColors::primary(primary));
    const auto opacityMetadata = adqt::icons::describeIcon(opacityRef);
    const QImage opacity = render(opacityRef, QSize(32, 32)).toImage();
    require(opacityMetadata.key.pack == QStringLiteral("snow-shot") &&
                opacityMetadata.key.name == QStringLiteral("opacity") &&
                containsOpaqueColor(opacity, primary),
            "opacity should render from the Snow Shot pack with its primary slot");

    const QImage screenshot = snow_shot::presentation::icons::renderTintedIconPixmap(
                                  icons::twotone::ScreenshotFeature(), QSize(32, 32), 1.0, primary)
                                  .toImage();
    const QRect upperRight(screenshot.width() / 2, 0, screenshot.width() - screenshot.width() / 2,
                           screenshot.height() / 2);
    require(containsOpaqueColor(screenshot, primary) &&
                containsOpaqueColorInRect(screenshot, brandPurple, upperRight),
            "screenshot feature should preserve its upper-right purple accent when tinted");

    const auto appRef = icons::app::ApplicationIcon();
    const auto appMetadata = adqt::icons::describeIcon(appRef);
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
    require(appMetadata.colorModel == adqt::icons::IconColorModel::FullColor &&
                opaqueColors.size() > 4,
            "Snow Shot application icon should preserve full-color source pixels");
}

void ocrTranslateIconUsesTheSuppliedProjectAsset() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QColor primary(0, 166, 90);
    const auto translateRef =
        icons::outlined::OcrTranslate(adqt::icons::IconColors::primary(primary));
    const auto translateMetadata = adqt::icons::describeIcon(translateRef);
    const QImage translate = render(translateRef, QSize(32, 32)).toImage();
    require(translateMetadata.key.pack == QStringLiteral("snow-shot") &&
                translateMetadata.key.name == QStringLiteral("ocr-translate") &&
                containsOpaqueColor(translate, primary) && !alphaBounds(translate).isEmpty(),
            "OCR Translate should render the supplied project asset with its primary color");
}

void pinToScreenIconKeepsItsIntendedFootprint() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QImage image = render(icons::outlined::PinToScreen(), QSize(32, 32)).toImage();
    const QRect bounds = alphaBounds(image);
    const int rightMargin = image.width() - bounds.right() - 1;
    const int bottomMargin = image.height() - bounds.bottom() - 1;

    require(bounds.width() <= 26 && bounds.height() <= 26,
            "pin-to-screen should not be enlarged within its icon canvas");
    require(std::abs(bounds.left() - rightMargin) <= 1 &&
                std::abs(bounds.top() - bottomMargin) <= 1,
            "pin-to-screen should remain centered within its icon canvas");
}

void flipVerticalIconUsesTheRotatedProjectAsset() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto ref = icons::outlined::FlipVertical();
    const auto metadata = adqt::icons::describeIcon(ref);
    const QRect bounds = alphaBounds(render(ref, QSize(64, 64)).toImage());

    require(metadata.key.pack == QStringLiteral("snow-shot") &&
                metadata.key.name == QStringLiteral("flip-vertical") &&
                bounds.height() > bounds.width(),
            "flip-vertical should use the rotated Snow Shot project asset");
}

void scrollingIconsUseTheRequestedOrientations() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QColor tint(0, 166, 90);
    const auto colors = adqt::icons::IconColors::primary(tint);
    const auto horizontalRef = icons::outlined::ScrollingHorizontal(colors);
    const auto verticalRef = icons::outlined::ScrollingVertical(colors);
    const auto horizontalMetadata = adqt::icons::describeIcon(horizontalRef);
    const auto verticalMetadata = adqt::icons::describeIcon(verticalRef);
    const QImage horizontal = render(horizontalRef, QSize(64, 64)).toImage();
    const QImage vertical = render(verticalRef, QSize(64, 64)).toImage();
    const QRect horizontalBounds = alphaBounds(horizontal);
    const QRect verticalBounds = alphaBounds(vertical);

    require(horizontalMetadata.key.name == QStringLiteral("scrolling-horizontal") &&
                verticalMetadata.key.name == QStringLiteral("scrolling-vertical"),
            "scrolling mode icons should resolve to their project assets");
    require(std::abs(horizontalBounds.width() - verticalBounds.height()) <= 1 &&
                std::abs(horizontalBounds.height() - verticalBounds.width()) <= 1,
            "vertical scrolling should be the horizontal asset rotated by 90 degrees");
    require(containsOpaqueColor(horizontal, tint) && containsOpaqueColor(vertical, tint),
            "scrolling mode icons should inherit their requested primary color");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--ocr-translate-only"))) {
            ocrTranslateIconUsesTheSuppliedProjectAsset();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--all-entries-only"))) {
            everySnowShotEntryRenders();
            return 0;
        }
        everySnowShotEntryRenders();
        projectIconColorsAndModelsArePreserved();
        ocrTranslateIconUsesTheSuppliedProjectAsset();
        scrollingIconsUseTheRequestedOrientations();
        pinToScreenIconKeepsItsIntendedFootprint();
        flipVerticalIconUsesTheRotatedProjectAsset();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
