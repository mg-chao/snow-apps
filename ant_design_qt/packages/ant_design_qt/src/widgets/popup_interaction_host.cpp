#include "popup_interaction_host.h"

#include "detail/popup_geometry.h"
#include "detail/timing_hub.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QString>
#include <QTouchEvent>
#include <QWidget>

#include <algorithm>
#include <optional>

namespace adqt::widgets::detail {

namespace {

constexpr char kRelayoutTaskKey[] = "PopupInteractionHost.Relayout";
constexpr char kRefreshAnchorWatchersTaskKey[] = "PopupInteractionHost.RefreshAnchorWatchers";
constexpr char kFrameRelayoutTaskKey[] = "PopupInteractionHost.FrameRelayout";
constexpr char kScopeDeactivateTaskKey[] = "PopupInteractionHost.ScopeDeactivate";

QPoint mouseEventGlobalPos(const QMouseEvent* event) {
  if (!event) {
    return QPoint();
  }
  return event->globalPosition().toPoint();
}

std::optional<QPoint> touchEventGlobalPos(const QTouchEvent* event) {
  if (!event) {
    return std::nullopt;
  }
  const QList<QEventPoint>& points = event->points();
  if (points.isEmpty()) {
    return std::nullopt;
  }
  return points.constFirst().globalPosition().toPoint();
}

std::optional<QPoint> popupInteractionGlobalPos(const QEvent* event) {
  if (!event) {
    return std::nullopt;
  }
  switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
      return mouseEventGlobalPos(static_cast<const QMouseEvent*>(event));
    case QEvent::TouchBegin:
      return touchEventGlobalPos(static_cast<const QTouchEvent*>(event));
    default:
      return std::nullopt;
  }
}

bool isAnchorGeometryEvent(QEvent::Type type) {
  switch (type) {
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::LayoutRequest:
    case QEvent::Wheel:
    case QEvent::ContentsRectChange:
    case QEvent::ScrollPrepare:
    case QEvent::Scroll:
    case QEvent::StyleChange:
    case QEvent::PolishRequest:
      return true;
    default:
      return false;
  }
}

bool isScrollBarActivityEvent(QEvent::Type type) {
  switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::LayoutRequest:
    case QEvent::StyleChange:
    case QEvent::Paint:
      return true;
    default:
      return false;
  }
}

class PopupInteractionHost final : public QObject {
 public:
  explicit PopupInteractionHost(QWidget* scopeWindow)
      : QObject(scopeWindow), scopeWindow_(scopeWindow) {}

  bool owns(const PopupInteractionOwner* owner) const {
    if (activeOwner_ == owner) {
      return true;
    }
    return std::any_of(
        suspendedOwners_.cbegin(), suspendedOwners_.cend(),
        [owner](const SuspendedOwnerState& suspended) { return suspended.owner == owner; });
  }

  void activateOwner(PopupInteractionOwner* owner) {
    if (!owner || !scopeWindow_) {
      return;
    }

    QObject* ownerObject = owner->popupOwnerObject();
    QWidget* anchorWidget = owner->popupAnchorWidget();
    if (!ownerObject || !anchorWidget) {
      return;
    }

    if (activeOwner_ == owner) {
      refreshAnchorChainWatchers();
      refreshFrameRelayoutSubscription();
      if (owner->popupIsVisible()) {
        scheduleRelayout();
      }
      restackOwnerChain();
      return;
    }

    removeSuspendedOwner(owner);

    if (activeOwner_ && activeOwner_ != owner) {
      if (anchorInsideActiveOwnerPopup(anchorWidget)) {
        suspendActiveOwner();
      } else {
        closeOwnerChain(PopupCloseReason::SupersededByAnotherOwner);
      }
    }

    adoptActiveOwner(owner, ownerObject, anchorWidget);
    restackOwnerChain();
  }

  void deactivateOwner(PopupInteractionOwner* owner) {
    if (!owner) {
      return;
    }
    if (activeOwner_ == owner) {
      removeAllWatchers();
      clearActiveOwner();
      restoreSuspendedOwner();
      return;
    }
    removeSuspendedOwner(owner);
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!watched || !event || !activeOwner_) {
      return QObject::eventFilter(watched, event);
    }

    const bool watchedScope = (scopeWindow_ && watched == scopeWindow_.data());
    const bool watchedAnchorChain = watchedInAnchorChain(watched);

    if (auto* scrollBar = qobject_cast<QScrollBar*>(watched)) {
      if (scopeWindow_ &&
          (scrollBar == scopeWindow_.data() || scopeWindow_->isAncestorOf(scrollBar)) &&
          isScrollBarActivityEvent(event->type())) {
        scheduleRelayout();
      }
      return QObject::eventFilter(watched, event);
    }

