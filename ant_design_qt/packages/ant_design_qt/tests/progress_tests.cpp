#include "widgets/progress.h"
#include "theme/theme_manager.h"
#include "widgets/detail/timing_hub.h"

#include <QAccessible>
#include <QAccessibleValueInterface>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QTranslator>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>

namespace {

using adqt::theme::ThemeManager;
using adqt::theme::ThemeScheme;
using adqt::widgets::AdProgress;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool near(qreal actual, qreal expected, qreal tolerance = 0.001) {
  return std::abs(actual - expected) <= tolerance;
}

void diagnosticStyle(AdProgress& progress) {
  progress.setAnimationEnabled(false);
  progress.setShowInfo(false);
  progress.setStatus(AdProgress::Status::Normal);
  progress.setStrokeColor(Qt::red);
  progress.setRailColor(Qt::blue);
  progress.setSuccessColor(Qt::green);
  AdProgress::SemanticStyles styles;
  styles.root.backgroundColor = Qt::black;
  progress.setSemanticStyles(styles);
}

QImage render(QWidget& widget, qreal dpr = 1.0) {
  widget.ensurePolished();
  QImage image(QSize(qCeil(widget.width() * dpr), qCeil(widget.height() * dpr)),
               QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(dpr);
  image.fill(Qt::black);
  QPainter painter(&image);
  widget.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
  return image;
}

bool dominant(const QColor& color, int channel) {
  const int components[] = {color.red(), color.green(), color.blue()};
  return components[channel] > 150 && components[channel] > components[(channel + 1) % 3] + 90 &&
         components[channel] > components[(channel + 2) % 3] + 90;
}

QColor at(const QImage& image, qreal x, qreal y) {
  const qreal dpr = image.devicePixelRatio();
  return image.pixelColor(std::clamp(qFloor(x * dpr), 0, image.width() - 1),
                          std::clamp(qFloor(y * dpr), 0, image.height() - 1));
}

struct ColorCoverage {
  qreal weight = 0;
  QPointF center;
};

ColorCoverage coverage(const QImage& image, int channel) {
  ColorCoverage result;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor color = image.pixelColor(x, y);
      const int components[] = {color.red(), color.green(), color.blue()};
      const qreal weight =
          std::max(0, components[channel] -
                          std::max(components[(channel + 1) % 3], components[(channel + 2) % 3])) /
          255.0;
      result.weight += weight;
      result.center += QPointF(x + 0.5, y + 0.5) * weight;
    }
  }
  if (result.weight > 0) result.center /= result.weight * image.devicePixelRatio();
  result.weight /= image.devicePixelRatio() * image.devicePixelRatio();
  return result;
}

ColorCoverage centerCoverage(const QImage& image, int channel) {
  const int inset = image.width() / 3;
  QImage center = image.copy(inset, inset, image.width() - 2 * inset, image.height() - 2 * inset);
  center.setDevicePixelRatio(image.devicePixelRatio());
  return coverage(center, channel);
}

int horizontalRuns(const QImage& image, int channel) {
  int runs = 0;
  bool previous = false;
  const int y = image.height() / 2;
  for (int x = 0; x < image.width(); ++x) {
    const bool current = dominant(image.pixelColor(x, y), channel);
    if (current && !previous) ++runs;
    previous = current;
  }
  return runs;
}

