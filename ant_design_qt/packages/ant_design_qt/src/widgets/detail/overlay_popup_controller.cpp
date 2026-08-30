#include "overlay_popup_controller.h"

#include "overlay_popup_surface.h"
#include "qt_tooltip_bridge.h"
#include "timing_hub.h"
#include "top_level_popup_window.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <utility>

namespace adqt::widgets::detail {

namespace {

constexpr char kHoverTransitionTaskKey[] = "OverlayPopup.HoverTransition";
constexpr char kHoverMonitorTaskKey[] = "OverlayPopup.HoverMonitor";
constexpr int kHoverMonitorIntervalMs = 25;
constexpr char kFocusRecheckTaskKey[] = "OverlayPopup.FocusRecheck";
constexpr char kPopupRelayoutTaskKey[] = "OverlayPopup.Relayout";
constexpr char kScopePopupRelayoutTaskKey[] = "OverlayPopup.ScopeRelayout";
constexpr char kGeometryFrameSyncTaskKey[] = "OverlayPopup.GeometryFrameSync";
constexpr qint64 kGeometryFrameSyncTailMs = 140;
constexpr qint64 kAnchorScrollWatchersRefreshIntervalMs = 280;

std::atomic<qint64> gSyncPopupGeometryCallCount{0};
std::atomic<qint64> gSyncPopupGeometryShortCircuitCount{0};
std::atomic<bool> gSyncPopupGeometryCountersEnabled{false};

inline void recordSyncPopupGeometryCallForTesting() {
  if (!gSyncPopupGeometryCountersEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  gSyncPopupGeometryCallCount.fetch_add(1, std::memory_order_relaxed);
}

inline void recordSyncPopupGeometryShortCircuitForTesting() {
  if (!gSyncPopupGeometryCountersEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  gSyncPopupGeometryShortCircuitCount.fetch_add(1, std::memory_order_relaxed);
}

using PendingRelayoutList = QVector<QPointer<OverlayPopupController>>;

bool pendingRelayoutListContains(const PendingRelayoutList& controllers,
                                 const OverlayPopupController* target) {
  if (!target) {
    return false;
  }
  return std::any_of(controllers.cbegin(), controllers.cend(),
                     [target](const QPointer<OverlayPopupController>& controller) {
                       return controller.data() == target;
                     });
}

void pruneDeadPendingRelayouts(PendingRelayoutList* controllers) {
  if (!controllers) {
    return;
  }
  controllers->erase(std::remove_if(controllers->begin(), controllers->end(),
                                    [](const QPointer<OverlayPopupController>& controller) {
                                      return controller.isNull();
                                    }),
                     controllers->end());
}

QHash<QWidget*, PendingRelayoutList>& pendingScopeRelayouts() {
  static QHash<QWidget*, PendingRelayoutList> pending;
  return pending;
}

QHash<QWidget*, QMetaObject::Connection>& scopeRelayoutDestroyedConnections() {
  static QHash<QWidget*, QMetaObject::Connection> connections;
  return connections;
}

void clearScopeRelayoutDestroyedWatcherIfUnused(QWidget* scope) {
  if (!scope || pendingScopeRelayouts().contains(scope)) {
    return;
  }
  auto& connections = scopeRelayoutDestroyedConnections();
  auto it = connections.find(scope);
  if (it == connections.end()) {
    return;
  }
  QObject::disconnect(it.value());
  connections.erase(it);
}

void ensureScopeRelayoutDestroyedWatcher(QWidget* scope) {
  if (!scope) {
    return;
  }
  auto& connections = scopeRelayoutDestroyedConnections();
  if (connections.contains(scope)) {
    return;
  }
  connections.insert(scope, QObject::connect(scope, &QObject::destroyed, [](QObject* destroyed) {
                       QWidget* scopeWidget = qobject_cast<QWidget*>(destroyed);
                       if (!scopeWidget) {
                         return;
                       }
                       pendingScopeRelayouts().remove(scopeWidget);
                       auto& watchers = scopeRelayoutDestroyedConnections();
                       auto it = watchers.find(scopeWidget);
                       if (it == watchers.end()) {
                         return;
                       }
                       QObject::disconnect(it.value());
                       watchers.erase(it);
                     }));
}

void removeControllerFromPendingScopeRelayouts(OverlayPopupController* controller,
                                               QWidget* scopeHint = nullptr) {
  if (!controller) {
    return;
  }
  auto& pending = pendingScopeRelayouts();
  auto removeFromScope = [&](QWidget* scope) -> bool {
    auto it = pending.find(scope);
    if (it == pending.end()) {
      return false;
    }
    pruneDeadPendingRelayouts(&it.value());
    it.value().erase(std::remove_if(it.value().begin(), it.value().end(),
                                    [controller](const QPointer<OverlayPopupController>& queued) {
                                      return queued.data() == controller;
                                    }),
                     it.value().end());
    if (it.value().isEmpty()) {
      pending.erase(it);
      clearScopeRelayoutDestroyedWatcherIfUnused(scope);
    }
    return true;
  };
  if (scopeHint && removeFromScope(scopeHint)) {
    return;
  }
  for (auto it = pending.begin(); it != pending.end();) {
    pruneDeadPendingRelayouts(&it.value());
    it.value().erase(std::remove_if(it.value().begin(), it.value().end(),
                                    [controller](const QPointer<OverlayPopupController>& queued) {
                                      return queued.data() == controller;
                                    }),
                     it.value().end());
    if (it.value().isEmpty()) {
      QWidget* scope = it.key();
      it = pending.erase(it);
      clearScopeRelayoutDestroyedWatcherIfUnused(scope);
      continue;
    }
    ++it;
  }
}

QRect widgetGlobalRect(const QWidget* widget) {
  if (!widget) {
    return QRect();
  }
  return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
}

QRect widgetRectInAncestorSpace(const QWidget* widget, const QWidget* ancestor) {
  if (!widget || !ancestor) {
    return QRect();
  }
  if (widget != ancestor && !ancestor->isAncestorOf(const_cast<QWidget*>(widget))) {
    return QRect();
  }
  return QRect(widget->mapTo(const_cast<QWidget*>(ancestor), QPoint(0, 0)), widget->size());
}

QRect widgetGlobalRectIfEffectivelyVisible(const QWidget* widget, const QWidget* scopeWindow) {
  if (!widget || !widget->isVisible()) {
    return QRect();
  }

  QRect clippedRect = widgetGlobalRect(widget);
  if (!clippedRect.isValid()) {
    return QRect();
  }

  bool reachedScope = false;
  const QWidget* cursor = widget;
  while (cursor) {
    if (!cursor->isVisible()) {
      return QRect();
    }
    const QRect cursorRect = widgetGlobalRect(cursor);
    if (!cursorRect.isValid()) {
      return QRect();
    }
    clippedRect = clippedRect.intersected(cursorRect);
    if (!clippedRect.isValid()) {
      return QRect();
    }
    if (scopeWindow && cursor == scopeWindow) {
      reachedScope = true;
      break;
    }
    cursor = cursor->parentWidget();
  }

  if (scopeWindow && !reachedScope) {
    const QRect scopeRect = widgetGlobalRect(scopeWindow);
    if (!scopeRect.isValid()) {
      return QRect();
    }
    clippedRect = clippedRect.intersected(scopeRect);
    if (!clippedRect.isValid()) {
      return QRect();
    }
  }

  return clippedRect;
}

QRect widgetRectIfEffectivelyVisibleInAncestorSpace(const QWidget* widget,
                                                    const QWidget* ancestor) {
  if (!widget || !ancestor || !widget->isVisible()) {
    return QRect();
  }

  QRect clippedRect = widgetRectInAncestorSpace(widget, ancestor);
  if (!clippedRect.isValid()) {
    return QRect();
  }

  bool reachedAncestor = false;
  const QWidget* cursor = widget;
  while (cursor) {
    if (!cursor->isVisible()) {
      return QRect();
    }
    const QRect cursorRect = widgetRectInAncestorSpace(cursor, ancestor);
    if (!cursorRect.isValid()) {
      return QRect();
    }
    clippedRect = clippedRect.intersected(cursorRect);
    if (!clippedRect.isValid()) {
      return QRect();
    }
    if (cursor == ancestor) {
      reachedAncestor = true;
      break;
    }
    cursor = cursor->parentWidget();
  }

  return reachedAncestor ? clippedRect : QRect();
}

QRect widgetVisibleRectInAncestorMappedToGlobal(const QWidget* widget, const QWidget* ancestor) {
  if (!widget) {
    return QRect();
  }
  if (!ancestor) {
    return widgetGlobalRectIfEffectivelyVisible(widget, nullptr);
  }
  const QRect visibleLocalRect = widgetRectIfEffectivelyVisibleInAncestorSpace(widget, ancestor);
  if (!visibleLocalRect.isValid()) {
    return QRect();
  }
  return QRect(ancestor->mapToGlobal(visibleLocalRect.topLeft()), visibleLocalRect.size());
}

bool widgetContainsGlobalPos(const QWidget* widget, const QPoint& globalPos) {
  const QRect rect = widgetGlobalRect(widget);
  return rect.isValid() && rect.contains(globalPos);
}

bool popupInteractiveContainsGlobalPos(const QWidget* popup, const QPoint& globalPos) {
  if (!popup || !popup->isVisible()) {
    return false;
  }
  if (const auto* surface = dynamic_cast<const OverlayPopupSurface*>(popup)) {
    return surface->containsInteractiveGlobalPos(globalPos);
  }
  return widgetContainsGlobalPos(popup, globalPos);
}

bool widgetInTree(const QWidget* candidate, const QWidget* root) {
  if (!candidate || !root) {
    return false;
  }
  return candidate == root || root->isAncestorOf(const_cast<QWidget*>(candidate));
}

void applyPopupVisibility(QWidget* popup, bool shouldShow, bool raiseWhenShowing) {
  if (!popup) {
    return;
  }

  if (!shouldShow) {
    if (popup->isVisible()) {
      popup->hide();
    }
    return;
  }

  if (!popup->isVisible()) {
    popup->show();
  }
  if (raiseWhenShowing) {
    popup->raise();
  }
}

bool watchedObjectListContains(const OverlayPopupController::WatchedObjectList& objects,
                               const QWidget* target) {
  if (!target) {
    return false;
  }
  return std::any_of(objects.cbegin(), objects.cend(),
                     [target](const QPointer<QWidget>& object) { return object.data() == target; });
}

void pruneDeadWatchedObjects(OverlayPopupController::WatchedObjectList* objects) {
  if (!objects) {
    return;
  }

  objects->erase(std::remove_if(objects->begin(), objects->end(),
                                [](const QPointer<QWidget>& object) { return object.isNull(); }),
                 objects->end());
}

void installPopupWatcher(OverlayPopupController::WatchedObjectList* objects, QObject* filter,
                         QObject* object) {
  auto* widget = qobject_cast<QWidget*>(object);
  if (!objects || !filter || !widget) {
    return;
  }

  pruneDeadWatchedObjects(objects);
  if (watchedObjectListContains(*objects, widget)) {
    return;
  }

  widget->installEventFilter(filter);
  objects->append(widget);
}

template <typename Callback>
void traverseObjectTree(QObject* root, Callback&& callback) {
  if (!root) {
    return;
  }

  callback(root);
  const QObjectList children = root->children();
  for (QObject* child : children) {
    traverseObjectTree(child, callback);
  }
}

void preparePopupForGeometrySync(QWidget* popup) {
  if (!popup) {
    return;
  }

  QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);

  traverseObjectTree(popup, [](QObject* object) {
    auto* widget = qobject_cast<QWidget*>(object);
    if (!widget) {
      return;
    }
    widget->ensurePolished();
    QCoreApplication::sendPostedEvents(widget, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(widget, QEvent::LayoutRequest);
    if (QLayout* layout = widget->layout()) {
      layout->activate();
    }
    widget->updateGeometry();
  });

  QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
  popup->adjustSize();
}

QMargins popupShadowMarginsForGeometry(const QWidget* popup) {
  const auto* surface = dynamic_cast<const OverlayPopupSurface*>(popup);
  return surface ? surface->shadowMargins() : QMargins();
}

QSize popupVisualSizeHintForGeometry(const QWidget* popup) {
  const auto* surface = dynamic_cast<const OverlayPopupSurface*>(popup);
  if (surface) {
    return surface->visualSizeHint();
  }
  return popup ? popup->sizeHint() : QSize(1, 1);
}

QSize popupFrameSizeForVisualSize(const QSize& visualSize, const QMargins& margins) {
  return QSize(std::max(1, visualSize.width() + margins.left() + margins.right()),
               std::max(1, visualSize.height() + margins.top() + margins.bottom()));
}

QPoint popupFrameTopLeftForVisualTopLeft(const QPoint& visualTopLeft, const QMargins& margins) {
  return QPoint(visualTopLeft.x() - margins.left(), visualTopLeft.y() - margins.top());
}

}  // namespace

OverlayPopupController::OverlayPopupController(OverlayPopupControllerDelegate* delegate,
                                               QObject* parent,
                                               CursorPositionProvider cursorPositionProvider)
    : QObject(parent),
      delegate_(delegate),
      cursorPositionProvider_(std::move(cursorPositionProvider)) {
  if (!cursorPositionProvider_) {
    cursorPositionProvider_ = []() { return QCursor::pos(); };
  }
}

OverlayPopupController::~OverlayPopupController() {
  syncTopLevelPopupTooltipRoute(this, nullptr, nullptr, false);
  resetHoverInteraction();
  cancelPopupRelayout();
  clearFrameSubscription(this, QString::fromLatin1(kGeometryFrameSyncTaskKey));
  clearAnchorScrollBarWatchers();
  delegate_ = nullptr;
  setPopupInteractionHostOpen(this, false);
  clearTriggerWatchers();
  clearPopupWatchers();
}

void OverlayPopupController::resetSyncPopupGeometryCountersForTesting() {
  gSyncPopupGeometryCountersEnabled.store(true, std::memory_order_relaxed);
  gSyncPopupGeometryCallCount.store(0);
  gSyncPopupGeometryShortCircuitCount.store(0);
}

qint64 OverlayPopupController::syncPopupGeometryCallCountForTesting() {
  return gSyncPopupGeometryCallCount.load();
}

qint64 OverlayPopupController::syncPopupGeometryShortCircuitCountForTesting() {
  return gSyncPopupGeometryShortCircuitCount.load();
}

void OverlayPopupController::setTriggerModes(Triggers value) {
  if (triggerModes_ == value) {
    return;
  }
  triggerModes_ = value;

  if (!hasTrigger(Trigger::Hover)) {
    setReasonOpen(InternalOpenReason::Hover, false);
    resetHoverInteraction();
  }
  if (!hasTrigger(Trigger::Focus)) {
    setReasonOpen(InternalOpenReason::Focus, false);
    focusTriggerActive_ = false;
    focusPopupActive_ = false;
  }
  if (!hasTrigger(Trigger::Click)) {
    setReasonOpen(InternalOpenReason::Click, false);
    triggerPressActive_ = false;
    triggerKeyPressActive_ = false;
  }
  if (!hasTrigger(Trigger::ContextMenu)) {
    setReasonOpen(InternalOpenReason::ContextMenu, false);
    contextMenuGlobalPos_.reset();
  }

  updatePopupVisibility(true, VisibilityUpdateSource::InternalState);
}

void OverlayPopupController::setVisibilityMode(VisibilityMode value) {
  if (visibilityMode_ == value) {
    return;
  }

  visibilityMode_ = value;
  resetHoverInteraction();
  if (visibilityMode_ == VisibilityMode::External) {
    clearAllOpenReasons();
    return;
  }

  clearAllOpenReasons();
  if (popupVisible_) {
    setReasonOpen(InternalOpenReason::Programmatic, true);
  }
}

void OverlayPopupController::setPopupVisible(bool value) {
  if (visibilityMode_ == VisibilityMode::External) {
    clearAllOpenReasons();
    if (!value) {
      resetHoverInteraction();
    }
    setPopupVisibleInternal(value, true);
    if (value) {
      reconcileHoverFromCursor();
    }
    return;
  }

  if (value) {
    setReasonOpen(InternalOpenReason::Programmatic, true);
  } else {
    resetHoverInteraction();
    clearAllOpenReasons();
  }
  updatePopupVisibility(true, VisibilityUpdateSource::InternalState);
  if (value) {
    reconcileHoverFromCursor();
  }
}

void OverlayPopupController::setDisabled(bool value) {
  if (disabled_ == value) {
    return;
  }
  disabled_ = value;
  if (disabled_) {
    resetHoverInteraction();
    clearAllOpenReasons();
  }
  updatePopupVisibility(true, VisibilityUpdateSource::InternalState);
}

void OverlayPopupController::setMouseEnterDelayMs(int value) {
  mouseEnterDelayMs_ = std::max(0, value);
}

void OverlayPopupController::setMouseLeaveDelayMs(int value) {
  mouseLeaveDelayMs_ = std::max(0, value);
}

void OverlayPopupController::anchorWidgetChanged() {
  clearTriggerWatchers();
  markAnchorScrollWatchersDirty();
  refreshTriggerWatchers();
  if (popupVisible_) {
    schedulePopupRelayout(true);
  }
  syncPopupTooltipRoute();
}

void OverlayPopupController::popupSurfaceChanged() {
  refreshPopupWatchers();
  invalidatePopupGeometry();
  setPopupInteractionHostOpen(this, popupVisible_);
  if (!delegate_ || !delegate_->popupSurfaceWidget()) {
    focusPopupActive_ = false;
  }
  if (popupVisible_) {
    schedulePopupRelayout(true);
  }
  syncPopupTooltipRoute();
}

void OverlayPopupController::popupContentChanged(bool emitSignal) {
  invalidatePopupGeometry();
  updatePopupVisibility(emitSignal, VisibilityUpdateSource::InternalState);
}

void OverlayPopupController::refreshVisiblePopup() {
  invalidatePopupGeometry();
  if (!popupVisible_ || !delegate_) {
    return;
  }
  noteGeometryActivity();
  delegate_->popupEnsureSurface();
  delegate_->popupPrepareToShow();
  syncPreparedPopupVisibility();
}

void OverlayPopupController::invalidatePopupGeometry() { resetGeometrySyncSnapshot(); }
bool OverlayPopupController::eventFilter(QObject* watched, QEvent* event) {
  if (!watched || !event) {
    return QObject::eventFilter(watched, event);
  }

  const QEvent::Type eventType = event->type();
  if (watchedByTrigger(watched)) {
    const bool watchedIsAnchor = watched == popupAnchorWidget();
    switch (eventType) {
      case QEvent::Enter:
      case QEvent::HoverEnter:
        handleTriggerHoverEnter();
        break;
      case QEvent::Leave:
      case QEvent::HoverLeave:
        handleTriggerHoverLeave();
        break;
      case QEvent::MouseMove:
      case QEvent::HoverMove:
        reconcileHoverFromCursor();
        break;
      case QEvent::FocusIn:
        focusTriggerActive_ = true;
        if (hasTrigger(Trigger::Focus)) {
          setReasonOpen(InternalOpenReason::Focus, true);
          updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
        }
        break;
      case QEvent::FocusOut:
        handleTriggerFocusOutDeferred();
        break;
      case QEvent::MouseButtonPress:
        handleTriggerPress(watched, event);
        break;
      case QEvent::MouseButtonRelease:
        handleTriggerRelease(watched, event);
        break;
      case QEvent::KeyPress:
        handleTriggerKeyPress(event);
        break;
      case QEvent::KeyRelease:
        handleTriggerKeyRelease(event);
        break;
      case QEvent::ContextMenu:
        handleTriggerContextMenu(event);
        break;
      case QEvent::Move:
      case QEvent::Resize:
      case QEvent::Show:
        if (popupVisible_ && watchedIsAnchor) {
          if (QApplication::mouseButtons() == Qt::NoButton) {
            schedulePopupRelayout(true);
          }
        }
        break;
      case QEvent::ParentChange:
      case QEvent::ParentAboutToChange:
        if (watchedIsAnchor) {
          markAnchorScrollWatchersDirty();
        }
        if (popupVisible_ && watchedIsAnchor) {
          if (QApplication::mouseButtons() == Qt::NoButton) {
            schedulePopupRelayout(true);
          }
        }
        break;
      case QEvent::Hide:
        if (watchedIsAnchor) {
          triggerPressActive_ = false;
          triggerKeyPressActive_ = false;
          if (popupVisible_) {
            clearAllOpenReasons();
            updatePopupVisibility(true, VisibilityUpdateSource::InternalState);
          }
        }
        break;
      case QEvent::ChildAdded: {
        auto* childEvent = static_cast<QChildEvent*>(event);
        if (childEvent->child()) {
          traverseObjectTree(childEvent->child(), [this](QObject* object) {
            installPopupWatcher(&watchedTriggerObjects_, this, object);
          });
        }
        break;
      }
      default:
        break;
    }
  } else if (watchedByPopup(watched)) {
    if (handlePopupShadowPointerEvent(event)) {
      return true;
    }
    switch (eventType) {
      case QEvent::LayoutRequest:
        if (popupVisible_ && watched == (delegate_ ? delegate_->popupSurfaceWidget() : nullptr)) {
          auto* popupWidget = qobject_cast<QWidget*>(watched);
          if (popupWidget && geometrySyncSnapshotValid_ &&
              popupWidget->parentWidget() == geometrySyncParent_) {
            QSize requestedSize = popupWidget->sizeHint();
            requestedSize.setWidth(std::max(1, requestedSize.width()));
            requestedSize.setHeight(std::max(1, requestedSize.height()));
            if (requestedSize == geometrySyncPopupSize_ &&
                popupWidget->size() == geometrySyncPopupSize_) {
              break;
            }
          }
          if (!shouldSkipQueuedRelayoutSync()) {
            schedulePopupRelayout(false);
          }
        }
        break;
      case QEvent::Enter:
      case QEvent::HoverEnter:
        handlePopupHoverEnter();
        break;
      case QEvent::Leave:
      case QEvent::HoverLeave:
        handlePopupHoverLeave();
        break;
      case QEvent::MouseMove:
      case QEvent::HoverMove:
        reconcileHoverFromCursor();
        break;
      case QEvent::FocusIn:
        focusPopupActive_ = true;
        if (hasTrigger(Trigger::Focus)) {
          setReasonOpen(InternalOpenReason::Focus, true);
          updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
        }
        break;
      case QEvent::FocusOut:
        handleTriggerFocusOutDeferred();
        break;
      case QEvent::KeyPress: {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape && popupVisible_) {
          clearAllOpenReasons();
          updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
          keyEvent->accept();
        }
        break;
      }
      case QEvent::Hide:
        if (popupVisible_ && delegate_ && watched == delegate_->popupSurfaceWidget()) {
          clearAllOpenReasons();
          setPopupVisibleInternal(false, true);
        }
        break;
      case QEvent::ChildAdded: {
        auto* childEvent = static_cast<QChildEvent*>(event);
        if (childEvent->child()) {
          traverseObjectTree(childEvent->child(), [this](QObject* object) {
            installPopupWatcher(&watchedPopupObjects_, this, object);
          });
        }
        break;
      }
      default:
        break;
    }
  }

  return QObject::eventFilter(watched, event);
}

void OverlayPopupController::setReasonOpen(InternalOpenReason reason, bool enabled) {
  switch (reason) {
    case InternalOpenReason::Hover:
      openByHover_ = enabled;
      break;
    case InternalOpenReason::Focus:
      openByFocus_ = enabled;
      break;
    case InternalOpenReason::Click:
      openByClick_ = enabled;
      break;
    case InternalOpenReason::ContextMenu:
      openByContextMenu_ = enabled;
      if (!enabled) {
        contextMenuGlobalPos_.reset();
      }
      break;
    case InternalOpenReason::Programmatic:
      openByProgrammatic_ = enabled;
      break;
  }
}

bool OverlayPopupController::reasonOpen(InternalOpenReason reason) const {
  switch (reason) {
    case InternalOpenReason::Hover:
      return openByHover_;
    case InternalOpenReason::Focus:
      return openByFocus_;
    case InternalOpenReason::Click:
      return openByClick_;
    case InternalOpenReason::ContextMenu:
      return openByContextMenu_;
    case InternalOpenReason::Programmatic:
      return openByProgrammatic_;
  }
  return false;
}

void OverlayPopupController::clearAllOpenReasons() {
  openByHover_ = false;
  openByFocus_ = false;
  openByClick_ = false;
  openByContextMenu_ = false;
  openByProgrammatic_ = false;
  triggerPressActive_ = false;
  triggerKeyPressActive_ = false;
  contextMenuGlobalPos_.reset();
}

bool OverlayPopupController::hasTrigger(Trigger trigger) const {
  return triggerModes_.testFlag(trigger);
}

bool OverlayPopupController::shouldBeOpen() const {
  if (!delegate_ || disabled_ || !delegate_->popupHasContent()) {
    return false;
  }
  return openByHover_ || openByFocus_ || openByClick_ || openByContextMenu_ || openByProgrammatic_;
}

QPoint OverlayPopupController::cursorGlobalPos() const {
  return cursorPositionProvider_ ? cursorPositionProvider_() : QCursor::pos();
}

bool OverlayPopupController::triggerContainsGlobalPos(const QPoint& globalPos) const {
  if (!delegate_) {
    return false;
  }

  if (delegate_->popupTriggerGlobalRect().has_value()) {
    return delegate_->popupTriggerGlobalRect().value().contains(globalPos);
  }

  QWidget* trigger = popupTriggerWidget();
  if (!trigger) {
    return false;
  }
  const QRect triggerRect = widgetGlobalRectIfEffectivelyVisible(trigger, popupScopeWindow());
  return triggerRect.isValid() && triggerRect.contains(globalPos);
}

bool OverlayPopupController::hoverRegionContainsGlobalPos(const QPoint& globalPos) const {
  return triggerContainsGlobalPos(globalPos) ||
         popupInteractiveContainsGlobalPos(delegate_ ? delegate_->popupSurfaceWidget() : nullptr,
                                           globalPos);
}

void OverlayPopupController::reconcileHoverFromCursor() {
  if (!hasTrigger(Trigger::Hover) || disabled_ || !delegate_) {
    return;
  }

  const bool inside = hoverRegionContainsGlobalPos(cursorGlobalPos());
  if (hoverRegionInside_ == inside) {
    return;
  }

  hoverRegionInside_ = inside;
  if (inside) {
    hoverSessionActive_ = true;
    scheduleHoverOpen();
  } else {
    scheduleHoverClose();
  }
}

void OverlayPopupController::scheduleHoverOpen() {
  cancelTimingTask(this, QString::fromLatin1(kHoverTransitionTaskKey));
  hoverTransitionPending_ = true;
  refreshHoverMonitor();
  const int delay = std::max(0, mouseEnterDelayMs_);
  if (delay == 0) {
    finishHoverOpen();
    return;
  }

  scheduleTimingTask(this, QString::fromLatin1(kHoverTransitionTaskKey), delay,
                     [this]() { finishHoverOpen(); });
}

void OverlayPopupController::scheduleHoverClose() {
  cancelTimingTask(this, QString::fromLatin1(kHoverTransitionTaskKey));
  hoverTransitionPending_ = true;
  refreshHoverMonitor();
  const int delay = std::max(0, mouseLeaveDelayMs_);
  if (delay == 0) {
    finishHoverClose();
    return;
  }

  scheduleTimingTask(this, QString::fromLatin1(kHoverTransitionTaskKey), delay,
                     [this]() { finishHoverClose(); });
}

void OverlayPopupController::finishHoverOpen() {
  hoverTransitionPending_ = false;
  if (!hasTrigger(Trigger::Hover) || disabled_ || !delegate_) {
    resetHoverInteraction();
    return;
  }

  const bool inside = hoverRegionContainsGlobalPos(cursorGlobalPos());
  hoverRegionInside_ = inside;
  if (!inside) {
    scheduleHoverClose();
    return;
  }

  hoverSessionActive_ = true;
  setReasonOpen(InternalOpenReason::Hover, true);
  updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
  refreshHoverMonitor();
}

void OverlayPopupController::finishHoverClose() {
  hoverTransitionPending_ = false;
  if (!hasTrigger(Trigger::Hover) || disabled_ || !delegate_) {
    resetHoverInteraction();
    return;
  }

  const bool inside = hoverRegionContainsGlobalPos(cursorGlobalPos());
  hoverRegionInside_ = inside;
  if (inside) {
    hoverSessionActive_ = true;
    scheduleHoverOpen();
    return;
  }

  const bool hadHoverSession = hoverSessionActive_ || reasonOpen(InternalOpenReason::Hover);
  hoverSessionActive_ = false;
  setReasonOpen(InternalOpenReason::Hover, false);
  if (hadHoverSession) {
    updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
  }
  refreshHoverMonitor();
}

void OverlayPopupController::resetHoverInteraction() {
  cancelTimingTask(this, QString::fromLatin1(kHoverTransitionTaskKey));
  hoverTransitionPending_ = false;
  hoverRegionInside_ = false;
  hoverSessionActive_ = false;
  setReasonOpen(InternalOpenReason::Hover, false);
  cancelTimingTask(this, QString::fromLatin1(kHoverMonitorTaskKey));
  hoverMonitorScheduled_ = false;
}

void OverlayPopupController::refreshHoverMonitor() {
  const bool shouldMonitor =
      hasTrigger(Trigger::Hover) && !disabled_ &&
      (hoverTransitionPending_ || hoverSessionActive_ || reasonOpen(InternalOpenReason::Hover));
  if (!shouldMonitor) {
    cancelTimingTask(this, QString::fromLatin1(kHoverMonitorTaskKey));
    hoverMonitorScheduled_ = false;
    return;
  }
  if (hoverMonitorScheduled_) {
    return;
  }

  hoverMonitorScheduled_ = true;
  scheduleTimingTask(this, QString::fromLatin1(kHoverMonitorTaskKey), kHoverMonitorIntervalMs,
                     [this]() {
                       hoverMonitorScheduled_ = false;
                       reconcileHoverFromCursor();
                       refreshHoverMonitor();
                     });
}

void OverlayPopupController::noteGeometryActivity() {
  if (!popupVisible_) {
    return;
  }
  const qint64 now = timingNowMs();
  const qint64 nextDeadline = now + kGeometryFrameSyncTailMs;
  if (nextDeadline <= geometryFrameSyncDeadlineMs_) {
    return;
  }
  geometryFrameSyncDeadlineMs_ = nextDeadline;
  if (!geometryFrameSyncSubscribed_) {
    refreshGeometryFrameSync();
  }
}

void OverlayPopupController::schedulePopupRelayout(bool extendFrameTail) {
  if (!popupVisible_) {
    return;
  }
  if (popupRelayoutQueued_) {
    return;
  }
  if (!extendFrameTail && shouldSkipQueuedRelayoutSync()) {
    return;
  }
  if (extendFrameTail) {
    noteGeometryActivity();
  }
  popupRelayoutQueued_ = true;
  QWidget* scope = popupScopeWindow();
  if (!scope) {
    popupRelayoutQueuedScope_.clear();
    deferTimingTask(this, QString::fromLatin1(kPopupRelayoutTaskKey), [this]() {
      popupRelayoutQueued_ = false;
      popupRelayoutFromHost();
    });
    return;
  }

  auto& pending = pendingScopeRelayouts();
  ensureScopeRelayoutDestroyedWatcher(scope);
  popupRelayoutQueuedScope_ = scope;
  PendingRelayoutList& queuedSet = pending[scope];
  pruneDeadPendingRelayouts(&queuedSet);
  const bool shouldScheduleScopeTask = queuedSet.isEmpty();
  if (!pendingRelayoutListContains(queuedSet, this)) {
    queuedSet.append(this);
  }
  if (!shouldScheduleScopeTask) {
    return;
  }
  deferTimingTask(scope, QString::fromLatin1(kScopePopupRelayoutTaskKey), [scope]() {
    auto& pendingMap = pendingScopeRelayouts();
    PendingRelayoutList queued = pendingMap.take(scope);
    pruneDeadPendingRelayouts(&queued);
    clearScopeRelayoutDestroyedWatcherIfUnused(scope);
    if (queued.isEmpty()) {
      return;
    }
    for (const QPointer<OverlayPopupController>& controller : queued) {
      if (!controller) {
        continue;
      }
      controller->popupRelayoutQueued_ = false;
      controller->popupRelayoutQueuedScope_.clear();
      if (!controller->popupVisible_) {
        continue;
      }
      controller->popupRelayoutFromHost();
    }
  });
}

void OverlayPopupController::cancelPopupRelayout() {
  popupRelayoutQueued_ = false;
  QWidget* queuedScope = popupRelayoutQueuedScope_.data();
  popupRelayoutQueuedScope_.clear();
  if (queuedScope) {
    removeControllerFromPendingScopeRelayouts(this, queuedScope);
  }
  cancelTimingTask(this, QString::fromLatin1(kPopupRelayoutTaskKey));
}

void OverlayPopupController::refreshGeometryFrameSync() {
  if (!popupVisible_ || timingNowMs() >= geometryFrameSyncDeadlineMs_) {
    geometryFrameSyncDeadlineMs_ = -1;
    geometryFrameSyncSubscribed_ = false;
    clearFrameSubscription(this, QString::fromLatin1(kGeometryFrameSyncTaskKey));
    return;
  }
  if (geometryFrameSyncSubscribed_) {
    return;
  }

  geometryFrameSyncSubscribed_ = true;
  setFrameSubscription(this, QString::fromLatin1(kGeometryFrameSyncTaskKey), true,
                       [this](qint64 nowMs, qint64) {
                         if (!popupVisible_) {
                           geometryFrameSyncDeadlineMs_ = -1;
                           refreshGeometryFrameSync();
                           return;
                         }
                         schedulePopupRelayout(false);
                         if (nowMs >= geometryFrameSyncDeadlineMs_) {
                           geometryFrameSyncDeadlineMs_ = -1;
                           refreshGeometryFrameSync();
                         }
                       });
}
bool OverlayPopupController::shouldSkipQueuedRelayoutSync() const {
  QWidget* popup = delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
  if (!popupVisible_ || !popup || !geometrySyncSnapshotValid_ || !delegate_) {
    return false;
  }
  if (popupUsesTopLevelToolLayer()) {
    return false;
  }

  QWidget* popupParent = popup->parentWidget();
  if (!popupParent || geometrySyncParent_ != popupParent) {
    return false;
  }

  QWidget* anchor = popupAnchorWidget();
  QWidget* scope = popupScopeWindow();
  const qint64 now = timingNowMs();
  if (anchorScrollWatchersDirty_ || watchedScrollAnchor_ != anchor ||
      watchedScrollScope_ != scope || now >= nextAnchorScrollWatchersRefreshMs_) {
    return false;
  }

  QRect anchorRect;
  if (contextMenuGlobalPos_.has_value() && reasonOpen(InternalOpenReason::ContextMenu)) {
    anchorRect = QRect(popupParent->mapFromGlobal(contextMenuGlobalPos_.value()), QSize(1, 1));
  } else if (delegate_->popupAnchorGlobalRect().has_value()) {
    anchorRect = delegate_->popupAnchorGlobalRect().value();
    anchorRect.moveTopLeft(popupParent->mapFromGlobal(anchorRect.topLeft()));
    const QRect visibleAnchorRect =
        widgetRectIfEffectivelyVisibleInAncestorSpace(popupAnchorWidget(), popupParent);
    if (!visibleAnchorRect.isValid()) {
      anchorRect = QRect();
    } else {
      anchorRect = anchorRect.intersected(visibleAnchorRect);
    }
  } else {
    anchorRect = widgetRectIfEffectivelyVisibleInAncestorSpace(anchor, popupParent);
  }
  if (!anchorRect.isValid()) {
    return false;
  }

  QSize popupSize = popupVisualSizeHintForGeometry(popup);
  popupSize.setWidth(std::max(1, popupSize.width()));
  popupSize.setHeight(std::max(1, popupSize.height()));

  OverlayPopupPlacementInput currentInput;
  currentInput.anchorRect = anchorRect;
  currentInput.popupSize = popupSize;
  currentInput.bounds = QRect(QPoint(0, 0), popupParent->size());
  currentInput.preferredPlacement = delegate_->popupPlacement();
  currentInput.popupOffset = std::max(0, delegate_->popupOffset());
  currentInput.allowFallback = delegate_->popupAutoAdjustOverflow();
  currentInput.pointAtCenter = delegate_->popupArrowPointAtCenter();
  currentInput.arrowOffsetHorizontal = delegate_->popupArrowOffsetHorizontal();
  currentInput.arrowOffsetVertical = delegate_->popupArrowOffsetVertical();

  OverlayPopupPlacementInput previousInput;
  previousInput.anchorRect = geometrySyncAnchorRect_;
  previousInput.popupSize = geometrySyncPopupSize_;
  previousInput.bounds = geometrySyncBounds_;
  previousInput.preferredPlacement = geometrySyncPlacement_;
  previousInput.popupOffset = geometrySyncPopupOffset_;
  previousInput.allowFallback = geometrySyncAutoAdjustOverflow_;
  previousInput.pointAtCenter = geometrySyncArrowPointAtCenter_;
  previousInput.arrowOffsetHorizontal = geometrySyncArrowOffsetHorizontal_;
  previousInput.arrowOffsetVertical = geometrySyncArrowOffsetVertical_;

  const OverlayPopupPlacementOutput currentPlacement = resolveOverlayPopupPlacement(currentInput);
  const OverlayPopupPlacementOutput previousPlacement = resolveOverlayPopupPlacement(previousInput);

  const bool placementUnchanged = currentPlacement.placement == previousPlacement.placement;
  const bool topLeftUnchanged = currentPlacement.topLeft == previousPlacement.topLeft;
  const bool arrowUnchanged = qFuzzyCompare(currentPlacement.arrowCenterCoord + 1.0,
                                            previousPlacement.arrowCenterCoord + 1.0);
  if (!placementUnchanged || !topLeftUnchanged || !arrowUnchanged) {
    return false;
  }

  const QPoint expectedPopupPos = currentPlacement.topLeft;
  if (popup->pos() != expectedPopupPos) {
    return false;
  }
  if (popup->size() != popupSize) {
    return false;
  }
  return true;
}

void OverlayPopupController::resetGeometrySyncSnapshot() {
  geometrySyncParent_.clear();
  geometrySyncAnchorRect_ = QRect();
  geometrySyncBounds_ = QRect();
  geometrySyncPopupSize_ = QSize();
  geometrySyncPlacement_ = OverlayPopupPlacement::Top;
  geometrySyncLayerMode_ = AdPopupLayerMode::InWindow;
  geometrySyncAutoAdjustOverflow_ = true;
  geometrySyncPopupOffset_ = 0;
  geometrySyncArrowPointAtCenter_ = false;
  geometrySyncArrowOffsetHorizontal_ = 0;
  geometrySyncArrowOffsetVertical_ = 0;
  geometrySyncSnapshotValid_ = false;
}

void OverlayPopupController::markAnchorScrollWatchersDirty() {
  anchorScrollWatchersDirty_ = true;
  nextAnchorScrollWatchersRefreshMs_ = 0;
}

void OverlayPopupController::refreshAnchorScrollBarWatchers() {
  if (!popupVisible_) {
    clearAnchorScrollBarWatchers();
    return;
  }

  QWidget* anchor = popupAnchorWidget();
  if (!anchor) {
    clearAnchorScrollBarWatchers();
    return;
  }
  QWidget* scope = popupScopeWindow();

  const qint64 now = timingNowMs();
  if (!anchorScrollWatchersDirty_ && watchedScrollAnchor_ == anchor &&
      watchedScrollScope_ == scope && now < nextAnchorScrollWatchersRefreshMs_) {
    return;
  }
  watchedScrollAnchor_ = anchor;
  watchedScrollScope_ = scope;

  QSet<QScrollBar*> nextScrollBars;
  QWidget* cursor = anchor;
  while (cursor) {
    if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(cursor)) {
      if (QScrollBar* verticalBar = scrollArea->verticalScrollBar()) {
        nextScrollBars.insert(verticalBar);
      }
      if (QScrollBar* horizontalBar = scrollArea->horizontalScrollBar()) {
        nextScrollBars.insert(horizontalBar);
      }
    }
    if (scope && cursor == scope) {
      break;
    }
    cursor = cursor->parentWidget();
  }

