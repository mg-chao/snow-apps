#include "presentation/pinned/pinnedplacementgeometry.h"
#include "presentation/pinned/screenshotpinnednativegeometrycontroller.h"
#include <cstdlib>
#include <iostream>
#include <cmath>

using namespace snow_shot::presentation;
using snow_shot::storage::PinnedWindowPlacement;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace
int main() {
    const PinnedDisplayGeometry retina{QStringLiteral("retina"), QStringLiteral("a"),
                                       QRectF(-1600, -300, 1600, 1000),
                                       QRectF(-1600, -262, 1600, 930), 2};
    const PinnedDisplayGeometry external{QStringLiteral("external"), QStringLiteral("b"),
                                         QRectF(0, 0, 1920, 1080), QRectF(0, 25, 1920, 1010), 1};
    require(pinnedDisplayContains(retina, QPointF(-.25, 100)) &&
                !pinnedDisplayContains(external, QPointF(-.25, 100)) &&
                !pinnedDisplayContains(retina, QPointF(0, 100)) &&
                pinnedDisplayContains(external, QPointF(0, 100)),
            "fractional boundary positions must select displays using half-open logical bounds");
    PinnedWindowPlacement placement{retina.name, retina.serial, QPointF(100.5, 200.5),
                                    QSize(321, 181)};
    require(pinnedDesktopRect(placement, retina) == QRectF(-1499.5, -99.5, 160.5, 90.5),
            "negative display origins and half-point geometry must remain exact");
    const QPointF anchor(137, 61);
    const QPointF pointer(120, 160);
    const auto moved = pinnedPlacementAtPointer(placement, external, pointer, anchor);
    require(moved.pixelSize == placement.pixelSize && moved.position == QPointF(-17, 99),
            "display changes must preserve pixel size and grabbed image pixel");
    const auto back = pinnedPlacementAtPointer(
        moved, retina,
        pinnedDesktopRect(placement, retina).topLeft() + anchor / retina.backingScale, anchor);
    require(back == placement, "mixed Retina round trips must not accumulate drift");
    for (int i = 0; i < 1000; ++i) {
        placement = pinnedPlacementAtPointer(placement, external, pointer, anchor);
        placement = pinnedPlacementAtPointer(placement, retina, QPointF(-1431, -69), anchor);
    }
    require(placement.pixelSize == QSize(321, 181),
            "repeated display changes must preserve physical extent");
    placement.position = QPointF(10000, -10000);
    const auto recovered = recoverPinnedPlacement(placement, retina);
    require(retina.usableBounds.contains(pinnedDesktopRect(recovered, retina)),
            "display recovery must keep the complete pin below the menu bar and above the Dock");
    placement.pixelSize = QSize(10000, 10000);
    require(pinnedDesktopRect(recoverPinnedPlacement(placement, external), external).topLeft() ==
                external.usableBounds.topLeft(),
            "oversized pins must retain reachable top-left controls without rescaling");
    ScreenshotPinnedNativeGeometryController controller;
    require(controller.initialize(QRect(0, 0, 321, 181)), "initialization failed");
    require(!controller.acceptInteractiveGeometry(QRect(1, 1, 321, 181)),
            "passive changes cannot enter a drag transaction");
    require(controller.beginMove(QPoint()), "begin failed");
    require(controller.acceptInteractiveGeometry(QRect(40, -20, 321, 181)),
            "controlled drag update rejected");
    controller.cancelPendingInteraction();
    require(controller.targetGeometry() == QRect(0, 0, 321, 181),
            "cancel must restore the transaction origin");
    require(controller.beginResize(screenshot_pinned_resize_geometry::DragHandle::Left),
            "resize failed");
    require(controller.acceptInteractiveGeometry(QRect(-321, 0, 642, 362)),
            "controlled resize update rejected");
    static_cast<void>(controller.commitTarget());
    require(controller.committedGeometry() == QRect(-321, 0, 642, 362),
            "accepted resize must commit");
    require(controller.beginProgrammatic(QRect(-320, 0, 642, 362),
                                         ScreenshotPinnedNativeGeometryController::Origin::Scale),
            "programmatic transaction failed");
    require(controller.acceptAppliedGeometry(QRect(-319, 0, 642, 362)),
            "native rounding readback was rejected");
    require(controller.committedGeometry() == QRect(-321, 0, 642, 362),
            "readback must not commit before the shared transaction finishes");
    static_cast<void>(controller.commitTarget());
    require(controller.committedGeometry() == QRect(-319, 0, 642, 362),
            "native rounding must become the next transaction's origin");
    return 0;
}