void valueAndSignalInvariants() {
  AdProgress progress;
  require(progress.status() == AdProgress::Status::Automatic &&
              progress.effectiveStatus() == AdProgress::Status::Normal,
          "the default status must infer normal progress");
  require(near(progress.percent(), 0) && near(progress.successPercent(), -1),
          "the initial value and unset success segment must be stable");
  int percentSignals = 0;
  int displayedSignals = 0;
  int successSignals = 0;
  int statusSignals = 0;
  int effectiveSignals = 0;
  QObject::connect(&progress, &AdProgress::percentChanged, [&] { ++percentSignals; });
  QObject::connect(&progress, &AdProgress::displayedPercentChanged, [&] { ++displayedSignals; });
  QObject::connect(&progress, &AdProgress::successPercentChanged, [&] { ++successSignals; });
  QObject::connect(&progress, &AdProgress::statusChanged, [&] { ++statusSignals; });
  QObject::connect(&progress, &AdProgress::effectiveStatusChanged, [&] { ++effectiveSignals; });

  progress.setPercent(37.5);
  progress.setPercent(37.5);
  require(near(progress.percent(), 37.5) && near(progress.displayedPercent(), 37.5) &&
              percentSignals == 1 && displayedSignals == 1,
          "a hidden value update must snap once and identical updates must emit nothing");
  for (qreal invalid :
       {std::numeric_limits<qreal>::quiet_NaN(), std::numeric_limits<qreal>::infinity(),
        -std::numeric_limits<qreal>::infinity()}) {
    progress.setPercent(invalid);
    progress.setSuccessPercent(invalid);
  }
  require(near(progress.percent(), 37.5) && near(progress.successPercent(), -1) &&
              percentSignals == 1 && successSignals == 0,
          "nonfinite percentages must leave state and notifications unchanged");
  progress.setPercent(120);
  progress.setPercent(150);
  require(near(progress.percent(), 100) && percentSignals == 2 && effectiveSignals == 1 &&
              progress.effectiveStatus() == AdProgress::Status::Success,
          "percent must clamp before equality checks and automatic completion");
  progress.setStatus(AdProgress::Status::Normal);
  progress.setStatus(AdProgress::Status::Normal);
  require(statusSignals == 1 && progress.effectiveStatus() == AdProgress::Status::Normal,
          "explicit normal must remain normal at one hundred percent");
  progress.setStatus(AdProgress::Status::Active);
  require(progress.effectiveStatus() == AdProgress::Status::Active,
          "explicit active must remain active at one hundred percent");
  progress.setStatus(AdProgress::Status::Exception);
  progress.setPercent(-5);
  require(
      near(progress.percent(), 0) && progress.effectiveStatus() == AdProgress::Status::Exception,
      "explicit exception must survive a clamped empty percentage");
  progress.setStatus(AdProgress::Status::Success);
  require(progress.effectiveStatus() == AdProgress::Status::Success,
          "explicit success must survive an incomplete percentage");
  progress.setStatus(AdProgress::Status::Automatic);
  progress.setPercent(100);
  progress.setSuccessPercent(30);
  require(progress.effectiveStatus() == AdProgress::Status::Normal,
          "automatic status must use a configured success segment before the total");
  progress.setSuccessPercent(101);
  progress.setSuccessPercent(140);
  require(near(progress.successPercent(), 100) && successSignals == 2 &&
              progress.effectiveStatus() == AdProgress::Status::Success,
          "the success segment must clamp and infer completion only once");
  progress.setSuccessPercent(-7);
  progress.clearSuccess();
  require(near(progress.successPercent(), -1) && successSignals == 3,
          "negative success percentages and clear must share the unset sentinel");

  progress.setStrokeWidth(-4);
  progress.setSteps(-9);
  progress.setStepGap(-3);
  progress.setGapDegree(-9);
  require(near(progress.strokeWidth(), 0) && progress.steps() == 0 && near(progress.stepGap(), 0) &&
              near(progress.gapDegree(), -1),
          "invalid finite dimensions must normalize to documented bounds");
  progress.setStrokeWidth(12);
  progress.setStepGap(3);
  progress.setGapDegree(400);
  progress.setSteps(20000);
  require(progress.steps() == 10000 && near(progress.gapDegree(), 295),
          "segment count and dashboard gap must be bounded");
  progress.setStrokeWidth(std::numeric_limits<qreal>::quiet_NaN());
  progress.setStepGap(std::numeric_limits<qreal>::infinity());
  progress.setGapDegree(-std::numeric_limits<qreal>::infinity());
  require(near(progress.strokeWidth(), 12) && near(progress.stepGap(), 3) &&
              near(progress.gapDegree(), 295),
          "nonfinite geometry must preserve the last valid geometry");
}

void accessibilityAndFormatting() {
  AdProgress progress;
  progress.setAccessibleName(QStringLiteral("Upload progress"));
  progress.setPercent(42.5);
  progress.setSuccessPercent(15);
  QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(&progress);
  require(interface && interface->role() == QAccessible::ProgressBar,
          "progress must expose the platform progress bar role");
  require(interface->text(QAccessible::Name) == QStringLiteral("Upload progress"),
          "the accessible interface must preserve an explicit accessible name");
  QAccessibleValueInterface* value = interface->valueInterface();
  require(value && near(value->currentValue().toDouble(), 42.5) &&
              near(value->minimumValue().toDouble(), 0) &&
              near(value->maximumValue().toDouble(), 100),
          "assistive technology must receive the actual percentage and bounds");
  value->setCurrentValue(88);
  require(near(progress.percent(), 42.5), "the passive value interface must remain read only");
  progress.setPercent(73.25);
  require(near(value->currentValue().toDouble(), 73.25),
          "accessible values must follow later changes");
  require(progress.focusPolicy() == Qt::NoFocus,
          "a passive progress indicator must not take keyboard focus");

  int formatCalls = 0;
  qreal formattedPercent = -1;
  qreal formattedSuccess = -1;
  progress.setFormat([&](qreal percent, qreal success) {
    ++formatCalls;
    formattedPercent = percent;
    formattedSuccess = success;
    return QStringLiteral("%1 of %2").arg(percent).arg(success);
  });
  require(progress.formattedText() == QStringLiteral("73.25 of 15") &&
              near(formattedPercent, 73.25) && near(formattedSuccess, 15),
          "format callbacks must receive normalized logical values");
  progress.resize(260, 30);
  render(progress);  // Polish may resolve a platform font before the content cache settles.
  const int cachedCalls = formatCalls;
  for (int index = 0; index < 4; ++index) {
    render(progress);
    progress.sizeHint();
    progress.minimumSizeHint();
    progress.formattedText();
  }
  require(formatCalls == cachedCalls,
          "painting and repeated layout queries must reuse formatted content");
  progress.setSuccessPercent(20);
  require(progress.formattedText() == QStringLiteral("73.25 of 20") && formatCalls > cachedCalls,
          "success updates must invalidate formatted content");
  progress.resetFormat();
  require(progress.formattedText().contains(QStringLiteral("73")) &&
              progress.formattedText().endsWith(QLatin1Char('%')),
          "resetting a formatter must restore the percentage text");
  progress.setType(AdProgress::Type::Circle);
  progress.setProgressSize(QSize(16, 16));
  progress.resize(16, 16);
  render(progress);
  require(!progress.toolTip().isEmpty(),
          "micro circles must retain progress information in a tooltip");
}

