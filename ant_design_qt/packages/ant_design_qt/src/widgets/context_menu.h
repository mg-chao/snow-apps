#pragma once

#include <QColor>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>
#include <QPixmap>
#include <QRectF>
#include <memory>
#include <optional>
#include <functional>

#include "icon_core.h"

class QHideEvent;
class QPaintEvent;
class QActionEvent;

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

  // A prepared background only affects the shared widget surface. Image decoding,
  // scaling and filtering remain the caller's responsibility.
  struct BackgroundFrame {
    QPixmap image;
    QRectF normalizedPlacement;
    qreal imageOpacity = 1.0;
    qreal maskOpacity = 0.8;
  };

  explicit AdContextMenu(QWidget* parent = nullptr);
  explicit AdContextMenu(const QString& title, QWidget* parent = nullptr);
  ~AdContextMenu() override;

  using QMenu::addAction;
  using QMenu::addMenu;

  // Appearance tokens apply to the widget menu. macOS uses a native menu until
  // setNativeMenuEnabled(false) selects that shared widget menu. Submenus follow
  // that choice, including menus already attached, and stored icons are rebuilt
  // for the selected surface.
  ColorScheme colorScheme() const;
  void setColorScheme(ColorScheme value);

  bool nativeMenuEnabled() const;
  void setNativeMenuEnabled(bool enabled);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  BackgroundFrame backgroundFrame() const;
  void setBackgroundFrame(const BackgroundFrame& frame);
  void resetBackgroundFrame();

  QWidget* triggerWidget() const;
  void setTriggerWidget(QWidget* widget);

  QAction* addItem(const QString& text, const adqt::icons::IconRef& icon = {},
                   const QKeySequence& shortcut = {});
  AdContextMenu* addSubMenu(const QString& text, const adqt::icons::IconRef& icon = {});
  using ContentFactory = std::function<void(AdContextMenu*)>;
  // Qt requires an empty submenu shell for its arrow and keyboard navigation.
  // Populate only when opened; retire the populated shell on hide and replace
  // it with an empty one if the parent popup is still open.
  // Restore retired shells when a persistent parent popup opens again.
  QAction* addLazySubMenu(const QString& text, ContentFactory factory,
                          const adqt::icons::IconRef& icon = {});

  void setActionIcon(QAction* action, const adqt::icons::IconRef& icon);
  adqt::icons::IconRef actionIcon(const QAction* action) const;

  void setActionDanger(QAction* action, bool danger = true);
  bool actionDanger(const QAction* action) const;

  // Display-only metadata, independent of the action label and keyboard shortcut.
  // The widget menu uses its trailing column; macOS uses an NSMenuItem badge.
  void setActionBadge(QAction* action, const QString& text);
  QString actionBadge(const QAction* action) const;

  // Native menus track outside QWidget visibility on macOS.
  bool isPopupVisible() const;
  void dismissPopup();
  // Heap-owned popup sessions retire after hiding and action delivery. Do not
  // enable this for stack-owned menus displayed with execAt().
  void setDeleteOnHide(bool enabled = true);
  bool isRetiring() const;
  QSize sizeHint() const override;

  void popupAt(const QPoint& globalPosition);
  QAction* execAt(const QPoint& globalPosition, QAction* initialAction = nullptr);
  // Platform adapters (for example NSStatusItem) keep the same action-delivery
  // and retirement ordering while supplying their own native presenter.
  QAction* execNativePopup(const std::function<void()>& presenter);

 signals:
  void colorSchemeChanged(ColorScheme value);
  void componentTokensChanged();
  void triggerWidgetChanged(QWidget* widget);
  void popupFinished();
  // Detach observers that call menu APIs before the derived state is released.
  // QPointer is cleared later, during QObject destruction, after QWidget teardown events.
  void aboutToDestroy();

 protected:
  bool event(QEvent* event) override;
  void actionEvent(QActionEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  friend class detail::AdContextMenuStyle;

  void refreshVisuals(bool relayout);
  void configureSurfaceAttributes();
  void configureCustomSurface();
  void configurePlatformSurface();
  void applyStoredActionIcon(QAction* action);
  void retirePopup();
  AdContextMenu(QWidget* parent, bool deferredSurface);
  class LazySubMenu;

  class Private;
  std::unique_ptr<Private> d_;
};

}  // namespace adqt::widgets
