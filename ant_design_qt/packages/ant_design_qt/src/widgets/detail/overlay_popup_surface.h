#pragma once

#include "../popup_placement.h"
#include "top_level_popup_window.h"

#include <QColor>
#include <QMargins>
#include <QPainterPath>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <memory>

class QPainter;
class QHideEvent;
class QShowEvent;

namespace adqt::widgets::detail {

class OverlayPopupSurfaceTestAccess;

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
  friend class OverlayPopupSurfaceTestAccess;

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
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

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
  void invalidateShadowCache() const;
  void ensurePathCache() const;
  void ensureShadowCache() const;
  void paintSurface(QPainter& painter) const;

  struct PathCache {
    bool valid = false;
    QSize size;
    QPainterPath bubble;
    QPainterPath arrow;
    QPainterPath interactive;
  };

  struct ShadowCache {
    bool valid = false;
    QVector<QPainterPath> paths;
  };

  QPointer<QWidget> bodyWidget_;
  OverlayPopupSurfaceStyle style_;
  OverlayPopupPlacement placement_ = OverlayPopupPlacement::Top;
  ArrowSide arrowSide_ = ArrowSide::Bottom;
  bool arrowVisible_ = true;
  qreal arrowCenter_ = 0.0;
  mutable std::unique_ptr<PathCache> pathCache_;
  mutable std::unique_ptr<ShadowCache> shadowCache_;
};

}  // namespace adqt::widgets::detail
