#include "context_menu.h"

#ifdef Q_OS_MACOS
#include "context_menu_mac_p.h"
#endif

#include "detail/button_rendering.h"

#include <QActionEvent>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHideEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QShowEvent>
#include <QStyleOptionMenuItem>
#include <algorithm>
#include <cmath>

#include "theme/theme.h"

namespace adqt::widgets {

namespace {

constexpr char kActionDangerProperty[] = "adqt.contextMenu.danger";
constexpr char kActionIconProperty[] = "adqt.contextMenu.iconRef";
constexpr char kActionBadgeProperty[] = "adqt.contextMenu.badge";

QColor colorOr(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

template <typename T>
T tokenOr(const std::optional<T>& token, const T& fallback) {
  return token.has_value() ? token.value() : fallback;
}

QString withoutMnemonicMarkers(const QString& text) {
  QString result;
  result.reserve(text.size());
  for (int i = 0; i < text.size(); ++i) {
    if (text.at(i) != QLatin1Char('&')) {
      result.append(text.at(i));
      continue;
    }
    if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
      result.append(QLatin1Char('&'));
      ++i;
    }
  }
  return result;
}

struct ContextMenuVisualStyle {
  int itemHeight = 32;
  int horizontalPadding = 12;
  int iconSize = 14;
  int iconTextGap = 8;
  int menuPadding = 4;
  int borderRadius = 8;
  int itemBorderRadius = 4;
  int minimumWidth = 160;
  int separatorHeight = 9;
  int shortcutGap = 24;
  int arrowColumnWidth = 12;
  int trailingColumnGap = 8;
  int borderWidth = 1;