class ProgressTranslator final : public QTranslator {
 public:
  bool isEmpty() const override { return false; }
  QString translate(const char* context, const char* sourceText, const char*, int) const override {
    if (std::strcmp(context, "adqt::widgets::AdProgress") == 0 &&
        std::strcmp(sourceText, "%1%") == 0) {
      return QStringLiteral("%1 percent");
    }
    return QString();
  }
};

void languageChangeRefreshesCachedText(QApplication& app) {
  AdProgress progress;
  progress.setPercent(45);
  progress.setType(AdProgress::Type::Circle);
  progress.setProgressSize(QSize(16, 16));
  progress.resize(16, 16);
  ProgressTranslator translator;
  require(app.installTranslator(&translator), "the deterministic test translator must install");
  QEvent change(QEvent::LanguageChange);
  QCoreApplication::sendEvent(&progress, &change);
  require(progress.formattedText() == QStringLiteral("45 percent") &&
              progress.toolTip() == QStringLiteral("45 percent"),
          "language changes must refresh cached percentages and automatic micro tooltips");
  app.removeTranslator(&translator);
  QCoreApplication::sendEvent(&progress, &change);
  require(progress.formattedText() == QStringLiteral("45%"),
          "removing a translator must restore the source percentage text");
}

void sizingAndSmallGeometry() {
  AdProgress progress;
  progress.setAnimationEnabled(false);
  const QSize outer = progress.minimumSizeHint();
  progress.setShowInfo(false);
  require(outer.width() > progress.minimumSizeHint().width(),
          "outer percentage information must participate in the minimum line width");
  require(!progress.hasHeightForWidth(), "line progress must behave as a horizontal control");
  progress.setProgressSize(QSize(180, 12));
  require(progress.sizeHint().width() >= 180 && progress.sizeHint().height() >= 12,
          "custom line dimensions must participate in the preferred size");
  progress.setStrokeWidth(30);
  require(progress.sizeHint().height() == 12,
          "explicit line height must take precedence over legacy stroke width");
  progress.setSteps(5);
  require(progress.sizeHint().height() == 12,
          "explicit step height must take precedence over legacy stroke width");
  progress.setShowInfo(true);
  progress.setPercentPlacement(AdProgress::PercentPlacement::Inner);
  require(progress.minimumSizeHint().width() > 10,
          "step minimum size must reserve its outer information even with inner requested");
  progress.setShowInfo(false);
  progress.setSteps(0);
  progress.setStrokeWidth(0);
  progress.setPercentPlacement(AdProgress::PercentPlacement::Outer);
  progress.setProgressSize(QSize());
  progress.setType(AdProgress::Type::Circle);
  require(progress.hasHeightForWidth() && progress.heightForWidth(90) == 90,
          "circle progress must preserve a square layout through height for width");
  progress.setSizeClass(AdProgress::Size::Middle);
  const QSize middle = progress.sizeHint();
  progress.setSizeClass(AdProgress::Size::Small);
  const QSize small = progress.sizeHint();
  require(small.width() < middle.width() && small.width() == small.height() &&
              middle.width() == middle.height(),
          "small and middle circles must have distinct square preferred sizes");
  progress.setType(AdProgress::Type::Dashboard);
  require(progress.hasHeightForWidth() && progress.heightForWidth(75) == 75,
          "dashboard progress must preserve its square layout");
  for (AdProgress::Type type :
       {AdProgress::Type::Line, AdProgress::Type::Circle, AdProgress::Type::Dashboard}) {
    progress.setType(type);
    for (const QSize size : {QSize(1, 1), QSize(7, 3), QSize(17, 17), QSize(200, 31)}) {
      progress.resize(size);
      for (int steps : {0, 5, 10000}) {
        progress.setSteps(steps);
        progress.setStrokeWidth(200);
        progress.setStepGap(100);
        const QImage image = render(progress, 1.25);
        require(!image.isNull() && image.width() == qCeil(size.width() * 1.25),
                "tiny and overconstrained progress geometry must render safely");
      }
    }
  }
}

