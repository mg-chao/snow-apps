#pragma once

class QWidget;

namespace adqt::widgets::detail {

class TopLevelToolResourceReleaser {
 public:
  virtual ~TopLevelToolResourceReleaser() = default;
  virtual void releaseTopLevelToolResources() = 0;
};

void syncTopLevelToolTransientParent(QWidget* toolWindow, QWidget* ownerWindow);

// Releases the native window and backing store after a hidden QtTool popup has
// completed its hide sequence. The QWidget and its child content are retained.
void releaseTopLevelToolResourcesOnHide(QWidget* toolWindow);

}  // namespace adqt::widgets::detail