  QColor background;
  QColor border;
  QColor text;
  QColor secondaryText;
  QColor disabledText;
  QColor hoverBackground;
  QColor hoverText;
  QColor dangerText;
  QColor dangerHoverText;
  QColor dangerHoverBackground;
  QColor divider;
  QColor checkmark;
  QFont font;
};

ContextMenuVisualStyle resolveVisualStyle(const AdContextMenu* menu) {
  ContextMenuVisualStyle style;
  const auto& manager = adqt::theme::ThemeManager::instance();
  const QWidget* logicalOwner = menu && menu->triggerWidget()
                                    ? menu->triggerWidget()
                                    : (menu ? menu->parentWidget() : nullptr);
  adqt::theme::ResolvedTheme resolved = manager.resolve(menu, logicalOwner);

  if (menu && menu->colorScheme() != AdContextMenu::ColorScheme::Inherit) {
    const adqt::theme::ThemeScheme requested =
        menu->colorScheme() == AdContextMenu::ColorScheme::Dark ? adqt::theme::ThemeScheme::Dark
                                                                : adqt::theme::ThemeScheme::Light;
    if (resolved.config.scheme != requested) {
      adqt::theme::ThemeConfig config = resolved.config;
      config.scheme = requested;
      resolved = adqt::theme::makeResolvedTheme(config);
    }
  }

  const auto& map = resolved.values;
  const AdContextMenu::ComponentTokens tokens =
      menu ? menu->componentTokens() : AdContextMenu::ComponentTokens{};

  style.itemHeight = tokenOr(tokens.itemHeight, std::max(24, qRound(map.controlHeight)));
  style.horizontalPadding = tokenOr(tokens.horizontalPadding, std::max(8, qRound(map.sizeSM)));
  style.iconSize = tokenOr(tokens.iconSize, std::max(12, qRound(map.fontSize)));
  style.iconTextGap = tokenOr(tokens.iconTextGap, std::max(6, qRound(map.sizeXS)));
  style.menuPadding = tokenOr(tokens.menuPadding, std::max(2, qRound(map.sizeXXS)));
  style.borderRadius = tokenOr(tokens.borderRadius, std::max(0, qRound(map.borderRadiusLG)));
  style.itemBorderRadius =
      tokenOr(tokens.itemBorderRadius, std::max(0, qRound(map.borderRadiusSM)));
  style.minimumWidth = tokenOr(tokens.minimumWidth, 160);
  style.separatorHeight = std::max(7, style.menuPadding * 2 + 1);
  style.borderWidth = std::max(1, qRound(map.lineWidth));

  style.font = menu ? menu->font() : QApplication::font();
  style.font.setPixelSize(std::max(12, qRound(map.fontSize)));

  style.background =
      tokenOr(tokens.background, colorOr(map.colorBgElevated, QColor(QStringLiteral("#ffffff"))));
  style.border =
      tokenOr(tokens.border, colorOr(map.colorBorderSecondary, QColor(QStringLiteral("#f0f0f0"))));
  style.text = tokenOr(tokens.text, colorOr(map.colorText, QColor(QStringLiteral("#141414"))));
  style.secondaryText = tokenOr(tokens.secondaryText,
                                colorOr(map.colorTextSecondary, QColor(QStringLiteral("#8c8c8c"))));
  style.disabledText = tokenOr(tokens.disabledText,
                               colorOr(map.colorTextQuaternary, QColor(QStringLiteral("#bfbfbf"))));
  style.hoverBackground = tokenOr(
      tokens.hoverBackground, colorOr(map.colorFillTertiary, QColor(QStringLiteral("#f5f5f5"))));
  style.hoverText = tokenOr(tokens.hoverText, style.text);
  style.dangerText =
      tokenOr(tokens.dangerText, colorOr(map.colorError, QColor(QStringLiteral("#ff4d4f"))));
  style.dangerHoverText = tokenOr(
      tokens.dangerHoverText,
      colorOr(map.colorErrorTextHover, colorOr(map.colorError, QColor(QStringLiteral("#ff4d4f")))));
  style.dangerHoverBackground = tokenOr(
      tokens.dangerHoverBackground, colorOr(map.colorErrorBg, QColor(QStringLiteral("#fff2f0"))));
  style.divider = tokenOr(tokens.divider, style.border);
  style.checkmark =
      tokenOr(tokens.checkmark, colorOr(map.colorPrimary, QColor(QStringLiteral("#1677ff"))));

  style.itemHeight = std::max(20, style.itemHeight);
  style.horizontalPadding = std::max(0, style.horizontalPadding);
  style.iconSize = std::max(10, style.iconSize);
  style.iconTextGap = std::max(0, style.iconTextGap);
  style.menuPadding = std::max(0, style.menuPadding);
  style.borderRadius = std::max(0, style.borderRadius);
  style.itemBorderRadius = std::max(0, style.itemBorderRadius);
  style.minimumWidth = std::max(1, style.minimumWidth);
  return style;
}

QAction* actionForOption(const QMenu* menu, const QStyleOptionMenuItem* option) {
  if (!menu || !option) {
    return nullptr;
  }
  for (QAction* action : menu->actions()) {
    if (!action || !action->isVisible()) {
      continue;
    }
    const QRect actionRect = menu->actionGeometry(action);
    if (actionRect == option->rect || actionRect.contains(option->rect.center())) {
      return action;
    }
  }
  return menu->actionAt(option->rect.center());
}

bool menuUsesIconColumn(const QMenu* menu) {
  if (!menu) {
    return false;
  }
  for (const QAction* action : menu->actions()) {
    if (action && action->isVisible() &&
        (action->isCheckable() || !action->icon().isNull() ||
         action->property(kActionIconProperty).isValid())) {
      return true;
    }
  }
  return false;
}

QString actionShortcutText(const QAction* action, const QString& optionText,
                           bool includeBadge = true) {
  const QString badge =
      action && includeBadge ? action->property(kActionBadgeProperty).toString() : QString{};
  QString shortcut;
  const qsizetype tab = optionText.indexOf(QLatin1Char('\t'));
  if (tab >= 0) {
    shortcut = optionText.mid(tab + 1);
  } else if (action && action->isShortcutVisibleInContextMenu() && !action->shortcut().isEmpty()) {
    shortcut = action->shortcut().toString(QKeySequence::NativeText);
  }
  if (badge.isEmpty()) {
    return shortcut;
  }
  return shortcut.isEmpty() ? badge : badge + QStringLiteral("  ") + shortcut;
}

QString actionLabelText(const QString& optionText) {
  const qsizetype tab = optionText.indexOf(QLatin1Char('\t'));
  return tab >= 0 ? optionText.left(tab) : optionText;
}

int maximumShortcutWidth(const QMenu* menu, const QFontMetrics& metrics, bool includeBadge = true) {
  int result = 0;
  if (!menu) {
    return result;
  }
  for (const QAction* action : menu->actions()) {
    if (!action || !action->isVisible() || action->isSeparator()) {
      continue;
    }
    const QString shortcut = actionShortcutText(action, action->text(), includeBadge);
    result = std::max(result, metrics.horizontalAdvance(shortcut));
  }
  return result;
}

int constrainedMenuItemWidth(const QMenu* menu, const ContextMenuVisualStyle& visual, int width,
                             int shortcutWidth) {
  if (!menu || menu->maximumWidth() >= QWIDGETSIZE_MAX) {
    return width;
  }

  const int availableWidth =
      std::max(1, menu->maximumWidth() - visual.menuPadding * 2 - shortcutWidth);
  return std::min(width, availableWidth);
}

QRect mirroredRect(Qt::LayoutDirection direction, const QRect& bounds, const QRect& logicalRect) {
  return QStyle::visualRect(direction, bounds, logicalRect);
}

void paintCheckmark(QPainter& painter, const QRect& rect, const QColor& color, bool exclusive) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (exclusive) {
    const qreal side = std::max<qreal>(4.0, std::min(rect.width(), rect.height()) * 0.45);
    const QRectF dot(rect.center().x() - side / 2.0, rect.center().y() - side / 2.0, side, side);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(dot);
  } else {
    const qreal stroke = std::clamp(rect.height() * 0.11, 1.4, 2.0);
    QPen pen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const qreal left = rect.left() + rect.width() * 0.18;
    const qreal midX = rect.left() + rect.width() * 0.43;
    const qreal right = rect.right() - rect.width() * 0.12;
    const qreal midY = rect.top() + rect.height() * 0.57;
    painter.drawLine(QPointF(left, midY), QPointF(midX, rect.bottom() - rect.height() * 0.20));
    painter.drawLine(QPointF(midX, rect.bottom() - rect.height() * 0.20),
                     QPointF(right, rect.top() + rect.height() * 0.25));
  }
  painter.restore();
}

void paintSubmenuArrow(QPainter& painter, const QRect& rect, const QColor& color,
                       Qt::LayoutDirection direction) {
  const qreal centerX = rect.center().x();
  const qreal centerY = rect.center().y();
  const qreal dx = std::max<qreal>(2.0, rect.width() * 0.18);
  const qreal dy = std::max<qreal>(3.0, rect.height() * 0.25);
  const qreal sign = direction == Qt::RightToLeft ? -1.0 : 1.0;

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  painter.drawLine(QPointF(centerX - sign * dx, centerY - dy),
                   QPointF(centerX + sign * dx, centerY));
  painter.drawLine(QPointF(centerX + sign * dx, centerY),
                   QPointF(centerX - sign * dx, centerY + dy));
  painter.restore();
}

}  // namespace

namespace detail {

class AdContextMenuStyle final : public QProxyStyle {
 public:
  explicit AdContextMenuStyle(AdContextMenu* menu) : menu_(menu) { setParent(menu); }

  void polish(QWidget* widget) override {
    QProxyStyle::polish(widget);
    if (widget == menu_) {
      widget->setGraphicsEffect(nullptr);
    }
  }

