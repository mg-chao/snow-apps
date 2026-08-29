#pragma once

#include <QList>
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
                                                       qreal contentScale,
                                                       quint64 revision = 0);

  bool equivalentTo(const AdControlScaleContext& other) const;
};

class AdControlScaleParticipant {
 public:
  virtual ~AdControlScaleParticipant() = default;

  // Prepare may only invalidate internal caches. Geometry and paint requests
  // belong in commit, after every participant has observed the new context.
  virtual void prepareControlScale(const AdControlScaleContext& context) = 0;
  virtual void commitControlScale(const AdControlScaleContext& context) = 0;
};

class AdControlScaleScope final : public QObject {
  Q_OBJECT

 public:
  explicit AdControlScaleScope(QWidget* root, QObject* parent = nullptr);

  QWidget* rootWidget() const;
  AdControlScaleContext context() const;
  QSize logicalClientExtent() const;

  bool publishScale(qreal referenceDpr, qreal currentDpr,
                    const QSize& logicalClientExtent = QSize());
  bool publishScale(const AdControlScaleContext& requested,
                    const QSize& logicalClientExtent = QSize());
  bool applyCurrentScaleToSubtree(QWidget* subtree);

 signals:
  void scaleCommitted(const adqt::widgets::AdControlScaleContext& context,
                      const QSize& logicalClientExtent);

 private:
  QList<AdControlScaleParticipant*> participantsInSubtree(QWidget* subtree) const;

  QPointer<QWidget> root_;
  AdControlScaleContext context_;
  QSize logicalClientExtent_;
};

// Rounds absolute reference-coordinate boundaries. Supplying targetExtent
// forces the final edge to the native-derived logical client boundary.
QVector<int> scaleCumulativeEdges(const QVector<qreal>& referenceEdges, qreal logicalScale,
                                  int targetExtent = -1);
QVector<int> scaleCumulativeWidths(const QVector<int>& referenceWidths, qreal logicalScale,
                                   int targetExtent = -1);

AdControlScaleContext controlScaleContextFor(const QWidget* widget);

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdControlScaleContext)
