#include "widgets/detail/timing_hub.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace adqt::widgets::detail;
static void require(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  for (int scenario = 0; scenario < 4; ++scenario) {
    QObject first;
    auto* second = new QObject;
    std::vector<int> order;
    QEventLoop loop;
    scheduleTimingTask(&first, "first", 0, [&]() {
      order.push_back(1);
      if (scenario == 0) {
        delete second;
        second = nullptr;
      }
      if (scenario == 1) {
        cancelTimingTask(second, "second");
      }
      if (scenario == 2) {
        scheduleTimingTask(second, "second", 0, [&]() { order.push_back(4); });
      }
      scheduleTimingTask(&first, "next", 0, [&]() {
        order.push_back(5);
        loop.quit();
      });
    });
    scheduleTimingTask(second, "second", 0, [&]() { order.push_back(2); });
    scheduleTimingTask(&first, "third", 0, [&]() { order.push_back(3); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    QThread::msleep(10);  // All original deadlines must be due in the same dispatch.
    loop.exec();
    const std::vector<int> expected = scenario == 2   ? std::vector<int>{1, 3, 4, 5}
                                      : scenario == 3 ? std::vector<int>{1, 2, 3, 5}
                                                      : std::vector<int>{1, 3, 5};
    require(order == expected, "due tasks must respect ordering, cancellation and lifetime");
    delete second;
  }
  return 0;
}
