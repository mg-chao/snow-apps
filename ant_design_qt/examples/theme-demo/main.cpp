#include <QAbstractSlider>
#include <QApplication>
#include <QAbstractItemModel>
#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

#include "icon_theme_adapter.h"
#include "antd_icons.h"
#include "alert_docs_page.h"
#include "carousel_docs_page.h"
#include "color_picker_docs_page.h"
#include "checkbox_docs_page.h"
#include "date_picker_docs_page.h"
#include "divider_docs_page.h"
#include "descriptions_docs_page.h"
#include "form_docs_page.h"
#include "image_docs_page.h"
#include "input_docs_page.h"
#include "input_number_docs_page.h"
#include "menu_docs_page.h"
#include "message_docs_page.h"
#include "modal_docs_page.h"
#include "notification_docs_page.h"
#include "pagination_docs_page.h"
#include "popconfirm_docs_page.h"
#include "popover_docs_page.h"
#include "radio_docs_page.h"
#include "segmented_docs_page.h"
#include "select_docs_page.h"
#include "slider_docs_page.h"
#include "spin_docs_page.h"
#include "switch_docs_page.h"
#include "tabs_docs_page.h"
#include "tag_docs_page.h"
#include "tooltip_docs_page.h"
#include "theme/theme.h"
#include "widgets/detail/timing_hub.h"
#include "widgets/widgets.h"

using adqt::theme::ThemeDensity;
using adqt::theme::ThemeManager;
using adqt::theme::ThemeScheme;
using adqt::widgets::AdButton;
using adqt::widgets::AdNavigationMenu;
using adqt::widgets::AdScrollArea;
namespace outlined_icons = adqt::icons::antd::outlined;

namespace {

struct ThemePresetSelection {
  ThemeScheme scheme = ThemeScheme::Light;
  ThemeDensity density = ThemeDensity::Comfortable;
};

ThemePresetSelection configForIndex(int index) {
  switch (index) {
    case 1:
      return {ThemeScheme::Dark, ThemeDensity::Comfortable};
    case 2:
      return {ThemeScheme::Light, ThemeDensity::Compact};
    case 3:
      return {ThemeScheme::Dark, ThemeDensity::Compact};
    case 0:
    default:
      return {ThemeScheme::Light, ThemeDensity::Comfortable};
  }
}

QStandardItem* makeMenuModelItem(const QString& key, const QString& label,
                                 const adqt::icons::IconRef& icon = {}) {
  auto* item = new QStandardItem(label);
  item->setEditable(false);
  item->setData(key, AdNavigationMenu::StableIdRole);
  item->setData(static_cast<int>(AdNavigationMenu::NodeKind::Action),
                AdNavigationMenu::NodeKindRole);
  if (adqt::icons::isValid(icon)) {
    item->setData(QVariant::fromValue(icon), Qt::DecorationRole);
  }
  return item;
}

QModelIndex findMenuIndexByKey(const QAbstractItemModel* model, const QString& key,
                               const QModelIndex& parent = QModelIndex()) {
  if (!model) {
    return QModelIndex();
  }
  const int rows = model->rowCount(parent);
  for (int row = 0; row < rows; ++row) {
    const QModelIndex index = model->index(row, 0, parent);
    if (index.data(AdNavigationMenu::StableIdRole).toString() == key) {
      return index;
    }
    if (const QModelIndex child = findMenuIndexByKey(model, key, index); child.isValid()) {
      return child;
    }
  }
  return QModelIndex();
}

class GhostDemoPanel final : public QFrame {
 public:
  explicit GhostDemoPanel(QWidget* parent = nullptr) : QFrame(parent) {
    setFrameStyle(QFrame::NoFrame);
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(190, 200, 200));
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6.0, 6.0);
  }
};

