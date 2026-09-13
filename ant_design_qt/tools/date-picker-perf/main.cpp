#include "widgets/date_picker.h"

#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QPixmap>
#include <QTextStream>
#include <QTime>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

using adqt::widgets::AdDatePicker;
using adqt::widgets::AdDatePickerPanel;
using adqt::widgets::AdDateRangePicker;

void processEvents() { QCoreApplication::processEvents(QEventLoop::AllEvents, 1); }

struct BenchmarkResult {
  QString name;
  int iterations = 0;
  double totalMs = 0.0;
};

struct SampleStats {
  QString name;
  QVector<double> samplesMs;
};

double elapsedMs(const QElapsedTimer& timer) {
  return static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
}

SampleStats runSamples(const QString& name, int iterations, const std::function<void()>& setup,
                       const std::function<void()>& body,
                       const std::function<void()>& cleanup = {}) {
  SampleStats result{name, {}};
  result.samplesMs.reserve(iterations);
  for (int i = 0; i < iterations; ++i) {
    setup();
    QElapsedTimer timer;
    timer.start();
    body();
    result.samplesMs.append(elapsedMs(timer));
    if (cleanup) {
      cleanup();
    }
  }
  return result;
}

BenchmarkResult runBenchmark(const QString& name, int iterations,
                             const std::function<void(int)>& body) {
  for (int i = 0; i < 3; ++i) {
    body(i);
    processEvents();
  }

  QElapsedTimer timer;
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    body(i);
    processEvents();
  }
  return {name, iterations, static_cast<double>(timer.nsecsElapsed()) / 1000000.0};
}

void printResult(QTextStream& out, const BenchmarkResult& result) {
  const double perIteration =
      result.iterations > 0 ? result.totalMs / static_cast<double>(result.iterations) : 0.0;
  out << result.name << ": iterations=" << result.iterations
      << " total_ms=" << QString::number(result.totalMs, 'f', 3)
      << " per_iter_ms=" << QString::number(perIteration, 'f', 3) << Qt::endl;
}

void printStats(QTextStream& out, SampleStats result) {
  if (result.samplesMs.isEmpty()) {
    return;
  }
  std::sort(result.samplesMs.begin(), result.samplesMs.end());
  double total = 0.0;
  for (double sample : result.samplesMs) {
    total += sample;
  }
  const auto percentile = [&result](double fraction) {
    const int last = result.samplesMs.size() - 1;
    const int index = std::clamp(static_cast<int>(fraction * last + 0.5), 0, last);
    return result.samplesMs.at(index);
  };
  out << result.name << ": samples=" << result.samplesMs.size()
      << " min_ms=" << QString::number(result.samplesMs.constFirst(), 'f', 3)
      << " median_ms=" << QString::number(percentile(0.50), 'f', 3)
      << " mean_ms=" << QString::number(total / result.samplesMs.size(), 'f', 3)
      << " p95_ms=" << QString::number(percentile(0.95), 'f', 3)
      << " max_ms=" << QString::number(result.samplesMs.constLast(), 'f', 3) << Qt::endl;
}

QVector<QDate> selectedDateSet() {
  QVector<QDate> values;
  values.reserve(96);
  const QDate first(2026, 1, 1);
  for (int i = 0; i < 96; ++i) {
    values.append(first.addDays(i * 2));
  }
  return values;
}

QVector<AdDatePickerPanel::PresetItem> datePresetSet() {
  QVector<AdDatePickerPanel::PresetItem> presets;
  presets.reserve(8);
  for (int i = 0; i < 8; ++i) {
    AdDatePickerPanel::PresetItem preset;
    preset.label = QStringLiteral("Preset %1").arg(i + 1);
    preset.value = QDate(2026, 7, 1).addDays(i);
    presets.append(preset);
  }
  return presets;
}

QVector<AdDatePickerPanel::PresetItem> rangePresetSet() {
  QVector<AdDatePickerPanel::PresetItem> presets;
  presets.reserve(8);
  for (int i = 0; i < 8; ++i) {
    AdDatePickerPanel::PresetItem preset;
    preset.label = QStringLiteral("Range %1").arg(i + 1);
    preset.rangeStartValue = QDate(2026, 7, 1).addDays(i);
    preset.rangeEndValue = preset.rangeStartValue.addDays(7);
    presets.append(preset);
  }
  return presets;
}

void forceRender(QWidget* widget) {
  if (!widget) {
    return;
  }
  const QSize size = widget->sizeHint().expandedTo(QSize(320, 260));
  widget->resize(size);
  QPixmap pixmap(size);
  pixmap.fill(Qt::transparent);
  widget->render(&pixmap);
}

