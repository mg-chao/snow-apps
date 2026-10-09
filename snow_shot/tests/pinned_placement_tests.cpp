#include "snow_shot/presentation/pinnedgeometry.h"
#include "presentation/pinned/pinnedplacementgeometry.h"
#include "presentation/pinned/pinneddisplayselection.h"
#include "presentation/pinned/screenshotpinnednativegeometrycontroller.h"
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <array>
#include <limits>
#include <optional>
#include <vector>

using namespace snow_shot::presentation;
using snow_shot::storage::PinnedWindowPlacement;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void displaySelectionPreservesIdentity() {
    const PinnedDisplayIdentity primary{QStringLiteral("primary"), QStringLiteral("shared")};
    const PinnedDisplayIdentity secondary{QStringLiteral("secondary"), QStringLiteral("shared")};
    const PinnedDisplayIdentity other{QStringLiteral("other"), QStringLiteral("unique")};
    std::array displays{primary, secondary, other};
    PinnedWindowPlacement placement{secondary.name, secondary.serial, QPointF(250, 200),
                                    QSize(240, 120)};
    // A serial number is not necessarily unique, even for connected displays.
    // Enumeration order must never redirect a placement to another monitor.
    for (int order = 0; order < 3; ++order) {
        const auto index = pinnedDisplayIndex(placement, displays);
        require(index && displays[*index].name == secondary.name,
                "duplicate monitor serials must be disambiguated by the saved display name");
        std::rotate(displays.begin(), displays.begin() + 1, displays.end());
    }

    placement.displayName = QStringLiteral("old-name");
    placement.displaySerial = other.serial;
    require(pinnedDisplayIndex(placement, displays) == 2,
            "a unique serial must preserve monitor identity after a display is renamed");
    placement.displayName = primary.name;
    require(pinnedDisplayIndex(placement, displays) == 2,
            "a unique serial must take precedence over a name reused by another monitor");

    placement.displayName = secondary.name;
    placement.displaySerial.clear();
    require(pinnedDisplayIndex(placement, displays) == 1,
            "displays without a saved serial must still resolve by a unique name");
    placement.displaySerial = QStringLiteral("old-serial");
    require(pinnedDisplayIndex(placement, displays) == 1,
            "a unique name must remain usable when a serial is no longer reported");
    displays[1].serial.clear();
    require(pinnedDisplayIndex(placement, displays) == 1,
            "a monitor that stops reporting its serial must still resolve by name");

    displays[1] = secondary;
    placement.displayName = QStringLiteral("disconnected");
    placement.displaySerial = primary.serial;
    require(!pinnedDisplayIndex(placement, displays),
            "an ambiguous serial without a matching name must defer to the caller's fallback");
    placement.displaySerial.clear();
    require(!pinnedDisplayIndex(placement, displays),
            "a disconnected display must defer to the caller's fallback");

    displays[1] = primary;
    placement.displayName = primary.name;
    placement.displaySerial = primary.serial;
    require(!pinnedDisplayIndex(placement, displays),
            "even matching name and serial must not arbitrarily select identical identities");
    displays[1].serial = QStringLiteral("different");
    require(pinnedDisplayIndex(placement, displays) == 0,
            "the serial must disambiguate displays with the same name");
    placement.displaySerial.clear();
    require(!pinnedDisplayIndex(placement, displays),
            "an ambiguous name alone must defer to the caller's fallback");

    const std::array<PinnedDisplayIdentity, 2> anonymous{};
    placement.displayName.clear();
    require(!pinnedDisplayIndex(placement, anonymous),
            "empty display identifiers must not be treated as an identity match");
    require(!pinnedDisplayIndex(placement, {}),
            "an empty display list must have no identity match");
}

