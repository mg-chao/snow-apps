#include "qt_tooltip_bridge.h"

#include "overlay_popup_surface.h"

#include "../tooltip.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPointer>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <algorithm>

namespace adqt::widgets::detail {

namespace {

constexpr int kLeaveHideDelayMs = 300;

int tooltipWakeUpDelay(const QWidget* widget) {
  QStyle* style = widget ? widget->style() : QApplication::style();
  if (!style) {
    return 700;
  }
  return std::max(0, style->styleHint(QStyle::SH_ToolTip_WakeUpDelay, nullptr, widget));
}

bool widgetInTree(const QWidget* candidate, const QWidget* root) {
  return candidate && root &&
         (candidate == root || root->isAncestorOf(const_cast<QWidget*>(candidate)));
}

int tooltipDisplayTime(int configuredDuration, const QString& text) {
  if (configuredDuration > 0) {
    return configuredDuration;
  }
  qsizetype textLength = text.size();
  if (Qt::mightBeRichText(text)) {
    QTextDocument document;
    document.setHtml(text);
    textLength = document.toPlainText().size();
  }
  const qsizetype extraCharacters = std::max<qsizetype>(0, textLength - 100);
  return 10000 + 40 * static_cast<int>(extraCharacters);
}

struct PopupTooltipRoute {
  QPointer<QObject> owner;
  QPointer<QWidget> triggerRoot;
  QPointer<QWidget> popupSurface;
  quint64 activationOrder = 0;
};

struct PendingTooltipRequest {
  QPointer<QObject> routeOwner;
  QPointer<QWidget> popup;
  QPointer<QWidget> target;
  QPointer<QWidget> anchor;
  QString text;
  QRect activeRect;
  QRect anchorRect;
  int displayTimeMs = 0;
  quint64 hoverGeneration = 0;
  bool usesPopupRoute = false;
};

class QtTooltipBridge final : public QObject {
 public:
  explicit QtTooltipBridge(QObject* parent) : QObject(parent) {
    createTooltip();

    expireTimer_.setSingleShot(true);
    connect(&expireTimer_, &QTimer::timeout, this, [this]() { hideImmediately(); });
    hideTimer_.setSingleShot(true);
    connect(&hideTimer_, &QTimer::timeout, this, [this]() { hideImmediately(); });
    wakeTimer_.setSingleShot(true);
    connect(&wakeTimer_, &QTimer::timeout, this, [this]() { showPendingTooltip(); });

    if (qApp) {
      qApp->installEventFilter(this);
    }
  }

  ~QtTooltipBridge() override {
    if (qApp) {
      qApp->removeEventFilter(this);
    }
  }

  void enableApplicationTooltips() { applicationTooltipsEnabled_ = true; }

  void showText(QWidget* widget, const QString& text, int displayTimeMs) {
    pruneRoutes();
    QToolTip::hideText();

    // QToolTip::showText() delays a hide request while a tip is visible and
    // otherwise leaves the current tip untouched.
    if (text.isEmpty()) {
      clearPendingTooltip();
      scheduleHide();
      return;
    }

    if (!widget || !widget->isVisible() || triggerRouteFor(widget)) {
      return;
    }

    QObject* tooltipManager = widget->property(kTooltipManagerProperty).value<QObject*>();
    if (tooltipManager && tooltipManager != tooltip_) {
      return;
    }

    const PopupTooltipRoute* route = popupRouteFor(widget);
    const PendingTooltipRequest request = makeTooltipRequest(
        widget, text, QRect(), tooltipDisplayTime(displayTimeMs, text), 0, route);
    if (!request.anchor || !request.anchorRect.isValid()) {
      return;
    }

    if (tooltip_ && tooltip_->isVisible()) {
      if (requestMatches(widget, text, widget->rect().center(), activeTarget_, activeText_,
                         activeRect_)) {
        return;
      }
      clearPendingTooltip();
      showTooltip(request);
      return;
    }

    clearPendingTooltip();
    showTooltip(request);
  }

