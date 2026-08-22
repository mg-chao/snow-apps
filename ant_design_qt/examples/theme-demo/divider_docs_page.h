#pragma once

#include <QVector>
#include <QStringList>
#include <QWidget>

class QVBoxLayout;

class DividerDocsPage final : public QWidget {
 public:
  explicit DividerDocsPage(QWidget* parent = nullptr);

  const QVector<QWidget*>& sectionAnchors() const;
  const QStringList& sectionTitles() const;

 private:
  void addSection(QVBoxLayout* root, const QString& title, const QString& description,
                  QWidget* content);
  QWidget* buildHorizontalDemo();
  QWidget* buildWithTextDemo();
  QWidget* buildSizeDemo();
  QWidget* buildPlainDemo();
  QWidget* buildVerticalDemo();
  QWidget* buildVariantDemo();
  QWidget* buildTokenDemo();
  QWidget* buildSemanticDemo();

  QVector<QWidget*> anchors_;
  QStringList titles_;
};
