#include "snow_shot/presentation/components/icons/iconrenderutils.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"

#include "icon_renderer.h"
#include "antd_icons.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QSet>
#include <QStringList>
#include <QXmlStreamReader>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

QStringList renderWarnings;
QtMessageHandler previousMessageHandler = nullptr;

void captureRenderWarnings(QtMsgType type, const QMessageLogContext& context,
                           const QString& message) {
    if (type == QtWarningMsg || type == QtCriticalMsg)
        renderWarnings.append(message);
    if (previousMessageHandler != nullptr)
        previousMessageHandler(type, context, message);
}

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

int opaquePixelCount(const QImage& image, const QRect& bounds) {
    int count = 0;
    const QRect clippedBounds = bounds.intersected(image.rect());
    for (int y = clippedBounds.top(); y <= clippedBounds.bottom(); ++y) {
        for (int x = clippedBounds.left(); x <= clippedBounds.right(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++count;
            }
        }
    }
    return count;
}

QPixmap render(const adqt::icons::IconRef& ref, const QSize& size, qreal dpr = 1.0) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = size;
    request.devicePixelRatio = dpr;
    return adqt::icons::renderIconPixmap(ref, request);
}

void everySnowShotEntryRenders() {
    namespace icons = snow_shot::presentation::icons::custom;
    adqt::icons::IconRenderer renderer;
    const auto registered = icons::registerWith(renderer);
    require(registered.ok(), "Snow Shot pack registration should succeed");
    const adqt::icons::IconPack* staticPack = icons::pack().staticPack();
    require(staticPack != nullptr && staticPack->entryCount == 201,
            "Snow Shot pack should contain all 201 project-owned assets");

    adqt::icons::IconRenderRequest request;
    request.logicalSize = QSize(32, 32);
    request.devicePixelRatio = 1.25;
    renderWarnings.clear();
    struct MessageCapture {
        MessageCapture() {
            previousMessageHandler = qInstallMessageHandler(captureRenderWarnings);
        }
        ~MessageCapture() {
            qInstallMessageHandler(previousMessageHandler);
        }
    } capture;
    for (std::size_t index = 0; index < staticPack->entryCount; ++index) {
        const auto ref = icons::pack().icon(index);
        require(ref.isValid(), "every Snow Shot pack entry should create a reference");
        const QPixmap pixmap = renderer.renderIconPixmap(ref, request);
        require(!pixmap.isNull() && pixmap.size() == QSize(40, 40) &&
                    qFuzzyCompare(pixmap.devicePixelRatio(), 1.25),
                "every Snow Shot pack entry should render at fractional DPR");
        require(!alphaBounds(pixmap.toImage()).isEmpty(),
                "every Snow Shot pack entry should have nonblank alpha bounds");
    }
    for (const auto& warning : renderWarnings)
        std::cerr << warning.toStdString() << '\n';
    require(renderWarnings.isEmpty(),
            "every Snow Shot icon must render without missing images or undefined references");
}

void airDropIconFollowsAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom;
    for (const QColor color : {QColor(35, 35, 35), QColor(240, 240, 240)}) {
        const auto icon = icons::outlined::AirDrop(adqt::icons::IconColors::primary(color));
        require(adqt::icons::describeIcon(icon).colorModel ==
                    adqt::icons::IconColorModel::Monochrome,
                "AirDrop uses the monochrome theme pipeline");
        for (int size : {16, 20, 24, 32}) {
            for (qreal dpr : {1.0, 1.25, 2.0}) {
                const auto image = render(icon, QSize(size, size), dpr).toImage();
                const auto bounds = alphaBounds(image);
                require(!bounds.isEmpty() && bounds.left() > 0 &&
                            bounds.right() < image.width() - 1 && bounds.top() > 0 &&
                            bounds.bottom() < image.height(),
                        "AirDrop stays legible and unclipped at toolbar sizes and fractional DPR");
                require(std::abs(bounds.center().x() - image.rect().center().x()) <= 1,
                        "AirDrop remains optically centered");
                if (size >= 24)
                    require(containsOpaqueColor(image, color),
                            "AirDrop follows the requested theme color");
            }
        }
    }
    const QString previewPath = qEnvironmentVariable("SNOW_AIRDROP_ICON_PREVIEW");
    if (!previewPath.isEmpty()) {
        QImage preview(560, 180, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&preview);
        for (int row = 0; row < 2; ++row) {
            painter.fillRect(0, row * 90, 560, 90, row ? QColor(30, 30, 30) : QColor(Qt::white));
            const QColor color = row ? QColor(240, 240, 240) : QColor(35, 35, 35);
            painter.setPen(color);
            int x = 20;
            for (int size : {16, 20, 24, 32}) {
                painter.drawText(x, row * 90 + 20, QString::number(size) + QStringLiteral(" px"));
                painter.drawPixmap(
                    x, row * 90 + 36,
                    render(icons::outlined::Save(adqt::icons::IconColors::primary(color)),
                           QSize(size, size)));
                painter.drawPixmap(x + 40, row * 90 + 36,
                                   render(adqt::icons::antd::outlined::Printer(
                                              adqt::icons::IconColors::primary(color)),
                                          QSize(size, size)));
                painter.drawPixmap(
                    x + 80, row * 90 + 36,
                    render(icons::outlined::AirDrop(adqt::icons::IconColors::primary(color)),
                           QSize(size, size)));
                x += 135;
            }
        }
        painter.end();
        require(preview.save(previewPath), "save AirDrop toolbar icon preview");
    }
}

void recaptureIconUsesThemeColor() {
    namespace icons = snow_shot::presentation::icons::custom;
    for (const QColor color : {QColor(0, 166, 90), QColor(240, 240, 240)}) {
        const auto ref = icons::outlined::RefreshCapture(adqt::icons::IconColors::primary(color));
        require(adqt::icons::describeIcon(ref).colorModel ==
                    adqt::icons::IconColorModel::Monochrome,
                "Recapture must use the shared monochrome theme-color pipeline");
        const QImage image = render(ref, QSize(32, 32), 1.5).toImage();
        require(!alphaBounds(image).isEmpty() && containsOpaqueColor(image, color),
                "Recapture must render using the supplied theme color");
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                // Fractional SVG transforms can round an opaque overlap by one color level.
                require(pixel.alpha() != 255 || (std::abs(pixel.red() - color.red()) <= 1 &&
                                                 std::abs(pixel.green() - color.green()) <= 1 &&
                                                 std::abs(pixel.blue() - color.blue()) <= 1),
                        "Recapture must not retain any fixed SVG colors");
            }
        }
    }
}