  void syncRoute(QObject* owner, QWidget* triggerRoot, QWidget* popupSurface, bool active) {
    pruneRoutes();
    auto existing =
        std::find_if(routes_.begin(), routes_.end(),
                     [owner](const PopupTooltipRoute& route) { return route.owner == owner; });

    if (!active || !owner || !triggerRoot || !popupSurface || !popupSurface->isWindow() ||
        popupSurface->windowType() == Qt::ToolTip) {
      if (existing != routes_.end()) {
        const bool activeRouteRemoved =
            activeUsesPopupRoute_ && activeRouteOwner_ == existing->owner;
        const bool pendingRouteRemoved =
            pendingRequest_.usesPopupRoute && pendingRequest_.routeOwner == existing->owner;
        routes_.erase(existing);
        if (activeRouteRemoved || pendingRouteRemoved) {
          hideImmediately();
        }
      }
      return;
    }

    if (existing != routes_.end() && existing->triggerRoot == triggerRoot &&
        existing->popupSurface == popupSurface) {
      return;
    }

    PopupTooltipRoute route;
    route.owner = owner;
    route.triggerRoot = triggerRoot;
    route.popupSurface = popupSurface;
    route.activationOrder = ++activationCounter_;
    if (existing != routes_.end()) {
      *existing = route;
    } else {
      routes_.append(route);
    }

    QToolTip::hideText();
    hideImmediately();
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!watched || !event) {
      return QObject::eventFilter(watched, event);
    }

    pruneRoutes();
    QWidget* widget = qobject_cast<QWidget*>(watched);
    if (event->type() == QEvent::MouseMove && widget) {
      const auto* mouseEvent = static_cast<QMouseEvent*>(event);
      if (tooltip_ && tooltip_->isVisible() && widget == activeTarget_ && !activeRect_.isNull() &&
          !activeRect_.contains(mouseEvent->position().toPoint())) {
        // QTipLabel tests the receiver of the mouse event, rather than the
        // widget currently found below the cursor.
        scheduleHide();
      }
      if (mouseEvent->buttons() == Qt::NoButton &&
          (!widget->window() ||
           widget->window()->objectName() != QStringLiteral("adtooltip-surface"))) {
        QWidget* hoverWidget = QApplication::widgetAt(mouseEvent->globalPosition().toPoint());
        trackHoverCandidate(hoverWidget ? hoverWidget : widget, mouseEvent);
      }
    }
    if (event->type() == QEvent::ToolTip && widget) {
      if (triggerRouteFor(widget)) {
        QToolTip::hideText();
        hideImmediately();
        event->ignore();
        return true;
      }
      QObject* tooltipManager = widget->property(kTooltipManagerProperty).value<QObject*>();
      if (tooltipManager && tooltipManager != tooltip_) {
        hideImmediately();
        event->accept();
        return true;
      }
      if (PopupTooltipRoute* route = popupRouteFor(widget)) {
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        if (handleTooltipEvent(widget, helpEvent, route)) {
          event->accept();
        } else {
          event->ignore();
        }
        return true;
      }
      if (!applicationTooltipsEnabled_) {
        return QObject::eventFilter(watched, event);
      }
      auto* helpEvent = static_cast<QHelpEvent*>(event);
      if (handleTooltipEvent(widget, helpEvent, nullptr)) {
        event->accept();
      } else {
        event->ignore();
      }
      return true;
    }

    if ((!tooltip_ || !tooltip_->isVisible()) && !pendingRequest_.target && !hoverTarget_) {
      return QObject::eventFilter(watched, event);
    }

    switch (event->type()) {
#ifdef Q_OS_MACOS
      case QEvent::KeyPress:
      case QEvent::KeyRelease: {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key < Qt::Key_Shift || key > Qt::Key_ScrollLock) {
          hideImmediately();
        }
        break;
      }
#endif
      case QEvent::Leave:
        // QApplication stops its single tooltip wake-up timer for every
        // leave event. QTipLabel separately starts its 300 ms hide timer for
        // an already visible tooltip.
        clearPendingTooltip();
        clearHoverCandidate();
        scheduleHide();
        break;
      case QEvent::Enter:
        // Neither QApplication's tooltip wake-up timer nor QTipLabel's
        // delayed hide timer changes on enter.
      case QEvent::MouseMove:
        break;
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonRelease:
      case QEvent::MouseButtonDblClick:
      case QEvent::Wheel:
      case QEvent::WindowActivate:
      case QEvent::WindowDeactivate:
      case QEvent::FocusIn:
      case QEvent::FocusOut:
        hideImmediately();
        break;
      default:
        break;
    }

    return QObject::eventFilter(watched, event);
  }