void contentAnchorsSurviveMixedDisplayScales() {
    const PinnedDisplayGeometry source{QStringLiteral("source"), QStringLiteral("source-serial"),
                                       QRectF(-1600, -300, 1600, 1000),
                                       QRectF(-1600, -262, 1600, 930), 1.25};
    const PinnedDisplayGeometry destination{
        QStringLiteral("destination"), QStringLiteral("destination-serial"),
        QRectF(0, 0, 1920, 1080), QRectF(0, 25, 1920, 1010), 1.75};
    const QPointF translation(1550.125, 250.25);
    const auto near = [](const QPointF& left, const QPointF& right) {
        return std::abs(left.x() - right.x()) < 1.e-9 && std::abs(left.y() - right.y()) < 1.e-9;
    };
    for (const auto units :
         {PinnedGeometryUnits::PhysicalPixels, PinnedGeometryUnits::LogicalPixels}) {
        const int margin = pinnedShadowMargin(units);
        for (const QSize contentSize : {QSize(321, 181), QSize(1, 1)}) {
            const PinnedWindowPlacement original{source.name, source.serial, QPointF(100.5, 200.5),
                                                 pinnedOuterSize(contentSize, units), units};
            const QPointF contentOrigin =
                pinnedDesktopContentRect(original, source, margin).topLeft();
            const QPointF movedContentOrigin = contentOrigin + translation;
            auto moved =
                pinnedPlacementAtContentOrigin(original, destination, movedContentOrigin, margin);
            require(moved.windowSize == original.windowSize &&
                        moved.displayName == destination.name &&
                        moved.displaySerial == destination.serial &&
                        near(pinnedDesktopContentRect(moved, destination, margin).topLeft(),
                             movedContentOrigin),
                    "shared movement must preserve the content translation across fractional DPI");
            const auto outerOrigin = pinnedDesktopRect(moved, destination).topLeft();
            const qreal destinationInset =
                margin / pinnedGeometryScale(destination.backingScale, units);
            require(
                near(outerOrigin + QPointF(destinationInset, destinationInset), movedContentOrigin),
                "destination outer bounds must reserve the destination display's frame inset");

            moved.windowSize = pinnedOuterSize(contentSize * 2, units);
            moved = pinnedPlacementAtContentOrigin(moved, destination, movedContentOrigin, margin);
            const QRectF scaledContent = pinnedDesktopContentRect(moved, destination, margin);
            const QSizeF expectedContentSize =
                QSizeF(contentSize * 2) / pinnedGeometryScale(destination.backingScale, units);
            require(near(scaledContent.topLeft(), movedContentOrigin) &&
                        std::abs(scaledContent.width() - expectedContentSize.width()) < 1.e-9 &&
                        std::abs(scaledContent.height() - expectedContentSize.height()) < 1.e-9,
                    "shared scaling must change only content extent while preserving its anchor");

            auto roundTrip = original;
            for (int iteration = 0; iteration < 1000; ++iteration) {
                roundTrip = pinnedPlacementAtContentOrigin(roundTrip, destination,
                                                           movedContentOrigin, margin);
                roundTrip =
                    pinnedPlacementAtContentOrigin(roundTrip, source, contentOrigin, margin);
            }
            require(roundTrip.windowSize == original.windowSize &&
                        roundTrip.displayName == original.displayName &&
                        roundTrip.displaySerial == original.displaySerial &&
                        near(roundTrip.position, original.position),
                    "repeated shared display changes must not accumulate content-anchor drift");
            require(original.windowSize == pinnedOuterSize(contentSize, units) &&
                        original.position == QPointF(100.5, 200.5),
                    "content-anchor conversion must preserve the original outer rollback snapshot");
        }
    }
}

void exactPlacementRetriesPreserveContentAfterDisplayReassignment() {
    const PinnedDisplayGeometry source{QStringLiteral("source"), QStringLiteral("source-serial"),
                                       QRectF(-1600, -300, 1600, 1000),
                                       QRectF(-1600, -262, 1600, 930), 1.25};
    const PinnedDisplayGeometry destination{
        QStringLiteral("destination"), QStringLiteral("destination-serial"),
        QRectF(0, 0, 1920, 1080), QRectF(0, 25, 1920, 1010), 1.75};
    const QPointF contentOrigin(-5.125, 70.25);
    for (const auto units :
         {PinnedGeometryUnits::PhysicalPixels, PinnedGeometryUnits::LogicalPixels}) {
        const int margin = pinnedShadowMargin(units);
        auto original = pinnedPlacementAtContentOrigin(
            PinnedWindowPlacement{{}, {}, {}, pinnedOuterSize({321, 181}, units), units}, source,
            contentOrigin, margin);
        const QPointF outerOrigin = pinnedDesktopRect(original, source).topLeft();
        for (const int anchorMargin : {0, margin}) {
            std::optional<PinnedWindowPlacement> actual;
            std::vector<PinnedWindowPlacement> proposals;
            const auto apply = [&](const auto& requested, const PinnedDisplayGeometry* display) {
                proposals.push_back(requested);
                // The backend selects another display while preserving the requested
                // outer origin. Its size readback already matches the requested extent.
                const QPointF outer = pinnedDesktopRect(requested, *display).topLeft();
                actual = pinnedPlacementAtPointer(requested, destination, outer, {});
                return true;
            };
            const auto describe = [](const PinnedDisplayGeometry* display) { return *display; };
            const auto resolve = [&](const auto&, const auto*) { return &destination; };
            require(applyExactPinnedPlacement(
                        original, &source, anchorMargin, describe, apply, [&] { return actual; },
                        resolve),
                    "exact placement must settle after a backend assigns another DPI display");
            const QPointF settled =
                pinnedDesktopContentRect(*actual, destination, anchorMargin).topLeft();
            const QPointF expected = anchorMargin == 0 ? outerOrigin : contentOrigin;
            require(std::abs(settled.x() - expected.x()) < 1.e-9 &&
                        std::abs(settled.y() - expected.y()) < 1.e-9 &&
                        actual->windowSize == original.windowSize,
                    "placement retry must preserve its selected anchor without scaling the frame");
            const std::size_t expectedAttempts =
                anchorMargin != 0 && units == PinnedGeometryUnits::PhysicalPixels ? 2 : 1;
            require(proposals.size() == expectedAttempts,
                    "content DPI correction must settle once; outer/logical anchors need no retry");
        }

        int attempts = 0;
        std::optional<PinnedWindowPlacement> displaced;
        require(!applyExactPinnedPlacement(
                    original, &source, margin,
                    [](const PinnedDisplayGeometry* display) { return *display; },
                    [&](const auto& requested, const auto*) {
                        ++attempts;
                        displaced = requested;
                        displaced->position += QPointF(10, 10);
                        return true;
                    },
                    [&] { return displaced; }, [&](const auto&, const auto*) { return &source; }) &&
                    attempts == (units == PinnedGeometryUnits::PhysicalPixels ? 3 : 1),
                "an unsatisfied exact placement must fail after its bounded retries");
        const QPointF originalContent =
            pinnedDesktopContentRect(original, source, margin).topLeft();
        require(original.windowSize == pinnedOuterSize({321, 181}, units) &&
                    std::abs(originalContent.x() - contentOrigin.x()) < 1.e-9 &&
                    std::abs(originalContent.y() - contentOrigin.y()) < 1.e-9,
                "placement retries must preserve the original outer rollback snapshot");
    }
}

