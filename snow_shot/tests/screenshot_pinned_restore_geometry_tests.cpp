#include "screenshotpinnedrestoregeometry.h"

#include <QtGlobal>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
namespace restore_geometry = screenshot_pinned_restore_geometry;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

restore_geometry::ScreenGeometry screen(QRect physicalBounds, QRect availableBounds,
                                        qreal devicePixelRatio) {
    restore_geometry::ScreenGeometry geometry;
    geometry.physicalBounds = physicalBounds;
    geometry.availableBounds = availableBounds;
    geometry.devicePixelRatio = devicePixelRatio;
    return geometry;
}

restore_geometry::SavedState saved(QRect nativeGeometry, double scalePercent,
                                   QRect screenPhysicalBounds, qreal screenDevicePixelRatio,
                                   QRect preThumbnailNativeGeometry = QRect()) {
    restore_geometry::SavedState state;
    state.nativeGeometry = nativeGeometry;
    state.preThumbnailNativeGeometry = preThumbnailNativeGeometry;
    state.scalePercent = scalePercent;
    state.screenPhysicalBounds = screenPhysicalBounds;
    state.screenDevicePixelRatio = screenDevicePixelRatio;
    return state;
}

// The window derives its scale as 100 * width / basis width. After a
// translation, the restored percent must still describe the restored width
// up to the single pixel that integer geometry can lose to rounding.
bool percentDescribesWidth(double percent, int width, int basisWidth) {
    const double derived = 100.0 * width / basisWidth;
    const double onePixel = 100.0 / basisWidth;
    return std::fabs(percent - derived) <= onePixel;
}

void sameDpiRestorePreservesEveryFieldExactly() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 1920, 1080),
                                                           QRect(0, 0, 1920, 1040), 1.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    // 110% of a 993 px basis is stored as 1092 px, which derives back to
    // 109.97%. The exact percent must survive an unchanged DPI bit for bit,
    // otherwise the wheel step that targets 110% becomes a no-op.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(120, 80), QSize(1092, 547)), 110.0, QRect(0, 0, 1920, 1080), 1.0,
              QRect(QPoint(200, 100), QSize(300, 150))),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(120, 80), QSize(1092, 547)),
            "same-DPI restore should preserve the native geometry");
    require(restored.preThumbnailNativeGeometry == QRect(QPoint(200, 100), QSize(300, 150)),
            "same-DPI restore should preserve the pre-thumbnail geometry");
    require(restored.scalePercent == 110.0,
            "same-DPI restore should preserve the saved scale percent exactly");
}

void higherTargetDpiScalesEveryFieldTogether() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 2880, 1620),
                                                           QRect(0, 0, 2880, 1570), 1.5);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(200, 100), QSize(400, 200)), 100.0, QRect(0, 0, 1920, 1080), 1.0,
              QRect(QPoint(40, 20), QSize(800, 400))),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(300, 150), QSize(600, 300)),
            "restoring onto a higher-DPI screen should scale size and relative position");
    require(restored.preThumbnailNativeGeometry == QRect(QPoint(60, 30), QSize(1200, 600)),
            "restoring onto a higher-DPI screen should scale the pre-thumbnail geometry too");
    require(restored.scalePercent == 150.0,
            "restoring onto a higher-DPI screen should scale the percent by the same ratio");
    require(percentDescribesWidth(restored.scalePercent, restored.nativeGeometry.width(), 400),
            "the restored percent should describe the restored width");
}

void lowerTargetDpiShrinksEveryFieldTogether() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 1920, 1080),
                                                           QRect(0, 0, 1920, 1040), 1.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    // Saved at 50% of an 800 px basis on a 2x monitor.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(300, 200), QSize(400, 200)), 50.0, QRect(0, 0, 3840, 2160), 2.0),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(150, 100), QSize(200, 100)),
            "restoring onto a lower-DPI screen should shrink size and relative position");
    require(restored.scalePercent == 25.0,
            "restoring onto a lower-DPI screen should shrink the percent by the same ratio");
    require(percentDescribesWidth(restored.scalePercent, restored.nativeGeometry.width(), 800),
            "the restored percent should describe the restored width");
}

