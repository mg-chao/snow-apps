#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QEvent>
#include <QMargins>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QToolButton>
#include <QVector>

#include "icon_core.h"
#include "input_style.h"

class QEnterEvent;
class QFont;
class QPaintEvent;
class QWidget;

namespace adqt::widgets::detail::input_internal {

struct InputFramePaintStyle {
  QColor background;
  QColor border;
  qreal borderWidth = 1.0;
  qreal topLeftRadius = 0.0;
  qreal topRightRadius = 0.0;
  qreal bottomRightRadius = 0.0;
  qreal bottomLeftRadius = 0.0;
  bool underlined = false;
  bool joinedLeft = false;
  bool joinedRight = false;
};

struct PreservedScrollPosition {
  QPointer<QAbstractScrollArea> area;
  int horizontalValue = 0;
  int verticalValue = 0;
};

class InputIconButton final : public QToolButton {
 public:
  explicit InputIconButton(QWidget* parent = nullptr);

  void setContentAlignment(Qt::Alignment alignment);
  Qt::Alignment contentAlignment() const;

  void setSlotSize(const QSize& size);
  QSize slotSize() const;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 protected:
  bool event(QEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

 private:
  QSize effectiveSlotSize() const;

  Qt::Alignment contentAlignment_ = Qt::AlignCenter;
  QSize slotSize_;
};

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs);
QPoint mouseEventPos(const QMouseEvent* event);
bool isLeftMouseActivationEvent(const QEvent* event);
QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft);
QColor parseThemeColor(const QColor& value, const QColor& fallback);
QColor compositeOn(const QColor& foreground, const QColor& background);
QPixmap renderTintedIcon(const adqt::icons::IconRef& token, const QColor& color, int side,
                         qreal dpr);
int boundedCursorPosition(const QString& text, int requested);
int lineHeightForFont(const QFont& font);
QMargins textControlContentMargins(const adqt::widgets::detail::InputVisualStyle& style);
int textAreaRowPaddingExtra(const adqt::widgets::detail::InputVisualStyle& style);
int textAreaScrollBarHoverThickness();
QRectF joinedBorderRect(const QRect& bounds, qreal borderWidth, bool joinedLeft, bool joinedRight);
qreal snapToDevicePixelCoord(qreal value, qreal dpr);
QRectF snapRectToDevicePixels(const QRectF& rect, qreal dpr);
void paintInputFrame(QPainter* painter, const QRect& bounds, const InputFramePaintStyle& style);
QVector<PreservedScrollPosition> captureAncestorScrollPositions(QWidget* widget);
void restoreAncestorScrollPositions(const QVector<PreservedScrollPosition>& positions);
void restoreAncestorScrollPositionsDeferred(QWidget* owner,
                                            const QVector<PreservedScrollPosition>& positions);
void updateInputFocusOverlay(const QWidget* owner, const QRect& frameRect,
                             const adqt::widgets::detail::InputVisualStyle& style, bool joinedLeft,
                             bool joinedRight);

constexpr int kTextAreaScrollBarBaseThickness = 8;
constexpr int kTextAreaScrollBarCollapsedVisualThickness = 3;

}  // namespace adqt::widgets::detail::input_internal