class ButtonDocsPage final : public QWidget {
 public:
  explicit ButtonDocsPage(QWidget* parent = nullptr) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 24);
    root->setSpacing(16);

    auto* title = new QLabel("Button");
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 8);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* subtitle = new QLabel("Buttons are used to trigger an immediate action.");
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    addSection(
        root, "Button Types",
        "Use `buttonStyle` to switch among solid/outline/dashed/tonal/text/link button styles.",
        buildBasicDemo());
    addSection(
        root, "Color & Variant",
        "Set `accentRole` and `buttonStyle` together to compose additional button appearances.",
        buildColorVariantDemo());
    addSection(root, "Button Icon", "Set `iconRef` to show an icon in the button.",
               buildIconDemo());
    addSection(root, "Icon Position",
               "Set `iconPosition` to `leading` or `trailing` to control icon placement.",
               buildIconPositionDemo());
    addSection(root, "Button Size",
               "Buttons support large/medium/small sizes via the `sizeClass` property.",
               buildSizeDemo());
    addSection(root, "Disabled",
               "Set `disabled` to make a button non-interactive and visually disabled.",
               buildDisabledDemo());
    addSection(root, "Loading",
               "Set `busy` to show a spinner, or use `busyIconRef` for a custom loading icon.",
               buildLoadingDemo());
    addSection(root, "Qt Integration",
               "Use Qt menus, default buttons, and layout policies directly with `AdButton`.",
               buildQtIntegrationDemo());
    addSection(root, "Multiple Actions", "Compose related actions with ordinary Qt layouts.",
               buildMultipleDemo());
    addSection(root, "Ghost Button",
               "Ghost buttons invert foreground and keep transparent background.",
               buildGhostDemo());
    addSection(root, "Danger Button", "Use danger styling for destructive actions.",
               buildDangerDemo());
    addSection(
        root, "Expanded Width",
        "Use `QSizePolicy::Expanding` and layouts when a button should span the available width.",
        buildBlockDemo());
    root->addStretch();
  }

  const QVector<QWidget*>& sectionAnchors() const { return anchors_; }

  const QStringList& sectionTitles() const { return titles_; }

  void addSection(QVBoxLayout* root, const QString& title, const QString& description,
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

    auto* descLabel = new QLabel(description);
    descLabel->setWordWrap(true);

    layout->addWidget(titleLabel);
    layout->addWidget(descLabel);
    layout->addWidget(content);

    root->addWidget(panel);
    anchors_.append(panel);
    titles_.append(title);
  }

  QWidget* buildBasicDemo() {
    auto* box = new QWidget();
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);

    auto* primary = new AdButton("Primary Button");
    primary->setButtonStyle(AdButton::ButtonStyle::Solid);
    primary->setAccentRole(AdButton::AccentRole::Primary);
    row->addWidget(primary);

    row->addWidget(new AdButton("Default Button"));

    auto* dashed = new AdButton("Dashed Button");
    dashed->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashed->setAccentRole(AdButton::AccentRole::Neutral);
    row->addWidget(dashed);

    auto* text = new AdButton("Text Button");
    text->setButtonStyle(AdButton::ButtonStyle::Text);
    text->setAccentRole(AdButton::AccentRole::Neutral);
    row->addWidget(text);

    auto* link = new AdButton("Link Button");
    link->setButtonStyle(AdButton::ButtonStyle::Link);
    link->setAccentRole(AdButton::AccentRole::Neutral);
    row->addWidget(link);

    row->addStretch();
    return box;
  }

  QWidget* buildColorVariantDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    const QList<AdButton::AccentRole> colors = {
        AdButton::AccentRole::Neutral, AdButton::AccentRole::Primary, AdButton::AccentRole::Danger,
        AdButton::AccentRole::Pink,    AdButton::AccentRole::Purple,  AdButton::AccentRole::Cyan,
    };
    const QList<QPair<QString, AdButton::ButtonStyle>> variants = {
        {"Solid", AdButton::ButtonStyle::Solid},   {"Outlined", AdButton::ButtonStyle::Outline},
        {"Dashed", AdButton::ButtonStyle::Dashed}, {"Filled", AdButton::ButtonStyle::Tonal},
        {"Text", AdButton::ButtonStyle::Text},     {"Link", AdButton::ButtonStyle::Link},
    };

    for (AdButton::AccentRole color : colors) {
      auto* row = new QHBoxLayout();
      for (const auto& variant : variants) {
        auto* button = new AdButton(variant.first);
        button->setAccentRole(color);
        button->setButtonStyle(variant.second);
        row->addWidget(button);
      }

      row->addStretch();
      layout->addLayout(row);
    }

    return box;
  }

  QWidget* buildIconDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);
    const auto search = outlined_icons::Search();

    auto* pCircle = new AdButton();
    pCircle->setButtonStyle(AdButton::ButtonStyle::Solid);
    pCircle->setAccentRole(AdButton::AccentRole::Primary);
    pCircle->setShape(AdButton::Shape::Circle);
    pCircle->setIconRef(search);
    pCircle->setAccessibleName("Primary search");
    row1->addWidget(pCircle);

    auto* pCircleA = new AdButton("A");
    pCircleA->setButtonStyle(AdButton::ButtonStyle::Solid);
    pCircleA->setAccentRole(AdButton::AccentRole::Primary);
    pCircleA->setShape(AdButton::Shape::Pill);
    row1->addWidget(pCircleA);

    auto* pIcon = new AdButton("Search");
    pIcon->setButtonStyle(AdButton::ButtonStyle::Solid);
    pIcon->setAccentRole(AdButton::AccentRole::Primary);
    pIcon->setIconRef(search);
    row1->addWidget(pIcon);

    auto* defaultCircle = new AdButton();
    defaultCircle->setShape(AdButton::Shape::Circle);
    defaultCircle->setIconRef(search);
    defaultCircle->setAccessibleName("Search");
    row1->addWidget(defaultCircle);

    auto* defaultIcon = new AdButton("Search");
    defaultIcon->setIconRef(search);
    row1->addWidget(defaultIcon);
    row1->addStretch();

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);
    auto* defaultCircle2 = new AdButton();
    defaultCircle2->setShape(AdButton::Shape::Circle);
    defaultCircle2->setIconRef(search);
    defaultCircle2->setAccessibleName("Search");
    row2->addWidget(defaultCircle2);

    auto* defaultIcon2 = new AdButton("Search");
    defaultIcon2->setIconRef(search);
    row2->addWidget(defaultIcon2);

    auto* dashedCircle = new AdButton();
    dashedCircle->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashedCircle->setAccentRole(AdButton::AccentRole::Neutral);
    dashedCircle->setShape(AdButton::Shape::Circle);
    dashedCircle->setIconRef(search);
    dashedCircle->setAccessibleName("Dashed search");
    row2->addWidget(dashedCircle);

    auto* dashedIcon = new AdButton("Search");
    dashedIcon->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashedIcon->setAccentRole(AdButton::AccentRole::Neutral);
    dashedIcon->setIconRef(search);
    row2->addWidget(dashedIcon);

    auto* defaultIconOnly = new AdButton();
    defaultIconOnly->setIconRef(search);
    defaultIconOnly->setAccessibleName("Search");
    row2->addWidget(defaultIconOnly);
    row2->addStretch();

    layout->addLayout(row1);
    layout->addLayout(row2);
    return box;
  }

  QWidget* buildIconPositionDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* switchRow = new QHBoxLayout();
    switchRow->addWidget(new QLabel("iconPosition:"));
    iconPositionBox_ = new QComboBox();
    iconPositionBox_->addItem("leading", static_cast<int>(AdButton::IconPosition::Leading));
    iconPositionBox_->addItem("trailing", static_cast<int>(AdButton::IconPosition::Trailing));
    iconPositionBox_->setCurrentIndex(1);
    switchRow->addWidget(iconPositionBox_);
    switchRow->addStretch();
    layout->addLayout(switchRow);

    iconPositionButtons_.clear();
    const auto icon = outlined_icons::Search();

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);

    auto* primaryCircle = new AdButton();
    primaryCircle->setButtonStyle(AdButton::ButtonStyle::Solid);
    primaryCircle->setAccentRole(AdButton::AccentRole::Primary);
    primaryCircle->setShape(AdButton::Shape::Circle);
    primaryCircle->setIconRef(icon);
    primaryCircle->setAccessibleName("Primary search");
    row1->addWidget(primaryCircle);

    auto* primaryCircleText = new AdButton("A");
    primaryCircleText->setButtonStyle(AdButton::ButtonStyle::Solid);
    primaryCircleText->setAccentRole(AdButton::AccentRole::Primary);
    primaryCircleText->setShape(AdButton::Shape::Pill);
    row1->addWidget(primaryCircleText);

    auto* primarySearch = new AdButton("Search");
    primarySearch->setButtonStyle(AdButton::ButtonStyle::Solid);
    primarySearch->setAccentRole(AdButton::AccentRole::Primary);
    primarySearch->setIconRef(icon);
    row1->addWidget(primarySearch);
    iconPositionButtons_.append(primarySearch);

    auto* defaultCircle = new AdButton();
    defaultCircle->setShape(AdButton::Shape::Circle);
    defaultCircle->setIconRef(icon);
    defaultCircle->setAccessibleName("Search");
    row1->addWidget(defaultCircle);

    auto* defaultSearch = new AdButton("Search");
    defaultSearch->setIconRef(icon);
    row1->addWidget(defaultSearch);
    iconPositionButtons_.append(defaultSearch);

    row1->addStretch();
    layout->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);

    auto* defaultCircle2 = new AdButton();
    defaultCircle2->setShape(AdButton::Shape::Circle);
    defaultCircle2->setIconRef(icon);
    defaultCircle2->setAccessibleName("Search");
    row2->addWidget(defaultCircle2);

    auto* textSearch = new AdButton("Search");
    textSearch->setButtonStyle(AdButton::ButtonStyle::Text);
    textSearch->setAccentRole(AdButton::AccentRole::Neutral);
    textSearch->setIconRef(icon);
    row2->addWidget(textSearch);
    iconPositionButtons_.append(textSearch);

    auto* dashedCircle = new AdButton();
    dashedCircle->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashedCircle->setAccentRole(AdButton::AccentRole::Neutral);
    dashedCircle->setShape(AdButton::Shape::Circle);
    dashedCircle->setIconRef(icon);
    dashedCircle->setAccessibleName("Dashed search");
    row2->addWidget(dashedCircle);

    auto* dashedSearch = new AdButton("Search");
    dashedSearch->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashedSearch->setAccentRole(AdButton::AccentRole::Neutral);
    dashedSearch->setIconRef(icon);
    row2->addWidget(dashedSearch);
    iconPositionButtons_.append(dashedSearch);

    auto* iconOnly = new AdButton();
    iconOnly->setIconRef(icon);
    iconOnly->setAccessibleName("Search");
    row2->addWidget(iconOnly);
    iconPositionButtons_.append(iconOnly);

    auto* loadingButton = new AdButton("Loading");
    loadingButton->setButtonStyle(AdButton::ButtonStyle::Solid);
    loadingButton->setAccentRole(AdButton::AccentRole::Primary);
    loadingButton->setBusy(true);
    row2->addWidget(loadingButton);
    iconPositionButtons_.append(loadingButton);

    row2->addStretch();
    layout->addLayout(row2);

    const auto applyPlacement = [this](AdButton::IconPosition placement) {
      for (AdButton* button : iconPositionButtons_) {
        if (button) {
          button->setIconPosition(placement);
        }
      }
    };

    applyPlacement(static_cast<AdButton::IconPosition>(iconPositionBox_->currentData().toInt()));

    connect(iconPositionBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
              const auto placement =
                  static_cast<AdButton::IconPosition>(iconPositionBox_->itemData(index).toInt());
              for (AdButton* button : iconPositionButtons_) {
                if (button) {
                  button->setIconPosition(placement);
                }
              }
            });

    return box;
  }

  QWidget* buildSizeDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* sizeRow = new QHBoxLayout();
    sizeRow->addWidget(new QLabel("size:"));

    auto* group = new QButtonGroup(this);
    auto* large = new QRadioButton("large");
    auto* medium = new QRadioButton("medium");
    auto* small = new QRadioButton("small");
    large->setChecked(true);
    group->addButton(large, static_cast<int>(AdButton::SizeClass::Large));
    group->addButton(medium, static_cast<int>(AdButton::SizeClass::Medium));
    group->addButton(small, static_cast<int>(AdButton::SizeClass::Small));
    sizeRow->addWidget(large);
    sizeRow->addWidget(medium);
    sizeRow->addWidget(small);
    sizeRow->addStretch();
    layout->addLayout(sizeRow);

    auto* line1 = new QHBoxLayout();
    sizePrimary_ = new AdButton("Primary");
    sizePrimary_->setButtonStyle(AdButton::ButtonStyle::Solid);
    sizePrimary_->setAccentRole(AdButton::AccentRole::Primary);
    sizeDefault_ = new AdButton("Default");
    sizeDashed_ = new AdButton("Dashed");
    sizeDashed_->setButtonStyle(AdButton::ButtonStyle::Dashed);
    sizeDashed_->setAccentRole(AdButton::AccentRole::Neutral);
    line1->addWidget(sizePrimary_);
    line1->addWidget(sizeDefault_);
    line1->addWidget(sizeDashed_);
    line1->addStretch();
    layout->addLayout(line1);

    auto* line2 = new QHBoxLayout();
    sizeLink_ = new AdButton("Link");
    sizeLink_->setButtonStyle(AdButton::ButtonStyle::Link);
    sizeLink_->setAccentRole(AdButton::AccentRole::Neutral);
    line2->addWidget(sizeLink_);
    line2->addStretch();
    layout->addLayout(line2);

    auto* line3 = new QHBoxLayout();
    const auto dl = outlined_icons::Download();
    sizeIconOnly_ = new AdButton();
    sizeIconOnly_->setButtonStyle(AdButton::ButtonStyle::Solid);
    sizeIconOnly_->setAccentRole(AdButton::AccentRole::Primary);
    sizeIconOnly_->setIconRef(dl);
    sizeIconOnly_->setAccessibleName("Download");
    sizeCircle_ = new AdButton();
    sizeCircle_->setButtonStyle(AdButton::ButtonStyle::Solid);
    sizeCircle_->setAccentRole(AdButton::AccentRole::Primary);
    sizeCircle_->setShape(AdButton::Shape::Circle);
    sizeCircle_->setIconRef(dl);
    sizeCircle_->setAccessibleName("Download");
    sizeRoundIcon_ = new AdButton();
    sizeRoundIcon_->setButtonStyle(AdButton::ButtonStyle::Solid);
    sizeRoundIcon_->setAccentRole(AdButton::AccentRole::Primary);
    sizeRoundIcon_->setShape(AdButton::Shape::Pill);
    sizeRoundIcon_->setIconRef(dl);
    sizeRoundIcon_->setAccessibleName("Download");
    sizeRoundText_ = new AdButton("Download");
    sizeRoundText_->setButtonStyle(AdButton::ButtonStyle::Solid);
    sizeRoundText_->setAccentRole(AdButton::AccentRole::Primary);
    sizeRoundText_->setShape(AdButton::Shape::Pill);
    sizeRoundText_->setIconRef(dl);
    sizePlainText_ = new AdButton("Download");
    sizePlainText_->setButtonStyle(AdButton::ButtonStyle::Solid);
    sizePlainText_->setAccentRole(AdButton::AccentRole::Primary);
    sizePlainText_->setIconRef(dl);
    line3->addWidget(sizeIconOnly_);
    line3->addWidget(sizeCircle_);
    line3->addWidget(sizeRoundIcon_);
    line3->addWidget(sizeRoundText_);
    line3->addWidget(sizePlainText_);
    line3->addStretch();
    layout->addLayout(line3);

    connect(group, QOverload<int>::of(&QButtonGroup::idClicked), this,
            [this](int id) { applySizeToDemo(static_cast<AdButton::SizeClass>(id)); });
    applySizeToDemo(AdButton::SizeClass::Large);

    return box;
  }

  void applySizeToDemo(AdButton::SizeClass size) {
    const QList<AdButton*> buttons = {
        sizePrimary_, sizeDefault_,   sizeDashed_,    sizeLink_,      sizeIconOnly_,
        sizeCircle_,  sizeRoundIcon_, sizeRoundText_, sizePlainText_,
    };
    for (AdButton* button : buttons) {
      if (button) {
        button->setSizeClass(size);
      }
    }
  }

  QWidget* buildDisabledDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto addRow = [layout](const std::function<void(QHBoxLayout*)>& fill) {
      auto* row = new QHBoxLayout();
      fill(row);
      row->addStretch();
      layout->addLayout(row);
    };

    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Primary");
      a->setButtonStyle(AdButton::ButtonStyle::Solid);
      a->setAccentRole(AdButton::AccentRole::Primary);
      auto* b = new AdButton("Primary(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Solid);
      b->setAccentRole(AdButton::AccentRole::Primary);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Default");
      auto* b = new AdButton("Default(disabled)");
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Dashed");
      a->setButtonStyle(AdButton::ButtonStyle::Dashed);
      a->setAccentRole(AdButton::AccentRole::Neutral);
      auto* b = new AdButton("Dashed(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Dashed);
      b->setAccentRole(AdButton::AccentRole::Neutral);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Text");
      a->setButtonStyle(AdButton::ButtonStyle::Text);
      a->setAccentRole(AdButton::AccentRole::Neutral);
      auto* b = new AdButton("Text(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Text);
      b->setAccentRole(AdButton::AccentRole::Neutral);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Link");
      a->setButtonStyle(AdButton::ButtonStyle::Link);
      a->setAccentRole(AdButton::AccentRole::Neutral);
      auto* b = new AdButton("Link(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Link);
      b->setAccentRole(AdButton::AccentRole::Neutral);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Href Primary");
      a->setButtonStyle(AdButton::ButtonStyle::Solid);
      a->setAccentRole(AdButton::AccentRole::Primary);
      auto* b = new AdButton("Href Primary(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Solid);
      b->setAccentRole(AdButton::AccentRole::Primary);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Danger Default");
      a->setAccentRole(AdButton::AccentRole::Danger);
      auto* b = new AdButton("Danger Default(disabled)");
      b->setAccentRole(AdButton::AccentRole::Danger);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Danger Text");
      a->setButtonStyle(AdButton::ButtonStyle::Text);
      a->setAccentRole(AdButton::AccentRole::Neutral);
      a->setAccentRole(AdButton::AccentRole::Danger);
      auto* b = new AdButton("Danger Text(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Text);
      b->setAccentRole(AdButton::AccentRole::Neutral);
      b->setAccentRole(AdButton::AccentRole::Danger);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* a = new AdButton("Danger Link");
      a->setButtonStyle(AdButton::ButtonStyle::Link);
      a->setAccentRole(AdButton::AccentRole::Neutral);
      a->setAccentRole(AdButton::AccentRole::Danger);
      auto* b = new AdButton("Danger Link(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::Link);
      b->setAccentRole(AdButton::AccentRole::Neutral);
      b->setAccentRole(AdButton::AccentRole::Danger);
      b->setEnabled(false);
      row->addWidget(a);
      row->addWidget(b);
    });
    addRow([](QHBoxLayout* row) {
      auto* wrap = new GhostDemoPanel();
      auto* ghostRow = new QHBoxLayout(wrap);
      ghostRow->setContentsMargins(12, 8, 12, 8);
      ghostRow->setSpacing(8);

      auto* a = new AdButton("Ghost");
      a->setButtonStyle(AdButton::ButtonStyle::GhostOutline);
      auto* b = new AdButton("Ghost(disabled)");
      b->setButtonStyle(AdButton::ButtonStyle::GhostOutline);
      b->setEnabled(false);

      ghostRow->addWidget(a);
      ghostRow->addWidget(b);
      ghostRow->addStretch();
      row->addWidget(wrap);
    });

    return box;
  }

  QWidget* buildLoadingDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* row1 = new QHBoxLayout();
    auto* loading = new AdButton("Loading");
    loading->setButtonStyle(AdButton::ButtonStyle::Solid);
    loading->setAccentRole(AdButton::AccentRole::Primary);
    loading->setBusy(true);
    row1->addWidget(loading);

    auto* loadingSmall = new AdButton("Loading");
    loadingSmall->setButtonStyle(AdButton::ButtonStyle::Solid);
    loadingSmall->setAccentRole(AdButton::AccentRole::Primary);
    loadingSmall->setSizeClass(AdButton::SizeClass::Small);
    loadingSmall->setBusy(true);
    row1->addWidget(loadingSmall);

    auto* loadingIcon = new AdButton();
    loadingIcon->setButtonStyle(AdButton::ButtonStyle::Solid);
    loadingIcon->setAccentRole(AdButton::AccentRole::Primary);
    loadingIcon->setIconRef(outlined_icons::Poweroff());
    loadingIcon->setAccessibleName("Power");
    loadingIcon->setBusy(true);
    row1->addWidget(loadingIcon);

    auto* loadingCustom = new AdButton("Loading Icon");
    loadingCustom->setButtonStyle(AdButton::ButtonStyle::Solid);
    loadingCustom->setAccentRole(AdButton::AccentRole::Primary);
    loadingCustom->setBusyIconRef(outlined_icons::Sync());
    loadingCustom->setBusy(true);
    row1->addWidget(loadingCustom);
    row1->addStretch();
    layout->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);
    const auto powerIcon = outlined_icons::Poweroff();
    const auto syncIcon = outlined_icons::Sync();
    auto* iconStart = new AdButton("Icon Start");
    iconStart->setButtonStyle(AdButton::ButtonStyle::Solid);
    iconStart->setAccentRole(AdButton::AccentRole::Primary);
    iconStart->setIconRef(powerIcon);

    auto* iconEnd = new AdButton("Icon End");
    iconEnd->setButtonStyle(AdButton::ButtonStyle::Solid);
    iconEnd->setAccentRole(AdButton::AccentRole::Primary);
    iconEnd->setIconPosition(AdButton::IconPosition::Trailing);
    iconEnd->setIconRef(powerIcon);

    auto* iconReplace = new AdButton("Icon Replace");
    iconReplace->setButtonStyle(AdButton::ButtonStyle::Solid);
    iconReplace->setAccentRole(AdButton::AccentRole::Primary);
    iconReplace->setIconRef(powerIcon);

    auto* iconOnly = new AdButton();
    iconOnly->setButtonStyle(AdButton::ButtonStyle::Solid);
    iconOnly->setAccentRole(AdButton::AccentRole::Primary);
    iconOnly->setIconRef(powerIcon);
    iconOnly->setAccessibleName("Power");

    auto* loadingIconButton = new AdButton("Loading Icon");
    loadingIconButton->setButtonStyle(AdButton::ButtonStyle::Solid);
    loadingIconButton->setAccentRole(AdButton::AccentRole::Primary);
    loadingIconButton->setIconRef(powerIcon);
    loadingIconButton->setBusyIconRef(syncIcon);

    auto startLoadingFor = [](AdButton* button, int ms) {
      if (!button) {
        return;
      }
      button->setBusy(true);
      adqt::widgets::detail::scheduleTimingTask(button, QStringLiteral("ThemeDemo.ButtonLoading"),
                                                ms, [button]() {
                                                  if (button) {
                                                    button->setBusy(false);
                                                  }
                                                });
    };

    auto startLoadingPairFor = [](AdButton* first, AdButton* second, int ms) {
      if (first) {
        first->setBusy(true);
      }
      if (second) {
        second->setBusy(true);
      }
      QObject* owner = first ? static_cast<QObject*>(first) : static_cast<QObject*>(second);
      if (!owner) {
        return;
      }
      adqt::widgets::detail::scheduleTimingTask(
          owner, QStringLiteral("ThemeDemo.ButtonPairLoading"), ms, [first, second]() {
            if (first) {
              first->setBusy(false);
            }
            if (second) {
              second->setBusy(false);
            }
          });
    };

    connect(iconStart, &QAbstractButton::clicked, this,
            [iconStart, startLoadingFor]() { startLoadingFor(iconStart, 3000); });
    connect(iconEnd, &QAbstractButton::clicked, this,
            [iconEnd, startLoadingFor]() { startLoadingFor(iconEnd, 3000); });
    connect(iconReplace, &QAbstractButton::clicked, this,
            [iconReplace, startLoadingFor]() { startLoadingFor(iconReplace, 3000); });
    connect(iconOnly, &QAbstractButton::clicked, this,
            [iconOnly, loadingIconButton, startLoadingPairFor]() {
              startLoadingPairFor(iconOnly, loadingIconButton, 3000);
            });
    connect(loadingIconButton, &QAbstractButton::clicked, this,
            [iconOnly, loadingIconButton, startLoadingPairFor]() {
              startLoadingPairFor(iconOnly, loadingIconButton, 3000);
            });

    row2->addWidget(iconStart);
    row2->addWidget(iconEnd);
    row2->addWidget(iconReplace);
    row2->addWidget(iconOnly);
    row2->addWidget(loadingIconButton);
    row2->addStretch();
    layout->addLayout(row2);

    return box;
  }

  QWidget* buildQtIntegrationDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    auto* menuButton = new AdButton("Actions");
    auto* menu = new QMenu(menuButton);
    menu->addAction("Rename");
    menu->addAction("Duplicate");
    menu->addSeparator();
    menu->addAction("Archive");
    menuButton->setMenu(menu);

    auto* defaultButton = new AdButton("Default Action");
    defaultButton->setButtonStyle(AdButton::ButtonStyle::Solid);
    defaultButton->setAccentRole(AdButton::AccentRole::Primary);
    defaultButton->setDefault(true);

    topRow->addWidget(menuButton);
    topRow->addWidget(defaultButton);
    topRow->addStretch();
    layout->addLayout(topRow);

    auto* note = new QLabel(
        "Use Qt menus directly, and call setDefault(true) when a dialog action should stand out.");
    note->setWordWrap(true);
    layout->addWidget(note);

    return box;
  }

  QWidget* buildGhostDemo() {
    auto* wrap = new GhostDemoPanel();
    auto* layout = new QHBoxLayout(wrap);
    layout->setContentsMargins(12, 12, 12, 12);

    auto* primary = new AdButton("Primary");
    primary->setButtonStyle(AdButton::ButtonStyle::Solid);
    primary->setAccentRole(AdButton::AccentRole::Primary);
    primary->setButtonStyle(AdButton::ButtonStyle::GhostOutline);
    layout->addWidget(primary);

    auto* normal = new AdButton("Default");
    normal->setButtonStyle(AdButton::ButtonStyle::GhostOutline);
    layout->addWidget(normal);

    auto* dashed = new AdButton("Dashed");
    dashed->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashed->setAccentRole(AdButton::AccentRole::Neutral);
    dashed->setButtonStyle(AdButton::ButtonStyle::GhostDashed);
    layout->addWidget(dashed);

    auto* danger = new AdButton("Danger");
    danger->setButtonStyle(AdButton::ButtonStyle::Solid);
    danger->setAccentRole(AdButton::AccentRole::Danger);
    danger->setButtonStyle(AdButton::ButtonStyle::GhostOutline);
    layout->addWidget(danger);
    layout->addStretch();

    return wrap;
  }

  QWidget* buildDangerDemo() {
    auto* box = new QWidget();
    auto* row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 0, 0);

    auto* primary = new AdButton("Primary");
    primary->setButtonStyle(AdButton::ButtonStyle::Solid);
    primary->setAccentRole(AdButton::AccentRole::Danger);
    row->addWidget(primary);

    auto* normal = new AdButton("Default");
    normal->setAccentRole(AdButton::AccentRole::Danger);
    row->addWidget(normal);

    auto* dashed = new AdButton("Dashed");
    dashed->setButtonStyle(AdButton::ButtonStyle::Dashed);
    dashed->setAccentRole(AdButton::AccentRole::Danger);
    row->addWidget(dashed);

    auto* text = new AdButton("Text");
    text->setButtonStyle(AdButton::ButtonStyle::Text);
    text->setAccentRole(AdButton::AccentRole::Danger);
    row->addWidget(text);

    auto* link = new AdButton("Link");
    link->setButtonStyle(AdButton::ButtonStyle::Link);
    link->setAccentRole(AdButton::AccentRole::Danger);
    row->addWidget(link);
    row->addStretch();

    return box;
  }

  QWidget* buildBlockDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto addButton = [layout](const QString& text, AdButton::ButtonStyle variant,
                              AdButton::AccentRole tone, bool disabled) {
      auto* button = new AdButton(text);
      button->setButtonStyle(variant);
      button->setAccentRole(tone);
      button->setEnabled(!disabled);
      button->setSizePolicy(QSizePolicy::Expanding, button->sizePolicy().verticalPolicy());
      layout->addWidget(button);
    };

    addButton("Primary", AdButton::ButtonStyle::Solid, AdButton::AccentRole::Primary, false);
    addButton("Default", AdButton::ButtonStyle::Outline, AdButton::AccentRole::Neutral, false);
    addButton("Dashed", AdButton::ButtonStyle::Dashed, AdButton::AccentRole::Neutral, false);
    addButton("disabled", AdButton::ButtonStyle::Outline, AdButton::AccentRole::Neutral, true);
    addButton("text", AdButton::ButtonStyle::Text, AdButton::AccentRole::Neutral, false);
    addButton("Link", AdButton::ButtonStyle::Link, AdButton::AccentRole::Neutral, false);

    return box;
  }

  QWidget* buildMultipleDemo() {
    auto* box = new QWidget();
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* primary = new AdButton("primary");
    primary->setButtonStyle(AdButton::ButtonStyle::Solid);
    primary->setAccentRole(AdButton::AccentRole::Primary);
    layout->addWidget(primary, 0, Qt::AlignLeft);

    auto* secondary = new AdButton("secondary");
    layout->addWidget(secondary, 0, Qt::AlignLeft);

    auto* actionsRow = new QHBoxLayout();
    actionsRow->setContentsMargins(0, 0, 0, 0);
    actionsRow->setSpacing(8);
    auto* actions = new AdButton("Actions");
    auto* more = new AdButton("...");
    actionsRow->addWidget(actions);
    actionsRow->addWidget(more);
    actionsRow->addStretch();
    layout->addLayout(actionsRow);

    return box;
  }

  QVector<QWidget*> anchors_;
  QStringList titles_;

  QComboBox* iconPositionBox_ = nullptr;
  QList<AdButton*> iconPositionButtons_;

  AdButton* sizePrimary_ = nullptr;
  AdButton* sizeDefault_ = nullptr;
  AdButton* sizeDashed_ = nullptr;
  AdButton* sizeLink_ = nullptr;
  AdButton* sizeIconOnly_ = nullptr;
  AdButton* sizeCircle_ = nullptr;
  AdButton* sizeRoundIcon_ = nullptr;
  AdButton* sizeRoundText_ = nullptr;
  AdButton* sizePlainText_ = nullptr;
};