void decorationGeometryRejectsOverflow() {
    constexpr int minimum = std::numeric_limits<int>::min();
    constexpr int maximum = std::numeric_limits<int>::max();
    for (const auto units :
         {PinnedGeometryUnits::PhysicalPixels, PinnedGeometryUnits::LogicalPixels}) {
        const int margin = pinnedShadowMargin(units);
        const int maximumContent = maximum - 2 * margin;
        require(pinnedOuterSize({maximumContent, 1}, units) == QSize(maximum, 1 + 2 * margin) &&
                    pinnedOuterSize({maximumContent + 1, 1}, units).isEmpty() &&
                    pinnedOuterSize({1, maximum}, units).isEmpty(),
                "frame inflation must accept the largest representable extent and reject overflow");
        const QRect limit(QPoint(minimum + margin, minimum + margin), QSize(maximumContent, 1));
        const QRect expanded = pinnedOuterRect(limit, units);
        require(expanded.isValid() && expanded.size() == QSize(maximum, 1 + 2 * margin) &&
                    pinnedContentRect(expanded, units) == limit,
                "checked decoration geometry must round-trip representable coordinate limits");
        require(
            pinnedOuterRect(QRect(minimum, 0, 1, 1), units).isEmpty() &&
                pinnedOuterRect(QRect(QPoint(maximum, 0), QPoint(maximum, 0)), units).isEmpty() &&
                pinnedOuterRect(QRect(0, 0, maximum, 1), units).isEmpty() &&
                pinnedContentRect(QRect(QPoint(minimum, 0), QPoint(maximum, 64)), units).isEmpty(),
            "frame geometry must reject overflowed coordinates and oversized stored bounds");
    }
}
} // namespace
int main() {
    for (const auto units :
         {PinnedGeometryUnits::PhysicalPixels, PinnedGeometryUnits::LogicalPixels}) {
        const int margin = units == PinnedGeometryUnits::PhysicalPixels ? 16 : 12;
        const QRect content(-1920, -317, 321, 181);
        const QRect outer = pinnedOuterRect(content, units);
        require(outer == content.adjusted(-margin, -margin, margin, margin) &&
                    pinnedContentRect(outer, units) == content &&
                    pinnedOuterSize(content.size(), units) == outer.size(),
                "frame margin must expand outward without moving or resizing the content");
        require(pinnedOuterSize({1, 1}, units) == QSize(1 + 2 * margin, 1 + 2 * margin) &&
                    pinnedContentRect(QRect(0, 0, 2 * margin, 2 * margin + 1), units).isEmpty(),
                "reserved frame must preserve one-unit content and reject empty content bounds");
    }
    displaySelectionPreservesIdentity();
    contentAnchorsSurviveMixedDisplayScales();
    exactPlacementRetriesPreserveContentAfterDisplayReassignment();
    decorationGeometryRejectsOverflow();
    QImage raster(QSize(600, 400), QImage::Format_RGB32);
    const bool logicalUnits = kPinnedGeometryUnits == PinnedGeometryUnits::LogicalPixels;
    for (const qreal imageScale : {1.0, 2.0, 3.0}) {
        raster.setDevicePixelRatio(imageScale);
        require(pinnedImageWindowSize(raster, 1.0) == QSize(600, 400),
                "standard display pins must use raster size regardless of image DPR metadata");
        require(
            pinnedImageWindowSize(raster, 2.0) ==
                (logicalUnits ? QSize(300, 200) : QSize(600, 400)),
            "Retina file and clipboard pins must use target display DPI, even without metadata");
        require(pinnedImageWindowSize(raster, 1.5) ==
                    (logicalUnits ? QSize(400, 267) : QSize(600, 400)),
                "fractional display scaling must round initial window dimensions");
        require(raster.size() == QSize(600, 400) && raster.devicePixelRatio() == imageScale,
                "pin sizing must preserve source pixels and metadata");
    }
    require(pinnedImageWindowSize(QImage(), 2).isEmpty(), "empty images must have no window size");
    require(pinnedImageWindowSize(QImage(1, 1, QImage::Format_RGB32), 2) == QSize(1, 1),
            "small images must retain a nonempty window at high DPI");
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
                                    QSize(321, 181),
                                    snow_shot::storage::PinnedGeometryUnits::PhysicalPixels};
    require(pinnedDesktopRect(placement, retina) == QRectF(-1499.5, -99.5, 160.5, 90.5),
            "negative display origins and half-point geometry must remain exact");
    const QPointF anchor(137, 61);
    const QPointF pointer(120, 160);
    const auto moved = pinnedPlacementAtPointer(placement, external, pointer, anchor);
    require(moved.windowSize == placement.windowSize && moved.position == QPointF(-17, 99),
            "display changes must preserve pixel size and grabbed image pixel");
    const auto back = pinnedPlacementAtPointer(
        moved, retina,
        pinnedDesktopRect(placement, retina).topLeft() + anchor / retina.backingScale, anchor);
    require(back == placement, "mixed Retina round trips must not accumulate drift");
    for (int i = 0; i < 1000; ++i) {
        placement = pinnedPlacementAtPointer(placement, external, pointer, anchor);
        placement = pinnedPlacementAtPointer(placement, retina, QPointF(-1431, -69), anchor);
    }
    require(placement.windowSize == QSize(321, 181),
            "repeated display changes must preserve physical extent");
    placement.position = QPointF(10000, -10000);
    const auto recovered = recoverPinnedPlacement(placement, retina);
    require(retina.usableBounds.contains(pinnedDesktopRect(recovered, retina)),
            "display recovery must keep the complete pin below the menu bar and above the Dock");
    placement.windowSize = QSize(10000, 10000);
    require(pinnedDesktopRect(recoverPinnedPlacement(placement, external), external).topLeft() ==
                external.usableBounds.topLeft(),
            "oversized pins must retain reachable top-left controls without rescaling");
    // Logical geometry is independent of the display's raster backing scale.
    using Units = snow_shot::storage::PinnedGeometryUnits;
    PinnedWindowPlacement logical{retina.name, retina.serial, QPointF(100, 80), QSize(301, 201),
                                  Units::LogicalPixels};
    const auto logicalOrigin = logical;
    require(pinnedDesktopRect(logical, retina) == QRectF(-1500, -220, 301, 201),
            "Retina must not divide logical position or size");
    for (int i = 0; i < 1000; ++i) {
        logical = pinnedPlacementAtPointer(logical, external, QPointF(300, 250), QPointF(30, 20));
        require(pinnedDesktopRect(logical, external) == QRectF(270, 230, 301, 201),
                "logical drag must preserve size and pointer anchor on a 1x display");
        logical = pinnedPlacementAtPointer(logical, retina, QPointF(-1470, -200), QPointF(30, 20));
        require(logical == logicalOrigin, "logical mixed-scale round trips must not drift");
    }
    logical.position = QPointF(10000, -10000);
    require(retina.usableBounds.contains(
                pinnedDesktopRect(recoverPinnedPlacement(logical, retina), retina)),
            "logical recovery must keep controls in the usable point bounds");
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
    require(!controller.acceptAppliedGeometry(QRect(-319, 0, 642, 362)),
            "exact physical application must reject placement rounding");
    require(controller.acceptAppliedGeometry(QRect(-319, 0, 642, 362), true),
            "controlled platform placement must retain its explicit adjustment policy");
    require(controller.committedGeometry() == QRect(-321, 0, 642, 362),
            "readback must not commit before the shared transaction finishes");
    static_cast<void>(controller.commitTarget());
    require(controller.committedGeometry() == QRect(-319, 0, 642, 362),
            "native rounding must become the next transaction's origin");
    return 0;
}