    // Application-level event filters receive the concrete target QObject (not qApp itself),
    // so outside-click close must not depend on watched == qApp.
    const bool outsideCloseInteractionEvent = event->type() == QEvent::MouseButtonPress ||
                                              event->type() == QEvent::MouseButtonDblClick ||
                                              event->type() == QEvent::TouchBegin;
    if (outsideCloseInteractionEvent && activeOwner_->popupIsVisible()) {
      const std::optional<QPoint> interactionGlobalPos = popupInteractionGlobalPos(event);
      if (!interactionGlobalPos.has_value()) {
        return QObject::eventFilter(watched, event);
      }
      if (widgetContainsGlobalPos(scopeWindow_, interactionGlobalPos.value())) {
        requestCloseOwnersOutsidePoint(interactionGlobalPos.value(),
                                       PopupCloseReason::OutsidePressInScope);
      }
      return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::KeyPress && activeOwner_->popupIsVisible()) {
      const auto* keyEvent = static_cast<const QKeyEvent*>(event);
      if (keyEvent && keyEvent->key() == Qt::Key_Escape) {
        requestCloseActive(PopupCloseReason::EscapeKeyPress);
      }
      return QObject::eventFilter(watched, event);
    }

    if (event->type() == QEvent::ApplicationDeactivate && activeOwner_->popupIsVisible()) {
      requestCloseActive(PopupCloseReason::ScopeDeactivated);
      return QObject::eventFilter(watched, event);
    }

    if (watchedScope) {
      switch (event->type()) {
        case QEvent::Hide:
          requestCloseActive(PopupCloseReason::ScopeHidden);
          break;
        case QEvent::WindowDeactivate:
          scheduleScopeDeactivateClose();
          break;
        default:
          if (isAnchorGeometryEvent(event->type())) {
            scheduleRelayout();
          }
          break;
      }
      return QObject::eventFilter(watched, event);
    }

    if (watchedAnchorChain) {
      if (event->type() == QEvent::Hide) {
        requestCloseActive(PopupCloseReason::OwnerHidden);
      } else if (event->type() == QEvent::Destroy) {
        requestCloseActive(PopupCloseReason::OwnerDestroyed);
      } else if (event->type() == QEvent::ParentChange ||
                 event->type() == QEvent::ParentAboutToChange) {
        scheduleRelayout();
        detail::deferTimingTask(this, QString::fromLatin1(kRefreshAnchorWatchersTaskKey), [this]() {
          refreshAnchorChainWatchers();
          scheduleRelayout();
        });
      } else if (isAnchorGeometryEvent(event->type())) {
        scheduleRelayout();
      }
      return QObject::eventFilter(watched, event);
    }

