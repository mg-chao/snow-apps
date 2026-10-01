#pragma once

#include <QColor>
#include <QFont>
#include <QMap>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

namespace adqt::widgets {

class AdDescriptions final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(bool bordered READ bordered WRITE setBordered NOTIFY borderedChanged)
  Q_PROPERTY(Size descriptionSize READ descriptionSize WRITE setDescriptionSize NOTIFY sizeChanged)
  Q_PROPERTY(LayoutMode layoutMode READ layoutMode WRITE setLayoutMode NOTIFY layoutModeChanged)
  Q_PROPERTY(int column READ column WRITE setColumn NOTIFY columnChanged)
  Q_PROPERTY(int effectiveColumn READ effectiveColumn NOTIFY effectiveColumnChanged)
  Q_PROPERTY(bool colon READ colon WRITE setColon NOTIFY colonChanged)
  Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
  Q_PROPERTY(QString extra READ extra WRITE setExtra NOTIFY extraChanged)

 public:
  enum class Size {
    Default,
    Middle,
    Small,
  };
  Q_ENUM(Size)

  enum class LayoutMode {
    Horizontal,
    Vertical,
  };
  Q_ENUM(LayoutMode)

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
    std::optional<QFont> font;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle header;
    SemanticSlotStyle title;
    SemanticSlotStyle extra;
    SemanticSlotStyle label;
    SemanticSlotStyle content;
  };

  struct Item {
    QString key;
    QString label;
    QString content;
    QPointer<QWidget> labelWidget;
    QPointer<QWidget> contentWidget;
    // Spans wider than the current row are capped to its remaining columns.
    int span = 1;
    // Optional minimum-width-to-span map, equivalent to Ant Design's responsive span object.
    QMap<int, int> responsiveSpans;
    // Equivalent to Ant Design's span="filled".
    bool fillRemaining = false;
    SemanticSlotStyle labelStyle;
    SemanticSlotStyle contentStyle;
    QVariant data;
  };

  struct ComponentTokenContext {
    bool bordered = false;
    Size size = Size::Default;
    LayoutMode layoutMode = LayoutMode::Horizontal;
    bool enabled = true;
  };

  struct ColorTokens {
    std::optional<QColor> labelBackground;
    std::optional<QColor> labelColor;
    std::optional<QColor> titleColor;
    std::optional<QColor> contentColor;
    std::optional<QColor> extraColor;
    std::optional<QColor> borderColor;
  };

  struct MetricTokens {
    std::optional<int> titleMarginBottom;
    std::optional<int> itemPaddingBottom;
    std::optional<int> itemPaddingEnd;
    std::optional<int> colonMarginLeft;
    std::optional<int> colonMarginRight;
    std::optional<int> borderedPaddingBlock;
    std::optional<int> borderedPaddingInline;
    std::optional<int> borderWidth;
    std::optional<int> borderRadius;
  };

  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };

  using ComponentTokenResolver = std::function<ComponentTokens(const ComponentTokenContext&)>;
  using SemanticStyleResolver = std::function<SemanticStyles(const ComponentTokenContext&)>;

  explicit AdDescriptions(QWidget* parent = nullptr);
  ~AdDescriptions() override;

  int count() const;
  Item itemAt(int index) const;
  int indexOf(const QString& key) const;
  int indexOf(const QWidget* widget) const;

  int addItem(const QString& label, const QString& content, int span = 1);
  int addItem(const Item& item);
  // Successful insertion transfers ownership of custom label/content widgets.
  int insertItem(int index, const Item& item);
  bool updateItem(int index, const Item& item);
  // removeItem() deletes owned custom widgets; takeItem() transfers them to the caller.
  void removeItem(int index);
  void removeItem(const QString& key);
  Item takeItem(int index);
  void clear();

  void setItemLabel(int index, const QString& label);
  void setItemContent(int index, const QString& content);
  void setItemSpan(int index, int span);
  void setItemResponsiveSpans(int index, const QMap<int, int>& spans);
  void setItemFillRemaining(int index, bool fill);
  void setItemLabelWidget(int index, QWidget* widget);
  void setItemContentWidget(int index, QWidget* widget);
  QWidget* takeItemLabelWidget(int index);
  QWidget* takeItemContentWidget(int index);

  bool bordered() const;
  void setBordered(bool value);
  Size descriptionSize() const;
  void setDescriptionSize(Size value);
  // Convenience alias matching Ant Design's `size` prop terminology.
  void setSize(Size value);
  LayoutMode layoutMode() const;
  void setLayoutMode(LayoutMode value);
  int column() const;
  // Sets a fixed column count and clears the responsive column map.
  void setColumn(int value);
  int effectiveColumn() const;
  QMap<int, int> responsiveColumns() const;
  // Keys are minimum widget widths in device-independent pixels.
  void setResponsiveColumns(const QMap<int, int>& columns);
  void resetResponsiveColumns();
  bool colon() const;
  void setColon(bool value);

  QString title() const;
  void setTitle(const QString& value);
  QString extra() const;
  void setExtra(const QString& value);
  QWidget* titleWidget() const;
  void setTitleWidget(QWidget* widget);
  QWidget* takeTitleWidget();
  QWidget* extraWidget() const;
  void setExtraWidget(QWidget* widget);
  QWidget* takeExtraWidget();

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
  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;

 signals:
  void countChanged(int value);
  void itemAdded(int index);
  void itemChanged(int index);
  void itemRemoved(int index);
  void borderedChanged(bool value);
  void sizeChanged(Size value);
  void layoutModeChanged(LayoutMode value);
  void columnChanged(int value);
  void effectiveColumnChanged(int value);
  void responsiveColumnsChanged();
  void colonChanged(bool value);
  void titleChanged(const QString& value);
  void extraChanged(const QString& value);
  void titleWidgetChanged(QWidget* widget);
  void extraWidgetChanged(QWidget* widget);
  void componentTokensChanged();
  void semanticStylesChanged();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void changeEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  struct Private;
  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdDescriptions::Size)
Q_DECLARE_METATYPE(adqt::widgets::AdDescriptions::LayoutMode)
