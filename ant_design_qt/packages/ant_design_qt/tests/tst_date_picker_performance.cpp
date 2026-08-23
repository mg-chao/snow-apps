#include <QApplication>
#include <QListWidget>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QScrollArea>
#include <QtTest>

#include "widgets/date_picker.h"

#include <algorithm>

namespace {

using adqt::widgets::AdDatePicker;
using adqt::widgets::AdDatePickerPanel;
using adqt::widgets::AdDateRangePicker;

QList<QListWidget*> timeColumns(QWidget* widget) {
  QList<QListWidget*> result;
  const QList<QListWidget*> lists = widget->findChildren<QListWidget*>();
  for (QListWidget* list : lists) {
    if (list->property("adqt.semantic.class").toString() ==
        QStringLiteral("addatepicker-panel-time-column")) {
      result.append(list);
    }
  }
  return result;
}

void processEvents() { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }

}  // namespace

class DatePickerPerformanceTests final : public QObject {
  Q_OBJECT

 private slots:
  void datePanelDefersTimeControls();
  void timePanelCreatesOnlySingleGroup();
  void rangePanelCreatesSidesOnDemand();
  void rangePanelResynchronizationRepaintsGrid();
  void unchangedPanelRenderReusesCellState();
  void deferredColumnsHonorConfiguredSteps();
  void meridiemColumnUsesConfiguredLocale();
  void presetsAreDeferredUntilConfigured();
  void pickerPopupUsesDeferredPanel();
  void rangePickerStartEditAdvancesToEnd();
  void rangePickerStartAfterEndClickAdvancesToEnd();
  void rangePickerStartAfterEndContinuesEditing();
  void rangePickerCrossPanelEditRefreshesHighlight();
};

void DatePickerPerformanceTests::datePanelDefersTimeControls() {
  AdDatePickerPanel panel;
  QCOMPARE(timeColumns(&panel).size(), 0);

  panel.setSelectedTime(QTime(13, 45, 30));
  QCOMPARE(panel.selectedTime(), QTime(13, 45, 30));
  QCOMPARE(timeColumns(&panel).size(), 0);
}

void DatePickerPerformanceTests::timePanelCreatesOnlySingleGroup() {
  AdDatePickerPanel panel;
  panel.setSelectedTime(QTime(13, 45, 30));
  panel.setShowTime(true);

  QCOMPARE(timeColumns(&panel).size(), 4);
  QCOMPARE(panel.selectedTime(), QTime(13, 45, 30));

  panel.setShowTime(false);
  panel.setShowTime(true);
  QCOMPARE(timeColumns(&panel).size(), 4);
}

void DatePickerPerformanceTests::meridiemColumnUsesConfiguredLocale() {
  const QLocale locale(QLocale::Chinese, QLocale::China);
  AdDatePickerPanel panel;
  panel.setLocale(locale);
  panel.setShowTime(true);

  const QList<QListWidget*> columns = timeColumns(&panel);
  QCOMPARE(columns.size(), 4);
  const auto meridiemIt = std::find_if(columns.cbegin(), columns.cend(), [&locale](QListWidget* list) {
    return list && list->count() >= 2 && list->item(0)->text() == locale.amText() &&
           list->item(1)->text() == locale.pmText();
  });
  QVERIFY(meridiemIt != columns.cend());
  QListWidget* const meridiem = *meridiemIt;
  QCOMPARE(meridiem->item(0)->text(), locale.amText());
  QCOMPARE(meridiem->item(1)->text(), locale.pmText());
}

void DatePickerPerformanceTests::rangePanelCreatesSidesOnDemand() {
  AdDatePickerPanel panel;
  panel.setSelectionMode(AdDatePickerPanel::SelectionMode::Range);
  panel.setShowTime(true);
  QCOMPARE(timeColumns(&panel).size(), 4);

  panel.setVisibleRangeTimePart(AdDatePickerPanel::TimeSelectionPart::End);
  QCOMPARE(timeColumns(&panel).size(), 8);

  panel.setVisibleRangeTimePart(AdDatePickerPanel::TimeSelectionPart::Start);
  QCOMPARE(timeColumns(&panel).size(), 8);
}