void lineRenderingAndRtl() {
  AdProgress progress;
  diagnosticStyle(progress);
  progress.resize(200, 24);
  progress.setStrokeWidth(8);
  progress.setStrokeLinecap(AdProgress::StrokeLinecap::Butt);
  progress.setPercent(35);
  for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
    progress.setLayoutDirection(Qt::LeftToRight);
    const QImage left = render(progress, dpr);
    require(dominant(at(left, 20, 12), 0) && dominant(at(left, 160, 12), 2),
            "line progress must paint the proportional track and remaining rail");
    const ColorCoverage track = coverage(left, 0);
    require(near(track.center.y(), 12, 0.3) && track.center.x() < 50,
            "line strokes must remain centered at integer and fractional device scales");
    progress.setLayoutDirection(Qt::RightToLeft);
    const QImage right = render(progress, dpr);
    require(dominant(at(right, 180, 12), 0) && dominant(at(right, 20, 12), 2),
            "right to left progress must fill from the logical start");
    require(near(coverage(right, 0).center.x() + track.center.x(), 200, 1),
            "right to left line geometry must mirror the painted track");
    progress.setLayoutDirection(Qt::LeftToRight);
    progress.setSuccessPercent(15);
    const QImage success = render(progress, dpr);
    require(dominant(at(success, 15, 12), 1) && dominant(at(success, 50, 12), 0) &&
                dominant(at(success, 160, 12), 2),
            "a line success segment must share the track start and preserve remaining progress");
    progress.setSuccessPercent(60);
    require(dominant(at(render(progress, dpr), 100, 12), 1),
            "a separate success segment may extend beyond the total percentage");
    progress.clearSuccess();
  }
  progress.setPercent(0);
  require(coverage(render(progress), 0).weight < 0.1,
          "an empty line must not leak a round cap or residual track");
  progress.setPercent(100);
  const QImage full = render(progress);
  require(dominant(at(full, 2, 12), 0) && dominant(at(full, 197, 12), 0),
          "a complete explicit normal line must cover its entire rail");
}

void stepsAndGradients() {
  AdProgress progress;
  diagnosticStyle(progress);
  progress.setSteps(5);
  progress.setProgressSize(QSize(20, 8));
  progress.setStepGap(4);
  progress.resize(progress.sizeHint());
  progress.setPercent(50);
  require(horizontalRuns(render(progress), 0) == 3 && horizontalRuns(render(progress), 2) == 2,
          "line steps must round fifty percent to three of five filled segments");
  progress.setStepRounding([](qreal value) { return static_cast<int>(std::floor(value)); });
  require(horizontalRuns(render(progress), 0) == 2,
          "custom step rounding must determine the number of filled segments");
  progress.setStepRounding([](qreal) { return -10; });
  require(horizontalRuns(render(progress), 0) == 0,
          "negative custom rounding must clamp to no filled segments");
  progress.setStepRounding([](qreal) { return 1000; });
  require(horizontalRuns(render(progress), 0) == 5,
          "excessive custom rounding must clamp to the segment count");
  progress.resetStepRounding();
  progress.setPercent(100);
  progress.setStepColors({Qt::red, Qt::green, Qt::blue, Qt::red, Qt::green});
  const QImage colors = render(progress);
  require(horizontalRuns(colors, 0) == 2 && horizontalRuns(colors, 1) == 2 &&
              horizontalRuns(colors, 2) == 1,
          "individual step colors must follow the supplied order");
  progress.setStepColors({});
  progress.resize(300, 24);
  require(near(coverage(render(progress), 0).weight, 5 * 20 * 8, 1),
          "line steps must retain configured unit widths when their widget has extra space");
  progress.setSteps(0);
  progress.setProgressSize(QSize());
  progress.resize(200, 24);
  progress.setStrokeWidth(8);
  progress.setStrokeLinecap(AdProgress::StrokeLinecap::Butt);
  progress.setStrokeGradient(
      {{1.0, Qt::blue}, {0.0, Qt::red}, {std::numeric_limits<qreal>::quiet_NaN(), Qt::green}});
  require(progress.strokeGradient().size() == 2 && near(progress.strokeGradient().front().first, 0),
          "gradients must discard nonfinite stops and sort valid positions");
  const QImage gradient = render(progress);
  require(at(gradient, 20, 12).red() > at(gradient, 20, 12).blue() &&
              at(gradient, 180, 12).blue() > at(gradient, 180, 12).red(),
          "line gradients must follow the progress direction");
  progress.setLayoutDirection(Qt::RightToLeft);
  const QImage rightToLeft = render(progress);
  require(at(rightToLeft, 180, 12).red() > at(rightToLeft, 180, 12).blue() &&
              at(rightToLeft, 20, 12).blue() > at(rightToLeft, 20, 12).red(),
          "line gradients must begin at the logical start in right to left layouts");
  progress.setLayoutDirection(Qt::LeftToRight);
  progress.setGradientReversed(true);
  const QImage reversed = render(progress);
  require(at(reversed, 20, 12).blue() > at(reversed, 20, 12).red() &&
              at(reversed, 180, 12).red() > at(reversed, 180, 12).blue(),
          "explicit gradient reversal must swap the visual color direction");
  progress.setType(AdProgress::Type::Circle);
  progress.resize(120, 120);
  const QImage circular = render(progress, 2);
  require(coverage(circular, 0).weight > 100 && coverage(circular, 2).weight > 100,
          "a circular gradient must retain both ends across the sweep");
  require(at(circular, 115, 60).blue() > at(circular, 115, 60).red() &&
              at(circular, 5, 60).red() > at(circular, 5, 60).blue(),
          "a reversed circle gradient must interpolate clockwise from blue to red");
  progress.setGradientReversed(false);
  const QImage clockwise = render(progress);
  require(at(clockwise, 115, 60).red() > at(clockwise, 115, 60).blue() &&
              at(clockwise, 5, 60).blue() > at(clockwise, 5, 60).red(),
          "circle gradients must preserve their angular color ordering");
  progress.setType(AdProgress::Type::Dashboard);
  progress.setGapDegree(90);
  const QImage dashboard = render(progress);
  require(at(dashboard, 5, 60).red() > at(dashboard, 5, 60).blue() &&
              at(dashboard, 115, 60).blue() > at(dashboard, 115, 60).red(),
          "dashboard gradients must map their stops over the visible sweep");
  progress.setStrokeGradient({});
  require(coverage(render(progress), 2).weight < 0.1,
          "clearing a gradient must restore the explicit solid stroke");
}

