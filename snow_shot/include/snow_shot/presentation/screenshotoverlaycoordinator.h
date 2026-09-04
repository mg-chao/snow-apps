#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYCOORDINATOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYCOORDINATOR_H

#include "snow_shot/presentation/screenshottypes.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotselectorworkflowports.h"
#include "snow_shot/presentation/screenshotoverlaycanvaspresenter.h"
#include "snow_shot/presentation/screenshotoverlaypool.h"
#include "snow_shot/presentation/screenshotoverlayuihost.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QVector>

#include <cstdint>

class ScreenshotColorPickerWidget;
class ScreenshotDisplaySession;
class ScreenshotOverlayEventSink;
class ScreenshotSelectionToolbarCommandSink;
class ScreenshotSelectionToolbarWidget;
class ScreenshotToolbarCommandSink;
class ScreenshotToolbarWindow;
class SnowCanvasRuntime;
namespace snow_shot::presentation {
class WindowShortcutManager;
}

class ScreenshotOverlayCoordinator final : public ScreenshotOverlayExclusionPort {
  public:
    explicit ScreenshotOverlayCoordinator(ScreenshotOverlayEventSink& eventSink,
                                          SnowCanvasRuntime& canvasRuntime,
                                          snow_shot::presentation::WindowShortcutManager&
                                              shortcutManager);
    ~ScreenshotOverlayCoordinator();

    void setToolbarCommandSinks(ScreenshotToolbarCommandSink& toolbarCommands,
                                ScreenshotSelectionToolbarCommandSink& selectionToolbarCommands);

    void prewarmDisplayPool(ScreenshotDisplaySession& displaySession, int displayCount);
    void clearOverlayCanvases(const ScreenshotDisplaySession& displaySession) const;
    void clearDisplays(ScreenshotDisplaySession& displaySession);
    void destroyDisplayPool(ScreenshotDisplaySession& displaySession);
    void resetForNewCapture(ScreenshotDisplaySession& displaySession);
    void prepareDisplayModels(ScreenshotDisplaySession& displaySession);
    void applyDisplayModels(ScreenshotDisplaySession& displaySession);
    [[nodiscard]] bool preparePreCaptureOverlayWindows(ScreenshotDisplaySession& displaySession);
    void showOverlayWindows(const ScreenshotDisplaySession& displaySession,
                            ScreenshotOverlayShowMode mode);
    void hideOverlayWindowsImmediately(const ScreenshotDisplaySession& displaySession);
    void hideOverlayWindows(const ScreenshotDisplaySession& displaySession);
    void updateOverlayState(const ScreenshotDisplaySession& displaySession, const QRectF& selection,
                            int cornerRadius, int shadowWidth, const QColor& shadowColor,
                            bool selectionToolbarHovered, bool selectionHandlesVisible,
                            bool intelligentSelecting, bool manualSelecting, bool dragging);
    void setScrollingCaptureMode(const ScreenshotDisplaySession& displaySession,
                                 const QRectF& selection, bool enabled);
    void updateOverlayCursors(const ScreenshotDisplaySession& displaySession, bool selecting,
                              bool dragging) const;
    void setSelectionMaskColor(const ScreenshotDisplaySession& displaySession,
                               const QColor& color) const;
    void updateGuideLines(const ScreenshotDisplaySession& displaySession,
                          ScreenshotOverlayWindow* owner, const QPointF& localPosition,
                          bool selecting, const QColor& cursorColor,
                          const QColor& monitorCenterColor) const;
    void updateGuideLinesAtGlobalPosition(const ScreenshotDisplaySession& displaySession,
                                          const QPoint& globalPosition, bool selecting,
                                          const QColor& cursorColor,
                                          const QColor& monitorCenterColor) const;
    void clearGuideLines(const ScreenshotDisplaySession& displaySession) const;
    void setOverlayCursor(ScreenshotOverlayWindow* overlay,
                          ScreenshotSelectionDragMode dragMode) const;
    void setCanvasInteractionEnabled(const ScreenshotDisplaySession& displaySession,
                                     bool enabled) const;
    void setCanvasTool(const ScreenshotDisplaySession& displaySession, SnowCanvasTool tool);
    void refreshCanvasCreationStyles(const ScreenshotDisplaySession& displaySession,
                                     const SnowCanvasStyleDefaults& defaults) const;
    [[nodiscard]] bool resetEditingState(const ScreenshotDisplaySession& displaySession) const;
    [[nodiscard]] bool tryCurrentRectangleStyle(const ScreenshotDisplaySession& displaySession,
                                                SnowCanvasShapeStyle* outStyle) const;
    SnowCanvasShapeStyle
    currentRectangleStyle(const ScreenshotDisplaySession& displaySession) const;
    void setShapeStylePatch(const ScreenshotDisplaySession& displaySession,
                            const SnowCanvasShapeStyle& style, quint32 properties,
                            SnowCanvasShapeKind kind);
    void setFilterStyle(const ScreenshotDisplaySession& displaySession,
                        const SnowCanvasFilterStyle& style, quint32 properties);
    void setWatermarkConfig(const ScreenshotDisplaySession& displaySession,
                            const SnowCanvasWatermarkConfig& config);
    void previewWatermarkConfig(const ScreenshotDisplaySession& displaySession,
                                const SnowCanvasWatermarkConfig& config);
    void setSpotlightConfig(ScreenshotDisplaySession& displaySession,
                            const SnowCanvasSpotlightConfig& config);
    void previewSpotlightConfig(ScreenshotDisplaySession& displaySession,
                                const SnowCanvasSpotlightConfig& config);
    void setTextStyle(const ScreenshotDisplaySession& displaySession,
                      const SnowCanvasTextStyle& style);
    void setSerialNumberStyle(const ScreenshotDisplaySession& displaySession,
                              const SnowCanvasSerialNumberStyle& style);
    void adjustSelectedSerialNumbers(const ScreenshotDisplaySession& displaySession, qint64 delta);
    void createTextForSelectedSerialNumber(const ScreenshotDisplaySession& displaySession);
    void reorderSelectedElements(const ScreenshotDisplaySession& displaySession,
                                 SnowCanvasSelectionOrder order);
    void setSelectedElementsOpacity(const ScreenshotDisplaySession& displaySession, qreal opacity);
    void duplicateSelectedElements(const ScreenshotDisplaySession& displaySession);
    void deleteSelectedElements(const ScreenshotDisplaySession& displaySession);

