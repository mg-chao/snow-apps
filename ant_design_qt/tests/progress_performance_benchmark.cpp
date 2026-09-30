#include "widgets/progress.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>

#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using adqt::widgets::AdProgress;
constexpr int kWidgetCount = 128;
constexpr int kRenderIterations = 40;

enum class Scenario { Line, Circle, Dashboard, LineSteps, CircleSteps, Gradient };

const char* scenarioName(Scenario scenario) {
  switch (scenario) {
    case Scenario::Line:
      return "line";
    case Scenario::Circle:
      return "circle";
    case Scenario::Dashboard:
      return "dashboard";
    case Scenario::LineSteps:
      return "line-steps";
    case Scenario::CircleSteps:
      return "circle-steps";
    case Scenario::Gradient:
      return "circle-gradient";
  }
  return "unknown";
}

void configure(AdProgress& progress, Scenario scenario, int index) {
  progress.setAnimationEnabled(false);
  progress.setPercent(index % 101);
  const bool circle = scenario != Scenario::Line && scenario != Scenario::LineSteps;
  progress.setType(scenario == Scenario::Dashboard ? AdProgress::Type::Dashboard
                   : circle                        ? AdProgress::Type::Circle
                                                   : AdProgress::Type::Line);
  progress.resize(circle ? QSize(120, 120) : QSize(240, 24));
  if (scenario == Scenario::LineSteps || scenario == Scenario::CircleSteps) {
    progress.setSteps(24);
    progress.setStepGap(2);
  }
  if (scenario == Scenario::Gradient) {
    progress.setStrokeGradient(
        {{0, QColor("#108ee9")}, {0.5, QColor("#36cfc9")}, {1, QColor("#87d068")}});
  }
  progress.ensurePolished();
}

void runScenario(Scenario scenario, bool updateValues, qreal dpr) {
  std::vector<std::unique_ptr<AdProgress>> widgets;
  widgets.reserve(kWidgetCount);
  QElapsedTimer construction;
  construction.start();
  for (int index = 0; index < kWidgetCount; ++index) {
    auto progress = std::make_unique<AdProgress>();
    configure(*progress, scenario, index);
    widgets.push_back(std::move(progress));
  }
  const qint64 constructionNs = construction.nsecsElapsed();
  const QSize logicalSize = widgets.front()->size();
  QImage buffer(QSize(qCeil(logicalSize.width() * dpr), qCeil(logicalSize.height() * dpr)),
                QImage::Format_ARGB32_Premultiplied);
  buffer.setDevicePixelRatio(dpr);
  buffer.fill(Qt::transparent);
  QPainter painter(&buffer);
  for (auto& progress : widgets) {
    progress->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
  }
  QElapsedTimer rendering;
  rendering.start();
  for (int iteration = 0; iteration < kRenderIterations; ++iteration) {
    for (int index = 0; index < kWidgetCount; ++index) {
      AdProgress& progress = *widgets[static_cast<std::size_t>(index)];
      progress.setPercent(updateValues ? (iteration + index) % 101 : progress.percent());
      progress.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    }
  }
  const qint64 renderingNs = rendering.nsecsElapsed();
  painter.end();
  int childCount = 0;
  for (auto& progress : widgets) {
    childCount += static_cast<int>(progress->findChildren<QWidget*>().size());
  }
  const qreal count = kWidgetCount * kRenderIterations;
  std::cout << scenarioName(scenario) << "," << (updateValues ? "update-render" : "steady-render")
            << "," << dpr << "," << kWidgetCount << "," << kRenderIterations << ","
            << static_cast<qreal>(constructionNs) / (1000.0 * kWidgetCount) << ","
            << static_cast<qreal>(renderingNs) / (1000.0 * count) << "," << childCount << std::endl;
  // Include an observable raster result so optimized builds still carry out every render.
  if (buffer.isNull()) std::terminate();
}

}  // namespace

int main(int argc, char** argv) {
#ifndef NDEBUG
  std::cerr << "Progress performance measurements require a Release build\n";
  return 1;
#else
  QApplication app(argc, argv);
  adqt::theme::ThemeManager::instance().applyTo(app);
  std::cout << std::fixed << std::setprecision(3)
            << "scenario,operation,dpr,widgets,iterations,construct_us_per_widget,"
               "us_per_render,child_widgets\n";
  for (qreal dpr : {1.0, 2.0}) {
    for (Scenario scenario : {Scenario::Line, Scenario::Circle, Scenario::Dashboard,
                              Scenario::LineSteps, Scenario::CircleSteps, Scenario::Gradient}) {
      for (bool update : {false, true}) runScenario(scenario, update, dpr);
    }
  }
  return 0;
#endif
}
