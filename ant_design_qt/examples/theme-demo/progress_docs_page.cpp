#include "progress_docs_page.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

#include <array>
#include <cmath>

#include "antd_icons.h"
#include "demo_theme_utils.h"
#include "widgets/button.h"
#include "widgets/slider.h"

using adqt::widgets::AdButton;
using adqt::widgets::AdProgress;
using adqt::widgets::AdSlider;
namespace outlined_icons = adqt::icons::antd::outlined;

namespace {

struct SectionText {
  const char* title;
  const char* description;
};

const std::array<SectionText, 16> kSections = {{
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Progress bar"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Show a task's completion, active work, an exception, or success. The "
                       "percentage can also be hidden.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Circular progress"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "A circular indicator keeps the percentage or status icon in its center.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Mini progress bar"),
     QT_TRANSLATE_NOOP("ProgressDocsPage", "Small bars fit comfortably in compact content.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Mini circular progress"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Small circles preserve the same status treatment. Micro indicators expose "
                       "their percentage in a tooltip.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Dynamic progress"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Use the slider or buttons to update both indicators. Completed progress "
                       "automatically changes to success.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Custom text"),
     QT_TRANSLATE_NOOP(
         "ProgressDocsPage",
         "A format callback can display task-specific units or a completion message.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Dashboard"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Place the opening at the top, bottom, start, or end. Adjust its angle with "
                       "the slider.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Success segment"),
     QT_TRANSLATE_NOOP(
         "ProgressDocsPage",
         "Distinguish completed work from work in progress within the same indicator.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Stroke caps"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Round, butt, and square caps apply to both bars and circular strokes.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Gradients"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Use multiple color stops for a linear bar or a gradient around a circle. "
                       "The active bar retains its animated highlight.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Steps"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Discrete steps support custom spacing, individual colors, and a rounding "
                       "callback for partially completed steps.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Circular steps"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Segment circles and dashboards into steps. Change the count and gap "
                       "independently.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Size"),
     QT_TRANSLATE_NOOP(
         "ProgressDocsPage",
         "Choose small, middle, or large sizes, or set an explicit logical pixel size.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Percentage position"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Align the information at the start, center, or end, inside or outside the "
                       "progress bar.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Component tokens"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Override the progress colors, rail color, corner radius, and circular "
                       "typography for an individual widget.")},
    {QT_TRANSLATE_NOOP("ProgressDocsPage", "Semantic styling"),
     QT_TRANSLATE_NOOP("ProgressDocsPage",
                       "Style the root, body, rail, track, and indicator independently. Custom "
                       "styles work with every progress type.")},
}};

QString translated(const char* source) {
  return QCoreApplication::translate("ProgressDocsPage", source);
}

QWidget* column() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(14);
  return box;
}

QHBoxLayout* row() {
  auto* layout = new QHBoxLayout();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(24);
  return layout;
}

}  // namespace

ProgressDocsPage::ProgressDocsPage(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("progressDocsPage"));
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 24);
  root->setSpacing(16);

  auto* title = label(QT_TRANSLATE_NOOP("ProgressDocsPage", "Progress"));
  QFont titleFont = title->font();
  titleFont.setPointSize(titleFont.pointSize() + 8);
  titleFont.setBold(true);
  title->setFont(titleFont);
  root->addWidget(title);
  root->addWidget(label(QT_TRANSLATE_NOOP(
      "ProgressDocsPage", "Display the current progress and status of an operation.")));

  addSection(root, 0, buildLineDemo(false));
  addSection(root, 1, buildCircleDemo(false));
  addSection(root, 2, buildLineDemo(true));
  addSection(root, 3, buildCircleDemo(true));
  addSection(root, 4, buildDynamicDemo());
  addSection(root, 5, buildFormatDemo());
  addSection(root, 6, buildDashboardDemo());
  addSection(root, 7, buildSuccessDemo());
  addSection(root, 8, buildLinecapDemo());
  addSection(root, 9, buildGradientDemo());
  addSection(root, 10, buildStepsDemo());
  addSection(root, 11, buildCircleStepsDemo());
  addSection(root, 12, buildSizeDemo());
  addSection(root, 13, buildInfoPositionDemo());
  addSection(root, 14, buildTokenDemo());
  addSection(root, 15, buildSemanticDemo());
  root->addStretch();
}