template <typename Widget, typename Configure>
SampleStats benchmarkLifecycle(const QString& name, int iterations, Configure configure) {
  return runSamples(
      name, iterations, []() {},
      [&]() {
        auto widget = std::make_unique<Widget>();
        configure(*widget);
      });
}

template <typename Picker, typename Configure>
SampleStats benchmarkFreshPopup(QWidget* host, const QString& name, int iterations,
                                typename Picker::PopupLayerMode mode, Configure configure) {
  std::unique_ptr<Picker> picker;
  return runSamples(
      name, iterations,
      [&]() {
        picker = std::make_unique<Picker>(host);
        picker->setPopupLayerMode(mode);
        configure(*picker);
        picker->show();
        processEvents();
      },
      [&]() {
        picker->showPopup();
        processEvents();
        forceRender(host);
      },
      [&]() {
        picker->hidePopup();
        picker.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        processEvents();
      });
}

template <typename Configure>
SampleStats benchmarkFreshPanelRender(const QString& name, int iterations, Configure configure) {
  return runSamples(
      name, iterations, []() {},
      [&]() {
        AdDatePickerPanel panel;
        configure(panel);
        panel.show();
        processEvents();
        forceRender(&panel);
      });
}

}  // namespace

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }

  QApplication app(argc, argv);
  QTextStream out(stdout);

  out << "date_picker_perf_version=2" << Qt::endl;
  out << "qt_version=" << QT_VERSION_STR << Qt::endl;

  QWidget coldHost;
  coldHost.resize(1200, 720);
  coldHost.show();
  processEvents();
  printStats(out, benchmarkFreshPopup<AdDatePicker>(
                      &coldHost, "process_first_single_popup_in_window", 1,
                      AdDatePicker::PopupLayerMode::InWindow,
                      [](AdDatePicker& picker) { picker.setDate(QDate(2026, 7, 8)); }));

  printStats(out, benchmarkLifecycle<AdDatePicker>("lifecycle_single", 80, [](AdDatePicker&) {}));
  printStats(out, benchmarkLifecycle<AdDatePicker>("lifecycle_time", 40, [](AdDatePicker& picker) {
               picker.setShowTime(true);
             }));
  printStats(
      out, benchmarkLifecycle<AdDateRangePicker>("lifecycle_range", 50, [](AdDateRangePicker&) {}));
  printStats(out, benchmarkLifecycle<AdDatePickerPanel>("lifecycle_panel_date", 40,
                                                        [](AdDatePickerPanel&) {}));
  printStats(out, benchmarkLifecycle<AdDatePickerPanel>(
                      "lifecycle_panel_time", 20,
                      [](AdDatePickerPanel& panel) { panel.setShowTime(true); }));

  printStats(out, benchmarkFreshPopup<AdDatePicker>(
                      &coldHost, "fresh_single_popup_in_window", 24,
                      AdDatePicker::PopupLayerMode::InWindow,
                      [](AdDatePicker& picker) { picker.setDate(QDate(2026, 7, 8)); }));
  printStats(out,
             benchmarkFreshPopup<AdDatePicker>(
                 &coldHost, "fresh_single_popup_qt_tool", 16, AdDatePicker::PopupLayerMode::QtTool,
                 [](AdDatePicker& picker) { picker.setDate(QDate(2026, 7, 8)); }));
  printStats(out, benchmarkFreshPopup<AdDatePicker>(
                      &coldHost, "fresh_time_popup_in_window", 16,
                      AdDatePicker::PopupLayerMode::InWindow, [](AdDatePicker& picker) {
                        picker.setShowTime(true);
                        picker.setDateTime(QDateTime(QDate(2026, 7, 8), QTime(13, 45, 30)));
                      }));
  printStats(out, benchmarkFreshPopup<AdDatePicker>(&coldHost, "fresh_multiple_popup_in_window", 16,
                                                    AdDatePicker::PopupLayerMode::InWindow,
                                                    [](AdDatePicker& picker) {
                                                      picker.setMultiple(true);
                                                      picker.setSelectedDates(selectedDateSet());
                                                    }));
  printStats(out, benchmarkFreshPopup<AdDatePicker>(
                      &coldHost, "fresh_disabled_popup_in_window", 16,
                      AdDatePicker::PopupLayerMode::InWindow, [](AdDatePicker& picker) {
                        picker.setDate(QDate(2026, 7, 8));
                        picker.setDisabledDatePredicate([](const QDate& date) {
                          return date.dayOfWeek() == Qt::Saturday || date.dayOfWeek() == Qt::Sunday;
                        });
                      }));
  printStats(out, benchmarkFreshPopup<AdDatePicker>(
                      &coldHost, "fresh_single_presets_popup_in_window", 12,
                      AdDatePicker::PopupLayerMode::InWindow, [](AdDatePicker& picker) {
                        picker.setDate(QDate(2026, 7, 8));
                        picker.setPresets(datePresetSet());
                      }));
  printStats(out, benchmarkFreshPopup<AdDateRangePicker>(
                      &coldHost, "fresh_range_popup_in_window", 16,
                      AdDateRangePicker::PopupLayerMode::InWindow, [](AdDateRangePicker& picker) {
                        picker.setRange(QDate(2026, 7, 8), QDate(2026, 7, 22));
                      }));
  printStats(out, benchmarkFreshPopup<AdDateRangePicker>(
                      &coldHost, "fresh_range_presets_popup_in_window", 10,
                      AdDateRangePicker::PopupLayerMode::InWindow, [](AdDateRangePicker& picker) {
                        picker.setRange(QDate(2026, 7, 8), QDate(2026, 7, 22));
                        picker.setPresets(rangePresetSet());
                      }));
  printStats(out, benchmarkFreshPopup<AdDateRangePicker>(
                      &coldHost, "fresh_range_popup_qt_tool", 12,
                      AdDateRangePicker::PopupLayerMode::QtTool, [](AdDateRangePicker& picker) {
                        picker.setRange(QDate(2026, 7, 8), QDate(2026, 7, 22));
                      }));
  printStats(out, benchmarkFreshPopup<AdDateRangePicker>(
                      &coldHost, "fresh_range_time_popup_in_window", 10,
                      AdDateRangePicker::PopupLayerMode::InWindow, [](AdDateRangePicker& picker) {
                        picker.setShowTime(true);
                        picker.setDateTimeRange(QDateTime(QDate(2026, 7, 8), QTime(9, 15)),
                                                QDateTime(QDate(2026, 7, 22), QTime(18, 30)));
                      }));
  printStats(out,
             benchmarkFreshPanelRender("fresh_panel_date_render", 24, [](AdDatePickerPanel& panel) {
               panel.setSelectedDate(QDate(2026, 7, 8));
             }));
  printStats(out,
             benchmarkFreshPanelRender("fresh_panel_time_render", 12, [](AdDatePickerPanel& panel) {
               panel.setShowTime(true);
               panel.setSelectedDateTime(QDateTime(QDate(2026, 7, 8), QTime(13, 45, 30)));
             }));

  AdDatePickerPanel countDatePanel;
  AdDatePickerPanel countTimePanel;
  countTimePanel.setShowTime(true);
  out << "date_panel_child_widgets=" << countDatePanel.findChildren<QWidget*>().size() << Qt::endl;
  out << "time_panel_child_widgets=" << countTimePanel.findChildren<QWidget*>().size() << Qt::endl;

  QWidget host;
  auto* layout = new QHBoxLayout(&host);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  AdDatePicker singlePicker;
  singlePicker.setDate(QDate(2026, 7, 8));
  singlePicker.setPopupLayerMode(AdDatePicker::PopupLayerMode::InWindow);
  layout->addWidget(&singlePicker);

  AdDatePicker timePicker;
  timePicker.setShowTime(true);
  timePicker.setDateTime(QDateTime(QDate(2026, 7, 8), QTime(13, 45, 30)));
  timePicker.setPopupLayerMode(AdDatePicker::PopupLayerMode::InWindow);
  layout->addWidget(&timePicker);

  AdDateRangePicker rangePicker;
  rangePicker.setRange(QDate(2026, 7, 8), QDate(2026, 7, 22));
  rangePicker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  layout->addWidget(&rangePicker);

  AdDatePicker disabledSinglePicker;
  disabledSinglePicker.setDate(QDate(2026, 7, 8));
  disabledSinglePicker.setPopupLayerMode(AdDatePicker::PopupLayerMode::InWindow);
  disabledSinglePicker.setDisabledDatePredicate([](const QDate& date) {
    return date.dayOfWeek() == Qt::Saturday || date.dayOfWeek() == Qt::Sunday;
  });
  layout->addWidget(&disabledSinglePicker);

  AdDateRangePicker disabledRangePicker;
  disabledRangePicker.setRange(QDate(2026, 7, 8), QDate(2026, 7, 22));
  disabledRangePicker.setPopupLayerMode(AdDateRangePicker::PopupLayerMode::InWindow);
  disabledRangePicker.setDisabledDatePredicate([](const QDate& date) {
    return date.dayOfWeek() == Qt::Saturday || date.dayOfWeek() == Qt::Sunday;
  });
  layout->addWidget(&disabledRangePicker);

  AdDatePicker multiplePicker;
  multiplePicker.setMultiple(true);
  multiplePicker.setSelectedDates(selectedDateSet());
  multiplePicker.setPopupLayerMode(AdDatePicker::PopupLayerMode::InWindow);
  layout->addWidget(&multiplePicker);

  host.show();
  processEvents();

  AdDatePickerPanel panel;
  panel.setSelectedDate(QDate(2026, 7, 8));
  panel.show();
  processEvents();

  AdDatePickerPanel multiplePanel;
  multiplePanel.setSelectionMode(AdDatePickerPanel::SelectionMode::Multiple);
  multiplePanel.setSelectedDates(selectedDateSet());
  multiplePanel.show();
  processEvents();

  printResult(out, runBenchmark("single_popup_open_close", 80, [&](int i) {
                singlePicker.setPickerValue(QDate(2026, 7, 1).addMonths(i % 12));
                singlePicker.showPopup();
                processEvents();
                singlePicker.hidePopup();
              }));

  printResult(out, runBenchmark("time_popup_open_close", 50, [&](int i) {
                timePicker.setDateTime(QDateTime(QDate(2026, 7, 8).addDays(i % 28),
                                                 QTime((i * 3) % 24, (i * 7) % 60, i % 60)));
                timePicker.showPopup();
                processEvents();
                timePicker.hidePopup();
              }));

  printResult(out, runBenchmark("range_popup_open_close", 50, [&](int i) {
                const QDate start = QDate(2026, 7, 1).addDays(i % 20);
                rangePicker.setRange(start, start.addDays(7));
                rangePicker.showPopup();
                processEvents();
                rangePicker.hidePopup();
              }));

  printResult(out, runBenchmark("range_set_hidden", 200, [&](int i) {
                const QDate start = QDate(2026, 8, 1).addDays(i % 24);
                rangePicker.setRange(start, start.addDays(7));
              }));

  printResult(out, runBenchmark("single_set_hidden", 200, [&](int i) {
                singlePicker.setDate(QDate(2026, 8, 1).addDays(i % 24));
              }));

  printResult(out, runBenchmark("time_set_hidden", 200, [&](int i) {
                timePicker.setDateTime(QDateTime(QDate(2026, 8, 1).addDays(i % 24),
                                                 QTime((i * 5) % 24, (i * 11) % 60, i % 60)));
              }));

  QVector<QDate> hiddenSetA = selectedDateSet();
  QVector<QDate> hiddenSetB;
  hiddenSetB.reserve(hiddenSetA.size());
  for (const QDate& date : hiddenSetA) {
    hiddenSetB.append(date.addDays(1));
  }
  multiplePicker.showPopup();
  processEvents();
  multiplePicker.hidePopup();
  printResult(out, runBenchmark("multiple_set_hidden", 120, [&](int i) {
                multiplePicker.setSelectedDates((i % 2) == 0 ? hiddenSetA : hiddenSetB);
              }));

  rangePicker.setRange(QDate(2026, 9, 1), QDate(2026, 9, 8));
  printResult(out, runBenchmark("range_popup_stable_open_close", 80, [&](int) {
                rangePicker.showPopup();
                processEvents();
                rangePicker.hidePopup();
              }));

  singlePicker.setDate(QDate(2026, 9, 1));
  printResult(out, runBenchmark("single_popup_stable_open_close", 100, [&](int) {
                singlePicker.showPopup();
                processEvents();
                singlePicker.hidePopup();
              }));

  printResult(out, runBenchmark("single_disabled_stable_open_close", 100, [&](int) {
                disabledSinglePicker.showPopup();
                processEvents();
                disabledSinglePicker.hidePopup();
              }));

  printResult(out, runBenchmark("range_disabled_stable_open_close", 80, [&](int) {
                disabledRangePicker.showPopup();
                processEvents();
                disabledRangePicker.hidePopup();
              }));

  printResult(out, runBenchmark("panel_navigate_render", 200, [&](int i) {
                panel.setViewDate(QDate(2026, 1, 1).addMonths(i));
                forceRender(&panel);
              }));

  printResult(out, runBenchmark("multiple_panel_render", 120, [&](int i) {
                multiplePanel.setViewDate(QDate(2026, 1, 1).addMonths(i % 12));
                forceRender(&multiplePanel);
              }));

  return 0;
}
