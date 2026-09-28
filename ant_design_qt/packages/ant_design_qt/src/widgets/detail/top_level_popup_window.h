#pragma once
#include <QLoggingCategory>

class QWidget;

namespace adqt::widgets::detail {

Q_DECLARE_LOGGING_CATEGORY(popupLog)

class TopLevelToolResourceReleaser {
 public:
  virtual ~TopLevelToolResourceReleaser() = default;
  virtual void releaseTopLevelToolResources() = 0;
};

void syncTopLevelToolTransientParent(QWidget* toolWindow, QWidget* ownerWindow);

// Assign ownership to already-created surfaces without moving the owner's group.
void setTopLevelToolTransientParent(QWidget* toolWindow, QWidget* ownerWindow);

// Keep native tool shows/raises inside the owner's existing stacking group,
// without pulling that group ahead of unrelated applications. Call from nativeEvent.
void constrainTopLevelToolStackingToOwner(QWidget* toolWindow, void* message);

#if defined(Q_OS_MACOS)
void syncMacTopLevelPopupOwnership(QWidget* popup);
#endif

// Releases the native window and backing store after a hidden QtTool popup has
// completed its hide sequence. The QWidget and its child content are retained.
void releaseTopLevelToolResourcesOnHide(QWidget* toolWindow);

}  // namespace adqt::widgets::detail