void magnifierDecorationIconsFollowThemeAndToolbarMetrics() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QList<adqt::icons::IconRef> refs{icons::outlined::MagnifierConnectionNone(),
                                           icons::outlined::MagnifierGradient(),
                                           icons::outlined::SourceRegionOutline()};
    for (const auto& ref : refs) {
        const auto metadata = adqt::icons::describeIcon(ref);
        const auto svg = ref.descriptor()->svg;
        require(metadata.colorModel == adqt::icons::IconColorModel::Monochrome &&
                    svg.find("viewBox=\"0 0 1024 1024\"") != std::string_view::npos &&
                    svg.find("stroke-width=\"72\"") != std::string_view::npos &&
                    svg.find("stroke-linecap=\"round\"") != std::string_view::npos &&
                    svg.find("stroke-linejoin=\"round\"") != std::string_view::npos,
                "magnifier decoration icons share the Ant Design canvas and theme-color model");
        for (const QColor color :
             {QColor("#262626"), QColor("#f0f0f0"), QColor("#1677ff"), QColor("#bfbfbf")}) {
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                const QImage image = snow_shot::presentation::icons::renderTintedIconPixmap(
                                         ref, QSize(16, 16), dpr, color)
                                         .toImage()
                                         .convertToFormat(QImage::Format_ARGB32_Premultiplied);
                require(!image.isNull() && image.width() == qRound(16 * dpr) &&
                            !alphaBounds(image).isEmpty(),
                        "magnifier icons remain readable at toolbar sizes and fractional DPI");
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        // Gradient-stop premultiplication and interpolation each
                        // round to eight bits. Compare stored channels within their
                        // two-level bound; unpremultiplication amplifies that error.
                        const QRgb pixel = image.pixel(x, y);
                        const double alpha = qAlpha(pixel) / 255.0;
                        const int tolerance =
                            metadata.key.name == QStringLiteral("magnifier-gradient") &&
                                    qAlpha(pixel) < 255
                                ? 2
                                : 1;
                        require(std::abs(qRed(pixel) - qRound(color.red() * alpha)) <= tolerance &&
                                    std::abs(qGreen(pixel) - qRound(color.green() * alpha)) <=
                                        tolerance &&
                                    std::abs(qBlue(pixel) - qRound(color.blue() * alpha)) <=
                                        tolerance,
                                "stroke and translucent bridge use the supplied theme tint");
                    }
                if (metadata.key.name == QStringLiteral("magnifier-gradient")) {
                    const int bridgeAlpha =
                        qAlpha(image.pixel(qFloor(image.width() * 352.0 / 1024.0),
                                           qFloor(image.height() * 480.0 / 1024.0)));
                    require(bridgeAlpha > 0 && bridgeAlpha < 128,
                            "the theme-tinted bridge remains translucent between both icon frames");
                }
            }
        }
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

    const auto mouseRef = icons::outlined::Mouse(adqt::icons::IconColors::primary(primary));
    const auto mouseMetadata = adqt::icons::describeIcon(mouseRef);
    require(mouseMetadata.key.pack == QStringLiteral("snow-shot") &&
                mouseMetadata.key.name == QStringLiteral("mouse") &&
                containsOpaqueColor(render(mouseRef, QSize(32, 32)).toImage(), primary),
            "mouse should expose a tintable project-owned icon factory");

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

void miniLogoPreservesTheWordmarkAndAddsRoundedVectorLettering() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto mini = icons::brand::SnowShotMiniLogo();
    const auto svg = mini.descriptor()->svg;
    require(svg.find("<text") == std::string_view::npos &&
                svg.find("<image") == std::string_view::npos,
            "Mini branding must scale without installed fonts or embedded raster images");
    for (const QColor color : {QColor(32, 34, 38), QColor(240, 240, 242)}) {
        const auto colors = adqt::icons::IconColors::primary(color);
        for (const qreal scale : {1.0, 2.0, 3.0}) {
            const QImage original =
                render(icons::brand::SnowShotLogo(colors), QSize(95, 17), scale).toImage();
            const QImage extended =
                render(icons::brand::SnowShotMiniLogo(colors), QSize(137, 17), scale).toImage();
            require(extended.copy(original.rect()) == original,
                    "Mini must preserve the original Snow Shot artwork and its themed text");
        }
    }
    for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
        const QImage image = render(mini, QSize(113, 14), scale).toImage();
        int letterCount = 0;
        bool previousColumnHasInk = false;
        for (int x = qRound(image.width() * 99.0 / 137.0); x < image.width(); ++x) {
            bool columnHasInk = false;
            for (int y = 0; y < image.height(); ++y) {
                columnHasInk |= image.pixelColor(x, y).alpha() >= 80;
            }
            if (columnHasInk && !previousColumnHasInk) {
                ++letterCount;
            }
            previousColumnHasInk = columnHasInk;
        }
        require(letterCount == 4,
                "all four Mini letters must remain separated at the small title bar size");
    }
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

