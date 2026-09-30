#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

#include "widgets/progress.h"

class QLabel;
class QVBoxLayout;

class ProgressDocsPage final : public QWidget {
 public:
  explicit ProgressDocsPage(QWidget* parent = nullptr);

  static const QStringList& defaultSectionTitles();
  const QVector<QWidget*>& sectionAnchors() const;
  const QStringList& sectionTitles() const;

 protected:
  void changeEvent(QEvent* event) override;

 private:
  void addSection(QVBoxLayout* root, int index, QWidget* content);
  QLabel* label(const char* source, bool hint = false);
  void bindAccessibleText(QWidget* widget, const char* source);
  adqt::widgets::AdProgress* progress(
      qreal percent, adqt::widgets::AdProgress::Type type = adqt::widgets::AdProgress::Type::Line);

  QWidget* buildLineDemo(bool small);
  QWidget* buildCircleDemo(bool small);
  QWidget* buildDynamicDemo();
  QWidget* buildFormatDemo();
  QWidget* buildDashboardDemo();
  QWidget* buildSuccessDemo();
  QWidget* buildLinecapDemo();
  QWidget* buildGradientDemo();
  QWidget* buildStepsDemo();
  QWidget* buildCircleStepsDemo();
  QWidget* buildSizeDemo();
  QWidget* buildInfoPositionDemo();
  QWidget* buildTokenDemo();
  QWidget* buildSemanticDemo();

  QVector<QWidget*> anchors_;
  QStringList titles_;
  QVector<std::function<void()>> translations_;
};
