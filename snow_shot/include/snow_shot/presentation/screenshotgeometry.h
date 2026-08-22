#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTGEOMETRY_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QtGlobal>
#include <QVector>

class QScreen;
class ScreenshotDisplaySession;

struct ScreenshotHalfOpenRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    static ScreenshotHalfOpenRect fromRect(const QRect& rect);
    static ScreenshotHalfOpenRect fromRectF(const QRectF& rect);
    static ScreenshotHalfOpenRect fromEdges(double left, double top, double right, double bottom);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] double width() const;
    [[nodiscard]] double height() const;
    [[nodiscard]] QPointF topLeft() const;
    [[nodiscard]] QPointF bottomRight() const;
    [[nodiscard]] QPointF center() const;
    [[nodiscard]] QRectF toRectF() const;
    [[nodiscard]] QRect toAlignedQRect() const;
    [[nodiscard]] bool contains(const QPointF& point) const;
    [[nodiscard]] bool intersects(const ScreenshotHalfOpenRect& other) const;
    [[nodiscard]] ScreenshotHalfOpenRect intersected(const ScreenshotHalfOpenRect& other) const;
    [[nodiscard]] ScreenshotHalfOpenRect united(const ScreenshotHalfOpenRect& other) const;
    [[nodiscard]] ScreenshotHalfOpenRect translated(const QPointF& delta) const;
};

struct ScreenshotPinnedImageGeometry {
    QRect nativeGeometry;
    QRectF canvasSourceRect;
    QSize initialPhysicalSize;
};

struct ScreenshotPinnedImagePlacement {
    ScreenshotPinnedImageGeometry geometry;
    QPointer<QScreen> screen;
    bool valid = false;
};

struct ScreenshotPinnedImageFit {
    QRect nativeGeometry;
    QSize fullResolutionSize;
    double scalePercent = 0.0;
    bool valid = false;
};

struct ScreenshotAnchoredToolbarPlacement {
    QPoint contentPosition;
    bool usesTopRightPlacement = false;
};

struct ScreenshotDisplayViewportGeometry {
    QRectF canvasRect;
    QPointF canvasCenter;
    qreal canvasToLogicalScale = 1.0;
    QRect logicalRect;
    bool valid = false;
};

struct ScreenshotDisplayPlacementGeometry {
    QScreen* screen = nullptr;
    QRect logicalBounds;
    QRect physicalBounds;
    bool valid = false;
};

