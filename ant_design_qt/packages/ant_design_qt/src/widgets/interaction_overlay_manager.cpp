#include "interaction_overlay_manager.h"
#include "detail/timing_hub.h"

#include <QHash>
#include <QChildEvent>
#include <QDynamicPropertyChangeEvent>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegion>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace adqt::widgets {

namespace {

constexpr char kInteractionSurfaceProperty[] = "adqt.interaction.surface";
constexpr char kInteractionOverlayMarkerProperty[] = "adqt.interaction.overlay";
constexpr int kInteractionWaveSpreadDurationMs = 300;
constexpr int kInteractionWaveThickenDurationMs = 180;
constexpr qreal kInteractionWaveInitialOpacity = 0.16;
constexpr qreal kInteractionWaveStrokeWidth = 1.6;
constexpr char kWaveFrameKey[] = "SharedInteractionOverlay.WaveFrame";

qreal clampUnit(qreal value) { return std::clamp(value, 0.0, 1.0); }

qreal easeOutCirc(qreal t) {
  const qreal x = clampUnit(t) - 1.0;
  return std::sqrt(std::max<qreal>(0.0, 1.0 - x * x));
}

qreal lerp(qreal a, qreal b, qreal t) { return a + (b - a) * clampUnit(t); }

bool rectApproximatelyEqual(const QRectF& lhs, const QRectF& rhs) {
  constexpr qreal kEpsilon = 0.25;
  return std::abs(lhs.x() - rhs.x()) <= kEpsilon && std::abs(lhs.y() - rhs.y()) <= kEpsilon &&
         std::abs(lhs.width() - rhs.width()) <= kEpsilon &&
         std::abs(lhs.height() - rhs.height()) <= kEpsilon;
}

bool canMapWidgetToHostWindow(const QWidget* widget, const QWidget* hostWindow) {
  if (!widget || !hostWindow) {
    return false;
  }
  if (widget == hostWindow) {
    return true;
  }
  if (widget->window() != hostWindow) {
    return false;
  }
  return hostWindow->isAncestorOf(widget);
}

bool canMapHostWindowToOverlayParent(const QWidget* hostWindow, const QWidget* overlayParent) {
  if (!hostWindow || !overlayParent) {
    return false;
  }
  if (hostWindow == overlayParent) {
    return true;
  }
  if (overlayParent->window() != hostWindow) {
    return false;
  }
  return hostWindow->isAncestorOf(overlayParent);
}

qreal expandedCornerRadius(qreal baseRadius, qreal outwardOffset) {
  if (baseRadius <= 0.0) {
    return 0.0;
  }
  return baseRadius + std::max<qreal>(0.0, outwardOffset);
}

QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft) {
  const qreal w = std::max(rect.width(), 0.0);
  const qreal h = std::max(rect.height(), 0.0);
  const qreal maxRadius = std::min(w, h) / 2.0;

  topLeft = std::clamp(topLeft, 0.0, maxRadius);
  topRight = std::clamp(topRight, 0.0, maxRadius);
  bottomRight = std::clamp(bottomRight, 0.0, maxRadius);
  bottomLeft = std::clamp(bottomLeft, 0.0, maxRadius);

  const qreal left = rect.left();
  const qreal top = rect.top();
  const qreal right = left + rect.width();
  const qreal bottom = top + rect.height();

  QPainterPath path;
  path.moveTo(left + topLeft, top);
  path.lineTo(right - topRight, top);
  if (topRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * topRight, top, 2.0 * topRight, 2.0 * topRight), 90.0, -90.0);
  }
  path.lineTo(right, bottom - bottomRight);
  if (bottomRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * bottomRight, bottom - 2.0 * bottomRight, 2.0 * bottomRight,
                      2.0 * bottomRight),
               0.0, -90.0);
  }
  path.lineTo(left + bottomLeft, bottom);
  if (bottomLeft > 0.0) {
    path.arcTo(QRectF(left, bottom - 2.0 * bottomLeft, 2.0 * bottomLeft, 2.0 * bottomLeft), 270.0,
               -90.0);
  }
  path.lineTo(left, top + topLeft);
  if (topLeft > 0.0) {
    path.arcTo(QRectF(left, top, 2.0 * topLeft, 2.0 * topLeft), 180.0, -90.0);
  }
  path.closeSubpath();
  return path;
}