void flipIconsShowMirroredShapesWithoutDividers() {
    namespace icons = snow_shot::presentation::icons::custom;
    for (const QColor tint : {QColor(31, 31, 31), QColor(240, 240, 240), QColor(22, 119, 255)}) {
        const auto colors = adqt::icons::IconColors::primary(tint);
        const auto horizontalRef = icons::outlined::FlipHorizontal(colors);
        const auto verticalRef = icons::outlined::FlipVertical(colors);
        const auto horizontalMetadata = adqt::icons::describeIcon(horizontalRef);
        const auto verticalMetadata = adqt::icons::describeIcon(verticalRef);
        require(horizontalMetadata.key.pack == QStringLiteral("snow-shot") &&
                    horizontalMetadata.key.name == QStringLiteral("flip-horizontal") &&
                    verticalMetadata.key.pack == QStringLiteral("snow-shot") &&
                    verticalMetadata.key.name == QStringLiteral("flip-vertical"),
                "both flip operations should resolve to their dedicated project assets");
        for (const int size : {16, 20, 24, 32}) {
            for (const qreal dpr : {1.0, 1.25, 2.0}) {
                for (const auto& ref : {horizontalRef, verticalRef}) {
                    const QImage image = render(ref, QSize(size, size), dpr).toImage();
                    require(!alphaBounds(image).isEmpty() && containsOpaqueColor(image, tint),
                            "flip icons should remain visible and theme-aware at toolbar sizes");
                }
            }
        }
        const QImage horizontal = render(horizontalRef, QSize(64, 64)).toImage();
        const QImage vertical = render(verticalRef, QSize(64, 64)).toImage();
        require(opaquePixelCount(horizontal, QRect(29, 0, 6, 64)) == 0 &&
                    opaquePixelCount(vertical, QRect(0, 29, 64, 6)) == 0,
                "flip icons should leave the space between mirrored shapes free of dividers");
        require(horizontal.pixelColor(25, 32).alpha() > 0 &&
                    horizontal.pixelColor(38, 32).alpha() > 0 &&
                    horizontal.pixelColor(20, 40).alpha() == 0 &&
                    horizontal.pixelColor(43, 40).alpha() == 0 &&
                    vertical.pixelColor(31, 25).alpha() > 0 &&
                    vertical.pixelColor(31, 38).alpha() > 0 &&
                    vertical.pixelColor(23, 20).alpha() == 0 &&
                    vertical.pixelColor(23, 43).alpha() == 0,
                "both icons should retain two outlined mirrored panels in the correct orientation");
        require(horizontal.pixelColor(10, 14).alpha() == 0 &&
                    horizontal.pixelColor(53, 14).alpha() == 0 &&
                    horizontal.pixelColor(22, 12).alpha() > 0 &&
                    horizontal.pixelColor(41, 12).alpha() > 0 &&
                    vertical.pixelColor(49, 10).alpha() == 0 &&
                    vertical.pixelColor(49, 53).alpha() == 0 &&
                    vertical.pixelColor(51, 22).alpha() > 0 &&
                    vertical.pixelColor(51, 41).alpha() > 0,
                "the clipped outer corners should visibly reverse across the reflection direction");
    }
}

void conversionIconsUseTheSuppliedProjectAssets() {
    namespace icons = snow_shot::presentation::icons::custom::outlined;
    for (const QColor tint : {QColor(24, 24, 24), QColor(240, 240, 240)}) {
        const auto colors = adqt::icons::IconColors::primary(tint);
        for (const auto& ref : {icons::Markdown(colors), icons::Html(colors)}) {
            const auto metadata = adqt::icons::describeIcon(ref);
            require(metadata.key.pack == QStringLiteral("snow-shot") &&
                        (metadata.key.name == QStringLiteral("markdown") ||
                         metadata.key.name == QStringLiteral("html")),
                    "conversion icons resolve to the supplied project vector assets");
            for (const qreal dpr : {1.0, 1.5, 2.0}) {
                const QImage image = render(ref, QSize(20, 20), dpr).toImage();
                require(!alphaBounds(image).isEmpty() && containsOpaqueColor(image, tint),
                        "conversion icons remain nonblank and theme-aware at toolbar sizes and "
                        "fractional DPR");
            }
        }
    }
}

void scrollingIconsUseTheRequestedOrientations() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QColor tint(0, 166, 90);
    const auto colors = adqt::icons::IconColors::primary(tint);
    const auto horizontalRef = icons::outlined::ScrollingHorizontal(colors);
    const auto verticalRef = icons::outlined::ScrollingVertical(colors);
    const auto autoScrollRef = icons::outlined::AutoScroll(colors);
    require(adqt::icons::describeIcon(autoScrollRef).key.name == QStringLiteral("auto-scroll") &&
                containsOpaqueColor(render(autoScrollRef, QSize(32, 32)).toImage(), tint),
            "auto-scroll must render the supplied project asset with its requested color");
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

void arrowheadIconsFaceTheirRespectiveEndpoints() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto* pack = icons::pack().staticPack();
    for (const char* name :
         {"standard", "bar", "dot", "circle", "circle-outline", "indented-triangle", "triangle",
          "triangle-outline", "diamond", "diamond-outline", "crowfoot-one", "crowfoot-many",
          "crowfoot-one-or-many", "none"}) {
        const std::string endName = std::string("arrowhead-") + name;
        const std::string startName = endName + "-start";
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            const auto end = render(pack->icon("outlined", endName), QSize(40, 20), dpr).toImage();
            const auto start =
                render(pack->icon("outlined", startName), QSize(40, 20), dpr).toImage();
            require(!alphaBounds(start).isEmpty() && !alphaBounds(end).isEmpty(),
                    "both arrowhead endpoints should render");
            const QString previewDirectory = qEnvironmentVariable("SNOW_ARROWHEAD_PREVIEW_DIR");
            if (!previewDirectory.isEmpty() && std::string_view(name) == "indented-triangle" &&
                dpr == 2.0) {
                require(start.save(previewDirectory + QStringLiteral("/indented-start.png")) &&
                            end.save(previewDirectory + QStringLiteral("/indented-end.png")),
                        "save requested indented triangle icon previews");
            }
            qint64 difference = 0;
            for (int y = 0; y < end.height(); ++y) {
                for (int x = 0; x < end.width(); ++x) {
                    difference += std::abs(end.pixelColor(x, y).alpha() -
                                           start.pixelColor(end.width() - 1 - x, y).alpha());
                }
            }
            require(difference <= end.width() * end.height() * 2,
                    "start arrowhead should horizontally mirror its end arrowhead");
            // The marker glyph (head, bar, dot, ...) must sit at the boundary the option
            // names: the end variant on the right of the tail line, start on the left.
            const QRect leftHalf(0, 0, end.width() / 2, end.height());
            const QRect rightHalf(end.width() - end.width() / 2, 0, end.width() / 2, end.height());
            require(opaquePixelCount(end, rightHalf) > opaquePixelCount(end, leftHalf),
                    "end arrowhead should place its marker on the right of the tail");
            require(opaquePixelCount(start, leftHalf) > opaquePixelCount(start, rightHalf),
                    "start arrowhead should place its marker on the left of the tail");
        }
    }
}

