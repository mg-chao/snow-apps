#include "dpi_stable_window_controller.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
// clang-format off
#include <commctrl.h>
// clang-format on
#ifndef WM_GETDPISCALEDSIZE
#define WM_GETDPISCALEDSIZE 0x02E4
#endif
#endif

namespace adqt::widgets {
namespace {

constexpr QEvent::Type kScaleCommitEvent = static_cast<QEvent::Type>(QEvent::User + 421);
#if defined(Q_OS_WIN) || defined(_WIN32)
constexpr UINT_PTR kDpiStableSubclassId = 0x41445154;  // "ADQT"

template <typename Pointer, typename Integer>
Pointer nativePointerFromInteger(Integer value) {
  // Qt WId and Win32 message fields intentionally transport native pointer values as integers.
  return reinterpret_cast<Pointer>(value);  // NOLINT(performance-no-int-to-ptr)
}

bool usesWindowsNativeWindows() {
  return QGuiApplication::platformName().compare(QStringLiteral("windows"), Qt::CaseInsensitive) ==
         0;
}

LRESULT CALLBACK dpiStableSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR, DWORD_PTR refData) {
  auto* controller = nativePointerFromInteger<AdDpiStableWindowController*>(refData);
  if (controller) {
    MSG nativeMessage{};
    nativeMessage.hwnd = hwnd;
    nativeMessage.message = message;
    nativeMessage.wParam = wParam;
    nativeMessage.lParam = lParam;
    qintptr result = 0;
    if (controller->handleNativeMessage(&nativeMessage, &result)) {
      return static_cast<LRESULT>(result);
    }
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}
#endif

}  // namespace

AdDpiStableWindowController::AdDpiStableWindowController(QWidget* window, QObject* parent)
    : QObject(parent ? parent : window), window_(window) {
  qRegisterMetaType<AdDpiStableWindowDiagnostics>();
  if (window_) {
    window_->installEventFilter(this);
    installForCurrentWinId();
    captureBaseline();
  }
}

AdDpiStableWindowController::~AdDpiStableWindowController() { removeSubclass(); }

QWidget* AdDpiStableWindowController::window() const { return window_; }

void AdDpiStableWindowController::setScaleScope(AdControlScaleScope* scope) { scaleScope_ = scope; }

AdControlScaleScope* AdDpiStableWindowController::scaleScope() const { return scaleScope_; }

bool AdDpiStableWindowController::captureBaseline(qreal referenceDpr) {
  if (!window_) {
    return false;
  }
  if (nativeTransitionActive_) {
    return hasBaseline();
  }
  window_->winId();
  installForCurrentWinId();
  const qreal windowDpr = currentDpr();
  PhysicalBaseline next;
  next.windowId = subclassWinId_;
  next.referenceDpr =
      referenceDpr > 0.0 ? AdControlScaleContext::normalizeDpr(referenceDpr) : windowDpr;
  next.generation = baseline_.generation + 1;
  lastCommittedDpr_ = windowDpr;
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (usesWindowsNativeWindows()) {
    const HWND hwnd = nativePointerFromInteger<HWND>(subclassWinId_);
    RECT frame{};
    RECT client{};
    if (!hwnd || !GetWindowRect(hwnd, &frame) || !GetClientRect(hwnd, &client)) {
      resetBaseline();
      return false;
    }
    next.frameGeometry = QRect(frame.left, frame.top, std::max(1L, frame.right - frame.left),
                               std::max(1L, frame.bottom - frame.top));
    next.frameSize = next.frameGeometry.size();
    next.clientSize =
        QSize(std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top));
  } else
#endif
  {
    next.clientSize = QSize(std::max(1, qRound(window_->width() * windowDpr)),
                            std::max(1, qRound(window_->height() * windowDpr)));
    next.frameSize = next.clientSize;
    next.frameGeometry = QRect(window_->pos(), next.frameSize);
  }
  baseline_ = next;
  diagnostics_.finalPhysicalGeometry = baseline_.frameGeometry;
  return true;
}

void AdDpiStableWindowController::resetBaseline() {
  const quint64 nextGeneration = baseline_.generation + 1;
  baseline_ = PhysicalBaseline{};
  baseline_.generation = nextGeneration;
  dragSession_.reset();
  pendingCommit_ = PendingScaleCommit{};
  finishNativeTransition();
}

bool AdDpiStableWindowController::hasBaseline() const { return baseline_.valid(); }

