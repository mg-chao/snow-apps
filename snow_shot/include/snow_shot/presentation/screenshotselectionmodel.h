#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H

#include "snow_shot/presentation/screenshotselectionaspectratio.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QtGlobal>

class ScreenshotSelectionModel final {
  public:
    enum class RegionOperation { Replace, Add, Subtract };
    void reset();
    ScreenshotRegionType regionType() const {
        return m_regionType;
    }
    void setRegionType(ScreenshotRegionType type) {
        m_regionType = type;
    }
    bool constructionActive() const {
        return m_draftRegion.has_value();
    }
    bool cornerRadiusApplicable() const {
        return !selectionRegion().custom();
    }
    void setDraftRegion(const ScreenshotRegionGeometry& region,
                        const QVector<QPointF>& vertices = {});
    QPainterPath draftPath() const {
        return m_draftRegion ? m_draftRegion->path() : QPainterPath();
    }
    const QVector<QPointF>& draftVertices() const {
        return m_draftVertices;
    }
    void clearDraftRegion();
    void commitDraftRegion(const QRect& canvasBounds = {});
    [[nodiscard]] ScreenshotRegionGeometry selectionRegion() const;
    // Compose a displayed smart-selection frame without changing the capture operand.
    [[nodiscard]] ScreenshotRegionGeometry selectionRegionForMarquee(const QRectF& marquee) const;
    [[nodiscard]] ScreenshotRegionGeometry confirmedRegion() const;
    [[nodiscard]] bool rectangular() const;
    [[nodiscard]] bool regionOperationActive() const;
    [[nodiscard]] RegionOperation regionOperation() const;
    [[nodiscard]] QRectF pendingMarquee() const;
    void beginRegionOperation(RegionOperation operation);
    void commitRegionOperation();
    void cancelRegionOperation();
    void setSelectionRegion(const ScreenshotRegionGeometry& region);
    void setDraggedSelectionRect(const QRectF& rect, ScreenshotSelectionDragMode mode);
    [[nodiscard]] ScreenshotResultStyle resultStyle() const;

    [[nodiscard]] QRectF normalizedSelection() const;
    [[nodiscard]] QRect pixelSelection() const;
    [[nodiscard]] bool hasPixelSelection() const;

    void clearSelection();
    // Stores a selection rectangle as provided. Detection sources (intelligent selection,
    // persisted params) may provide sub-pixel rects that pixel conversion rounds outward.
    // Pointer drags must use setDraggedSelectionRect with the shared drag geometry
    // (draggedScreenshotSelectionRect / grabAdjustedScreenshotSelectionRect) so confirmation
    // preserves any gesture-specific aspect ratio.
    // Returns whether the displayed geometry changed, including replacement of a custom region.
    bool setSelectionRect(const QRectF& selection);
    // Stores a selection spanning the pointer cells under the press and
    // release positions: both cells are inclusive, exactly like a marquee
    // drag, so pointer-seeded state always addresses whole canvas pixels.
    void setSelectionStartEnd(const QPointF& start, const QPointF& end);

    void beginMoveDrag(const QPointF& startPosition);
    // Rebase a move/resize gesture without changing the visible selection.
    // This is used when a transient modifier switches an active resize into
    // moving the whole selection, avoiding a cursor jump.
    void rebaseMoveDrag(const QPointF& startPosition);
    [[nodiscard]] QRectF moveOriginalSelection() const;
    [[nodiscard]] QRectF selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                              const QPointF& position, const QRectF& bounds,
                                              qreal minimumSelectionSize,
                                              qreal lockedAspectRatioOverride = -1.0) const;

    [[nodiscard]] QRectF boundedSelectionRect(const QRectF& selection, const QRectF& bounds,
                                              bool preserveSize, qreal minimumSelectionSize) const;
    [[nodiscard]] bool adjustFromToolbar(int minDx, int minDy, int maxDx, int maxDy,
                                         const QRectF& bounds, qreal minimumSelectionSize);

    [[nodiscard]] int cornerRadius() const;
    [[nodiscard]] int shadowWidth() const;
    [[nodiscard]] QColor shadowColor() const;
    [[nodiscard]] bool aspectRatioLocked() const;
    [[nodiscard]] ScreenshotSelectionAspectRatioPreset aspectRatioPreset() const;

    [[nodiscard]] bool setCornerRadius(int radius);
    [[nodiscard]] bool setShadowWidth(int shadowWidth);
    void setShadowColor(const QColor& color);
    [[nodiscard]] bool setAspectRatioLockEnabled(bool enabled, qreal minimumSelectionSize);
    void toggleAspectRatioLock(qreal minimumSelectionSize);
    [[nodiscard]] bool setAspectRatioPreset(ScreenshotSelectionAspectRatioPreset preset,
                                            const QRectF& bounds, qreal minimumSelectionSize);
    // Commit a drag's already-constrained geometry without reshaping its preview.
    [[nodiscard]] bool setDraggedAspectRatioPreset(ScreenshotSelectionAspectRatioPreset preset);
    [[nodiscard]] bool finalizeAspectRatio(const QRectF& bounds, qreal minimumSelectionSize);
    // Exact incoming geometry remains authoritative without losing a custom lock.
    void clearAspectRatioPresetForReplacement();

    [[nodiscard]] ScreenshotSelectionParams params(const QRect& bounds) const;
    [[nodiscard]] bool applyParams(const ScreenshotSelectionParams& params, const QRect& bounds);

  private:
    QVector<QPointF> m_draftVertices;
    mutable std::optional<ScreenshotRegionGeometry> m_cachedSelectionRegion;
    ScreenshotRegionType m_regionType = ScreenshotRegionType::Rectangle;
    std::optional<ScreenshotRegionGeometry> m_draftRegion;
    std::optional<ScreenshotRegionGeometry> m_region;
    ScreenshotRegionGeometry m_confirmedRegion;
    ScreenshotRegionGeometry m_moveOriginalRegion;
    RegionOperation m_regionOperation = RegionOperation::Replace;
    QPointF m_start;
    QPointF m_end;
    QPointF m_moveStart;
    QRectF m_moveOriginalSelection;
    int m_cornerRadius = 0;
    int m_shadowWidth = 0;
    QColor m_shadowColor = QColor(0x33, 0x33, 0x33);
    bool m_aspectRatioLockEnabled = false;
    ScreenshotSelectionAspectRatioPreset m_aspectRatioPreset =
        ScreenshotSelectionAspectRatioPreset::Free;
    // Pointer drags already resolve their ratio; confirmation must preserve their geometry.
    bool m_selectionFromDrag = false;
    double m_lockedAspectRatio = 0.0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H