void indentedTriangleCornersAreSymmetric() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto* pack = icons::pack().staticPack();
    for (const char* name : {"arrowhead-indented-triangle", "arrowhead-indented-triangle-start"}) {
        const auto* descriptor = pack->find("outlined", name);
        require(descriptor != nullptr, "indented triangle SVG should be registered");
        QXmlStreamReader xml(
            QByteArray(descriptor->svg.data(), static_cast<qsizetype>(descriptor->svg.size())));
        QString headPath;
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement &&
                xml.name() == QLatin1String("path")) {
                headPath = xml.attributes().value(QLatin1String("d")).toString().trimmed();
            }
        }
        // Qt joins coincident endpoints even without Z, but browsers leave two butt caps.
        require(!xml.hasError() && headPath.endsWith(QLatin1Char('z'), Qt::CaseInsensitive),
                "the arrowhead outline must explicitly close so all SVG renderers join its corner");
        const QImage image = render(pack->icon("outlined", name), QSize(400, 200)).toImage();
        qint64 difference = 0;
        for (int y = 0; y < image.height() / 2; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                difference += std::abs(image.pixelColor(x, y).alpha() -
                                       image.pixelColor(x, image.height() - 1 - y).alpha());
            }
        }
        const QString previewDirectory = qEnvironmentVariable("SNOW_ARROWHEAD_PREVIEW_DIR");
        if (!previewDirectory.isEmpty()) {
            require(image.save(previewDirectory + QLatin1Char('/') + QString::fromLatin1(name) +
                               QStringLiteral("-corners.png")),
                    "save enlarged arrowhead corner preview");
        }
        // A closed stroke joins both base corners identically. Allow only minor raster rounding.
        require(difference <= image.width() * 2,
                "indented triangle base corners should be complete and vertically symmetric");
    }
}

void imageStitchingIconFollowsAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto icon = icons::outlined::ImageStitching();
    const auto* descriptor = icon.descriptor();
    require(descriptor && descriptor->colorModel == adqt::icons::IconColorModel::Monochrome,
            "stitching icon belongs to the themed monochrome icon pack");
    const QByteArray svg(descriptor->svg.data(), static_cast<qsizetype>(descriptor->svg.size()));
    require(svg.contains("viewBox=\"0 0 1024 1024\"") && svg.contains("stroke-width=\"72\"") &&
                svg.contains("rx=\"72\"") && !svg.contains("<text") && !svg.contains("<image"),
            "stitching icon uses the Ant Design grid, weight and rounded vector geometry");
    QImage preview(384, 128, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::transparent);
    QPainter painter(&preview);
    for (int row = 0; row < 2; ++row) {
        const QColor color = row ? QColor("#f0f0f0") : QColor("#262626");
        painter.fillRect(QRect(0, row * 64, 384, 64), row ? QColor("#141414") : Qt::white);
        int column = 0;
        for (int size : {16, 20, 32}) {
            for (qreal dpr : {1.0, 1.25, 2.0}) {
                const QImage pixels =
                    render(icons::outlined::ImageStitching(adqt::icons::IconColors::primary(color)),
                           QSize(size, size), dpr)
                        .toImage();
                const QRect bounds = alphaBounds(pixels);
                require(!bounds.isEmpty() && bounds.left() > 0 &&
                            bounds.right() < pixels.width() - 1 &&
                            containsOpaqueColor(pixels, color),
                        "stitching icon stays legible, padded and themed at menu sizes and "
                        "fractional DPI");
            }
            painter.drawPixmap(
                column++ * 128 + 24, row * 64 + (64 - size) / 2,
                render(icons::outlined::ImageStitching(adqt::icons::IconColors::primary(color)),
                       QSize(size, size)));
        }
    }
    painter.end();
    if (const auto path = qEnvironmentVariable("SNOW_STITCHING_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save stitching icon light/dark inspection sheet");
}

void spreadsheetIconFollowsAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto ref = icons::outlined::OpenSpreadsheetFile();
    const auto* descriptor = ref.descriptor();
    require(descriptor && descriptor->colorModel == adqt::icons::IconColorModel::Monochrome,
            "spreadsheet uses the monochrome theme pipeline");
    const QByteArray svg(descriptor->svg.data(), static_cast<qsizetype>(descriptor->svg.size()));
    require(svg.contains("viewBox=\"0 0 1024 1024\"") && svg.contains("stroke-width=\"72\"") &&
                svg.contains("__ADQT_SLOT_PRIMARY__"),
            "spreadsheet follows the Ant Design grid and outline weight");
    const QRect bounds = alphaBounds(render(ref, QSize(1024, 1024)).toImage());
    require(bounds.left() >= 64 && bounds.top() >= 64 && bounds.right() < 960 &&
                bounds.bottom() < 960,
            "spreadsheet keeps the Ant Design safe margin");
    QImage preview(440, 256, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::white);
    QPainter painter(&preview);
    const QList<QColor> colors{QColor("#262626"), QColor("#f0f0f0"), QColor("#1677ff"),
                               QColor("#bfbfbf")};
    for (int row = 0; row < colors.size(); ++row) {
        const QColor color = colors[row];
        painter.fillRect(QRect(0, row * 64, 440, 64), row == 1 ? QColor("#141414") : Qt::white);
        int column = 0;
        for (int size : {14, 16, 20, 32}) {
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                const QImage pixels = render(icons::outlined::OpenSpreadsheetFile(
                                                 adqt::icons::IconColors::primary(color)),
                                             QSize(size, size), dpr)
                                          .toImage();
                require(!alphaBounds(pixels).isEmpty(),
                        "spreadsheet remains legible at menu sizes");
                for (int y = 0; y < pixels.height(); ++y) {
                    for (int x = 0; x < pixels.width(); ++x) {
                        const QColor pixel = pixels.pixelColor(x, y);
                        if (pixel.alpha() > 128)
                            require(qAbs(pixel.red() - color.red()) <= 2 &&
                                        qAbs(pixel.green() - color.green()) <= 2 &&
                                        qAbs(pixel.blue() - color.blue()) <= 2,
                                    "spreadsheet inherits menu foreground color at every DPI");
                    }
                }
            }
            painter.drawPixmap(24 + column++ * 90, row * 64 + (64 - size) / 2,
                               render(icons::outlined::OpenSpreadsheetFile(
                                          adqt::icons::IconColors::primary(color)),
                                      QSize(size, size)));
        }
    }
    painter.end();
    adqt::icons::IconRenderRequest disabled;
    disabled.logicalSize = QSize(20, 20);
    disabled.mode = QIcon::Disabled;
    adqt::icons::IconStatePalette palette;
    palette.set(QIcon::Disabled, QIcon::Off, adqt::icons::IconColors::primary(colors.last()));
    require(containsOpaqueColor(adqt::icons::renderIconPixmap(ref, disabled, palette).toImage(),
                                colors.last()),
            "spreadsheet supports the menu's disabled palette");
    if (const auto path = qEnvironmentVariable("SNOW_SPREADSHEET_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save spreadsheet icon visual inspection sheet");
}

void thoughtBubbleIconFollowsAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto ref = icons::outlined::ShapeThoughtBubble();
    const auto* descriptor = ref.descriptor();
    require(descriptor && descriptor->colorModel == adqt::icons::IconColorModel::Monochrome,
            "thought bubble uses the monochrome theme pipeline");
    const QByteArray svg(descriptor->svg.data(), static_cast<qsizetype>(descriptor->svg.size()));
    require(svg.contains("viewBox=\"0 0 1024 1024\"") && svg.contains("stroke-width=\"72\"") &&
                svg.contains("__ADQT_SLOT_PRIMARY__"),
            "thought bubble follows the Ant Design grid and outline weight");
    const QRect bounds = alphaBounds(render(ref, QSize(1024, 1024)).toImage());
    require(bounds.left() >= 64 && bounds.top() >= 64 && bounds.right() < 960 &&
                bounds.bottom() < 960,
            "thought bubble keeps the Ant Design safe margin");
    QImage preview(440, 256, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::white);
    QPainter painter(&preview);
    const QList<QColor> colors{QColor("#262626"), QColor("#f0f0f0"), QColor("#1677ff"),
                               QColor("#bfbfbf")};
    for (int row = 0; row < colors.size(); ++row) {
        const QColor color = colors[row];
        painter.fillRect(QRect(0, row * 64, 440, 64), row == 1 ? QColor("#141414") : Qt::white);
        int column = 0;
        for (int size : {14, 16, 20, 32}) {
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                const QImage pixels = render(icons::outlined::ShapeThoughtBubble(
                                                 adqt::icons::IconColors::primary(color)),
                                             QSize(size, size), dpr)
                                          .toImage();
                require(!alphaBounds(pixels).isEmpty(),
                        "thought bubble remains legible at menu sizes");
                for (int y = 0; y < pixels.height(); ++y) {
                    for (int x = 0; x < pixels.width(); ++x) {
                        const QColor pixel = pixels.pixelColor(x, y);
                        if (pixel.alpha() > 128)
                            require(qAbs(pixel.red() - color.red()) <= 2 &&
                                        qAbs(pixel.green() - color.green()) <= 2 &&
                                        qAbs(pixel.blue() - color.blue()) <= 2,
                                    "thought bubble inherits menu foreground color at every DPI");
                    }
                }
            }
            painter.drawPixmap(
                12 + column * 108, row * 64 + (64 - size) / 2,
                render(icons::outlined::ShapeDiamond(adqt::icons::IconColors::primary(color)),
                       QSize(size, size)));
            painter.drawPixmap(
                48 + column++ * 108, row * 64 + (64 - size) / 2,
                render(icons::outlined::ShapeThoughtBubble(adqt::icons::IconColors::primary(color)),
                       QSize(size, size)));
        }
    }
    painter.end();
    adqt::icons::IconRenderRequest disabled;
    disabled.logicalSize = QSize(20, 20);
    disabled.mode = QIcon::Disabled;
    adqt::icons::IconStatePalette palette;
    palette.set(QIcon::Disabled, QIcon::Off, adqt::icons::IconColors::primary(colors.last()));
    require(containsOpaqueColor(adqt::icons::renderIconPixmap(ref, disabled, palette).toImage(),
                                colors.last()),
            "thought bubble supports the menu's disabled palette");
    if (const auto path = qEnvironmentVariable("SNOW_THOUGHT_BUBBLE_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save thought bubble icon visual inspection sheet");
}

void revealIconFollowsAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto ref = icons::outlined::RevealInFolder();
    const auto* descriptor = ref.descriptor();
    require(descriptor && descriptor->colorModel == adqt::icons::IconColorModel::Monochrome,
            "reveal uses the monochrome theme pipeline");
    const QByteArray svg(descriptor->svg.data(), static_cast<qsizetype>(descriptor->svg.size()));
    require(svg.contains("viewBox=\"0 0 1024 1024\"") && svg.contains("stroke-width=\"72\"") &&
                svg.contains("__ADQT_SLOT_PRIMARY__"),
            "reveal follows the Ant Design grid and outline weight");
    const QRect bounds = alphaBounds(render(ref, QSize(1024, 1024)).toImage());
    require(bounds.left() >= 64 && bounds.top() >= 64 && bounds.right() < 960 &&
                bounds.bottom() < 960,
            "reveal keeps the Ant Design safe margin");
    QImage preview(440, 256, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::white);
    QPainter painter(&preview);
    const QList<QColor> colors{QColor("#262626"), QColor("#f0f0f0"), QColor("#1677ff"),
                               QColor("#bfbfbf")};
    for (int row = 0; row < colors.size(); ++row) {
        const QColor color = colors[row];
        painter.fillRect(QRect(0, row * 64, 440, 64), row == 1 ? QColor("#141414") : Qt::white);
        int column = 0;
        for (int size : {14, 16, 20, 32}) {
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                const QImage pixels =
                    render(icons::outlined::RevealInFolder(adqt::icons::IconColors::primary(color)),
                           QSize(size, size), dpr)
                        .toImage();
                require(!alphaBounds(pixels).isEmpty(), "reveal remains legible at menu sizes");
                for (int y = 0; y < pixels.height(); ++y) {
                    for (int x = 0; x < pixels.width(); ++x) {
                        const QColor pixel = pixels.pixelColor(x, y);
                        if (pixel.alpha() > 128)
                            require(qAbs(pixel.red() - color.red()) <= 2 &&
                                        qAbs(pixel.green() - color.green()) <= 2 &&
                                        qAbs(pixel.blue() - color.blue()) <= 2,
                                    "reveal inherits menu foreground color at every DPI");
                    }
                }
            }
            painter.drawPixmap(
                24 + column++ * 90, row * 64 + (64 - size) / 2,
                render(icons::outlined::RevealInFolder(adqt::icons::IconColors::primary(color)),
                       QSize(size, size)));
        }
    }
    painter.end();
    adqt::icons::IconRenderRequest disabled;
    disabled.logicalSize = QSize(20, 20);
    disabled.mode = QIcon::Disabled;
    adqt::icons::IconStatePalette palette;
    palette.set(QIcon::Disabled, QIcon::Off, adqt::icons::IconColors::primary(colors.last()));
    require(containsOpaqueColor(adqt::icons::renderIconPixmap(ref, disabled, palette).toImage(),
                                colors.last()),
            "reveal supports the menu's disabled palette");
    if (const auto path = qEnvironmentVariable("SNOW_REVEAL_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save reveal icon visual inspection sheet");
}

void cropIconsFollowAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom::outlined;
    const adqt::icons::IconRef refs[] = {icons::CropVertical(), icons::CropHorizontal()};
    QImage preview(480, 160, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&preview);
    for (int row = 0; row < 2; ++row) {
        const QColor foreground(row == 0 ? "#262626" : "#f0f0f0");
        painter.fillRect(QRect(0, row * 80, 480, 80), row == 0 ? Qt::white : QColor("#141414"));
        for (int index = 0; index < 2; ++index) {
            const auto ref = refs[index].withColors(adqt::icons::IconColors::primary(foreground));
            const auto bounds = alphaBounds(render(ref, QSize(1024, 1024)).toImage());
            require(QRect(64, 64, 896, 896).contains(bounds),
                    "crop icons respect Ant Design bleed");
            int column = 0;
            for (int size : {16, 24, 48}) {
                for (qreal dpr : {1.0, 1.5, 2.0}) {
                    const QImage pixels = render(ref, QSize(size, size), dpr).toImage();
                    require(!alphaBounds(pixels).isEmpty() &&
                                containsOpaqueColor(pixels, foreground),
                            "crop icons render with theme colors at toolbar sizes and scales");
                }
                painter.drawPixmap(index * 240 + column * 72 + (72 - size) / 2,
                                   row * 80 + (80 - size) / 2, render(ref, QSize(size, size)));
                ++column;
            }
        }
    }
    painter.end();
    require(render(refs[0], QSize(24, 24)).toImage() != render(refs[1], QSize(24, 24)).toImage(),
            "crop directions are visually distinct at toolbar size");
    if (const auto path = qEnvironmentVariable("SNOW_CROP_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save crop icon visual inspection sheet");
}