void indicatorRegressions() {
  AdProgress progress;
  diagnosticStyle(progress);
  progress.setShowInfo(true);
  AdProgress::SemanticStyles styles;
  styles.root.backgroundColor = Qt::black;
  styles.indicator.backgroundColor = Qt::green;
  progress.setSemanticStyles(styles);
  progress.setType(AdProgress::Type::Circle);
  progress.resize(120, 120);
  progress.setPercent(50);
  for (qreal stroke : {50.0, 100.0}) {
    progress.setStrokeWidth(stroke);
    require(centerCoverage(render(progress), 1).weight > 10,
            "thick circles must retain a nonempty center indicator area");
    progress.setStatus(AdProgress::Status::Success);
    require(centerCoverage(render(progress), 1).weight > 10,
            "thick circles must retain the success indicator area");
    progress.setStatus(AdProgress::Status::Normal);
  }
  progress.setStrokeWidth(0);
  progress.setType(AdProgress::Type::Line);
  progress.setSteps(5);
  progress.resize(240, 24);
  progress.setPercentPlacement(AdProgress::PercentPlacement::Outer);
  const QImage outer = render(progress);
  progress.setPercentPlacement(AdProgress::PercentPlacement::Inner);
  require(render(progress) == outer && coverage(outer, 1).weight > 10,
          "line steps must preserve outer information when inner placement is requested");
  progress.setSteps(0);
  progress.setType(AdProgress::Type::Circle);
  progress.setProgressSize(QSize());
  progress.resize(16, 16);
  render(progress);
  require(!progress.toolTip().isEmpty(),
          "a naturally sized circle resized to a micro control must acquire a tooltip");
  progress.setToolTip(QStringLiteral("Application tooltip"));
  progress.resize(120, 120);
  progress.setPercent(60);
  require(progress.toolTip() == QStringLiteral("Application tooltip"),
          "resizing and updating progress must preserve an application supplied tooltip");

  progress.resetSemanticStyles();
  progress.setRailColor(Qt::blue);
  progress.setStrokeColor(Qt::blue);
  AdProgress::ComponentTokens tokens;
  tokens.colors.exceptionColor = Qt::red;
  progress.setComponentTokens(tokens);
  progress.setFormat([](qreal, qreal) { return QStringLiteral("Failed"); });
  progress.setStatus(AdProgress::Status::Exception);
  require(centerCoverage(render(progress), 0).weight > 1,
          "a custom circle formatter must retain the exception indicator color");
}

