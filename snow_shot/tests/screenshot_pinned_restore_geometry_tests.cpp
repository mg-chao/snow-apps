#include "screenshotpinnedrestoregeometry.h"

#include <QtGlobal>

#include <iostream>
#include <stdexcept>

namespace {
namespace restore_geometry = screenshot_pinned_restore_geometry;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

restore_geometry::ScreenGeometry screen(QRect physicalBounds, QRect availableBounds) {
    restore_geometry::ScreenGeometry geometry;
    geometry.physicalBounds = physicalBounds;
    geometry.availableBounds = availableBounds;
    return geometry;
}

restore_geometry::SavedState saved(QRect nativeGeometry, QRect screenPhysicalBounds,
                                   QRect preThumbnailNativeGeometry = QRect()) {
    restore_geometry::SavedState state;
    state.nativeGeometry = nativeGeometry;
    state.preThumbnailNativeGeometry = preThumbnailNativeGeometry;
    state.screenPhysicalBounds = screenPhysicalBounds;
    return state;
}

void sameScreenRestorePreservesGeometryExactly() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(120, 80), QSize(1092, 547)), QRect(0, 0, 1920, 1080),
              QRect(QPoint(200, 100), QSize(300, 150))),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(120, 80), QSize(1092, 547)),
            "same-screen restore should preserve the native geometry");
    require(restored.preThumbnailNativeGeometry == QRect(QPoint(200, 100), QSize(300, 150)),
            "same-screen restore should preserve the pre-thumbnail geometry");
}

void higherTargetDpiKeepsEveryPhysicalPixel() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(0, 0, 2880, 1620), QRect(0, 0, 2880, 1570));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    // A window saved at 100% on a 1x monitor and restored on a 1.5x monitor
    // keeps its saved physical pixels; the scale value derives from those
    // pixels, so the monitor DPI cannot change it.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(200, 100), QSize(400, 200)), QRect(0, 0, 1920, 1080),
              QRect(QPoint(40, 20), QSize(800, 400))),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(200, 100), QSize(400, 200)),
            "restoring onto a higher-DPI screen should keep the saved size and offset");
    require(restored.preThumbnailNativeGeometry == QRect(QPoint(40, 20), QSize(800, 400)),
            "restoring onto a higher-DPI screen should keep the pre-thumbnail pixels too");
}

void lowerTargetDpiKeepsEveryPhysicalPixel() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    // Saved at 50% of an 800 px basis on a 2x monitor; the restore must not
    // shrink the physical size to a lower-DPI monitor.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(300, 200), QSize(400, 200)), QRect(0, 0, 3840, 2160)), target, screens);
    require(restored.nativeGeometry == QRect(QPoint(300, 200), QSize(400, 200)),
            "restoring onto a lower-DPI screen should keep size and relative offset");
}

void differentScreenOriginRebasesOffsetUnscaled() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(5000, 200, 2560, 1440), QRect(5000, 200, 2560, 1400));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    // The saved monitor sits left of the target; the relative offset to the
    // saved origin survives verbatim while the anchor moves.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(1920 + 240, 130), QSize(400, 200)), QRect(1920, 0, 3840, 2160)), target,
        screens);
    require(restored.nativeGeometry == QRect(QPoint(5240, 330), QSize(400, 200)),
            "a changed monitor layout should re-base the offset without scaling it");
}

void emptyPreThumbnailGeometryStaysEmpty() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(100, 60), QSize(200, 100)), QRect(0, 0, 3840, 2160)), target, screens);
    require(restored.preThumbnailNativeGeometry.isEmpty(),
            "an absent optional geometry should stay absent after reconciliation");
}

void missingSavedScreenBoundsSkipsEveryTranslation() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(100, 60), QSize(200, 100)), QRect(),
              QRect(QPoint(10, 10), QSize(400, 200))),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(100, 60), QSize(200, 100)),
            "legacy records without saved screen bounds should not be translated");
    require(restored.preThumbnailNativeGeometry == QRect(QPoint(10, 10), QSize(400, 200)),
            "legacy pre-thumbnail geometry should not be translated");
}

void offscreenRestoredGeometryMovesOntoTargetWorkArea() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(5000, 0, 2000, 1000), QRect(5000, 0, 2000, 1000));
    const restore_geometry::ScreenGeometry other =
        screen(QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040));
    const QList<restore_geometry::ScreenGeometry> screens{target, other};
    // Saved at a relative (-500, -500) from the saved screen origin, which
    // lands above and left of every current screen after re-basing.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(-500, -500), QSize(100, 50)), QRect(0, 0, 1920, 1080)), target, screens);
    require(restored.nativeGeometry == QRect(QPoint(5000, 0), QSize(100, 50)),
            "geometry restored off every screen should move into the target work area");
}

void geometryWiderThanWorkAreaAlignsWithWorkAreaOrigin() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(5000, 0, 2000, 1000), QRect(5000, 0, 2000, 1000));
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(9000, 5000), QSize(3000, 800)), QRect(0, 0, 3840, 2160)), target,
        screens);
    require(restored.nativeGeometry == QRect(QPoint(5000, 200), QSize(3000, 800)),
            "geometry wider than the work area should align with the work area origin");
}
} // namespace

int main() {
    try {
        sameScreenRestorePreservesGeometryExactly();
        higherTargetDpiKeepsEveryPhysicalPixel();
        lowerTargetDpiKeepsEveryPhysicalPixel();
        differentScreenOriginRebasesOffsetUnscaled();
        emptyPreThumbnailGeometryStaysEmpty();
        missingSavedScreenBoundsSkipsEveryTranslation();
        offscreenRestoredGeometryMovesOntoTargetWorkArea();
        geometryWiderThanWorkAreaAlignsWithWorkAreaOrigin();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