class DemoWindow final : public QWidget {
 public:
  DemoWindow() {
    setWindowTitle("ant_design_qt docs demo");
    resize(1320, 820);

    auto* root = new QVBoxLayout(this);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("Theme:");
    modeBox_ = new QComboBox();
    modeBox_->addItem("Light");
    modeBox_->addItem("Dark");
    modeBox_->addItem("Light / Compact");
    modeBox_->addItem("Dark / Compact");
    header->addWidget(title);
    header->addWidget(modeBox_);
    header->addStretch();
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    navMenu_ = new AdNavigationMenu();
    navMenu_->setIndentation(24);
    navMenu_->setMode(AdNavigationMenu::Mode::Inline);
    navModel_ = new QStandardItemModel(this);
    navMenu_->setModel(navModel_);
    navScroll_ = new AdScrollArea();
    navScroll_->setFixedWidth(256);
    navScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navScroll_->setContentWidget(navMenu_);
    body->addWidget(navScroll_);

    scroll_ = new QScrollArea();
    scroll_->setWidgetResizable(true);
    docsStack_ = new QStackedWidget();
    scroll_->setWidget(docsStack_);
    body->addWidget(scroll_, 1);

    ensureDocsPage(DocsKind::Button);
    navOpenKeys_ = QStringList{docsGroupKey(DocsKind::Button)};

    root->addLayout(body, 1);

    rebuildNavMenuItems();

    connect(navMenu_, &AdNavigationMenu::activated, this,
            [this](const QModelIndex& index) { handleNavLeafTrigger(index); });
    connect(navMenu_, &AdNavigationMenu::expanded, this,
            [this](const QModelIndex& index) { handleNavExpandedChanged(index, true); });
    connect(
        navMenu_,
        static_cast<void (AdNavigationMenu::*)(const QModelIndex&)>(&AdNavigationMenu::collapsed),
        this, [this](const QModelIndex& index) { handleNavExpandedChanged(index, false); });
    connect(scroll_->verticalScrollBar(), &QAbstractSlider::valueChanged, this,
            [this](int value) { syncNavToScroll(value); });

    connect(modeBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyThemeFromControls(); });