    return QObject::eventFilter(watched, event);
  }

 private:
  struct ScrollBarWatch {
    QMetaObject::Connection valueChanged;
    QMetaObject::Connection destroyed;
  };

  struct SuspendedOwnerState {
    PopupInteractionOwner* owner = nullptr;
    QPointer<QObject> ownerObject;
    QPointer<QWidget> anchorWidget;
  };

  bool watchedInAnchorChain(QObject* watched) const {
    return std::any_of(
        watchedAnchorChain_.cbegin(), watchedAnchorChain_.cend(),
        [watched](const QPointer<QWidget>& widget) { return widget && widget.data() == watched; });
  }

  void scheduleRelayout() {
    if (relayoutQueued_) {
      return;
    }
    relayoutQueued_ = true;
    detail::deferTimingTask(this, QString::fromLatin1(kRelayoutTaskKey), [this]() {
      relayoutQueued_ = false;
      if (!activeOwner_) {
        return;
      }
      refreshAnchorChainWatchers();
      QWidget* anchor = activeOwner_->popupAnchorWidget();
      QWidget* scope = activeOwner_->popupScopeWindow();
      if (!anchor || !scope || !anchor->isVisible() || !scope->isVisible()) {
        requestCloseActive(PopupCloseReason::OwnerHidden);
        return;
      }
      activeOwner_->popupRelayoutFromHost();
      restackOwnerChain();
    });
  }

  void scheduleScopeDeactivateClose() {
    detail::deferTimingTask(this, QString::fromLatin1(kScopeDeactivateTaskKey), [this]() {
      if (!activeOwner_ || !activeOwner_->popupIsVisible()) {
        return;
      }

      QWidget* surface = activeOwner_->popupSurfaceWidget();
      if (surface && surface->isWindow() &&
          surface->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)) {
        return;
      }
      QWidget* activeWindow = QApplication::activeWindow();
      QWidget* focusedWidget = QApplication::focusWidget();
      if (activeWindow == scopeWindow_) {
        return;
      }
      const bool toolIsActive = surface && surface->isWindow() &&
                                (activeWindow == surface || surface->isAncestorOf(activeWindow) ||
                                 focusedWidget == surface || surface->isAncestorOf(focusedWidget));
      if (toolIsActive) {
        return;
      }

      requestCloseActive(PopupCloseReason::ScopeDeactivated);
    });
  }

  void requestCloseActive(PopupCloseReason reason) {
    if (!activeOwner_ || closingActive_) {
      return;
    }
    closingActive_ = true;
    PopupInteractionOwner* owner = activeOwner_;
    owner->popupCloseFromHost(reason);
    closingActive_ = false;
    if (activeOwner_ == owner && !owner->popupIsVisible()) {
      deactivateOwner(owner);
    }
  }

  bool anchorInsideActiveOwnerPopup(const QWidget* candidateAnchor) const {
    if (!activeOwner_ || !candidateAnchor || !activeOwner_->popupIsVisible()) {
      return false;
    }
    const auto anchorRect = PopupWidgetRect::whole(candidateAnchor).visible();
    if (!anchorRect.rect.isValid()) {
      return false;
    }
    return activeOwner_->popupContainsGlobalPos(
        candidateAnchor->mapToGlobal(anchorRect.rect.center()));
  }

  void suspendActiveOwner() {
    if (!activeOwner_) {
      return;
    }

    SuspendedOwnerState state;
    state.owner = activeOwner_;
    state.ownerObject = activeOwnerObject_;
    state.anchorWidget = activeAnchorWidget_;
    suspendedOwners_.append(state);

    removeAllWatchers();
    clearActiveOwner();
  }

  void adoptActiveOwner(PopupInteractionOwner* owner, QObject* ownerObject, QWidget* anchorWidget) {
    if (!owner || !ownerObject || !anchorWidget) {
      return;
    }

    activeOwner_ = owner;
    activeOwnerObject_ = ownerObject;
    activeAnchorWidget_ = anchorWidget;

    ownerDestroyedConnection_ = QObject::connect(ownerObject, &QObject::destroyed, this, [this]() {
      removeAllWatchers();
      clearActiveOwner();
      restoreSuspendedOwner();
    });

    if (qApp) {
      qApp->removeEventFilter(this);
      qApp->installEventFilter(this);
    }
    if (scopeWindow_) {
      scopeWindow_->removeEventFilter(this);
      scopeWindow_->installEventFilter(this);
    }
    refreshFrameRelayoutSubscription();

    refreshAnchorChainWatchers();
    if (owner->popupIsVisible()) {
      scheduleRelayout();
    }
  }

  void requestCloseOwnersOutsidePoint(const QPoint& globalPos, PopupCloseReason reason) {
    int guard = 0;
    while (activeOwner_ && activeOwner_->popupIsVisible() &&
           !activeOwner_->popupContainsGlobalPos(globalPos) && guard < 8) {
      ++guard;
      PopupInteractionOwner* previousOwner = activeOwner_;
      requestCloseActive(reason);
      if (activeOwner_ == previousOwner) {
        break;
      }
    }
  }

  void closeSuspendedOwners(PopupCloseReason reason) {
    const QVector<SuspendedOwnerState> ownersToClose = suspendedOwners_;
    suspendedOwners_.clear();

    for (const SuspendedOwnerState& state : ownersToClose) {
      if (!state.owner || !state.ownerObject) {
        continue;
      }
      if (!state.owner->popupIsVisible()) {
        continue;
      }
      state.owner->popupCloseFromHost(reason);
    }
  }

  void closeOwnerChain(PopupCloseReason reason) {
    int guard = 0;
    while (activeOwner_ && guard < 8) {
      ++guard;
      PopupInteractionOwner* previousOwner = activeOwner_;
      requestCloseActive(reason);
      if (activeOwner_ != previousOwner) {
        continue;
      }

      if (previousOwner->popupIsVisible()) {
        previousOwner->popupCloseFromHost(reason);
      }
      if (activeOwner_ == previousOwner && !previousOwner->popupIsVisible()) {
        deactivateOwner(previousOwner);
      }
      if (activeOwner_ == previousOwner) {
        removeAllWatchers();
        clearActiveOwner();
      }
    }
    closeSuspendedOwners(reason);
  }

  void removeSuspendedOwner(PopupInteractionOwner* owner) {
    if (!owner || suspendedOwners_.isEmpty()) {
      return;
    }

    for (auto it = suspendedOwners_.begin(); it != suspendedOwners_.end();) {
      if (it->owner == owner || !it->owner || !it->ownerObject || !it->anchorWidget) {
        it = suspendedOwners_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void restoreSuspendedOwner() {
    while (!suspendedOwners_.isEmpty()) {
      const SuspendedOwnerState state = suspendedOwners_.takeLast();
      if (!state.owner || !state.ownerObject || !state.anchorWidget) {
        continue;
      }
      if (!state.owner->popupIsVisible()) {
        continue;
      }
      adoptActiveOwner(state.owner, state.ownerObject.data(), state.anchorWidget.data());
      restackOwnerChain();
      return;
    }
  }

  void restackOwnerChain() {
    for (const SuspendedOwnerState& state : suspendedOwners_) {
      raiseOwnerSurface(state.owner);
    }
    raiseOwnerSurface(activeOwner_);
  }

  void raiseOwnerSurface(PopupInteractionOwner* owner) {
    if (!owner) {
      return;
    }
    QWidget* surface = owner->popupSurfaceWidget();
    if (!surface || !surface->isVisible()) {
      return;
    }
    surface->raise();
  }

  void removeAllWatchers() {
    for (const QPointer<QWidget>& widget : watchedAnchorChain_) {
      if (widget) {
        widget->removeEventFilter(this);
      }
    }
    watchedAnchorChain_.clear();
    clearScrollBarWatchers();

    if (scopeWindow_) {
      scopeWindow_->removeEventFilter(this);
    }
    if (qApp) {
      qApp->removeEventFilter(this);
    }
    detail::clearFrameSubscription(this, QString::fromLatin1(kFrameRelayoutTaskKey));
  }

  void refreshFrameRelayoutSubscription() {
    if (!activeOwner_ || !activeOwner_->popupWantsHostFrameRelayout()) {
      detail::clearFrameSubscription(this, QString::fromLatin1(kFrameRelayoutTaskKey));
      return;
    }

    detail::setFrameSubscription(this, QString::fromLatin1(kFrameRelayoutTaskKey), true,
                                 [this](qint64, qint64) {
                                   if (!activeOwner_) {
                                     return;
                                   }
                                   scheduleRelayout();
                                 });
  }

  void refreshAnchorChainWatchers() {
    if (!activeOwner_) {
      return;
    }

    QWidget* anchor = activeOwner_->popupAnchorWidget();
    QWidget* scope = scopeWindow_;
    if (!anchor || !scope) {
      return;
    }

    QVector<QWidget*> nextChain;
    QWidget* cursor = anchor;
    while (cursor && cursor != scope) {
      nextChain.append(cursor);
      cursor = cursor->parentWidget();
    }

    QSet<QWidget*> nextSet;
    nextSet.reserve(nextChain.size());
    for (QWidget* widget : nextChain) {
      nextSet.insert(widget);
    }

    for (const QPointer<QWidget>& widget : watchedAnchorChain_) {
      if (widget && !nextSet.contains(widget.data())) {
        widget->removeEventFilter(this);
      }
    }
    watchedAnchorChain_.clear();
    for (QWidget* widget : nextChain) {
      if (!widget) {
        continue;
      }
      widget->removeEventFilter(this);
      widget->installEventFilter(this);
      watchedAnchorChain_.append(widget);
    }

    refreshScrollBarWatchers(nextChain);
  }

  void refreshScrollBarWatchers(const QVector<QWidget*>& anchorChain) {
    QSet<QScrollBar*> nextScrollBars;
    for (QWidget* widget : anchorChain) {
      auto* scrollArea = qobject_cast<QAbstractScrollArea*>(widget);
      if (!scrollArea) {
        continue;
      }
      if (QScrollBar* verticalBar = scrollArea->verticalScrollBar()) {
        nextScrollBars.insert(verticalBar);
      }
      if (QScrollBar* horizontalBar = scrollArea->horizontalScrollBar()) {
        nextScrollBars.insert(horizontalBar);
      }
    }

    for (auto it = watchedScrollBars_.begin(); it != watchedScrollBars_.end();) {
      QScrollBar* bar = it.key();
      if (!bar || !nextScrollBars.contains(bar)) {
        QObject::disconnect(it.value().valueChanged);
        QObject::disconnect(it.value().destroyed);
        it = watchedScrollBars_.erase(it);
        continue;
      }
      ++it;
    }

    for (QScrollBar* bar : nextScrollBars) {
      if (!bar || watchedScrollBars_.contains(bar)) {
        continue;
      }

      ScrollBarWatch watch;
      watch.valueChanged = QObject::connect(bar, &QScrollBar::valueChanged, this,
                                            [this](int) { scheduleRelayout(); });
      watch.destroyed = QObject::connect(bar, &QObject::destroyed, this, [this, bar]() {
        auto it = watchedScrollBars_.find(bar);
        if (it == watchedScrollBars_.end()) {
          return;
        }
        QObject::disconnect(it.value().valueChanged);
        QObject::disconnect(it.value().destroyed);
        watchedScrollBars_.erase(it);
      });
      watchedScrollBars_.insert(bar, watch);
    }
  }

  void clearScrollBarWatchers() {
    for (auto it = watchedScrollBars_.begin(); it != watchedScrollBars_.end(); ++it) {
      QObject::disconnect(it.value().valueChanged);
      QObject::disconnect(it.value().destroyed);
    }
    watchedScrollBars_.clear();
  }

  void clearActiveOwner() {
    relayoutQueued_ = false;
    detail::cancelTimingTask(this, QString::fromLatin1(kScopeDeactivateTaskKey));
    if (ownerDestroyedConnection_) {
      QObject::disconnect(ownerDestroyedConnection_);
      ownerDestroyedConnection_ = QMetaObject::Connection();
    }
    activeOwner_ = nullptr;
    activeOwnerObject_.clear();
    activeAnchorWidget_.clear();
  }

  QPointer<QWidget> scopeWindow_;
  PopupInteractionOwner* activeOwner_ = nullptr;
  QPointer<QObject> activeOwnerObject_;
  QPointer<QWidget> activeAnchorWidget_;
  QVector<SuspendedOwnerState> suspendedOwners_;
  QVector<QPointer<QWidget>> watchedAnchorChain_;
  QHash<QScrollBar*, ScrollBarWatch> watchedScrollBars_;
  QMetaObject::Connection ownerDestroyedConnection_;
  bool relayoutQueued_ = false;
  bool closingActive_ = false;
};

QHash<QWidget*, PopupInteractionHost*>& popupHostMap() {
  static QHash<QWidget*, PopupInteractionHost*> hosts;
  return hosts;
}

PopupInteractionHost* hostForScope(QWidget* scopeWindow) {
  if (!scopeWindow) {
    return nullptr;
  }
  return popupHostMap().value(scopeWindow, nullptr);
}

PopupInteractionHost* ensureHostForScope(QWidget* scopeWindow) {
  if (!scopeWindow) {
    return nullptr;
  }
  auto& hosts = popupHostMap();
  if (auto it = hosts.find(scopeWindow); it != hosts.end() && it.value()) {
    return it.value();
  }

  auto* host = new PopupInteractionHost(scopeWindow);
  hosts.insert(scopeWindow, host);
  QObject::connect(scopeWindow, &QObject::destroyed, scopeWindow,
                   [scopeWindow]() { popupHostMap().remove(scopeWindow); });
  return host;
}

PopupInteractionHost* hostForOwner(const PopupInteractionOwner* owner) {
  if (!owner) {
    return nullptr;
  }
  const auto& hosts = popupHostMap();
  for (auto it = hosts.constBegin(); it != hosts.constEnd(); ++it) {
    PopupInteractionHost* host = it.value();
    if (host && host->owns(owner)) {
      return host;
    }
  }
  return nullptr;
}

}  // namespace

void setPopupInteractionHostOpen(PopupInteractionOwner* owner, bool open) {
  if (!owner) {
    return;
  }

  PopupInteractionHost* currentOwnerHost = hostForOwner(owner);
  if (open) {
    QWidget* scopeWindow = owner->popupScopeWindow();
    if (!scopeWindow) {
      return;
    }
    PopupInteractionHost* targetHost = ensureHostForScope(scopeWindow);
    if (!targetHost) {
      return;
    }
    if (currentOwnerHost && currentOwnerHost != targetHost) {
      currentOwnerHost->deactivateOwner(owner);
    }
    targetHost->activateOwner(owner);
    return;
  }

  if (currentOwnerHost) {
    currentOwnerHost->deactivateOwner(owner);
    return;
  }

  QWidget* scopeWindow = owner->popupScopeWindow();
  if (PopupInteractionHost* scopeHost = hostForScope(scopeWindow)) {
    scopeHost->deactivateOwner(owner);
  }
}

}  // namespace adqt::widgets::detail