void DatePickerPerformanceTests::rangePanelResynchronizationRepaintsGrid() {
  QWidget host;
  host.resize(500, 500);
  host.show();

  AdDatePickerPanel panel(&host);
  panel.setSelectionMode(AdDatePickerPanel::SelectionMode::Range);
  panel.setRange(QDate(2026, 7, 3), QDate(2026, 7, 10));
  int paintCount = 0;
  panel.setCellRenderCallback([&](QPainter&, const AdDatePickerPanel::CellRenderInfo& info) {
    if (info.inView && info.date == QDate(2026, 7, 5)) {
      ++paintCount;
    }
  });
  panel.show();
  processEvents();
  QVERIFY(paintCount > 0);

  paintCount = 0;
  panel.setRange(QDate(2026, 7, 3), QDate(2026, 7, 10));
  processEvents();
  QVERIFY(paintCount > 0);
}

void DatePickerPerformanceTests::unchangedPanelRenderReusesCellState() {
  AdDatePickerPanel panel;
  panel.setViewDate(QDate(2026, 7, 1));
  int predicateCalls = 0;
  panel.setDisabledDatePredicate([&](const QDate&) {
    ++predicateCalls;
    return false;
  });
  panel.show();
  processEvents();

  QWidget* grid = panel.findChild<QWidget*>(QStringLiteral("addatepicker-panel-content"));
  QVERIFY(grid != nullptr);
  QImage image(grid->size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  grid->render(&image);

  predicateCalls = 0;
  grid->render(&image);
  QCOMPARE(predicateCalls, 0);

  panel.setViewDate(QDate(2026, 8, 1));
  grid->render(&image);
  QVERIFY(predicateCalls > 0);
}

void DatePickerPerformanceTests::deferredColumnsHonorConfiguredSteps() {
  AdDatePickerPanel panel;
  panel.setTimeSteps(2, 15, 30);
  QCOMPARE(timeColumns(&panel).size(), 0);

  panel.setShowTime(true);
  QList<int> itemCounts;
  for (const QListWidget* list : timeColumns(&panel)) {
    int valueCount = 0;
    for (int index = 0; index < list->count(); ++index) {
      const QListWidgetItem* item = list->item(index);
      if (item && item->data(Qt::UserRole).isValid()) {
        ++valueCount;
      }
    }
    itemCounts.append(valueCount);
  }
  std::sort(itemCounts.begin(), itemCounts.end());
  QCOMPARE(itemCounts, QList<int>({2, 2, 4, 12}));
}

void DatePickerPerformanceTests::presetsAreDeferredUntilConfigured() {
  AdDatePickerPanel panel;
  QCOMPARE(panel.findChildren<QScrollArea*>().size(), 0);

  AdDatePickerPanel::PresetItem preset;
  preset.label = QStringLiteral("Today");
  preset.value = QDate(2026, 7, 8);
  panel.setPresets({preset});
  QCOMPARE(panel.findChildren<QScrollArea*>().size(), 1);
  panel.clearPresets();
  QCOMPARE(panel.findChildren<QScrollArea*>().size(), 1);

  QWidget host;
  host.resize(800, 600);
  host.show();
  AdDateRangePicker picker(&host);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  picker.show();
  picker.showPopup();
  processEvents();
  QCOMPARE(host.findChildren<QScrollArea*>(QStringLiteral("addaterangepicker-popup-presets-scroll"))
               .size(),
           0);

  AdDateRangePicker::PresetItem rangePreset;
  rangePreset.label = QStringLiteral("This week");
  rangePreset.rangeStartValue = QDate(2026, 7, 6);
  rangePreset.rangeEndValue = QDate(2026, 7, 12);
  picker.setPresets({rangePreset});
  QCOMPARE(host.findChildren<QScrollArea*>(QStringLiteral("addaterangepicker-popup-presets-scroll"))
               .size(),
           1);
  picker.hidePopup();
}

void DatePickerPerformanceTests::pickerPopupUsesDeferredPanel() {
  QWidget host;
  host.resize(800, 600);
  host.show();

  AdDatePicker picker(&host);
  picker.setPopupLayerMode(AdDatePicker::PopupLayerMode::InWindow);
  picker.show();
  picker.showPopup();
  processEvents();
  QCOMPARE(timeColumns(&host).size(), 0);
  picker.hidePopup();

  AdDatePicker timePicker(&host);
  timePicker.setPopupLayerMode(AdDatePicker::PopupLayerMode::InWindow);
  timePicker.setShowTime(true);
  timePicker.show();
  timePicker.showPopup();
  processEvents();
  QCOMPARE(timeColumns(&host).size(), 4);
  timePicker.hidePopup();
}

void DatePickerPerformanceTests::rangePickerStartEditAdvancesToEnd() {
  QWidget host;
  host.resize(800, 600);
  host.show();

  AdDateRangePicker picker(&host);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  picker.setRange(QDate(2026, 7, 1), QDate(2026, 7, 10));
  QSignalSpy acceptedSpy(&picker, &AdDateRangePicker::accepted);
  QMap<QDate, AdDateRangePicker::CellRenderInfo> renderedCells;
  picker.setCellRenderCallback([&](QPainter&, const AdDateRangePicker::CellRenderInfo& info) {
    if (info.inView && info.type == AdDatePickerPanel::PickerMode::Date) {
      renderedCells.insert(info.date, info);
    }
  });
  picker.show();
  picker.showPopup();
  processEvents();

  QVERIFY(picker.lineEdit() != nullptr);
  QVERIFY(picker.panel() != nullptr);
  QWidget* grid = nullptr;
  for (QWidget* child : picker.panel()->findChildren<QWidget*>()) {
    if (child->objectName() == QStringLiteral("addatepicker-panel-content")) {
      grid = child;
      break;
    }
  }
  QVERIFY(grid != nullptr);
  QTest::mouseClick(picker.lineEdit(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(4, picker.lineEdit()->height() / 2));
  processEvents();

  const auto pointForDate = [grid, &picker](const QDate& date) {
    const QDate view = picker.panel()->viewDate();
    const QDate firstOfMonth(view.year(), view.month(), 1);
    const int firstDayOffset =
        (firstOfMonth.dayOfWeek() - static_cast<int>(picker.panel()->firstDayOfWeek()) + 7) % 7;
    const int index = firstDayOffset + date.day() - 1;
    const int row = index / 7;
    const int column = index % 7;
    return QPoint((column * 2 + 1) * grid->width() / 14,
                  (row * 2 + 3) * grid->height() / 14);
  };

  const auto clickDate = [grid, &pointForDate](const QDate& date) {
    const QPoint point = pointForDate(date);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, point);
    processEvents();
  };

  const auto hoverDate = [&picker](const QDate& date) {
    // DatePickerCalendarGrid emits this signal for its mouse-hovered cell.
    picker.panel()->previewDateChanged(date);
    processEvents();
  };

  const auto capturePrimaryGrid = [&]() {
    renderedCells.clear();
    QImage image(grid->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    grid->render(&painter);
    return image;
  };

  QTest::mouseClick(picker.lineEdit(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(4, picker.lineEdit()->height() / 2));
  processEvents();
  hoverDate(QDate(2026, 7, 4));
  hoverDate(QDate(2026, 7, 5));
  capturePrimaryGrid();
  QVERIFY(renderedCells.value(QDate(2026, 7, 5)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 8)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 10)).hoverRangeEnd);

  clickDate(QDate(2026, 7, 3));
  const QImage firstEditedImage = capturePrimaryGrid();
  QVERIFY(picker.popupVisible());
  QVERIFY(picker.lineEdit()->cursorPosition() > picker.lineEdit()->text().size() / 2);
  QVERIFY(renderedCells.value(QDate(2026, 7, 3)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 5)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 10)).hoverRangeEnd);

  hoverDate(QDate(2026, 7, 4));
  hoverDate(QDate(2026, 7, 5));
  capturePrimaryGrid();
  QVERIFY(renderedCells.value(QDate(2026, 7, 3)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 4)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 5)).hoverRangeEnd);

  clickDate(QDate(2026, 7, 12));
  processEvents();
  QCOMPARE(picker.startDate(), QDate(2026, 7, 3));
  QCOMPARE(picker.endDate(), QDate(2026, 7, 12));
  QCOMPARE(acceptedSpy.count(), 1);

  picker.showPopup();
  processEvents();
  const QImage secondEditedImage = capturePrimaryGrid();
  QVERIFY(renderedCells.value(QDate(2026, 7, 3)).rangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 5)).inRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 10)).inRange);
  QVERIFY(!renderedCells.value(QDate(2026, 7, 10)).rangeEnd);
  QVERIFY(renderedCells.value(QDate(2026, 7, 12)).rangeEnd);

  const QRect oldEndRect = renderedCells.value(QDate(2026, 7, 10)).cellRect;
  const QRect newEndRect = renderedCells.value(QDate(2026, 7, 12)).cellRect;
  QVERIFY(firstEditedImage.pixelColor(oldEndRect.center()) !=
          secondEditedImage.pixelColor(oldEndRect.center()));
  QVERIFY(firstEditedImage.pixelColor(newEndRect.center()) !=
          secondEditedImage.pixelColor(newEndRect.center()));

  picker.hidePopup();
  QTest::mouseClick(picker.lineEdit(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(4, picker.lineEdit()->height() / 2));
  processEvents();
  clickDate(QDate(2026, 7, 5));
  capturePrimaryGrid();
  QVERIFY(renderedCells.value(QDate(2026, 7, 5)).rangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 8)).inRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 12)).rangeEnd);

  clickDate(QDate(2026, 7, 14));
  QCOMPARE(picker.startDate(), QDate(2026, 7, 5));
  QCOMPARE(picker.endDate(), QDate(2026, 7, 14));
  QCOMPARE(acceptedSpy.count(), 2);
}

