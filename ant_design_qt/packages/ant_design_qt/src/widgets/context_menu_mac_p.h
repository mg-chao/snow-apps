#pragma once

#include <QPoint>
#include <QSize>

class QAction;
class QIcon;
class QMenu;

namespace adqt::icons {
class IconRef;
}

namespace adqt::widgets {
class AdContextMenu;

namespace detail {
bool usesNativeContextMenu();
void initializeNativeContextMenu(QMenu* menu);
QIcon nativeContextMenuIcon(QMenu* menu, const adqt::icons::IconRef& icon);
QSize nativeContextMenuSize(QMenu* menu);
QAction* execNativeContextMenu(AdContextMenu* menu, const QPoint& globalPosition,
                               QAction* initialAction);
void dismissNativeContextMenu(QMenu* menu);
}  // namespace detail
}  // namespace adqt::widgets