void percentStaysWithinOnePixelOfTranslatedWidthUnderRounding() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 1920, 1080),
                                                           QRect(0, 0, 1920, 1040), 1.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    // 1092 px halves to 546 px, which derives to 54.98%; the translated exact
    // percent is 55%. Both describe the same window within one pixel.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(0, 0), QSize(1092, 547)), 110.0, QRect(0, 0, 3840, 2160), 2.0),
        target, screens);
    require(restored.nativeGeometry.size() == QSize(546, 274),
            "halving a rounded geometry should round each side independently");
    require(restored.scalePercent == 55.0,
            "the percent should be translated exactly rather than re-derived from pixels");
    require(percentDescribesWidth(restored.scalePercent, restored.nativeGeometry.width(), 993),
            "a translated exact percent should stay within one pixel of the restored width");
}

void emptyPreThumbnailGeometryStaysEmptyWhilePercentIsTranslated() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 1920, 1080),
                                                           QRect(0, 0, 1920, 1040), 1.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(100, 60), QSize(200, 100)), 80.0, QRect(0, 0, 3840, 2160), 2.0),
        target, screens);
    require(restored.preThumbnailNativeGeometry.isEmpty(),
            "an absent optional geometry should stay absent after reconciliation");
    require(restored.scalePercent == 40.0,
            "an absent optional geometry must not suppress the percent translation");
}

void missingSavedScreenBoundsSkipsEveryTranslation() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 1920, 1080),
                                                           QRect(0, 0, 1920, 1040), 2.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(100, 60), QSize(200, 100)), 70.0, QRect(), 1.0,
              QRect(QPoint(10, 10), QSize(400, 200))),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(100, 60), QSize(200, 100)),
            "legacy records without saved screen bounds should not be translated");
    require(restored.preThumbnailNativeGeometry == QRect(QPoint(10, 10), QSize(400, 200)),
            "legacy pre-thumbnail geometry should not be translated");
    require(restored.scalePercent == 70.0,
            "legacy records must keep their percent when their geometry is kept");
}

void nonPositiveSavedDpiFallsBackToUnity() {
    const restore_geometry::ScreenGeometry target = screen(QRect(0, 0, 2880, 1620),
                                                           QRect(0, 0, 2880, 1570), 2.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(100, 60), QSize(200, 100)), 100.0, QRect(0, 0, 1920, 1080), 0.0),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(200, 120), QSize(400, 200)),
            "a corrupt saved DPI should fall back to unity instead of dividing by zero");
    require(restored.scalePercent == 200.0,
            "the percent should use the same unity fallback as the geometry");
}

void offscreenTranslatedGeometryMovesOntoTargetWorkArea() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(5000, 0, 2000, 1000), QRect(5000, 0, 2000, 1000), 1.0);
    const restore_geometry::ScreenGeometry other = screen(QRect(0, 0, 1920, 1080),
                                                          QRect(0, 0, 1920, 1040), 1.0);
    const QList<restore_geometry::ScreenGeometry> screens{target, other};
    // Saved at a relative (-500, -500) from the saved screen origin, which
    // lands above and left of every current screen after translation.
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(-500, -500), QSize(100, 50)), 100.0, QRect(0, 0, 1920, 1080), 1.0),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(5000, 0), QSize(100, 50)),
            "geometry restored off every screen should move into the target work area");
}

void geometryWiderThanWorkAreaAlignsWithWorkAreaOrigin() {
    const restore_geometry::ScreenGeometry target =
        screen(QRect(5000, 0, 2000, 1000), QRect(5000, 0, 2000, 1000), 1.0);
    const QList<restore_geometry::ScreenGeometry> screens{target};
    const restore_geometry::RestoredState restored = restore_geometry::reconcileSavedState(
        saved(QRect(QPoint(9000, 5000), QSize(3000, 800)), 100.0, QRect(0, 0, 3840, 2160), 1.0),
        target, screens);
    require(restored.nativeGeometry == QRect(QPoint(5000, 200), QSize(3000, 800)),
            "geometry wider than the work area should align with the work area origin");
}
} // namespace

int main() {
    try {
        sameDpiRestorePreservesEveryFieldExactly();
        higherTargetDpiScalesEveryFieldTogether();
        lowerTargetDpiShrinksEveryFieldTogether();
        percentStaysWithinOnePixelOfTranslatedWidthUnderRounding();
        emptyPreThumbnailGeometryStaysEmptyWhilePercentIsTranslated();
        missingSavedScreenBoundsSkipsEveryTranslation();
        nonPositiveSavedDpiFallsBackToUnity();
        offscreenTranslatedGeometryMovesOntoTargetWorkArea();
        geometryWiderThanWorkAreaAlignsWithWorkAreaOrigin();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