  for (auto it = watchedAnchorScrollBars_.begin(); it != watchedAnchorScrollBars_.end();) {
    QScrollBar* bar = it.key();
    if (!bar || !nextScrollBars.contains(bar)) {
      QObject::disconnect(it.value().valueChanged);
      QObject::disconnect(it.value().destroyed);
      it = watchedAnchorScrollBars_.erase(it);
      continue;
    }
    ++it;
  }

  for (QScrollBar* bar : nextScrollBars) {
    if (!bar || watchedAnchorScrollBars_.contains(bar)) {
      continue;
    }

    ScrollBarWatch watch;
    watch.valueChanged = QObject::connect(bar, &QScrollBar::valueChanged, this,
                                          [this](int) { schedulePopupRelayout(true); });
    watch.destroyed = QObject::connect(bar, &QObject::destroyed, this, [this, bar]() {
      auto it = watchedAnchorScrollBars_.find(bar);
      if (it == watchedAnchorScrollBars_.end()) {
        return;
      }
      QObject::disconnect(it.value().valueChanged);
      QObject::disconnect(it.value().destroyed);
      watchedAnchorScrollBars_.erase(it);
    });
    watchedAnchorScrollBars_.insert(bar, watch);
  }

  anchorScrollWatchersDirty_ = false;
  nextAnchorScrollWatchersRefreshMs_ = now + kAnchorScrollWatchersRefreshIntervalMs;
}

void OverlayPopupController::clearAnchorScrollBarWatchers() {
  for (auto it = watchedAnchorScrollBars_.begin(); it != watchedAnchorScrollBars_.end(); ++it) {
    QObject::disconnect(it.value().valueChanged);
    QObject::disconnect(it.value().destroyed);
  }
  watchedAnchorScrollBars_.clear();
  watchedScrollAnchor_.clear();
  watchedScrollScope_.clear();
  markAnchorScrollWatchersDirty();
}

void OverlayPopupController::emitVisibilityRequest(bool requestedVisible) {
  if (visibilityMode_ != VisibilityMode::External || !delegate_) {
    return;
  }
  if (requestedVisible && (disabled_ || !delegate_->popupHasContent())) {
    return;
  }
  if (requestedVisible == popupVisible_) {
    return;
  }
  emit popupVisibilityRequested(requestedVisible);
}

void OverlayPopupController::updatePopupVisibility(bool emitSignal, VisibilityUpdateSource source) {
  const bool shouldOpen = shouldBeOpen();
  if (visibilityMode_ == VisibilityMode::External) {
    if (!emitSignal || source != VisibilityUpdateSource::UserInteraction) {
      return;
    }
    emitVisibilityRequest(shouldOpen);
    return;
  }
  setPopupVisibleInternal(shouldOpen, emitSignal);
}

void OverlayPopupController::setPopupVisibleInternal(bool visible, bool emitSignal) {
  if (!delegate_) {
    return;
  }
  if (updatingPopupVisible_) {
    // QWidget show/hide events are synchronous and can request the opposite
    // state before the current surface transition has finished.
    if (pendingPopupVisible_.has_value() && pendingPopupVisible_.value() == visible) {
      pendingPopupVisibleEmitSignal_ = pendingPopupVisibleEmitSignal_ || emitSignal;
    } else {
      pendingPopupVisible_ = visible;
      pendingPopupVisibleEmitSignal_ = emitSignal;
    }
    return;
  }
  updatingPopupVisible_ = true;

  if (popupVisible_ == visible) {
    if (popupVisible_) {
      setPopupInteractionHostOpen(this, true);
      refreshAnchorScrollBarWatchers();
      noteGeometryActivity();
      delegate_->popupEnsureSurface();
      delegate_->popupPrepareToShow();
      syncPreparedPopupVisibility();
    }
    syncPopupTooltipRoute();
    finishPopupVisibilityUpdate();
    return;
  }

  popupVisible_ = visible;
  setPopupInteractionHostOpen(this, popupVisible_);
  refreshAnchorScrollBarWatchers();
  noteGeometryActivity();
  if (popupVisible_) {
    delegate_->popupEnsureSurface();
    delegate_->popupPrepareToShow();
    syncPreparedPopupVisibility();
  } else {
    cancelPopupRelayout();
    geometryFrameSyncDeadlineMs_ = -1;
    refreshGeometryFrameSync();
    resetGeometrySyncSnapshot();
    clearAnchorScrollBarWatchers();
    resetHoverInteraction();
    applyPopupVisibility(delegate_->popupSurfaceWidget(), false, false);
    if (delegate_->popupReleaseOnHide()) {
      delegate_->popupReleaseSurface();
    } else if (popupUsesTopLevelToolLayer()) {
      releaseTopLevelToolResourcesOnHide(delegate_->popupSurfaceWidget());
    }
  }

  if (emitSignal) {
    emit popupVisibleChanged(popupVisible_);
  }
  syncPopupTooltipRoute();
  finishPopupVisibilityUpdate();
}

void OverlayPopupController::finishPopupVisibilityUpdate() {
  updatingPopupVisible_ = false;
  if (!pendingPopupVisible_.has_value()) {
    return;
  }

  const bool pendingVisible = pendingPopupVisible_.value();
  const bool pendingEmitSignal = pendingPopupVisibleEmitSignal_;
  pendingPopupVisible_.reset();
  pendingPopupVisibleEmitSignal_ = false;
  setPopupVisibleInternal(pendingVisible, pendingEmitSignal);
}

void OverlayPopupController::syncPreparedPopupVisibility() {
  QWidget* popup = delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
  if (!delegate_ || !popupVisible_ || !popup) {
    return;
  }

  // popupPrepareToShow() has already polished and activated the popup's
  // content chain. Repeating a recursive polish/layout pass here makes every
  // open pay for the same tree twice.
  const bool canShowPopup = syncPopupGeometry(false);
  applyPopupVisibility(popup, canShowPopup, true);
  if (!canShowPopup || !popup->isVisible()) {
    return;
  }

  // Showing can change the size hint, but the tree was fully prepared by the first pass.
  const bool updatedCanShowPopup = syncPopupGeometry(false);
  applyPopupVisibility(popup, updatedCanShowPopup, true);
  if (updatedCanShowPopup && popup->isVisible()) {
    if (popupUsesInWindowLayer()) {
      popup->update();
    } else {
      popup->repaint();
    }
  }
}

bool OverlayPopupController::syncPopupGeometry(bool prepareLayout) {
  recordSyncPopupGeometryCallForTesting();
  QWidget* popup = delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
  if (!popupVisible_ || !popup || !delegate_) {
    return false;
  }

  if (prepareLayout) {
    preparePopupForGeometrySync(popup);
  }

  refreshAnchorScrollBarWatchers();

  const bool useTopLevelToolLayer = popupUsesTopLevelToolLayer();
  QWidget* popupParent = popup->parentWidget();
  QWidget* expectedPopupParent = useTopLevelToolLayer ? nullptr : popupScopeWindow();
  if (useTopLevelToolLayer && popupParent) {
    const bool wasVisible = popup->isVisible();
    popup->setParent(nullptr, popup->windowFlags());
    popupParent = nullptr;
    refreshPopupWatchers();
    setPopupInteractionHostOpen(this, true);
    applyPopupVisibility(popup, wasVisible, true);
  }
  if (!useTopLevelToolLayer && expectedPopupParent && popupParent != expectedPopupParent) {
    const bool wasVisible = popup->isVisible();
    popup->setParent(expectedPopupParent, popup->windowFlags());
    popupParent = expectedPopupParent;
    refreshPopupWatchers();
    setPopupInteractionHostOpen(this, true);
    applyPopupVisibility(popup, wasVisible, true);
  }
  if (!useTopLevelToolLayer && !popupParent) {
    applyPopupVisibility(popup, false, false);
    resetGeometrySyncSnapshot();
    return false;
  }
  QWidget* anchorVisibilityScope = popupScopeWindow();
  QWidget* geometrySnapshotParent = useTopLevelToolLayer ? anchorVisibilityScope : popupParent;

  QRect anchorRect;
  if (contextMenuGlobalPos_.has_value() && reasonOpen(InternalOpenReason::ContextMenu)) {
    const QPoint contextMenuPos = useTopLevelToolLayer
                                      ? contextMenuGlobalPos_.value()
                                      : popupParent->mapFromGlobal(contextMenuGlobalPos_.value());
    anchorRect = QRect(contextMenuPos, QSize(1, 1));
  } else if (delegate_->popupAnchorGlobalRect().has_value()) {
    anchorRect = delegate_->popupAnchorGlobalRect().value();
    if (!useTopLevelToolLayer) {
      anchorRect.moveTopLeft(popupParent->mapFromGlobal(anchorRect.topLeft()));
    }
    const QRect visibleAnchorRect =
        useTopLevelToolLayer
            ? widgetVisibleRectInAncestorMappedToGlobal(popupAnchorWidget(), anchorVisibilityScope)
            : widgetRectIfEffectivelyVisibleInAncestorSpace(popupAnchorWidget(), popupParent);
    if (!visibleAnchorRect.isValid()) {
      anchorRect = QRect();
    } else {
      anchorRect = anchorRect.intersected(visibleAnchorRect);
    }
  } else {
    anchorRect =
        useTopLevelToolLayer
            ? widgetVisibleRectInAncestorMappedToGlobal(popupAnchorWidget(), anchorVisibilityScope)
            : widgetRectIfEffectivelyVisibleInAncestorSpace(popupAnchorWidget(), popupParent);
  }
  if (!anchorRect.isValid()) {
    applyPopupVisibility(popup, false, false);
    resetGeometrySyncSnapshot();
    return false;
  }

  const QMargins popupShadowMargins = popupShadowMarginsForGeometry(popup);
  QSize popupSize = popupVisualSizeHintForGeometry(popup);
  popupSize.setWidth(std::max(1, popupSize.width()));
  popupSize.setHeight(std::max(1, popupSize.height()));
  const QSize popupFrameSize = popupFrameSizeForVisualSize(popupSize, popupShadowMargins);

  QScreen* toolScreen = nullptr;
  if (useTopLevelToolLayer) {
    const QWidget* screenOwner = popupAnchorWidget() ? popupAnchorWidget() : anchorVisibilityScope;
    toolScreen = popupScreenForGlobalRect(screenOwner, anchorRect);
    if (toolScreen && popup->isWindow() && popup->screen() != toolScreen) {
      popup->setScreen(toolScreen);
    }
    syncTopLevelToolTransientParent(popup, anchorVisibilityScope);
  }
  const QRect bounds = useTopLevelToolLayer ? (toolScreen ? toolScreen->availableGeometry()
                                                          : popupScreenBoundsInGlobal(anchorRect))
                                            : QRect(QPoint(0, 0), popupParent->size());
  const int popupOffset = std::max(0, delegate_->popupOffset());
  const int arrowOffsetHorizontal = delegate_->popupArrowOffsetHorizontal();
  const int arrowOffsetVertical = delegate_->popupArrowOffsetVertical();

  const bool inputsUnchanged =
      geometrySyncSnapshotValid_ && geometrySyncParent_ == geometrySnapshotParent &&
      geometrySyncAnchorRect_ == anchorRect && geometrySyncBounds_ == bounds &&
      geometrySyncPopupSize_ == popupSize &&
      geometrySyncPlacement_ == delegate_->popupPlacement() &&
      geometrySyncLayerMode_ == delegate_->popupLayerMode() &&
      geometrySyncAutoAdjustOverflow_ == delegate_->popupAutoAdjustOverflow() &&
      geometrySyncPopupOffset_ == popupOffset &&
      geometrySyncArrowPointAtCenter_ == delegate_->popupArrowPointAtCenter() &&
      geometrySyncArrowOffsetHorizontal_ == arrowOffsetHorizontal &&
      geometrySyncArrowOffsetVertical_ == arrowOffsetVertical;
  if (inputsUnchanged) {
    recordSyncPopupGeometryShortCircuitForTesting();
    return true;
  }

  if (popup->size() != popupFrameSize) {
    popup->resize(popupFrameSize);
  }

  OverlayPopupPlacementInput placementInput;
  placementInput.anchorRect = anchorRect;
  placementInput.popupSize = popupSize;
  placementInput.bounds = bounds;
  placementInput.preferredPlacement = delegate_->popupPlacement();
  placementInput.popupOffset = popupOffset;
  placementInput.allowFallback = delegate_->popupAutoAdjustOverflow();
  placementInput.pointAtCenter = delegate_->popupArrowPointAtCenter();
  placementInput.arrowOffsetHorizontal = arrowOffsetHorizontal;
  placementInput.arrowOffsetVertical = arrowOffsetVertical;
  const OverlayPopupPlacementOutput placementResult = resolveOverlayPopupPlacement(placementInput);

  const QPoint popupTopLeft =
      popupFrameTopLeftForVisualTopLeft(placementResult.topLeft, popupShadowMargins);
  if (popup->pos() != popupTopLeft) {
    popup->move(popupTopLeft);
  }
  delegate_->popupApplyResolvedPlacement(placementResult.placement,
                                         placementResult.arrowCenterCoord);

  geometrySyncParent_ = geometrySnapshotParent;
  geometrySyncAnchorRect_ = anchorRect;
  geometrySyncBounds_ = bounds;
  geometrySyncPopupSize_ = popupSize;
  geometrySyncPlacement_ = delegate_->popupPlacement();
  geometrySyncLayerMode_ = delegate_->popupLayerMode();
  geometrySyncAutoAdjustOverflow_ = delegate_->popupAutoAdjustOverflow();
  geometrySyncPopupOffset_ = popupOffset;
  geometrySyncArrowPointAtCenter_ = delegate_->popupArrowPointAtCenter();
  geometrySyncArrowOffsetHorizontal_ = arrowOffsetHorizontal;
  geometrySyncArrowOffsetVertical_ = arrowOffsetVertical;
  geometrySyncSnapshotValid_ = true;
  return true;
}

bool OverlayPopupController::popupUsesInWindowLayer() const {
  return delegate_ && delegate_->popupLayerMode() == AdPopupLayerMode::InWindow;
}

bool OverlayPopupController::popupUsesTopLevelToolLayer() const {
  return delegate_ && delegate_->popupLayerMode() == AdPopupLayerMode::QtTool;
}

void OverlayPopupController::syncPopupTooltipRoute() {
  QWidget* popup = delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
  const bool active = popupVisible_ && popupUsesTopLevelToolLayer() && popup && popup->isWindow();
  syncTopLevelPopupTooltipRoute(this, delegate_ ? delegate_->popupTriggerWidget() : nullptr, popup,
                                active);
}

void OverlayPopupController::refreshTriggerWatchers() {
  clearTriggerWatchers();
  QWidget* trigger = popupTriggerWidget();
  if (!trigger) {
    return;
  }

  watchedTriggerRoot_ = trigger;
  traverseObjectTree(trigger, [this](QObject* object) {
    installPopupWatcher(&watchedTriggerObjects_, this, object);
  });
}

void OverlayPopupController::clearTriggerWatchers() {
  const WatchedObjectList watchedObjects = watchedTriggerObjects_;
  watchedTriggerObjects_.clear();
  for (const QPointer<QWidget>& object : watchedObjects) {
    if (object) {
      object->removeEventFilter(this);
    }
  }
  watchedTriggerRoot_.clear();
}

void OverlayPopupController::refreshPopupWatchers() {
  clearPopupWatchers();
  QWidget* popup = delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
  if (!popup) {
    return;
  }
  traverseObjectTree(
      popup, [this](QObject* object) { installPopupWatcher(&watchedPopupObjects_, this, object); });
}

void OverlayPopupController::clearPopupWatchers() {
  const WatchedObjectList watchedObjects = watchedPopupObjects_;
  watchedPopupObjects_.clear();
  for (const QPointer<QWidget>& object : watchedObjects) {
    if (object) {
      object->removeEventFilter(this);
    }
  }
}

bool OverlayPopupController::watchedByTrigger(QObject* watched) const {
  auto* widget = qobject_cast<QWidget*>(watched);
  return widget && watchedObjectListContains(watchedTriggerObjects_, widget);
}

bool OverlayPopupController::watchedByPopup(QObject* watched) const {
  auto* widget = qobject_cast<QWidget*>(watched);
  return widget && watchedObjectListContains(watchedPopupObjects_, widget);
}

void OverlayPopupController::handleTriggerPress(QObject* watched, QEvent* event) {
  Q_UNUSED(watched)
  if (!event || disabled_ || !hasTrigger(Trigger::Click)) {
    return;
  }
  if (event->type() != QEvent::MouseButtonPress) {
    return;
  }

  auto* mouseEvent = static_cast<QMouseEvent*>(event);
  if (mouseEvent->button() != Qt::LeftButton || triggerPressActive_) {
    return;
  }
  if (!triggerContainsGlobalPos(mouseEvent->globalPosition().toPoint())) {
    return;
  }
  triggerPressActive_ = true;
}

void OverlayPopupController::handleTriggerRelease(QObject* watched, QEvent* event) {
  if (!event || event->type() != QEvent::MouseButtonRelease) {
    return;
  }
  auto* mouseEvent = static_cast<QMouseEvent*>(event);
  if (mouseEvent->button() != Qt::LeftButton || !triggerPressActive_) {
    return;
  }

  triggerPressActive_ = false;
  QWidget* releaseTarget = qobject_cast<QWidget*>(watched);
  const bool releaseOnTriggerTree = widgetInTree(releaseTarget, popupTriggerWidget());
  if (!releaseOnTriggerTree) {
    if (!triggerContainsGlobalPos(mouseEvent->globalPosition().toPoint())) {
      return;
    }
  } else if (!triggerContainsGlobalPos(mouseEvent->globalPosition().toPoint())) {
    return;
  }

  if (disabled_ || !hasTrigger(Trigger::Click)) {
    return;
  }
  if (visibilityMode_ == VisibilityMode::External) {
    emitVisibilityRequest(!popupVisible_);
    return;
  }

  setReasonOpen(InternalOpenReason::Click, !reasonOpen(InternalOpenReason::Click));
  if (!reasonOpen(InternalOpenReason::Click)) {
    contextMenuGlobalPos_.reset();
  }
  updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
}

void OverlayPopupController::handleTriggerKeyPress(QEvent* event) {
  if (!event || event->type() != QEvent::KeyPress) {
    return;
  }

  auto* keyEvent = static_cast<QKeyEvent*>(event);
  if (keyEvent->key() == Qt::Key_Escape && popupVisible_) {
    clearAllOpenReasons();
    updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
    keyEvent->accept();
    return;
  }
  if (disabled_ || !hasTrigger(Trigger::Click) || keyEvent->isAutoRepeat()) {
    return;
  }
  if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter ||
      keyEvent->key() == Qt::Key_Space) {
    triggerKeyPressActive_ = true;
  }
}