void circleAndDashboardRendering() {
  AdProgress progress;
  diagnosticStyle(progress);
  progress.setType(AdProgress::Type::Circle);
  progress.setStrokeWidth(8);
  progress.setPercent(100);
  progress.resize(100, 100);
  for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
    const QImage circle = render(progress, dpr);
    const ColorCoverage ring = coverage(circle, 0);
    require(ring.weight > 100 && near(ring.center.x(), 50, 0.2) && near(ring.center.y(), 50, 0.2) &&
                !dominant(at(circle, 50, 50), 0),
            "a full circle must render a concentric annulus at every device scale");
    progress.setPercent(25);
    const ColorCoverage quarter = coverage(render(progress, dpr), 0);
    require(quarter.center.x() > 65 && quarter.center.y() < 35 &&
                quarter.weight > ring.weight * 0.20 && quarter.weight < ring.weight * 0.32,
            "a quarter circle must begin at the top and sweep clockwise without distorting area");
    progress.setPercent(100);
  }
  progress.setPercent(0);
  require(coverage(render(progress), 0).weight < 0.1, "an empty circle must have no residual cap");
  progress.setSuccessPercent(50);
  const ColorCoverage success = coverage(render(progress), 1);
  require(success.weight > 100 && success.center.x() > 65,
          "circle success must render independently of an empty total");
  progress.clearSuccess();
  progress.setPercent(100);
  progress.setType(AdProgress::Type::Dashboard);
  progress.setGapDegree(90);
  progress.setStrokeLinecap(AdProgress::StrokeLinecap::Butt);
  for (AdProgress::GapPlacement placement :
       {AdProgress::GapPlacement::Top, AdProgress::GapPlacement::Bottom,
        AdProgress::GapPlacement::Start, AdProgress::GapPlacement::End}) {
    progress.setGapPlacement(placement);
    for (Qt::LayoutDirection direction : {Qt::LeftToRight, Qt::RightToLeft}) {
      progress.setLayoutDirection(direction);
      const QImage image = render(progress, 2);
      const QPointF sample =
          placement == AdProgress::GapPlacement::Top      ? QPointF(50, 4)
          : placement == AdProgress::GapPlacement::Bottom ? QPointF(50, 96)
          : (placement == AdProgress::GapPlacement::Start) == (direction == Qt::LeftToRight)
              ? QPointF(4, 50)
              : QPointF(96, 50);
      const QPointF opposite = QPointF(100, 100) - sample;
      require(!dominant(at(image, sample.x(), sample.y()), 0) &&
                  dominant(at(image, opposite.x(), opposite.y()), 0),
              "dashboard gaps must honor physical top/bottom and logical start/end in RTL");
    }
  }
  progress.setType(AdProgress::Type::Circle);
  progress.setGapDegree(0);
  progress.setSteps(8);
  progress.setStepGap(4);
  progress.setPercent(50);
  const QImage stepped = render(progress, 2);
  require(coverage(stepped, 0).weight > 100 && coverage(stepped, 2).weight > 100,
          "a stepped circle must retain filled and unfilled segments");
  const QColor beforeTop = at(stepped, 47, 4);
  require(!dominant(beforeTop, 0) && !dominant(beforeTop, 2) && dominant(at(stepped, 53, 4), 0),
          "circular steps must begin at the top and preserve the gap after each segment");
}

void repeatedCircularPaintsRetainGeometry() {
  AdProgress progress;
  diagnosticStyle(progress);
  progress.setType(AdProgress::Type::Circle);
  progress.resize(120, 120);
  progress.setPercent(37);
  for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
    const QImage first = render(progress, dpr);
    const QImage repeated = render(progress, dpr);
    require(first == repeated && repeated == render(progress, dpr),
            "repeated circular paints must preserve pixel geometry at every target scale");
  }
  progress.setPercent(73);
  const QImage changed = render(progress, 2);
  require(changed == render(progress, 2),
          "value changes must invalidate every previously painted circular raster");
  progress.setStrokeGradient({{0, Qt::red}, {1, Qt::green}});
  const QImage gradient = render(progress, 2);
  require(
      changed != gradient && gradient == render(progress, 2),
      "changing a circular gradient must invalidate the raster while retaining repeated output");
}

void denseStepsKeepQuantizedCompletion() {
  AdProgress progress;
  diagnosticStyle(progress);
  progress.setSteps(10000);
  progress.setStepGap(0);
  progress.setPercent(50);
  progress.resize(200, 24);
  const QImage line = render(progress, 2);
  require(dominant(at(line, 20, 12), 0) && dominant(at(line, 180, 12), 2),
          "dense zero-gap line steps must retain proportional completion");
  progress.setType(AdProgress::Type::Circle);
  progress.resize(100, 100);
  const QImage circle = render(progress, 2);
  require(dominant(at(circle, 96, 50), 0) && dominant(at(circle, 4, 50), 2),
          "dense zero-gap circular steps must retain quantized clockwise completion");
  progress.setPercent(0);
  require(coverage(render(progress, 2), 0).weight < 0.1,
          "empty dense steps must never paint a residual active cap");
}

void statusesThemesAndOverrides() {
  ThemeManager& manager = ThemeManager::instance();
  const auto original = manager.config();
  QWidget scope;
  AdProgress progress(&scope);
  progress.resize(200, 24);
  progress.setAnimationEnabled(false);
  progress.setShowInfo(false);
  progress.setPercent(55);
  progress.setStrokeLinecap(AdProgress::StrokeLinecap::Butt);
  AdProgress::ComponentTokens tokens;
  tokens.colors.defaultColor = Qt::red;
  tokens.colors.successColor = Qt::green;
  tokens.colors.exceptionColor = Qt::blue;
  progress.setComponentTokens(tokens);
  for (const auto pair :
       {std::pair{AdProgress::Status::Normal, 0}, std::pair{AdProgress::Status::Success, 1},
        std::pair{AdProgress::Status::Exception, 2}}) {
    progress.setStatus(pair.first);
    require(dominant(at(render(progress), 20, 12), pair.second),
            "status colors must resolve through component tokens");
  }
  AdProgress::SemanticStyles styles;
  styles.track.backgroundColor = Qt::green;
  styles.rail.backgroundColor = Qt::red;
  styles.root.backgroundColor = Qt::black;
  progress.setStatus(AdProgress::Status::Normal);
  progress.setSemanticStyles(styles);
  require(dominant(at(render(progress), 20, 12), 1) && dominant(at(render(progress), 180, 12), 0),
          "semantic track and rail styles must override their corresponding paint slots");
  progress.resetSemanticStyles();
  progress.resetComponentTokens();
  manager.setPreset(ThemeScheme::Light);
  const QImage light = render(progress);
  manager.setPreset(ThemeScheme::Dark);
  const QImage dark = render(progress);
  require(light != dark, "live light and dark theme changes must invalidate progress appearance");
  adqt::theme::ThemeOverride overrideValue;
  overrideValue.info = QColor(230, 20, 20);
  overrideValue.motion = false;
  manager.setScopeOverride(&scope, overrideValue);
  require(dominant(at(render(progress), 20, 12), 0),
          "progress must resolve the info color from its ancestor theme scope");
  manager.clearScopeOverride(&scope);
  require(!dominant(at(render(progress), 20, 12), 0),
          "clearing a scope must restore the global progress palette");
  overrideValue.motion = false;
  manager.setScopeOverride(&scope, overrideValue);
  progress.setAnimationEnabled(true);
  scope.resize(220, 40);
  scope.show();
  progress.show();
  progress.setPercent(85);
  require(near(progress.displayedPercent(), 85),
          "a scoped motion override must disable value animation while the widget is visible");
  scope.hide();
  manager.clearScopeOverride(&scope);
  manager.setConfig(original);
}