  int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr) const override {
    const ContextMenuVisualStyle visual = resolveVisualStyle(menu_);
    switch (metric) {
      case PM_MenuHMargin:
      case PM_MenuVMargin:
        return visual.menuPadding;
      case PM_MenuPanelWidth:
        return 0;
      case PM_SmallIconSize:
        return visual.iconSize;
      case PM_MenuButtonIndicator:
        return visual.arrowColumnWidth;
      case PM_MenuScrollerHeight:
        return visual.itemHeight;
      default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
  }

  QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contentsSize,
                         const QWidget* widget = nullptr) const override {
    if (type != CT_MenuItem) {
      return QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
    }

    const auto* menuOption = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    const auto* menu = qobject_cast<const QMenu*>(widget);
    if (!menuOption || !menu) {
      return QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
    }

    const ContextMenuVisualStyle visual = resolveVisualStyle(menu_);
    if (menuOption->menuItemType == QStyleOptionMenuItem::Separator) {
      return QSize(std::max(1, visual.minimumWidth - visual.menuPadding * 2),
                   visual.separatorHeight);
    }

    const QFontMetrics metrics(visual.font);
    const QString label = withoutMnemonicMarkers(actionLabelText(menuOption->text));
    // Elision uses fractional advances, so rounding down can truncate a content-sized item.
    const int labelWidth = qCeil(QFontMetricsF(visual.font).horizontalAdvance(label));
    const int shortcutWidth =
        std::max(maximumShortcutWidth(menu, metrics),
                 metrics.horizontalAdvance(actionShortcutText(nullptr, menuOption->text)));

    int width = visual.horizontalPadding * 2 + labelWidth;
    if (menuUsesIconColumn(menu)) {
      width += visual.iconSize + visual.iconTextGap;
    }
    if (shortcutWidth > 0) {
      width += visual.shortcutGap;
    }
    if (menuOption->menuItemType == QStyleOptionMenuItem::SubMenu) {
      width += visual.trailingColumnGap + visual.arrowColumnWidth;
    }
    width = std::max(visual.minimumWidth - visual.menuPadding * 2, width);
    // QMenu adds its own shortcut column after asking the style for item sizes.
    // Badge metadata is invisible to that calculation, so reserve the remainder here.
    const int qtShortcutWidth = maximumShortcutWidth(menu, QFontMetrics(menu->font()), false);
    width += std::max(0, shortcutWidth - qtShortcutWidth);
    width = constrainedMenuItemWidth(menu, visual, width, qtShortcutWidth);
    return QSize(width, visual.itemHeight);
  }

  void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override {
    if (element == PE_FrameMenu) {
      return;
    }
    if (element != PE_PanelMenu || !option || !painter) {
      QProxyStyle::drawPrimitive(element, option, painter, widget);
      return;
    }

    const ContextMenuVisualStyle visual = resolveVisualStyle(menu_);
    const qreal devicePixelRatio =
        painter->device() ? painter->device()->devicePixelRatioF() : menu_->devicePixelRatioF();
    const qreal logicalBorderWidth =
        detail::deviceAlignedPenWidth(visual.borderWidth, devicePixelRatio);
    QRectF panelRect = option->rect;
    const qreal inset = logicalBorderWidth / 2.0;
    panelRect.adjust(inset, inset, -inset, -inset);

    const QPainterPath path =
        detail::roundedButtonPath(panelRect, visual.borderRadius, visual.borderRadius,
                                  visual.borderRadius, visual.borderRadius);
    painter->save();
    // A translucent popup can receive an uninitialized backing store on
    // Windows.  Clear the complete panel clip first so pixels outside the
    // rounded path are transparent instead of retaining opaque garbage.
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->fillRect(option->rect, Qt::transparent);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto background = menu_->backgroundFrame();
    if (background.image.isNull()) {
      painter->setPen(
          detail::makeButtonBorderPen(visual.border, logicalBorderWidth, Qt::SolidLine));
      painter->setBrush(visual.background);
      painter->drawPath(path);
    } else {
      QColor base = visual.background;
      base.setAlpha(255);
      painter->setPen(Qt::NoPen);
      painter->setBrush(base);
      painter->drawPath(path);
      painter->save();
      painter->setClipPath(path);
      const QRectF placement = background.normalizedPlacement;
      const QRectF target(option->rect.x() + placement.x() * option->rect.width(),
                          option->rect.y() + placement.y() * option->rect.height(),
                          placement.width() * option->rect.width(),
                          placement.height() * option->rect.height());
      painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
      painter->setOpacity(background.imageOpacity);
      painter->drawPixmap(target, background.image, QRectF(background.image.rect()));
      painter->setOpacity(1.0);
      QColor mask = visual.background;
      mask.setAlphaF(mask.alphaF() * static_cast<float>(background.maskOpacity));
      painter->fillRect(option->rect, mask);
      painter->restore();
      painter->setPen(
          detail::makeButtonBorderPen(visual.border, logicalBorderWidth, Qt::SolidLine));
      painter->setBrush(Qt::NoBrush);
      painter->drawPath(path);
    }
    painter->restore();
  }