    ScreenshotToolbarWindow* ensureToolbar();
    ScreenshotToolbarWindow* toolbar() const;
    void attachToolbarToOverlay(ScreenshotOverlayWindow* overlay);
    void prewarmToolbarSurface(const ScreenshotDisplaySession& displaySession);
    void undoCanvasEdit();
    void redoCanvasEdit();
    ScreenshotSelectionToolbarWidget* selectionToolbar() const;
    void attachSelectionToolbarToOverlay(ScreenshotOverlayWindow* overlay);
    ScreenshotColorPickerWidget* colorPicker() const;
    void updateColorPicker(ScreenshotOverlayWindow* overlay, const QImage& image,
                           const QRect& physicalRect, const QPoint& physicalPoint,
                           const QPointF& localPosition, qreal opacity);
    void hideColorPicker();
    void setColorPickerCenterGuideLineColor(const QColor& color);
    void updateShortcutHints(ScreenshotOverlayWindow* overlay,
                             const ScreenshotShortcutHintContext& context, qreal opacity,
                             const QRectF& selectionGlobal = {});
    [[nodiscard]] bool screenshotUiContainsGlobalCursor() const;
    [[nodiscard]] bool stepToolbarStrokeWidth(int direction);
    [[nodiscard]] bool stepToolbarSelectionOpacity(int direction);
    [[nodiscard]] bool stepToolbarSpotlightOpacity(int direction);
    [[nodiscard]] bool stepToolbarFilterIntensity(int direction);
    [[nodiscard]] bool stepToolbarPenFilterStrokeWidth(int direction);
    [[nodiscard]] bool stepToolbarWatermarkFontSize(int direction);
    void resetToolbarForNewCapture();
    void hideToolbar();
    void showToolbar();
    void hideSelectionToolbar();
    void showSelectionToolbar();
    void raiseSelectionToolbar();
    void destroyUiResources();

    [[nodiscard]] QVector<std::uintptr_t>
    excludedHwnds(const ScreenshotDisplaySession& displaySession) const override;

  private:
    void clearOverlayCanvas(ScreenshotOverlayWindow* overlay) const;
    void detachOverlayTransientUi(ScreenshotOverlayWindow* overlay);
    void flushDeferredOverlayMaintenance(const ScreenshotDisplaySession& displaySession);

    ScreenshotOverlayUiHost m_uiHost;
    ScreenshotOverlayPool m_overlayPool;
    ScreenshotOverlayCanvasPresenter m_canvasPresenter;
    bool m_overlayMaintenancePending = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYCOORDINATOR_H