const QStringList& ProgressDocsPage::defaultSectionTitles() {
  static QStringList titles;
  titles.clear();
  titles.reserve(static_cast<qsizetype>(kSections.size()));
  for (const auto& section : kSections) {
    titles.append(translated(section.title));
  }
  return titles;
}

const QVector<QWidget*>& ProgressDocsPage::sectionAnchors() const { return anchors_; }

const QStringList& ProgressDocsPage::sectionTitles() const { return titles_; }

void ProgressDocsPage::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    for (const auto& refresh : translations_) {
      refresh();
    }
    titles_ = defaultSectionTitles();
  }
}

void ProgressDocsPage::addSection(QVBoxLayout* root, int index, QWidget* content) {
  const SectionText& section = kSections.at(static_cast<std::size_t>(index));
  auto* panel = new QFrame();
  panel->setObjectName(QStringLiteral("progressSection%1").arg(index));
  panel->setFrameShape(QFrame::StyledPanel);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(10);

  auto* title = label(section.title);
  QFont titleFont = title->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 1);
  title->setFont(titleFont);
  layout->addWidget(title);
  layout->addWidget(label(section.description, true));
  layout->addWidget(content);
  root->addWidget(panel);
  anchors_.append(panel);
  titles_.append(translated(section.title));
}

QLabel* ProgressDocsPage::label(const char* source, bool hint) {
  auto* result = hint ? demo::makeHintLabel(translated(source)) : new QLabel(translated(source));
  result->setWordWrap(true);
  translations_.append([result, source]() { result->setText(translated(source)); });
  return result;
}

void ProgressDocsPage::bindAccessibleText(QWidget* widget, const char* source) {
  const auto refresh = [widget, source]() {
    widget->setAccessibleName(translated(source));
    widget->setToolTip(translated(source));
  };
  refresh();
  translations_.append(refresh);
}

AdProgress* ProgressDocsPage::progress(qreal percent, AdProgress::Type type) {
  auto* result = new AdProgress();
  result->setType(type);
  result->setPercent(percent);
  const char* source = QT_TRANSLATE_NOOP("ProgressDocsPage", "Line progress");
  if (type == AdProgress::Type::Circle) {
    source = QT_TRANSLATE_NOOP("ProgressDocsPage", "Circular progress");
  } else if (type == AdProgress::Type::Dashboard) {
    source = QT_TRANSLATE_NOOP("ProgressDocsPage", "Dashboard progress");
  }
  const auto refresh = [result, source]() { result->setAccessibleName(translated(source)); };
  refresh();
  translations_.append(refresh);
  if (type == AdProgress::Type::Line) {
    result->setMaximumWidth(560);
  }
  return result;
}

QWidget* ProgressDocsPage::buildLineDemo(bool small) {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  const QList<QPair<qreal, AdProgress::Status>> examples = {{30, AdProgress::Status::Normal},
                                                            {50, AdProgress::Status::Active},
                                                            {70, AdProgress::Status::Exception},
                                                            {100, AdProgress::Status::Success}};
  for (const auto& example : examples) {
    auto* indicator = progress(example.first);
    indicator->setStatus(example.second);
    if (small) {
      indicator->setSizeClass(AdProgress::Size::Small);
    }
    layout->addWidget(indicator);
  }
  auto* withoutInfo = progress(50);
  withoutInfo->setShowInfo(false);
  if (small) {
    withoutInfo->setSizeClass(AdProgress::Size::Small);
  }
  layout->addWidget(withoutInfo);
  return box;
}

QWidget* ProgressDocsPage::buildCircleDemo(bool small) {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  QHBoxLayout* circles = row();
  const QList<QPair<qreal, AdProgress::Status>> examples = {{75, AdProgress::Status::Normal},
                                                            {70, AdProgress::Status::Exception},
                                                            {100, AdProgress::Status::Success}};
  for (const auto& example : examples) {
    auto* indicator = progress(example.first, AdProgress::Type::Circle);
    indicator->setStatus(example.second);
    if (small) {
      indicator->setSizeClass(AdProgress::Size::Small);
    }
    circles->addWidget(indicator, 0, Qt::AlignVCenter);
  }
  circles->addStretch();
  layout->addLayout(circles);
  if (small) {
    auto* microRow = row();
    microRow->setSpacing(16);
    microRow->addWidget(label(QT_TRANSLATE_NOOP("ProgressDocsPage", "Micro indicators"), true));
    for (const auto& example : examples) {
      auto* indicator = progress(example.first, AdProgress::Type::Circle);
      indicator->setStatus(example.second);
      indicator->setProgressSize(QSize(20, 20));
      microRow->addWidget(indicator);
    }
    microRow->addStretch();
    layout->addLayout(microRow);
  }
  return box;
}

