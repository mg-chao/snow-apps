#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYCANVASPRESENTER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYCANVASPRESENTER_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshottypes.h"

#include <QColor>
#include <QPointer>
#include <QPoint>
#include <QRectF>

#include <functional>

class ScreenshotDisplaySession;
class ScreenshotOverlayWindow;

class ScreenshotOverlayCanvasPresenter final {
  public:
    using OverlayFactory = std::function<ScreenshotOverlayWindow*(ScreenshotOverlayWindow*)>;

    explicit ScreenshotOverlayCanvasPresenter(OverlayFactory ensureOverlay);

    void clearOverlayCanvas(ScreenshotOverlayWindow* overlay) const;
    void prepareDisplayModels(ScreenshotDisplaySession& displaySession) const;
    void applyDisplayModels(ScreenshotDisplaySession& displaySession) const;
    void showOverlayWindows(const ScreenshotDisplaySession& displaySession,
                            ScreenshotOverlayShowMode mode) const;
    void updateOverlayState(const ScreenshotDisplaySession& displaySession, const QRectF& selection,
                            int cornerRadius, int shadowWidth, const QColor& shadowColor,
                            bool selectionToolbarHovered, bool selectionHandlesVisible,
                            bool intelligentSelecting, bool manualSelecting, bool dragging) const;
    void updateOverlayCursors(const ScreenshotDisplaySession& displaySession, bool selecting,
                              bool dragging) const;
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
    void setCanvasTool(const ScreenshotDisplaySession& displaySession, SnowCanvasTool tool) const;
    void refreshCanvasCreationStyles(const ScreenshotDisplaySession& displaySession,
                                     const SnowCanvasStyleDefaults& defaults) const;
    [[nodiscard]] bool resetEditingState(const ScreenshotDisplaySession& displaySession) const;
    [[nodiscard]] bool tryCurrentRectangleStyle(const ScreenshotDisplaySession& displaySession,
                                                SnowCanvasShapeStyle* outStyle) const;
    [[nodiscard]] SnowCanvasShapeStyle
    currentRectangleStyle(const ScreenshotDisplaySession& displaySession) const;
    void setShapeStylePatch(const ScreenshotDisplaySession& displaySession,
                            const SnowCanvasShapeStyle& style, quint32 properties,
                            SnowCanvasShapeKind kind) const;
    void setFilterStyle(const ScreenshotDisplaySession& displaySession,
                        const SnowCanvasFilterStyle& style, quint32 properties) const;
    void setWatermarkConfig(const ScreenshotDisplaySession& displaySession,
                            const SnowCanvasWatermarkConfig& config) const;
    void previewWatermarkConfig(const ScreenshotDisplaySession& displaySession,
                                const SnowCanvasWatermarkConfig& config) const;
    void setSpotlightConfig(ScreenshotDisplaySession& displaySession,
                            const SnowCanvasSpotlightConfig& config) const;
    void previewSpotlightConfig(ScreenshotDisplaySession& displaySession,
                                const SnowCanvasSpotlightConfig& config) const;
    void setTextStyle(const ScreenshotDisplaySession& displaySession,
                      const SnowCanvasTextStyle& style) const;
    void setSerialNumberStyle(const ScreenshotDisplaySession& displaySession,
                              const SnowCanvasSerialNumberStyle& style) const;
    void adjustSelectedSerialNumbers(const ScreenshotDisplaySession& displaySession,
                                     qint64 delta) const;
    void createTextForSelectedSerialNumber(const ScreenshotDisplaySession& displaySession) const;
    void reorderSelectedElements(const ScreenshotDisplaySession& displaySession,
                                 SnowCanvasSelectionOrder order) const;
    void setSelectedElementsOpacity(const ScreenshotDisplaySession& displaySession,
                                    qreal opacity) const;
    void duplicateSelectedElements(const ScreenshotDisplaySession& displaySession) const;
    void deleteSelectedElements(const ScreenshotDisplaySession& displaySession) const;

  private:
    OverlayFactory m_ensureOverlay;
    mutable QPointer<ScreenshotOverlayWindow> m_guideLineOwner;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYCANVASPRESENTER_H