class PaintCounter final : public QObject {
 public:
  int count = 0;

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::Paint) ++count;
    return false;
  }
};

void waitForTimingFrames(int count) {
  QObject probe;
  QEventLoop loop;
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
  int observed = 0;
  adqt::widgets::detail::setFrameSubscription(&probe, QStringLiteral("ProgressTestFrameProbe"),
                                              true, [&](qint64, qint64) {
                                                if (++observed >= count) loop.quit();
                                              });
  guard.start(5000);
  loop.exec();
  adqt::widgets::detail::clearFrameSubscription(&probe, QStringLiteral("ProgressTestFrameProbe"));
  QCoreApplication::processEvents();
  require(observed == count, "the timing hub must dispatch the requested frames");
}

void activeFramesStopWhenSettled() {
  AdProgress progress;
  progress.resize(200, 24);
  AdProgress::ComponentTokens tokens;
  tokens.metrics.animationDurationMs = 0;
  progress.setComponentTokens(tokens);
  progress.setStatus(AdProgress::Status::Active);
  progress.setPercent(50);
  PaintCounter counter;
  progress.installEventFilter(&counter);
  progress.show();
  QCoreApplication::processEvents();
  counter.count = 0;
  waitForTimingFrames(3);
  require(counter.count > 0, "visible active lines must repaint from timing hub frames");
  progress.setPercent(100);
  QCoreApplication::processEvents();
  counter.count = 0;
  waitForTimingFrames(3);
  require(counter.count == 0, "a completed active line must release its frame subscription");
  progress.setPercent(50);
  progress.setAnimationEnabled(false);
  QCoreApplication::processEvents();
  counter.count = 0;
  waitForTimingFrames(3);
  require(counter.count == 0, "disabled motion must stop active line repaint subscriptions");
  progress.hide();
}

void animationVisibilityAndLifetime() {
  AdProgress progress;
  progress.resize(200, 24);
  progress.setPercent(10);
  progress.setStatus(AdProgress::Status::Active);
  AdProgress::ComponentTokens tokens;
  tokens.metrics.animationDurationMs = 5000;
  progress.setComponentTokens(tokens);
  progress.show();
  QCoreApplication::processEvents();
  progress.setPercent(90);
  require(progress.displayedPercent() >= 10 && progress.displayedPercent() <= 90,
          "animated display values must remain between their start and target");
  progress.hide();
  require(near(progress.displayedPercent(), 90),
          "hiding progress must stop motion and snap the displayed value to its target");
  progress.show();
  QCoreApplication::processEvents();
  require(near(progress.displayedPercent(), 90),
          "showing a settled widget must not replay a hidden value transition");
  progress.setPercent(30);
  progress.setAnimationEnabled(false);
  require(near(progress.displayedPercent(), 30),
          "disabling animations must immediately settle the display value");
  progress.hide();
  for (int iteration = 0; iteration < 8; ++iteration) {
    auto* temporary = new AdProgress;
    temporary->setStatus(AdProgress::Status::Active);
    temporary->show();
    temporary->setPercent(50);
    delete temporary;
    QCoreApplication::processEvents();
  }
}