 private:
  struct ResolvedTooltipContent {
    QString text;
    QRect activeRect;
    int configuredDuration = -1;
  };

  void createTooltip() {
    tooltip_ = new AdTooltip(this);
    tooltip_->setActivationMode(AdTooltip::ActivationMode::Manual);
    tooltip_->setLayerMode(AdTooltip::LayerMode::TopLevelTransient);
    tooltip_->setPopupLifetime(AdTooltip::PopupLifetime::RecreateOnOpen);
    tooltip_->setPlacement(AdTooltip::Placement::Bottom);
    tooltip_->setArrowPointAtCenter(true);
  }

  static ResolvedTooltipContent resolveTooltipContent(QWidget* widget,
                                                      const QPoint& localPosition) {
    ResolvedTooltipContent content;
    if (!widget) {
      return content;
    }

    content.configuredDuration = widget->toolTipDuration();
    bool itemTooltipHandled = false;
    auto* itemView = qobject_cast<QAbstractItemView*>(widget->parentWidget());
    if (itemView && itemView->viewport() == widget) {
      const QModelIndex index = itemView->indexAt(localPosition);
      if (index.isValid()) {
        const QVariant itemTooltip = index.data(Qt::ToolTipRole);
        if (itemTooltip.canConvert<QString>() && !itemTooltip.toString().isEmpty()) {
          itemTooltipHandled = true;
          content.configuredDuration = -1;
          content.text = itemTooltip.toString();
          const QRect itemRect = itemView->visualRect(index);
          if (itemRect.isValid()) {
            content.activeRect = itemRect;
          }
        }
      }
    }
    if (!itemTooltipHandled) {
      content.text = widget->toolTip();
    }
    return content;
  }

  void clearHoverCandidate() {
    hoverTarget_.clear();
    hoverText_.clear();
    hoverRect_ = QRect();
    hoverElapsed_.invalidate();
    ++hoverGeneration_;
  }

  void abandonHoverCandidate() {
    clearPendingTooltip();
    clearHoverCandidate();
  }

  void trackHoverCandidate(QWidget* widget, const QMouseEvent* event) {
    if (!widget || !event) {
      return;
    }

    if (triggerRouteFor(widget)) {
      abandonHoverCandidate();
      return;
    }
    QObject* tooltipManager = widget->property(kTooltipManagerProperty).value<QObject*>();
    if (tooltipManager && tooltipManager != tooltip_) {
      abandonHoverCandidate();
      return;
    }
    PopupTooltipRoute* route = popupRouteFor(widget);
    if (!route && !applicationTooltipsEnabled_) {
      abandonHoverCandidate();
      return;
    }

    const QPoint localPosition = widget->mapFromGlobal(event->globalPosition().toPoint());
    const ResolvedTooltipContent content = resolveTooltipContent(widget, localPosition);
    if (content.text.isEmpty()) {
      abandonHoverCandidate();
      return;
    }

    if (!tooltip_ || !tooltip_->isVisible()) {
      clearPendingTooltip();
    }
    // QApplication restarts toolTipWakeUp on every eligible mouse move,
    // including moves that remain in the same tooltip region.
    hoverTarget_ = widget;
    hoverText_ = content.text;
    hoverRect_ = content.activeRect;
    ++hoverGeneration_;
    hoverElapsed_.restart();
  }

  void pruneRoutes() {
    routes_.erase(std::remove_if(routes_.begin(), routes_.end(),
                                 [](const PopupTooltipRoute& route) {
                                   return route.owner.isNull() || route.triggerRoot.isNull() ||
                                          route.popupSurface.isNull();
                                 }),
                  routes_.end());
    const bool activeRouteInvalid =
        activeUsesPopupRoute_ && !routeStillActive(activeRouteOwner_, activePopup_, activeTarget_);
    const bool pendingRouteInvalid =
        pendingRequest_.usesPopupRoute &&
        !routeStillActive(pendingRequest_.routeOwner, pendingRequest_.popup,
                          pendingRequest_.target);
    if ((tooltip_ && tooltip_->isVisible() && (activeTarget_.isNull() || activeRouteInvalid)) ||
        (wakeTimer_.isActive() && (pendingRequest_.target.isNull() ||
                                   pendingRequest_.anchor.isNull() || pendingRouteInvalid))) {
      hideImmediately();
    }
  }

