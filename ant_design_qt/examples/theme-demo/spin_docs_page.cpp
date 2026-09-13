#include "spin_docs_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include "antd_icons.h"
#include "icon_core.h"
#include "widgets/widgets.h"

using adqt::widgets::AdButton;
using adqt::widgets::AdSpin;
using adqt::widgets::AdSwitch;
namespace outlined_icons = adqt::icons::antd::outlined;

namespace {

QWidget* makeRow() {
  auto* row = new QWidget();
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(24);
  return row;
}

QFrame* makeContentPanel() {
  auto* panel = new QFrame();
  panel->setFrameShape(QFrame::StyledPanel);
  panel->setMinimumHeight(112);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(18, 16, 18, 16);
  layout->setSpacing(6);
  auto* title = new QLabel(QStringLiteral("Asynchronous content"));
  QFont titleFont = title->font();
  titleFont.setBold(true);
  title->setFont(titleFont);
  auto* body =
      new QLabel(QStringLiteral("The existing content remains visible while "
                                "the loading surface blocks interaction."));
  body->setWordWrap(true);
  layout->addWidget(title);
  layout->addWidget(body);
  layout->addStretch();
  return panel;
}

}  // namespace

SpinDocsPage::SpinDocsPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 24);
  root->setSpacing(16);

  auto* title = new QLabel(QStringLiteral("Spin"));
  QFont titleFont = title->font();
  titleFont.setPointSize(titleFont.pointSize() + 8);
  titleFont.setBold(true);
  title->setFont(titleFont);
  root->addWidget(title);

  auto* subtitle = new QLabel(QStringLiteral("Used for the loading status of a page or a block."));
  subtitle->setWordWrap(true);
  root->addWidget(subtitle);

  addSection(root, QStringLiteral("Basic Usage"), QStringLiteral("Demo: basic.tsx"),
             buildBasicDemo());
  addSection(root, QStringLiteral("Size"), QStringLiteral("Demo: size.tsx"), buildSizeDemo());
  addSection(root, QStringLiteral("Embedded mode"), QStringLiteral("Demo: nested.tsx"),
             buildNestedDemo());
  addSection(root, QStringLiteral("Customized description"), QStringLiteral("Demo: tip.tsx"),
             buildDescriptionDemo());
  addSection(root, QStringLiteral("Delay"), QStringLiteral("Demo: delayAndDebounce.tsx"),
             buildDelayDemo());
  addSection(root, QStringLiteral("Custom spinning indicator"),
             QStringLiteral("Demo: custom-indicator.tsx"), buildCustomIndicatorDemo());
  addSection(root, QStringLiteral("Progress"), QStringLiteral("Demo: percent.tsx"),
             buildProgressDemo());
  addSection(root, QStringLiteral("Fullscreen"), QStringLiteral("Demo: fullscreen.tsx"),
             buildFullscreenDemo());
  addSection(root, QStringLiteral("Component tokens"), QStringLiteral("Qt-native token overrides"),
             buildTokenDemo());
  root->addStretch();
}

const QVector<QWidget*>& SpinDocsPage::sectionAnchors() const { return anchors_; }

const QStringList& SpinDocsPage::sectionTitles() const { return titles_; }

void SpinDocsPage::addSection(QVBoxLayout* root, const QString& title, const QString& description,
                              QWidget* content) {
  auto* panel = new QFrame();
  panel->setFrameShape(QFrame::StyledPanel);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(10);

  auto* titleLabel = new QLabel(title);
  QFont titleFont = titleLabel->font();
  titleFont.setBold(true);
  titleFont.setPointSize(titleFont.pointSize() + 1);
  titleLabel->setFont(titleFont);
  auto* descriptionLabel = new QLabel(description);
  descriptionLabel->setWordWrap(true);

  layout->addWidget(titleLabel);
  layout->addWidget(descriptionLabel);
  layout->addWidget(content);
  root->addWidget(panel);
  anchors_.append(panel);
  titles_.append(title);
}

QWidget* SpinDocsPage::buildBasicDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  layout->addWidget(new AdSpin());
  layout->addStretch();
  return row;
}

QWidget* SpinDocsPage::buildSizeDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  auto* small = new AdSpin();
  small->setSizeClass(AdSpin::SizeClass::Small);
  auto* medium = new AdSpin();
  auto* large = new AdSpin();
  large->setSizeClass(AdSpin::SizeClass::Large);
  layout->addWidget(small);
  layout->addWidget(medium);
  layout->addWidget(large);
  layout->addStretch();
  return row;
}

