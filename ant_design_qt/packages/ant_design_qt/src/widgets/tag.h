#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QPoint>
#include <QRect>

#include <functional>
#include <optional>

#include "icon_core.h"

class QEnterEvent;
class QEvent;
class QFocusEvent;
class QHideEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace adqt::widgets {

class AdTag final : public QAbstractButton {
  Q_OBJECT

  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(
      BorderStyle borderStyle READ borderStyle WRITE setBorderStyle NOTIFY borderStyleChanged)
  Q_PROPERTY(
      ColorScheme colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
  Q_PROPERTY(QColor customColor READ customColor WRITE setCustomColor NOTIFY customColorChanged)
  Q_PROPERTY(bool closable READ closable WRITE setClosable NOTIFY closableChanged)
  Q_PROPERTY(bool autoHideOnClose READ autoHideOnClose WRITE setAutoHideOnClose NOTIFY
                 autoHideOnCloseChanged)
  Q_PROPERTY(adqt::icons::IconRef iconRef READ iconRef WRITE setIconRef NOTIFY iconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef closeIconRef READ closeIconRef WRITE setCloseIconRef NOTIFY
                 closeIconRefChanged)

 public:
  enum class Variant {
    Filled,
    Solid,
    Outlined,
  };
  Q_ENUM(Variant)

  enum class BorderStyle {
    Solid,
    Dashed,
  };
  Q_ENUM(BorderStyle)

  enum class ColorScheme {
    Default,
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
    Success,
    Processing,
    Warning,
    Error,
    Custom,
  };
  Q_ENUM(ColorScheme)

  struct ColorTokens {
    std::optional<QColor> defaultBg;
    std::optional<QColor> defaultColor;
    std::optional<QColor> defaultBorderColor;
    std::optional<QColor> solidTextColor;
    std::optional<QColor> closeColor;
    std::optional<QColor> closeHoverColor;
    std::optional<QColor> closeHoverBackground;
    std::optional<QColor> colorTextDisabled;
    std::optional<QColor> colorBgContainerDisabled;
    std::optional<QColor> colorBorderDisabled;
    std::optional<QColor> checkableHoverColor;
    std::optional<QColor> checkableHoverBg;
    std::optional<QColor> checkableCheckedColor;
    std::optional<QColor> checkableCheckedBg;
    std::optional<QColor> checkableCheckedHoverBg;
    std::optional<QColor> checkableActiveBg;
    std::optional<QColor> focusOutlineColor;
    std::optional<QColor> waveColor;
  };

  struct MetricTokens {
    std::optional<int> height;
    std::optional<int> borderRadius;
    std::optional<int> borderWidth;
    std::optional<int> paddingHorizontal;
    std::optional<int> iconSize;
    std::optional<int> contentGap;
    std::optional<int> closeGap;
    std::optional<qreal> focusOutlineWidth;
    std::optional<qreal> focusOutlineOffset;
  };

  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle icon;
    SemanticSlotStyle content;
    SemanticSlotStyle closeIcon;
  };

  struct ComponentTokenContext {
    Variant variant = Variant::Filled;
    BorderStyle borderStyle = BorderStyle::Solid;
    ColorScheme colorScheme = ColorScheme::Default;
    QColor customColor;
    bool checkable = false;
    bool checked = false;
    bool closable = false;
    bool disabled = false;
    bool hovered = false;
    bool pressed = false;
    bool closeHovered = false;
  };

  struct StyleContext {
    Variant variant = Variant::Filled;
    BorderStyle borderStyle = BorderStyle::Solid;
    ColorScheme colorScheme = ColorScheme::Default;
    QColor customColor;
    bool checkable = false;
    bool checked = false;
    bool closable = false;
    bool disabled = false;
    bool hovered = false;
    bool pressed = false;
    bool closeHovered = false;
  };

  using ComponentTokenResolver = std::function<ComponentTokens(const ComponentTokenContext&)>;
  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdTag(QWidget* parent = nullptr);
  explicit AdTag(const QString& text, QWidget* parent = nullptr);
  ~AdTag() override;

  void setCheckable(bool value);

  Variant variant() const;
  void setVariant(Variant value);

  BorderStyle borderStyle() const;
  void setBorderStyle(BorderStyle value);

  ColorScheme colorScheme() const;
  void setColorScheme(ColorScheme value);

  QColor customColor() const;
  void setCustomColor(const QColor& value);

  bool closable() const;
  void setClosable(bool value);

  bool autoHideOnClose() const;
  void setAutoHideOnClose(bool value);

  adqt::icons::IconRef iconRef() const;
  void setIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef closeIconRef() const;
  void setCloseIconRef(const adqt::icons::IconRef& value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  void setComponentTokenResolver(ComponentTokenResolver resolver);
  void resetComponentTokenResolver();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void resetSemanticStyles();
  void setSemanticStyleResolver(SemanticStyleResolver resolver);
  void resetSemanticStyleResolver();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void variantChanged(Variant value);
  void borderStyleChanged(BorderStyle value);
  void colorSchemeChanged(ColorScheme value);
  void customColorChanged(const QColor& value);
  void closableChanged(bool value);
  void autoHideOnCloseChanged(bool value);
  void iconRefChanged(const adqt::icons::IconRef& value);
  void closeIconRefChanged(const adqt::icons::IconRef& value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void closeRequested();
  void closed();

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool event(QEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void changeEvent(QEvent* event) override;
  bool hitButton(const QPoint& pos) const override;

 private:
  ComponentTokens resolvedComponentTokens() const;
  SemanticStyles resolvedSemanticStyles() const;
  ComponentTokenContext currentComponentTokenContext() const;
  StyleContext currentStyleContext() const;
  QRect closeButtonRect() const;
  bool closeButtonVisible() const;
  bool closeButtonHit(const QPoint& pos) const;
  void refreshAfterStateChange(bool updateGeometry = true);
  void updateHoverState(const QPoint& pos);

  Variant variant_ = Variant::Filled;
  BorderStyle borderStyle_ = BorderStyle::Solid;
  ColorScheme colorScheme_ = ColorScheme::Default;
  QColor customColor_;
  bool closable_ = false;
  bool autoHideOnClose_ = true;
  adqt::icons::IconRef iconRef_;
  adqt::icons::IconRef closeIconRef_;
  ComponentTokens componentTokens_;
  ComponentTokenResolver componentTokenResolver_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
  bool pressed_ = false;
  bool closeHovered_ = false;
  bool closePressed_ = false;
  bool focusVisible_ = false;
  bool pendingClosedEmission_ = false;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdTag::Variant)
Q_DECLARE_METATYPE(adqt::widgets::AdTag::BorderStyle)
Q_DECLARE_METATYPE(adqt::widgets::AdTag::ColorScheme)