  void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                   const QWidget* widget = nullptr) const override {
    if (element != CE_MenuItem || !option || !painter) {
      QProxyStyle::drawControl(element, option, painter, widget);
      return;
    }

    const auto* menuOption = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    const auto* menu = qobject_cast<const QMenu*>(widget);
    if (!menuOption || !menu) {
      QProxyStyle::drawControl(element, option, painter, widget);
      return;
    }

    const ContextMenuVisualStyle visual = resolveVisualStyle(menu_);
    if (menuOption->menuItemType == QStyleOptionMenuItem::Separator) {
      const qreal devicePixelRatio =
          painter->device() ? painter->device()->devicePixelRatioF() : menu_->devicePixelRatioF();
      const qreal logicalWidth =
          detail::deviceAlignedPenWidth(std::max(1, visual.borderWidth), devicePixelRatio);
      const qreal y = detail::deviceAlignedStrokeCenter(menuOption->rect.center().y(), logicalWidth,
                                                        devicePixelRatio);
      const QRect lineRect =
          menuOption->rect.adjusted(visual.horizontalPadding, 0, -visual.horizontalPadding, 0);
      painter->save();
      painter->setPen(detail::makeButtonBorderPen(visual.divider, logicalWidth, Qt::SolidLine));
      painter->drawLine(QPointF(lineRect.left(), y), QPointF(lineRect.right(), y));
      painter->restore();
      return;
    }

    QAction* action = actionForOption(menu, menuOption);
    const bool enabled = menuOption->state.testFlag(State_Enabled);
    const bool selected = enabled && menuOption->state.testFlag(State_Selected);
    const bool danger = action && action->property(kActionDangerProperty).toBool();

    QColor textColor = danger ? visual.dangerText : visual.text;
    QColor background(Qt::transparent);
    if (!enabled) {
      textColor = visual.disabledText;
    } else if (selected && danger) {
      textColor = visual.dangerHoverText;
      background = visual.dangerHoverBackground;
    } else if (selected) {
      textColor = visual.hoverText;
      background = visual.hoverBackground;
    }

    if (background.alpha() > 0) {
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing, true);
      painter->setPen(Qt::NoPen);
      painter->setBrush(background);
      painter->drawRoundedRect(QRectF(menuOption->rect), visual.itemBorderRadius,
                               visual.itemBorderRadius);
      painter->restore();
    }

    const bool iconColumn = menuUsesIconColumn(menu);
    const bool submenuItem = menuOption->menuItemType == QStyleOptionMenuItem::SubMenu;
    const QFontMetrics metrics(visual.font);
    const QString shortcut = actionShortcutText(action, menuOption->text);
    // Only this item's shortcut may reduce the space available for its label.
    const int shortcutWidth = metrics.horizontalAdvance(shortcut);
    const QRect bounds = menuOption->rect;

    int left = bounds.left() + visual.horizontalPadding;
    int right = bounds.right() - visual.horizontalPadding;
    QRect logicalIconRect;
    if (iconColumn) {
      logicalIconRect = QRect(left, bounds.top() + (bounds.height() - visual.iconSize) / 2,
                              visual.iconSize, visual.iconSize);
      left += visual.iconSize + visual.iconTextGap;
    }

    QRect logicalArrowRect;
    if (submenuItem) {
      logicalArrowRect = QRect(right - visual.arrowColumnWidth + 1,
                               bounds.top() + (bounds.height() - visual.iconSize) / 2,
                               visual.arrowColumnWidth, visual.iconSize);
      right -= visual.arrowColumnWidth + visual.trailingColumnGap;
    }

    QRect logicalShortcutRect;
    if (shortcutWidth > 0) {
      logicalShortcutRect =
          QRect(right - shortcutWidth + 1, bounds.top(), shortcutWidth, bounds.height());
      right -= shortcutWidth + visual.shortcutGap;
    }
    const QRect logicalTextRect(left, bounds.top(), std::max(0, right - left + 1), bounds.height());

    const QRect iconRect = mirroredRect(menuOption->direction, bounds, logicalIconRect);
    const QRect arrowRect = mirroredRect(menuOption->direction, bounds, logicalArrowRect);
    const QRect shortcutRect = mirroredRect(menuOption->direction, bounds, logicalShortcutRect);
    const QRect textRect = mirroredRect(menuOption->direction, bounds, logicalTextRect);

    if (iconColumn && menuOption->checkType != QStyleOptionMenuItem::NotCheckable &&
        menuOption->checked) {
      paintCheckmark(*painter, iconRect, enabled ? visual.checkmark : visual.disabledText,
                     menuOption->checkType == QStyleOptionMenuItem::Exclusive);
    } else if (iconColumn) {
      adqt::icons::IconRef iconRef;
      if (action) {
        iconRef = action->property(kActionIconProperty).value<adqt::icons::IconRef>();
      }
      if (adqt::icons::isValid(iconRef)) {
        if (!iconRef.colors().primarySlot()) {
          iconRef = iconRef.withColors(iconRef.colors().withPrimary(textColor));
        }
        adqt::icons::paintIcon(
            painter, iconRef, QRectF(iconRect),
            {QSize(), 0.0, enabled ? QIcon::Normal : QIcon::Disabled, QIcon::Off});
      } else if (!menuOption->icon.isNull()) {
        menuOption->icon.paint(painter, iconRect, Qt::AlignCenter,
                               enabled ? QIcon::Normal : QIcon::Disabled,
                               menuOption->checked ? QIcon::On : QIcon::Off);
      }
    }

    const QString label = actionLabelText(menuOption->text);
    int textFlags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextShowMnemonic;
    textFlags |= menuOption->direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft;
    if (styleHint(SH_UnderlineShortcut, menuOption, widget) == 0) {
      textFlags |= Qt::TextHideMnemonic;
    }

    painter->save();
    painter->setFont(visual.font);
    painter->setPen(textColor);
    painter->drawText(textRect, textFlags,
                      metrics.elidedText(label, Qt::ElideRight, textRect.width()));
    if (!shortcut.isEmpty() && shortcutRect.isValid()) {
      painter->setPen(enabled ? visual.secondaryText : visual.disabledText);
      const int shortcutFlags =
          Qt::AlignVCenter | Qt::TextSingleLine |
          (menuOption->direction == Qt::RightToLeft ? Qt::AlignLeft : Qt::AlignRight);
      painter->drawText(shortcutRect, shortcutFlags, shortcut);
    }
    painter->restore();

    if (menuOption->menuItemType == QStyleOptionMenuItem::SubMenu && arrowRect.isValid()) {
      paintSubmenuArrow(*painter, arrowRect, textColor, menuOption->direction);
    }
  }

 private:
  QPointer<AdContextMenu> menu_;
};

}  // namespace detail

