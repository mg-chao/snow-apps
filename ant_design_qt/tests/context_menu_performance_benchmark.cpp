#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/context_menu.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using Menu = adqt::widgets::AdContextMenu;
namespace icons = adqt::icons::antd::outlined;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class BenchmarkApplication final : public QApplication {
 public:
  using QApplication::QApplication;
  QPointer<Menu> measuredMenu;
  QElapsedTimer* measuredTimer = nullptr;
  qint64 firstPaintNs = 0;

  bool notify(QObject* receiver, QEvent* event) override {
    const bool measure = receiver == measuredMenu && measuredTimer && firstPaintNs == 0 &&
                         event->type() == QEvent::Paint;
    const bool handled = QApplication::notify(receiver, event);
    if (measure && firstPaintNs == 0) firstPaintNs = measuredTimer->nsecsElapsed();
    return handled;
  }
};

void drainRetirement() {
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QJsonObject memorySnapshot() {
  QJsonObject result;
  result.insert(QStringLiteral("widgets"), QApplication::allWidgets().size());
  const auto cache = adqt::icons::cacheStatistics();
  result.insert(QStringLiteral("icon_cache_bytes"), double(cache.costBytes));
  result.insert(QStringLiteral("icon_cache_entries"), cache.entryCount);
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  require(
      GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                           sizeof(memory)) != FALSE,
      "read process memory");
  result.insert(QStringLiteral("private_bytes"), double(memory.PrivateUsage));
  result.insert(QStringLiteral("working_set_bytes"), double(memory.WorkingSetSize));
  result.insert(QStringLiteral("user_objects"),
                int(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)));
  result.insert(QStringLiteral("gdi_objects"),
                int(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)));
#endif
  return result;
}

// The Snow Shot pinned root has 17 entries and three initially empty submenu
// shells. Use the public library API and representative icons, without creating
// capture/OCR services or decoding a skin during this shared-menu measurement.
void populateMenu(Menu& menu, int extraItems) {
  menu.setNativeMenuEnabled(false);
  menu.setFixedWidth(300);
  menu.addItem(QStringLiteral("Copy to clipboard"), icons::Copy());
  menu.addItem(QStringLiteral("Copy original content"), icons::FileImage());
  menu.addItem(QStringLiteral("Save as file"), icons::Save());
  auto* ocr = menu.addItem(QStringLiteral("Display text recognition results"), icons::FileText());
  ocr->setCheckable(true);
  ocr->setEnabled(false);
  menu.addSeparator();
  menu.addItem(QStringLiteral("Annotation mode"), icons::Edit())->setCheckable(true);
  const auto deferred = [](Menu* child) {
    for (int i = 0; i < 20; ++i) child->addItem(QStringLiteral("Deferred item %1").arg(i));
  };
  menu.addLazySubMenu(QStringLiteral("Process image"), deferred, icons::Picture());
  menu.addSeparator();
  menu.addLazySubMenu(QStringLiteral("Group: Default"), deferred, icons::Folder());
  menu.addItem(QStringLiteral("Thumbnail mode"), icons::Compress())->setCheckable(true);
  menu.addItem(QStringLiteral("Hide to Top"), icons::ArrowUp())->setCheckable(true);
  menu.addItem(QStringLiteral("Click-through"), icons::Drag())->setCheckable(true);
  menu.addLazySubMenu(QStringLiteral("Window Management"), deferred, icons::Apartment());
  menu.addSeparator();
  menu.addItem(QStringLiteral("Show main interface"), icons::Desktop());
  menu.addItem(QStringLiteral("Close"), icons::Close());
  auto* destroy = menu.addItem(QStringLiteral("Destroy"), icons::IconDelete());
  menu.setActionDanger(destroy);
  const auto actions = menu.actions();
  for (qsizetype i = 0; i < actions.size(); ++i) {
    if (i % 3 == 0 && !actions[i]->isSeparator())
      actions[i]->setText(actions[i]->text() + QStringLiteral("\tCtrl+Shift+C"));
  }
  for (int i = 0; i < extraItems; ++i)
    menu.addItem(QStringLiteral("Additional item %1\tCtrl+Alt+Shift+F12").arg(i));
}

double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  const auto index = std::size_t(std::ceil(fraction * double(values.size())) - 1.0);
  return values.at(index);
}
}  // namespace

