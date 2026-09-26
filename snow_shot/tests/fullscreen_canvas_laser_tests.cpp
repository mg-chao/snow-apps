#include "../src/presentation/canvas/fullscreencanvaslaser.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>

#include <cstdlib>
#include <iostream>

namespace {
using snow_shot::presentation::FullscreenCanvasLaser;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Fixture {
    qint64 now = 0;
    SnowCanvasWidget canvas;
    FullscreenCanvasLaser laser{canvas, nullptr, [this] { return now; }};

    Fixture() {
        canvas.resize(260, 160);
        canvas.setInteractionEnabled(false);
    }

    void mouse(QEvent::Type type, const QPointF& position, Qt::MouseButton button,
               Qt::MouseButtons buttons) {
        QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }

    void press(const QPointF& position) {
        mouse(QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton);
    }

    void move(const QPointF& position) {
        mouse(QEvent::MouseMove, position, Qt::NoButton, Qt::LeftButton);
    }

    void release(const QPointF& position) {
        mouse(QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton);
    }

    QImage image(const QRegion& exposed = QRegion(QRect(0, 0, 260, 160))) {
        QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        const SnowCanvasRenderContext context{canvas.rect(), exposed, QTransform(), 1.0};
        laser.renderAfterCanvas(painter, context);
        return image;
    }
};

quint64 alphaSum(const QImage& image) {
    quint64 sum = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            sum += static_cast<quint64>(image.pixelColor(x, y).alpha());
        }
    }
    return sum;
}

void onlyActiveLeftDraggingCreatesTrails() {
    Fixture fixture;
    fixture.press({20.0, 40.0});
    fixture.move({150.0, 40.0});
    fixture.release({150.0, 40.0});
    require(fixture.laser.trailEmpty(), "inactive laser must ignore mouse gestures");

    fixture.laser.setActive(true);
    fixture.mouse(QEvent::MouseMove, {30.0, 40.0}, Qt::NoButton, Qt::NoButton);
    fixture.mouse(QEvent::MouseButtonPress, {30.0, 40.0}, Qt::RightButton, Qt::RightButton);
    fixture.mouse(QEvent::MouseMove, {100.0, 40.0}, Qt::NoButton, Qt::RightButton);
    fixture.mouse(QEvent::MouseButtonRelease, {100.0, 40.0}, Qt::RightButton, Qt::NoButton);
    require(fixture.laser.trailEmpty(), "hover and right dragging must not create a laser trail");
    require(!fixture.laser.animationActive(), "ignored mouse events must not start animation");

    fixture.press({20.0, 40.0});
    require(fixture.laser.animationActive(), "press must start the expiration timer");
    require(alphaSum(fixture.image()) == 0, "one point must not create a permanent laser dot");
    fixture.move({150.0, 40.0});
    require(alphaSum(fixture.image()) > 0, "left dragging must paint a laser trail");
    require(fixture.image().pixelColor(60, 40).red() == 255, "the default trail must be red");
    require(!fixture.canvas.canvasHistoryState().canUndo &&
                !fixture.canvas.canvasHistoryState().canRedo,
            "laser gestures must not change document history");
}

void releasedTrailsFadeAndStopAnimating() {
    Fixture fixture;
    fixture.laser.setActive(true);
    fixture.press({20.0, 40.0});
    fixture.move({160.0, 40.0});
    fixture.release({180.0, 40.0});
    const quint64 initialAlpha = alphaSum(fixture.image());
    require(initialAlpha > 0, "release must retain the fading trail");
    fixture.now = 975;
    fixture.laser.advance();
    const quint64 fadedAlpha = alphaSum(fixture.image());
    require(fadedAlpha > 0 && fadedAlpha < initialAlpha,
            "the trail must taper smoothly before expiration");
    fixture.now = 1000;
    fixture.laser.advance();
    require(fixture.laser.trailEmpty(), "the trail must expire at its configured lifetime");
    require(alphaSum(fixture.image()) == 0, "expired trails must render no pixels");
    require(!fixture.laser.animationActive(), "animation must stop when all samples expire");
}

void stationaryPressExpiresAndInterruptedInputDoesNotResume() {
    Fixture fixture;
    fixture.laser.setActive(true);
    fixture.press({20.0, 40.0});
    fixture.move({160.0, 40.0});
    fixture.now = 1100;
    fixture.laser.advance();
    require(fixture.laser.trailEmpty() && !fixture.laser.animationActive(),
            "holding the pointer still must not keep a trail alive");
    fixture.move({170.0, 45.0});
    fixture.move({190.0, 45.0});
    require(alphaSum(fixture.image()) > 0, "movement may restart a trail while still pressed");

    fixture.laser.clear();
    fixture.press({20.0, 40.0});
    QFocusEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(&fixture.canvas, &focusOut);
    fixture.move({160.0, 40.0});
    require(alphaSum(fixture.image()) == 0,
            "focus loss must end the drag instead of drawing a later stray movement");
}

