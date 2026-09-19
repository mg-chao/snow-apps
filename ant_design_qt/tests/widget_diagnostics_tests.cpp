#include "theme/theme.h"
#include "widgets/alert.h"
#include "widgets/form.h"
#include "widgets/navigation_menu.h"

#include <QAccessible>
#include <QApplication>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>

using namespace adqt::widgets;

namespace {
QHash<QObject*, int> publishedRows;

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

void recordAccessibilityUpdate(QAccessibleEvent* event) {
  if (event->type() != QAccessible::TableModelChanged) {
    return;
  }
  auto* interface = event->accessibleInterface();
  if (auto* table = interface ? interface->tableInterface() : nullptr) {
    publishedRows.insert(event->object(), table->rowCount());
  }
}

void requiredMarkUsesLabelFont() {
  AdFormItem item;
  item.setLabel(QStringLiteral("Name"));
  item.setRequired(true);
  auto* mark = item.findChild<QLabel*>(QStringLiteral("ad-form-item-required-mark"));
  auto* label = item.findChild<QLabel*>(QStringLiteral("ad-form-item-label"));
  require(mark && label, "Form label and required mark must exist");
  require(mark->font() == label->font(), "Required mark must use the theme label font");
  require(mark->text() == QStringLiteral("*"), "Required mark must remain an asterisk");
  require(!mark->isHidden(), "Required mark must remain visible for required fields");
}

void menuPublishesRowsBeforeSelection() {
  const bool wasActive = QAccessible::isActive();
  QAccessible::setActive(true);
  const auto previousHandler = QAccessible::installUpdateHandler(recordAccessibilityUpdate);
  for (auto mode : {AdNavigationMenu::Mode::Inline, AdNavigationMenu::Mode::Vertical,
                    AdNavigationMenu::Mode::Horizontal}) {
    QStandardItemModel model;
    model.appendRow(new QStandardItem(QStringLiteral("First")));
    model.appendRow(new QStandardItem(QStringLiteral("Second")));
    AdNavigationMenu menu;
    menu.setMode(mode);
    publishedRows.clear();
    menu.setModel(&model);
    const auto trees = menu.findChildren<QTreeView*>();
    require(trees.size() == 2, "Menu must have inline and vertical tree views");
    for (auto* tree : trees) {
      require(publishedRows.value(tree, -1) == 2,
              "Menu must publish populated rows before returning from setModel");
    }
    menu.selectionModel()->select(model.index(0, 0), QItemSelectionModel::ClearAndSelect);
    menu.setCurrentIndex(model.index(0, 0));
    model.appendRow(new QStandardItem(QStringLiteral("Third")));
    for (auto* tree : trees) {
      require(publishedRows.value(tree, -1) == 3,
              "Inserted rows must reach accessibility before the next selection");
    }
    model.clear();
    for (auto* tree : trees) {
      require(publishedRows.value(tree, -1) == 0, "Cleared menus must publish zero rows");
    }
  }
  QAccessible::installUpdateHandler(previousHandler);
  QAccessible::setActive(wasActive);
}

void ancestorPaletteDoesNotOverrideSemanticAlertStyle() {
  QWidget parent;
  auto* layout = new QVBoxLayout(&parent);
  QPalette palette = parent.palette();
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#ff00ff")));
  palette.setColor(QPalette::Mid, QColor(QStringLiteral("#00ff00")));
  palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#0000ff")));
  parent.setPalette(palette);

  auto* alert = new AdAlert(&parent);
  alert->setSeverity(AdAlert::Severity::Warning);
  alert->setText(QStringLiteral("Permissions needed"));
  alert->setInformativeText(QStringLiteral("Review access in System Settings."));
  layout->addWidget(alert);
  parent.resize(480, 160);
  parent.show();
  QApplication::processEvents();

  const QImage rendered = alert->grab().toImage();
  const qreal scale = rendered.devicePixelRatio();
  const QPoint sample(qRound((alert->width() - 8) * scale), qRound(alert->height() / 2.0 * scale));
  const QColor actual = rendered.pixelColor(sample);
  const QColor expected =
      adqt::theme::ThemeManager::instance().resolve(alert).theme.palette.colorWarningBg;
  require(actual == expected,
          "An ancestor palette must not replace an alert's semantic warning background");
}

void descriptiveAlertUsesBalancedPadding() {
  AdAlert alert;
  alert.setText(QStringLiteral("Permissions needed"));
  alert.setInformativeText(QStringLiteral("Review access in System Settings."));
  const QMargins margins = alert.layout()->contentsMargins();
  require(margins.left() == margins.top() && margins.top() == margins.right() &&
              margins.right() == margins.bottom(),
          "Alerts with informative text must use balanced padding on every edge");
}
}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  requiredMarkUsesLabelFont();
  menuPublishesRowsBeforeSelection();
  ancestorPaletteDoesNotOverrideSemanticAlertStyle();
  descriptiveAlertUsesBalancedPadding();
  return EXIT_SUCCESS;
}