void OverlayPopupController::handleTriggerKeyRelease(QEvent* event) {
  if (!event || event->type() != QEvent::KeyRelease) {
    return;
  }

  auto* keyEvent = static_cast<QKeyEvent*>(event);
  if (keyEvent->isAutoRepeat()) {
    return;
  }
  const bool activationKey = keyEvent->key() == Qt::Key_Return ||
                             keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Space;
  if (!activationKey || !triggerKeyPressActive_) {
    return;
  }
  triggerKeyPressActive_ = false;

  if (disabled_ || !hasTrigger(Trigger::Click) ||
      !widgetInTree(QApplication::focusWidget(), popupTriggerWidget())) {
    return;
  }
  if (visibilityMode_ == VisibilityMode::External) {
    emitVisibilityRequest(!popupVisible_);
    keyEvent->accept();
    return;
  }

  setReasonOpen(InternalOpenReason::Click, !reasonOpen(InternalOpenReason::Click));
  if (!reasonOpen(InternalOpenReason::Click)) {
    contextMenuGlobalPos_.reset();
  }
  updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
  keyEvent->accept();
}

void OverlayPopupController::handleTriggerContextMenu(QEvent* event) {
  if (!event || disabled_ || !hasTrigger(Trigger::ContextMenu) ||
      event->type() != QEvent::ContextMenu) {
    return;
  }

  auto* contextEvent = static_cast<QContextMenuEvent*>(event);
  if (!triggerContainsGlobalPos(contextEvent->globalPos())) {
    return;
  }
  contextMenuGlobalPos_ = contextEvent->globalPos();
  if (visibilityMode_ == VisibilityMode::External) {
    emitVisibilityRequest(true);
    contextEvent->accept();
    return;
  }

  setReasonOpen(InternalOpenReason::ContextMenu, true);
  updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
  contextEvent->accept();
}

