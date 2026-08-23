#pragma once

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QMetaObject>
#include <QPointer>

#include <functional>
#include <optional>

class QEvent;
class QPaintEvent;
class QResizeEvent;

namespace adqt::widgets {

namespace detail {
struct DividerAppearance;
}

class AdDivider final : public QFrame {
  Q_OBJECT

  Q_PROPERTY(
      Orientation orientation READ orientation WRITE setOrientation NOTIFY orientationChanged)
  Q_PROPERTY(Orientation type READ type WRITE setType NOTIFY typeChanged)
  Q_PROPERTY(bool vertical READ vertical WRITE setVertical NOTIFY verticalChanged)
  Q_PROPERTY(qreal orientationMargin READ orientationMargin WRITE setOrientationMargin NOTIFY
                 orientationMarginChanged)
  Q_PROPERTY(Size dividerSize READ dividerSize WRITE setDividerSize NOTIFY sizeChanged)
  Q_PROPERTY(TitlePlacement titlePlacement READ titlePlacement WRITE setTitlePlacement NOTIFY
                 titlePlacementChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(bool dashed READ dashed WRITE setDashed NOTIFY dashedChanged)
  Q_PROPERTY(bool plain READ plain WRITE setPlain NOTIFY plainChanged)
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
  Q_PROPERTY(
      QWidget* contentWidget READ contentWidget WRITE setContentWidget NOTIFY contentWidgetChanged)

 public:
  enum class Orientation {
    Horizontal,
    Vertical,
  };
  Q_ENUM(Orientation)

  enum class Size {
    Small,
    Middle,
    Large,
  };
  Q_ENUM(Size)

  enum class TitlePlacement {
    Start,
    Center,
    End,
    // Deprecated physical aliases retained for compatibility with Ant Design 5.x.
    Left,
    Right,
  };
  Q_ENUM(TitlePlacement)

  enum class Variant {
    Solid,
    Dashed,
    Dotted,
  };
  Q_ENUM(Variant)

  struct ColorTokens {
    std::optional<QColor> splitColor;
    std::optional<QColor> textColor;
    std::optional<QColor> headingTextColor;
  };

  struct MetricTokens {
    std::optional<qreal> lineWidth;
    std::optional<int> textPaddingInline;
    std::optional<qreal> orientationMargin;
    std::optional<int> verticalMarginInline;
    std::optional<int> horizontalMarginSmall;
    std::optional<int> horizontalMarginMiddle;
    std::optional<int> horizontalMarginLarge;
    std::optional<int> horizontalMarginWithText;
    std::optional<qreal> verticalHeightFactor;
  };

  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
    std::optional<qreal> borderWidth;
    std::optional<QFont> font;
    std::optional<int> marginStart;
    std::optional<int> marginEnd;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle rail;
    SemanticSlotStyle content;
  };

  struct StyleContext {
    Orientation orientation = Orientation::Horizontal;
    Size size = Size::Large;
    TitlePlacement titlePlacement = TitlePlacement::Center;
    Variant variant = Variant::Solid;
    bool plain = false;
    bool hasContent = false;
    bool enabled = true;
  };

  using ComponentTokenResolver = std::function<ComponentTokens(const StyleContext&)>;
  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdDivider(QWidget* parent = nullptr);
  explicit AdDivider(const QString& text, QWidget* parent = nullptr);
  ~AdDivider() override;

  Orientation orientation() const;
  void setOrientation(Orientation value);
  Orientation type() const;
  void setType(Orientation value);
  bool vertical() const;
  void setVertical(bool value);

  qreal orientationMargin() const;
  void setOrientationMargin(qreal value);

  Size dividerSize() const;
  void setDividerSize(Size value);
  void setSize(Size value);

  TitlePlacement titlePlacement() const;
  void setTitlePlacement(TitlePlacement value);

  Variant variant() const;
  void setVariant(Variant value);
  bool dashed() const;
  void setDashed(bool value);

  bool plain() const;
  void setPlain(bool value);

  QString text() const;
  void setText(const QString& value);

  QWidget* contentWidget() const;
  // A non-null widget is reparented to the divider, which takes ownership of it.
  void setContentWidget(QWidget* widget);
  // Releases the custom content widget to the caller without deleting it.
  QWidget* takeContentWidget();

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  void setComponentTokenResolver(ComponentTokenResolver resolver);
  void resetComponentTokenResolver();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& value);
  void resetSemanticStyles();
  void setSemanticStyleResolver(SemanticStyleResolver resolver);
  void resetSemanticStyleResolver();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void orientationChanged(Orientation value);
  void typeChanged(Orientation value);
  void verticalChanged(bool value);
  void orientationMarginChanged(qreal value);
  void sizeChanged(Size value);
  void titlePlacementChanged(TitlePlacement value);
  void variantChanged(Variant value);
  void dashedChanged(bool value);
  void plainChanged(bool value);
  void textChanged(const QString& value);
  void contentWidgetChanged(QWidget* widget);
  void componentTokensChanged();
  void semanticStylesChanged();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  bool hasContent() const;
  StyleContext currentStyleContext() const;
  ComponentTokens resolvedComponentTokens() const;
  SemanticStyles resolvedSemanticStyles() const;
  detail::DividerAppearance resolvedAppearance() const;
  void refreshAfterStateChange(bool geometryChanged = true);
  void updateFrameAndSizePolicy();
  void updateContentWidgetGeometry();
  void updateAccessibleText();
  void clearContentWidget(bool deleteWidget);
  void attachContentWidget(QWidget* widget);

  Orientation orientation_ = Orientation::Horizontal;
  Size size_ = Size::Large;
  TitlePlacement titlePlacement_ = TitlePlacement::Center;
  Variant variant_ = Variant::Solid;
  bool plain_ = false;
  QString text_;
  QPointer<QWidget> contentWidget_;
  QMetaObject::Connection contentDestroyedConnection_;
  ComponentTokens componentTokens_;
  ComponentTokenResolver componentTokenResolver_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdDivider::Orientation)
Q_DECLARE_METATYPE(adqt::widgets::AdDivider::Size)
Q_DECLARE_METATYPE(adqt::widgets::AdDivider::TitlePlacement)
Q_DECLARE_METATYPE(adqt::widgets::AdDivider::Variant)