class AdContextMenu::Private {
 public:
  ColorScheme colorScheme = ColorScheme::Inherit;
  ComponentTokens componentTokens;
  BackgroundFrame backgroundFrame;
  QPointer<QWidget> triggerWidget;
  QPointer<detail::AdContextMenuStyle> menuStyle;
  quint64 popupGeneration = 0;
  bool popupPending = false;
  bool nativeTracking = false;
  bool useNativeMenu = false;
  bool customSurface = false;
  bool themeConnected = false;
  bool deferredSurface = false;
  bool deleteOnHide = false;
  bool retiring = false;
  int executionDepth = 0;
};

void AdContextMenu::configureSurfaceAttributes() {
  const bool translucent = !d_->useNativeMenu;
  Qt::WindowFlags flags = Qt::Popup;
  if (translucent) {
    // Keep QMenu's popup behavior without an opaque native frame or shadow.
    flags |= Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
  }
  // QMenu creates its window before aboutToShow. Even an empty lazy shell
  // needs these attributes now so its eventual window has an alpha channel.
  // Setting them does not allocate a window or backing store.
  setWindowFlags(flags);
  setAttribute(Qt::WA_TranslucentBackground, translucent);
  setAttribute(Qt::WA_NoSystemBackground, translucent);
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  setAutoFillBackground(false);
}

void AdContextMenu::configureCustomSurface() {
  if (d_->customSurface) {
    return;
  }
  d_->customSurface = true;
  d_->nativeTracking = false;
  d_->popupPending = false;
  configureSurfaceAttributes();

  // Popups do not inherit their owner's font by default. DirectWrite's default
  // hinting retains grid fitting at fractional DPI, so give every menu (including
  // submenus and ownerless tray menus) smooth outlines for labels and shortcuts.
  QFont menuFont = font();
  menuFont.setHintingPreference(QFont::PreferNoHinting);
  setFont(menuFont);

  // The ARGB surface is also the window shape.  Do not add QWidget::setMask()
  // here: its binary QRegion is rounded independently at device-pixel
  // boundaries and clips the antialiased outer half of the 1 px border,
  // producing asymmetric or missing corner pixels at fractional DPI scales.

  if (!d_->menuStyle) {
    d_->menuStyle = new detail::AdContextMenuStyle(this);
  }
  QMenu::setStyle(d_->menuStyle);
  if (!d_->themeConnected) {
    d_->themeConnected = true;
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this]() { refreshVisuals(true); });
  }
  refreshVisuals(true);
}

void AdContextMenu::configurePlatformSurface() {
  d_->customSurface = false;
  d_->nativeTracking = false;
  d_->popupPending = false;
  if (d_->menuStyle) {
    if (style() == d_->menuStyle.data()) {
      QMenu::setStyle(nullptr);
    }
    delete d_->menuStyle.data();
    d_->menuStyle = nullptr;
  }
  configureSurfaceAttributes();
#ifdef Q_OS_MACOS
  detail::initializeNativeContextMenu(this);
#endif
}

AdContextMenu::AdContextMenu(QWidget* parent) : AdContextMenu(parent, false) {}

AdContextMenu::AdContextMenu(QWidget* parent, bool deferredSurface)
    : QMenu(parent), d_(std::make_unique<Private>()) {
  d_->deferredSurface = deferredSurface;
  setObjectName(QStringLiteral("ad-context-menu"));
  setSeparatorsCollapsible(false);
  setToolTipsVisible(true);

#ifdef Q_OS_MACOS
  d_->useNativeMenu = true;
  if (!deferredSurface) detail::initializeNativeContextMenu(this);
  connect(this, &QMenu::aboutToShow, this, [this]() {
    if (d_->useNativeMenu && detail::usesNativeContextMenu()) {
      d_->nativeTracking = true;
      for (QAction* action : actions()) {
        if (action->property(kActionBadgeProperty).isValid()) {
          detail::syncNativeContextMenuBadge(this, action);
        }
      }
    }
  });
  connect(this, &QMenu::aboutToHide, this, [this]() { d_->nativeTracking = false; });
#else
  d_->useNativeMenu = false;
  if (!deferredSurface) configureCustomSurface();
#endif
  if (deferredSurface) configureSurfaceAttributes();
  connect(this, &QMenu::aboutToShow, this, [this]() {
    if (d_->deferredSurface) {
      d_->deferredSurface = false;
      if (!d_->useNativeMenu) configureCustomSurface();
    }
  });
  connect(this, &QMenu::aboutToHide, this, [this]() {
    if (d_->deleteOnHide) retirePopup();
  });
}

AdContextMenu::AdContextMenu(const QString& title, QWidget* parent) : AdContextMenu(parent) {
  setTitle(title);
}

AdContextMenu::~AdContextMenu() {
  d_->deleteOnHide = false;
  dismissPopup();
  if (d_->triggerWidget) {
    d_->triggerWidget->removeEventFilter(this);
  }
  // Retire platform items while QMenu::actionEvent still handles action removal.
  // QWidget's destructor no longer dispatches that override.
  clear();
}

bool AdContextMenu::nativeMenuEnabled() const { return d_->useNativeMenu; }