class ScreenshotGeometryMapper final {
  public:
    void rebuild(ScreenshotDisplaySession& displaySession);
    void clear();

    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] QPoint canvasOrigin() const;
    [[nodiscard]] QRectF canvasBounds() const;
    [[nodiscard]] QRectF physicalBounds() const;

    [[nodiscard]] const CapturedDisplayModel*
    displayForOverlay(const ScreenshotDisplaySession& displaySession,
                      const ScreenshotOverlayWindow* overlay) const;
    [[nodiscard]] const CapturedDisplayModel*
    displayForPhysicalPoint(const ScreenshotDisplaySession& displaySession,
                            const QPointF& point) const;
    [[nodiscard]] const CapturedDisplayModel*
    displayForCanvasPoint(const ScreenshotDisplaySession& displaySession,
                          const QPointF& point) const;
    [[nodiscard]] const CapturedDisplayModel*
    displayForCanvasRect(const ScreenshotDisplaySession& displaySession, const QRectF& rect) const;

    [[nodiscard]] QPointF
    canvasPositionForOverlayLocalPoint(const ScreenshotDisplaySession& displaySession,
                                       const ScreenshotOverlayWindow* overlay,
                                       const QPointF& localPosition) const;
    [[nodiscard]] QPointF logicalPositionForCanvasPoint(const CapturedDisplayModel& display,
                                                        const QPointF& point) const;
    [[nodiscard]] QPointF logicalPositionForPhysicalPoint(const CapturedDisplayModel& display,
                                                          const QPointF& point) const;
    [[nodiscard]] QPoint
    physicalPositionForCanvasPoint(const ScreenshotDisplaySession& displaySession,
                                   const QPointF& point) const;
    [[nodiscard]] QPointF
    canvasPositionForPhysicalPoint(const ScreenshotDisplaySession& displaySession,
                                   const QPointF& point) const;
    [[nodiscard]] QRectF canvasRectForPhysicalRect(const ScreenshotDisplaySession& displaySession,
                                                   const QRectF& rect) const;
    [[nodiscard]] QPoint
    physicalPositionForLogicalPoint(const ScreenshotDisplaySession& displaySession,
                                    const QPointF& point) const;

    [[nodiscard]] QPoint clampPhysicalPointToDesktop(const QPoint& point) const;
    [[nodiscard]] QPoint clampPhysicalPointToDisplay(const CapturedDisplayModel& display,
                                                     const QPoint& point) const;

    [[nodiscard]] static qreal canvasToLogicalScale(const CapturedDisplayModel& display);
    [[nodiscard]] static QRectF displayCanvasRect(const CapturedDisplayModel& display);
    [[nodiscard]] static QRectF displayImageSourceCanvasRect(const CapturedDisplayModel& display);
    [[nodiscard]] static ScreenshotDisplayViewportGeometry
    displayViewportGeometry(const CapturedDisplayModel& display);
    [[nodiscard]] static ScreenshotDisplayPlacementGeometry
    displayPlacementGeometry(const CapturedDisplayModel* display,
                             const QRect& fallbackLogicalBounds = QRect());
    [[nodiscard]] static QRect physicalRectForScreen(const QScreen& screen);
    [[nodiscard]] static QRectF logicalRectFForPhysicalRect(const QRect& rect,
                                                            const QScreen* screen);
    [[nodiscard]] static QRect logicalRectForPhysicalRect(const QRect& rect, const QScreen* screen);
    [[nodiscard]] static QRect nativeRectForLogicalRect(const QRect& logicalRect,
                                                        const QRect& ownerLogicalBounds,
                                                        const QRect& ownerPhysicalBounds);
    [[nodiscard]] static ScreenshotPinnedImageFit fitImageToAvailableGeometry(
        const QSize& fullResolutionSize, const QRect& availableLogicalGeometry,
        const QRect& screenLogicalGeometry, const QRect& screenNativeGeometry,
        int logicalMargin = 16);
    [[nodiscard]] static ScreenshotPinnedImageFit centerImageAtFullResolution(
        const QSize& fullResolutionSize, const QRect& availableLogicalGeometry,
        const QRect& screenLogicalGeometry, const QRect& screenNativeGeometry);
    [[nodiscard]] static QPoint clampContentPositionToRect(const QPoint& desiredPosition,
                                                           const QRect& contentRect,
                                                           const QRect& bounds);
    [[nodiscard]] static QPoint cursorPanelPosition(const QPoint& cursorPosition,
                                                     const QSize& panelSize,
                                                     const QRect& bounds, int gap);
    [[nodiscard]] static ScreenshotAnchoredToolbarPlacement
    anchoredToolbarPlacement(const QPoint& bottomRightAnchor, const QPoint& topRightAnchor,
                             const QRect& toolbarOccupiedRect, const QRect& bounds, int gap,
                             const QRect& topRightToolbarRect = QRect(),
                             const QRect& topRightReservedRect = QRect());
    [[nodiscard]] static QPointF
    nativePositionForLogicalPlacementPoint(const QPointF& logicalPosition,
                                           const QRect& ownerLogicalBounds,
                                           const QRect& ownerPhysicalBounds);
    [[nodiscard]] static QPointF
    physicalPositionForLogicalDragPoint(const QPointF& logicalPosition,
                                        const QRect& ownerLogicalBounds,
                                        const QRect& ownerPhysicalBounds);
    [[nodiscard]] static QPointF
    logicalPositionForPhysicalDragPoint(const QPointF& physicalPosition,
                                        const QRect& ownerLogicalBounds,
                                        const QRect& ownerPhysicalBounds);
    [[nodiscard]] static QPointF logicalPositionForPhysicalPointInBounds(
        const QPointF& physicalPosition, const QRect& logicalBounds, const QRect& physicalBounds);
    [[nodiscard]] static QPointF logicalDragPositionForPhysicalPoint(
        const QPointF& globalLogicalPosition, const QPointF& physicalPosition,
        const QRect& ownerLogicalBounds, const QRect& ownerPhysicalBounds);
    [[nodiscard]] static ScreenshotPinnedImageGeometry
    pinnedImageGeometry(const QRect& nativeWindowGeometry, const QSize& imagePixelSize);
    [[nodiscard]] ScreenshotPinnedImagePlacement
    pinnedImagePlacement(const ScreenshotDisplaySession& displaySession,
                         const QRect& canvasSelection, const QSize& imagePixelSize,
                         int shadowPadding) const;
    [[nodiscard]] static QScreen* screenForCaptureDisplay(const QString& name,
                                                          const QRect& physicalRect);
    [[nodiscard]] static QScreen* screenForPhysicalRect(const QRect& rect);

  private:
    QPoint m_canvasOrigin;
    QRectF m_canvasBounds;
    QRectF m_physicalBounds;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTGEOMETRY_H
