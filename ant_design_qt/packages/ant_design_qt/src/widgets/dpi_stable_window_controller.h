#pragma once

#include "control_scale.h"

#include <QElapsedTimer>
#include <QMargins>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QtGui/qwindowdefs.h>

#include <optional>

class QEvent;
class QWidget;

namespace adqt::widgets {

class AdDpiStableWindowControllerTestAccess;

struct AdDpiStableWindowDiagnostics {
  qint64 nativeHandlerNanoseconds = 0;
  qint64 queuedCommitNanoseconds = 0;
  quint64 transitionCount = 0;
  quint64 coalescedCount = 0;
  quint64 committedGeneration = 0;
  quint64 reconciliationCount = 0;
  qreal oldDpr = 1.0;
  qreal newDpr = 1.0;
  QRect finalPhysicalGeometry;
};

struct AdDpiStableWindowTransition {
  quint64 generation = 0;
  quint64 baselineGeneration = 0;
  WId windowId = 0;
  AdControlScaleContext context;
  QRect physicalFrame;
  QSize physicalClientSize;
  QSize logicalClientExtent;
  QPointF physicalContentAnchor;
};

class AdDpiStableWindowController final : public QObject {
  Q_OBJECT

 public:
  explicit AdDpiStableWindowController(QWidget* window, QObject* parent = nullptr);
  ~AdDpiStableWindowController() override;

  QWidget* window() const;
  void setScaleScope(AdControlScaleScope* scope);
  AdControlScaleScope* scaleScope() const;

  // A non-positive referenceDpr uses the window's current display. A positive
  // value keeps logical scaling anchored to the caller's reference display
  // while the physical baseline is measured from the current window.
  // An explicit client extent establishes an intentional physical size before
  // placement can generate nested DPI messages (frameless clients have no margins).
  bool captureBaseline(qreal referenceDpr = 0.0, const QSize& physicalClientExtent = QSize());
  void resetBaseline();
  bool hasBaseline() const;
  qreal referenceDpr() const;
  QSize stablePhysicalFrameSize() const;
  QSize stablePhysicalClientSize() const;
  QRect nativeFrameGeometry() const;
  void setPhysicalContentOffset(const QPointF& offset);
  bool restorePhysicalContentAnchor(const QPointF& anchor, const QPointF& offset);
  // Coalesces programmatic reconciliation with pending native DPI changes.
  void requestScaleCommit();

  bool beginPhysicalDrag();
  bool beginPhysicalDrag(const QPointF& physicalCursor);
  bool moveForPhysicalCursor(const QPointF& physicalCursor);
  void endPhysicalDrag();
  bool physicalDragActive() const;
  QPointF physicalDragAnchor() const;

  void registerAuxiliarySurface(QWidget* surface);
  void unregisterAuxiliarySurface(QWidget* surface);

  AdDpiStableWindowDiagnostics diagnostics() const;

  static bool enforceStablePhysicalSizeForMessage(void* nativeMessage, WId expectedWindowId,
                                                  const QSize& stableFrameSize,
                                                  bool transitionActive, qintptr* result);

  // Used by the platform subclass trampoline. Consumers should not call this.
  bool handleNativeMessage(void* nativeMessage, qintptr* result);

 signals:
  // Direct GUI-thread handlers reconcile content while presentation is suspended.
  void scaleCommitReady(const adqt::widgets::AdDpiStableWindowTransition& transition);
  void scaleCommitCompleted(const adqt::widgets::AdControlScaleContext& context,
                            const QSize& logicalClientExtent);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  bool event(QEvent* event) override;

 private:
  friend class AdDpiStableWindowControllerTestAccess;

  struct PhysicalBaseline {
    WId windowId = 0;
    qreal referenceDpr = 1.0;
    QSize frameSize;
    QSize clientSize;
    QRect frameGeometry;
    quint64 generation = 0;

    [[nodiscard]] bool valid() const {
      return windowId != 0 && frameSize.isValid() && !frameSize.isEmpty() && clientSize.isValid() &&
             !clientSize.isEmpty();
    }
  };

  struct PhysicalDragSession {
    QPointF cursorToFrameOffset;
    QPointF lastCursor;
  };

  struct PendingScaleCommit {
    quint64 baselineGeneration = 0;
    bool queued = false;
  };

  void installForCurrentWinId();
  void removeSubclass();
  qreal currentDpr() const;
  QRect currentNativeFrameGeometry() const;
  // Native frame origin for the active drag session; requires dragSession_ to be engaged.
  QPoint dragFrameTopLeft() const;
  // Snaps a native frame origin onto the logical pixel grid of the screen that will host
  // the frame, so Qt's logical<->native round trip reproduces it exactly.
  static QPoint stableNativeTopLeft(const QPoint& nativeTopLeft, const QSize& nativeFrameSize);
  void queueScaleCommit();
  void commitPendingScale();
  void finishNativeTransition();
  void suspendPresentation();
  void syncAuxiliarySurfaces(const QPoint& physicalDelta = QPoint());

  QPointer<QWidget> window_;
  QPointer<AdControlScaleScope> scaleScope_;
  QList<QPointer<QWidget>> auxiliarySurfaces_;
  WId subclassWinId_ = 0;
  PhysicalBaseline baseline_;
  std::optional<PhysicalDragSession> dragSession_;
  PendingScaleCommit pendingCommit_;
  qreal lastCommittedDpr_ = 1.0;
  QPointF physicalContentOffset_;
  quint64 transitionGeneration_ = 0;
  bool presentationSuspended_ = false;
  bool committing_ = false;
  bool nativeTransitionActive_ = false;
  bool windowUpdatesWereEnabled_ = true;
  AdDpiStableWindowDiagnostics diagnostics_;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdDpiStableWindowDiagnostics)
Q_DECLARE_METATYPE(adqt::widgets::AdDpiStableWindowTransition)