void OverlayPopupController::handleTriggerFocusOutDeferred() {
  deferTimingTask(this, QString::fromLatin1(kFocusRecheckTaskKey), [this]() {
    QWidget* focused = QApplication::focusWidget();
    focusTriggerActive_ = widgetInTree(focused, popupTriggerWidget());
    focusPopupActive_ =
        widgetInTree(focused, delegate_ ? delegate_->popupSurfaceWidget() : nullptr);
    if (!hasTrigger(Trigger::Focus)) {
      return;
    }
    if (visibilityMode_ == VisibilityMode::External) {
      emitVisibilityRequest(focusTriggerActive_ || focusPopupActive_);
      return;
    }
    setReasonOpen(InternalOpenReason::Focus, focusTriggerActive_ || focusPopupActive_);
    updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
  });
}

void OverlayPopupController::handleTriggerHoverEnter() { reconcileHoverFromCursor(); }

void OverlayPopupController::handleTriggerHoverLeave() { reconcileHoverFromCursor(); }

void OverlayPopupController::handlePopupHoverEnter() { reconcileHoverFromCursor(); }

void OverlayPopupController::handlePopupHoverLeave() { reconcileHoverFromCursor(); }

bool OverlayPopupController::handlePopupShadowPointerEvent(QEvent* event) {
  if (!event) {
    return false;
  }

  auto* surface =
      dynamic_cast<OverlayPopupSurface*>(delegate_ ? delegate_->popupSurfaceWidget() : nullptr);
  if (!surface) {
    return false;
  }

  const QEvent::Type eventType = event->type();
  QPoint globalPos;
  switch (eventType) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
      globalPos = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
      break;
    case QEvent::Wheel:
      if (surface->isWindow()) {
        return false;
      }
      globalPos = static_cast<QWheelEvent*>(event)->globalPosition().toPoint();
      break;
    case QEvent::ContextMenu:
      globalPos = static_cast<QContextMenuEvent*>(event)->globalPos();
      break;
    default:
      return false;
  }

  if (surface->containsInteractiveGlobalPos(globalPos)) {
    return false;
  }

  QWidget* target = nullptr;
  if (surface->isWindow()) {
    QWidget* scope = popupAnchorWidget() ? popupAnchorWidget()->window() : popupScopeWindow();
    if (scope) {
      const QPoint scopePos = scope->mapFromGlobal(globalPos);
      if (scope->rect().contains(scopePos)) {
        target = scope->childAt(scopePos);
        if (!target) {
          target = scope;
        }
      }
    }
  }
  if (!target) {
    target = QApplication::widgetAt(globalPos);
  }
  if (!target || widgetInTree(target, surface)) {
    const bool wasTransparent = surface->testAttribute(Qt::WA_TransparentForMouseEvents);
    surface->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    target = QApplication::widgetAt(globalPos);
    surface->setAttribute(Qt::WA_TransparentForMouseEvents, wasTransparent);
  }
  if (!widgetInTree(target, popupTriggerWidget())) {
    popupCloseFromHost(PopupCloseReason::OutsidePressInScope);
  }
  if (!target || widgetInTree(target, surface)) {
    return true;
  }

  const QPointF localPos = target->mapFromGlobal(globalPos);
  if (eventType == QEvent::Wheel) {
    const auto* wheelEvent = static_cast<QWheelEvent*>(event);
    QWheelEvent forwardedEvent(localPos, globalPos, wheelEvent->pixelDelta(),
                               wheelEvent->angleDelta(), wheelEvent->buttons(),
                               wheelEvent->modifiers(), wheelEvent->phase(), wheelEvent->inverted(),
                               wheelEvent->source(), wheelEvent->pointingDevice());
    QApplication::sendEvent(target, &forwardedEvent);
  } else if (eventType == QEvent::ContextMenu) {
    const auto* contextEvent = static_cast<QContextMenuEvent*>(event);
    QContextMenuEvent forwardedEvent(contextEvent->reason(), localPos.toPoint(), globalPos,
                                     contextEvent->modifiers());
    QApplication::sendEvent(target, &forwardedEvent);
  } else {
    const auto* mouseEvent = static_cast<QMouseEvent*>(event);
    QWidget* targetWindow = target->window();
    const QPointF scenePos = targetWindow ? targetWindow->mapFromGlobal(globalPos) : localPos;
    QMouseEvent forwardedEvent(eventType, localPos, scenePos, globalPos, mouseEvent->button(),
                               mouseEvent->buttons(), mouseEvent->modifiers(),
                               mouseEvent->pointingDevice());
    QApplication::sendEvent(target, &forwardedEvent);
  }
  return true;
}

QObject* OverlayPopupController::popupOwnerObject() const {
  return const_cast<OverlayPopupController*>(this);
}

QWidget* OverlayPopupController::popupTriggerWidget() const {
  return delegate_ ? delegate_->popupTriggerWidget() : nullptr;
}

QWidget* OverlayPopupController::popupAnchorWidget() const {
  return delegate_ ? delegate_->popupAnchorWidget() : nullptr;
}

QWidget* OverlayPopupController::popupScopeWindow() const {
  return delegate_ ? delegate_->popupScopeWindow() : nullptr;
}

QWidget* OverlayPopupController::popupSurfaceWidget() const {
  return delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
}

bool OverlayPopupController::popupIsVisible() const {
  QWidget* popup = delegate_ ? delegate_->popupSurfaceWidget() : nullptr;
  return popupVisible_ && popup && popup->isVisible();
}

bool OverlayPopupController::popupWantsHostFrameRelayout() const { return false; }

bool OverlayPopupController::popupContainsGlobalPos(const QPoint& globalPos) const {
  return widgetContainsGlobalPos(popupTriggerWidget(), globalPos) ||
         widgetContainsGlobalPos(popupAnchorWidget(), globalPos) ||
         popupInteractiveContainsGlobalPos(delegate_ ? delegate_->popupSurfaceWidget() : nullptr,
                                           globalPos);
}

void OverlayPopupController::popupCloseFromHost(PopupCloseReason reason) {
  Q_UNUSED(reason)
  if (closingFromHost_) {
    return;
  }
  closingFromHost_ = true;
  clearAllOpenReasons();
  updatePopupVisibility(true, VisibilityUpdateSource::UserInteraction);
  closingFromHost_ = false;
}

void OverlayPopupController::popupRelayoutFromHost() {
  if (!popupVisible_ || !delegate_) {
    return;
  }
  if (shouldSkipQueuedRelayoutSync()) {
    return;
  }
  const bool canShowPopup = syncPopupGeometry();
  applyPopupVisibility(delegate_->popupSurfaceWidget(), canShowPopup, true);
}

}  // namespace adqt::widgets::detail