  bool routeStillActive(const QObject* owner, const QWidget* popup, const QWidget* target) const {
    if (!owner || !popup || !target || !popup->isVisible()) {
      return false;
    }
    return std::any_of(routes_.cbegin(), routes_.cend(),
                       [owner, popup, target](const PopupTooltipRoute& route) {
                         return route.owner == owner && route.popupSurface == popup &&
                                widgetInTree(target, route.popupSurface);
                       });
  }

  PopupTooltipRoute* popupRouteFor(QWidget* widget) {
    PopupTooltipRoute* best = nullptr;
    for (PopupTooltipRoute& route : routes_) {
      if (!route.popupSurface || !route.popupSurface->isVisible() ||
          !widgetInTree(widget, route.popupSurface)) {
        continue;
      }
      if (!best || route.activationOrder > best->activationOrder) {
        best = &route;
      }
    }
    return best;
  }

  PopupTooltipRoute* triggerRouteFor(QWidget* widget) {
    PopupTooltipRoute* best = nullptr;
    for (PopupTooltipRoute& route : routes_) {
      if (!route.popupSurface || !route.popupSurface->isVisible() ||
          !widgetInTree(widget, route.triggerRoot) ||
          route.triggerRoot->property(kPopupTriggerTooltipEnabledProperty).toBool()) {
        continue;
      }
      if (!best || route.activationOrder > best->activationOrder) {
        best = &route;
      }
    }
    return best;
  }

  static bool requestMatches(QWidget* widget, const QString& text, const QPoint& localPosition,
                             const QWidget* requestTarget, const QString& requestText,
                             const QRect& requestRect) {
    if (requestTarget != widget || requestText != text) {
      return false;
    }
    return requestRect.isNull() || requestRect.contains(localPosition);
  }

  static PendingTooltipRequest makeTooltipRequest(QWidget* widget, const QString& text,
                                                  const QRect& activeRect, int displayTimeMs,
                                                  quint64 hoverGeneration,
                                                  const PopupTooltipRoute* route) {
    PendingTooltipRequest request;
    request.routeOwner = route ? route->owner : nullptr;
    request.popup = route ? route->popupSurface : nullptr;
    request.target = widget;
    request.anchor = route ? route->popupSurface.data() : widget;
    request.text = text;
    request.activeRect = activeRect;
    request.displayTimeMs = displayTimeMs;
    request.hoverGeneration = hoverGeneration;
    request.usesPopupRoute = route != nullptr;

    QWidget* anchorWidget = request.anchor.data();
    const QRect targetRect = activeRect.isValid() ? activeRect : widget->rect();
    if (anchorWidget && targetRect.isValid()) {
      const QPoint globalTopLeft = widget->mapToGlobal(targetRect.topLeft());
      request.anchorRect = QRect(anchorWidget->mapFromGlobal(globalTopLeft), targetRect.size());
    }
    return request;
  }

