#include "screenshotpinnednativegeometrycontroller.h"

#include <iostream>
#include <stdexcept>

namespace {
namespace resize_geometry = screenshot_pinned_resize_geometry;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ScreenshotPinnedNativeGeometryController initializedController() {
    ScreenshotPinnedNativeGeometryController controller;
    require(controller.initialize(QRect(1015, 805, 1298, 737)), "controller initialization failed");
    return controller;
}

void passiveProposalsCannotChangeStableGeometry() {
    auto controller = initializedController();
    const QRect proposed(1016, 806, 1298, 737);
    require(controller.constrainWindowPos(proposed, true, true) == QRect(1015, 805, 1298, 737),
            "stable geometry must reject passive native rounding");
    require(controller.constrainWindowPos(proposed, true, false).topLeft() == QPoint(1015, 805) &&
                controller.constrainWindowPos(proposed, true, false).size() == proposed.size(),
            "position-only proposals must preserve their size flags");
}

void clickWithoutMovementRestoresTheExactStart() {
    auto controller = initializedController();
    require(controller.beginMove(QPoint(1664, 1173)), "move transaction did not begin");
    const QRect roundedProposal(1016, 806, 1298, 737);
    require(controller.constrainWindowPos(roundedProposal, true, false) ==
                QRect(1015, 805, 1298, 737),
            "pending move must reject a no-op rounding proposal");
    require(controller.updateMove(roundedProposal, QPoint(1664, 1173)) ==
                QRect(1015, 805, 1298, 737),
            "a stationary cursor must retain the exact starting rectangle");
    require(controller.finishInteractiveTarget() == QRect(1015, 805, 1298, 737),
            "a click without movement must finish at the starting rectangle");
}

void externallyInitiatedMovementEstablishesAnExplicitTransaction() {
    auto controller = initializedController();
    const QRect proposed(1039, 823, 1298, 737);
    require(controller.updateMove(proposed, QPoint(1700, 1200)) == proposed,
            "an external WM_MOVING proposal must establish its own move transaction");
    require(controller.hasAcceptedInteractiveGeometry() &&
                controller.finishInteractiveTarget() == proposed,
            "the externally established move transaction must be committable");
}

void realMovementUsesPhysicalCursorDelta() {
    auto controller = initializedController();
    require(controller.beginMove(QPoint(1664, 1173)), "move transaction did not begin");
    const QRect biasedProposal(1053, 835, 1298, 737);
    const QRect expected(1052, 834, 1298, 737);
    require(controller.updateMove(biasedProposal, QPoint(1701, 1202)) == expected,
            "small logical-to-native bias must be removed from a real move");
    const auto change = controller.commitTarget();
    require(change.isValid() && change.positionChanged && !change.sizeChanged &&
                change.geometry == expected,
            "the exact physical move must commit once");
}

void nativeManagedMoveProposalsRemainAvailable() {
    auto controller = initializedController();
    require(controller.beginMove(QPoint(1664, 1173)), "move transaction did not begin");
    const QRect nativeManagedProposal(0, 0, 1600, 900);
    require(controller.updateMove(nativeManagedProposal, QPoint(1665, 1174)) ==
                nativeManagedProposal,
            "materially different native move proposals must be preserved");
}

void resizingUsesTheTransactionStartAsItsFixedReference() {
    auto controller = initializedController();
    require(controller.beginResize(resize_geometry::DragHandle::TopLeft),
            "resize transaction did not begin");
    const auto target =
        controller.updateResize(QRect(900, 700, 1400, 800), resize_geometry::DragHandle::TopLeft,
                                QSize(1298, 737), 0.1, 5.0);
    require(target.has_value(), "resize proposal was rejected");
    require(target->bottomRight() == QRect(1015, 805, 1298, 737).bottomRight(),
            "resize must preserve the opposite transaction-start anchor");
    require(controller.finishInteractiveTarget() == *target,
            "validated resize target must remain authoritative");
}

void dpiAndProgrammaticTargetsAreExplicitTransactions() {
    auto controller = initializedController();
    // The system owns the geometry of a monitor transition: the suggested
    // rect is adopted verbatim, position included.
    const QRect dpiTarget(1200, 900, 1623, 921);
    require(controller.adoptDpiTarget(dpiTarget, std::nullopt), "DPI target was rejected");
    require(controller.constrainWindowPos(QRect(1201, 901, 1624, 922), true, true) == dpiTarget,
            "DPI transaction target must reject later rounded proposals");
    const auto dpiChange = controller.commitTarget();
    require(dpiChange.dpiChanged && dpiChange.sizeChanged && dpiChange.positionChanged &&
                dpiChange.geometry == dpiTarget,
            "DPI target must commit exactly as the system suggested it");

    const QRect scaleTarget(1015, 805, 1298, 737);
    require(controller.beginProgrammatic(scaleTarget,
                                         ScreenshotPinnedNativeGeometryController::Origin::Scale),
            "programmatic scale transaction did not begin");
    require(controller.constrainWindowPos(QRect(1201, 901, 1299, 738), true, true) == scaleTarget,
            "programmatic geometry must remain authoritative during reentrant messages");
    require(controller.commitTarget().geometry == scaleTarget,
            "programmatic geometry did not commit");
}

void midDragDpiTargetKeepsTheSystemGeometry() {
    auto controller = initializedController();
    require(controller.beginMove(QPoint(1664, 1173)), "move transaction did not begin");
    const QRect moved(1100, 900, 1298, 737);
    require(controller.updateMove(moved, QPoint(1749, 1268)) == moved,
            "move transaction did not track the cursor");
    const QRect dpiTarget(1120, 920, 1623, 921);
    require(controller.adoptDpiTarget(dpiTarget, QPoint(1749, 1268)),
            "mid-drag DPI target was rejected");
    require(controller.targetGeometry() == dpiTarget,
            "a mid-drag DPI transition must adopt the system geometry verbatim");
    require(controller.finishInteractiveTarget() == dpiTarget,
            "the drag must finish at the system-provided DPI geometry");
    const auto change = controller.commitTarget();
    require(change.dpiChanged && change.sizeChanged && change.geometry == dpiTarget,
            "the system DPI geometry must commit as a size-changing transaction");
}

void shortcutMovementAfterDpiUsesTheAdoptedTargetAsItsAnchor() {
    {
        auto pendingController = initializedController();
        const QPoint cursor(1664, 1173);
        require(pendingController.beginMove(cursor), "pending move transaction did not begin");
        const QRect dpiTarget(1035, 825, 1623, 921);
        require(!pendingController.adoptDpiTarget(dpiTarget, std::nullopt),
                "move-phase DPI adoption must require a cursor reference");
        require(pendingController.adoptDpiTarget(dpiTarget, cursor),
                "pending move DPI target was rejected");
        require(pendingController.updateMove({}, cursor + QPoint(1, 0)) ==
                    dpiTarget.translated(1, 0),
                "pending cursor movement must continue from the adopted DPI target");
    }

    auto controller = initializedController();
    const QPoint dragStartCursor(1664, 1173);
    const QPoint dpiCursor(1749, 1268);
    require(controller.beginMove(dragStartCursor), "move transaction did not begin");
    require(controller.updateMove(QRect(1100, 900, 1298, 737), dpiCursor) ==
                QRect(1100, 900, 1298, 737),
            "move transaction did not track the cursor before the DPI transition");

    const QRect dpiTarget(1120, 920, 1623, 921);
    require(controller.adoptDpiTarget(dpiTarget, dpiCursor),
            "mid-drag DPI target was rejected");
    QRect expected = dpiTarget;
    expected.translate(1, 0);
    require(controller.updateMove({}, dpiCursor + QPoint(1, 0)) == expected,
            "cursor-derived movement must continue from the adopted DPI target");

    controller.prepareRollback();
    require(controller.targetGeometry() == QRect(1015, 805, 1298, 737),
            "rebasing movement must not replace the transaction rollback geometry");
    require(controller.finishRollback().geometry == QRect(1015, 805, 1298, 737),
            "a rebased move must still roll back to the original committed geometry");
}

void failedTransactionsRollBackDeterministically() {
    auto controller = initializedController();
    require(
        controller.beginProgrammatic(QRect(900, 600, 800, 450),
                                     ScreenshotPinnedNativeGeometryController::Origin::Thumbnail),
        "programmatic transaction did not begin");
    controller.prepareRollback();
    require(controller.targetGeometry() == QRect(1015, 805, 1298, 737),
            "rollback must target the last committed geometry");
    const auto restored = controller.finishRollback();
    require(restored.geometry == QRect(1015, 805, 1298, 737) &&
                controller.phase() == ScreenshotPinnedNativeGeometryController::Phase::Stable,
            "rollback must return the controller to stable state");
}
} // namespace

int main() {
    try {
        passiveProposalsCannotChangeStableGeometry();
        clickWithoutMovementRestoresTheExactStart();
        realMovementUsesPhysicalCursorDelta();
        externallyInitiatedMovementEstablishesAnExplicitTransaction();
        nativeManagedMoveProposalsRemainAvailable();
        resizingUsesTheTransactionStartAsItsFixedReference();
        dpiAndProgrammaticTargetsAreExplicitTransactions();
        midDragDpiTargetKeepsTheSystemGeometry();
        shortcutMovementAfterDpiUsesTheAdoptedTargetAsItsAnchor();
        failedTransactionsRollBackDeterministically();
    } catch (const std::exception& error) {
        std::cerr << "screenshot pinned native geometry controller test failure: " << error.what()
                  << '\n';
        return 1;
    }

    std::cout << "screenshot pinned native geometry controller tests passed\n";
    return 0;
}
