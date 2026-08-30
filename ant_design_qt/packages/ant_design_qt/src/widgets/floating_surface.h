#pragma once

#include <QColor>
#include <QMargins>
#include <QRegion>
#include <QWidget>

#include <memory>

class QPainter;
class QHideEvent;
class QShowEvent;

namespace adqt::widgets {

class AdFloatingSurfaceTestAccess;

class AdFloatingSurface : public QWidget {
  Q_OBJECT
  friend class AdFloatingSurfaceTestAccess;

  Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius)
  Q_PROPERTY(qreal borderWidth READ borderWidth WRITE setBorderWidth)
  Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor)
  Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor)

 public:
  explicit AdFloatingSurface(QWidget* parent = nullptr);
  ~AdFloatingSurface() override;

  QWidget* contentBody() const;
  void setContentMargins(const QMargins& margins);
  QMargins contentMargins() const;

  qreal cornerRadius() const;
  void setCornerRadius(qreal value);
  qreal borderWidth() const;
  void setBorderWidth(qreal value);
  QColor backgroundColor() const;
  void setBackgroundColor(const QColor& value);
  QColor borderColor() const;
  void setBorderColor(const QColor& value);

  void setShadow(qreal blurRadius, const QPointF& offset, const QColor& color);
  qreal shadowBlurRadius() const;
  QPointF shadowOffset() const;
  QColor shadowColor() const;
  QMargins shadowMargins() const;

  QRect bodyRect() const;
  QRegion interactiveRegion() const;
  bool containsInteractivePoint(const QPointF& point) const;
  QSize sizeHint() const override;

  static void paintCachedShadow(QPainter& painter, const QRectF& bodyRect, qreal cornerRadius,
                                qreal blurRadius, const QPointF& offset, const QColor& color,
                                qreal devicePixelRatio = 1.0);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  struct ShadowCache;

  void updateBodyGeometry();
  void invalidateSurfaceGeometry();

  QWidget* body_ = nullptr;
  QMargins contentMargins_;
  qreal cornerRadius_ = 8.0;
  qreal borderWidth_ = 0.0;
  QColor backgroundColor_ = Qt::white;
  QColor borderColor_ = Qt::transparent;
  qreal shadowBlurRadius_ = 18.0;
  QPointF shadowOffset_ = QPointF(0.0, 3.0);
  QColor shadowColor_ = QColor(0, 0, 0, 90);
  std::unique_ptr<ShadowCache> shadowCache_;
};

}  // namespace adqt::widgets