void imageSplittingIconFollowsAntDesign() {
    namespace icons = snow_shot::presentation::icons::custom;
    const auto ref = icons::outlined::ImageSplitting();
    const auto* descriptor = ref.descriptor();
    require(descriptor && descriptor->colorModel == adqt::icons::IconColorModel::Monochrome,
            "splitting icon uses the shared monochrome theme pipeline");
    const QByteArray svg(descriptor->svg.data(), static_cast<qsizetype>(descriptor->svg.size()));
    require(svg.contains("viewBox=\"0 0 1024 1024\"") && svg.contains("stroke-width=\"72\"") &&
                svg.contains("__ADQT_SLOT_PRIMARY__") && !svg.contains("<text") &&
                !svg.contains("<image"),
            "splitting uses the Ant Design canvas, outline weight and themed vector geometry");
    const QImage geometry = render(ref, QSize(1024, 1024)).toImage();
    require(alphaBounds(geometry) == QRect(64, 176, 896, 672),
            "splitting respects the 64-unit bleed and horizontal image proportions");
    require(opaquePixelCount(geometry, QRect(448, 0, 128, 1024)) == 0,
            "a clear central cut separates the two image panels");
    QImage preview(384, 192, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::transparent);
    QPainter painter(&preview);
    for (int row = 0; row < 3; ++row) {
        const QColor color = row == 0   ? QColor("#262626")
                             : row == 1 ? QColor("#f0f0f0")
                                        : QColor("#bfbfbf");
        painter.fillRect(QRect(0, row * 64, 384, 64), row == 1 ? QColor("#141414") : Qt::white);
        int column = 0;
        for (int size : {16, 20, 32}) {
            for (qreal dpr : {1.0, 1.25, 2.0}) {
                const auto colors = adqt::icons::IconColors::primary(color);
                const QImage pixels =
                    render(icons::outlined::ImageSplitting(colors), QSize(size, size), dpr)
                        .toImage();
                const QRect bounds = alphaBounds(pixels);
                require(!pixels.isNull() && !bounds.isEmpty() && bounds.left() > 0 &&
                            bounds.right() < pixels.width() - 1 &&
                            containsOpaqueColor(pixels, color),
                        "splitting stays padded and themed at menu sizes and fractional DPI");
                require(
                    opaquePixelCount(pixels, QRect(pixels.width() / 2, 0, 1, pixels.height())) == 0,
                    "the split remains visible at menu sizes and fractional DPI");
            }
            painter.drawPixmap(
                column++ * 128 + 24, row * 64 + (64 - size) / 2,
                render(icons::outlined::ImageSplitting(adqt::icons::IconColors::primary(color)),
                       QSize(size, size)));
        }
    }
    painter.end();
    adqt::icons::IconRenderRequest disabled;
    disabled.logicalSize = QSize(20, 20);
    disabled.mode = QIcon::Disabled;
    const QColor disabledColor(QStringLiteral("#bfbfbf"));
    adqt::icons::IconStatePalette palette;
    palette.set(QIcon::Disabled, QIcon::Off, adqt::icons::IconColors::primary(disabledColor));
    const QImage disabledPixels = adqt::icons::renderIconPixmap(ref, disabled, palette).toImage();
    require(!disabledPixels.isNull() && containsOpaqueColor(disabledPixels, disabledColor),
            "splitting icon supports the supplied disabled-state palette");
    if (const auto path = qEnvironmentVariable("SNOW_SPLITTING_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path),
                "save splitting icon light, dark and disabled inspection sheet");
}

void brushModeIconsRenderWithDistinctPressureAndThemeColors() {
    namespace icons = snow_shot::presentation::icons::custom;
    const QList<adqt::icons::IconRef> refs{icons::outlined::BrushRegular(),
                                           icons::outlined::BrushPressureSensitive()};
    for (const auto& ref : refs) {
        require(adqt::icons::describeIcon(ref).colorModel ==
                    adqt::icons::IconColorModel::Monochrome,
                "brush mode icons use the shared monochrome theme-color pipeline");
        const QRect bounds = alphaBounds(render(ref, QSize(1024, 1024)).toImage());
        require(bounds.left() >= 64 && bounds.top() >= 64 && bounds.right() < 960 &&
                    bounds.bottom() < 960,
                "brush mode artwork preserves the Ant Design 64-unit safe margin");
    }
    const QImage regular = render(refs[0], QSize(1024, 1024)).toImage();
    const QImage pressure = render(refs[1], QSize(1024, 1024)).toImage();
    // At each wave extremum the tangent is horizontal, so a vertical sample
    // measures brush width without including the stroke's diagonal projection.
    const auto widthAt = [](const QImage& image, int x) {
        int width = 0;
        for (int y = 0; y < image.height(); ++y)
            if (image.pixelColor(x, y).alpha() >= 128)
                ++width;
        return width;
    };
    const int regularFirst = widthAt(regular, 320);
    require(std::abs(widthAt(regular, 704) - regularFirst) <= 2,
            "the regular brush keeps a constant width across the wave");
    require(widthAt(pressure, 320) < regularFirst,
            "the pressure-sensitive brush starts lighter than the regular brush");
    require(widthAt(pressure, 704) > regularFirst * 1.5,
            "the pressure-sensitive brush ends with a visibly heavier stroke");
    for (const QColor color :
         {QColor("#262626"), QColor("#f0f0f0"), QColor("#1677ff"), QColor("#bfbfbf")}) {
        for (int size : {16, 20, 24, 32}) {
            for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                QList<QImage> images;
                for (const auto& ref : refs) {
                    const QImage image =
                        render(ref.withColors(adqt::icons::IconColors::primary(color)),
                               QSize(size, size), dpr)
                            .toImage();
                    const QRect bounds = alphaBounds(image);
                    require(!bounds.isEmpty() && bounds.left() > 0 &&
                                bounds.right() < image.width() - 1,
                            "both brush modes stay visible and unclipped at toolbar sizes");
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const QColor pixel = image.pixelColor(x, y);
                            if (pixel.alpha() >= 128)
                                require(
                                    std::abs(pixel.red() - color.red()) <= 2 &&
                                        std::abs(pixel.green() - color.green()) <= 2 &&
                                        std::abs(pixel.blue() - color.blue()) <= 2,
                                    "brush icons respect light, dark, active and disabled tints");
                        }
                    images.append(image);
                }
                require(images[0] != images[1],
                        "regular and pressure-sensitive modes remain distinct at every scale");
            }
        }
    }
    if (const auto path = qEnvironmentVariable("SNOW_BRUSH_ICON_PREVIEW"); !path.isEmpty()) {
        QImage preview(960, 520, QImage::Format_ARGB32_Premultiplied);
        preview.fill(QColor("#ffffff"));
        QPainter painter(&preview);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor("#595959"));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 11));
        painter.drawText(QPoint(40, 36), QStringLiteral("SNOW SHOT  /  BRUSH MODES"));
        painter.setPen(QColor("#f0f0f0"));
        painter.drawLine(480, 68, 480, 360);
        for (int mode = 0; mode < 2; ++mode) {
            const int x = mode * 480;
            painter.drawPixmap(
                x + 128, 72,
                render(refs[mode].withColors(adqt::icons::IconColors::primary(QColor("#262626"))),
                       QSize(224, 224)));
            painter.setPen(QColor("#262626"));
            painter.setFont(QFont(QStringLiteral("Segoe UI"), 16, QFont::DemiBold));
            painter.drawText(QRect(x, 276, 480, 36), Qt::AlignCenter,
                             mode ? QStringLiteral("Pressure-sensitive brush")
                                  : QStringLiteral("Regular brush"));
            painter.setPen(QColor("#8c8c8c"));
            painter.setFont(QFont(QStringLiteral("Segoe UI"), 10));
            painter.drawText(QRect(x, 316, 480, 24), Qt::AlignCenter,
                             mode ? QStringLiteral("Smoothly increasing stroke width")
                                  : QStringLiteral("Consistent 72-unit stroke width"));
        }
        for (int row = 0; row < 2; ++row) {
            const int y = 376 + row * 72;
            const QColor background = row ? QColor("#141414") : QColor("#f5f5f5");
            const QColor foreground = row ? QColor("#f0f0f0") : QColor("#262626");
            painter.fillRect(QRect(0, y, 960, 72), background);
            painter.setPen(QColor("#8c8c8c"));
            painter.setFont(QFont(QStringLiteral("Segoe UI"), 9));
            painter.drawText(QPoint(40, y + 41),
                             row ? QStringLiteral("DARK") : QStringLiteral("LIGHT"));
            for (int mode = 0; mode < 2; ++mode) {
                int column = 0;
                for (int size : {16, 20, 24, 32}) {
                    painter.drawPixmap(
                        mode * 480 + 140 + column++ * 56, y + (72 - size) / 2,
                        render(refs[mode].withColors(adqt::icons::IconColors::primary(foreground)),
                               QSize(size, size)));
                }
                painter.drawPixmap(mode * 480 + 390, y + 24,
                                   render(refs[mode].withColors(
                                              adqt::icons::IconColors::primary(QColor("#1677ff"))),
                                          QSize(24, 24)));
            }
        }
        painter.end();
        require(preview.save(path), "save the brush icon light/dark inspection sheet");
    }
}

