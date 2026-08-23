#include "presentation/recording/screenrecordinggeometry.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QtMath>

#include <cstdlib>
#include <iostream>

namespace recording = snow_shot::presentation::recording;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void recordingFrameStaysOutsideTheSelection(qreal scale) {
    const QRectF selection(300.4, 200.8, 641.2, 359.6);
    const recording::ScreenRecordingAreaFrameGeometry geometry =
        recording::screenRecordingAreaFrameGeometry(selection, scale);

    require(QRectF(geometry.windowGeometry).contains(selection),
            "recording frame window should contain the complete selection");
    const QRectF displayedSelection =
        geometry.selectionRect.translated(geometry.windowGeometry.topLeft());
    require(displayedSelection == selection,
            "recording frame interior should exactly match the screenshot selection");
    const QRectF displayedFrame = geometry.frameRect.translated(geometry.windowGeometry.topLeft());
    require(geometry.windowGeometry.contains(displayedFrame.toAlignedRect()),
            "recording border should expand only outside the screenshot selection");
    require(qAbs(selection.left() - displayedFrame.left() - geometry.borderWidth) < 0.0001,
            "recording border should have the requested left thickness");
    require(qAbs(selection.top() - displayedFrame.top() - geometry.borderWidth) < 0.0001,
            "recording border should have the requested top thickness");
    require(qAbs(displayedFrame.left() + displayedFrame.width() - selection.left() -
                 selection.width() - geometry.borderWidth) < 0.0001,
            "recording border should have the requested right thickness");
    require(qAbs(displayedFrame.top() + displayedFrame.height() - selection.top() -
                 selection.height() - geometry.borderWidth) < 0.0001,
            "recording border should have the requested bottom thickness");
    require(qAbs(geometry.borderWidth * scale - 2.0) < 0.0001,
            "recording border should remain two physical pixels on every side");
}

void physicalSelectionMapsToItsLogicalSubregion() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "geometry test requires a primary screen");

    const QRect logicalBounds = screen->geometry();
    const QRect physicalBounds = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect physicalSelection(physicalBounds.left() + physicalBounds.width() / 4,
                                  physicalBounds.top() + physicalBounds.height() / 5,
                                  physicalBounds.width() / 2, physicalBounds.height() / 3);
    const QRectF mappedF =
        ScreenshotGeometryMapper::logicalRectFForPhysicalRect(physicalSelection, screen);
    const QRect mapped =
        ScreenshotGeometryMapper::logicalRectForPhysicalRect(physicalSelection, screen);
    const double scaleX =
        static_cast<double>(logicalBounds.width()) / static_cast<double>(physicalBounds.width());
    const double scaleY =
        static_cast<double>(logicalBounds.height()) / static_cast<double>(physicalBounds.height());
    const int expectedLeft =
        logicalBounds.left() +
        qFloor(static_cast<double>(physicalSelection.left() - physicalBounds.left()) * scaleX);
    const int expectedTop =
        logicalBounds.top() +
        qFloor(static_cast<double>(physicalSelection.top() - physicalBounds.top()) * scaleY);
    const int expectedRight =
        logicalBounds.left() +
        qCeil(static_cast<double>(physicalSelection.left() + physicalSelection.width() -
                                  physicalBounds.left()) *
              scaleX);
    const int expectedBottom =
        logicalBounds.top() +
        qCeil(static_cast<double>(physicalSelection.top() + physicalSelection.height() -
                                  physicalBounds.top()) *
              scaleY);
    const QRect expectedAligned(expectedLeft, expectedTop, expectedRight - expectedLeft,
                                expectedBottom - expectedTop);
    require(mapped == expectedAligned,
            "physical screenshot selection should map to the matching logical subregion");
    const QRectF expectedF(
        QPointF(logicalBounds.left() +
                    static_cast<double>(physicalSelection.left() - physicalBounds.left()) * scaleX,
                logicalBounds.top() +
                    static_cast<double>(physicalSelection.top() - physicalBounds.top()) * scaleY),
        QPointF(logicalBounds.left() +
                    static_cast<double>(physicalSelection.left() + physicalSelection.width() -
                                        physicalBounds.left()) *
                        scaleX,
                logicalBounds.top() +
                    static_cast<double>(physicalSelection.top() + physicalSelection.height() -
                                        physicalBounds.top()) *
                        scaleY));
    require(qAbs(mappedF.left() - expectedF.left()) < 0.0001 &&
                qAbs(mappedF.top() - expectedF.top()) < 0.0001 &&
                qAbs(mappedF.right() - expectedF.right()) < 0.0001 &&
                qAbs(mappedF.bottom() - expectedF.bottom()) < 0.0001,
            "physical selection mapping should preserve fractional high-DPI edges");
    require(mapped != logicalBounds,
            "partial physical selection must not expand to the entire screen");
}