void renderContactSheet(const QString& directory, ThemeScheme scheme, qreal dpr) {
  ThemeManager& manager = ThemeManager::instance();
  manager.setPreset(scheme);
  const QColor background = manager.resolveTheme().colorBgContainer;
  const QColor text = manager.resolveTheme().colorText;
  constexpr int cellWidth = 280;
  constexpr int cellHeight = 180;
  QImage sheet(QSize(qRound(cellWidth * 4 * dpr), qRound(cellHeight * 3 * dpr)),
               QImage::Format_ARGB32_Premultiplied);
  sheet.setDevicePixelRatio(dpr);
  sheet.fill(background);
  QPainter painter(&sheet);
  const QStringList labels = {QStringLiteral("Line · 30%"),
                              QStringLiteral("Active · 50%"),
                              QStringLiteral("Exception · 70%"),
                              QStringLiteral("Success · 100%"),
                              QStringLiteral("Success segment · 30 / 80"),
                              QStringLiteral("Inner percentage · thick stroke"),
                              QStringLiteral("Steps · custom colors"),
                              QStringLiteral("Gradient"),
                              QStringLiteral("Circle"),
                              QStringLiteral("Circle · steps"),
                              QStringLiteral("Dashboard"),
                              QStringLiteral("Small & micro circles")};
  for (int index = 0; index < labels.size(); ++index) {
    const QPoint origin((index % 4) * cellWidth, (index / 4) * cellHeight);
    painter.setPen(text);
    painter.drawText(QRect(origin + QPoint(16, 10), QSize(cellWidth - 32, 24)),
                     Qt::AlignLeft | Qt::AlignVCenter, labels[index]);
    AdProgress progress;
    progress.setAnimationEnabled(false);
    progress.setPercent(30 + index % 4 * 20);
    progress.resize(240, 32);
    QPoint position = origin + QPoint(20, 76);
    if (index == 1) progress.setStatus(AdProgress::Status::Active);
    if (index == 2) progress.setStatus(AdProgress::Status::Exception);
    if (index == 3) progress.setPercent(100);
    if (index == 4) {
      progress.setPercent(80);
      progress.setSuccessPercent(30);
    }
    if (index == 5) {
      progress.setStrokeWidth(22);
      progress.setPercentPlacement(AdProgress::PercentPlacement::Inner);
      progress.setPercentAlignment(AdProgress::PercentAlignment::Center);
    }
    if (index == 6) {
      progress.setSteps(8);
      progress.setPercent(75);
      progress.setStepColors({QColor("#1677ff"), QColor("#36cfc9"), QColor("#52c41a"),
                              QColor("#faad14"), QColor("#fa8c16"), QColor("#ff4d4f")});
    }
    if (index == 7) {
      progress.setPercent(85);
      progress.setStrokeGradient({{0, QColor("#108ee9")}, {1, QColor("#87d068")}});
    }
    if (index >= 8) {
      progress.setType(index == 10 ? AdProgress::Type::Dashboard : AdProgress::Type::Circle);
      progress.resize(120, 120);
      position = origin + QPoint(80, 44);
      progress.setPercent(index == 10 ? 75 : 65);
    }
    if (index == 9) {
      progress.setSteps(10);
      progress.setStepGap(3);
      progress.setSuccessPercent(20);
    }
    if (index == 11) {
      progress.setSizeClass(AdProgress::Size::Small);
      progress.resize(60, 60);
      position = origin + QPoint(60, 74);
    }
    progress.ensurePolished();
    progress.render(&painter, position, QRegion(), QWidget::DrawChildren);
    if (index == 11) {
      AdProgress micro;
      micro.setAnimationEnabled(false);
      micro.setType(AdProgress::Type::Circle);
      micro.setProgressSize(QSize(16, 16));
      micro.setPercent(75);
      micro.resize(16, 16);
      micro.render(&painter, origin + QPoint(160, 96), QRegion(), QWidget::DrawChildren);
      micro.setPercent(100);
      micro.render(&painter, origin + QPoint(200, 96), QRegion(), QWidget::DrawChildren);
    }
  }
  painter.end();
  const QString fileName =
      QStringLiteral("progress-%1-%2x.png")
          .arg(scheme == ThemeScheme::Light ? QStringLiteral("light") : QStringLiteral("dark"))
          .arg(dpr);
  require(sheet.save(QDir(directory).filePath(fileName)), "progress contact sheet must save");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
  if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
    const QString fonts = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts"));
    if (QDir(fonts).exists()) qputenv("QT_QPA_FONTDIR", fonts.toUtf8());
  }
#endif
  QApplication app(argc, argv);
  ThemeManager::instance().applyTo(app);
  valueAndSignalInvariants();
  accessibilityAndFormatting();
  languageChangeRefreshesCachedText(app);
  sizingAndSmallGeometry();
  lineRenderingAndRtl();
  stepsAndGradients();
  circleAndDashboardRendering();
  repeatedCircularPaintsRetainGeometry();
  denseStepsKeepQuantizedCompletion();
  indicatorRegressions();
  statusesThemesAndOverrides();
  animationVisibilityAndLifetime();
  activeFramesStopWhenSettled();
  const QStringList arguments = app.arguments();
  const qsizetype renderIndex = arguments.indexOf(QStringLiteral("--render-dir"));
  if (renderIndex >= 0) {
    require(renderIndex + 1 < arguments.size(), "--render-dir requires a directory path");
    const QString directory = arguments[renderIndex + 1];
    require(QDir().mkpath(directory), "the contact sheet directory must be created");
    for (ThemeScheme scheme : {ThemeScheme::Light, ThemeScheme::Dark}) {
      for (qreal dpr : {1.0, 2.0}) renderContactSheet(directory, scheme, dpr);
    }
  }
  std::cout << "Progress tests passed\n";
  return 0;
}
