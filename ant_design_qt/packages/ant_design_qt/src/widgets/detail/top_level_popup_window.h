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

// Releases the native window and backing store after a hidden QtTool popup has
// completed its hide sequence. The QWidget and its child content are retained.
void releaseTopLevelToolResourcesOnHide(QWidget* toolWindow);

}  // namespace adqt::widgets::detail
