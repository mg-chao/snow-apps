#include "platform_compatibility.h"

#include <QApplication>

#if defined(Q_OS_MACOS)
#include <QAccessible>
#include <QEvent>
#include <QPointer>
#include <QPlatformSurfaceEvent>
#include <QWidget>
#include <QWindow>
#include <QtGui/private/qaccessiblecache_p.h>
#endif

namespace adqt::widgets {
#if defined(Q_OS_MACOS)
namespace {
class WidgetAccessibilityRepair final : public QObject {
 public:
  explicit WidgetAccessibilityRepair(QApplication& app) : QObject(&app) {
    app.installEventFilter(this);
  }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (event->type() != QEvent::PlatformSurface ||
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() !=
            QPlatformSurfaceEvent::SurfaceCreated) {
      return false;
    }
    auto* window = qobject_cast<QWindow*>(object);
    if (!window) {
      return false;
    }
    QWidget* widget = nullptr;
    for (auto* candidate : QApplication::topLevelWidgets()) {
      if (candidate->windowHandle() == window) {
        widget = candidate;
        break;
      }
    }
    if (!widget) {
      return false;
    }
    auto* cache = QAccessibleCache::instance();
    const auto id = cache->idForObject(widget);
    if (!id) {
      return false;
    }
    auto* accessible = cache->interfaceForId(id);
    if (!accessible || (accessible->object() == widget && accessible->isValid())) {
      return false;
    }

    // Qt 6.11 can return an expired interface for a reused QObject address. Its
    // setWindowTitle_sys() then calls QAccessibleWidget::text() without checking
    // validity. SurfaceCreated precedes that lookup during QWidget::create();
    // WinIdChange and Polish are too late on some platform paths.
    // Invalidate the object's entire cache entry so Qt can rebuild it normally;
    // do not disable accessibility or substitute an empty accessible name.
    // Use the cache's destruction slot: deleteAccessibleInterface() cannot remove
    // an orphan's object mapping, and deleteInterface(id, widget) leaves interfaces
    // cached for the object's other construction-time metaobjects behind.
    const bool cleared = QMetaObject::invokeMethod(cache, "objectDestroyed", Qt::DirectConnection,
                                                   Q_ARG(QObject*, widget));
    if (!cleared) {
      qFatal(
          "Qt accessibility cache cleanup is unavailable; revalidate the macOS "
          "platform compatibility layer for this Qt version");
    }
    qWarning("Recovered stale Qt accessibility cache entries for %s",
             widget->metaObject()->className());
    return false;
  }
};
}  // namespace
#endif

void initializePlatformCompatibility(QApplication& app) {
#if defined(Q_OS_MACOS)
  static QPointer<WidgetAccessibilityRepair> repair;
  if (!repair) {
    repair = new WidgetAccessibilityRepair(app);
  }
#else
  Q_UNUSED(app)
#endif
}
}  // namespace adqt::widgets
