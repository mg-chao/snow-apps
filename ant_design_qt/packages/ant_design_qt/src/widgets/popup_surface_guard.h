#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <functional>

class QWidget;

namespace adqt::widgets {
namespace detail {
class OverlayPopupController;
class OverlayPopupSurface;
}  // namespace detail

// Gates native overlay popup surfaces belonging to a window, including lazy and nested
// popups. Native identities are retained until the guard is removed so an
// asynchronous acknowledgment remains valid across hide/show transitions.
class AdPopupSurfaceGuard final : public QObject {
  Q_OBJECT

 public:
  using Guard = std::function<bool(QWidget*)>;
  AdPopupSurfaceGuard(QWidget* scopeWindow, Guard guard);
  ~AdPopupSurfaceGuard() override;

  // Retries requested popups; cancelled requests remain closed.
  void refresh();

 signals:
  void surfaceRequested(QWidget* surface);

 private:
  friend class detail::OverlayPopupController;
  static AdPopupSurfaceGuard* forScope(QWidget* scope);
  static void registerController(detail::OverlayPopupController* controller);
  bool canShow(QWidget* surface);

  struct RetainedSurface {
    QPointer<detail::OverlayPopupSurface> surface;
    bool originallyRetained = false;
  };
  QPointer<QWidget> scopeWindow_;
  Guard guard_;
  QVector<RetainedSurface> surfaces_;
};

}  // namespace adqt::widgets