QWidget* ProgressDocsPage::buildDynamicDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  auto* line = progress(40);
  line->setObjectName(QStringLiteral("progressDynamicLine"));
  auto* circle = progress(40, AdProgress::Type::Circle);
  circle->setObjectName(QStringLiteral("progressDynamicCircle"));
  auto* slider = new AdSlider();
  slider->setObjectName(QStringLiteral("progressPercentSlider"));
  slider->setRange(0, 100);
  slider->setSingleStep(5);
  slider->setValue(40);
  slider->setMaximumWidth(560);
  bindAccessibleText(slider, QT_TRANSLATE_NOOP("ProgressDocsPage", "Progress percentage"));
  auto* decrease = new AdButton();
  decrease->setObjectName(QStringLiteral("progressDecreaseButton"));
  decrease->setIconRef(outlined_icons::Minus());
  bindAccessibleText(decrease, QT_TRANSLATE_NOOP("ProgressDocsPage", "Decrease progress"));
  auto* increase = new AdButton();
  increase->setObjectName(QStringLiteral("progressIncreaseButton"));
  increase->setIconRef(outlined_icons::Plus());
  bindAccessibleText(increase, QT_TRANSLATE_NOOP("ProgressDocsPage", "Increase progress"));
  auto* controls = row();
  controls->setSpacing(8);
  controls->addWidget(decrease);
  controls->addWidget(increase);
  controls->addStretch();

  connect(slider, &AdSlider::valueChanged, box, [line, circle, decrease, increase](double value) {
    line->setPercent(value);
    circle->setPercent(value);
    decrease->setEnabled(value > 0);
    increase->setEnabled(value < 100);
  });
  connect(decrease, &QAbstractButton::clicked, slider,
          [slider]() { slider->setValue(slider->value() - 10); });
  connect(increase, &QAbstractButton::clicked, slider,
          [slider]() { slider->setValue(slider->value() + 10); });
  layout->addWidget(line);
  // A row constrains the circle's allocated width before Qt evaluates its
  // height-for-width hint, keeping the vertical demonstration compact.
  auto* circleRow = row();
  circleRow->addWidget(circle);
  circleRow->addStretch();
  layout->addLayout(circleRow);
  layout->addWidget(slider);
  layout->addLayout(controls);
  return box;
}

QWidget* ProgressDocsPage::buildFormatDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  auto* line = progress(75);
  line->setFormat([](qreal percent, qreal) {
    return QCoreApplication::translate("ProgressDocsPage", "%1 of 80 files")
        .arg(QLocale().toString(qRound(percent * 0.8)));
  });
  layout->addWidget(line);
  auto* circles = row();
  auto* custom = progress(75, AdProgress::Type::Circle);
  custom->setFormat([](qreal percent, qreal) {
    return QCoreApplication::translate("ProgressDocsPage", "%1 days")
        .arg(QLocale().toString(qRound(percent)));
  });
  auto* complete = progress(100, AdProgress::Type::Circle);
  complete->setFormat(
      [](qreal, qreal) { return QCoreApplication::translate("ProgressDocsPage", "Done"); });
  circles->addWidget(custom);
  circles->addWidget(complete);
  circles->addStretch();
  layout->addLayout(circles);
  return box;
}