qreal AdDpiStableWindowController::referenceDpr() const { return baseline_.referenceDpr; }
QSize AdDpiStableWindowController::stablePhysicalFrameSize() const {
  return baseline_.frameSize;
}
QSize AdDpiStableWindowController::stablePhysicalClientSize() const {
  return baseline_.clientSize;
}
QRect AdDpiStableWindowController::nativeFrameGeometry() const {
  return baseline_.frameGeometry;
}

bool AdDpiStableWindowController::beginPhysicalDrag() {
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (usesWindowsNativeWindows()) {
    POINT cursor{};
    return GetCursorPos(&cursor) && beginPhysicalDrag(QPointF(cursor.x, cursor.y));
  }
  return false;
#else
  return false;
#endif
}

bool AdDpiStableWindowController::beginPhysicalDrag(const QPointF& cursor) {
  if (!hasBaseline() && !captureBaseline()) {
    return false;
  }
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (usesWindowsNativeWindows()) {
    RECT frame{};
    RECT client{};
    const HWND hwnd = nativePointerFromInteger<HWND>(subclassWinId_);
    if (!hwnd || !GetWindowRect(hwnd, &frame) || !GetClientRect(hwnd, &client)) {
      return false;
    }
    baseline_.frameGeometry =
        QRect(frame.left, frame.top, std::max(1L, frame.right - frame.left),
              std::max(1L, frame.bottom - frame.top));
    baseline_.frameSize = baseline_.frameGeometry.size();
    baseline_.clientSize =
        QSize(std::max(1L, client.right - client.left), std::max(1L, client.bottom - client.top));
  }
#endif
  PhysicalDragSession drag;
  drag.lastCursor = cursor;
  drag.cursorToFrameOffset = cursor - QPointF(baseline_.frameGeometry.topLeft());
  dragSession_ = drag;
  return true;
}

