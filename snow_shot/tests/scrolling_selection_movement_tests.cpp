#include "presentation/capture/scrollingselectionmovement.h"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using namespace snow_shot::capture_detail;
    using Axis = ScreenshotScrollingRecognitionMode;
    ScrollingSelectionMovement movement;
    const QRect bounds(-1920, -1080, 5760, 3240);
    const QRect selection(-100, -200, 640, 480);
    require(!movement.begin(Axis::Horizontal, Axis::Vertical, selection, {}),
            "controller must reject forbidden axis");
    require(movement.begin(Axis::Vertical, Axis::Vertical, selection, QPoint(200, 100)),
            "start vertical drag");
    require(!movement.begin(Axis::Vertical, Axis::Vertical, selection, {}), "nested drag rejected");
    require(movement.update(QPoint(800, 600), bounds) == QRect(-100, 300, 640, 480),
            "vertical movement must ignore horizontal displacement");
    require(movement.update(QPoint(200, -5000), bounds).top() == -1080, "clamp upper desktop edge");
    require(movement.update(QPoint(200, 5000), bounds).bottom() == bounds.bottom(),
            "clamp lower desktop edge");
    require(movement.update(QPoint(200, 100), bounds) == selection,
            "return to press position without drift");
    movement.end();
    require(!movement.active(), "release clears movement");
    require(movement.begin(Axis::Horizontal, Axis::Horizontal, selection, QPoint(-50, 20)),
            "start horizontal drag");
    require(movement.update(QPoint(2150, 920), bounds) == QRect(2100, -200, 640, 480),
            "cross-monitor movement keeps physical size and vertical position");
    require(movement.update(QPoint(-9000, 20), bounds).left() == bounds.left(),
            "clamp left desktop edge");
    require(movement.update(QPoint(9000, 20), bounds).right() == bounds.right(),
            "clamp right desktop edge");
    movement.end();
    require(!movement.begin(Axis::Vertical, Axis::Vertical, {}, {}), "reject empty selection");
    return 0;
}