void numericFormatIconsRenderAtToolbarSizes() {
    namespace icons = snow_shot::presentation::icons::custom;
    QImage preview(600, 240, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::white);
    QPainter painter(&preview);
    const QColor backgrounds[] = {QColor("#ffffff"), QColor("#141414"), QColor("#e6f4ff"),
                                  QColor("#f5f5f5")};
    const QColor foregrounds[] = {QColor("#262626"), QColor("#f0f0f0"), QColor("#1677ff"),
                                  QColor("#bfbfbf")};
    for (int row = 0; row < 4; ++row) {
        painter.fillRect(QRect(0, row * 60, 600, 60), backgrounds[row]);
        const auto colors = adqt::icons::IconColors::primary(foregrounds[row]);
        const QList<adqt::icons::IconRef> refs = {
            icons::outlined::SequenceNumberNumericArabic(colors),
            icons::outlined::SequenceNumberNumericRoman(colors),
            icons::outlined::SequenceNumberNumericLowercaseLetters(colors),
            icons::outlined::SequenceNumberNumericUppercaseLetters(colors),
            icons::outlined::SequenceNumberNumericChinese(colors)};
        for (int column = 0; column < refs.size(); ++column) {
            const auto* descriptor = refs[column].descriptor();
            require(descriptor != nullptr, "numeric icon is registered");
            const QByteArray svg(descriptor->svg.data(),
                                 static_cast<qsizetype>(descriptor->svg.size()));
            require(svg.contains("0 0 1024 1024") && !svg.contains("<text"),
                    "numeric icons use an Ant Design grid and vector paths");
            for (qreal dpr : {1.0, 1.5, 2.0}) {
                const QPixmap pixmap = render(refs[column], QSize(16, 16), dpr);
                require(!pixmap.isNull() && opaquePixelCount(pixmap.toImage(), pixmap.rect()) > 0,
                        "every numeric icon paints at normal and fractional DPI");
            }
            painter.drawPixmap(column * 120 + 24, row * 60 + 22,
                               render(refs[column], QSize(16, 16)));
            painter.drawPixmap(column * 120 + 60, row * 60 + 14,
                               render(refs[column], QSize(32, 32)));
        }
    }
    painter.end();
    if (const auto path = qEnvironmentVariable("SNOW_SERIAL_NUMBER_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save numeric icon inspection sheet");
}

void borderFillIconMatchesToolbarGeometry() {
    namespace icons = snow_shot::presentation::icons::custom;
    QImage preview(480, 144, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&preview);
    for (int row = 0; row < 2; ++row) {
        const QColor foreground(row ? "#f0f0f0" : "#595959");
        painter.fillRect(0, row * 72, 480, 72, QColor(row ? "#141414" : "#ffffff"));
        int column = 0;
        for (int size : {16, 20, 32}) {
            for (double dpr : {1.0, 1.25, 2.0}) {
                const auto ref =
                    icons::outlined::FillBorder(adqt::icons::IconColors::primary(foreground));
                const auto pixmap = render(ref, QSize(size, size), dpr);
                const auto image = pixmap.toImage();
                require(!pixmap.isNull() && opaquePixelCount(image, image.rect()) > 0,
                        "Border icon paints at toolbar sizes and fractional DPI");
                require(image.pixelColor(image.width() / 2, image.height() / 2).alpha() == 0,
                        "Border icon has an empty center");
                const QRect bounds = alphaBounds(image);
                require(qAbs(bounds.width() - bounds.height()) <= 1,
                        "Border uses a balanced square silhouette");
                painter.drawPixmap(12 + column++ * 52, row * 72 + (72 - size) / 2, pixmap);
            }
        }
    }
    painter.end();
    if (const auto path = qEnvironmentVariable("SNOW_BORDER_FILL_ICON_PREVIEW"); !path.isEmpty())
        require(preview.save(path), "save Border icon inspection sheet");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--airdrop-only"))) {
            airDropIconFollowsAntDesign();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--thought-bubble-only"))) {
            thoughtBubbleIconFollowsAntDesign();
            return 0;
        }
        thoughtBubbleIconFollowsAntDesign();
        spreadsheetIconFollowsAntDesign();
        if (application.arguments().contains(QStringLiteral("--spreadsheet-only")))
            return 0;
        if (application.arguments().contains(QStringLiteral("--flip-icons-only"))) {
            flipIconsShowMirroredShapesWithoutDividers();
            return 0;
        }
        cropIconsFollowAntDesign();
        if (application.arguments().contains(QStringLiteral("--crop-icons-only"))) {
            return 0;
        }
        revealIconFollowsAntDesign();
        if (application.arguments().contains(QStringLiteral("--reveal-only"))) {
            everySnowShotEntryRenders();
            return 0;
        }
        borderFillIconMatchesToolbarGeometry();
        if (application.arguments().contains(QStringLiteral("--border-fill-only"))) {
            return 0;
        }
        imageSplittingIconFollowsAntDesign();
        if (application.arguments().contains(QStringLiteral("--image-splitting-only"))) {
            everySnowShotEntryRenders();
            return 0;
        }
        brushModeIconsRenderWithDistinctPressureAndThemeColors();
        if (application.arguments().contains(QStringLiteral("--brush-icons-only"))) {
            everySnowShotEntryRenders();
            return 0;
        }
        imageStitchingIconFollowsAntDesign();
        if (application.arguments().contains(QStringLiteral("--image-stitching-only"))) {
            everySnowShotEntryRenders();
            return 0;
        }
        magnifierDecorationIconsFollowThemeAndToolbarMetrics();
        numericFormatIconsRenderAtToolbarSizes();
        if (application.arguments().contains(QStringLiteral("--recapture-only"))) {
            recaptureIconUsesThemeColor();
            return 0;
        }
        recaptureIconUsesThemeColor();
        if (application.arguments().contains(QStringLiteral("--ocr-translate-only"))) {
            ocrTranslateIconUsesTheSuppliedProjectAsset();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--all-entries-only"))) {
            everySnowShotEntryRenders();
            return 0;
        }
        everySnowShotEntryRenders();
        arrowheadIconsFaceTheirRespectiveEndpoints();
        indentedTriangleCornersAreSymmetric();
        conversionIconsUseTheSuppliedProjectAssets();
        projectIconColorsAndModelsArePreserved();
        miniLogoPreservesTheWordmarkAndAddsRoundedVectorLettering();
        ocrTranslateIconUsesTheSuppliedProjectAsset();
        scrollingIconsUseTheRequestedOrientations();
        flipIconsShowMirroredShapesWithoutDividers();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