void DatePickerPerformanceTests::rangePickerStartAfterEndContinuesEditing() {
  QWidget host;
  host.resize(800, 600);
  host.show();

  AdDateRangePicker picker(&host);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  picker.setRange(QDate(2026, 7, 10), QDate(2026, 7, 20));
  QSignalSpy acceptedSpy(&picker, &AdDateRangePicker::accepted);
  QMap<QDate, AdDateRangePicker::CellRenderInfo> renderedCells;
  picker.setCellRenderCallback([&](QPainter&, const AdDateRangePicker::CellRenderInfo& info) {
    if (info.inView && info.type == AdDatePickerPanel::PickerMode::Date) {
      renderedCells.insert(info.date, info);
    }
  });
  picker.show();
  picker.showPopup();
  processEvents();

  QWidget* grid = nullptr;
  for (QWidget* child : picker.panel()->findChildren<QWidget*>()) {
    if (child->objectName() == QStringLiteral("addatepicker-panel-content")) {
      grid = child;
      break;
    }
  }
  QVERIFY(grid != nullptr);
  const auto pointForDate = [grid, &picker](const QDate& date) {
    const QDate view = picker.panel()->viewDate();
    const QDate firstOfMonth(view.year(), view.month(), 1);
    const int firstDayOffset =
        (firstOfMonth.dayOfWeek() - static_cast<int>(picker.panel()->firstDayOfWeek()) + 7) % 7;
    const int index = firstDayOffset + date.day() - 1;
    const int row = index / 7;
    const int column = index % 7;
    return QPoint((column * 2 + 1) * grid->width() / 14,
                  (row * 2 + 3) * grid->height() / 14);
  };
  const auto clickDate = [grid, &pointForDate](const QDate& date) {
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, pointForDate(date));
    processEvents();
  };
  const auto hoverDate = [&picker](const QDate& date) {
    // DatePickerCalendarGrid emits this signal for its mouse-hovered cell.
    picker.panel()->previewDateChanged(date);
    processEvents();
  };
  const auto captureGrid = [&]() {
    renderedCells.clear();
    QImage image(grid->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    grid->render(&painter);
  };

  QTest::mouseClick(picker.lineEdit(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(4, picker.lineEdit()->height() / 2));
  processEvents();
  hoverDate(QDate(2026, 7, 25));
  captureGrid();
  QVERIFY(!renderedCells.value(QDate(2026, 7, 10)).selected);
  QVERIFY(!renderedCells.value(QDate(2026, 7, 20)).selected);
  QVERIFY(!renderedCells.value(QDate(2026, 7, 15)).inRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 20)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 22)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 25)).hoverRangeEnd);

  clickDate(QDate(2026, 7, 25));

  QVERIFY(picker.popupVisible());
  QVERIFY(picker.lineEdit()->cursorPosition() > picker.lineEdit()->text().size() / 2);
  QCOMPARE(picker.startDate(), QDate(2026, 7, 10));
  QCOMPARE(picker.endDate(), QDate(2026, 7, 20));
  QCOMPARE(acceptedSpy.count(), 0);

  hoverDate(QDate(2026, 7, 8));
  captureGrid();
  QVERIFY(renderedCells.value(QDate(2026, 7, 8)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 20)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 25)).hoverRangeEnd);

  hoverDate(QDate(2026, 7, 27));
  captureGrid();
  QVERIFY(!renderedCells.value(QDate(2026, 7, 10)).selected);
  QVERIFY(!renderedCells.value(QDate(2026, 7, 20)).selected);
  QVERIFY(renderedCells.value(QDate(2026, 7, 25)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 26)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 27)).hoverRangeEnd);

  clickDate(QDate(2026, 7, 28));
  QCOMPARE(picker.startDate(), QDate(2026, 7, 25));
  QCOMPARE(picker.endDate(), QDate(2026, 7, 28));
  QCOMPARE(acceptedSpy.count(), 1);
  QVERIFY(!picker.popupVisible());
}