void separateStrokesRemainDisconnectedAndDeactivationClears() {
    Fixture fixture;
    fixture.laser.setActive(true);
    fixture.press({20.0, 25.0});
    fixture.move({65.0, 25.0});
    fixture.release({65.0, 25.0});
    fixture.press({180.0, 120.0});
    fixture.move({230.0, 120.0});
    fixture.release({230.0, 120.0});
    const QImage image = fixture.image();
    require(image.pixelColor(30, 25).alpha() > 0 && image.pixelColor(190, 120).alpha() > 0,
            "both recent strokes must remain visible");
    require(image.pixelColor(110, 70).alpha() == 0,
            "separate strokes must not be joined across the canvas");
    fixture.laser.setActive(false);
    require(!fixture.laser.active() && fixture.laser.trailEmpty() &&
                !fixture.laser.animationActive() && alphaSum(fixture.image()) == 0,
            "deactivation must clear all transient state and stop animation");
    fixture.laser.setActive(true);
    fixture.move({230.0, 120.0});
    require(fixture.laser.trailEmpty(), "reactivation must wait for a fresh press");
}

void styleAndExposedRegionAreRespected() {
    Fixture fixture;
    fixture.laser.setStyle(QColor(20, 100, 230, 128), 12.0, 400);
    fixture.laser.setActive(true);
    fixture.press({20.0, 60.0});
    fixture.move({80.0, 60.0});
    fixture.move({130.0, 60.0});
    fixture.move({180.0, 60.0});
    fixture.release({180.0, 60.0});
    const QImage full = fixture.image();
    require(full.pixelColor(50, 64).alpha() > 0, "configured width must affect trail thickness");
    require(full.pixelColor(50, 60).blue() > full.pixelColor(50, 60).red(),
            "configured color must affect the trail");
    for (int y = 0; y < full.height(); ++y) {
        for (int x = 0; x < full.width(); ++x) {
            require(full.pixelColor(x, y).alpha() <= 128,
                    "overlapping rounded segments must composite translucent color only once");
        }
    }
    const QImage clipped = fixture.image(QRegion(QRect(40, 50, 30, 20)));
    require(clipped.pixelColor(50, 60).alpha() > 0 && clipped.pixelColor(90, 60).alpha() == 0,
            "rendering must respect the exposed damage region");
    fixture.now = 400;
    fixture.laser.advance();
    require(fixture.laser.trailEmpty(), "configured duration must control expiration");
}

void longGesturesExpireWithoutChangingTheCanvas() {
    Fixture fixture;
    fixture.laser.setActive(true);
    fixture.press({20.0, 80.0});
    for (int sample = 0; sample < 600; ++sample) {
        fixture.now = sample;
        fixture.move({20.0 + sample % 220, 70.0 + sample % 20});
    }
    require(alphaSum(fixture.image()) > 0, "long gestures must keep the newest trail visible");
    fixture.now = 1600;
    fixture.laser.advance();
    require(fixture.laser.trailEmpty() && !fixture.laser.animationActive(),
            "bounded long gestures must expire completely");
    require(!fixture.canvas.canvasHistoryState().canUndo,
            "long gestures must not create document history");
}

void styleBoundsMatchTheSupportedControls() {
    Fixture fixture;
    fixture.laser.setStyle(Qt::red, 100.0, 10000);
    fixture.laser.setActive(true);
    fixture.press({20.0, 60.0});
    fixture.move({180.0, 60.0});
    fixture.release({180.0, 60.0});
    const QImage maximumWidth = fixture.image();
    require(maximumWidth.pixelColor(50, 68).alpha() > 0 &&
                maximumWidth.pixelColor(50, 71).alpha() == 0,
            "programmatic laser widths must clamp to the supported 20-pixel maximum");
    fixture.now = 4999;
    fixture.laser.advance();
    require(!fixture.laser.trailEmpty(), "maximum laser duration must last through 4999 ms");
    fixture.now = 5000;
    fixture.laser.advance();
    require(fixture.laser.trailEmpty(), "programmatic laser durations must clamp to 5000 ms");

    fixture.now = 6000;
    fixture.laser.setStyle(Qt::red, 0.0, 0);
    fixture.press({20.0, 60.0});
    fixture.move({180.0, 60.0});
    fixture.release({180.0, 60.0});
    const QImage minimumWidth = fixture.image();
    require(alphaSum(minimumWidth) > 0 && minimumWidth.pixelColor(50, 62).alpha() == 0,
            "programmatic laser widths must retain the supported one-pixel minimum");
    fixture.now = 6099;
    fixture.laser.advance();
    require(!fixture.laser.trailEmpty(), "minimum laser duration must last through 99 ms");
    fixture.now = 6100;
    fixture.laser.advance();
    require(fixture.laser.trailEmpty(), "programmatic laser durations must clamp to 100 ms");
}
} // namespace

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    onlyActiveLeftDraggingCreatesTrails();
    releasedTrailsFadeAndStopAnimating();
    stationaryPressExpiresAndInterruptedInputDoesNotResume();
    separateStrokesRemainDisconnectedAndDeactivationClears();
    styleAndExposedRegionAreRespected();
    longGesturesExpireWithoutChangingTheCanvas();
    styleBoundsMatchTheSupportedControls();
    std::cout << "Full-screen canvas laser tests passed\n";
    return 0;
}
