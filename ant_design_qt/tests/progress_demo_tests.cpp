#include "progress_docs_page.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QSet>
#include <QStyleFactory>
#include <QTranslator>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/slider.h"

namespace {

using adqt::theme::ThemeManager;
using adqt::theme::ThemeScheme;
using adqt::widgets::AdButton;
using adqt::widgets::AdProgress;
using adqt::widgets::AdSlider;

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool near(qreal actual, qreal expected) { return std::abs(actual - expected) < 0.001; }

template <typename Widget>
Widget* namedChild(QWidget& parent, const char* name) {
  Widget* result = parent.findChild<Widget*>(QString::fromLatin1(name));
  require(result != nullptr, "the Progress demo must expose its named interactive controls");
  return result;
}

void sectionNavigation(ProgressDocsPage& page) {
  require(page.sectionAnchors().size() == 16 && page.sectionTitles().size() == 16,
          "Progress documentation must expose all sixteen sections");
  require(page.sectionTitles() == ProgressDocsPage::defaultSectionTitles(),
          "lazy navigation titles must match the instantiated Progress page");
  QSet<QWidget*> uniqueAnchors;
  for (int index = 0; index < page.sectionAnchors().size(); ++index) {
    QWidget* anchor = page.sectionAnchors().at(index);
    require(anchor != nullptr && page.isAncestorOf(anchor),
            "every Progress section must have an anchor belonging to the page");
    require(anchor->objectName() == QStringLiteral("progressSection%1").arg(index),
            "Progress anchors must retain stable section identities");
    require(!page.sectionTitles().at(index).isEmpty(), "every Progress section must have a title");
    uniqueAnchors.insert(anchor);
  }
  require(uniqueAnchors.size() == 16, "Progress sections must expose unique navigation anchors");
}

void dynamicControls(ProgressDocsPage& page) {
  auto* slider = namedChild<AdSlider>(page, "progressPercentSlider");
  auto* line = namedChild<AdProgress>(page, "progressDynamicLine");
  auto* circle = namedChild<AdProgress>(page, "progressDynamicCircle");
  auto* increase = namedChild<AdButton>(page, "progressIncreaseButton");
  auto* decrease = namedChild<AdButton>(page, "progressDecreaseButton");
  require(line->type() == AdProgress::Type::Line && circle->type() == AdProgress::Type::Circle,
          "dynamic progress must demonstrate both line and circular indicators");

  slider->setValue(75);
  require(near(line->percent(), 75) && near(circle->percent(), 75),
          "changing the percentage slider must update both progress indicators");
  increase->click();
  require(near(slider->value(), 85) && near(line->percent(), 85) && near(circle->percent(), 85),
          "the increase button must advance both indicators by ten percentage points");
  decrease->click();
  require(near(slider->value(), 75) && near(line->percent(), 75) && near(circle->percent(), 75),
          "the decrease button must reduce both indicators by ten percentage points");

  slider->setValue(95);
  increase->click();
  require(near(slider->value(), 100) && near(line->percent(), 100) && near(circle->percent(), 100),
          "the increase button must clamp progress at completion");
  require(!increase->isEnabled() && decrease->isEnabled(),
          "only the increase button must be disabled at completion");
  require(line->effectiveStatus() == AdProgress::Status::Success &&
              circle->effectiveStatus() == AdProgress::Status::Success,
          "dynamic indicators must show automatic success at completion");
  slider->setValue(5);
  decrease->click();
  require(near(slider->value(), 0) && near(line->percent(), 0) && near(circle->percent(), 0),
          "the decrease button must clamp progress at zero");
  require(!decrease->isEnabled() && increase->isEnabled(),
          "only the decrease button must be disabled at zero");
  slider->setValue(75);
}

void dashboardControls(ProgressDocsPage& page) {
  QWidget* section = page.sectionAnchors().at(6);
  const auto sliders = section->findChildren<AdSlider*>();
  const auto dashboards = section->findChildren<AdProgress*>();
  require(sliders.size() == 1 && dashboards.size() == 4,
          "the dashboard demo must expose four placements and one angle control");
  QSet<int> placements;
  for (AdProgress* dashboard : dashboards) {
    require(dashboard->type() == AdProgress::Type::Dashboard,
            "the dashboard section must contain dashboard indicators");
    placements.insert(static_cast<int>(dashboard->gapPlacement()));
  }
  require(placements.size() == 4, "the dashboard demo must cover all four gap placements");
  sliders.first()->setValue(100);
  for (AdProgress* dashboard : dashboards) {
    require(near(dashboard->gapDegree(), 100),
            "changing the gap angle must update every dashboard placement");
  }
}

void circularStepControls(ProgressDocsPage& page) {
  auto* count = namedChild<AdSlider>(page, "progressStepsCountSlider");
  auto* gap = namedChild<AdSlider>(page, "progressStepsGapSlider");
  const auto indicators = page.sectionAnchors().at(11)->findChildren<AdProgress*>();
  require(indicators.size() == 2,
          "circular steps must demonstrate both circular and dashboard indicators");
  count->setValue(5);
  gap->setValue(11);
  QSet<int> types;
  for (AdProgress* indicator : indicators) {
    types.insert(static_cast<int>(indicator->type()));
    require(indicator->steps() == 5 && near(indicator->stepGap(), 11),
            "step count and gap controls must update both segmented indicators");
  }
  require(types.contains(static_cast<int>(AdProgress::Type::Circle)) &&
              types.contains(static_cast<int>(AdProgress::Type::Dashboard)),
          "the circular steps demo must cover both supported ring types");
}

class DemoTranslator final : public QTranslator {
 public:
  bool isEmpty() const override { return false; }
  QString translate(const char* context, const char* source, const char*, int) const override {
    if (std::strcmp(context, "ProgressDocsPage") == 0) {
      return QStringLiteral("Localized: ") + QString::fromUtf8(source);
    }
    return QString();
  }
};

void languageChangePreservesState(QApplication& app, ProgressDocsPage& page) {
  const QStringList originalTitles = page.sectionTitles();
  auto* slider = namedChild<AdSlider>(page, "progressPercentSlider");
  auto* line = namedChild<AdProgress>(page, "progressDynamicLine");
  auto* circle = namedChild<AdProgress>(page, "progressDynamicCircle");
  auto* count = namedChild<AdSlider>(page, "progressStepsCountSlider");
  auto* gap = namedChild<AdSlider>(page, "progressStepsGapSlider");
  DemoTranslator translator;
  require(app.installTranslator(&translator), "the deterministic demo translator must install");
  QEvent change(QEvent::LanguageChange);
  QCoreApplication::sendEvent(&page, &change);
  QApplication::processEvents();
  require(page.sectionTitles().first() == QStringLiteral("Localized: ") + originalTitles.first(),
          "language changes must refresh cached section titles");
  require(page.sectionTitles() == ProgressDocsPage::defaultSectionTitles(),
          "translated lazy navigation titles must match the translated page");
  require(slider->accessibleName().startsWith(QStringLiteral("Localized: ")),
          "language changes must refresh interactive accessible names");
  require(slider == namedChild<AdSlider>(page, "progressPercentSlider") &&
              near(slider->value(), 75) && near(line->percent(), 75) && near(circle->percent(), 75),
          "language changes must retain the dynamic controls and their progress state");
  require(near(count->value(), 5) && near(gap->value(), 11),
          "language changes must retain the circular step controls' values");
  for (AdProgress* dashboard : page.sectionAnchors().at(6)->findChildren<AdProgress*>()) {
    require(near(dashboard->gapDegree(), 100),
            "language changes must retain customized dashboard angles");
  }
  app.removeTranslator(&translator);
  QCoreApplication::sendEvent(&page, &change);
  QApplication::processEvents();
  require(page.sectionTitles() == originalTitles,
          "removing the demo translator must restore source navigation titles");
}

void renderPage(ProgressDocsPage& page, const QString& directory, ThemeScheme scheme) {
  ThemeManager::instance().setColorScheme(scheme);
  page.resize(760, page.layout()->sizeHint().height());
  page.layout()->activate();
  QApplication::processEvents();
  QImage image(page.size(), QImage::Format_ARGB32_Premultiplied);
  image.fill(page.palette().color(QPalette::Window));
  QPainter painter(&image);
  page.render(&painter);
  painter.end();
  const QString name = scheme == ThemeScheme::Light ? QStringLiteral("progress-demo-light.png")
                                                    : QStringLiteral("progress-demo-dark.png");
  require(image.save(QDir(directory).filePath(name)), "the Progress demo screenshot must save");
}

void circularLayoutStaysCompact(ProgressDocsPage& page) {
  page.resize(760, page.layout()->sizeHint().height());
  page.show();
  page.layout()->activate();
  QApplication::processEvents();
  auto* circle = page.findChild<AdProgress*>(QStringLiteral("progressDynamicCircle"));
  require(circle && circle->size() == circle->sizeHint(),
          "the dynamic circle must retain its natural square size in a wide page layout");
  page.hide();
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
  if (QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  }
  ThemeManager::instance().applyTo(app);
  ProgressDocsPage page;
  for (AdProgress* indicator : page.findChildren<AdProgress*>()) {
    indicator->setAnimationEnabled(false);
  }
  sectionNavigation(page);
  dynamicControls(page);
  dashboardControls(page);
  circularStepControls(page);
  languageChangePreservesState(app, page);
  circularLayoutStaysCompact(page);
  const QStringList arguments = app.arguments();
  const qsizetype renderIndex = arguments.indexOf(QStringLiteral("--render-dir"));
  if (renderIndex >= 0) {
    require(renderIndex + 1 < arguments.size(), "--render-dir requires a directory path");
    const QString directory = arguments.at(renderIndex + 1);
    require(QDir().mkpath(directory), "the Progress demo screenshot directory must be created");
    for (ThemeScheme scheme : {ThemeScheme::Light, ThemeScheme::Dark}) {
      renderPage(page, directory, scheme);
    }
  }
  std::cout << "Progress demo tests passed\n";
  return 0;
}
