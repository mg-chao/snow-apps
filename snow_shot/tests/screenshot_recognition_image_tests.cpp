#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include <QGuiApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QThread>
#include <future>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
ScreenshotRecognitionImageSnapshot fixture() {
    ScreenshotRecognitionImageSnapshot snapshot;
    snapshot.image = QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
    snapshot.image.fill(QColor(230, 240, 250));
    snapshot.canvasRect = QRectF(640, 360, 320, 180);
    snapshot.font = QGuiApplication::font();
    snapshot.textColor = Qt::black;
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Translated result");
    line.quad = {{660, 380}, {920, 380}, {920, 420}, {660, 420}};
    snapshot.lines.push_back(line);
    return snapshot;
}
void rendersNativeCoordinatesAndPreservesSource() {
    auto snapshot = fixture();
    snapshot.image.setDevicePixelRatio(2.0);
    const QImage source = snapshot.image;
    const QImage rendered = renderScreenshotRecognitionImage(snapshot);
    require(rendered.size() == source.size() && rendered.devicePixelRatio() == 1.0,
            "export retains physical image resolution regardless of display DPR");
    require(snapshot.image == source, "worker render must not mutate source image");
    require(rendered.pixelColor(0, 0) == source.pixelColor(0, 0),
            "non-text image pixels are preserved");
    int textPixels = 0;
    for (int y = 0; y < rendered.height(); ++y) {
        for (int x = 0; x < rendered.width(); ++x) {
            if (rendered.pixelColor(x, y) != source.pixelColor(x, y)) {
                require(QRect(20, 20, 260, 40).contains(x, y), "text stays in its output quad");
                ++textPixels;
            }
        }
    }
    require(textPixels > 100, "recognized text is rasterized");
    snapshot.lines.clear();
    require(renderScreenshotRecognitionImage(snapshot).pixelColor(70, 30) ==
                source.pixelColor(70, 30),
            "pending and empty recognition preserve the currently displayed source");
}
void displayedBackgroundAndEffectsAreAppliedOnce() {
    auto snapshot = fixture();
    snapshot.lines.clear();
    snapshot.filteredImage = QImage(40, 20, QImage::Format_ARGB32_Premultiplied);
    snapshot.filteredImage.fill(Qt::green);
    snapshot.filteredCanvasRect = QRectF(680, 380, 40, 20);
    QImage image = renderScreenshotRecognitionImage(snapshot);
    require(image.pixelColor(45, 25) == QColor(Qt::green) &&
                image.pixelColor(39, 25) == snapshot.image.pixelColor(39, 25),
            "displayed filtered patch uses canvas coordinates without covering other image pixels");
    snapshot.resultStyle = {16, 8, Qt::black};
    image = renderScreenshotRecognitionImage(snapshot);
    require(image.size() == QSize(336, 196) && image.pixelColor(53, 33) == QColor(Qt::green),
            "shadow padding is applied exactly once after recognition composition");
    require(image.pixelColor(8, 8).alpha() < 255, "rounded result corners preserve transparency");
}
void layoutModesAndTransformsRenderOnWorkers() {
    for (bool vertical : {false, true}) {
        for (bool paragraph : {false, true}) {
            auto snapshot = fixture();
            snapshot.lines[0].direction = vertical ? ScreenshotOcrTextDirection::Vertical
                                                   : ScreenshotOcrTextDirection::Horizontal;
            snapshot.lines[0].paragraph = paragraph;
            snapshot.lines[0].text =
                QString::fromUtf8("繁體文字 Á العربية 👩‍💻");
            snapshot.lines[0].quad = {{675, 375}, {820, 390}, {805, 525}, {660, 510}};
            const QImage expected = renderScreenshotRecognitionImage(snapshot);
            auto result = std::async(std::launch::async, [snapshot]() {
                return renderScreenshotRecognitionImage(snapshot);
            });
            require(
                !expected.isNull() && result.get() == expected,
                "Unicode, vertical and paragraph layout are deterministic on independent workers");
            if (expected == snapshot.image)
                std::cerr << "empty layout vertical=" << vertical << " paragraph=" << paragraph
                          << '\n';
            require(expected != snapshot.image, "transformed Unicode text produces visible pixels");
        }
    }
}
void sourceRowsSurviveImageExportOnWorkers() {
    auto snapshot = fixture();
    snapshot.image = QImage(360, 160, QImage::Format_ARGB32_Premultiplied);
    snapshot.image.fill(Qt::white);
    snapshot.canvasRect = QRectF(-180, -80, 360, 160);
    auto& line = snapshot.lines[0];
    line.text = QString(31, QChar(0x7530));
    line.paragraph = true;
    line.quad = QPolygonF(QRectF(-150, -50, 300, 96));
    line.quad.removeLast();
    for (const QRectF& row :
         {QRectF(-150, -50, 300, 24), QRectF(-150, -14, 300, 24), QRectF(-150, 22, 180, 24)}) {
        QPolygonF quad(row);
        quad.removeLast();
        line.sourceLineQuads.push_back(quad);
    }
    const QImage expected = renderScreenshotRecognitionImage(snapshot);
    auto worker = std::async(std::launch::async,
                             [snapshot]() { return renderScreenshotRecognitionImage(snapshot); });
    require(worker.get() == expected, "source row export is deterministic on a worker thread");
    const auto inkBounds = [&](const QRect& region) {
        QRect bounds;
        for (int y = region.top(); y <= region.bottom(); ++y) {
            for (int x = region.left(); x <= region.right(); ++x) {
                if (expected.pixelColor(x, y).red() < 80)
                    bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
        return bounds;
    };
    for (const QRect& row :
         {QRect(30, 30, 300, 24), QRect(30, 66, 300, 24), QRect(30, 102, 180, 24)}) {
        const QRect ink = inkBounds(row);
        require(ink.width() >= row.width() * 0.9 && ink.height() >= row.height() * 0.65,
                "image export preserves the full source rows and shorter final row");
    }
    require(inkBounds(QRect(30, 56, 300, 8)).isEmpty() &&
                inkBounds(QRect(30, 92, 300, 8)).isEmpty() &&
                inkBounds(QRect(212, 102, 118, 24)).isEmpty(),
            "image export preserves source line gaps and the shorter final row boundary");
    line.sourceLineQuads.clear();
    const QImage fallback = renderScreenshotRecognitionImage(snapshot);
    line.sourceLineQuads = {line.quad, line.quad};
    require(renderScreenshotRecognitionImage(snapshot) == fallback,
            "image export falls back to paragraph layout for overlapping source rows");
}
void cancellationAndInvalidInputNeverPublishPartialImages() {
    auto snapshot = fixture();
    int polls = 0;
    require(renderScreenshotRecognitionImage(snapshot, [&]() { return ++polls >= 2; }).isNull(),
            "mid-render cancellation discards partial output");
    snapshot.canvasRect = {};
    require(renderScreenshotRecognitionImage(snapshot).isNull(),
            "invalid geometry fails rendering");
    snapshot = fixture();
    snapshot.lines[0].quad[0].setX(std::numeric_limits<qreal>::quiet_NaN());
    require(renderScreenshotRecognitionImage(snapshot) == snapshot.image,
            "malformed text geometry cannot corrupt the exported image");
}
} // namespace
int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
#if defined(Q_OS_WIN)
    if (QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) < 0 ||
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) < 0)
        return 3;
    QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif

    try {
        rendersNativeCoordinatesAndPreservesSource();
        displayedBackgroundAndEffectsAreAppliedOnce();
        layoutModesAndTransformsRenderOnWorkers();
        sourceRowsSurviveImageExportOnWorkers();
        cancellationAndInvalidInputNeverPublishPartialImages();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