bool isValidInteractionWaveRequest(const InteractionWaveRequest& request) {
  if (!request.owner) {
    return false;
  }
  if (!request.baseRectInWindow.isValid() || request.baseRectInWindow.width() <= 0.0 ||
      request.baseRectInWindow.height() <= 0.0) {
    return false;
  }
  if (!request.color.isValid() || request.color.alpha() <= 0) {
    return false;
  }
  if (request.strokeWidthScale <= 0.0) {
    return false;
  }
  return true;
}

bool isValidInteractionFocusRequest(const InteractionFocusRequest& request) {
  if (!request.owner) {
    return false;
  }
  if (!request.baseRectInWindow.isValid() || request.baseRectInWindow.width() <= 0.0 ||
      request.baseRectInWindow.height() <= 0.0) {
    return false;
  }
  if (!request.color.isValid() || request.color.alpha() <= 0) {
    return false;
  }
  if (request.strokeWidth <= 0.0) {
    return false;
  }
  return true;
}

QWidget* resolveInteractionHostWindow(const QWidget* owner) {
  if (!owner) {
    return nullptr;
  }

  QWidget* hostWindow = owner->window();
  if (hostWindow) {
    return hostWindow;
  }

  return const_cast<QWidget*>(owner);
}

const QWidget* resolveInteractionSurfaceRoot(const QWidget* owner, const QWidget* hostWindow) {
  if (!owner || !hostWindow) {
    return nullptr;
  }

  const QWidget* cursor = owner;
  while (cursor) {
    if (cursor->property(kInteractionSurfaceProperty).toBool()) {
      return cursor;
    }
    if (cursor == hostWindow) {
      break;
    }
    cursor = cursor->parentWidget();
  }
  return nullptr;
}

bool isOverlayWidget(const QWidget* widget) {
  return widget && widget->property(kInteractionOverlayMarkerProperty).toBool();
}

QList<QWidget*> visibleSurfaceWidgets(const QWidget* hostWindow) {
  QList<QWidget*> surfaces;
  if (!hostWindow) {
    return surfaces;
  }

  QVector<const QObject*> pending;
  const QObjectList& rootChildren = hostWindow->children();
  pending.reserve(rootChildren.size());
  for (const QObject* child : rootChildren) {
    pending.push_back(child);
  }

  while (!pending.isEmpty()) {
    const QObject* node = pending.back();
    pending.pop_back();

    const QObjectList& childNodes = node->children();
    for (const QObject* child : childNodes) {
      pending.push_back(child);
    }

    const QWidget* candidate = qobject_cast<const QWidget*>(node);
    if (!candidate || candidate->window() != hostWindow) {
      continue;
    }

    if (!candidate->property(kInteractionSurfaceProperty).toBool()) {
      continue;
    }
    if (!candidate->isVisible() || isOverlayWidget(candidate)) {
      continue;
    }
    surfaces.append(const_cast<QWidget*>(candidate));
  }
  return surfaces;
}

QWidget* topVisibleSurfaceSibling(QWidget* hostWindow) {
  if (!hostWindow) {
    return nullptr;
  }

  const QObjectList& children = hostWindow->children();
  for (qsizetype i = children.size() - 1; i >= 0; --i) {
    auto* child = qobject_cast<QWidget*>(children.at(i));
    if (!child || child->parentWidget() != hostWindow) {
      continue;
    }
    if (!child->isVisible() || isOverlayWidget(child)) {
      continue;
    }
    if (child->property(kInteractionSurfaceProperty).toBool()) {
      return child;
    }
  }
  return nullptr;
}