QWidget* ProgressDocsPage::buildDashboardDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  auto* indicators = row();
  QVector<AdProgress*> dashboards;
  const std::array<QPair<const char*, AdProgress::GapPlacement>, 4> placements = {{
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Bottom"), AdProgress::GapPlacement::Bottom},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Top"), AdProgress::GapPlacement::Top},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Start"), AdProgress::GapPlacement::Start},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "End"), AdProgress::GapPlacement::End},
  }};
  for (const auto& placement : placements) {
    auto* group = column();
    auto* groupLayout = static_cast<QVBoxLayout*>(group->layout());
    auto* dashboard = progress(65, AdProgress::Type::Dashboard);
    dashboard->setGapPlacement(placement.second);
    dashboard->setGapDegree(75);
    auto* caption = label(placement.first, true);
    caption->setAlignment(Qt::AlignCenter);
    groupLayout->addWidget(dashboard, 0, Qt::AlignHCenter);
    groupLayout->addWidget(caption);
    indicators->addWidget(group, 0, Qt::AlignTop);
    dashboards.append(dashboard);
  }
  indicators->addStretch();
  layout->addLayout(indicators);
  auto* angleLabel = label(QT_TRANSLATE_NOOP("ProgressDocsPage", "Gap angle"), true);
  auto* angle = new AdSlider();
  angle->setRange(0, 180);
  angle->setSingleStep(5);
  angle->setValue(75);
  angle->setMaximumWidth(560);
  angleLabel->setBuddy(angle);
  bindAccessibleText(angle, QT_TRANSLATE_NOOP("ProgressDocsPage", "Gap angle"));
  connect(angle, &AdSlider::valueChanged, box, [dashboards](double value) {
    for (AdProgress* dashboard : dashboards) {
      dashboard->setGapDegree(value);
    }
  });
  layout->addWidget(angleLabel);
  layout->addWidget(angle);
  return box;
}

QWidget* ProgressDocsPage::buildSuccessDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  auto* line = progress(60);
  line->setSuccessPercent(30);
  layout->addWidget(line);
  auto* indicators = row();
  for (const auto type : {AdProgress::Type::Circle, AdProgress::Type::Dashboard}) {
    auto* indicator = progress(60, type);
    indicator->setSuccessPercent(30);
    indicators->addWidget(indicator);
  }
  indicators->addStretch();
  layout->addLayout(indicators);
  layout->addWidget(label(
      QT_TRANSLATE_NOOP("ProgressDocsPage", "3 completed / 3 in progress / 4 remaining"), true));
  return box;
}

QWidget* ProgressDocsPage::buildLinecapDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  const std::array<QPair<const char*, AdProgress::StrokeLinecap>, 3> caps = {{
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Round"), AdProgress::StrokeLinecap::Round},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Butt"), AdProgress::StrokeLinecap::Butt},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Square"), AdProgress::StrokeLinecap::Square},
  }};
  auto* circles = row();
  for (const auto& cap : caps) {
    auto* line = progress(75);
    line->setStrokeLinecap(cap.second);
    layout->addWidget(label(cap.first, true));
    layout->addWidget(line);
    auto* circle = progress(75, AdProgress::Type::Circle);
    circle->setStrokeLinecap(cap.second);
    circles->addWidget(circle);
  }
  circles->addStretch();
  layout->addLayout(circles);
  return box;
}

QWidget* ProgressDocsPage::buildGradientDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  const QGradientStops blueGreen = {{0, QColor(QStringLiteral("#108ee9"))},
                                    {1, QColor(QStringLiteral("#87d068"))}};
  const QGradientStops multicolor = {{0, QColor(QStringLiteral("#87d068"))},
                                     {0.5, QColor(QStringLiteral("#ffe58f"))},
                                     {1, QColor(QStringLiteral("#ffccc7"))}};
  auto* line = progress(99.9);
  line->setStrokeGradient(blueGreen);
  layout->addWidget(line);
  auto* active = progress(50);
  active->setStatus(AdProgress::Status::Active);
  active->setStrokeGradient(blueGreen);
  layout->addWidget(active);
  auto* reversed = progress(75);
  reversed->setStrokeGradient(blueGreen);
  reversed->setGradientReversed(true);
  layout->addWidget(reversed);
  for (const auto type : {AdProgress::Type::Circle, AdProgress::Type::Dashboard}) {
    auto* indicators = row();
    auto* partial = progress(90, type);
    partial->setStrokeGradient(blueGreen);
    auto* complete = progress(100, type);
    complete->setStrokeGradient(blueGreen);
    auto* custom = progress(93, type);
    custom->setStrokeGradient(multicolor);
    indicators->addWidget(partial);
    indicators->addWidget(complete);
    indicators->addWidget(custom);
    indicators->addStretch();
    layout->addLayout(indicators);
  }
  return box;
}