  bool handleTooltipEvent(QWidget* widget, QHelpEvent* event, const PopupTooltipRoute* route) {
    if (!widget || !event) {
      return false;
    }

    const QPoint localPosition = widget->mapFromGlobal(event->globalPos());
    const ResolvedTooltipContent content = resolveTooltipContent(widget, localPosition);
    const QString& text = content.text;
    const QRect& activeRect = content.activeRect;

    QToolTip::hideText();
    if (text.isEmpty()) {
      clearPendingTooltip();
      scheduleHide();
      return false;
    }

    // QToolTip reuses a visible QTipLabel.  Equal requests retain the
    // existing position and expiry; changed text, owner, or active rect is
    // presented immediately and restarts only the expiry timer.
    if (tooltip_ && tooltip_->isVisible()) {
      if (requestMatches(widget, text, localPosition, activeTarget_, activeText_, activeRect_)) {
        return true;
      }

      const PendingTooltipRequest request = makeTooltipRequest(
          widget, text, activeRect, tooltipDisplayTime(content.configuredDuration, text), 0, route);
      if (!request.anchor || !request.anchorRect.isValid()) {
        return false;
      }
      clearPendingTooltip();
      showTooltip(request);
      return tooltip_ && tooltip_->isVisible();
    }

    const bool hasHoverCandidate = hoverTarget_ && hoverElapsed_.isValid();
    bool matchesHoverCandidate =
        hasHoverCandidate &&
        requestMatches(widget, text, localPosition, hoverTarget_, hoverText_, hoverRect_);
    if (hasHoverCandidate && !matchesHoverCandidate) {
      const bool sameHoverRegion =
          hoverTarget_ == widget && (hoverRect_.isNull() || hoverRect_.contains(localPosition));
      if (!sameHoverRegion) {
        return false;
      }
      hoverText_ = text;
      hoverRect_ = activeRect;
      ++hoverGeneration_;
      hoverElapsed_.restart();
      matchesHoverCandidate = true;
    }

    const quint64 requestHoverGeneration = matchesHoverCandidate ? hoverGeneration_ : 0;
    if (wakeTimer_.isActive() &&
        requestMatches(widget, text, localPosition, pendingRequest_.target, pendingRequest_.text,
                       pendingRequest_.activeRect) &&
        pendingRequest_.hoverGeneration == requestHoverGeneration) {
      return true;
    }

    const PendingTooltipRequest request = makeTooltipRequest(
        widget, text, activeRect, tooltipDisplayTime(content.configuredDuration, text),
        requestHoverGeneration, route);
    if (!request.anchor || !request.anchorRect.isValid()) {
      return false;
    }

    const bool replacingPendingTooltip = wakeTimer_.isActive();
    int remainingWakeDelay = 0;
    if (matchesHoverCandidate) {
      remainingWakeDelay =
          std::max(0, tooltipWakeUpDelay(widget) - static_cast<int>(hoverElapsed_.elapsed()));
    } else if (replacingPendingTooltip) {
      remainingWakeDelay = tooltipWakeUpDelay(widget);
    }

    if (remainingWakeDelay > 0) {
      if (replacingPendingTooltip) {
        hideImmediately(!matchesHoverCandidate);
      }
      pendingRequest_ = request;
      wakeTimer_.start(remainingWakeDelay);
      return true;
    }

    if (replacingPendingTooltip) {
      hideImmediately(!matchesHoverCandidate);
    }
    pendingRequest_ = request;
    showPendingTooltip();
    return true;
  }

  void showPendingTooltip() {
    wakeTimer_.stop();
    const PendingTooltipRequest request = pendingRequest_;
    pendingRequest_ = PendingTooltipRequest{};
    if (!request.target || !request.anchor || request.text.isEmpty() ||
        !request.target->isVisible() || !request.anchor->isVisible()) {
      return;
    }
    if (request.hoverGeneration != 0) {
      const bool hoverRequestStillCurrent =
          request.hoverGeneration == hoverGeneration_ && hoverTarget_ == request.target &&
          hoverText_ == request.text && hoverRect_ == request.activeRect && hoverElapsed_.isValid();
      if (!hoverRequestStillCurrent) {
        return;
      }
      const int remainingWakeDelay = std::max(
          0, tooltipWakeUpDelay(request.target) - static_cast<int>(hoverElapsed_.elapsed()));
      if (remainingWakeDelay > 0) {
        pendingRequest_ = request;
        wakeTimer_.start(remainingWakeDelay);
        return;
      }
    }
    if (request.usesPopupRoute &&
        !routeStillActive(request.routeOwner, request.popup, request.target)) {
      return;
    }
    QObject* tooltipManager = request.target->property(kTooltipManagerProperty).value<QObject*>();
    if (tooltipManager && tooltipManager != tooltip_) {
      return;
    }

    showTooltip(request);
  }

