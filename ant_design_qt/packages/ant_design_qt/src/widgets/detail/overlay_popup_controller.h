#pragma once

#include "../popup_interaction_host.h"
#include "../popup_placement.h"
#include "../popup_types.h"

#include <QFlags>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QRect>
#include <QScrollBar>
#include <QVector>

#include <functional>
#include <optional>

class QEvent;
class QWidget;

namespace adqt::widgets::detail {

class OverlayPopupControllerDelegate {
 public:
  virtual ~OverlayPopupControllerDelegate() = default;

  virtual QObject* popupOwnerObject() const = 0;
  virtual QWidget* popupTriggerWidget() const { return popupAnchorWidget(); }
  virtual QWidget* popupAnchorWidget() const = 0;
  virtual QWidget* popupScopeWindow() const = 0;
  virtual QWidget* popupSurfaceWidget() const = 0;
  virtual QWidget* popupEnsureSurface() = 0;
  virtual void popupPrepareToShow() = 0;
  virtual bool popupHasContent() const = 0;
  virtual bool popupAcceptsGeometry(const QRect&, const QSize&, const QRect&) const { return true; }
  virtual std::optional<QRect> popupTriggerGlobalRect() const { return std::nullopt; }
  virtual std::optional<QRect> popupAnchorGlobalRect() const { return std::nullopt; }
  virtual OverlayPopupPlacement popupPlacement() const = 0;
  virtual AdPopupLayerMode popupLayerMode() const { return AdPopupLayerMode::InWindow; }
  virtual bool popupAutoAdjustOverflow() const = 0;
  virtual bool popupArrowVisible() const = 0;
  virtual bool popupArrowPointAtCenter() const = 0;
  virtual int popupOffset() const = 0;
  virtual int popupArrowOffsetHorizontal() const = 0;
  virtual int popupArrowOffsetVertical() const = 0;
  virtual void popupApplyResolvedPlacement(OverlayPopupPlacement placement,
                                           qreal arrowCenterCoord) = 0;
  virtual bool popupReleaseOnHide() const { return false; }
  virtual void popupReleaseSurface() {}
};

class OverlayPopupController final : public QObject, private PopupInteractionOwner {
  Q_OBJECT

 public:
  using CursorPositionProvider = std::function<QPoint()>;

  enum class Trigger {
    Hover = 0x1,
    Focus = 0x2,
    Click = 0x4,
    ContextMenu = 0x8,
  };
  Q_DECLARE_FLAGS(Triggers, Trigger)

  enum class VisibilityMode {
    Automatic,
    External,
  };

  explicit OverlayPopupController(OverlayPopupControllerDelegate* delegate,
                                  QObject* parent = nullptr,
                                  CursorPositionProvider cursorPositionProvider = {});
  ~OverlayPopupController() override;

  static void resetSyncPopupGeometryCountersForTesting();
  static qint64 syncPopupGeometryCallCountForTesting();
  static qint64 syncPopupGeometryShortCircuitCountForTesting();

  OverlayPopupControllerDelegate* delegate() const { return delegate_; }

  Triggers triggerModes() const { return triggerModes_; }
  void setTriggerModes(Triggers value);

  VisibilityMode visibilityMode() const { return visibilityMode_; }
  void setVisibilityMode(VisibilityMode value);

  bool popupVisible() const { return popupVisible_; }
  void setPopupVisible(bool value);

  bool disabled() const { return disabled_; }
  void setDisabled(bool value);

  int mouseEnterDelayMs() const { return mouseEnterDelayMs_; }
  void setMouseEnterDelayMs(int value);

  int mouseLeaveDelayMs() const { return mouseLeaveDelayMs_; }
  void setMouseLeaveDelayMs(int value);

  void anchorWidgetChanged();
  void popupSurfaceChanged();
  void popupContentChanged(bool emitSignal = true);
  void refreshVisiblePopup();
  void invalidatePopupGeometry();

  using WatchedObjectList = QVector<QPointer<QWidget>>;

 signals:
  void popupVisibleChanged(bool value);
  void popupVisibilityRequested(bool value);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  enum class InternalOpenReason {
    Hover,
    Focus,
    Click,
    ContextMenu,
    Programmatic,
  };

  enum class VisibilityUpdateSource {
    InternalState,
    UserInteraction,
  };

  void tracePopup(const char* event, int detail = 0) const;
  int geometryRejection_ = 0;
  void setReasonOpen(InternalOpenReason reason, bool enabled);
  bool reasonOpen(InternalOpenReason reason) const;
  void clearAllOpenReasons();
  bool hasTrigger(Trigger trigger) const;
  bool shouldBeOpen() const;
  QPoint cursorGlobalPos() const;
  bool triggerContainsGlobalPos(const QPoint& globalPos) const;
  bool hoverRegionContainsGlobalPos(const QPoint& globalPos) const;
  void reconcileHoverFromCursor();
  void scheduleHoverOpen();
  void scheduleHoverClose();
  void finishHoverOpen();
  void finishHoverClose();
  void resetHoverInteraction();
  void refreshHoverMonitor();

