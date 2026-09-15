#include "widgets/form.h"
#include "widgets/navigation_menu.h"

#include <QAccessible>
#include <QApplication>
#include <QHash>
#include <QLabel>
#include <QStandardItemModel>
#include <QTreeView>

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
}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  requiredMarkUsesLabelFont();
  menuPublishesRowsBeforeSelection();
  return EXIT_SUCCESS;
}
