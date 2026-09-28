#include "widgets/modal.h"
#include "widgets/popover.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QEnterEvent>
#include <QPushButton>
#include <QTest>
#include <QWindow>

using adqt::widgets::AdModal;
using adqt::widgets::AdPopover;
using adqt::widgets::AdTooltip;

namespace {

void enter(QWidget* target) {
  const QPoint local = target->rect().center();
  QEnterEvent event(local, target->mapTo(target->window(), local), target->mapToGlobal(local));
  QApplication::sendEvent(target, &event);
}

void prepareTooltip(AdTooltip& tooltip, QWidget* target) {
  tooltip.setTargetWidget(target);
  tooltip.setText(QStringLiteral("Hover details"));
  tooltip.setHoverOpenDelayMs(0);
  tooltip.setHoverCloseDelayMs(0);
}

bool hoverOpens(AdTooltip& tooltip) {
  tooltip.hide();
  enter(tooltip.targetWidget());
  return tooltip.isVisible();
}

}  // namespace

class ModalPopupInteractionTest final : public QObject {
  Q_OBJECT

 private slots:
  void tooltipInsideModal_data() {
    QTest::addColumn<Qt::WindowModality>("modality");
    QTest::addColumn<bool>("transientTooltip");
    QTest::newRow("owner-embedded") << Qt::WindowModal << false;
    QTest::newRow("owner-transient") << Qt::WindowModal << true;
    QTest::newRow("application-embedded") << Qt::ApplicationModal << false;
    QTest::newRow("application-transient") << Qt::ApplicationModal << true;
  }

  void tooltipInsideModal() {
    QFETCH(Qt::WindowModality, modality);
    QFETCH(bool, transientTooltip);
    QWidget owner;
    owner.show();
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    modal.setWindowModality(modality);
    auto* target = new QPushButton(QStringLiteral("Details"));
    modal.setContentWidget(target);
    AdTooltip tooltip;
    prepareTooltip(tooltip, target);
    if (transientTooltip) tooltip.setLayerMode(AdTooltip::LayerMode::TopLevelTransient);

    for (int session = 0; session < 2; ++session) {
      modal.open();
      QVERIFY(QTest::qWaitForWindowExposed(target->window()));
      QVERIFY(hoverOpens(tooltip));
      modal.reject();
      QVERIFY(!tooltip.isVisible());
    }
  }

  void hoverPopoverAndItsTooltipRemainInteractive() {
    QWidget owner;
    owner.show();
    AdModal modal(&owner);
    modal.setMode(AdModal::Mode::Window);
    auto* trigger = new QPushButton(QStringLiteral("Options"));
    modal.setContentWidget(trigger);
    modal.open();
    QVERIFY(QTest::qWaitForWindowExposed(trigger->window()));

    AdPopover popover;
    popover.setSourceWidget(trigger);
    popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popover.setTriggers(AdPopover::Trigger::Hover);
    popover.setHoverOpenDelayMs(0);
    popover.setHoverCloseDelayMs(0);
    auto* option = new QPushButton(QStringLiteral("Option"));
    popover.setContentWidget(option);
    AdTooltip tooltip;
    prepareTooltip(tooltip, option);

    enter(trigger);
    QVERIFY(popover.isVisible());
    QVERIFY(option->window() != trigger->window());
    QVERIFY(hoverOpens(tooltip));
    QVERIFY(popover.isVisible());
    modal.reject();
    QVERIFY(!popover.isVisible());
    QVERIFY(!tooltip.isVisible());
  }

