#pragma once

#include <QColor>
#include <QEnterEvent>
#include <QHideEvent>
#include <QMoveEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>

#include <memory>
#include <optional>

#include "icon_core.h"
#include "control_scale.h"

class QMenu;

namespace adqt::widgets {

class AdButton;

namespace detail {
class BusyIndicatorSurface;
struct ButtonStyleInput;
struct ButtonVisualStyle;
struct ButtonStateStyle;
enum class SegmentPosition : unsigned char;
void setButtonSegmentPosition(AdButton* button, SegmentPosition value);
SegmentPosition buttonSegmentPosition(const AdButton* button);
}  // namespace detail

class AdButton : public QPushButton, public AdControlScaleParticipant {
  Q_OBJECT

  Q_PROPERTY(
      ButtonStyle buttonStyle READ buttonStyle WRITE setButtonStyle NOTIFY buttonStyleChanged)
  Q_PROPERTY(AccentRole accentRole READ accentRole WRITE setAccentRole NOTIFY accentRoleChanged)
  Q_PROPERTY(Shape shape READ shape WRITE setShape NOTIFY shapeChanged)
  Q_PROPERTY(SizeClass sizeClass READ sizeClass WRITE setSizeClass NOTIFY sizeClassChanged)
  Q_PROPERTY(bool interactionBackgroundVisible READ interactionBackgroundVisible WRITE
                 setInteractionBackgroundVisible NOTIFY interactionBackgroundVisibleChanged)
  Q_PROPERTY(bool busy READ busy WRITE setBusy NOTIFY busyChanged)
  Q_PROPERTY(int busyDelayMs READ busyDelayMs WRITE setBusyDelayMs NOTIFY busyDelayMsChanged)
  Q_PROPERTY(BusyIndicatorPresentation busyIndicatorPresentation READ busyIndicatorPresentation
                 WRITE setBusyIndicatorPresentation NOTIFY busyIndicatorPresentationChanged)
  Q_PROPERTY(
      IconPosition iconPosition READ iconPosition WRITE setIconPosition NOTIFY iconPositionChanged)
  Q_PROPERTY(adqt::icons::IconRef iconRef READ iconRef WRITE setIconRef NOTIFY iconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef busyIconRef READ busyIconRef WRITE setBusyIconRef NOTIFY
                 busyIconRefChanged)

 public:
  enum class ButtonStyle {
    Outline,
    Dashed,
    Solid,
    Tonal,
    Text,
    Link,
    GhostOutline,
    GhostDashed,
  };
  Q_ENUM(ButtonStyle)

  enum class AccentRole {
    Neutral,
    Primary,
    Danger,
    Blue,
    Purple,
    Cyan,
    Green,
    Magenta,
    Pink,
    Red,
    Orange,
    Yellow,
    Volcano,
    Geekblue,
    Lime,
    Gold,
  };
  Q_ENUM(AccentRole)

  enum class Shape {
    Rectangle,
    Rounded,
    Pill,
    Circle,
  };
  Q_ENUM(Shape)

  enum class SizeClass {
    Large,
    Medium,
    Small,
  };
  Q_ENUM(SizeClass)

  enum class IconPosition {
    Leading,
    Trailing,
  };
  Q_ENUM(IconPosition)

  enum class BusyIndicatorPresentation {
    Inline,
    IsolatedSurface,
  };
  Q_ENUM(BusyIndicatorPresentation)

  explicit AdButton(QWidget* parent = nullptr);
  explicit AdButton(const QString& text, QWidget* parent = nullptr);
  ~AdButton() override;

  ButtonStyle buttonStyle() const;
  void setButtonStyle(ButtonStyle value);

  AccentRole accentRole() const;
  void setAccentRole(AccentRole value);

  Shape shape() const;
  void setShape(Shape value);

  SizeClass sizeClass() const;
  void setSizeClass(SizeClass value);