int main(int argc, char** argv) {
  BenchmarkApplication app(argc, argv);
  QApplication::setQuitOnLastWindowClosed(false);
  adqt::theme::ThemeManager::instance().applyTo(app);
  QCommandLineParser parser;
  parser.addHelpOption();
  const QCommandLineOption native(
      QStringLiteral("native"),
      QStringLiteral("Measure a platform window through its first paint."));
  const QCommandLineOption iterations(QStringLiteral("iterations"),
                                      QStringLiteral("Popup sessions."), QStringLiteral("count"),
                                      QStringLiteral("100"));
  const QCommandLineOption extra(QStringLiteral("extra-items"),
                                 QStringLiteral("Additional root items."), QStringLiteral("count"),
                                 QStringLiteral("0"));
  parser.addOptions({native, iterations, extra});
  parser.process(app);
  try {
    bool countValid = false;
    bool extraValid = false;
    const int count = parser.value(iterations).toInt(&countValid);
    const int extraItems = parser.value(extra).toInt(&extraValid);
    require(countValid && count >= 1 && count <= 10000, "iterations must be between 1 and 10000");
    require(extraValid && extraItems >= 0 && extraItems <= 1000,
            "extra items must be between 0 and 1000");
    const bool nativeWindow = parser.isSet(native);
    QWidget owner;
    owner.resize(640, 480);
    if (nativeWindow) {
      owner.show();
      drainRetirement();
    }
    const auto before = memorySnapshot();
    const auto idleWidgets = QApplication::allWidgets().size();
    QJsonArray samples;
    QJsonObject firstOpenMemory;
    QJsonObject firstClosedMemory;
    std::vector<double> subsequentTimes;
    quint64 rasterChecksum = 0;
    for (int i = 0; i < count; ++i) {
      QElapsedTimer timer;
      timer.start();
      QPointer<Menu> menu = new Menu(&owner);
      menu->setDeleteOnHide();
      const qint64 constructedNs = timer.nsecsElapsed();
      populateMenu(*menu, extraItems);
      const qint64 populatedNs = timer.nsecsElapsed();
      qint64 layoutNs = 0;
      qint64 completedNs = 0;
      if (nativeWindow) {
        app.measuredMenu = menu;
        app.measuredTimer = &timer;
        app.firstPaintNs = 0;
        menu->popupAt(owner.mapToGlobal(QPoint(80, 50)));
        layoutNs = timer.nsecsElapsed();
        while (app.firstPaintNs == 0 && timer.elapsed() < 5000) QCoreApplication::processEvents();
        require(app.firstPaintNs > 0, "popup must finish its first paint");
        completedNs = app.firstPaintNs;
        app.measuredTimer = nullptr;
        app.measuredMenu.clear();
      } else {
        menu->ensurePolished();
        menu->resize(menu->sizeHint());
        layoutNs = timer.nsecsElapsed();
        const qreal dpr = owner.devicePixelRatioF();
        QImage image(QSize(qCeil(menu->width() * dpr), qCeil(menu->height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        {
          QPainter painter(&image);
          menu->render(&painter);
        }
        completedNs = timer.nsecsElapsed();
        rasterChecksum += image.pixel(image.width() / 2, image.height() / 2);
      }
      const double totalMs = double(completedNs) / 1e6;
      if (i > 0) subsequentTimes.push_back(totalMs);
      samples.append(
          QJsonObject{{QStringLiteral("construct_ms"), double(constructedNs) / 1e6},
                      {QStringLiteral("populate_ms"), double(populatedNs - constructedNs) / 1e6},
                      {QStringLiteral("layout_or_popup_ms"), double(layoutNs - populatedNs) / 1e6},
                      {QStringLiteral("paint_ms"), double(completedNs - layoutNs) / 1e6},
                      {QStringLiteral("total_ms"), totalMs}});
      if (i == 0) firstOpenMemory = memorySnapshot();
      menu->dismissPopup();
      drainRetirement();
      require(!menu && QApplication::allWidgets().size() == idleWidgets,
              "closed sessions must release their menu tree and popup widgets");
      if (i == 0) firstClosedMemory = memorySnapshot();
    }
    QJsonObject result{{QStringLiteral("platform"), QGuiApplication::platformName()},
                       {QStringLiteral("native_window"), nativeWindow},
                       {QStringLiteral("device_pixel_ratio"), owner.devicePixelRatioF()},
                       {QStringLiteral("extra_items"), extraItems},
                       {QStringLiteral("iterations"), count},
                       {QStringLiteral("first_open_ms"),
                        samples.first().toObject().value(QStringLiteral("total_ms"))},
                       {QStringLiteral("samples"), samples},
                       {QStringLiteral("memory_before"), before},
                       {QStringLiteral("memory_first_open"), firstOpenMemory},
                       {QStringLiteral("memory_first_closed"), firstClosedMemory},
                       {QStringLiteral("memory_final_closed"), memorySnapshot()},
                       {QStringLiteral("raster_checksum"), double(rasterChecksum)}};
    if (!subsequentTimes.empty()) {
      result.insert(QStringLiteral("subsequent_p50_ms"), percentile(subsequentTimes, 0.5));
      result.insert(QStringLiteral("subsequent_p95_ms"), percentile(subsequentTimes, 0.95));
    }
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Indented).constData();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