  void independentWindowModalsKeepTheirOwnHoverScopes() {
    QWidget firstOwner;
    QWidget secondOwner;
    QWidget unrelated;
    firstOwner.show();
    secondOwner.show();
    unrelated.show();
    AdModal first(&firstOwner);
    AdModal second(&secondOwner);
    first.setMode(AdModal::Mode::Window);
    second.setMode(AdModal::Mode::Window);
    auto* firstTarget = new QPushButton(QStringLiteral("First dialog"));
    auto* secondTarget = new QPushButton(QStringLiteral("Second dialog"));
    first.setContentWidget(firstTarget);
    second.setContentWidget(secondTarget);
    first.open();
    second.open();
    QVERIFY(QTest::qWaitForWindowExposed(secondTarget->window()));

    AdTooltip tooltip;
    prepareTooltip(tooltip, firstTarget);
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(secondTarget);
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(&unrelated);
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(&firstOwner);
    QVERIFY(!hoverOpens(tooltip));
    tooltip.setTargetWidget(&secondOwner);
    QVERIFY(!hoverOpens(tooltip));
    second.reject();
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(&firstOwner);
    QVERIFY(!hoverOpens(tooltip));
    first.reject();
    QVERIFY(hoverOpens(tooltip));
  }

  void nestedDialogsAndModalityChangesRestoreHover() {
    QWidget owner;
    QWidget unrelated;
    owner.show();
    unrelated.show();
    AdModal outer(&owner);
    outer.setMode(AdModal::Mode::Window);
    auto* outerTarget = new QPushButton(QStringLiteral("Outer dialog"));
    outer.setContentWidget(outerTarget);
    outer.open();
    AdModal inner(outerTarget->window());
    inner.setMode(AdModal::Mode::Window);
    auto* innerTarget = new QPushButton(QStringLiteral("Inner dialog"));
    inner.setContentWidget(innerTarget);
    inner.open();
    QVERIFY(QTest::qWaitForWindowExposed(innerTarget->window()));

    AdTooltip tooltip;
    prepareTooltip(tooltip, outerTarget);
    QVERIFY(!hoverOpens(tooltip));
    tooltip.setTargetWidget(innerTarget);
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(&owner);
    QVERIFY(!hoverOpens(tooltip));
    inner.reject();
    QVERIFY(!hoverOpens(tooltip));
    tooltip.setTargetWidget(outerTarget);
    QVERIFY(hoverOpens(tooltip));

    outer.setWindowModality(Qt::ApplicationModal);
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(&unrelated);
    QVERIFY(!hoverOpens(tooltip));
    outer.setWindowModality(Qt::WindowModal);
    QVERIFY(hoverOpens(tooltip));
    tooltip.setTargetWidget(&owner);
    QVERIFY(!hoverOpens(tooltip));
    outer.setWindowModality(Qt::NonModal);
    QVERIFY(hoverOpens(tooltip));
  }

  void modalQWindowWithoutWidgetStillBlocksHover() {
    QWidget owner;
    QWidget unrelated;
    owner.show();
    unrelated.show();
    QWindow modal;
    modal.setTransientParent(owner.windowHandle());
    modal.setModality(Qt::WindowModal);
    modal.resize(100, 100);
    modal.show();
    QVERIFY(QTest::qWaitForWindowExposed(&modal));

    AdTooltip tooltip;
    prepareTooltip(tooltip, &owner);
    QVERIFY(!hoverOpens(tooltip));
    tooltip.setTargetWidget(&unrelated);
    QVERIFY(hoverOpens(tooltip));
    modal.hide();
    tooltip.setTargetWidget(&owner);
    QVERIFY(hoverOpens(tooltip));
  }

  void popupGrabStillRestrictsHover() {
    QWidget owner;
    owner.show();
    QWidget popup(&owner, Qt::Popup);
    QPushButton target(QStringLiteral("Popup option"), &popup);
    popup.show();
    QVERIFY(QApplication::activePopupWidget() == &popup);
    AdTooltip tooltip;
    prepareTooltip(tooltip, &owner);
    QVERIFY(!hoverOpens(tooltip));
    tooltip.setTargetWidget(&target);
    QVERIFY(hoverOpens(tooltip));
    popup.hide();
    tooltip.setTargetWidget(&owner);
    QVERIFY(hoverOpens(tooltip));
  }
};

QTEST_MAIN(ModalPopupInteractionTest)
#include "modal_popup_interaction_tests.moc"