QWidget* ProgressDocsPage::buildStepsDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  for (const auto& example : {QPair<int, qreal>{3, 50}, QPair<int, qreal>{5, 30}}) {
    auto* line = progress(example.second);
    line->setSteps(example.first);
    layout->addWidget(line, 0, Qt::AlignLeft);
  }
  auto* complete = progress(100);
  complete->setSteps(5);
  complete->setSizeClass(AdProgress::Size::Small);
  complete->setStrokeColor(QColor(QStringLiteral("#52c41a")));
  layout->addWidget(complete, 0, Qt::AlignLeft);
  auto* colored = progress(60);
  colored->setSteps(5);
  colored->setStepColors({QColor(QStringLiteral("#52c41a")), QColor(QStringLiteral("#52c41a")),
                          QColor(QStringLiteral("#ff4d4f"))});
  layout->addWidget(colored, 0, Qt::AlignLeft);
  layout->addWidget(
      label(QT_TRANSLATE_NOOP("ProgressDocsPage", "Round down to completed steps"), true));
  auto* rounded = progress(55);
  rounded->setSteps(5);
  rounded->setStepGap(6);
  rounded->setStepRounding([](qreal filled) { return static_cast<int>(std::floor(filled)); });
  layout->addWidget(rounded, 0, Qt::AlignLeft);
  return box;
}

QWidget* ProgressDocsPage::buildCircleStepsDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  auto* countLabel = label(QT_TRANSLATE_NOOP("ProgressDocsPage", "Step count"), true);
  auto* count = new AdSlider();
  count->setObjectName(QStringLiteral("progressStepsCountSlider"));
  count->setRange(2, 12);
  count->setSingleStep(1);
  count->setValue(8);
  count->setMaximumWidth(560);
  countLabel->setBuddy(count);
  bindAccessibleText(count, QT_TRANSLATE_NOOP("ProgressDocsPage", "Step count"));
  auto* gapLabel = label(QT_TRANSLATE_NOOP("ProgressDocsPage", "Step gap"), true);
  auto* gap = new AdSlider();
  gap->setObjectName(QStringLiteral("progressStepsGapSlider"));
  gap->setRange(0, 24);
  gap->setSingleStep(1);
  gap->setValue(7);
  gap->setMaximumWidth(560);
  gapLabel->setBuddy(gap);
  bindAccessibleText(gap, QT_TRANSLATE_NOOP("ProgressDocsPage", "Step gap"));
  layout->addWidget(countLabel);
  layout->addWidget(count);
  layout->addWidget(gapLabel);
  layout->addWidget(gap);
  auto* indicators = row();
  auto* dashboard = progress(50, AdProgress::Type::Dashboard);
  auto* circle = progress(100, AdProgress::Type::Circle);
  for (AdProgress* indicator : {dashboard, circle}) {
    indicator->setSteps(8);
    indicator->setStepGap(7);
    indicator->setStrokeWidth(20);
    indicators->addWidget(indicator);
  }
  indicators->addStretch();
  layout->addLayout(indicators);
  connect(count, &AdSlider::valueChanged, box, [dashboard, circle](double value) {
    dashboard->setSteps(qRound(value));
    circle->setSteps(qRound(value));
  });
  connect(gap, &AdSlider::valueChanged, box, [dashboard, circle](double value) {
    dashboard->setStepGap(value);
    circle->setStepGap(value);
  });
  return box;
}

QWidget* ProgressDocsPage::buildSizeDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  for (const auto size :
       {AdProgress::Size::Small, AdProgress::Size::Middle, AdProgress::Size::Large}) {
    auto* line = progress(50);
    line->setSizeClass(size);
    layout->addWidget(line);
  }
  auto* thick = progress(50);
  thick->setProgressSize(QSize(300, 20));
  layout->addWidget(thick, 0, Qt::AlignLeft);
  for (const auto type : {AdProgress::Type::Circle, AdProgress::Type::Dashboard}) {
    auto* indicators = row();
    for (const auto size :
         {AdProgress::Size::Large, AdProgress::Size::Middle, AdProgress::Size::Small}) {
      auto* indicator = progress(50, type);
      indicator->setSizeClass(size);
      indicators->addWidget(indicator, 0, Qt::AlignVCenter);
    }
    auto* micro = progress(50, type);
    micro->setProgressSize(QSize(20, 20));
    indicators->addWidget(micro, 0, Qt::AlignVCenter);
    indicators->addStretch();
    layout->addLayout(indicators);
  }
  auto* customSteps = progress(50);
  customSteps->setSteps(5);
  customSteps->setProgressSize(QSize(20, 24));
  customSteps->setStepGap(6);
  layout->addWidget(customSteps, 0, Qt::AlignLeft);
  return box;
}

