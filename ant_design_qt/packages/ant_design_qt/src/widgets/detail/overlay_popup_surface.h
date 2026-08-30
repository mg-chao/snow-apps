#pragma once

#include "../popup_placement.h"
#include "top_level_popup_window.h"

#include <QColor>
#include <QMargins>
#include <QPainterPath>
#include <QPointer>
#include <QVector>
#include <QWidget>

class QPainter;

namespace adqt::widgets::detail {

struct OverlayPopupSurfaceMetrics {
  int borderRadius = 8;
  int borderWidth = 1;
  int arrowSize = 8;
};

struct OverlayPopupSurfaceStyle {
  QColor background;
  QColor borderColor;
  QColor arrowBackground;
  QColor arrowBorderColor;
  OverlayPopupSurfaceMetrics metrics;
};

class OverlayPopupSurface final : public QWidget, public TopLevelToolResourceReleaser {
 public:
  explicit OverlayPopupSurface(QWidget* parent = nullptr);

  void releaseTopLevelToolResources() override { destroy(); }

  QWidget* bodyWidget() const { return bodyWidget_; }

  QMargins shadowMargins() const;
  QSize visualSizeHint() const;
  OverlayPopupSurfaceStyle surfaceStyle() const { return style_; }
  void setSurfaceStyle(const OverlayPopupSurfaceStyle& style);

  bool arrowVisible() const { return arrowVisible_; }
  void setArrowVisible(bool visible);

  OverlayPopupPlacement placement() const { return placement_; }
  void setPlacement(OverlayPopupPlacement placement);

  qreal arrowCenter() const { return arrowCenter_; }
  void setArrowCenter(qreal center);

  bool containsInteractiveLocalPos(const QPointF& pos) const;
  bool containsInteractiveGlobalPos(const QPoint& pos) const;

  QSize sizeHint() const override;

 protected:
  bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
  void resizeEvent(QResizeEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

 private:
  enum class ArrowSide {
    None,
    Top,
    Bottom,
    Left,
    Right,
  };

  static ArrowSide arrowSideForPlacement(OverlayPopupPlacement placement);
  int arrowProjection() const;
  qreal arrowBaseHalfWidth() const;
  qreal arrowBaseInsetForUnion() const;
  QRectF bubbleRectForPaint() const;
  qreal clampedArrowCenter(const QRectF& bubbleRect) const;
  QPolygonF arrowPolygon(const QRectF& bubbleRect) const;
  void updateBodyGeometry();
  void invalidatePathCache() const;
  void ensurePathCache() const;
  void paintSurface(QPainter& painter) const;

  QPointer<QWidget> bodyWidget_;
  OverlayPopupSurfaceStyle style_;
  OverlayPopupPlacement placement_ = OverlayPopupPlacement::Top;
  ArrowSide arrowSide_ = ArrowSide::Bottom;
  bool arrowVisible_ = true;
  qreal arrowCenter_ = 0.0;
  mutable bool pathCacheValid_ = false;
  mutable QSize pathCacheSize_;
  mutable QPainterPath bubblePathCache_;
  mutable QPainterPath arrowPathCache_;
  mutable QPainterPath interactivePathCache_;
  mutable QVector<QPainterPath> shadowPathCache_;
};

}  // namespace adqt::widgets::detail
