#pragma once

#include <QPoint>

class QObject;
class QWidget;

namespace adqt::widgets::detail {

enum class PopupCloseReason {
  OutsidePressInScope,
  EscapeKeyPress,
  ScopeHidden,
  ScopeDeactivated,
  OwnerHidden,
  OwnerDestroyed,
  SupersededByAnotherOwner,
  ExplicitClose,
};

class PopupInteractionOwner {
 public:
  virtual ~PopupInteractionOwner() = default;

  virtual QObject* popupOwnerObject() const = 0;
  virtual QWidget* popupAnchorWidget() const = 0;
  virtual QWidget* popupScopeWindow() const = 0;
  virtual QWidget* popupSurfaceWidget() const { return nullptr; }
  virtual bool popupIsVisible() const = 0;
  virtual bool popupWantsHostFrameRelayout() const { return true; }
  virtual bool popupContainsGlobalPos(const QPoint& globalPos) const = 0;
  virtual void popupCloseFromHost(PopupCloseReason reason) = 0;
  virtual void popupRelayoutFromHost() = 0;
};

void setPopupInteractionHostOpen(PopupInteractionOwner* owner, bool open);
// A suspended parent keeps its hover session while the pointer interacts with
// an actual descendant in the host's popup ownership chain.
bool popupDescendantContainsPointer(const PopupInteractionOwner* owner, const QWidget* target,
                                    const QPoint& globalPosition);

}  // namespace adqt::widgets::detail