    switchDocs(DocsKind::Button, true);
    syncNavMenuSelection(sectionKey(DocsKind::Button, 0), DocsKind::Button);
    applyThemeFromControls();
  }

 private:
  enum class DocsKind {
    Button,
    Alert,
    Input,
    Form,
    Descriptions,
    Divider,
    InputNumber,
    Switch,
    Segmented,
    Menu,
    Message,
    Notification,
    Modal,
    Select,
    Slider,
    Spin,
    Pagination,
    Tabs,
    Carousel,
    ColorPicker,
    DatePicker,
    Image,
    Popover,
    Popconfirm,
    Tooltip,
    Radio,
    Checkbox,
    Tag,
  };

  QString docsRootKey(DocsKind kind) const {
    if (kind == DocsKind::Button) {
      return QStringLiteral("button-docs");
    }
    if (kind == DocsKind::Alert) {
      return QStringLiteral("alert-docs");
    }
    if (kind == DocsKind::Input) {
      return QStringLiteral("input-docs");
    }
    if (kind == DocsKind::Form) {
      return QStringLiteral("form-docs");
    }
    if (kind == DocsKind::Descriptions) {
      return QStringLiteral("descriptions-docs");
    }
    if (kind == DocsKind::Divider) {
      return QStringLiteral("divider-docs");
    }
    if (kind == DocsKind::InputNumber) {
      return QStringLiteral("inputnumber-docs");
    }
    if (kind == DocsKind::Switch) {
      return QStringLiteral("switch-docs");
    }
    if (kind == DocsKind::Segmented) {
      return QStringLiteral("segmented-docs");
    }
    if (kind == DocsKind::Menu) {
      return QStringLiteral("menu-docs");
    }
    if (kind == DocsKind::Message) {
      return QStringLiteral("message-docs");
    }
    if (kind == DocsKind::Notification) {
      return QStringLiteral("notification-docs");
    }
    if (kind == DocsKind::Modal) {
      return QStringLiteral("modal-docs");
    }
    if (kind == DocsKind::Select) {
      return QStringLiteral("select-docs");
    }
    if (kind == DocsKind::Slider) {
      return QStringLiteral("slider-docs");
    }
    if (kind == DocsKind::Spin) {
      return QStringLiteral("spin-docs");
    }
    if (kind == DocsKind::Pagination) {
      return QStringLiteral("pagination-docs");
    }
    if (kind == DocsKind::Tabs) {
      return QStringLiteral("tabs-docs");
    }
    if (kind == DocsKind::Carousel) {
      return QStringLiteral("carousel-docs");
    }
    if (kind == DocsKind::ColorPicker) {
      return QStringLiteral("colorpicker-docs");
    }
    if (kind == DocsKind::DatePicker) {
      return QStringLiteral("datepicker-docs");
    }
    if (kind == DocsKind::Image) {
      return QStringLiteral("image-docs");
    }
    if (kind == DocsKind::Popover) {
      return QStringLiteral("popover-docs");
    }
    if (kind == DocsKind::Popconfirm) {
      return QStringLiteral("popconfirm-docs");
    }
    if (kind == DocsKind::Tooltip) {
      return QStringLiteral("tooltip-docs");
    }
    if (kind == DocsKind::Radio) {
      return QStringLiteral("radio-docs");
    }
    if (kind == DocsKind::Checkbox) {
      return QStringLiteral("checkbox-docs");
    }
    return QStringLiteral("tag-docs");
  }

  QString docsGroupKey(DocsKind kind) const { return docsRootKey(kind) + QStringLiteral(":group"); }

  QString sectionKey(DocsKind kind, int row) const {
    QString prefix;
    if (kind == DocsKind::Button) {
      prefix = QStringLiteral("button");
    } else if (kind == DocsKind::Alert) {
      prefix = QStringLiteral("alert");
    } else if (kind == DocsKind::Input) {
      prefix = QStringLiteral("input");
    } else if (kind == DocsKind::Form) {
      prefix = QStringLiteral("form");
    } else if (kind == DocsKind::Descriptions) {
      prefix = QStringLiteral("descriptions");
    } else if (kind == DocsKind::Divider) {
      prefix = QStringLiteral("divider");
    } else if (kind == DocsKind::InputNumber) {
      prefix = QStringLiteral("inputnumber");
    } else if (kind == DocsKind::Switch) {
      prefix = QStringLiteral("switch");
    } else if (kind == DocsKind::Segmented) {
      prefix = QStringLiteral("segmented");
    } else if (kind == DocsKind::Menu) {
      prefix = QStringLiteral("menu");
    } else if (kind == DocsKind::Message) {
      prefix = QStringLiteral("message");
    } else if (kind == DocsKind::Notification) {
      prefix = QStringLiteral("notification");
    } else if (kind == DocsKind::Modal) {
      prefix = QStringLiteral("modal");
    } else if (kind == DocsKind::Select) {
      prefix = QStringLiteral("select");
    } else if (kind == DocsKind::Slider) {
      prefix = QStringLiteral("slider");
    } else if (kind == DocsKind::Spin) {
      prefix = QStringLiteral("spin");
    } else if (kind == DocsKind::Pagination) {
      prefix = QStringLiteral("pagination");
    } else if (kind == DocsKind::Tabs) {
      prefix = QStringLiteral("tabs");
    } else if (kind == DocsKind::Carousel) {
      prefix = QStringLiteral("carousel");
    } else if (kind == DocsKind::ColorPicker) {
      prefix = QStringLiteral("colorpicker");
    } else if (kind == DocsKind::DatePicker) {
      prefix = QStringLiteral("datepicker");
    } else if (kind == DocsKind::Image) {
      prefix = QStringLiteral("image");
    } else if (kind == DocsKind::Popover) {
      prefix = QStringLiteral("popover");
    } else if (kind == DocsKind::Popconfirm) {
      prefix = QStringLiteral("popconfirm");
    } else if (kind == DocsKind::Tooltip) {
      prefix = QStringLiteral("tooltip");
    } else if (kind == DocsKind::Radio) {
      prefix = QStringLiteral("radio");
    } else if (kind == DocsKind::Checkbox) {
      prefix = QStringLiteral("checkbox");
    } else {
      prefix = QStringLiteral("tag");
    }
    return QStringLiteral("%1-section-%2").arg(prefix).arg(row);
  }

  bool parseSectionKey(const QString& key, DocsKind* kind, int* row) const {
    if (key.startsWith(QStringLiteral("button-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("button-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Button;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("alert-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("alert-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Alert;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("input-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("input-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Input;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("inputnumber-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("inputnumber-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::InputNumber;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("divider-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("divider-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Divider;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("switch-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("switch-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Switch;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("segmented-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("segmented-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Segmented;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("menu-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("menu-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Menu;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("message-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("message-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Message;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("notification-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("notification-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Notification;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("modal-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("modal-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Modal;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("select-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("select-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Select;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("slider-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("slider-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Slider;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("pagination-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("pagination-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Pagination;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("tabs-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("tabs-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Tabs;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("carousel-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("carousel-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Carousel;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("colorpicker-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("colorpicker-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::ColorPicker;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("datepicker-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("datepicker-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::DatePicker;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("image-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("image-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Image;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("popover-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("popover-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Popover;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("popconfirm-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("popconfirm-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Popconfirm;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("tooltip-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("tooltip-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Tooltip;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("radio-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("radio-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Radio;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("checkbox-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("checkbox-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Checkbox;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("spin-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("spin-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Spin;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("tag-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("tag-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Tag;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    if (key.startsWith(QStringLiteral("descriptions-section-"))) {
      bool ok = false;
      const int value = key.mid(QStringLiteral("descriptions-section-").size()).toInt(&ok);
      if (ok) {
        if (kind) {
          *kind = DocsKind::Descriptions;
        }
        if (row) {
          *row = value;
        }
        return true;
      }
      return false;
    }
    return false;
  }

  bool parseDocsRootKey(const QString& key, DocsKind* kind) const {
    if (key == docsRootKey(DocsKind::Button)) {
      if (kind) {
        *kind = DocsKind::Button;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Alert)) {
      if (kind) {
        *kind = DocsKind::Alert;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Input)) {
      if (kind) {
        *kind = DocsKind::Input;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Form)) {
      if (kind) {
        *kind = DocsKind::Form;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Descriptions)) {
      if (kind) {
        *kind = DocsKind::Descriptions;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Divider)) {
      if (kind) {
        *kind = DocsKind::Divider;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::InputNumber)) {
      if (kind) {
        *kind = DocsKind::InputNumber;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Switch)) {
      if (kind) {
        *kind = DocsKind::Switch;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Segmented)) {
      if (kind) {
        *kind = DocsKind::Segmented;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Menu)) {
      if (kind) {
        *kind = DocsKind::Menu;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Message)) {
      if (kind) {
        *kind = DocsKind::Message;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Notification)) {
      if (kind) {
        *kind = DocsKind::Notification;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Modal)) {
      if (kind) {
        *kind = DocsKind::Modal;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Select)) {
      if (kind) {
        *kind = DocsKind::Select;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Slider)) {
      if (kind) {
        *kind = DocsKind::Slider;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Spin)) {
      if (kind) {
        *kind = DocsKind::Spin;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Pagination)) {
      if (kind) {
        *kind = DocsKind::Pagination;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Tabs)) {
      if (kind) {
        *kind = DocsKind::Tabs;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Carousel)) {
      if (kind) {
        *kind = DocsKind::Carousel;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::ColorPicker)) {
      if (kind) {
        *kind = DocsKind::ColorPicker;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::DatePicker)) {
      if (kind) {
        *kind = DocsKind::DatePicker;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Image)) {
      if (kind) {
        *kind = DocsKind::Image;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Popover)) {
      if (kind) {
        *kind = DocsKind::Popover;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Popconfirm)) {
      if (kind) {
        *kind = DocsKind::Popconfirm;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Tooltip)) {
      if (kind) {
        *kind = DocsKind::Tooltip;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Radio)) {
      if (kind) {
        *kind = DocsKind::Radio;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Checkbox)) {
      if (kind) {
        *kind = DocsKind::Checkbox;
      }
      return true;
    }
    if (key == docsRootKey(DocsKind::Tag)) {
      if (kind) {
        *kind = DocsKind::Tag;
      }
      return true;
    }
    return false;
  }

  bool parseDocsGroupKey(const QString& key, DocsKind* kind) const {
    for (const DocsKind candidate :
         {DocsKind::Button,      DocsKind::Alert,        DocsKind::Input,
          DocsKind::Form,        DocsKind::Descriptions, DocsKind::Divider,
          DocsKind::InputNumber,
          DocsKind::Switch,      DocsKind::Segmented,    DocsKind::Menu,
          DocsKind::Message,     DocsKind::Notification, DocsKind::Modal,
          DocsKind::Select,      DocsKind::Slider,       DocsKind::Spin,
          DocsKind::Pagination,  DocsKind::Tabs,         DocsKind::Carousel,
          DocsKind::ColorPicker, DocsKind::DatePicker,   DocsKind::Image,
          DocsKind::Popover,     DocsKind::Popconfirm,   DocsKind::Tooltip,
          DocsKind::Radio,       DocsKind::Checkbox,     DocsKind::Tag}) {
      if (key == docsGroupKey(candidate)) {
        if (kind) {
          *kind = candidate;
        }
        return true;
      }
    }
    return false;
  }

  const QVector<QWidget*>& anchorsFor(DocsKind kind) const {
    static const QVector<QWidget*> kEmpty;
    if (kind == DocsKind::Button && buttonPage_) {
      return buttonPage_->sectionAnchors();
    }
    if (kind == DocsKind::Alert && alertPage_) {
      return alertPage_->sectionAnchors();
    }
    if (kind == DocsKind::Input && inputPage_) {
      return inputPage_->sectionAnchors();
    }
    if (kind == DocsKind::Form && formPage_) {
      return formPage_->sectionAnchors();
    }
    if (kind == DocsKind::Descriptions && descriptionsPage_) {
      return descriptionsPage_->sectionAnchors();
    }
    if (kind == DocsKind::Divider && dividerPage_) {
      return dividerPage_->sectionAnchors();
    }
    if (kind == DocsKind::InputNumber && inputNumberPage_) {
      return inputNumberPage_->sectionAnchors();
    }
    if (kind == DocsKind::Switch && switchPage_) {
      return switchPage_->sectionAnchors();
    }
    if (kind == DocsKind::Segmented && segmentedPage_) {
      return segmentedPage_->sectionAnchors();
    }
    if (kind == DocsKind::Menu && menuPage_) {
      return menuPage_->sectionAnchors();
    }
    if (kind == DocsKind::Message && messagePage_) {
      return messagePage_->sectionAnchors();
    }
    if (kind == DocsKind::Notification && notificationPage_) {
      return notificationPage_->sectionAnchors();
    }
    if (kind == DocsKind::Modal && modalPage_) {
      return modalPage_->sectionAnchors();
    }
    if (kind == DocsKind::Select && selectPage_) {
      return selectPage_->sectionAnchors();
    }
    if (kind == DocsKind::Slider && sliderPage_) {
      return sliderPage_->sectionAnchors();
    }
    if (kind == DocsKind::Spin && spinPage_) {
      return spinPage_->sectionAnchors();
    }
    if (kind == DocsKind::Pagination && paginationPage_) {
      return paginationPage_->sectionAnchors();
    }
    if (kind == DocsKind::Tabs && tabsPage_) {
      return tabsPage_->sectionAnchors();
    }
    if (kind == DocsKind::Carousel && carouselPage_) {
      return carouselPage_->sectionAnchors();
    }
    if (kind == DocsKind::ColorPicker && colorPickerPage_) {
      return colorPickerPage_->sectionAnchors();
    }
    if (kind == DocsKind::DatePicker && datePickerPage_) {
      return datePickerPage_->sectionAnchors();
    }
    if (kind == DocsKind::Image && imagePage_) {
      return imagePage_->sectionAnchors();
    }
    if (kind == DocsKind::Popover && popoverPage_) {
      return popoverPage_->sectionAnchors();
    }
    if (kind == DocsKind::Popconfirm && popconfirmPage_) {
      return popconfirmPage_->sectionAnchors();
    }
    if (kind == DocsKind::Tooltip && tooltipPage_) {
      return tooltipPage_->sectionAnchors();
    }
    if (kind == DocsKind::Radio && radioPage_) {
      return radioPage_->sectionAnchors();
    }
    if (kind == DocsKind::Checkbox && checkboxPage_) {
      return checkboxPage_->sectionAnchors();
    }
    if (kind == DocsKind::Tag && tagPage_) {
      return tagPage_->sectionAnchors();
    }
    return kEmpty;
  }

  const QStringList& titlesFor(DocsKind kind) const {
    static const QStringList kEmpty;
    if (kind == DocsKind::Button && buttonPage_) {
      return buttonPage_->sectionTitles();
    }
    if (kind == DocsKind::Alert && alertPage_) {
      return alertPage_->sectionTitles();
    }
    if (kind == DocsKind::Input && inputPage_) {
      return inputPage_->sectionTitles();
    }
    if (kind == DocsKind::Form && formPage_) {
      return formPage_->sectionTitles();
    }
    if (kind == DocsKind::Descriptions && descriptionsPage_) {
      return descriptionsPage_->sectionTitles();
    }
    if (kind == DocsKind::Divider && dividerPage_) {
      return dividerPage_->sectionTitles();
    }
    if (kind == DocsKind::InputNumber && inputNumberPage_) {
      return inputNumberPage_->sectionTitles();
    }
    if (kind == DocsKind::Switch && switchPage_) {
      return switchPage_->sectionTitles();
    }
    if (kind == DocsKind::Segmented && segmentedPage_) {
      return segmentedPage_->sectionTitles();
    }
    if (kind == DocsKind::Menu) {
      return menuPage_ ? menuPage_->sectionTitles() : MenuDocsPage::defaultSectionTitles();
    }
    if (kind == DocsKind::Message && messagePage_) {
      return messagePage_->sectionTitles();
    }
    if (kind == DocsKind::Notification && notificationPage_) {
      return notificationPage_->sectionTitles();
    }
    if (kind == DocsKind::Modal && modalPage_) {
      return modalPage_->sectionTitles();
    }
    if (kind == DocsKind::Select && selectPage_) {
      return selectPage_->sectionTitles();
    }
    if (kind == DocsKind::Slider && sliderPage_) {
      return sliderPage_->sectionTitles();
    }
    if (kind == DocsKind::Spin && spinPage_) {
      return spinPage_->sectionTitles();
    }
    if (kind == DocsKind::Pagination && paginationPage_) {
      return paginationPage_->sectionTitles();
    }
    if (kind == DocsKind::Tabs && tabsPage_) {
      return tabsPage_->sectionTitles();
    }
    if (kind == DocsKind::Carousel && carouselPage_) {
      return carouselPage_->sectionTitles();
    }
    if (kind == DocsKind::ColorPicker && colorPickerPage_) {
      return colorPickerPage_->sectionTitles();
    }
    if (kind == DocsKind::DatePicker && datePickerPage_) {
      return datePickerPage_->sectionTitles();
    }
    if (kind == DocsKind::Image && imagePage_) {
      return imagePage_->sectionTitles();
    }
    if (kind == DocsKind::Popover && popoverPage_) {
      return popoverPage_->sectionTitles();
    }
    if (kind == DocsKind::Popconfirm && popconfirmPage_) {
      return popconfirmPage_->sectionTitles();
    }
    if (kind == DocsKind::Tooltip && tooltipPage_) {
      return tooltipPage_->sectionTitles();
    }
    if (kind == DocsKind::Radio && radioPage_) {
      return radioPage_->sectionTitles();
    }
    if (kind == DocsKind::Checkbox && checkboxPage_) {
      return checkboxPage_->sectionTitles();
    }
    if (kind == DocsKind::Tag && tagPage_) {
      return tagPage_->sectionTitles();
    }
    return kEmpty;
  }

  QModelIndex navIndexForKey(const QString& key) const {
    return findMenuIndexByKey(navModel_, key);
  }

  bool hasDocsPage(DocsKind kind) const {
    if (kind == DocsKind::Button) {
      return buttonPage_ != nullptr;
    }
    if (kind == DocsKind::Alert) {
      return alertPage_ != nullptr;
    }
    if (kind == DocsKind::Input) {
      return inputPage_ != nullptr;
    }
    if (kind == DocsKind::Form) {
      return formPage_ != nullptr;
    }
    if (kind == DocsKind::Descriptions) {
      return descriptionsPage_ != nullptr;
    }
    if (kind == DocsKind::Divider) {
      return dividerPage_ != nullptr;
    }
    if (kind == DocsKind::InputNumber) {
      return inputNumberPage_ != nullptr;
    }
    if (kind == DocsKind::Switch) {
      return switchPage_ != nullptr;
    }
    if (kind == DocsKind::Segmented) {
      return segmentedPage_ != nullptr;
    }
    if (kind == DocsKind::Menu) {
      return menuPage_ != nullptr;
    }
    if (kind == DocsKind::Message) {
      return messagePage_ != nullptr;
    }
    if (kind == DocsKind::Notification) {
      return notificationPage_ != nullptr;
    }
    if (kind == DocsKind::Modal) {
      return modalPage_ != nullptr;
    }
    if (kind == DocsKind::Select) {
      return selectPage_ != nullptr;
    }
    if (kind == DocsKind::Slider) {
      return sliderPage_ != nullptr;
    }
    if (kind == DocsKind::Spin) {
      return spinPage_ != nullptr;
    }
    if (kind == DocsKind::Pagination) {
      return paginationPage_ != nullptr;
    }
    if (kind == DocsKind::Tabs) {
      return tabsPage_ != nullptr;
    }
    if (kind == DocsKind::Carousel) {
      return carouselPage_ != nullptr;
    }
    if (kind == DocsKind::ColorPicker) {
      return colorPickerPage_ != nullptr;
    }
    if (kind == DocsKind::DatePicker) {
      return datePickerPage_ != nullptr;
    }
    if (kind == DocsKind::Image) {
      return imagePage_ != nullptr;
    }
    if (kind == DocsKind::Popover) {
      return popoverPage_ != nullptr;
    }
    if (kind == DocsKind::Popconfirm) {
      return popconfirmPage_ != nullptr;
    }
    if (kind == DocsKind::Tooltip) {
      return tooltipPage_ != nullptr;
    }
    if (kind == DocsKind::Radio) {
      return radioPage_ != nullptr;
    }
    if (kind == DocsKind::Checkbox) {
      return checkboxPage_ != nullptr;
    }
    return tagPage_ != nullptr;
  }

  QStringList normalizeNavOpenKeys(const QStringList& keys) const {
    QStringList normalized;
    for (const QString& key : keys) {
      DocsKind kind = DocsKind::Button;
      if (!parseDocsGroupKey(key, &kind) || normalized.contains(key)) {
        continue;
      }
      normalized.append(key);
    }
    return normalized;
  }

  void setNavOpenKeys(const QStringList& keys) {
    const QStringList normalized = normalizeNavOpenKeys(keys);
    navOpenKeys_ = normalized;
    if (!navMenu_ || !navModel_) {
      return;
    }
    syncingNavExpansion_ = true;
    const int rows = navModel_->rowCount();
    for (int row = 0; row < rows; ++row) {
      const QModelIndex index = navModel_->index(row, 0);
      if (!index.isValid() || navModel_->rowCount(index) <= 0) {
        continue;
      }
      const QString key = index.data(AdNavigationMenu::StableIdRole).toString();
      const bool shouldExpand = navOpenKeys_.contains(key);
      if (navMenu_->isExpanded(index) != shouldExpand) {
        navMenu_->setExpanded(index, shouldExpand);
      }
    }
    syncingNavExpansion_ = false;
  }

  void ensureNavGroupOpen(DocsKind kind) {
    const QString key = docsGroupKey(kind);
    if (navOpenKeys_.contains(key)) {
      return;
    }
    QStringList nextOpenKeys = navOpenKeys_;
    nextOpenKeys.append(key);
    setNavOpenKeys(nextOpenKeys);
  }

  bool isNavLeafIndex(const QModelIndex& index) const {
    return index.isValid() && navModel_ && navModel_->rowCount(index) == 0;
  }

  void handleNavLeafTrigger(const QModelIndex& index) {
    if (!isNavLeafIndex(index)) {
      return;
    }
    handleNavClick(index.data(AdNavigationMenu::StableIdRole).toString());
  }

  void handleNavExpandedChanged(const QModelIndex& index, bool expanded) {
    if (syncingNavExpansion_) {
      return;
    }
    const QString key = index.data(AdNavigationMenu::StableIdRole).toString();
    DocsKind kind = DocsKind::Button;
    if (!parseDocsGroupKey(key, &kind)) {
      return;
    }

    QStringList nextOpenKeys = navOpenKeys_;
    if (expanded) {
      if (!nextOpenKeys.contains(key)) {
        nextOpenKeys.append(key);
      }
    } else {
      nextOpenKeys.removeAll(key);
    }
    navOpenKeys_ = normalizeNavOpenKeys(nextOpenKeys);

    if (!expanded || hasDocsPage(kind) || kind == DocsKind::Menu) {
      return;
    }

    QTimer::singleShot(0, this, [this, kind]() {
      if (hasDocsPage(kind)) {
        return;
      }
      ensureDocsPage(kind, false);
      rebuildNavMenuItems();
    });
  }

  void syncNavMenuSelection(const QString& requestedKey, DocsKind fallbackKind) {
    if (!navMenu_ || !navModel_) {
      return;
    }

    QModelIndex target = navIndexForKey(requestedKey);
    if (!target.isValid()) {
      target = navIndexForKey(docsRootKey(fallbackKind));
    }
    if (target.isValid() && navMenu_->currentIndex() != target) {
      navMenu_->setCurrentIndex(target);
      if (QItemSelectionModel* selectionModel = navMenu_->selectionModel()) {
        selectionModel->select(target,
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      }
    }
  }

  void rebuildNavMenuItems() {
    if (!navMenu_ || !navModel_) {
      return;
    }

    const QString previousKey =
        navMenu_->currentIndex().data(AdNavigationMenu::StableIdRole).toString();
    auto* replacementModel = new QStandardItemModel(this);

    auto buildSectionItems = [this](DocsKind kind) {
      QList<QStandardItem*> sectionItems;
      sectionItems.append(makeMenuModelItem(docsRootKey(kind), QStringLiteral("Overview"),
                                            outlined_icons::Right()));
      const QStringList& titles = titlesFor(kind);
      for (int i = 0; i < titles.size(); ++i) {
        sectionItems.append(
            makeMenuModelItem(sectionKey(kind, i), titles.at(i), outlined_icons::Right()));
      }
      return sectionItems;
    };

    auto appendRoot = [this, replacementModel, &buildSectionItems](
                          DocsKind kind, const QString& label, const adqt::icons::IconRef& icon) {
      auto* root = makeMenuModelItem(docsGroupKey(kind), label, icon);
      const QList<QStandardItem*> children = buildSectionItems(kind);
      for (QStandardItem* child : children) {
        root->appendRow(child);
      }
      replacementModel->appendRow(root);
    };

    appendRoot(DocsKind::Button, QStringLiteral("Button"), outlined_icons::Appstore());
    appendRoot(DocsKind::Alert, QStringLiteral("Alert"), outlined_icons::Alert());
    appendRoot(DocsKind::Input, QStringLiteral("Input"), outlined_icons::Edit());
    appendRoot(DocsKind::Form, QStringLiteral("Form"), outlined_icons::Form());
    appendRoot(DocsKind::Descriptions, QStringLiteral("Descriptions"), outlined_icons::Profile());
    appendRoot(DocsKind::Divider, QStringLiteral("Divider"), outlined_icons::Minus());
    appendRoot(DocsKind::InputNumber, QStringLiteral("InputNumber"), outlined_icons::Number());
    appendRoot(DocsKind::Switch, QStringLiteral("Switch"), outlined_icons::Switcher());
    appendRoot(DocsKind::Segmented, QStringLiteral("Segmented"), outlined_icons::Appstore());
    appendRoot(DocsKind::Menu, QStringLiteral("Menu"), outlined_icons::Menu());
    appendRoot(DocsKind::Message, QStringLiteral("Message"), outlined_icons::Message());
    appendRoot(DocsKind::Notification, QStringLiteral("Notification"),
               outlined_icons::Notification());
    appendRoot(DocsKind::Modal, QStringLiteral("Modal"), outlined_icons::Appstore());
    appendRoot(DocsKind::Select, QStringLiteral("Select"), outlined_icons::Select());
    appendRoot(DocsKind::Slider, QStringLiteral("Slider"), outlined_icons::Sliders());
    appendRoot(DocsKind::Spin, QStringLiteral("Spin"), outlined_icons::Loading());
    appendRoot(DocsKind::Pagination, QStringLiteral("Pagination"), outlined_icons::More());
    appendRoot(DocsKind::Tabs, QStringLiteral("Tabs"), outlined_icons::FolderOpen());
    appendRoot(DocsKind::Carousel, QStringLiteral("Carousel"), outlined_icons::Picture());
    appendRoot(DocsKind::ColorPicker, QStringLiteral("ColorPicker"), outlined_icons::BgColors());
    appendRoot(DocsKind::DatePicker, QStringLiteral("DatePicker"), outlined_icons::Calendar());
    appendRoot(DocsKind::Image, QStringLiteral("Image"), outlined_icons::Picture());
    appendRoot(DocsKind::Popover, QStringLiteral("Popover"), outlined_icons::Message());
    appendRoot(DocsKind::Popconfirm, QStringLiteral("Popconfirm"),
               outlined_icons::QuestionCircle());
    appendRoot(DocsKind::Tooltip, QStringLiteral("Tooltip"), outlined_icons::InfoCircle());
    appendRoot(DocsKind::Radio, QStringLiteral("Radio"), outlined_icons::Appstore());
    appendRoot(DocsKind::Checkbox, QStringLiteral("Checkbox"), outlined_icons::CheckSquare());
    appendRoot(DocsKind::Tag, QStringLiteral("Tag"), outlined_icons::Tags());

    QStandardItemModel* previousModel = navModel_;
    navModel_ = replacementModel;
    navMenu_->setModel(navModel_);
    if (previousModel && previousModel != navModel_) {
      previousModel->deleteLater();
    }

    setNavOpenKeys(navOpenKeys_.isEmpty() ? QStringList{docsGroupKey(currentKind_)} : navOpenKeys_);

    const QString fallbackKey =
        !previousKey.isEmpty() ? previousKey
                               : (titlesFor(currentKind_).isEmpty() ? docsRootKey(currentKind_)
                                                                    : sectionKey(currentKind_, 0));
    syncNavMenuSelection(fallbackKey, currentKind_);
  }

  QWidget* ensureDocsPage(DocsKind kind, bool rebuildNavWhenCreated = true) {
    if (!docsStack_) {
      return nullptr;
    }

    bool created = false;
    QWidget* target = nullptr;
    if (kind == DocsKind::Button) {
      if (!buttonPage_) {
        buttonPage_ = new ButtonDocsPage();
        docsStack_->addWidget(buttonPage_);
        created = true;
      }
      target = buttonPage_;
    } else if (kind == DocsKind::Alert) {
      if (!alertPage_) {
        alertPage_ = new AlertDocsPage();
        docsStack_->addWidget(alertPage_);
        created = true;
      }
      target = alertPage_;
    } else if (kind == DocsKind::Input) {
      if (!inputPage_) {
        inputPage_ = new InputDocsPage();
        docsStack_->addWidget(inputPage_);
        created = true;
      }
      target = inputPage_;
    } else if (kind == DocsKind::Form) {
      if (!formPage_) {
        formPage_ = new FormDocsPage();
        docsStack_->addWidget(formPage_);
        created = true;
      }
      target = formPage_;
    } else if (kind == DocsKind::Descriptions) {
      if (!descriptionsPage_) {
        descriptionsPage_ = new DescriptionsDocsPage();
        docsStack_->addWidget(descriptionsPage_);
        created = true;
      }
      target = descriptionsPage_;
    } else if (kind == DocsKind::Divider) {
      if (!dividerPage_) {
        dividerPage_ = new DividerDocsPage();
        docsStack_->addWidget(dividerPage_);
        created = true;
      }
      target = dividerPage_;
    } else if (kind == DocsKind::InputNumber) {
      if (!inputNumberPage_) {
        inputNumberPage_ = new InputNumberDocsPage();
        docsStack_->addWidget(inputNumberPage_);
        created = true;
      }
      target = inputNumberPage_;
    } else if (kind == DocsKind::Switch) {
      if (!switchPage_) {
        switchPage_ = new SwitchDocsPage();
        docsStack_->addWidget(switchPage_);
        created = true;
      }
      target = switchPage_;
    } else if (kind == DocsKind::Segmented) {
      if (!segmentedPage_) {
        segmentedPage_ = new SegmentedDocsPage();
        docsStack_->addWidget(segmentedPage_);
        created = true;
      }
      target = segmentedPage_;
    } else if (kind == DocsKind::Menu) {
      if (!menuPage_) {
        menuPage_ = new MenuDocsPage();
        docsStack_->addWidget(menuPage_);
        created = true;
      }
      target = menuPage_;
    } else if (kind == DocsKind::Message) {
      if (!messagePage_) {
        messagePage_ = new MessageDocsPage();
        docsStack_->addWidget(messagePage_);
        created = true;
      }
      target = messagePage_;
    } else if (kind == DocsKind::Notification) {
      if (!notificationPage_) {
        notificationPage_ = new NotificationDocsPage();
        docsStack_->addWidget(notificationPage_);
        created = true;
      }
      target = notificationPage_;
    } else if (kind == DocsKind::Modal) {
      if (!modalPage_) {
        modalPage_ = new ModalDocsPage();
        docsStack_->addWidget(modalPage_);
        created = true;
      }
      target = modalPage_;
    } else if (kind == DocsKind::Select) {
      if (!selectPage_) {
        selectPage_ = new SelectDocsPage();
        docsStack_->addWidget(selectPage_);
        created = true;
      }
      target = selectPage_;
    } else if (kind == DocsKind::Slider) {
      if (!sliderPage_) {
        sliderPage_ = new SliderDocsPage();
        docsStack_->addWidget(sliderPage_);
        created = true;
      }
      target = sliderPage_;
    } else if (kind == DocsKind::Spin) {
      if (!spinPage_) {
        spinPage_ = new SpinDocsPage();
        docsStack_->addWidget(spinPage_);
        created = true;
      }
      target = spinPage_;
    } else if (kind == DocsKind::Pagination) {
      if (!paginationPage_) {
        paginationPage_ = new PaginationDocsPage();
        docsStack_->addWidget(paginationPage_);
        created = true;
      }
      target = paginationPage_;
    } else if (kind == DocsKind::Tabs) {
      if (!tabsPage_) {
        tabsPage_ = new TabsDocsPage();
        docsStack_->addWidget(tabsPage_);
        created = true;
      }
      target = tabsPage_;
    } else if (kind == DocsKind::Carousel) {
      if (!carouselPage_) {
        carouselPage_ = new CarouselDocsPage();
        docsStack_->addWidget(carouselPage_);
        created = true;
      }
      target = carouselPage_;
    } else if (kind == DocsKind::ColorPicker) {
      if (!colorPickerPage_) {
        colorPickerPage_ = new ColorPickerDocsPage();
        docsStack_->addWidget(colorPickerPage_);
        created = true;
      }
      target = colorPickerPage_;
    } else if (kind == DocsKind::DatePicker) {
      if (!datePickerPage_) {
        datePickerPage_ = new DatePickerDocsPage();
        docsStack_->addWidget(datePickerPage_);
        created = true;
      }
      target = datePickerPage_;
    } else if (kind == DocsKind::Image) {
      if (!imagePage_) {
        imagePage_ = new ImageDocsPage();
        docsStack_->addWidget(imagePage_);
        created = true;
      }
      target = imagePage_;
    } else if (kind == DocsKind::Popover) {
      if (!popoverPage_) {
        popoverPage_ = new PopoverDocsPage();
        docsStack_->addWidget(popoverPage_);
        created = true;
      }
      target = popoverPage_;
    } else if (kind == DocsKind::Popconfirm) {
      if (!popconfirmPage_) {
        popconfirmPage_ = new PopconfirmDocsPage();
        docsStack_->addWidget(popconfirmPage_);
        created = true;
      }
      target = popconfirmPage_;
    } else if (kind == DocsKind::Tooltip) {
      if (!tooltipPage_) {
        tooltipPage_ = new TooltipDocsPage();
        docsStack_->addWidget(tooltipPage_);
        created = true;
      }
      target = tooltipPage_;
    } else if (kind == DocsKind::Radio) {
      if (!radioPage_) {
        radioPage_ = new RadioDocsPage();
        docsStack_->addWidget(radioPage_);
        created = true;
      }
      target = radioPage_;
    } else if (kind == DocsKind::Checkbox) {
      if (!checkboxPage_) {
        checkboxPage_ = new CheckboxDocsPage();
        docsStack_->addWidget(checkboxPage_);
        created = true;
      }
      target = checkboxPage_;
    } else {
      if (!tagPage_) {
        tagPage_ = new TagDocsPage();
        docsStack_->addWidget(tagPage_);
        created = true;
      }
      target = tagPage_;
    }

    if (created && rebuildNavWhenCreated) {
      rebuildNavMenuItems();
    }
    return target;
  }

  void applyThemeFromControls() {
    const ThemePresetSelection selection = configForIndex(modeBox_ ? modeBox_->currentIndex() : 0);
    ThemeManager::instance().setColorScheme(selection.scheme);
    ThemeManager::instance().setDensity(selection.density);

    if (navMenu_) {
      navMenu_->setColorScheme(AdNavigationMenu::ColorScheme::Inherit);
    }
  }

  void switchDocs(DocsKind kind, bool preserveScroll) {
    currentKind_ = kind;
    if (docsStack_) {
      if (QWidget* target = ensureDocsPage(kind)) {
        docsStack_->setCurrentWidget(target);
      }
    }
    if (!preserveScroll && scroll_) {
      scroll_->verticalScrollBar()->setValue(0);
    }
    if (scroll_) {
      syncNavToScroll(scroll_->verticalScrollBar()->value());
    }
  }

  void handleNavClick(const QString& key) {
    DocsKind kind = DocsKind::Button;
    int row = -1;
    if (parseSectionKey(key, &kind, &row)) {
      ensureNavGroupOpen(kind);
      switchDocs(kind, true);
      scrollToSection(kind, row);
      return;
    }

    if (!parseDocsRootKey(key, &kind)) {
      return;
    }

    ensureNavGroupOpen(kind);
    switchDocs(kind, false);
    if (scroll_) {
      scroll_->verticalScrollBar()->setValue(0);
    }
    syncNavMenuSelection(docsRootKey(kind), kind);
  }

  void scrollToSection(DocsKind kind, int row) {
    const QVector<QWidget*>& anchors = anchorsFor(kind);
    if (!scroll_ || row < 0 || row >= anchors.size()) {
      return;
    }

    QWidget* target = anchors.at(row);
    if (!target) {
      return;
    }

    scroll_->ensureWidgetVisible(target, 24, 24);
    syncNavMenuSelection(sectionKey(kind, row), kind);
  }

  void syncNavToScroll(int scrollValue) {
    const QVector<QWidget*>& anchors = anchorsFor(currentKind_);
    if (anchors.isEmpty() || !navMenu_) {
      return;
    }

    const int probeY = scrollValue + 28;
    int best = 0;
    for (int i = 0; i < anchors.size(); ++i) {
      QWidget* anchor = anchors.at(i);
      if (!anchor) {
        continue;
      }
      if (anchor->y() <= probeY) {
        best = i;
      } else {
        break;
      }
    }

    syncNavMenuSelection(sectionKey(currentKind_, best), currentKind_);
  }

  QComboBox* modeBox_ = nullptr;
  AdNavigationMenu* navMenu_ = nullptr;
  QStandardItemModel* navModel_ = nullptr;
  AdScrollArea* navScroll_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QStackedWidget* docsStack_ = nullptr;
  ButtonDocsPage* buttonPage_ = nullptr;
  AlertDocsPage* alertPage_ = nullptr;
  InputDocsPage* inputPage_ = nullptr;
  FormDocsPage* formPage_ = nullptr;
  DescriptionsDocsPage* descriptionsPage_ = nullptr;
  DividerDocsPage* dividerPage_ = nullptr;
  InputNumberDocsPage* inputNumberPage_ = nullptr;
  SwitchDocsPage* switchPage_ = nullptr;
  SegmentedDocsPage* segmentedPage_ = nullptr;
  MenuDocsPage* menuPage_ = nullptr;
  MessageDocsPage* messagePage_ = nullptr;
  NotificationDocsPage* notificationPage_ = nullptr;
  ModalDocsPage* modalPage_ = nullptr;
  SelectDocsPage* selectPage_ = nullptr;
  SliderDocsPage* sliderPage_ = nullptr;
  SpinDocsPage* spinPage_ = nullptr;
  PaginationDocsPage* paginationPage_ = nullptr;
  TabsDocsPage* tabsPage_ = nullptr;
  CarouselDocsPage* carouselPage_ = nullptr;
  ColorPickerDocsPage* colorPickerPage_ = nullptr;
  DatePickerDocsPage* datePickerPage_ = nullptr;
  ImageDocsPage* imagePage_ = nullptr;
  PopoverDocsPage* popoverPage_ = nullptr;
  PopconfirmDocsPage* popconfirmPage_ = nullptr;
  TooltipDocsPage* tooltipPage_ = nullptr;
  RadioDocsPage* radioPage_ = nullptr;
  CheckboxDocsPage* checkboxPage_ = nullptr;
  TagDocsPage* tagPage_ = nullptr;
  QStringList navOpenKeys_;
  bool syncingNavExpansion_ = false;
  DocsKind currentKind_ = DocsKind::Button;
};

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);

  if (QStyleFactory::keys().contains("Fusion", Qt::CaseInsensitive)) {
    app.setStyle(QStyleFactory::create("Fusion"));
  }

  ThemeManager::instance().applyTo(app);
  demo::installIconThemeResolver();

  DemoWindow window;
  window.show();

  return app.exec();
}