void toolbarUsesBottomRightThenTopRight() {
    const QRect bounds(0, 0, 1920, 1080);
    const QRect toolbarRect(12, 8, 520, 48);
    const QRect upperRegion(500, 200, 700, 400);
    const ScreenshotAnchoredToolbarPlacement bottomRightPlacement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(upperRegion.left() + upperRegion.width(),
                   upperRegion.top() + upperRegion.height()),
            QPoint(upperRegion.left() + upperRegion.width(), upperRegion.top()), toolbarRect,
            bounds, 4, toolbarRect, toolbarRect);
    const QRect bottomRightToolbar = toolbarRect.translated(bottomRightPlacement.contentPosition);
    require(!bottomRightPlacement.usesTopRightPlacement,
            "toolbar should prefer the recording region bottom-right corner");
    require(bottomRightToolbar.right() == upperRegion.left() + upperRegion.width(),
            "bottom-right placement should use the recording region right anchor");
    require(bottomRightToolbar.top() == upperRegion.top() + upperRegion.height() + 5,
            "toolbar should keep the requested gap below the recording region");

    const QRect lowerRegion(500, 930, 700, 120);
    const ScreenshotAnchoredToolbarPlacement topRightPlacement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(lowerRegion.left() + lowerRegion.width(),
                   lowerRegion.top() + lowerRegion.height()),
            QPoint(lowerRegion.left() + lowerRegion.width(), lowerRegion.top()), toolbarRect,
            bounds, 4, toolbarRect, toolbarRect);
    const QRect topRightToolbar = toolbarRect.translated(topRightPlacement.contentPosition);
    require(topRightPlacement.usesTopRightPlacement,
            "toolbar should use top-right when bottom-right lacks vertical space");
    require(topRightToolbar.right() == lowerRegion.left() + lowerRegion.width(),
            "top-right placement should keep the recording region right anchor");
    require(topRightToolbar.bottom() < lowerRegion.top(),
            "top-right placement should remain above the recording region");
}

void toolbarRemainsInsideTheAvailableScreen() {
    const QRect bounds(0, 0, 800, 600);
    const QRect toolbarRect(10, 6, 1000, 48);
    const QRect region(5, 570, 100, 20);
    const ScreenshotAnchoredToolbarPlacement placement =
        ScreenshotGeometryMapper::anchoredToolbarPlacement(
            QPoint(region.left() + region.width(), region.top() + region.height()),
            QPoint(region.left() + region.width(), region.top()), toolbarRect, bounds, 4,
            toolbarRect, toolbarRect);
    const QRect toolbar = toolbarRect.translated(placement.contentPosition);

    require(toolbar.left() == bounds.left() && toolbar.top() >= bounds.top(),
            "oversized toolbar placement should clamp to the screen origin");
    require(toolbar.bottom() <= bounds.bottom(),
            "toolbar placement should clamp vertically to the screen");
}

void encoderCompatibilityNeverShrinksTheSelection() {
    const QRect bounds(0, 0, 1920, 1080);
    const QRect oddSelection(100, 80, 641, 359);
    const QRect expanded = recording::screenRecordingCompatibleCaptureRegion(oddSelection, bounds);
    require(expanded.contains(oddSelection),
            "encoder-compatible capture region should retain every selected pixel");
    require(expanded.width() == 642 && expanded.height() == 360,
            "odd capture dimensions should expand to the next even dimensions");
    require(expanded.topLeft() == oddSelection.topLeft(),
            "capture expansion should prefer the right and bottom edges");

    const QRect edgeSelection(1279, 721, 641, 359);
    const QRect edgeExpanded =
        recording::screenRecordingCompatibleCaptureRegion(edgeSelection, bounds);
    require(edgeExpanded.contains(edgeSelection),
            "screen-edge capture expansion should retain every selected pixel");
    require(edgeExpanded.right() == bounds.right() && edgeExpanded.bottom() == bounds.bottom(),
            "screen-edge capture expansion should stay within physical bounds");
    require(edgeExpanded.width() == 642 && edgeExpanded.height() == 360,
            "screen-edge capture dimensions should remain encoder compatible");
}

void claritySettingsMapToMaximumOutputSizes() {
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("4k")) ==
                QSize(3840, 2160),
            "4K clarity should cap output at 3840x2160");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("2k")) ==
                QSize(2560, 1440),
            "2K clarity should cap output at 2560x1440");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("1080p")) ==
                QSize(1920, 1080),
            "1080p clarity should cap output at 1920x1080");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("720p")) ==
                QSize(1280, 720),
            "720p clarity should cap output at 1280x720");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("480p")) ==
                QSize(854, 480),
            "480p clarity should use an encoder-compatible 16:9 size");
    require(recording::screenRecordingMaximumSizeForClarity(QStringLiteral("invalid")) ==
                QSize(1920, 1080),
            "invalid clarity should use the persisted setting's 1080p default");
}

void clarityBoundsFollowCaptureOrientation() {
    require(recording::screenRecordingOrientedMaximumSize(QSize(1920, 1080), QSize(1080, 1920)) ==
                QSize(1080, 1920),
            "portrait captures should transpose landscape clarity bounds");
    require(recording::screenRecordingOrientedMaximumSize(QSize(1920, 1080), QSize(1920, 1080)) ==
                QSize(1920, 1080),
            "landscape captures should retain landscape clarity bounds");
    require(recording::screenRecordingOrientedMaximumSize(QSize(1080, 1920), QSize(1920, 1080)) ==
                QSize(1920, 1080),
            "landscape captures should transpose portrait bounds");
    require(recording::screenRecordingOrientedMaximumSize(QSize(1920, 1080), QSize(1000, 1000)) ==
                QSize(1920, 1080),
            "square captures should retain the configured orientation");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    physicalSelectionMapsToItsLogicalSubregion();
    recordingFrameStaysOutsideTheSelection(1.0);
    recordingFrameStaysOutsideTheSelection(1.5);
    recordingFrameStaysOutsideTheSelection(2.0);
    toolbarUsesBottomRightThenTopRight();
    toolbarRemainsInsideTheAvailableScreen();
    encoderCompatibilityNeverShrinksTheSelection();
    claritySettingsMapToMaximumOutputSizes();
    clarityBoundsFollowCaptureOrientation();
    return 0;
}