void DatePickerPerformanceTests::rangePickerStartAfterEndClickAdvancesToEnd() {
  QWidget host;
  host.resize(800, 600);
  host.show();

  AdDateRangePicker picker(&host);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  picker.setRange(QDate(2026, 7, 10), QDate(2026, 7, 20));
  QSignalSpy acceptedSpy(&picker, &AdDateRangePicker::accepted);
  QMap<QDate, AdDateRangePicker::CellRenderInfo> renderedCells;
  picker.setCellRenderCallback([&](QPainter&, const AdDateRangePicker::CellRenderInfo& info) {
    if (info.inView && info.type == AdDatePickerPanel::PickerMode::Date) {
      renderedCells.insert(info.date, info);
    }
  });

  picker.show();
  QTest::mouseClick(picker.lineEdit(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(4, picker.lineEdit()->height() / 2));
  processEvents();

  QWidget* grid = nullptr;
  for (QWidget* child : picker.panel()->findChildren<QWidget*>()) {
    if (child->objectName() == QStringLiteral("addatepicker-panel-content")) {
      grid = child;
      break;
    }
  }
  QVERIFY(grid != nullptr);

  const auto pointForDate = [grid, &picker](const QDate& date) {
    const QDate view = picker.panel()->viewDate();
    const QDate firstOfMonth(view.year(), view.month(), 1);
    const int firstDayOffset =
        (firstOfMonth.dayOfWeek() - static_cast<int>(picker.panel()->firstDayOfWeek()) + 7) % 7;
    const int index = firstDayOffset + date.day() - 1;
    const int row = index / 7;
    const int column = index % 7;
    return QPoint((column * 2 + 1) * grid->width() / 14,
                  (row * 2 + 3) * grid->height() / 14);
  };

  const auto clickDate = [grid, &pointForDate](const QDate& date) {
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, pointForDate(date));
    processEvents();
  };
  const auto captureGrid = [&]() {
    renderedCells.clear();
    QImage image(grid->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    grid->render(&painter);
  };

  clickDate(QDate(2026, 7, 25));
  captureGrid();

  QVERIFY(picker.popupVisible());
  QVERIFY(picker.lineEdit()->cursorPosition() > picker.lineEdit()->text().size() / 2);
  QCOMPARE(picker.startDate(), QDate(2026, 7, 10));
  QCOMPARE(picker.endDate(), QDate(2026, 7, 20));
  QCOMPARE(acceptedSpy.count(), 0);
  QVERIFY(!renderedCells.value(QDate(2026, 7, 10)).inRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 20)).hoverRangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 22)).hoverRange);
  QVERIFY(renderedCells.value(QDate(2026, 7, 25)).hoverRangeEnd);

  clickDate(QDate(2026, 7, 28));
  QCOMPARE(picker.startDate(), QDate(2026, 7, 25));
  QCOMPARE(picker.endDate(), QDate(2026, 7, 28));
  QCOMPARE(acceptedSpy.count(), 1);
  QVERIFY(!picker.popupVisible());
}

