#include "divider_docs_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "widgets/divider.h"

using adqt::widgets::AdDivider;

namespace {

QLabel* paragraph(const QString& text) {
  auto* label = new QLabel(text);
  label->setWordWrap(true);
  return label;
}

QLabel* demoLabel(const QString& text) {
  auto* label = new QLabel(text);
  label->setAlignment(Qt::AlignCenter);
  return label;
}

AdDivider* titledDivider(const QString& text,
                         AdDivider::TitlePlacement placement = AdDivider::TitlePlacement::Center) {
  auto* divider = new AdDivider(text);
  divider->setTitlePlacement(placement);
  return divider;
}

}  // namespace

DividerDocsPage::DividerDocsPage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 24);
  root->setSpacing(16);

  auto* title = new QLabel(QStringLiteral("Divider"));
  QFont titleFont = title->font();
  titleFont.setPointSize(titleFont.pointSize() + 8);
  titleFont.setBold(true);
  title->setFont(titleFont);
  root->addWidget(title);
  root->addWidget(paragraph(QStringLiteral("A divider line separates different content.")));

  addSection(root, QStringLiteral("Horizontal"),
             QStringLiteral("Use a horizontal divider between sections. Solid, dashed, and dotted "
                            "rails are supported."),
             buildHorizontalDemo());
  addSection(root, QStringLiteral("Divider with title"),
             QStringLiteral("Set text and titlePlacement to place a title at the start, center, or "
                            "end of the rail."),
             buildWithTextDemo());
  addSection(root, QStringLiteral("Size"),
             QStringLiteral("Small, middle, and large sizes control the vertical spacing of a "
                            "horizontal divider."),
             buildSizeDemo());
  addSection(
      root, QStringLiteral("Plain"),
      QStringLiteral("Plain titles use body typography instead of the default heading style."),
      buildPlainDemo());
  addSection(root, QStringLiteral("Vertical"),
             QStringLiteral("Vertical dividers are inline separators for compact rows of content."),
             buildVerticalDemo());
  addSection(root, QStringLiteral("Variant"),
             QStringLiteral("Choose solid, dotted, or dashed rails and customize their semantic "
                            "color."),
             buildVariantDemo());
  addSection(root, QStringLiteral("Component tokens"),
             QStringLiteral("Component tokens expose the same spacing and color decisions as the "
                            "Ant Design Divider."),
             buildTokenDemo());
  addSection(root, QStringLiteral("Semantic styling"),
             QStringLiteral("Semantic slots let an application style the root, rails, and title "
                            "independently."),
             buildSemanticDemo());
  root->addStretch();
}

const QVector<QWidget*>& DividerDocsPage::sectionAnchors() const { return anchors_; }

const QStringList& DividerDocsPage::sectionTitles() const { return titles_; }

void DividerDocsPage::addSection(QVBoxLayout* root, const QString& title,
                                 const QString& description, QWidget* content) {
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
  layout->addWidget(titleLabel);
  layout->addWidget(paragraph(description));
  layout->addWidget(content);

  root->addWidget(panel);
  anchors_.append(panel);
  titles_.append(title);
}

QWidget* DividerDocsPage::buildHorizontalDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(paragraph(QStringLiteral("Content before the divider.")));
  layout->addWidget(new AdDivider());
  auto* dashed = new AdDivider();
  dashed->setDashed(true);
  layout->addWidget(dashed);
  layout->addWidget(paragraph(QStringLiteral("Content after the divider.")));
  return box;
}

QWidget* DividerDocsPage::buildWithTextDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(titledDivider(QStringLiteral("Text")));
  layout->addWidget(titledDivider(QStringLiteral("Start Text"), AdDivider::TitlePlacement::Start));
  layout->addWidget(titledDivider(QStringLiteral("End Text"), AdDivider::TitlePlacement::End));
  return box;
}

QWidget* DividerDocsPage::buildSizeDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  for (const auto size :
       {AdDivider::Size::Small, AdDivider::Size::Middle, AdDivider::Size::Large}) {
    auto* divider = new AdDivider();
    divider->setDividerSize(size);
    layout->addWidget(divider);
  }
  return box;
}

QWidget* DividerDocsPage::buildPlainDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* center = titledDivider(QStringLiteral("Plain title"));
  center->setPlain(true);
  layout->addWidget(center);
  auto* start = titledDivider(QStringLiteral("Plain start"), AdDivider::TitlePlacement::Start);
  start->setPlain(true);
  layout->addWidget(start);
  auto* end = titledDivider(QStringLiteral("Plain end"), AdDivider::TitlePlacement::End);
  end->setPlain(true);
  layout->addWidget(end);
  return box;
}

QWidget* DividerDocsPage::buildVerticalDemo() {
  auto* box = new QWidget();
  auto* layout = new QHBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  layout->addWidget(demoLabel(QStringLiteral("Text")));
  auto* first = new AdDivider();
  first->setVertical(true);
  layout->addWidget(first);
  layout->addWidget(demoLabel(QStringLiteral("Link")));
  auto* second = new AdDivider();
  second->setOrientation(AdDivider::Orientation::Vertical);
  second->setVariant(AdDivider::Variant::Dotted);
  layout->addWidget(second);
  layout->addWidget(demoLabel(QStringLiteral("Another link")));
  return box;
}

QWidget* DividerDocsPage::buildVariantDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  const QList<QPair<QString, AdDivider::Variant>> variants = {
      {QStringLiteral("Solid"), AdDivider::Variant::Solid},
      {QStringLiteral("Dotted"), AdDivider::Variant::Dotted},
      {QStringLiteral("Dashed"), AdDivider::Variant::Dashed},
  };
  for (const auto& entry : variants) {
    auto* divider = new AdDivider(entry.first);
    divider->setVariant(entry.second);
    AdDivider::SemanticStyles styles;
    styles.root.borderColor = QColor(QStringLiteral("#7cb305"));
    styles.rail.borderColor = QColor(QStringLiteral("#7cb305"));
    styles.content.textColor = QColor(QStringLiteral("#389e0d"));
    divider->setSemanticStyles(styles);
    layout->addWidget(divider);
  }
  return box;
}

QWidget* DividerDocsPage::buildTokenDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* divider = new AdDivider(QStringLiteral("Tokenized"));
  AdDivider::ComponentTokens tokens;
  tokens.colors.splitColor = QColor(QStringLiteral("#1677ff"));
  tokens.metrics.lineWidth = 2.0;
  tokens.metrics.textPaddingInline = 10;
  tokens.metrics.orientationMargin = 0.2;
  tokens.metrics.verticalMarginInline = 12;
  divider->setComponentTokens(tokens);
  layout->addWidget(divider);
  return box;
}

QWidget* DividerDocsPage::buildSemanticDemo() {
  auto* box = new QWidget();
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* divider = new AdDivider(QStringLiteral("Semantic content"));
  AdDivider::SemanticStyles styles;
  styles.root.backgroundColor = QColor(QStringLiteral("#f6f7f9"));
  styles.root.borderColor = QColor(QStringLiteral("#d4380d"));
  styles.content.textColor = QColor(QStringLiteral("#ad2102"));
  styles.content.font = QFont(QStringLiteral("Segoe UI"), 11, QFont::DemiBold);
  divider->setSemanticStyles(styles);
  layout->addWidget(divider);

  auto* custom = new AdDivider(QStringLiteral("Custom child"));
  auto* label = new QLabel(QStringLiteral("A real QWidget can be used as the title."));
  label->setAlignment(Qt::AlignCenter);
  custom->setContentWidget(label);
  layout->addWidget(custom);
  return box;
}