QWidget* SpinDocsPage::buildNestedDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);
  auto* spin = new AdSpin(makeContentPanel(), box);
  spin->setDescription(QStringLiteral("Loading"));
  auto* toggle = new AdSwitch();
  toggle->setChecked(true);
  toggle->setText(QStringLiteral("Loading state"));
  connect(toggle, &QAbstractButton::toggled, spin, &AdSpin::setSpinning);
  layout->addWidget(spin);
  layout->addWidget(toggle, 0, Qt::AlignLeft);
  return box;
}

QWidget* SpinDocsPage::buildDescriptionDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  auto* small = new AdSpin();
  small->setSizeClass(AdSpin::SizeClass::Small);
  small->setDescription(QStringLiteral("Loading"));
  auto* large = new AdSpin();
  large->setSizeClass(AdSpin::SizeClass::Large);
  large->setDescription(QStringLiteral("Loading"));
  layout->addWidget(small);
  layout->addWidget(large);
  layout->addStretch();
  return row;
}

QWidget* SpinDocsPage::buildDelayDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  auto* spin = new AdSpin();
  spin->setSpinning(false);
  spin->setDelayMs(500);
  auto* trigger = new AdButton(QStringLiteral("Start delayed loading"));
  connect(trigger, &QAbstractButton::clicked, spin, [spin]() {
    spin->setSpinning(false);
    spin->setSpinning(true);
    QTimer::singleShot(1800, spin, [spin]() { spin->setSpinning(false); });
  });
  layout->addWidget(trigger);
  layout->addWidget(spin);
  layout->addStretch();
  return row;
}

QWidget* SpinDocsPage::buildCustomIndicatorDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  auto* spin = new AdSpin();
  auto* icon = new QLabel();
  icon->setFixedSize(36, 36);
  icon->setPixmap(adqt::icons::renderIconPixmap(outlined_icons::Sync(), {QSize(36, 36), 1.0}));
  icon->setScaledContents(true);
  icon->setAccessibleName(QStringLiteral("Custom loading indicator"));
  spin->setIndicatorWidget(icon);
  layout->addWidget(spin);
  layout->addStretch();
  return row;
}

QWidget* SpinDocsPage::buildProgressDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  auto* automatic = new AdSpin();
  automatic->setAutoProgress();
  auto* determinate = new AdSpin();
  determinate->setSizeClass(AdSpin::SizeClass::Large);
  determinate->setPercent(0.0);
  auto* timer = new QTimer(row);
  timer->setInterval(100);
  connect(timer, &QTimer::timeout, determinate, [determinate]() {
    const qreal next = determinate->percent() + 5.0;
    determinate->setPercent(next > 100.0 ? 0.0 : next);
  });
  timer->start();
  layout->addWidget(automatic);
  layout->addWidget(determinate);
  layout->addStretch();
  return row;
}

QWidget* SpinDocsPage::buildFullscreenDemo() {
  auto* box = new QWidget();
  auto* layout = new QHBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* trigger = new AdButton(QStringLiteral("Show fullscreen"));
  auto* spin = new AdSpin(box);
  spin->setGeometry(0, 0, 1, 1);
  spin->setFullscreen(true);
  spin->setDescription(QStringLiteral("Loading"));
  spin->setSpinning(false);
  connect(trigger, &QAbstractButton::clicked, spin, [spin]() {
    spin->setSpinning(true);
    QTimer::singleShot(1800, spin, [spin]() { spin->setSpinning(false); });
  });
  layout->addWidget(trigger);
  layout->addStretch();
  return box;
}

QWidget* SpinDocsPage::buildTokenDemo() {
  QWidget* row = makeRow();
  auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
  auto* spin = new AdSpin();
  spin->setDescription(QStringLiteral("Customized"));
  AdSpin::ComponentTokens tokens;
  tokens.colors.indicator = QColor(QStringLiteral("#52c41a"));
  tokens.colors.description = QColor(QStringLiteral("#389e0d"));
  tokens.metrics.dotSize = 28;
  tokens.metrics.descriptionGap = 12;
  spin->setComponentTokens(tokens);
  layout->addWidget(spin);
  layout->addStretch();
  return row;
}
