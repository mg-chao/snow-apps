#include <QApplication>
#include <QCoreApplication>
#include <QListView>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QWidget>

#include "widgets/date_picker.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"

using adqt::widgets::AdDatePicker;
using adqt::widgets::AdDateRangePicker;
using adqt::widgets::AdPopover;
using adqt::widgets::AdSelect;
using adqt::widgets::AdTooltip;

namespace {

AdSelect::Option makeOption(const QString& value, const QString& label) {
  AdSelect::Option option;
  option.value = value;
  option.label = label;
  return option;
}

QWidget* selectPopupSurface(AdSelect& select) {
  QWidget* candidate = select.view();
  while (candidate && candidate->objectName() != QStringLiteral("adselect-popup")) {
    candidate = candidate->parentWidget();
  }
  return candidate;
}

QWidget* findSurface(const QString& objectName) {
  for (QWidget* candidate : QApplication::allWidgets()) {
    if (candidate && candidate->objectName() == objectName) {
      return candidate;
    }
  }
  return nullptr;
}

void verifyNativeResourcesReleased(QWidget* surface) {
  QVERIFY(surface);
  QTRY_VERIFY(!surface->isVisible());
  QTRY_VERIFY(surface->windowHandle() == nullptr);
  QCOMPARE(surface->backingStore(), nullptr);
}

}  // namespace

class QtToolPopupTest final : public QObject {
  Q_OBJECT

 private slots:
  void popoverReleasesAndRecreatesNativeResources();
  void selectReleasesAndRecreatesNativeResources();
  void tooltipReleasesAndRecreatesNativeResources();
  void datePickerReleasesAndRecreatesNativeResources();
  void dateRangePickerReleasesAndRecreatesNativeResources();
  void recreateLifetimeStillDestroysPopupSurface();
};

void QtToolPopupTest::popoverReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);

  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setText(QStringLiteral("Popover content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QWidget* surface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  popover.hide();
  verifyNativeResourcesReleased(surface);

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCOMPARE(findSurface(QStringLiteral("adpopover-surface")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  // A close followed by an immediate reopen must cancel the deferred release.
  popover.hide();
  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QCoreApplication::processEvents();
  QVERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::selectReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  AdSelect select(&host);
  select.setGeometry(24, 24, 180, 32);
  select.setPopupLayerMode(AdSelect::PopupLayerMode::QtTool);
  select.setOptions({makeOption(QStringLiteral("one"), QStringLiteral("One")),
                     makeOption(QStringLiteral("two"), QStringLiteral("Two"))});

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  select.showPopup();
  QTRY_VERIFY(select.popupVisible());
  QWidget* surface = selectPopupSurface(select);
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  select.hidePopup();
  verifyNativeResourcesReleased(surface);

  select.showPopup();
  QTRY_VERIFY(select.popupVisible());
  QCOMPARE(selectPopupSurface(select), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::tooltipReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  auto* target = new QPushButton(QStringLiteral("Target"), &host);
  target->setGeometry(24, 24, 100, 32);

  AdTooltip tooltip;
  tooltip.setTargetWidget(target);
  tooltip.setLayerMode(AdTooltip::LayerMode::TopLevelTransient);
  tooltip.setTriggers(AdTooltip::Trigger::Click);
  tooltip.setText(QStringLiteral("Tooltip content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  tooltip.show();
  QTRY_VERIFY(tooltip.isVisible());
  QWidget* surface = findSurface(QStringLiteral("adtooltip-surface"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  tooltip.hide();
  verifyNativeResourcesReleased(surface);

  tooltip.show();
  QTRY_VERIFY(tooltip.isVisible());
  QCOMPARE(findSurface(QStringLiteral("adtooltip-surface")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::datePickerReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(640, 360);
  AdDatePicker picker(&host);
  picker.setGeometry(24, 24, 220, 32);
  picker.setPopupLayerMode(AdDatePicker::PopupLayerMode::QtTool);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QWidget* surface = findSurface(QStringLiteral("addatepicker-popup"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  picker.hidePopup();
  verifyNativeResourcesReleased(surface);

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QCOMPARE(findSurface(QStringLiteral("addatepicker-popup")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::dateRangePickerReleasesAndRecreatesNativeResources() {
  QWidget host;
  host.resize(760, 360);
  AdDateRangePicker picker(&host);
  picker.setGeometry(24, 24, 320, 32);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::QtTool);

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QWidget* surface = findSurface(QStringLiteral("addaterangepicker-popup"));
  QVERIFY(surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());

  picker.hidePopup();
  verifyNativeResourcesReleased(surface);

  picker.showPopup();
  QTRY_VERIFY(picker.popupVisible());
  QCOMPARE(findSurface(QStringLiteral("addaterangepicker-popup")), surface);
  QTRY_VERIFY(surface->isVisible());
  QVERIFY(surface->windowHandle());
  QVERIFY(surface->backingStore());
}

void QtToolPopupTest::recreateLifetimeStillDestroysPopupSurface() {
  QWidget host;
  host.resize(640, 360);
  auto* trigger = new QPushButton(QStringLiteral("Open"), &host);
  trigger->setGeometry(24, 24, 100, 32);

  AdPopover popover;
  popover.setSourceWidget(trigger);
  popover.setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
  popover.setPopupLifetime(AdPopover::PopupLifetime::RecreateOnOpen);
  popover.setText(QStringLiteral("Popover content"));

  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QPointer<QWidget> firstSurface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(firstSurface);
  QTRY_VERIFY(firstSurface->isVisible());

  popover.hide();
  QTRY_VERIFY(!popover.isVisible());
  QTRY_VERIFY(firstSurface.isNull());

  popover.show();
  QTRY_VERIFY(popover.isVisible());
  QWidget* secondSurface = findSurface(QStringLiteral("adpopover-surface"));
  QVERIFY(secondSurface);
}

QTEST_MAIN(QtToolPopupTest)

#include "qt_tool_popup_tests.moc"
