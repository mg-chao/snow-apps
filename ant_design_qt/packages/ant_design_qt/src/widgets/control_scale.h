#pragma once

#include <QList>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QVector>

class QWidget;

namespace adqt::widgets {

struct AdControlScaleContext {
  qreal referenceDpr = 1.0;
  qreal currentDpr = 1.0;
  qreal contentScale = 1.0;
  qreal logicalScale = 1.0;
  quint64 revision = 0;

  static qreal normalizeScale(qreal value);
  static qreal normalizeDpr(qreal value);
  static AdControlScaleContext fromDprs(qreal referenceDpr, qreal currentDpr, quint64 revision = 0);
  static AdControlScaleContext fromDprsAndContentScale(qreal referenceDpr, qreal currentDpr,
                                                       qreal contentScale, quint64 revision = 0);

  bool equivalentTo(const AdControlScaleContext& other) const;
};

class AdControlScaleParticipant {
 public:
  virtual ~AdControlScaleParticipant() = default;

  // Prepare may only invalidate internal caches. Geometry and paint requests
  // belong in commit, after every participant has observed the new context.
  virtual void prepareControlScale(const AdControlScaleContext& context) = 0;
  virtual void commitControlScale(const AdControlScaleContext& context) = 0;
  // Containers finish layout only after descendants have committed their metrics.
  virtual void finishControlScale(const AdControlScaleContext&) {}
};

class AdControlScaleScope final : public QObject {
  Q_OBJECT

 public:
  explicit AdControlScaleScope(QWidget* root, QObject* parent = nullptr);
  ~AdControlScaleScope() override;

  QWidget* rootWidget() const;
  AdControlScaleContext context() const;
  QSize logicalClientExtent() const;

  bool publishScale(qreal referenceDpr, qreal currentDpr,
                    const QSize& logicalClientExtent = QSize());
  bool publishScale(const AdControlScaleContext& requested,
                    const QSize& logicalClientExtent = QSize());
  bool applyCurrentScaleToSubtree(QWidget* subtree);
  // Retained lazy editor rows receive the latest context explicitly on activation.
  void setSubtreeDeferred(QWidget* subtree, bool deferred);

 signals:
  void scaleCommitted(const adqt::widgets::AdControlScaleContext& context,
                      const QSize& logicalClientExtent);

 private:
  bool ownsWidget(const QWidget* widget) const;
  bool isDeferred(const QWidget* widget, const QWidget* subtree) const;
  QList<QPointer<QWidget>> widgetsInSubtree(QWidget* subtree) const;
  void applyScale(QWidget* subtree);

  QPointer<QWidget> root_;
  AdControlScaleContext context_;
  QSize logicalClientExtent_;
  QList<QPointer<QWidget>> deferredSubtrees_;
  bool publishing_ = false;
  bool pending_ = false;
  AdControlScaleContext pendingContext_;
  QSize pendingExtent_;
};

// Reference metrics are never overwritten with their scaled result.
int scaleControlMetric(int reference, qreal scale, int minimum = 1);
QSize scaleControlSize(const QSize& reference, qreal scale);
QFont scaleControlFont(const QFont& reference, qreal scale);

// Rounds absolute reference-coordinate boundaries. Supplying targetExtent
// forces the final edge to the native-derived logical client boundary.
QVector<int> scaleCumulativeEdges(const QVector<qreal>& referenceEdges, qreal logicalScale,
                                  int targetExtent = -1);
QVector<int> scaleCumulativeWidths(const QVector<int>& referenceWidths, qreal logicalScale,
                                   int targetExtent = -1);

AdControlScaleContext controlScaleContextFor(const QWidget* widget);

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdControlScaleContext)