void AdContextMenu::setNativeMenuEnabled(bool enabled) {
#ifndef Q_OS_MACOS
  enabled = false;
#endif
  if (d_->useNativeMenu == enabled) {
    return;
  }
  d_->useNativeMenu = enabled;
  if (d_->deferredSurface) {
    configureSurfaceAttributes();
    return;
  }
  if (enabled) {
    configurePlatformSurface();
  } else {
    configureCustomSurface();
  }
  const QList<QAction*> menuActions = actions();
  for (QAction* action : menuActions) {
    applyStoredActionIcon(action);
#ifdef Q_OS_MACOS
    if (enabled && detail::usesNativeContextMenu() &&
        action->property(kActionBadgeProperty).isValid()) {
      detail::syncNativeContextMenuBadge(this, action);
    }
#endif
    auto* submenu = qobject_cast<AdContextMenu*>(action != nullptr ? action->menu() : nullptr);
    if (submenu != nullptr) {
      submenu->setNativeMenuEnabled(enabled);
    }
  }
}

AdContextMenu::ColorScheme AdContextMenu::colorScheme() const { return d_->colorScheme; }

void AdContextMenu::setColorScheme(ColorScheme value) {
  if (d_->colorScheme == value) {
    return;
  }
  d_->colorScheme = value;
  refreshVisuals(true);
  emit colorSchemeChanged(value);
}

AdContextMenu::ComponentTokens AdContextMenu::componentTokens() const {
  return d_->componentTokens;
}

void AdContextMenu::setComponentTokens(const ComponentTokens& tokens) {
  d_->componentTokens = tokens;
  refreshVisuals(true);
  emit componentTokensChanged();
}

void AdContextMenu::resetComponentTokens() {
  d_->componentTokens = {};
  refreshVisuals(true);
  emit componentTokensChanged();
}

AdContextMenu::BackgroundFrame AdContextMenu::backgroundFrame() const {
  return d_->backgroundFrame;
}

void AdContextMenu::setBackgroundFrame(const BackgroundFrame& frame) {
  BackgroundFrame normalized = frame;
  const QRectF placement = frame.normalizedPlacement;
  if (frame.image.isNull() || !placement.isValid() || !std::isfinite(placement.x()) ||
      !std::isfinite(placement.y()) || !std::isfinite(placement.width()) ||
      !std::isfinite(placement.height())) {
    normalized = {};
  } else {
    normalized.imageOpacity = std::isfinite(frame.imageOpacity)
                                  ? std::clamp(frame.imageOpacity, qreal(0.0), qreal(1.0))
                                  : 1.0;
    normalized.maskOpacity = std::isfinite(frame.maskOpacity)
                                 ? std::clamp(frame.maskOpacity, qreal(0.0), qreal(1.0))
                                 : 0.8;
  }
  const auto& previous = d_->backgroundFrame;
  if (previous.image.cacheKey() == normalized.image.cacheKey() &&
      previous.normalizedPlacement == normalized.normalizedPlacement &&
      previous.imageOpacity == normalized.imageOpacity &&
      previous.maskOpacity == normalized.maskOpacity) {
    return;
  }
  d_->backgroundFrame = std::move(normalized);
  if (!d_->useNativeMenu) {
    update();
  }
}

void AdContextMenu::resetBackgroundFrame() { setBackgroundFrame({}); }

QWidget* AdContextMenu::triggerWidget() const { return d_->triggerWidget.data(); }

void AdContextMenu::setTriggerWidget(QWidget* widget) {
  if (d_->triggerWidget == widget) {
    return;
  }
  if (d_->triggerWidget) {
    d_->triggerWidget->removeEventFilter(this);
  }
  d_->triggerWidget = widget;
  if (d_->triggerWidget) {
    d_->triggerWidget->installEventFilter(this);
  }
  refreshVisuals(false);
  emit triggerWidgetChanged(widget);
}

QAction* AdContextMenu::addItem(const QString& text, const adqt::icons::IconRef& icon,
                                const QKeySequence& shortcut) {
  QAction* action = QMenu::addAction(text);
  if (adqt::icons::isValid(icon)) {
    setActionIcon(action, icon);
  }
  if (!shortcut.isEmpty()) {
    action->setShortcut(shortcut);
    action->setShortcutVisibleInContextMenu(true);
  }
  return action;
}

AdContextMenu* AdContextMenu::addSubMenu(const QString& text, const adqt::icons::IconRef& icon) {
  auto* submenu = new AdContextMenu(text, this);
  submenu->setNativeMenuEnabled(nativeMenuEnabled());
  submenu->setColorScheme(colorScheme());
  submenu->setComponentTokens(componentTokens());
  connect(this, &AdContextMenu::colorSchemeChanged, submenu, &AdContextMenu::setColorScheme);
  connect(this, &AdContextMenu::componentTokensChanged, submenu,
          [this, submenu]() { submenu->setComponentTokens(componentTokens()); });
  QMenu::addMenu(submenu);
  if (adqt::icons::isValid(icon)) {
    setActionIcon(submenu->menuAction(), icon);
  }
  return submenu;
}

class AdContextMenu::LazySubMenu final : public QObject {
 public:
  LazySubMenu(AdContextMenu* parent, QAction* action, ContentFactory factory)
      : QObject(action), parent_(parent), action_(action), factory_(std::move(factory)) {
    installShell();
    connect(parent_, &QMenu::aboutToShow, this, [this]() {
      if (!parent_->isRetiring() && (!shell_ || shell_->isRetiring())) installShell();
    });
  }
  ~LazySubMenu() override {
    tearingDown_ = true;
    delete shell_.data();
  }