QWidget* ProgressDocsPage::buildInfoPositionDemo() {
  auto* box = new QWidget();
  auto* layout = new QGridLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setHorizontalSpacing(18);
  layout->setVerticalSpacing(14);
  const std::array<QPair<const char*, AdProgress::PercentAlignment>, 3> alignments = {{
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Start"), AdProgress::PercentAlignment::Start},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "Center"), AdProgress::PercentAlignment::Center},
      {QT_TRANSLATE_NOOP("ProgressDocsPage", "End"), AdProgress::PercentAlignment::End},
  }};
  int rowIndex = 0;
  for (const auto placement :
       {AdProgress::PercentPlacement::Inner, AdProgress::PercentPlacement::Outer}) {
    for (const auto& alignment : alignments) {
      const char* source = placement == AdProgress::PercentPlacement::Inner
                               ? QT_TRANSLATE_NOOP("ProgressDocsPage", "Inside / %1")
                               : QT_TRANSLATE_NOOP("ProgressDocsPage", "Outside / %1");
      auto* caption = new QLabel();
      const auto refresh = [caption, source, alignment]() {
        caption->setText(translated(source).arg(translated(alignment.first)));
      };
      refresh();
      translations_.append(refresh);
      auto* line = progress(60);
      line->setPercentAlignment(alignment.second);
      line->setPercentPlacement(placement);
      if (placement == AdProgress::PercentPlacement::Inner) {
        line->setProgressSize(QSize(360, 20));
      }
      layout->addWidget(caption, rowIndex, 0);
      layout->addWidget(
          line, rowIndex, 1,
          placement == AdProgress::PercentPlacement::Inner ? Qt::AlignLeft : Qt::Alignment());
      ++rowIndex;
    }
  }
  layout->setColumnStretch(1, 1);
  return box;
}

QWidget* ProgressDocsPage::buildTokenDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  AdProgress::ComponentTokens tokens;
  tokens.colors.defaultColor = QColor(QStringLiteral("#bae0ff"));
  tokens.colors.remainingColor = QColor(QStringLiteral("#fff1f0"));
  tokens.colors.circleTextColor = QColor(QStringLiteral("#389e0d"));
  tokens.metrics.lineBorderRadius = 50;
  tokens.metrics.circleTextFontSize = 24;
  for (const auto size : {AdProgress::Size::Middle, AdProgress::Size::Small}) {
    auto* line = progress(50);
    line->setSizeClass(size);
    line->setComponentTokens(tokens);
    layout->addWidget(line);
  }
  auto* thick = progress(50);
  thick->setProgressSize(QSize(300, 20));
  thick->setComponentTokens(tokens);
  layout->addWidget(thick, 0, Qt::AlignLeft);
  auto* indicators = row();
  for (const auto type : {AdProgress::Type::Circle, AdProgress::Type::Dashboard}) {
    auto* indicator = progress(50, type);
    indicator->setComponentTokens(tokens);
    indicators->addWidget(indicator);
  }
  indicators->addStretch();
  layout->addLayout(indicators);
  return box;
}

QWidget* ProgressDocsPage::buildSemanticDemo() {
  QWidget* box = column();
  auto* layout = static_cast<QVBoxLayout*>(box->layout());
  AdProgress::SemanticStyles styles;
  styles.track.backgroundColor = QColor(QStringLiteral("#722ed1"));
  styles.rail.backgroundColor = QColor(QStringLiteral("#efdbff"));
  styles.indicator.textColor = QColor(QStringLiteral("#722ed1"));
  QFont indicatorFont = font();
  indicatorFont.setBold(true);
  styles.indicator.font = indicatorFont;
  auto* line = progress(65);
  line->setSemanticStyles(styles);
  line->setProgressSize(QSize(360, 16));
  layout->addWidget(line, 0, Qt::AlignLeft);
  auto* indicators = row();
  for (const auto type : {AdProgress::Type::Circle, AdProgress::Type::Dashboard}) {
    auto* indicator = progress(65, type);
    indicator->setSemanticStyles(styles);
    indicators->addWidget(indicator);
  }
  indicators->addStretch();
  layout->addLayout(indicators);
  return box;
}
