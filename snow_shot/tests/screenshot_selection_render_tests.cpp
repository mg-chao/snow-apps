#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotsourceimagecomposer.h"
#include <QApplication>
#include <cstdlib>
#include <iostream>
#include <limits>
#include "../src/presentation/capture/captureframegeometry.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
CapturedDisplayModel display(QRect points, int scale, QColor color) {
    CapturedDisplayModel d;
    d.physicalRect = QRect(points.topLeft(), points.size() * scale);
    d.capturedLogicalRect = points;
    d.logicalRect = points;
    d.canvasRect = points;
    d.image = QImage(points.size() * scale, QImage::Format_RGBA8888);
    d.image.fill(color);
    d.canvasUsesPoints = true;
    d.backingScale = scale;
    d.active = true;
    return d;
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ScreenshotDisplaySession displays;
    displays.appendDisplay(display(QRect(-1920, 0, 1920, 1080), 2, Qt::red));
    displays.appendDisplay(display(QRect(0, 0, 1920, 1080), 1, Qt::blue));
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    require(geometry.canvasBounds() == QRectF(0, 0, 3840, 1080), "logical desktop layout changed");
    const QRect all(0, 0, 3840, 1080);
    const auto spec = screenshotSelectionRenderSpec(displays, all);
    require(spec.pixelSize == QSize(7680, 2160), "mixed-scale output dimensions");
    const QImage image = composeScreenshotSourceSelection(displays, all);
    require(image.size() == spec.pixelSize && image.devicePixelRatio() == 1,
            "encoded pixel dimensions");
    require(image.pixelColor(3839, 100) == QColor(Qt::red) &&
                image.pixelColor(3840, 100) == QColor(Qt::blue),
            "display seam or resampling is wrong");
    require(screenshotSelectionRenderSpec(displays, QRect(1920, 0, 1920, 1080)).pixelSize ==
                QSize(1920, 1080),
            "edge-only contact changed scale");
    require(screenshotSelectionRenderSpec(displays, QRect(1919, 0, 2, 10)).pixelSize ==
                QSize(4, 20),
            "partial intersection scale");
    require(screenshotSelectionRenderSpec(displays, QRect(0, 0, 100, 50)).pixelSize ==
                QSize(200, 100),
            "Retina selection");
    // Per-display pixel coordinates overlap here; owner identity must survive conversion.
    const auto& retina = displays.displayAt(0);
    const auto& external = displays.displayAt(1);
    require(retina.physicalRect.contains(QPoint(100, 20)) &&
                external.physicalRect.contains(QPoint(100, 20)),
            "fixture must exercise overlapping pixel ranges");
    require(geometry.canvasPositionForPhysicalPoint(retina, QPointF(100, 20)) == QPointF(1010, 10),
            "Retina cursor transform");
    require(geometry.canvasPositionForPhysicalPoint(external, QPointF(100, 20)) ==
                QPointF(2020, 20),
            "external cursor transform");
    require(geometry.displayForLogicalPoint(displays, QPointF(100, 20)) == &external,
            "cursor owner must come from desktop points");
    displays.displayAt(1).image.fill(Qt::black);
    displays.displayAt(1).image.setPixelColor(0, 0, Qt::white);
    const auto patterned = composeScreenshotSourceSelection(displays, QRect(1919, 0, 3, 2));
    require(patterned.size() == QSize(6, 4) &&
                patterned.pixelColor(2, 0).red() > patterned.pixelColor(3, 0).red() &&
                patterned.pixelColor(3, 0).red() > patterned.pixelColor(4, 0).red(),
            "lower-resolution display must be smoothly resampled on the shared grid");
    displays.displayAt(1).image = QImage(QSize(3840, 2160), QImage::Format_RGBA8888);
    displays.displayAt(1).image.fill(Qt::blue);
    displays.displayAt(1).backingScale = 2;
    geometry.rebuild(displays);
    require(geometry.canvasBounds() == QRectF(all) &&
                screenshotSelectionRenderSpec(displays, QRect(1920, 0, 100, 50)).pixelSize ==
                    QSize(200, 100),
            "scale-only change must preserve logical topology");
    SnowCaptureFrameGeometry native{};
    native.coordinate_space = 1;
    native.width = 1920;
    native.height = 1080;
    native.backing_scale = 2;
    require(snow_shot::presentation::capture::logicalFrameRect(native).has_value(),
            "valid geometry");
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        native.backing_scale = invalid;
        require(!snow_shot::presentation::capture::logicalFrameRect(native),
                "invalid scale must be rejected");
    }
    native.backing_scale = 2;
    native.x = std::numeric_limits<int>::max();
    require(!snow_shot::presentation::capture::logicalFrameRect(native),
            "overflowing geometry must be rejected");
    displays.displayAt(1).canvasRect = QRect(1920, 100, 1080, 1920);
    displays.displayAt(1).image = QImage(1080, 1920, QImage::Format_RGBA8888);
    displays.displayAt(1).image.fill(Qt::blue);
    const auto offset = composeScreenshotSourceSelection(displays, QRect(1910, 0, 30, 120));
    require(offset.size() == QSize(60, 240) && offset.pixelColor(40, 10).alpha() == 0 &&
                offset.pixelColor(40, 210) == QColor(Qt::blue),
            "rotated offset desktop gap");
    displays.displayAt(0).canvasUsesPoints = false;
    displays.displayAt(1).canvasUsesPoints = false;
    require(screenshotSelectionRenderSpec(displays, all).pixelSize == all.size(),
            "legacy Windows pixel semantics");
    require(!screenshotSelectionRenderSpec(displays, QRect(5000, 5000, 5, 5)).isValid(),
            "empty intersection");
    return 0;
}
