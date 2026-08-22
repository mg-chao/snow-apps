#pragma once

#include <QColor>
#include <QRadioButton>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

#include "control_scale.h"

class QEnterEvent;
class QEvent;
class QFocusEvent;
class QFontMetrics;
class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QMoveEvent;
class QPaintEvent;
class QPainter;
class QResizeEvent;
class QShowEvent;

namespace adqt::widgets {

namespace detail {
struct RadioButtonVisualStyle;
struct RadioButtonStyleInput;
struct RadioVisualStyle;
struct RadioStyleInput;
}  // namespace detail

class AdRadioButtonGroup;

class AdRadio final : public QRadioButton, public AdControlScaleParticipant {
  Q_OBJECT

  Q_PROPERTY(
      ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY controlSizeChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(
      ButtonStyle buttonStyle READ buttonStyle WRITE setButtonStyle NOTIFY buttonStyleChanged)

 public:
  enum class ControlSize {
    Large,
    Medium,
    Small,
  };
  Q_ENUM(ControlSize)

  enum class Variant {
    Default,
    Button,
  };
  Q_ENUM(Variant)

  enum class ButtonStyle {
    Outline,
    Solid,
  };
  Q_ENUM(ButtonStyle)

  struct ComponentTokenContext {
    ControlSize controlSize = ControlSize::Medium;
    Variant variant = Variant::Default;
    ButtonStyle buttonStyle = ButtonStyle::Outline;
    bool checked = false;
    bool disabled = false;
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
    bool block = false;
  };

  struct ColorTokens {
    std::optional<QColor> textColor;
    std::optional<QColor> indicatorBorderColor;
    std::optional<QColor> indicatorFillColor;
    std::optional<QColor> indicatorDotColor;
    std::optional<QColor> buttonTextColor;
    std::optional<QColor> buttonFillColor;
    std::optional<QColor> buttonBorderColor;
    std::optional<QColor> focusRingColor;
    std::optional<QColor> waveColor;
  };

  struct MetricTokens {
    std::optional<int> radioSize;
    std::optional<int> dotSize;
    std::optional<int> borderWidth;
    std::optional<int> labelPaddingInlineStart;
    std::optional<int> labelPaddingInlineEnd;
    std::optional<int> textLineHeight;
    std::optional<int> wrapperMarginInlineEnd;
    std::optional<int> buttonHeight;
    std::optional<int> buttonPaddingInline;
    std::optional<int> buttonBorderRadius;
    std::optional<qreal> focusOutlineWidth;
    std::optional<qreal> focusOutlineOffset;
  };

  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };

  using ComponentTokenResolver = std::function<ComponentTokens(const ComponentTokenContext&)>;

  explicit AdRadio(QWidget* parent = nullptr);
  explicit AdRadio(const QString& text, QWidget* parent = nullptr);
  ~AdRadio() override;

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);
  void resetControlSize();
  bool hasControlSizeOverride() const;

  Variant variant() const;
  void setVariant(Variant value);
  void resetVariant();
  bool hasVariantOverride() const;

  ButtonStyle buttonStyle() const;
  void setButtonStyle(ButtonStyle value);
  void resetButtonStyle();
  bool hasButtonStyleOverride() const;

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  bool hasComponentTokensOverride() const;
  void setComponentTokenResolver(ComponentTokenResolver resolver);
  void resetComponentTokenResolver();
  bool hasComponentTokenResolverOverride() const;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void prepareControlScale(const AdControlScaleContext& context) override;
  void commitControlScale(const AdControlScaleContext& context) override;

 signals:
  void controlSizeChanged(ControlSize value);
  void variantChanged(Variant value);
  void buttonStyleChanged(ButtonStyle value);
  void componentTokensChanged();

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
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
  void changeEvent(QEvent* event) override;

 private:
  enum class GroupPosition {
    None,
    Only,
    First,
    Middle,
    Last,
  };

  struct EffectiveStateSnapshot {
    ControlSize controlSize = ControlSize::Medium;
    Variant variant = Variant::Default;
    ButtonStyle buttonStyle = ButtonStyle::Outline;
    ComponentTokens resolvedTokens;
  };

  friend class AdRadioButtonGroup;

  bool interactionBlocked() const;
  ControlSize effectiveControlSize() const;
  Variant effectiveVariant() const;
  ButtonStyle effectiveButtonStyle() const;
  bool effectiveFill() const;
  ComponentTokenContext currentComponentTokenContext() const;
  ComponentTokens resolvedComponentTokens() const;
  int textWidth(const QFontMetrics& metrics) const;
  int horizontalSpacingHint() const;
  int buttonGroupOverlapHint() const;
  AdRadio* seamNeighborAt(const QPoint& pos) const;
  void refreshAfterPropertyChange(bool updateGeometry = true);
  void paintButtonVariant(QPainter* painter) const;
  void paintDefaultVariant(QPainter* painter) const;
  void updateInteractionFocusOverlay();
  void triggerInteractionWaveOverlay();
  void bumpGroupZOrder();
  void refreshAutomaticCursor();
  void applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape);
  detail::RadioStyleInput buildStyleInput() const;
  detail::RadioButtonStyleInput buildButtonStyleInput() const;
  const detail::RadioVisualStyle& resolvedRadioStyle() const;
  const detail::RadioButtonVisualStyle& resolvedRadioButtonStyle() const;
  EffectiveStateSnapshot captureEffectiveState(bool includeTokens) const;
  void applyEffectiveStateChange(const EffectiveStateSnapshot& before, bool tokensMayChange,
                                 bool updateGeometry = true);
  void resolveButtonCornerRadii(qreal* topLeft, qreal* topRight, qreal* bottomRight,
                                qreal* bottomLeft) const;
  void syncManagedSizePolicy();
  void setGroup(AdRadioButtonGroup* group);
  void setGroupPosition(GroupPosition position);
  void setGroupVertical(bool vertical);

  std::optional<ControlSize> controlSizeOverride_;
  std::optional<Variant> variantOverride_;
  std::optional<ButtonStyle> buttonStyleOverride_;
  bool hovered_ = false;
  bool pressed_ = false;
  bool focusVisible_ = false;
  GroupPosition groupPosition_ = GroupPosition::None;
  bool groupVertical_ = false;
  AdRadioButtonGroup* group_ = nullptr;
  AdRadio* forwardedPressTarget_ = nullptr;
  ComponentTokens componentTokens_;
  ComponentTokenResolver componentTokenResolver_;
  bool componentTokensOverride_ = false;
  bool componentTokenResolverOverride_ = false;
  bool explicitCursorOverride_ = false;
  bool autoCursorManaged_ = false;
  bool applyingAutoCursor_ = false;
  struct StyleCache;
  mutable std::unique_ptr<StyleCache> styleCache_;
  AdControlScaleContext controlScale_;
  QFont referenceFont_;
  QSize referenceIconSize_;
  bool referenceFontCaptured_ = false;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdRadio::ControlSize)
Q_DECLARE_METATYPE(adqt::widgets::AdRadio::Variant)
Q_DECLARE_METATYPE(adqt::widgets::AdRadio::ButtonStyle)
