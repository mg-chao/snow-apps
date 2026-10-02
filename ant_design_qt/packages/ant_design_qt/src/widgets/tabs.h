#pragma once

#include "icon_core.h"

#include <QColor>
#include <QList>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

namespace adqt::widgets {

// Tabs retain their natural extent along the navigation strip. Overflow scrolls
// within the strip, and selecting a tab brings it into view.
class AdTabs final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
  Q_PROPERTY(QString currentKey READ currentKey WRITE setCurrentKey NOTIFY currentKeyChanged)
  Q_PROPERTY(Type type READ type WRITE setType NOTIFY typeChanged)
  Q_PROPERTY(
      ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY controlSizeChanged)
  Q_PROPERTY(
      Placement tabPlacement READ tabPlacement WRITE setTabPlacement NOTIFY tabPlacementChanged)
  Q_PROPERTY(bool centered READ centered WRITE setCentered NOTIFY centeredChanged)
  Q_PROPERTY(bool animated READ animated WRITE setAnimated NOTIFY animatedChanged)
  Q_PROPERTY(bool hideAdd READ hideAdd WRITE setHideAdd NOTIFY hideAddChanged)
  Q_PROPERTY(int tabBarGutter READ tabBarGutter WRITE setTabBarGutter NOTIFY tabBarGutterChanged)
  Q_PROPERTY(int indicatorSize READ indicatorSize WRITE setIndicatorSize NOTIFY indicatorChanged)
  Q_PROPERTY(IndicatorAlignment indicatorAlignment READ indicatorAlignment WRITE
                 setIndicatorAlignment NOTIFY indicatorChanged)

 public:
  enum class Type {
    Line,
    Card,
    EditableCard,
  };
  Q_ENUM(Type)

  enum class ControlSize {
    Small,
    Medium,
    Large,
  };
  Q_ENUM(ControlSize)

  enum class Placement {
    Top,
    End,
    Bottom,
    Start,
  };
  Q_ENUM(Placement)

  enum class IndicatorAlignment {
    Start,
    Center,
    End,
    Fill,
  };
  Q_ENUM(IndicatorAlignment)

  struct TabItem {
    QString key;
    QString label;
    adqt::icons::IconRef icon;
    QPointer<QWidget> page;
    bool enabled = true;
    bool closable = true;
    QVariant data;
  };

  struct ComponentTokenContext {
    Type type = Type::Line;
    ControlSize controlSize = ControlSize::Medium;
    Placement placement = Placement::Top;
    bool enabled = true;
  };

  struct ColorTokens {
    std::optional<QColor> itemColor;
    std::optional<QColor> itemSelectedColor;
    std::optional<QColor> itemHoverColor;
    std::optional<QColor> itemActiveColor;
    std::optional<QColor> itemDisabledColor;
    std::optional<QColor> inkBarColor;
    std::optional<QColor> cardBackground;
    std::optional<QColor> cardActiveBackground;
    std::optional<QColor> borderColor;
    std::optional<QColor> focusOutline;
  };

  struct MetricTokens {
    std::optional<int> horizontalItemGutter;
    std::optional<int> horizontalItemPadding;
    std::optional<int> verticalItemPadding;
    std::optional<int> cardHeight;
    std::optional<int> indicatorThickness;
    std::optional<int> borderRadius;
    std::optional<int> iconSize;
    std::optional<int> iconGap;
  };

  struct ComponentTokens {
    ColorTokens colors;
    MetricTokens metrics;
  };

  using ComponentTokenResolver = std::function<ComponentTokens(const ComponentTokenContext&)>;

  explicit AdTabs(QWidget* parent = nullptr);
  ~AdTabs() override;

  int count() const;
  int currentIndex() const;
  QString currentKey() const;

  int addTab(const QString& key, const QString& label, QWidget* page = nullptr);
  int addTab(const TabItem& item);
  // AdTabs takes ownership of an inserted page. removeTab() schedules it for
  // deletion; takeTab() removes it and transfers ownership to the caller.
  int insertTab(int index, const TabItem& source);
  void removeTab(int index);
  void removeTab(const QString& key);
  QWidget* takeTab(int index);
  void clear();

  int indexOf(const QString& key) const;
  int indexOf(const QWidget* page) const;
  QString tabKey(int index) const;
  QString tabText(int index) const;
  void setTabText(int index, const QString& text);
  adqt::icons::IconRef tabIcon(int index) const;
  void setTabIcon(int index, const adqt::icons::IconRef& icon);
  QWidget* widget(int index) const;
  QVariant tabData(int index) const;
  void setTabData(int index, const QVariant& value);
  bool isTabEnabled(int index) const;
  void setTabEnabled(int index, bool enabled);
  bool isTabClosable(int index) const;
  void setTabClosable(int index, bool closable);

  Type type() const;
  void setType(Type value);
  ControlSize controlSize() const;
  void setControlSize(ControlSize value);
  Placement tabPlacement() const;
  void setTabPlacement(Placement value);
  bool centered() const;
  void setCentered(bool value);
  bool animated() const;
  void setAnimated(bool value);
  bool hideAdd() const;
  void setHideAdd(bool value);
  int tabBarGutter() const;
  void setTabBarGutter(int value);
  int indicatorSize() const;
  void setIndicatorSize(int value);
  IndicatorAlignment indicatorAlignment() const;
  void setIndicatorAlignment(IndicatorAlignment value);

  QWidget* tabBarExtraContentStart() const;
  void setTabBarExtraContentStart(QWidget* widget);
  QWidget* tabBarExtraContentEnd() const;
  void setTabBarExtraContentEnd(QWidget* widget);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  void setComponentTokenResolver(ComponentTokenResolver resolver);
  void resetComponentTokenResolver();

 public slots:
  void setCurrentIndex(int index);
  void setCurrentKey(const QString& key);

 signals:
  void currentIndexChanged(int index);
  void currentKeyChanged(const QString& key);
  void tabClicked(const QString& key);
  void tabCloseRequested(const QString& key);
  void addRequested();
  void typeChanged(Type value);
  void controlSizeChanged(ControlSize value);
  void tabPlacementChanged(Placement value);
  void centeredChanged(bool value);
  void animatedChanged(bool value);
  void hideAddChanged(bool value);
  void tabBarGutterChanged(int value);
  void indicatorChanged();
  void componentTokensChanged();

 protected:
  void changeEvent(QEvent* event) override;

 private:
  struct Private;
  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdTabs::Type)
Q_DECLARE_METATYPE(adqt::widgets::AdTabs::ControlSize)
Q_DECLARE_METATYPE(adqt::widgets::AdTabs::Placement)
Q_DECLARE_METATYPE(adqt::widgets::AdTabs::IndicatorAlignment)
