#pragma once

#include <QColor>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>
#include <memory>
#include <optional>

#include "icon_core.h"

class QPaintEvent;

namespace adqt::widgets {

namespace detail {
class AdContextMenuStyle;
}

class AdContextMenu final : public QMenu {
  Q_OBJECT

  Q_PROPERTY(
      ColorScheme colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
  Q_PROPERTY(
      QWidget* triggerWidget READ triggerWidget WRITE setTriggerWidget NOTIFY triggerWidgetChanged)

 public:
  enum class ColorScheme {
    Inherit,
    Light,
    Dark,
  };
  Q_ENUM(ColorScheme)

  struct ComponentTokens {
    std::optional<int> itemHeight;
    std::optional<int> horizontalPadding;
    std::optional<int> iconSize;
    std::optional<int> iconTextGap;
    std::optional<int> menuPadding;
    std::optional<int> borderRadius;
    std::optional<int> itemBorderRadius;
    std::optional<int> minimumWidth;

    std::optional<QColor> background;
    std::optional<QColor> border;
    std::optional<QColor> text;
    std::optional<QColor> secondaryText;
    std::optional<QColor> disabledText;
    std::optional<QColor> hoverBackground;
    std::optional<QColor> hoverText;
    std::optional<QColor> dangerText;
    std::optional<QColor> dangerHoverText;
    std::optional<QColor> dangerHoverBackground;
    std::optional<QColor> divider;
    std::optional<QColor> checkmark;
  };

  explicit AdContextMenu(QWidget* parent = nullptr);
  explicit AdContextMenu(const QString& title, QWidget* parent = nullptr);
  ~AdContextMenu() override;

  using QMenu::addAction;
  using QMenu::addMenu;

  ColorScheme colorScheme() const;
  void setColorScheme(ColorScheme value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  QWidget* triggerWidget() const;
  void setTriggerWidget(QWidget* widget);

  QAction* addItem(const QString& text, const adqt::icons::IconRef& icon = {},
                   const QKeySequence& shortcut = {});
  AdContextMenu* addSubMenu(const QString& text, const adqt::icons::IconRef& icon = {});

  void setActionIcon(QAction* action, const adqt::icons::IconRef& icon);
  adqt::icons::IconRef actionIcon(const QAction* action) const;

  void setActionDanger(QAction* action, bool danger = true);
  bool actionDanger(const QAction* action) const;

  void popupAt(const QPoint& globalPosition);
  QAction* execAt(const QPoint& globalPosition, QAction* initialAction = nullptr);

 signals:
  void colorSchemeChanged(ColorScheme value);
  void componentTokensChanged();
  void triggerWidgetChanged(QWidget* widget);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  friend class detail::AdContextMenuStyle;

  void refreshVisuals(bool relayout);

  class Private;
  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets
