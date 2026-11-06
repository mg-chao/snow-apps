// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
namespace snow_shot::presentation {
inline void bindScreenshotCanvasOperations(ScreenshotToolPalette* toolbar, SnowCanvasWidget& canvas,
                                           QObject* owner) {
    QObject::connect(toolbar, &ScreenshotToolPalette::sequenceNumberDecrementRequested, owner,
                     [&canvas]() { canvas.adjustSelectedSerialNumbers(-1); });
    QObject::connect(toolbar, &ScreenshotToolPalette::sequenceNumberIncrementRequested, owner,
                     [&canvas]() { canvas.adjustSelectedSerialNumbers(1); });
    QObject::connect(toolbar, &ScreenshotToolPalette::sequenceNumberCreateTextRequested, owner,
                     [&canvas]() { canvas.createSerialNumberText(); });
    QObject::connect(toolbar, &ScreenshotToolPalette::sendSelectionToBackRequested, owner,
                     [&canvas]() { canvas.reorderSelected(SnowCanvasSelectionOrder::SendToBack); });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::sendSelectionBackwardRequested, owner,
        [&canvas]() { canvas.reorderSelected(SnowCanvasSelectionOrder::SendBackward); });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::bringSelectionForwardRequested, owner,
        [&canvas]() { canvas.reorderSelected(SnowCanvasSelectionOrder::BringForward); });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::bringSelectionToFrontRequested, owner,
        [&canvas]() { canvas.reorderSelected(SnowCanvasSelectionOrder::BringToFront); });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::alignSelectionLeftRequested, owner,
        [&canvas]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignLeft); });
    QObject::connect(toolbar, &ScreenshotToolPalette::alignSelectionCenterHorizontallyRequested,
                     owner, [&canvas]() {
                         canvas.alignSelected(
                             SnowCanvasSelectionAlignment::AlignCenterHorizontally);
                     });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::alignSelectionRightRequested, owner,
        [&canvas]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignRight); });
    QObject::connect(toolbar, &ScreenshotToolPalette::alignSelectionTopRequested, owner,
                     [&canvas]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignTop); });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::alignSelectionCenterVerticallyRequested, owner,
        [&canvas]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignCenterVertically); });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::alignSelectionBottomRequested, owner,
        [&canvas]() { canvas.alignSelected(SnowCanvasSelectionAlignment::AlignBottom); });
    QObject::connect(toolbar, &ScreenshotToolPalette::distributeSelectionHorizontallyRequested,
                     owner, [&canvas]() {
                         canvas.alignSelected(SnowCanvasSelectionAlignment::DistributeHorizontally);
                     });
    QObject::connect(
        toolbar, &ScreenshotToolPalette::distributeSelectionVerticallyRequested, owner,
        [&canvas]() { canvas.alignSelected(SnowCanvasSelectionAlignment::DistributeVertically); });
    QObject::connect(toolbar, &ScreenshotToolPalette::selectionOpacityChanged, owner,
                     [&canvas](qreal opacity) { canvas.setSelectedOpacity(opacity); });
    QObject::connect(toolbar, &ScreenshotToolPalette::duplicateSelectionRequested, owner,
                     [&canvas]() { canvas.duplicateSelected(); });
    QObject::connect(toolbar, &ScreenshotToolPalette::deleteSelectionRequested, owner,
                     [&canvas]() { canvas.deleteSelected(); });
    QObject::connect(toolbar, &ScreenshotToolPalette::resetCanvasRequested, owner,
                     [&canvas]() { canvas.deleteAllElements(); });
}
} // namespace snow_shot::presentation
