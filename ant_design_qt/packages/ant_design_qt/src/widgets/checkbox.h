#pragma once

#include <QCheckBox>
#include <QColor>
#include <QVariant>

#include <functional>
#include <optional>

class QEnterEvent;
class QCursor;
class QEvent;
class QFocusEvent;
class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QMoveEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;

namespace adqt::widgets {

namespace detail {
struct CheckboxStyleInput;
}

class AdCheckboxGroup;

class AdCheckbox final : public QCheckBox {
  Q_OBJECT

  Q_PROPERTY(
      bool indeterminate READ isIndeterminate WRITE setIndeterminate NOTIFY indeterminateChanged)
  Q_PROPERTY(QVariant value READ value WRITE setValue NOTIFY valueChanged)

 public:
  struct ComponentTokenContext {
    bool checked = false;
    bool indeterminate = false;
    bool disabled = false;
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
  };

  struct ColorTokens {
    std::optional<QColor> textColor;
    std::optional<QColor> indicatorBorderColor;
    std::optional<QColor> indicatorFillColor;
    std::optional<QColor> indicatorMarkColor;
    std::optional<QColor> focusRingColor;
    std::optional<QColor> waveColor;
  };

  struct MetricTokens {
    std::optional<int> checkboxSize;
    std::optional<int> borderWidth;
    std::optional<int> borderRadius;
    std::optional<int> markWidth;
    std::optional<int> labelPaddingInlineStart;
    std::optional<int> labelPaddingInlineEnd;
    std::optional<int> textLineHeight;
    std::optional<int> wrapperMarginInlineEnd;
    std::optional<qreal> focusOutlineWidth;
    std::optional<qreal> focusOutlineOffset;
  };

  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };

  using ComponentTokenResolver = std::function<ComponentTokens(const ComponentTokenContext&)>;

  explicit AdCheckbox(QWidget* parent = nullptr);
  explicit AdCheckbox(const QString& text, QWidget* parent = nullptr);
  ~AdCheckbox() override;

  void setCursor(const QCursor& cursor);
  void unsetCursor();

  bool isIndeterminate() const;
  void setIndeterminate(bool value);

  QVariant value() const;
  void setValue(const QVariant& value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  void setComponentTokenResolver(ComponentTokenResolver resolver);
  void resetComponentTokenResolver();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void indeterminateChanged(bool value);
  void valueChanged(const QVariant& value);
  void componentTokensChanged();

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void nextCheckState() override;
  bool hitButton(const QPoint& pos) const override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  friend class AdCheckboxGroup;

  bool interactionBlocked() const;
  bool effectiveEnabled() const;
  ComponentTokenContext currentComponentTokenContext() const;
  ComponentTokens resolvedComponentTokens() const;
  static bool componentTokensEqual(const ComponentTokens& lhs, const ComponentTokens& rhs);
  detail::CheckboxStyleInput buildStyleInput() const;
  QRectF indicatorRect(int checkboxSize) const;
  int horizontalSpacingHint() const;
  void refreshAfterPropertyChange(bool geometryChanged = true);
  void refreshAutomaticCursor();
  void applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape);
  void updateInteractionFocusOverlay();
  void triggerInteractionWaveOverlay();
  void setGroup(AdCheckboxGroup* group);

  bool reportedIndeterminate_ = false;
  QVariant value_;
  bool pressed_ = false;
  bool focusVisible_ = false;
  ComponentTokens componentTokens_;
  ComponentTokenResolver componentTokenResolver_;
  AdCheckboxGroup* group_ = nullptr;
  bool explicitCursorOverride_ = false;
  bool autoCursorManaged_ = false;
  bool applyingAutoCursor_ = false;
  bool cursorRefreshPending_ = false;
};

}  // namespace adqt::widgets