  void noteGeometryActivity();
  void schedulePopupRelayout(bool extendFrameTail);
  void cancelPopupRelayout();
  void refreshGeometryFrameSync();
  bool shouldSkipQueuedRelayoutSync() const;
  void resetGeometrySyncSnapshot();
  void markAnchorScrollWatchersDirty();
  void refreshAnchorScrollBarWatchers();
  void clearAnchorScrollBarWatchers();

  void emitVisibilityRequest(bool requestedVisible);
  void updatePopupVisibility(bool emitSignal, VisibilityUpdateSource source);
  void setPopupVisibleInternal(bool visible, bool emitSignal);
  void finishPopupVisibilityUpdate();
  void syncPreparedPopupVisibility();
  bool syncPopupGeometry(bool prepareLayout = true);
  bool popupUsesInWindowLayer() const;
  bool popupUsesTopLevelToolLayer() const;
  void syncPopupTooltipRoute();

  void refreshTriggerWatchers();
  void clearTriggerWatchers();
  void refreshPopupWatchers();
  void clearPopupWatchers();
  bool watchedByTrigger(QObject* watched) const;
  bool watchedByPopup(QObject* watched) const;

  void handleTriggerPress(QObject* watched, QEvent* event);
  void handleTriggerRelease(QObject* watched, QEvent* event);
  void handleTriggerKeyPress(QEvent* event);
  void handleTriggerKeyRelease(QEvent* event);
  void handleTriggerContextMenu(QEvent* event);
  void handleTriggerFocusOutDeferred();
  void handleTriggerHoverEnter();
  void handleTriggerHoverLeave();
  void handlePopupHoverEnter();
  void handlePopupHoverLeave();
  bool handlePopupShadowPointerEvent(QEvent* event);

  QObject* popupOwnerObject() const override;
  QWidget* popupTriggerWidget() const;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  QWidget* popupSurfaceWidget() const override;
  bool popupIsVisible() const override;
  bool popupWantsHostFrameRelayout() const override;
  bool popupContainsGlobalPos(const QPoint& globalPos) const override;
  void popupCloseFromHost(PopupCloseReason reason) override;
  void popupRelayoutFromHost() override;

  OverlayPopupControllerDelegate* delegate_ = nullptr;
  Triggers triggerModes_ = Trigger::Hover;
  VisibilityMode visibilityMode_ = VisibilityMode::Automatic;
  bool popupVisible_ = false;
  bool disabled_ = false;
  int mouseEnterDelayMs_ = 100;
  int mouseLeaveDelayMs_ = 100;
  CursorPositionProvider cursorPositionProvider_;

  QPointer<QObject> watchedTriggerRoot_;
  WatchedObjectList watchedTriggerObjects_;
  WatchedObjectList watchedPopupObjects_;

  bool hoverRegionInside_ = false;
  bool hoverSessionActive_ = false;
  bool hoverTransitionPending_ = false;
  bool hoverMonitorScheduled_ = false;
  bool focusTriggerActive_ = false;
  bool focusPopupActive_ = false;
  bool openByHover_ = false;
  bool openByFocus_ = false;
  bool openByClick_ = false;
  bool openByContextMenu_ = false;
  bool openByProgrammatic_ = false;
  bool triggerPressActive_ = false;
  bool triggerKeyPressActive_ = false;
  bool closingFromHost_ = false;
  bool updatingPopupVisible_ = false;
  std::optional<bool> pendingPopupVisible_;
  bool pendingPopupVisibleEmitSignal_ = false;
  bool popupRelayoutQueued_ = false;
  QPointer<QWidget> popupRelayoutQueuedScope_;

  struct ScrollBarWatch {
    QMetaObject::Connection valueChanged;
    QMetaObject::Connection destroyed;
  };
  QHash<QScrollBar*, ScrollBarWatch> watchedAnchorScrollBars_;
  QPointer<QWidget> watchedScrollAnchor_;
  QPointer<QWidget> watchedScrollScope_;
  bool anchorScrollWatchersDirty_ = true;
  qint64 nextAnchorScrollWatchersRefreshMs_ = 0;
  bool geometryFrameSyncSubscribed_ = false;
  qint64 geometryFrameSyncDeadlineMs_ = -1;
  QPointer<QWidget> geometrySyncParent_;
  QRect geometrySyncAnchorRect_;
  QRect geometrySyncBounds_;
  QSize geometrySyncPopupSize_;
  OverlayPopupPlacement geometrySyncPlacement_ = OverlayPopupPlacement::Top;
  AdPopupLayerMode geometrySyncLayerMode_ = AdPopupLayerMode::InWindow;
  bool geometrySyncAutoAdjustOverflow_ = true;
  int geometrySyncPopupOffset_ = 0;
  bool geometrySyncArrowPointAtCenter_ = false;
  int geometrySyncArrowOffsetHorizontal_ = 0;
  int geometrySyncArrowOffsetVertical_ = 0;
  bool geometrySyncSnapshotValid_ = false;
  std::optional<QPoint> contextMenuGlobalPos_;
};

}  // namespace adqt::widgets::detail

Q_DECLARE_OPERATORS_FOR_FLAGS(adqt::widgets::detail::OverlayPopupController::Triggers)