 private:
  void installShell() {
    auto* menu = new AdContextMenu(parent_, true);
    shell_ = menu;
    menu->setTitle(action_->text());
    menu->setNativeMenuEnabled(parent_->nativeMenuEnabled());
    menu->setColorScheme(parent_->colorScheme());
    menu->setComponentTokens(parent_->componentTokens());
    menu->setDeleteOnHide();
    action_->setMenu(menu);
    connect(parent_, &AdContextMenu::colorSchemeChanged, menu, &AdContextMenu::setColorScheme);
    connect(parent_, &AdContextMenu::componentTokensChanged, menu,
            [this, menu]() { menu->setComponentTokens(parent_->componentTokens()); });
    connect(menu, &QMenu::aboutToShow, menu, [this, menu]() {
      menu->setTitle(action_->text());
      menu->menuAction()->setIcon(action_->icon());
      menu->menuAction()->setObjectName(action_->objectName());
      menu->clear();
      factory_(menu);
    });
    connect(menu, &QObject::destroyed, this, [this, menu]() {
      // An immediate parent reopen may already have installed a fresh shell.
      if (shell_ && shell_ != menu) return;
      shell_ = nullptr;
      if (!tearingDown_ && parent_->isPopupVisible() && !parent_->isRetiring()) installShell();
    });
  }
  AdContextMenu* parent_;
  QAction* action_;
  ContentFactory factory_;
  QPointer<AdContextMenu> shell_;
  bool tearingDown_ = false;
};

QAction* AdContextMenu::addLazySubMenu(const QString& text, ContentFactory factory,
                                       const adqt::icons::IconRef& icon) {
  QAction* action = addItem(text, icon);
  new LazySubMenu(this, action, std::move(factory));
  return action;
}

void AdContextMenu::setActionIcon(QAction* action, const adqt::icons::IconRef& icon) {
  if (!action) {
    return;
  }
  if (adqt::icons::isValid(icon)) {
    action->setProperty(kActionIconProperty, QVariant::fromValue(icon));
    action->setIconVisibleInMenu(true);
    applyStoredActionIcon(action);
  } else {
    action->setProperty(kActionIconProperty, QVariant());
    action->setIcon(QIcon());
  }
  // QAction::setIcon already sends ActionChanged to every associated menu,
  // invalidating Qt's complete item geometry (including the shared icon column).
  // Sending that event for unrelated actions makes menu population quadratic.
}

adqt::icons::IconRef AdContextMenu::actionIcon(const QAction* action) const {
  return action ? action->property(kActionIconProperty).value<adqt::icons::IconRef>()
                : adqt::icons::IconRef{};
}

void AdContextMenu::setActionDanger(QAction* action, bool danger) {
  if (!action || action->property(kActionDangerProperty).toBool() == danger) {
    return;
  }
  action->setProperty(kActionDangerProperty, danger);
  update(actionGeometry(action));
}

bool AdContextMenu::actionDanger(const QAction* action) const {
  return action && action->property(kActionDangerProperty).toBool();
}

void AdContextMenu::setActionBadge(QAction* action, const QString& text) {
  if (!action || actionBadge(action) == text) {
    return;
  }
  action->setProperty(kActionBadgeProperty, text);
  // Dynamic properties do not invalidate QMenu item geometry. Notify every menu
  // sharing this action so its trailing column and native badge stay in sync.
  const auto owners = action->associatedObjects();
  for (QObject* owner : owners) {
    if (auto* menu = qobject_cast<AdContextMenu*>(owner)) {
      QActionEvent event(QEvent::ActionChanged, action);
      QCoreApplication::sendEvent(menu, &event);
    }
  }
}

QString AdContextMenu::actionBadge(const QAction* action) const {
  return action ? action->property(kActionBadgeProperty).toString() : QString{};
}

void AdContextMenu::actionEvent(QActionEvent* event) {
  QMenu::actionEvent(event);
#ifdef Q_OS_MACOS
  if (d_->useNativeMenu && detail::usesNativeContextMenu() &&
      event->type() != QEvent::ActionRemoved &&
      event->action()->property(kActionBadgeProperty).isValid()) {
    detail::syncNativeContextMenuBadge(this, event->action());
  }
#endif
}

bool AdContextMenu::isPopupVisible() const {
  return d_->popupPending || d_->nativeTracking || QMenu::isVisible();
}

void AdContextMenu::dismissPopup() {
  ++d_->popupGeneration;
  d_->popupPending = false;
#ifdef Q_OS_MACOS
  if (d_->useNativeMenu && d_->nativeTracking) {
    detail::dismissNativeContextMenu(this);
  }
#endif
  QMenu::hide();
  if (d_->deleteOnHide) retirePopup();
}

void AdContextMenu::setDeleteOnHide(bool enabled) { d_->deleteOnHide = enabled; }

bool AdContextMenu::isRetiring() const { return d_->retiring; }

void AdContextMenu::retirePopup() {
  if (d_->retiring) return;
  d_->retiring = true;
  AdContextMenu* executing = nullptr;
  for (auto* owner = this; owner != nullptr;
       owner = qobject_cast<AdContextMenu*>(owner->parentWidget())) {
    if (owner->d_->executionDepth > 0) executing = owner;
  }
  if (executing) {
    connect(executing, &AdContextMenu::popupFinished, this, &QObject::deleteLater);
  } else {
    // QMenu emits aboutToHide before activating the selected action. Retire
    // after that event, never from inside popup teardown or native tracking.
    QMetaObject::invokeMethod(this, &QObject::deleteLater, Qt::QueuedConnection);
  }
}

QSize AdContextMenu::sizeHint() const {
#ifdef Q_OS_MACOS
  if (d_->useNativeMenu && detail::usesNativeContextMenu()) {
    return detail::nativeContextMenuSize(const_cast<AdContextMenu*>(this));
  }
#endif
  return QMenu::sizeHint();
}