bool AdDpiStableWindowController::moveForPhysicalCursor(const QPointF& cursor) {
  if (!dragSession_.has_value() || !window_) {
    return false;
  }
  dragSession_->lastCursor = cursor;
  const QPoint topLeft(qRound(cursor.x() - dragSession_->cursorToFrameOffset.x()),
                       qRound(cursor.y() - dragSession_->cursorToFrameOffset.y()));
  const QPoint physicalDelta = topLeft - baseline_.frameGeometry.topLeft();
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (usesWindowsNativeWindows()) {
    const HWND hwnd = nativePointerFromInteger<HWND>(subclassWinId_);
    if (!hwnd || !SetWindowPos(hwnd, nullptr, topLeft.x(), topLeft.y(), 0, 0,
                               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
      return false;
    }
  } else
#endif
  {
    window_->move(topLeft);
  }
  baseline_.frameGeometry.moveTopLeft(topLeft);
  if (pendingCommit_.queued && pendingCommit_.baselineGeneration == baseline_.generation) {
    pendingCommit_.physicalGeometry.moveTopLeft(topLeft);
  }
  syncAuxiliarySurfaces(physicalDelta);
  return true;
}

void AdDpiStableWindowController::endPhysicalDrag() { dragSession_.reset(); }
bool AdDpiStableWindowController::physicalDragActive() const {
  return dragSession_.has_value();
}
QPointF AdDpiStableWindowController::physicalDragAnchor() const {
  return dragSession_.has_value() ? dragSession_->cursorToFrameOffset : QPointF();
}

void AdDpiStableWindowController::registerAuxiliarySurface(QWidget* surface) {
  if (!surface) return;
  for (const auto& item : auxiliarySurfaces_)
    if (item == surface) return;
  auxiliarySurfaces_.append(surface);
}

void AdDpiStableWindowController::unregisterAuxiliarySurface(QWidget* surface) {
  auxiliarySurfaces_.erase(
      std::remove(auxiliarySurfaces_.begin(), auxiliarySurfaces_.end(), surface),
      auxiliarySurfaces_.end());
}

AdDpiStableWindowDiagnostics AdDpiStableWindowController::diagnostics() const {
  return diagnostics_;
}

bool AdDpiStableWindowController::enforceStablePhysicalSizeForMessage(void* message,
                                                                      WId expectedWindowId,
                                                                      const QSize& stableFrameSize,
                                                                      bool transitionActive,
                                                                      qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (!message || !expectedWindowId || !stableFrameSize.isValid() || stableFrameSize.isEmpty())
    return false;
  const auto* msg = static_cast<const MSG*>(message);
  if (msg->hwnd != nativePointerFromInteger<HWND>(expectedWindowId)) return false;
  if (msg->message == WM_GETDPISCALEDSIZE && msg->lParam) {
    auto* size = nativePointerFromInteger<SIZE*>(msg->lParam);
    size->cx = stableFrameSize.width();
    size->cy = stableFrameSize.height();
    if (result) *result = TRUE;
    return true;
  }
  if (msg->message == WM_WINDOWPOSCHANGING && transitionActive && msg->lParam) {
    auto* position = nativePointerFromInteger<WINDOWPOS*>(msg->lParam);
    if (!(position->flags & SWP_NOSIZE)) {
      position->cx = stableFrameSize.width();
      position->cy = stableFrameSize.height();
    }
  }
#else
  Q_UNUSED(message)
  Q_UNUSED(expectedWindowId)
  Q_UNUSED(stableFrameSize)
  Q_UNUSED(transitionActive)
  Q_UNUSED(result)
#endif
  return false;
}

bool AdDpiStableWindowController::eventFilter(QObject* watched, QEvent* event) {
  if (watched == window_ && event) {
    if (event->type() == QEvent::WinIdChange) {
      installForCurrentWinId();
      if (!subclassWinId_) {
        resetBaseline();
      } else if (baseline_.windowId != subclassWinId_) {
        resetBaseline();
        QTimer::singleShot(0, this, [this]() {
          if (window_ && subclassWinId_ && baseline_.windowId != subclassWinId_) {
            captureBaseline();
          }
        });
      }
    }
#if !defined(Q_OS_WIN) && !defined(_WIN32)
    if (event->type() == QEvent::DevicePixelRatioChange) {
      queueScaleCommit(currentDpr(), QRect(window_->pos(), window_->size()));
    }
#endif
  }
  return QObject::eventFilter(watched, event);
}

bool AdDpiStableWindowController::event(QEvent* event) {
  if (event && event->type() == kScaleCommitEvent) {
    commitPendingScale();
    return true;
  }
  return QObject::event(event);
}

void AdDpiStableWindowController::installForCurrentWinId() {
  if (!window_) return;
  const WId current = window_->internalWinId();
  if (!current) {
    removeSubclass();
    return;
  }
  if (current == subclassWinId_) return;
  removeSubclass();
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (usesWindowsNativeWindows()) {
    if (!SetWindowSubclass(nativePointerFromInteger<HWND>(current), dpiStableSubclassProc,
                           kDpiStableSubclassId, reinterpret_cast<DWORD_PTR>(this))) {
      return;
    }
  }
#endif
  subclassWinId_ = current;
}

void AdDpiStableWindowController::removeSubclass() {
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (subclassWinId_ && usesWindowsNativeWindows()) {
    RemoveWindowSubclass(nativePointerFromInteger<HWND>(subclassWinId_), dpiStableSubclassProc,
                         kDpiStableSubclassId);
  }
#endif
  subclassWinId_ = 0;
}

qreal AdDpiStableWindowController::currentDpr() const {
  if (window_ && window_->windowHandle() && window_->windowHandle()->devicePixelRatio() > 0.0)
    return window_->windowHandle()->devicePixelRatio();
  return window_ && window_->devicePixelRatioF() > 0.0 ? window_->devicePixelRatioF() : 1.0;
}

void AdDpiStableWindowController::queueScaleCommit(qreal dpr, const QRect& geometry) {
  pendingCommit_.dpr = AdControlScaleContext::normalizeDpr(dpr);
  pendingCommit_.physicalGeometry = geometry;
  pendingCommit_.baselineGeneration = baseline_.generation;
  if (pendingCommit_.queued) {
    ++diagnostics_.coalescedCount;
    return;
  }
  pendingCommit_.queued = true;
  QCoreApplication::postEvent(this, new QEvent(kScaleCommitEvent), Qt::HighEventPriority);
}

void AdDpiStableWindowController::commitPendingScale() {
  if (!pendingCommit_.queued) return;
  QElapsedTimer commitTimer;
  commitTimer.start();
  const PendingScaleCommit commit = pendingCommit_;
  pendingCommit_.queued = false;
  if (!hasBaseline() || commit.baselineGeneration != baseline_.generation) {
    finishNativeTransition();
    return;
  }
  const qreal dpr = commit.dpr;
  const QSize logicalExtent(
      hasBaseline() ? QSize(std::max(1, qRound(baseline_.clientSize.width() / dpr)),
                            std::max(1, qRound(baseline_.clientSize.height() / dpr)))
                    : QSize());
  AdControlScaleContext context =
      AdControlScaleContext::fromDprs(baseline_.referenceDpr, dpr,
                                      diagnostics_.transitionCount + 1);
  if (scaleScope_) scaleScope_->publishScale(context, logicalExtent);
  lastCommittedDpr_ = dpr;
  baseline_.frameGeometry = commit.physicalGeometry;
  diagnostics_.newDpr = dpr;
  diagnostics_.finalPhysicalGeometry = baseline_.frameGeometry;
  emit scaleCommitCompleted(context, logicalExtent);
  finishNativeTransition();
  syncAuxiliarySurfaces();
  ++diagnostics_.transitionCount;
  diagnostics_.queuedCommitNanoseconds = commitTimer.nsecsElapsed();
}

void AdDpiStableWindowController::finishNativeTransition() {
  if (window_ && nativeTransitionActive_ && windowUpdatesWereEnabled_) {
    window_->setUpdatesEnabled(true);
    window_->update();
  }
  nativeTransitionActive_ = false;
  windowUpdatesWereEnabled_ = true;
}

void AdDpiStableWindowController::syncAuxiliarySurfaces(const QPoint& physicalDelta) {
  auxiliarySurfaces_.erase(
      std::remove_if(auxiliarySurfaces_.begin(), auxiliarySurfaces_.end(),
                     [](const QPointer<QWidget>& surface) { return surface.isNull(); }),
      auxiliarySurfaces_.end());
  for (const auto& surface : auxiliarySurfaces_) {
    if (!surface) continue;
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!physicalDelta.isNull() && usesWindowsNativeWindows()) {
      const HWND hwnd = nativePointerFromInteger<HWND>(surface->winId());
      RECT frame{};
      if (hwnd && GetWindowRect(hwnd, &frame)) {
        SetWindowPos(hwnd, nullptr, frame.left + physicalDelta.x(), frame.top + physicalDelta.y(),
                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
#else
    Q_UNUSED(physicalDelta)
#endif
    surface->updateGeometry();
  }
}

bool AdDpiStableWindowController::handleNativeMessage(void* message, qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
  if (!message || !hasBaseline() || !subclassWinId_ || baseline_.windowId != subclassWinId_)
    return false;
  const auto* msg = static_cast<const MSG*>(message);
  if (msg->hwnd != nativePointerFromInteger<HWND>(subclassWinId_)) return false;
  if (enforceStablePhysicalSizeForMessage(message, subclassWinId_, baseline_.frameSize,
                                          nativeTransitionActive_ || dragSession_.has_value(),
                                          result)) {
    return true;
  }
  if (msg->message == WM_WINDOWPOSCHANGING &&
      (nativeTransitionActive_ || dragSession_.has_value())) {
    return false;
  }
  if (msg->message != WM_DPICHANGED || !msg->lParam) return false;

  QElapsedTimer timer;
  timer.start();
  auto* rect = nativePointerFromInteger<RECT*>(msg->lParam);
  QPoint topLeft(rect->left, rect->top);
  if (dragSession_.has_value()) {
    topLeft =
        QPoint(qRound(dragSession_->lastCursor.x() - dragSession_->cursorToFrameOffset.x()),
               qRound(dragSession_->lastCursor.y() - dragSession_->cursorToFrameOffset.y()));
  }
  rect->left = topLeft.x();
  rect->top = topLeft.y();
  rect->right = rect->left + baseline_.frameSize.width();
  rect->bottom = rect->top + baseline_.frameSize.height();
  const qreal newDpr = std::max<qreal>(1.0 / 96.0, HIWORD(msg->wParam) / 96.0);
  diagnostics_.oldDpr = lastCommittedDpr_;
  diagnostics_.newDpr = newDpr;
  if (window_ && !nativeTransitionActive_) {
    windowUpdatesWereEnabled_ = window_->updatesEnabled();
    if (windowUpdatesWereEnabled_) window_->setUpdatesEnabled(false);
  }
  nativeTransitionActive_ = true;
  DefSubclassProc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
  const QRect geometry(rect->left, rect->top, baseline_.frameSize.width(),
                       baseline_.frameSize.height());
  if (!dragSession_.has_value()) {
    syncAuxiliarySurfaces(geometry.topLeft() - baseline_.frameGeometry.topLeft());
  }
  queueScaleCommit(newDpr, geometry);
  diagnostics_.nativeHandlerNanoseconds = timer.nsecsElapsed();
  if (result) *result = 0;
  return true;
#else
  Q_UNUSED(message)
  Q_UNUSED(result)
  return false;
#endif
}

}  // namespace adqt::widgets