QRect mapRectToHostWindow(const QWidget* widget, const QWidget* hostWindow) {
  if (!widget || !hostWindow) {
    return QRect();
  }
  return QRect(widget->mapTo(const_cast<QWidget*>(hostWindow), QPoint(0, 0)), widget->size());
}

QRectF mapRectFromHostToOverlay(const QRectF& hostRect, const QWidget* hostWindow,
                                const QWidget* overlayParent) {
  if (!hostWindow || !overlayParent || hostWindow == overlayParent) {
    return hostRect;
  }
  if (!canMapHostWindowToOverlayParent(hostWindow, overlayParent)) {
    return hostRect;
  }

  const QPoint mappedTopLeft =
      overlayParent->mapFrom(const_cast<QWidget*>(hostWindow), hostRect.topLeft().toPoint());
  return QRectF(mappedTopLeft, hostRect.size());
}

bool tryMapHostRectToOwnerRect(const QWidget* owner, const QWidget* hostWindow,
                               const QRectF& hostRect, QRectF* ownerRect) {
  if (!owner || !hostWindow || !ownerRect) {
    return false;
  }
  if (!canMapWidgetToHostWindow(owner, hostWindow)) {
    return false;
  }

  const QPoint ownerOriginInHost = owner->mapTo(const_cast<QWidget*>(hostWindow), QPoint(0, 0));
  *ownerRect = hostRect.translated(-ownerOriginInHost.x(), -ownerOriginInHost.y());
  return true;
}

class SharedInteractionOverlay final : public QWidget {
 public:
  struct WaveTrack {
    QPointer<const QWidget> owner;
    InteractionWaveRequest request;
    qint64 startMs = 0;
  };

  struct FocusTrack {
    QPointer<const QWidget> owner;
    InteractionFocusRequest request;
    QRectF ownerBaseRect;
    bool ownerBaseRectValid = false;
    QRectF resolvedBaseRect;
    bool resolvedBaseRectValid = false;
  };

  explicit SharedInteractionOverlay(QWidget* parent, QWidget* hostWindow, bool clipAgainstSurfaces)
      : QWidget(parent), hostWindow_(hostWindow), clipAgainstSurfaces_(clipAgainstSurfaces) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setProperty(kInteractionOverlayMarkerProperty, true);
    hide();
    if (clipAgainstSurfaces_ && hostWindow_) {
      observeObjectTree(hostWindow_);
      invalidateVisibleSurfaceCache();
    }
  }

  void triggerInteractionWave(const InteractionWaveRequest& request, const QRectF& localBaseRect) {
    if (!isValidInteractionWaveRequest(request)) {
      return;
    }
    if (detail::waveDurationMs() <= 0) {
      return;
    }

    WaveTrack track;
    track.owner = request.owner;
    track.request = request;
    track.request.baseRectInWindow = localBaseRect;
    track.startMs = detail::timingNowMs();
    waveTracks_.insert(request.owner, track);

    refreshOverlayState();
  }

  void triggerInteractionFocus(const InteractionFocusRequest& request,
                               const QRectF& localBaseRect) {
    if (!isValidInteractionFocusRequest(request)) {
      return;
    }

    FocusTrack track;
    track.owner = request.owner;
    track.request = request;
    track.request.baseRectInWindow = localBaseRect;
    track.ownerBaseRectValid = tryMapHostRectToOwnerRect(
        request.owner, hostWindow_, request.baseRectInWindow, &track.ownerBaseRect);
    track.resolvedBaseRect = localBaseRect;
    track.resolvedBaseRectValid = true;
    if (track.ownerBaseRectValid && track.owner) {
      track.resolvedBaseRect = resolveFocusTrackRect(track);
    }
    focusTracks_.insert(request.owner, track);

    refreshOverlayState();
  }

  void stopInteractionWaveIfOwner(const QWidget* owner) {
    if (!owner) {
      return;
    }
    if (waveTracks_.remove(owner)) {
      refreshOverlayState();
    }
  }

  void stopInteractionFocusIfOwner(const QWidget* owner) {
    if (!owner) {
      return;
    }
    if (focusTracks_.remove(owner)) {
      refreshOverlayState();
    }
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!clipAgainstSurfaces_ || !hostWindow_ || !event) {
      return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
      case QEvent::ChildAdded: {
        auto* childEvent = static_cast<QChildEvent*>(event);
        if (childEvent && childEvent->child()) {
          observeObjectTree(childEvent->child());
        }
        markVisibleSurfaceCacheDirty();
        break;
      }
      case QEvent::ChildRemoved:
        markVisibleSurfaceCacheDirty();
        break;
      case QEvent::Show:
      case QEvent::Hide:
      case QEvent::ParentChange:
      case QEvent::ZOrderChange: {
        const QWidget* widget = qobject_cast<const QWidget*>(watched);
        if (widget && widget->property(kInteractionSurfaceProperty).toBool()) {
          markVisibleSurfaceCacheDirty();
        }
        break;
      }
      case QEvent::DynamicPropertyChange: {
        auto* propertyEvent = static_cast<QDynamicPropertyChangeEvent*>(event);
        if (propertyEvent &&
            propertyEvent->propertyName() == QByteArray(kInteractionSurfaceProperty)) {
          markVisibleSurfaceCacheDirty();
        }
        break;
      }
      case QEvent::Destroy:
        observedObjects_.remove(watched);
        markVisibleSurfaceCacheDirty();
        break;
      default:
        break;
    }

    return QWidget::eventFilter(watched, event);
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    pruneStaleTracks();
    pruneFinishedWaveTracks();
    if (!hasAnyTrack()) {
      refreshOverlayState();
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (clipAgainstSurfaces_ && hostWindow_) {
      refreshVisibleSurfaceCache();
      QRegion clip(rect());
      for (const QPointer<QWidget>& surfaceRef : visibleSurfaceCache_) {
        QWidget* surface = surfaceRef.data();
        if (!surface || surface == parentWidget()) {
          continue;
        }
        if (surface->window() != hostWindow_) {
          continue;
        }
        if (!surface->property(kInteractionSurfaceProperty).toBool()) {
          continue;
        }
        if (!surface->isVisible() || isOverlayWidget(surface)) {
          continue;
        }
        const QRect surfaceRectInHost = mapRectToHostWindow(surface, hostWindow_);
        if (!surfaceRectInHost.isValid()) {
          continue;
        }
        const QPoint topLeft = mapFrom(hostWindow_.data(), surfaceRectInHost.topLeft());
        clip -= QRect(topLeft, surfaceRectInHost.size());
      }
      painter.setClipRegion(clip);
    }

    const qint64 nowMs = detail::timingNowMs();
    const int totalWaveDurationMs = std::max(0, detail::waveDurationMs());
    for (auto it = waveTracks_.cbegin(); it != waveTracks_.cend(); ++it) {
      const WaveTrack& track = it.value();
      const qint64 elapsedMs = std::max<qint64>(0, nowMs - track.startMs);
      const qreal spreadProgress =
          easeOutCirc(static_cast<qreal>(elapsedMs) / kInteractionWaveSpreadDurationMs);
      const qreal fadeProgress =
          totalWaveDurationMs > 0 ? clampUnit(static_cast<qreal>(elapsedMs) / totalWaveDurationMs)
                                  : 1.0;
      const qreal thickenProgress =
          easeOutCirc(static_cast<qreal>(elapsedMs) / kInteractionWaveThickenDurationMs);
      const qreal opacity = kInteractionWaveInitialOpacity * (1.0 - fadeProgress);
      if (opacity <= 0.0) {
        continue;
      }

      const qreal strokeScale = std::max<qreal>(0.1, track.request.strokeWidthScale);
      const qreal startStroke =
          std::max<qreal>(0.6, (kInteractionWaveStrokeWidth * 0.32) * strokeScale);
      const qreal endStroke = std::max<qreal>(startStroke, 6.0 * strokeScale);
      const qreal strokeProgress = std::max(spreadProgress, thickenProgress);
      const qreal strokeWidth = lerp(startStroke, endStroke, strokeProgress);
      const qreal outwardOffset = strokeWidth * 0.5;

      const QRectF waveRect = track.request.baseRectInWindow.adjusted(
          -outwardOffset, -outwardOffset, outwardOffset, outwardOffset);
      const QPainterPath wavePath =
          roundedRectPath(waveRect, expandedCornerRadius(track.request.topLeft, outwardOffset),
                          expandedCornerRadius(track.request.topRight, outwardOffset),
                          expandedCornerRadius(track.request.bottomRight, outwardOffset),
                          expandedCornerRadius(track.request.bottomLeft, outwardOffset));

      QColor waveColor = track.request.color;
      const qreal baseAlpha = clampUnit(static_cast<double>(waveColor.alphaF()));
      waveColor.setAlphaF(static_cast<float>(baseAlpha * clampUnit(opacity)));
      if (waveColor.alpha() <= 0) {
        continue;
      }

      QPen wavePen(waveColor, strokeWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
      painter.setPen(wavePen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(wavePath);
    }

    for (auto it = focusTracks_.cbegin(); it != focusTracks_.cend(); ++it) {
      const FocusTrack& track = it.value();
      const qreal strokeWidth = std::max<qreal>(1.0, track.request.strokeWidth);
      const qreal outwardOffset = std::max<qreal>(0.0, track.request.offset) + strokeWidth / 2.0;
      const QRectF focusBaseRect = focusTrackBaseRect(track);
      const QRectF focusRect =
          focusBaseRect.adjusted(-outwardOffset, -outwardOffset, outwardOffset, outwardOffset);
      const QPainterPath focusPath =
          roundedRectPath(focusRect, expandedCornerRadius(track.request.topLeft, outwardOffset),
                          expandedCornerRadius(track.request.topRight, outwardOffset),
                          expandedCornerRadius(track.request.bottomRight, outwardOffset),
                          expandedCornerRadius(track.request.bottomLeft, outwardOffset));

      QPen focusPen(track.request.color, strokeWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
      painter.setPen(focusPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(focusPath);
    }
  }

 private:
  void observeObjectTree(QObject* root) {
    if (!root) {
      return;
    }

    QVector<QObject*> pending;
    pending.push_back(root);
    while (!pending.isEmpty()) {
      QObject* node = pending.back();
      pending.pop_back();
      if (!node || observedObjects_.contains(node)) {
        continue;
      }

      observedObjects_.insert(node);
      if (node != this) {
        node->installEventFilter(this);
      }

      const QObjectList children = node->children();
      for (QObject* child : children) {
        pending.push_back(child);
      }
    }
  }

  void markVisibleSurfaceCacheDirty() {
    invalidateVisibleSurfaceCache();
    if (hasAnyTrack() && isVisible()) {
      update();
    }
  }

  void invalidateVisibleSurfaceCache() { visibleSurfaceCacheDirty_ = true; }

  void refreshVisibleSurfaceCache() {
    if (!clipAgainstSurfaces_ || !hostWindow_ || !visibleSurfaceCacheDirty_) {
      return;
    }

    visibleSurfaceCache_.clear();
    const QList<QWidget*> surfaces = visibleSurfaceWidgets(hostWindow_);
    visibleSurfaceCache_.reserve(surfaces.size());
    for (QWidget* surface : surfaces) {
      visibleSurfaceCache_.append(surface);
    }
    visibleSurfaceCacheDirty_ = false;
  }

  QRectF focusTrackBaseRect(const FocusTrack& track) const {
    if (track.resolvedBaseRectValid) {
      return track.resolvedBaseRect;
    }
    return resolveFocusTrackRect(track);
  }

  QRectF resolveFocusTrackRect(const FocusTrack& track) const {
    if (!track.ownerBaseRectValid || !track.owner || !hostWindow_) {
      return track.request.baseRectInWindow;
    }
    if (!canMapWidgetToHostWindow(track.owner, hostWindow_)) {
      return QRectF();
    }

    QWidget* overlayParent = parentWidget();
    if (!overlayParent) {
      return track.request.baseRectInWindow;
    }

    const QPoint ownerOriginInHost = track.owner->mapTo(hostWindow_, QPoint(0, 0));
    const QRectF rectInHost =
        track.ownerBaseRect.translated(ownerOriginInHost.x(), ownerOriginInHost.y());
    return mapRectFromHostToOverlay(rectInHost, hostWindow_, overlayParent);
  }

  bool refreshDynamicFocusTrackRects() {
    if (!hostWindow_) {
      return false;
    }

    bool changed = false;
    for (auto it = focusTracks_.begin(); it != focusTracks_.end();) {
      FocusTrack& track = it.value();
      if (!track.ownerBaseRectValid || !track.owner) {
        ++it;
        continue;
      }
      if (!canMapWidgetToHostWindow(track.owner, hostWindow_)) {
        it = focusTracks_.erase(it);
        changed = true;
        continue;
      }

      const QRectF nextRect = resolveFocusTrackRect(track);
      if (!nextRect.isValid() || nextRect.width() <= 0.0 || nextRect.height() <= 0.0) {
        it = focusTracks_.erase(it);
        changed = true;
        continue;
      }
      if (!track.resolvedBaseRectValid ||
          !rectApproximatelyEqual(track.resolvedBaseRect, nextRect)) {
        track.resolvedBaseRect = nextRect;
        track.resolvedBaseRectValid = true;
        changed = true;
      }
      ++it;
    }
    return changed;
  }

  bool hasDynamicFocusTracks() const {
    if (!hostWindow_) {
      return false;
    }
    for (auto it = focusTracks_.cbegin(); it != focusTracks_.cend(); ++it) {
      const FocusTrack& track = it.value();
      if (track.ownerBaseRectValid && track.owner) {
        return true;
      }
    }
    return false;
  }

  void advanceInteractionWaveFrame() {
    const bool staleChanged = pruneStaleTracks();
    const bool finishedChanged = pruneFinishedWaveTracks();
    const bool changed = staleChanged || finishedChanged;
    if (!hasAnyTrack()) {
      refreshOverlayState();
      return;
    }

    const bool focusMoved = refreshDynamicFocusTrackRects();
    const bool needsRepaint = changed || !waveTracks_.isEmpty() || focusMoved ||
                              (clipAgainstSurfaces_ && visibleSurfaceCacheDirty_);

    syncGeometryToParent();
    refreshZOrder();
    if (needsRepaint) {
      update();
    }
    refreshFrameSubscription();
  }

  bool pruneStaleTracks() {
    bool changed = false;
    for (auto it = waveTracks_.begin(); it != waveTracks_.end();) {
      if (!it.value().owner) {
        it = waveTracks_.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }

    for (auto it = focusTracks_.begin(); it != focusTracks_.end();) {
      if (!it.value().owner) {
        it = focusTracks_.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    return changed;
  }

  bool pruneFinishedWaveTracks() {
    const int totalWaveDurationMs = std::max(0, detail::waveDurationMs());
    if (totalWaveDurationMs <= 0) {
      if (waveTracks_.isEmpty()) {
        return false;
      }
      waveTracks_.clear();
      return true;
    }

    bool changed = false;
    const qint64 nowMs = detail::timingNowMs();
    for (auto it = waveTracks_.begin(); it != waveTracks_.end();) {
      if (nowMs - it.value().startMs >= totalWaveDurationMs) {
        it = waveTracks_.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    return changed;
  }

  bool hasAnyTrack() const { return !waveTracks_.isEmpty() || !focusTracks_.isEmpty(); }

  void refreshFrameSubscription() {
    if (waveTracks_.isEmpty() && !hasDynamicFocusTracks()) {
      detail::clearFrameSubscription(this, QString::fromLatin1(kWaveFrameKey));
      return;
    }

    detail::setFrameSubscription(this, QString::fromLatin1(kWaveFrameKey), true,
                                 [this](qint64, qint64) { advanceInteractionWaveFrame(); });
  }

  void syncGeometryToParent() {
    QWidget* parent = parentWidget();
    if (!parent) {
      return;
    }
    const QRect parentRect = parent->rect();
    if (geometry() != parentRect) {
      setGeometry(parentRect);
    }
  }

  void refreshZOrder() {
    if (!isVisible()) {
      return;
    }

    QWidget* parent = parentWidget();
    if (!parent) {
      return;
    }

    if (clipAgainstSurfaces_) {
      QWidget* topSurface = topVisibleSurfaceSibling(parent);
      if (topSurface && topSurface != this) {
        stackUnder(topSurface);
        return;
      }
    }
    raise();
  }

  void refreshOverlayState() {
    pruneStaleTracks();
    pruneFinishedWaveTracks();
    refreshFrameSubscription();

    if (!hasAnyTrack()) {
      hide();
      return;
    }

    syncGeometryToParent();
    if (!isVisible()) {
      show();
    }
    refreshZOrder();
    update();
  }

  QPointer<QWidget> hostWindow_;
  bool clipAgainstSurfaces_ = false;
  bool visibleSurfaceCacheDirty_ = true;
  QSet<const QObject*> observedObjects_;
  QList<QPointer<QWidget>> visibleSurfaceCache_;
  QHash<const QWidget*, WaveTrack> waveTracks_;
  QHash<const QWidget*, FocusTrack> focusTracks_;
};

struct HostOverlays {
  QPointer<SharedInteractionOverlay> baseOverlay;
  QHash<QWidget*, QPointer<SharedInteractionOverlay>> surfaceOverlays;
};

QHash<QWidget*, HostOverlays>& interactionOverlayMap() {
  static QHash<QWidget*, HostOverlays> overlays;
  return overlays;
}

void pruneSurfaceOverlayEntries(HostOverlays& overlays) {
  for (auto it = overlays.surfaceOverlays.begin(); it != overlays.surfaceOverlays.end();) {
    if (!it.key() || !it.value()) {
      it = overlays.surfaceOverlays.erase(it);
    } else {
      ++it;
    }
  }
}

HostOverlays& ensureHostOverlays(QWidget* hostWindow) {
  auto& overlays = interactionOverlayMap();
  if (!overlays.contains(hostWindow)) {
    overlays.insert(hostWindow, HostOverlays());
    QObject::connect(hostWindow, &QObject::destroyed,
                     [hostWindow]() { interactionOverlayMap().remove(hostWindow); });
  }

  HostOverlays& hostOverlays = overlays[hostWindow];
  pruneSurfaceOverlayEntries(hostOverlays);
  return hostOverlays;
}

SharedInteractionOverlay* ensureBaseOverlay(QWidget* hostWindow) {
  if (!hostWindow) {
    return nullptr;
  }

  HostOverlays& hostOverlays = ensureHostOverlays(hostWindow);
  if (hostOverlays.baseOverlay) {
    return hostOverlays.baseOverlay;
  }

  auto* overlay = new SharedInteractionOverlay(hostWindow, hostWindow, true);
  hostOverlays.baseOverlay = overlay;
  return overlay;
}

SharedInteractionOverlay* ensureSurfaceOverlay(QWidget* hostWindow, QWidget* surfaceRoot) {
  if (!hostWindow || !surfaceRoot || surfaceRoot == hostWindow) {
    return ensureBaseOverlay(hostWindow);
  }

  HostOverlays& hostOverlays = ensureHostOverlays(hostWindow);
  const auto existing = hostOverlays.surfaceOverlays.constFind(surfaceRoot);
  if (existing != hostOverlays.surfaceOverlays.constEnd() && existing.value()) {
    return existing.value();
  }

  auto* overlay = new SharedInteractionOverlay(surfaceRoot, hostWindow, false);
  hostOverlays.surfaceOverlays.insert(surfaceRoot, overlay);

  QObject::connect(surfaceRoot, &QObject::destroyed, hostWindow, [hostWindow](QObject* destroyed) {
    QWidget* surface = qobject_cast<QWidget*>(destroyed);
    if (!surface) {
      return;
    }
    auto it = interactionOverlayMap().find(hostWindow);
    if (it == interactionOverlayMap().end()) {
      return;
    }
    it->surfaceOverlays.remove(surface);
  });

  return overlay;
}

SharedInteractionOverlay* resolveOverlayForOwner(const QWidget* owner,
                                                 const QRectF& rectInHostWindow,
                                                 QRectF* outRectInOverlay) {
  QWidget* hostWindow = resolveInteractionHostWindow(owner);
  if (!hostWindow) {
    return nullptr;
  }

  const QWidget* surfaceRoot = resolveInteractionSurfaceRoot(owner, hostWindow);
  if (surfaceRoot && surfaceRoot != hostWindow) {
    if (outRectInOverlay) {
      *outRectInOverlay = mapRectFromHostToOverlay(rectInHostWindow, hostWindow, surfaceRoot);
    }
    return ensureSurfaceOverlay(hostWindow, const_cast<QWidget*>(surfaceRoot));
  }

  if (outRectInOverlay) {
    *outRectInOverlay = rectInHostWindow;
  }
  return ensureBaseOverlay(hostWindow);
}

}  // namespace

void triggerInteractionWave(const InteractionWaveRequest& request) {
  if (!isValidInteractionWaveRequest(request)) {
    return;
  }

  QRectF rectInOverlay;
  SharedInteractionOverlay* overlay =
      resolveOverlayForOwner(request.owner, request.baseRectInWindow, &rectInOverlay);
  if (!overlay) {
    return;
  }

  overlay->triggerInteractionWave(request, rectInOverlay);
}

void stopInteractionWaveForOwner(const QWidget* owner) {
  if (!owner) {
    return;
  }

  auto& overlays = interactionOverlayMap();
  for (auto it = overlays.begin(); it != overlays.end(); ++it) {
    HostOverlays& hostOverlays = it.value();
    if (hostOverlays.baseOverlay) {
      hostOverlays.baseOverlay->stopInteractionWaveIfOwner(owner);
    }
    for (auto surfaceIt = hostOverlays.surfaceOverlays.begin();
         surfaceIt != hostOverlays.surfaceOverlays.end(); ++surfaceIt) {
      if (surfaceIt.value()) {
        surfaceIt.value()->stopInteractionWaveIfOwner(owner);
      }
    }
  }
}

void triggerInteractionFocus(const InteractionFocusRequest& request) {
  if (!isValidInteractionFocusRequest(request)) {
    return;
  }

  QRectF rectInOverlay;
  SharedInteractionOverlay* overlay =
      resolveOverlayForOwner(request.owner, request.baseRectInWindow, &rectInOverlay);
  if (!overlay) {
    return;
  }

  overlay->triggerInteractionFocus(request, rectInOverlay);
}

void stopInteractionFocusForOwner(const QWidget* owner) {
  if (!owner) {
    return;
  }

  auto& overlays = interactionOverlayMap();
  for (auto it = overlays.begin(); it != overlays.end(); ++it) {
    HostOverlays& hostOverlays = it.value();
    if (hostOverlays.baseOverlay) {
      hostOverlays.baseOverlay->stopInteractionFocusIfOwner(owner);
    }
    for (auto surfaceIt = hostOverlays.surfaceOverlays.begin();
         surfaceIt != hostOverlays.surfaceOverlays.end(); ++surfaceIt) {
      if (surfaceIt.value()) {
        surfaceIt.value()->stopInteractionFocusIfOwner(owner);
      }
    }
  }
}

}  // namespace adqt::widgets