  void showTooltip(const PendingTooltipRequest& request) {
    if (!tooltip_ || !request.target || !request.anchor || request.text.isEmpty()) {
      return;
    }

    QWidget* anchor = request.anchor;
    QRect anchorRect = request.anchorRect;
    AdTooltip::Placement placement = AdTooltip::Placement::Bottom;
    for (const PopupTooltipRoute& route : routes_) {
      if (!route.popupSurface || !route.popupSurface->isVisible() ||
          !widgetInTree(request.target, route.triggerRoot) ||
          !route.triggerRoot->property(kPopupTriggerTooltipEnabledProperty).toBool()) {
        continue;
      }
      // Anchor outside the visible popup, excluding its shadow padding and
      // including screen-edge flips. Keep horizontal alignment on the trigger.
      const QPoint triggerCenter = request.target->mapToGlobal(request.target->rect().center());
      anchor = route.popupSurface;
      anchorRect = anchor->rect();
      if (const auto* surface = dynamic_cast<const OverlayPopupSurface*>(anchor)) {
        anchorRect = anchorRect.marginsRemoved(surface->shadowMargins());
      }
      const int centerX = anchor->mapFromGlobal(triggerCenter).x();
      anchorRect.setLeft(centerX);
      anchorRect.setRight(centerX);
      placement = anchor->mapToGlobal(anchorRect.center()).y() < triggerCenter.y()
                      ? AdTooltip::Placement::Top
                      : AdTooltip::Placement::Bottom;
      break;
    }
    tooltip_->setTargetWidget(request.target);
    tooltip_->setAnchorWidget(anchor);
    tooltip_->setText(request.text);
    tooltip_->setPlacement(placement);
    tooltip_->setAnchorRect(anchorRect);
    activeUsesPopupRoute_ = request.usesPopupRoute;
    activeRouteOwner_ = request.routeOwner;
    activePopup_ = request.popup;
    activeTarget_ = request.target;
    activeText_ = request.text;
    activeRect_ = request.activeRect;
    tooltip_->show();
    if (!tooltip_->isVisible()) {
      activeRouteOwner_.clear();
      activePopup_.clear();
      activeTarget_.clear();
      activeUsesPopupRoute_ = false;
      activeText_.clear();
      activeRect_ = QRect();
      return;
    }
    expireTimer_.start(request.displayTimeMs);
    hideTimer_.stop();
  }

  void clearPendingTooltip() {
    wakeTimer_.stop();
    pendingRequest_ = PendingTooltipRequest{};
  }

  void scheduleHide() {
    if (tooltip_ && tooltip_->isVisible() && !hideTimer_.isActive()) {
      hideTimer_.start(kLeaveHideDelayMs);
    }
  }

  void hideImmediately(bool clearHover = true) {
    clearPendingTooltip();
    expireTimer_.stop();
    hideTimer_.stop();
    if (tooltip_) {
      tooltip_->hide();
      tooltip_->clearAnchorRect();
      tooltip_->setAnchorWidget(nullptr);
      tooltip_->setTargetWidget(nullptr);
      tooltip_->setText(QString());
    }
    activeRouteOwner_.clear();
    activePopup_.clear();
    activeTarget_.clear();
    activeUsesPopupRoute_ = false;
    activeText_.clear();
    activeRect_ = QRect();
    if (clearHover) {
      clearHoverCandidate();
    }
  }

  QVector<PopupTooltipRoute> routes_;
  QPointer<AdTooltip> tooltip_;
  QPointer<QObject> activeRouteOwner_;
  QPointer<QWidget> activePopup_;
  QPointer<QWidget> activeTarget_;
  QString activeText_;
  QRect activeRect_;
  QTimer expireTimer_;
  QTimer hideTimer_;
  QTimer wakeTimer_;
  PendingTooltipRequest pendingRequest_;
  QPointer<QWidget> hoverTarget_;
  QString hoverText_;
  QRect hoverRect_;
  QElapsedTimer hoverElapsed_;
  quint64 hoverGeneration_ = 0;
  bool applicationTooltipsEnabled_ = false;
  bool activeUsesPopupRoute_ = false;
  quint64 activationCounter_ = 0;
};

QtTooltipBridge* tooltipBridge(bool create) {
  static QPointer<QtTooltipBridge> bridge;
  if (!bridge && create && qApp) {
    bridge = new QtTooltipBridge(qApp);
  }
  return bridge;
}

}  // namespace

void installQtTooltipBridge() {
  if (QtTooltipBridge* bridge = tooltipBridge(true)) {
    bridge->enableApplicationTooltips();
  }
  QToolTip::hideText();
}

void showQtTooltip(QWidget* target, const QString& text, int displayTimeMs) {
  if (QtTooltipBridge* bridge = tooltipBridge(true)) {
    bridge->showText(target, text, displayTimeMs);
  }
}

void syncTopLevelPopupTooltipRoute(QObject* owner, QWidget* triggerRoot, QWidget* popupSurface,
                                   bool active) {
  if (QtTooltipBridge* bridge = tooltipBridge(active)) {
    bridge->syncRoute(owner, triggerRoot, popupSurface, active);
  }
}

}  // namespace adqt::widgets::detail
