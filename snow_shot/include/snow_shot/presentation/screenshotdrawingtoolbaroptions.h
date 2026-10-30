// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
namespace snow_shot::presentation {
inline ScreenshotToolPalette::Options screenshotDrawingToolbarOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showHistoryActions = true;
    options.showMoveTool = true;
    options.moveToolPresentation = ScreenshotToolPalette::MoveToolPresentation::ResizeWindow;
    options.showSelectTool = true;
    options.showShapeTool = true;
    options.showSpeechBubbleTool = true;
    options.showArrowTool = true;
    options.showLineTool = true;
    options.showDistanceTool = true;
    options.showAngleTool = true;
    options.showBracketTool = true;
    options.showFreeDrawTool = true;
    options.showHighlightTool = true;
    options.showSpotlightTool = true;
    options.showMagnifierTool = true;
    options.showEraserTool = true;
    options.showFilterTool = true;
    options.showWatermarkTool = true;
    options.showTextTool = true;
    options.showSerialNumberTool = true;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.showQrTool = true;
    options.showImageConversionTools = true;
    options.toolbarLayout = snow_shot::storage::ScreenshotToolbarSettings().layout(
        snow_shot::storage::ScreenshotToolbarLayoutKind::DrawingTools);
    options.actionToolsLayoutKind =
        snow_shot::storage::ScreenshotToolbarLayoutKind::PinnedActionTools;
    options.actionToolsLayout =
        snow_shot::storage::ScreenshotToolbarSettings().layout(options.actionToolsLayoutKind);
    options.showSaveButton = true;
    options.saveButtonWithResultActions = true;
    options.copyButtonWithNeutralIcon = true;
    options.separatorAfterSelect = true;
    options.separatorBeforeConfirm = true;
    options.showDrawingModeShortcutOnConfirm = true;
    options.actions = ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::ConfirmAction;
    options.styleDefaults = snow_shot::presentation::screenshotCanvasStyleDefaults();
    return options;
}

} // namespace snow_shot::presentation