  bool interactionBackgroundVisible() const;
  void setInteractionBackgroundVisible(bool value);

  bool busy() const;
  void setBusy(bool value);

  int busyDelayMs() const;
  void setBusyDelayMs(int value);

  BusyIndicatorPresentation busyIndicatorPresentation() const;
  void setBusyIndicatorPresentation(BusyIndicatorPresentation value);
  quint64 busyIndicatorFrameCount() const;
  QWidget* busyIndicatorSurface() const;

  IconPosition iconPosition() const;
  void setIconPosition(IconPosition value);

  adqt::icons::IconRef iconRef() const;
  void setIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef busyIconRef() const;
  void setBusyIconRef(const adqt::icons::IconRef& value);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void prepareControlScale(const AdControlScaleContext& context) override;
  void commitControlScale(const AdControlScaleContext& context) override;

 signals:
  void buttonStyleChanged(ButtonStyle value);
  void accentRoleChanged(AccentRole value);
  void shapeChanged(Shape value);
  void sizeClassChanged(SizeClass value);
  void interactionBackgroundVisibleChanged(bool value);
  void busyChanged(bool value);
  void busyDelayMsChanged(int value);
  void busyIndicatorPresentationChanged(BusyIndicatorPresentation value);
  void busyIndicatorSurfaceChanged(QWidget* surface);
  void iconPositionChanged(IconPosition value);
  void iconRefChanged(const adqt::icons::IconRef& value);
  void busyIconRefChanged(const adqt::icons::IconRef& value);

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

  void changeEvent(QEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  bool hitButton(const QPoint& pos) const override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

  // For subclasses that paint their own content but still want the standard
  // busy indicator visuals.
  void drawSpinner(QPainter& painter, const QRect& iconRect, const QColor& color) const;

 private:
  struct ContentLayout;
  struct Private;

  bool interactionBlocked() const;
  bool hasUserIconRef() const;
  bool hasBaseIcon() const;
  bool shouldApplyTwoCjkSpacing(const QString& sourceText) const;
  Shape effectiveShape(const QString& displayText) const;
  QString renderText() const;

  detail::ButtonStyleInput buildStyleInput() const;
  detail::ButtonVisualStyle resolvedStyle() const;
  detail::ButtonStateStyle currentStateStyle(const detail::ButtonVisualStyle& style) const;

  void refreshAfterPropertyChange(bool updateGeometry = true);
  void updateBusyVisualState();
  void syncBusyDefaultSuspension();
  void updateSpinnerState();
  bool usesIsolatedBusyIndicator() const;
  void syncIsolatedBusyIndicatorSurface();
  QRect busyIndicatorRect() const;
  QColor busyIndicatorColor() const;
  void drawBusyIndicator(QPainter& painter, const QRect& rect, const QColor& color) const;
  void bumpSegmentZOrder();
  std::optional<Qt::CursorShape> automaticCursorShape() const;
  void syncDisabledCursorOverlay();
  void updateDisabledCursorOverlayGeometry();
  void resetDisabledCursorOverlay();
  void updateCursorForRole();
  void updateInteractionFocusOverlay();
  void triggerInteractionWaveOverlay();
  void syncAccessibleState();
  void applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape);

  detail::SegmentPosition segmentPosition() const;
  void setSegmentPosition(detail::SegmentPosition value);
  bool joinsLeftEdge() const;
  bool joinsRightEdge() const;

  ContentLayout computeContentLayout(const QRect& contentRect, const QSize& iconSize,
                                     const QString& displayText, const QFontMetrics& fm,
                                     int iconGap, const QFont& contentFont, bool twoCjkAutoSpacing,
                                     int textFlags) const;

  friend void detail::setButtonSegmentPosition(AdButton* button, detail::SegmentPosition value);
  friend detail::SegmentPosition detail::buttonSegmentPosition(const AdButton* button);
  friend class detail::BusyIndicatorSurface;

  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets
