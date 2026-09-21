#include "control_scale.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLayout>
#include <QScopeGuard>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <limits>

namespace adqt::widgets {
namespace {

constexpr auto kScaleContextProperty = "_adqt_control_scale_context";

QVariant contextVariant(const AdControlScaleContext& context) {
  return QVariant::fromValue(context);
}

}  // namespace

qreal AdControlScaleContext::normalizeScale(qreal value) {
  if (!std::isfinite(value) || value <= 0.0) {
    return 1.0;
  }
  return std::clamp<qreal>(value, 0.25, 4.0);
}

qreal AdControlScaleContext::normalizeDpr(qreal value) {
  if (!std::isfinite(value) || value <= 0.0) {
    return 1.0;
  }
  return value;
}

AdControlScaleContext AdControlScaleContext::fromDprs(qreal reference, qreal current,
                                                      quint64 revision) {
  return fromDprsAndContentScale(reference, current, 1.0, revision);
}

AdControlScaleContext AdControlScaleContext::fromDprsAndContentScale(qreal reference, qreal current,
                                                                     qreal contentScale,
                                                                     quint64 revision) {
  AdControlScaleContext result;
  result.referenceDpr = normalizeDpr(reference);
  result.currentDpr = normalizeDpr(current);
  result.contentScale = normalizeScale(contentScale);
  result.logicalScale =
      normalizeScale(result.referenceDpr / result.currentDpr * result.contentScale);
  result.revision = revision;
  return result;
}

bool AdControlScaleContext::equivalentTo(const AdControlScaleContext& other) const {
  return qFuzzyCompare(referenceDpr + 1.0, other.referenceDpr + 1.0) &&
         qFuzzyCompare(currentDpr + 1.0, other.currentDpr + 1.0) &&
         qFuzzyCompare(contentScale + 1.0, other.contentScale + 1.0) &&
         qFuzzyCompare(logicalScale + 1.0, other.logicalScale + 1.0);
}

AdControlScaleScope::AdControlScaleScope(QWidget* root, QObject* parent)
    : QObject(parent ? parent : root), root_(root) {
  qRegisterMetaType<AdControlScaleContext>();
  if (root_) {
    root_->setProperty(kScaleContextProperty, contextVariant(context_));
  }
}

QWidget* AdControlScaleScope::rootWidget() const { return root_; }

AdControlScaleContext AdControlScaleScope::context() const { return context_; }

QSize AdControlScaleScope::logicalClientExtent() const { return logicalClientExtent_; }

bool AdControlScaleScope::publishScale(qreal referenceDpr, qreal currentDpr,
                                       const QSize& logicalClientExtent) {
  return publishScale(
      AdControlScaleContext::fromDprs(referenceDpr, currentDpr, context_.revision + 1),
      logicalClientExtent);
}

bool AdControlScaleScope::publishScale(const AdControlScaleContext& requested,
                                       const QSize& logicalClientExtent) {
  if (!root_) return false;
  if (publishing_) {
    pendingContext_ = requested;
    pendingExtent_ = logicalClientExtent;
    pending_ = true;
    return false;
  }
  const auto next = AdControlScaleContext::fromDprsAndContentScale(
      requested.referenceDpr, requested.currentDpr, requested.contentScale, context_.revision + 1);
  if (context_.equivalentTo(next) && logicalClientExtent_ == logicalClientExtent) return false;
  publishing_ = true;
  context_ = next;
  logicalClientExtent_ = logicalClientExtent;
  root_->setProperty(kScaleContextProperty, contextVariant(context_));
  QPointer<AdControlScaleScope> guard(this);
  applyScale(root_);
  if (!guard) return true;
  emit scaleCommitted(context_, logicalClientExtent_);
  if (!guard) return true;
  publishing_ = false;
  if (pending_) {
    const auto pendingContext = pendingContext_;
    const auto pendingExtent = pendingExtent_;
    pending_ = false;
    publishScale(pendingContext, pendingExtent);
  }
  return true;
}

AdControlScaleScope::~AdControlScaleScope() {
  if (root_) root_->setProperty(kScaleContextProperty, QVariant());
}

bool AdControlScaleScope::ownsWidget(const QWidget* widget) const {
  for (const QWidget* current = widget; current; current = current->parentWidget()) {
    if (current == root_) return true;
    if (current->isWindow() || current->property(kScaleContextProperty).isValid()) return false;
  }
  return false;
}

void AdControlScaleScope::setSubtreeDeferred(QWidget* subtree, bool deferred) {
  deferredSubtrees_.erase(
      std::remove_if(deferredSubtrees_.begin(), deferredSubtrees_.end(),
                     [subtree](const auto& item) { return !item || item == subtree; }),
      deferredSubtrees_.end());
  if (deferred && subtree && subtree != root_ && ownsWidget(subtree))
    deferredSubtrees_.append(subtree);
}

bool AdControlScaleScope::isDeferred(const QWidget* widget, const QWidget* subtree) const {
  for (const QWidget* current = widget; current && current != subtree;
       current = current->parentWidget()) {
    for (const auto& deferred : deferredSubtrees_)
      if (deferred == current) return true;
  }
  return false;
}

QList<QPointer<QWidget>> AdControlScaleScope::widgetsInSubtree(QWidget* subtree) const {
  QList<QPointer<QWidget>> widgets;
  if (!subtree || !ownsWidget(subtree)) return widgets;
  widgets.append(subtree);
  for (QWidget* child : subtree->findChildren<QWidget*>()) {
    if (ownsWidget(child)) widgets.append(child);
  }
  return widgets;
}

void AdControlScaleScope::applyScale(QWidget* subtree) {
  QPointer<AdControlScaleScope> guard(this);
  QPointer<QWidget> guardedRoot(subtree);
  auto widgets = widgetsInSubtree(subtree);
  if (widgets.isEmpty()) return;
  const bool updatesWereEnabled = subtree->updatesEnabled();
  if (updatesWereEnabled) subtree->setUpdatesEnabled(false);
  const auto restoreUpdates = qScopeGuard([guardedRoot, updatesWereEnabled]() {
    if (guardedRoot && updatesWereEnabled) {
      guardedRoot->setUpdatesEnabled(true);
      if (guardedRoot->window()->updatesEnabled()) guardedRoot->update();
    }
  });
  const auto context = context_;
  QList<QPointer<QWidget>> visited;
  while (!widgets.isEmpty()) {
    for (const auto& widget : widgets) {
      if (!widget || !ownsWidget(widget) || isDeferred(widget, subtree)) continue;
      if (auto* participant = dynamic_cast<AdControlScaleParticipant*>(widget.data()))
        participant->prepareControlScale(context);
      if (!guard) return;
    }
    for (const auto& widget : widgets) {
      if (!widget || !ownsWidget(widget) || isDeferred(widget, subtree)) continue;
      if (auto* participant = dynamic_cast<AdControlScaleParticipant*>(widget.data()))
        participant->commitControlScale(context);
      if (!guard) return;
    }
    visited.append(widgets);
    widgets.clear();
    if (!guardedRoot) return;
    // A participant may replace its subtree during commit. The replacement
    // must receive both phases before layout or the first resumed paint.
    for (const auto& widget : widgetsInSubtree(subtree)) {
      if (!visited.contains(widget)) widgets.append(widget);
    }
  }
  if (!guardedRoot) return;
  if (subtree == root_ && logicalClientExtent_.isValid() && !logicalClientExtent_.isEmpty())
    subtree->resize(logicalClientExtent_);
  // Work from leaves to containers, including children rebuilt by a commit. Never
  // discard unrelated LayoutRequest events or cross popup/nested-scope boundaries.
  const auto liveWidgets = widgetsInSubtree(subtree);
  for (auto it = liveWidgets.crbegin(); it != liveWidgets.crend(); ++it) {
    if (!*it || !ownsWidget(*it) || isDeferred(*it, subtree)) continue;
    if (auto* participant = dynamic_cast<AdControlScaleParticipant*>(it->data()))
      participant->finishControlScale(context);
    if (!guard) return;
    if (*it && (*it)->layout()) {
      (*it)->layout()->invalidate();
      (*it)->layout()->activate();
      if (!guard) return;
    }
  }
}

bool AdControlScaleScope::applyCurrentScaleToSubtree(QWidget* subtree) {
  if (!subtree || !ownsWidget(subtree)) return false;
  applyScale(subtree);
  return true;
}

int scaleControlMetric(int reference, qreal scale, int minimum) {
  return reference <= 0 ? 0 : qMax(minimum, qRound(reference * scale));
}

QSize scaleControlSize(const QSize& reference, qreal scale) {
  return QSize(scaleControlMetric(reference.width(), scale),
               scaleControlMetric(reference.height(), scale));
}

QFont scaleControlFont(const QFont& reference, qreal scale) {
  QFont font = reference;
  if (font.pixelSize() > 0)
    font.setPixelSize(scaleControlMetric(font.pixelSize(), scale));
  else if (font.pointSizeF() > 0.0)
    font.setPointSizeF(font.pointSizeF() * scale);
  return font;
}

QVector<int> scaleCumulativeEdges(const QVector<qreal>& referenceEdges, qreal logicalScale,
                                  int targetExtent) {
  QVector<int> result;
  result.reserve(referenceEdges.size());
  const qreal scale = AdControlScaleContext::normalizeScale(logicalScale);
  int previous = std::numeric_limits<int>::min();
  for (qreal edge : referenceEdges) {
    int scaled = qRound(edge * scale);
    if (previous != std::numeric_limits<int>::min()) {
      scaled = std::max(previous, scaled);
    }
    result.append(scaled);
    previous = scaled;
  }
  if (!result.isEmpty() && targetExtent >= 0) {
    result.last() = targetExtent;
    for (qsizetype index = result.size() - 1; index > 0; --index) {
      if (result.at(index - 1) > result.at(index)) {
        result[index - 1] = result.at(index);
      }
    }
  }
  return result;
}

QVector<int> scaleCumulativeWidths(const QVector<int>& referenceWidths, qreal logicalScale,
                                   int targetExtent) {
  QVector<qreal> edges;
  edges.reserve(referenceWidths.size() + 1);
  qreal edge = 0.0;
  edges.append(edge);
  for (int width : referenceWidths) {
    edge += std::max(0, width);
    edges.append(edge);
  }
  return scaleCumulativeEdges(edges, logicalScale, targetExtent);
}

AdControlScaleContext controlScaleContextFor(const QWidget* widget) {
  const QWidget* current = widget;
  while (current) {
    const QVariant value = current->property(kScaleContextProperty);
    if (value.isValid() && value.canConvert<AdControlScaleContext>()) {
      return value.value<AdControlScaleContext>();
    }
    if (current->isWindow()) break;
    current = current->parentWidget();
  }
  return AdControlScaleContext();
}

}  // namespace adqt::widgets