void DatePickerPerformanceTests::rangePickerCrossPanelEditRefreshesHighlight() {
  QWidget host;
  host.resize(1000, 700);
  host.show();

  AdDateRangePicker picker(&host);
  picker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  picker.setRange(QDate(2026, 7, 1), QDate(2026, 8, 10));

  QMap<QDate, AdDateRangePicker::CellRenderInfo> renderedCells;
  picker.setCellRenderCallback([&](QPainter&, const AdDateRangePicker::CellRenderInfo& info) {
    if (info.inView && info.type == AdDatePickerPanel::PickerMode::Date) {
      renderedCells.insert(info.date, info);
    }
  });
  picker.show();
  picker.showPopup();
  processEvents();

  auto findGrid = [](AdDatePickerPanel* panel) {
    if (!panel) {
      return static_cast<QWidget*>(nullptr);
    }
    for (QWidget* child : panel->findChildren<QWidget*>()) {
      if (child->objectName() == QStringLiteral("addatepicker-panel-content")) {
        return child;
      }
    }
    return static_cast<QWidget*>(nullptr);
  };
  QWidget* primaryGrid = findGrid(picker.panel());
  QWidget* secondaryGrid = findGrid(picker.endPanel());
  QVERIFY(primaryGrid != nullptr);
  QVERIFY(secondaryGrid != nullptr);

  const auto clickDate = [](AdDatePickerPanel* panel, QWidget* grid, const QDate& date) {
    const QDate view = panel->viewDate();
    const QDate firstOfMonth(view.year(), view.month(), 1);
    const int firstDayOffset =
        (firstOfMonth.dayOfWeek() - static_cast<int>(panel->firstDayOfWeek()) + 7) % 7;
    const int index = firstDayOffset + date.day() - 1;
    const int row = index / 7;
    const int column = index % 7;
    const QPoint point((column * 2 + 1) * grid->width() / 14,
                       (row * 2 + 3) * grid->height() / 14);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, point);
    processEvents();
  };

  QTest::mouseClick(picker.lineEdit(), Qt::LeftButton, Qt::NoModifier,
                    QPoint(4, picker.lineEdit()->height() / 2));
  processEvents();
  clickDate(picker.panel(), primaryGrid, QDate(2026, 7, 3));
  renderedCells.clear();
  {
    QImage image(primaryGrid->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    primaryGrid->render(&painter);
  }
  QVERIFY(renderedCells.value(QDate(2026, 7, 3)).rangeStart);
  QVERIFY(renderedCells.value(QDate(2026, 7, 8)).inRange);

  clickDate(picker.endPanel(), secondaryGrid, QDate(2026, 8, 12));
  QCOMPARE(picker.startDate(), QDate(2026, 7, 3));
  QCOMPARE(picker.endDate(), QDate(2026, 8, 12));

  picker.showPopup();
  processEvents();
  renderedCells.clear();
  {
    QImage image(secondaryGrid->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    secondaryGrid->render(&painter);
  }
  QVERIFY(renderedCells.value(QDate(2026, 8, 5)).inRange);
  QVERIFY(renderedCells.value(QDate(2026, 8, 12)).rangeEnd);
}

QTEST_MAIN(DatePickerPerformanceTests)
#include "tst_date_picker_performance.moc"