void AdContextMenu::popupAt(const QPoint& globalPosition) {
  if (d_->retiring) return;
#ifdef Q_OS_MACOS
  if (d_->useNativeMenu && detail::usesNativeContextMenu()) {
    if (d_->nativeTracking) {
      return;
    }
    d_->popupPending = true;
    const quint64 generation = ++d_->popupGeneration;
    QMetaObject::invokeMethod(
        this,
        [this, generation, globalPosition]() {
          if (!d_->popupPending || generation != d_->popupGeneration) {
            return;
          }
          d_->popupPending = false;
          execAt(globalPosition, activeAction());
        },
        Qt::QueuedConnection);
    return;
  }
#endif
  refreshVisuals(true);
  QMenu::popup(globalPosition);
}

QAction* AdContextMenu::execAt(const QPoint& globalPosition, QAction* initialAction) {
  if (d_->retiring) return nullptr;
  QPointer<AdContextMenu> lifetime(this);
  ++d_->executionDepth;
  QAction* selected = nullptr;
#ifdef Q_OS_MACOS
  if (d_->useNativeMenu && detail::usesNativeContextMenu()) {
    if (d_->nativeTracking) {
      --d_->executionDepth;
      return nullptr;
    }
    ++d_->popupGeneration;
    d_->popupPending = false;
    d_->nativeTracking = true;
    QPointer<AdContextMenu> guard(this);
    selected = detail::execNativeContextMenu(this, globalPosition, initialAction);
    if (guard) {
      d_->nativeTracking = false;
    }
  } else {
    refreshVisuals(true);
    selected = QMenu::exec(globalPosition, initialAction);
  }
#else
  refreshVisuals(true);
  selected = QMenu::exec(globalPosition, initialAction);
#endif
  if (lifetime) {
    --d_->executionDepth;
    emit popupFinished();
  }
  return selected;
}

QAction* AdContextMenu::execNativePopup(const std::function<void()>& presenter) {
  if (d_->retiring || d_->nativeTracking) return nullptr;
  QPointer<AdContextMenu> lifetime(this);
  ++d_->executionDepth;
  d_->nativeTracking = true;
#ifdef Q_OS_MACOS
  QAction* selected = detail::trackNativeContextMenu(this, presenter);
#else
  presenter();
  QAction* selected = nullptr;
#endif
  if (lifetime) {
    d_->nativeTracking = false;
    --d_->executionDepth;
    emit popupFinished();
  }
  return selected;
}

bool AdContextMenu::eventFilter(QObject* watched, QEvent* event) {
  if (watched == d_->triggerWidget && event && event->type() == QEvent::ContextMenu) {
    auto* contextEvent = static_cast<QContextMenuEvent*>(event);
    if (!d_->deleteOnHide && !d_->retiring && d_->triggerWidget && d_->triggerWidget->isEnabled() &&
        !actions().isEmpty()) {
      popupAt(contextEvent->globalPos());
      contextEvent->accept();
      return true;
    }
  }
  return QMenu::eventFilter(watched, event);
}

void AdContextMenu::changeEvent(QEvent* event) {
  QMenu::changeEvent(event);
  if (!event) {
    return;
  }
  if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::LayoutDirectionChange) {
    refreshVisuals(true);
  }
}

void AdContextMenu::showEvent(QShowEvent* event) {
  refreshVisuals(false);
  QMenu::showEvent(event);
}

void AdContextMenu::hideEvent(QHideEvent* event) {
  QMenu::hideEvent(event);
  if (d_->useNativeMenu || d_->deleteOnHide) {
    return;
  }
  // The translucent popup's backing store is the dominant resident cost of a
  // hidden menu (its full physical-size ARGB surface stays allocated until the
  // widget is destroyed).  Destroying the native window releases it while
  // keeping every widget-level state intact; Qt rebuilds the window on the next
  // popup.  Destroying from inside the hide sequence would re-enter popup
  // teardown, so defer to the event loop, and skip when the menu was re-shown
  // in the meantime (submenu chains, immediate re-popup).
  QMetaObject::invokeMethod(
      this,
      [this]() {
        if (!d_->useNativeMenu && !isVisible() && windowHandle() != nullptr) {
          destroy();
        }
      },
      Qt::QueuedConnection);
}

void AdContextMenu::paintEvent(QPaintEvent* event) {
  if (!d_->useNativeMenu) {
    // A native popup's backing store is not guaranteed to be initialized before
    // the first paint. Clear the entire surface with Source composition before
    // QMenu asks the style to paint the rounded panel and its items. This makes
    // pixels outside the panel's path explicitly transparent instead of
    // retaining the platform's black window background.
    QPainter painter(this);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.end();
  }
  QMenu::paintEvent(event);
}

void AdContextMenu::applyStoredActionIcon(QAction* action) {
  if (action == nullptr) {
    return;
  }
  const auto icon = action->property(kActionIconProperty).value<adqt::icons::IconRef>();
  if (!adqt::icons::isValid(icon)) {
    return;
  }
#ifdef Q_OS_MACOS
  action->setIcon(d_->useNativeMenu ? detail::nativeContextMenuIcon(this, icon)
                                    : adqt::icons::makeIcon(icon));
#else
  action->setIcon(adqt::icons::makeIcon(icon));
#endif
}

void AdContextMenu::refreshVisuals(bool relayout) {
  if (d_->deferredSurface) return;
  if (d_->useNativeMenu) {
#ifdef Q_OS_MACOS
    Q_UNUSED(relayout);
    for (QAction* action : actions()) {
      applyStoredActionIcon(action);
    }
#endif
    return;
  }
  if (relayout) {
    const QList<QAction*> menuActions = actions();
    for (QAction* action : menuActions) {
      if (!action) {
        continue;
      }
      QActionEvent actionChanged(QEvent::ActionChanged, action);
      QCoreApplication::sendEvent(this, &actionChanged);
    }
    updateGeometry();
    if (isVisible()) {
      adjustSize();
    }
  }
  update();
}

}  // namespace adqt::widgets
