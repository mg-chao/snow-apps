#include "widgets/slider.h"

#include <QAccessible>
#include <QApplication>
#include <QImage>
#include <QTest>

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using adqt::widgets::AdSlider;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class LayoutObserver final : public QObject {
 public:
  int requests = 0;
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::LayoutRequest) ++requests;
    return false;
  }
};

QImage render(AdSlider& slider) {
  slider.show();
  QCoreApplication::processEvents();
  return slider.grab().toImage().convertToFormat(QImage::Format_ARGB32);
}

void configure(AdSlider& slider) {
  slider.resize(220, 40);
  slider.setRange(-24, 24);
  slider.setValue(0);
  slider.setTooltipEnabled(false);
  AdSlider::ComponentTokens tokens;
  tokens.controlSize = 40;
  tokens.railSize = 8;
  tokens.handleSize = 12;
  tokens.handleSizeHover = 12;
  tokens.marginMain = 8;
  tokens.marginCross = 0;
  tokens.railBg = QColor(20, 20, 20);
  tokens.railHoverBg = QColor(20, 20, 20);
  tokens.trackBg = QColor(0, 200, 0);
  tokens.trackHoverBg = QColor(0, 200, 0);
  tokens.handleColor = QColor(200, 0, 200);
  tokens.handleActiveColor = QColor(200, 0, 200);
  slider.setComponentTokens(tokens);
}

bool green(const QImage& image, QPoint point) {
  const QColor color = image.pixelColor(point);
  return color.green() > 100 && color.red() < 80 && color.blue() < 80;
}

void independentTrackPreservesInputAndAccessibleValue() {
  AdSlider slider;
  configure(slider);
  render(slider);
  require(slider.trackFillRatio() == -1, "default track follows gain");
  int changes = 0;
  int moves = 0;
  QObject::connect(&slider, &AdSlider::valueChanged, [&changes](double) { ++changes; });
  QObject::connect(&slider, &AdSlider::sliderPositionChanged, [&moves](double) { ++moves; });
  LayoutObserver observer;
  slider.installEventFilter(&observer);
  slider.setTrackFillRatio(1);
  QImage full = render(slider);
  require(green(full, {170, 20}), "full meter reaches beyond centered gain thumb");
  slider.setTrackFillRatio(0);
  QImage empty = render(slider);
  require(!green(empty, {40, 20}), "empty meter does not fill the gain track");
  slider.setTrackFillRatio(0.25);
  QImage quarter = render(slider);
  require(green(quarter, {35, 20}) && !green(quarter, {80, 20}),
          "quarter meter fills independently of gain position");
  require(slider.value() == 0 && slider.sliderPosition() == 0 && changes == 0 && moves == 0,
          "meter updates never move or publish the gain thumb");
  auto* accessible = QAccessible::queryAccessibleInterface(&slider);
  require(accessible && accessible->valueInterface() &&
              accessible->valueInterface()->currentValue().toDouble() == 0,
          "accessibility value remains gain");
  require(observer.requests == 0, "meter ticks do not request layout");
  QTest::mouseClick(&slider, Qt::LeftButton, Qt::NoModifier, {170, 20});
  require(slider.value() > 0 && slider.trackFillRatio() == 0.25,
          "track clicks still edit gain without editing the meter");
  slider.setValue(0);
  slider.resetTrackFillRatio();
  QImage normal = render(slider);
  require(green(normal, {60, 20}) && !green(normal, {170, 20}),
          "reset restores handle-dependent track painting");
}

void meterFollowsVisualAxis() {
  AdSlider slider;
  configure(slider);
  slider.setTrackFillRatio(0.25);
  slider.setLayoutDirection(Qt::RightToLeft);
  QImage rtl = render(slider);
  require(green(rtl, {185, 20}) && !green(rtl, {140, 20}), "RTL meter fills from visual minimum");
  slider.setInvertedAppearance(true);
  QImage reverse = render(slider);
  require(green(reverse, {35, 20}) && !green(reverse, {80, 20}),
          "inverted RTL meter uses the same axis as the thumb");
  slider.setLayoutDirection(Qt::LeftToRight);
  slider.setInvertedAppearance(false);
  slider.setOrientation(Qt::Vertical);
  slider.resize(40, 220);
  QImage vertical = render(slider);
  require(green(vertical, {20, 185}) && !green(vertical, {20, 140}),
          "vertical meter fills upwards from minimum");
  slider.setTrackFillRatio(-3);
  require(slider.trackFillRatio() == 0, "negative meter values clamp to zero");
  slider.setTrackFillRatio(2);
  require(slider.trackFillRatio() == 1, "large meter values clamp to one");
  slider.setTrackFillRatio(std::numeric_limits<double>::quiet_NaN());
  require(slider.trackFillRatio() == 1, "non-finite meter values are ignored");
}

void unlabeledMarkPreservesTrackLayout() {
  AdSlider slider;
  configure(slider);
  slider.setValue(12);
  slider.setTrackFillRatio(1);
  const QSize horizontalHint = slider.sizeHint();
  const QImage baseline = render(slider);
  AdSlider::Mark mark;
  mark.labelVisible = false;
  slider.setMarks({{0.0, mark}});
  require(slider.sizeHint() == horizontalHint,
          "unlabeled mark does not reserve horizontal label space");
  const QImage marked = render(slider);
  require(marked.pixelColor(110, 20) != baseline.pixelColor(110, 20),
          "unlabeled zero mark remains visible at the gain midpoint");
  slider.setMarkIndicatorsVisible(false);
  require(render(slider) == baseline,
          "hidden mark label paints no numeric fallback and leaves the rail in place");
  int changes = 0;
  QObject::connect(&slider, &AdSlider::marksChanged, [&changes]() { ++changes; });
  mark.labelVisible = true;
  slider.setMarks({{0.0, mark}});
  require(changes == 1 && slider.sizeHint().height() > horizontalHint.height(),
          "changing label visibility restores the default numeric label layout");
  slider.clearMarks();
  slider.setOrientation(Qt::Vertical);
  slider.resize(40, 220);
  const QSize verticalHint = slider.sizeHint();
  const QImage verticalBaseline = render(slider);
  mark.labelVisible = false;
  slider.setMarks({{0.0, mark}});
  require(slider.sizeHint() == verticalHint && render(slider) == verticalBaseline,
          "unlabeled vertical mark reserves no label space or numeric fallback");
}
}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  independentTrackPreservesInputAndAccessibleValue();
  meterFollowsVisualAxis();
  unlabeledMarkPreservesTrackLayout();
  return 0;
}
