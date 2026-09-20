#ifndef SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGSELECTION_H
#define SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGSELECTION_H

#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

namespace snow_shot::presentation::recording {

inline void connectScreenRecordingSelection(ScreenshotToolPalette& palette,
                                            ScreenRecordingAreaWindow& area, QObject& context) {
    auto* canvas = area.canvas();
    QObject::connect(
        &palette, &ScreenshotToolPalette::selectRequested, &context, [&palette, &area, canvas]() {
            if (palette.activeTool() == ScreenshotToolPalette::Tool::Select) {
                canvas->setCanvasTool(SnowCanvasTool::Select);
                area.setInputMode(ScreenRecordingAreaWindow::InputMode::Drawing);
            } else {
                area.setInputMode(palette.recordingExportSettingsVisible()
                                      ? ScreenRecordingAreaWindow::InputMode::RegionEditing
                                      : ScreenRecordingAreaWindow::InputMode::PassThrough);
            }
        });
    QObject::connect(&palette, &ScreenshotToolPalette::recordingExportSettingsVisibleChanged,
                     &context, [&area](bool visible) {
                         area.setInputMode(visible
                                               ? ScreenRecordingAreaWindow::InputMode::RegionEditing
                                               : ScreenRecordingAreaWindow::InputMode::PassThrough);
                     });
    QObject::connect(
        canvas, &SnowCanvasWidget::historyStateChanged, &context,
        [&palette, canvas]() { palette.setHistoryState(canvas->canvasHistoryState()); });
    QObject::connect(
        canvas, &SnowCanvasWidget::styleToolbarStateChanged, &context,
        [&palette, canvas]() { palette.setStyleToolbarState(canvas->canvasStyleToolbarState()); });
    QObject::connect(&palette, &ScreenshotToolPalette::sendSelectionToBackRequested, &context,
                     [canvas]() { canvas->reorderSelected(SnowCanvasSelectionOrder::SendToBack); });
    QObject::connect(
        &palette, &ScreenshotToolPalette::sendSelectionBackwardRequested, &context,
        [canvas]() { canvas->reorderSelected(SnowCanvasSelectionOrder::SendBackward); });
    QObject::connect(
        &palette, &ScreenshotToolPalette::bringSelectionForwardRequested, &context,
        [canvas]() { canvas->reorderSelected(SnowCanvasSelectionOrder::BringForward); });
    QObject::connect(
        &palette, &ScreenshotToolPalette::bringSelectionToFrontRequested, &context,
        [canvas]() { canvas->reorderSelected(SnowCanvasSelectionOrder::BringToFront); });
    QObject::connect(
        &palette, &ScreenshotToolPalette::alignSelectionLeftRequested, &context,
        [canvas]() { canvas->alignSelected(SnowCanvasSelectionAlignment::AlignLeft); });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionCenterHorizontallyRequested,
                     &context, [canvas]() {
                         canvas->alignSelected(
                             SnowCanvasSelectionAlignment::AlignCenterHorizontally);
                     });
    QObject::connect(
        &palette, &ScreenshotToolPalette::alignSelectionRightRequested, &context,
        [canvas]() { canvas->alignSelected(SnowCanvasSelectionAlignment::AlignRight); });
    QObject::connect(&palette, &ScreenshotToolPalette::alignSelectionTopRequested, &context,
                     [canvas]() { canvas->alignSelected(SnowCanvasSelectionAlignment::AlignTop); });
    QObject::connect(
        &palette, &ScreenshotToolPalette::alignSelectionCenterVerticallyRequested, &context,
        [canvas]() { canvas->alignSelected(SnowCanvasSelectionAlignment::AlignCenterVertically); });
    QObject::connect(
        &palette, &ScreenshotToolPalette::alignSelectionBottomRequested, &context,
        [canvas]() { canvas->alignSelected(SnowCanvasSelectionAlignment::AlignBottom); });
    QObject::connect(&palette, &ScreenshotToolPalette::distributeSelectionHorizontallyRequested,
                     &context, [canvas]() {
                         canvas->alignSelected(
                             SnowCanvasSelectionAlignment::DistributeHorizontally);
                     });
    QObject::connect(
        &palette, &ScreenshotToolPalette::distributeSelectionVerticallyRequested, &context,
        [canvas]() { canvas->alignSelected(SnowCanvasSelectionAlignment::DistributeVertically); });
    QObject::connect(&palette, &ScreenshotToolPalette::selectionOpacityChanged, &context,
                     [canvas](qreal opacity) { canvas->setSelectedOpacity(opacity); });
    QObject::connect(&palette, &ScreenshotToolPalette::duplicateSelectionRequested, &context,
                     [canvas]() { canvas->duplicateSelected(); });
    QObject::connect(&palette, &ScreenshotToolPalette::deleteSelectionRequested, &context,
                     [canvas]() { canvas->deleteSelected(); });
    QObject::connect(&palette, &ScreenshotToolPalette::resetCanvasRequested, &context,
                     [canvas]() { canvas->deleteAllElements(); });
}

} // namespace snow_shot::presentation::recording

#endif // SNOW_SHOT_PRESENTATION_RECORDING_SCREENRECORDINGSELECTION_H
