#include "popconfirm.h"

#include "theme/theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QPalette>
#include <QPixmap>
#include <QShortcut>
#include <QSet>
#include <QStringList>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

#include "antd_icons.h"

namespace adqt::widgets {

namespace {

namespace filled_icons = adqt::icons::antd::filled;
namespace outlined_icons = adqt::icons::antd::outlined;

using StandardButton = AdPopconfirm::StandardButton;
using StandardButtons = AdPopconfirm::StandardButtons;
using Icon = AdPopconfirm::Icon;

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

void applySemanticSlot(const AdPopconfirm::SemanticSlotStyle& slot, QColor* textColor,
                       QColor* backgroundColor, QColor* borderColor) {
  if (textColor && slot.textColor.has_value()) {
    *textColor = slot.textColor.value();
  }
  if (backgroundColor && slot.backgroundColor.has_value()) {
    *backgroundColor = slot.backgroundColor.value();
  }
  if (borderColor && slot.borderColor.has_value()) {
    *borderColor = slot.borderColor.value();
  }
}

template <typename Callback>
void traverseObjectTree(QObject* root, Callback&& callback) {
  if (!root) {
    return;
  }
  callback(root);
  const QObjectList children = root->children();
  for (QObject* child : children) {
    traverseObjectTree(child, callback);
  }
}

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

bool widgetInTree(const QWidget* candidate, const QWidget* root) {
  if (!candidate || !root) {
    return false;
  }
  return candidate == root || root->isAncestorOf(const_cast<QWidget*>(candidate));
}

bool isVisualRefreshEvent(QEvent::Type type) {
  return type == QEvent::EnabledChange || type == QEvent::FontChange ||
         type == QEvent::ApplicationFontChange || type == QEvent::PaletteChange ||
         type == QEvent::ApplicationPaletteChange || type == QEvent::StyleChange ||
         type == QEvent::LayoutDirectionChange;
}

QString firstNonEmpty(std::initializer_list<QString> values) {
  for (const QString& value : values) {
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty()) {
      return trimmed;
    }
  }
  return QString();
}

QString plainTextForAccessibility(const QString& value) {
  if (value.trimmed().isEmpty()) {
    return QString();
  }
  if (!Qt::mightBeRichText(value)) {
    return value.trimmed();
  }

  QTextDocument document;
  document.setDocumentMargin(0.0);
  document.setHtml(value);
  return document.toPlainText().trimmed();
}

int measureIntrinsicTextWidth(const QString& value, const QFont& font, Qt::TextFormat format) {
  if (value.isEmpty()) {
    return 0;
  }

  const bool richText =
      format == Qt::RichText || (format != Qt::PlainText && Qt::mightBeRichText(value));
  if (richText) {
    QTextDocument document;
    document.setDocumentMargin(0.0);
    document.setDefaultFont(font);
    document.setHtml(value);
    return std::max(0, static_cast<int>(std::ceil(document.idealWidth())));
  }

  const QFontMetricsF metrics(font);
  int maxWidth = 0;
  const QStringList lines = value.split(QChar::LineFeed);
  for (QString line : lines) {
    line.remove(QChar::CarriageReturn);
    const qreal advance = metrics.horizontalAdvance(line);
    const qreal boundsWidth = metrics.boundingRect(line).width();
    maxWidth = std::max(maxWidth, static_cast<int>(std::ceil(std::max(advance, boundsWidth))));
  }
  return maxWidth;
}

int labelHorizontalPadding(const QLabel* label) {
  if (!label) {
    return 0;
  }

  const QMargins margins = label->contentsMargins();
  return margins.left() + margins.right() + (std::max(0, label->margin()) * 2);
}

int labelVerticalPadding(const QLabel* label) {
  if (!label) {
    return 0;
  }

  const QMargins margins = label->contentsMargins();
  return margins.top() + margins.bottom() + (std::max(0, label->margin()) * 2);
}

int measureIntrinsicTextHeight(const QString& value, const QFont& font, Qt::TextFormat format,
                               int width) {
  if (value.isEmpty()) {
    return 0;
  }

  const int constrainedWidth = std::max(1, width);
  const bool richText =
      format == Qt::RichText || (format != Qt::PlainText && Qt::mightBeRichText(value));
  if (richText) {
    QTextDocument document;
    document.setDocumentMargin(0.0);
    document.setDefaultFont(font);
    document.setHtml(value);
    document.setTextWidth(constrainedWidth);
    return std::max(0, static_cast<int>(std::ceil(document.size().height())));
  }

  const QFontMetricsF metrics(font);
  const QRectF bounds = metrics.boundingRect(QRectF(0.0, 0.0, constrainedWidth, 1000000.0),
                                             Qt::TextWordWrap | Qt::TextExpandTabs, value);
  return std::max(0, static_cast<int>(std::ceil(bounds.height())));
}

class IntrinsicWidthLabel final : public QLabel {
 public:
  using QLabel::QLabel;

  QSize sizeHint() const override {
    QSize hint = QLabel::sizeHint();
    const int intrinsicTextWidth = measureIntrinsicTextWidth(text(), font(), textFormat());
    if (intrinsicTextWidth <= 0) {
      return hint;
    }

    const int preferredWidth = intrinsicTextWidth + labelHorizontalPadding(this);
    if (!wordWrap()) {
      hint.setWidth(std::max(hint.width(), preferredWidth));
      return hint;
    }

    return QSize(std::max(hint.width(), preferredWidth), hint.height());
  }
};

StandardButtons allSupportedStandardButtons() {
  return StandardButton::Ok | StandardButton::Save | StandardButton::SaveAll |
         StandardButton::Open | StandardButton::Yes | StandardButton::YesToAll |
         StandardButton::No | StandardButton::NoToAll | StandardButton::Abort |
         StandardButton::Retry | StandardButton::Ignore | StandardButton::Close |
         StandardButton::Cancel | StandardButton::Discard | StandardButton::Help |
         StandardButton::Apply | StandardButton::Reset | StandardButton::RestoreDefaults;
}

bool isSupportedStandardButton(StandardButton button) {
  return allSupportedStandardButtons().testFlag(button);
}

QDialogButtonBox::ButtonRole roleForStandardButton(StandardButton button) {
  switch (button) {
    case StandardButton::Ok:
    case StandardButton::Save:
    case StandardButton::SaveAll:
    case StandardButton::Open:
    case StandardButton::Retry:
    case StandardButton::Ignore:
      return QDialogButtonBox::AcceptRole;
    case StandardButton::Yes:
    case StandardButton::YesToAll:
      return QDialogButtonBox::YesRole;
    case StandardButton::No:
    case StandardButton::NoToAll:
      return QDialogButtonBox::NoRole;
    case StandardButton::Abort:
    case StandardButton::Close:
    case StandardButton::Cancel:
      return QDialogButtonBox::RejectRole;
    case StandardButton::Discard:
      return QDialogButtonBox::DestructiveRole;
    case StandardButton::Help:
      return QDialogButtonBox::HelpRole;
    case StandardButton::Apply:
      return QDialogButtonBox::ApplyRole;
    case StandardButton::Reset:
    case StandardButton::RestoreDefaults:
      return QDialogButtonBox::ResetRole;
    case StandardButton::NoButton:
      break;
  }
  return QDialogButtonBox::InvalidRole;
}

bool isAcceptedRole(QDialogButtonBox::ButtonRole role) {
  return role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole;
}

bool isRejectedRole(QDialogButtonBox::ButtonRole role) {
  return role == QDialogButtonBox::RejectRole || role == QDialogButtonBox::NoRole;
}

bool isHelpRole(QDialogButtonBox::ButtonRole role) { return role == QDialogButtonBox::HelpRole; }

bool isDefaultRole(QDialogButtonBox::ButtonRole role) {
  return role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole;
}

int buttonKey(StandardButton button) { return static_cast<int>(button); }

QList<StandardButton> fallbackButtonOrder() {
  return {StandardButton::Ok,     StandardButton::Save,    StandardButton::SaveAll,
          StandardButton::Open,   StandardButton::Yes,     StandardButton::YesToAll,
          StandardButton::No,     StandardButton::NoToAll, StandardButton::Abort,
          StandardButton::Retry,  StandardButton::Ignore,  StandardButton::Close,
          StandardButton::Cancel, StandardButton::Discard, StandardButton::Help,
          StandardButton::Apply,  StandardButton::Reset,   StandardButton::RestoreDefaults};
}

void collectButtonsFromLayout(QLayout* layout, QList<QAbstractButton*>* buttons) {
  if (!layout || !buttons) {
    return;
  }
  for (int index = 0; index < layout->count(); ++index) {
    QLayoutItem* item = layout->itemAt(index);
    if (!item) {
      continue;
    }
    if (QLayout* childLayout = item->layout()) {
      collectButtonsFromLayout(childLayout, buttons);
      continue;
    }
    if (QWidget* widget = item->widget()) {
      if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        buttons->append(button);
      }
    }
  }
}

QList<StandardButton> platformOrderedButtons(StandardButtons buttons,
                                             Qt::LayoutDirection direction) {
  QList<StandardButton> orderedButtons;
  if (buttons == StandardButton::NoButton) {
    return orderedButtons;
  }

  QDialogButtonBox probe;
  probe.setLayoutDirection(direction);
  probe.setStandardButtons(buttons);
  probe.ensurePolished();
  if (QLayout* layout = probe.layout()) {
    layout->activate();
  }

  QList<QAbstractButton*> orderedWidgets;
  collectButtonsFromLayout(probe.layout(), &orderedWidgets);

  QSet<int> seen;
  for (QAbstractButton* button : orderedWidgets) {
    const StandardButton standardButton = probe.standardButton(button);
    if (!isSupportedStandardButton(standardButton) || !buttons.testFlag(standardButton) ||
        seen.contains(buttonKey(standardButton))) {
      continue;
    }
    seen.insert(buttonKey(standardButton));
    orderedButtons.append(standardButton);
  }

  const QList<StandardButton> fallbackOrder = fallbackButtonOrder();
  for (StandardButton button : fallbackOrder) {
    if (buttons.testFlag(button) && !seen.contains(buttonKey(button))) {
      seen.insert(buttonKey(button));
      orderedButtons.append(button);
    }
  }

  return orderedButtons;
}

QString defaultTextForStandardButton(StandardButton button) {
  switch (button) {
    case StandardButton::Ok:
      return AdPopconfirm::tr("OK");
    case StandardButton::Save:
      return AdPopconfirm::tr("Save");
    case StandardButton::SaveAll:
      return AdPopconfirm::tr("Save All");
    case StandardButton::Open:
      return AdPopconfirm::tr("Open");
    case StandardButton::Yes:
      return AdPopconfirm::tr("Yes");
    case StandardButton::YesToAll:
      return AdPopconfirm::tr("Yes to All");
    case StandardButton::No:
      return AdPopconfirm::tr("No");
    case StandardButton::NoToAll:
      return AdPopconfirm::tr("No to All");
    case StandardButton::Abort:
      return AdPopconfirm::tr("Abort");
    case StandardButton::Retry:
      return AdPopconfirm::tr("Retry");
    case StandardButton::Ignore:
      return AdPopconfirm::tr("Ignore");
    case StandardButton::Close:
      return AdPopconfirm::tr("Close");
    case StandardButton::Cancel:
      return AdPopconfirm::tr("Cancel");
    case StandardButton::Discard:
      return AdPopconfirm::tr("Discard");
    case StandardButton::Help:
      return AdPopconfirm::tr("Help");
    case StandardButton::Apply:
      return AdPopconfirm::tr("Apply");
    case StandardButton::Reset:
      return AdPopconfirm::tr("Reset");
    case StandardButton::RestoreDefaults:
      return AdPopconfirm::tr("Restore Defaults");
    case StandardButton::NoButton:
      break;
  }
  return QString();
}

AdButton::ButtonStyle defaultStyleForStandardButton(StandardButton button) {
  const QDialogButtonBox::ButtonRole role = roleForStandardButton(button);
  if (role == QDialogButtonBox::DestructiveRole || isAcceptedRole(role)) {
    return AdButton::ButtonStyle::Solid;
  }
  return AdButton::ButtonStyle::Outline;
}

AdButton::AccentRole defaultAccentRoleForStandardButton(StandardButton button) {
  const QDialogButtonBox::ButtonRole role = roleForStandardButton(button);
  if (role == QDialogButtonBox::DestructiveRole) {
    return AdButton::AccentRole::Danger;
  }
  if (isAcceptedRole(role)) {
    return AdButton::AccentRole::Primary;
  }
  return AdButton::AccentRole::Neutral;
}

class PopconfirmPanel final : public QWidget {
 public:
  explicit PopconfirmPanel(QWidget* parent = nullptr) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    rootLayout_ = new QVBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(0);

    messageHost_ = new QWidget(this);
    messageHost_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    messageLayout_ = new QHBoxLayout(messageHost_);
    messageLayout_->setContentsMargins(0, 0, 0, 0);
    messageLayout_->setSpacing(8);

    iconLabel_ = new QLabel(messageHost_);
    iconLabel_->setAlignment(Qt::AlignLeading | Qt::AlignTop);

    textHost_ = new QWidget(messageHost_);
    textHost_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    textLayout_ = new QVBoxLayout(textHost_);
    textLayout_->setContentsMargins(0, 0, 0, 0);
    textLayout_->setSpacing(0);

    titleLabel_ = new IntrinsicWidthLabel(textHost_);
    titleLabel_->setWordWrap(true);
    titleLabel_->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    titleLabel_->setTextFormat(Qt::AutoText);
    titleLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    descriptionLabel_ = new IntrinsicWidthLabel(textHost_);
    descriptionLabel_->setWordWrap(true);
    descriptionLabel_->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    descriptionLabel_->setTextFormat(Qt::AutoText);
    descriptionLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    textLayout_->addWidget(titleLabel_);
    textLayout_->addWidget(descriptionLabel_);

    messageLayout_->addWidget(iconLabel_, 0, Qt::AlignTop);
    messageLayout_->addWidget(textHost_, 1, Qt::AlignTop);

    buttonBox_ = new QDialogButtonBox(Qt::Horizontal, this);
    buttonBox_->setCenterButtons(false);
    buttonBox_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    if (QLayout* buttonLayout = buttonBox_->layout()) {
      buttonLayout->setContentsMargins(0, 0, 0, 0);
      buttonLayout->setSpacing(8);
    }

    rootLayout_->addWidget(messageHost_);
    rootLayout_->addWidget(buttonBox_);

    escapeShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeShortcut_->setContext(Qt::WidgetWithChildrenShortcut);
  }

  QSize sizeHint() const override { return constrainedHint(QWidget::sizeHint()); }

  QSize minimumSizeHint() const override { return constrainedHint(QWidget::minimumSizeHint()); }

  QVBoxLayout* rootLayout() const { return rootLayout_; }
  QWidget* messageHost() const { return messageHost_; }
  QHBoxLayout* messageLayout() const { return messageLayout_; }
  QWidget* textHost() const { return textHost_; }
  QVBoxLayout* textLayout() const { return textLayout_; }
  QLabel* iconLabel() const { return iconLabel_; }
  QLabel* titleLabel() const { return titleLabel_; }
  QLabel* descriptionLabel() const { return descriptionLabel_; }
  QDialogButtonBox* buttonBox() const { return buttonBox_; }
  QShortcut* escapeShortcut() const { return escapeShortcut_; }

  void clearButtons() {
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
      if (!it.value()) {
        continue;
      }
      buttonBox_->removeButton(it.value());
      delete it.value().data();
    }
    buttons_.clear();
  }

  AdButton* addButton(StandardButton buttonType, QDialogButtonBox::ButtonRole role) {
    auto* button = new AdButton(buttonBox_);
    button->setSizeClass(AdButton::SizeClass::Small);
    button->setAutoDefault(false);
    button->setDefault(false);
    buttonBox_->addButton(button, role);
    buttons_.insert(buttonKey(buttonType), button);
    return button;
  }

  AdButton* button(StandardButton buttonType) const {
    return buttons_.value(buttonKey(buttonType), nullptr);
  }

  void setButtonOrder(const QList<StandardButton>& orderedButtons) {
    QLayout* buttonLayout = buttonBox_->layout();
    if (!buttonLayout) {
      return;
    }

    for (StandardButton buttonType : orderedButtons) {
      if (AdButton* control = button(buttonType)) {
        buttonLayout->removeWidget(control);
      }
    }
    for (StandardButton buttonType : orderedButtons) {
      if (AdButton* control = button(buttonType)) {
        buttonLayout->addWidget(control);
      }
    }
  }

  StandardButton standardButtonFor(const QAbstractButton* button) const {
    if (!button) {
      return StandardButton::NoButton;
    }
    for (auto it = buttons_.cbegin(); it != buttons_.cend(); ++it) {
      if (it.value() == button) {
        return static_cast<StandardButton>(it.key());
      }
    }
    return StandardButton::NoButton;
  }

 private:
  QSize constrainedHint(QSize hint) const {
    if (minimumWidth() > 0) {
      hint.setWidth(std::max(hint.width(), minimumWidth()));
    }
    if (maximumWidth() < QWIDGETSIZE_MAX) {
      hint.setWidth(std::min(hint.width(), maximumWidth()));
    }
    if (minimumHeight() > 0) {
      hint.setHeight(std::max(hint.height(), minimumHeight()));
    }
    if (maximumHeight() < QWIDGETSIZE_MAX) {
      hint.setHeight(std::min(hint.height(), maximumHeight()));
    }
    return hint;
  }

  QVBoxLayout* rootLayout_ = nullptr;
  QWidget* messageHost_ = nullptr;
  QHBoxLayout* messageLayout_ = nullptr;
  QWidget* textHost_ = nullptr;
  QVBoxLayout* textLayout_ = nullptr;
  QLabel* iconLabel_ = nullptr;
  QLabel* titleLabel_ = nullptr;
  QLabel* descriptionLabel_ = nullptr;
  QDialogButtonBox* buttonBox_ = nullptr;
  QShortcut* escapeShortcut_ = nullptr;
  QHash<int, QPointer<AdButton>> buttons_;
};

}  // namespace

class AdPopconfirmPrivate {
 public:
  struct ButtonConfig {
    std::optional<QString> text;
    std::optional<AdButton::AccentRole> accentRole;
    std::optional<AdButton::ButtonStyle> style;
    bool busy = false;
  };

  struct DerivedVisualStyle {
    int titleMinWidth = 177;
    int popupMaximumWidth = 320;
    int zIndexPopup = 1060;
    int messageGap = 8;
    int messageBottom = 8;
    int descriptionGap = 4;
    int buttonGap = 8;
    int iconSize = 14;
    QFont titleFont;
    QFont titleOnlyFont;
    QFont descriptionFont;
    QColor titleColor = QColor("#141414");
    QColor descriptionColor = QColor("#141414");
    QColor iconColor = QColor("#faad14");
    QColor popoverBackgroundColor;
    QColor popoverBorderColor;
  };

  explicit AdPopconfirmPrivate(AdPopconfirm* qptr) : q(qptr) {}

  ~AdPopconfirmPrivate() {
    clearSourceObservation();
    clearObservedObjects();

    if (popover && popover->contentWidget() == panel) {
      QWidget* detached = popover->takeContentWidget();
      if (detached) {
        detached->hide();
        delete detached;
      }
    } else if (panel) {
      delete panel.data();
    }
  }

  Qt::LayoutDirection effectiveLayoutDirection() const {
    if (sourceWidget) {
      return sourceWidget->layoutDirection();
    }
    if (QWidget* parentWidget = qobject_cast<QWidget*>(q->parent())) {
      return parentWidget->layoutDirection();
    }
    return qApp ? qApp->layoutDirection() : Qt::LeftToRight;
  }

  QWidget* styleOwnerWidget() const {
    if (sourceWidget) {
      return sourceWidget;
    }
    return qobject_cast<QWidget*>(q->parent());
  }

  ButtonConfig buttonConfig(StandardButton buttonType) const {
    if (!isSupportedStandardButton(buttonType)) {
      return {};
    }
    return buttonConfigs.value(buttonKey(buttonType));
  }

  ButtonConfig* mutableButtonConfig(StandardButton buttonType) {
    if (!isSupportedStandardButton(buttonType)) {
      return nullptr;
    }
    return &buttonConfigs[buttonKey(buttonType)];
  }

  QString resolvedButtonText(StandardButton buttonType) const {
    if (!isSupportedStandardButton(buttonType)) {
      return QString();
    }
    const ButtonConfig config = buttonConfig(buttonType);
    if (config.text.has_value()) {
      return config.text.value();
    }
    return defaultTextForStandardButton(buttonType);
  }

  QList<StandardButton> orderedVisibleButtons() const {
    const StandardButtons visibleButtons = standardButtons & allSupportedStandardButtons();
    return platformOrderedButtons(visibleButtons, effectiveLayoutDirection());
  }

  StandardButton resolvedDefaultButton() const {
    if (standardButtons.testFlag(defaultButton)) {
      return defaultButton;
    }

    const QList<StandardButton> visibleButtons = orderedVisibleButtons();
    for (StandardButton buttonType : visibleButtons) {
      if (isDefaultRole(roleForStandardButton(buttonType))) {
        return buttonType;
      }
    }
    return visibleButtons.isEmpty() ? StandardButton::NoButton : visibleButtons.constFirst();
  }

  StandardButton resolvedEscapeButton() const {
    if (standardButtons.testFlag(escapeButton)) {
      return escapeButton;
    }

    if (standardButtons.testFlag(StandardButton::Cancel)) {
      return StandardButton::Cancel;
    }
    if (standardButtons.testFlag(StandardButton::No)) {
      return StandardButton::No;
    }
    if (standardButtons.testFlag(StandardButton::Close)) {
      return StandardButton::Close;
    }

    const QList<StandardButton> visibleButtons = orderedVisibleButtons();
    for (StandardButton buttonType : visibleButtons) {
      if (isRejectedRole(roleForStandardButton(buttonType))) {
        return buttonType;
      }
    }
    return StandardButton::NoButton;
  }

  adqt::icons::IconRef resolvedIconRef(const DerivedVisualStyle& style) const {
    adqt::icons::IconRef token;
    if (customIconRef.has_value() && adqt::icons::isValid(customIconRef.value())) {
      token = customIconRef.value();
    } else {
      switch (icon) {
        case Icon::Information:
          token = filled_icons::InfoCircle();
          break;
        case Icon::Critical:
          token = filled_icons::CloseCircle();
          break;
        case Icon::Question:
          token = outlined_icons::QuestionCircle();
          break;
        case Icon::Warning:
          token = filled_icons::ExclamationCircle();
          break;
        case Icon::NoIcon:
          return {};
      }
    }

    if (!token.colors().primarySlot()) {
      token = token.withColors(token.colors().withPrimary(style.iconColor));
    }
    return token;
  }

  DerivedVisualStyle deriveVisualStyle() const {
    DerivedVisualStyle style;
    const QWidget* owner = styleOwnerWidget();
    const adqt::theme::ThemeManager& themeManager = adqt::theme::ThemeManager::instance();
    const adqt::theme::ResolvedTheme resolved = themeManager.resolve(owner, owner);
    const adqt::theme::ThemeMapToken& map = resolved.values;
    const adqt::theme::ThemeSeedToken& seed = resolved.config;

    style.titleMinWidth = 177;
    style.popupMaximumWidth = 320;
    style.zIndexPopup = static_cast<int>(std::round(seed.zIndexPopupBase + 60.0));
    style.messageGap = std::max(0, qRound(map.sizeXS));
    style.messageBottom = std::max(0, qRound(map.sizeXS));
    style.descriptionGap = std::max(0, qRound(map.sizeXXS));
    style.buttonGap = std::max(0, qRound(map.sizeXS));
    style.iconSize = std::max(12, qRound(map.fontSize));
    style.titleColor = toColor(map.colorText, QColor("#141414"));
    style.descriptionColor = toColor(map.colorText, QColor("#141414"));

    switch (icon) {
      case Icon::Information:
        style.iconColor = toColor(map.colorInfo, QColor("#1677ff"));
        break;
      case Icon::Critical:
        style.iconColor = toColor(map.colorError, QColor("#ff4d4f"));
        break;
      case Icon::Question:
        style.iconColor = toColor(map.colorInfo, QColor("#1677ff"));
        break;
      case Icon::Warning:
      case Icon::NoIcon:
        style.iconColor = toColor(map.colorWarning, QColor("#faad14"));
        break;
    }

    const QFont baseFont = owner ? owner->font() : QFont();
    style.titleFont = baseFont;
    style.titleFont.setPixelSize(std::max(12, qRound(map.fontSize)));
    style.titleFont.setWeight(QFont::DemiBold);

    style.titleOnlyFont = style.titleFont;
    style.titleOnlyFont.setWeight(QFont::Normal);

    style.descriptionFont = baseFont;
    style.descriptionFont.setPixelSize(std::max(12, qRound(map.fontSize)));
    style.descriptionFont.setWeight(QFont::Normal);

    if (!enabled || (sourceWidget && !sourceWidget->isEnabled())) {
      const QColor disabledText = toColor(map.colorTextQuaternary, QColor("#bfbfbf"));
      style.titleColor = disabledText;
      style.descriptionColor = disabledText;
    }

    if (componentTokens.titleMinWidth.has_value()) {
      style.titleMinWidth = std::max(0, componentTokens.titleMinWidth.value());
    }
    if (componentTokens.popupMaximumWidth.has_value()) {
      style.popupMaximumWidth = std::max(1, componentTokens.popupMaximumWidth.value());
    }
    if (componentTokens.zIndexPopup.has_value()) {
      style.zIndexPopup = std::max(0, componentTokens.zIndexPopup.value());
    }
    if (componentTokens.messageGap.has_value()) {
      style.messageGap = std::max(0, componentTokens.messageGap.value());
    }
    if (componentTokens.messageBottom.has_value()) {
      style.messageBottom = std::max(0, componentTokens.messageBottom.value());
    }
    if (componentTokens.descriptionGap.has_value()) {
      style.descriptionGap = std::max(0, componentTokens.descriptionGap.value());
    }
    if (componentTokens.buttonGap.has_value()) {
      style.buttonGap = std::max(0, componentTokens.buttonGap.value());
    }
    if (componentTokens.iconSize.has_value()) {
      style.iconSize = std::max(10, componentTokens.iconSize.value());
    }
    if (componentTokens.iconColor.has_value()) {
      style.iconColor = toColor(componentTokens.iconColor.value(), style.iconColor);
    }

    AdPopconfirm::StyleContext context;
    context.placement = popover ? popover->placement() : AdPopconfirm::Placement::Top;
    context.triggers = popover ? popover->triggers() : AdPopconfirm::Trigger::Click;
    context.visibilityMode =
        popover ? popover->visibilityPolicy() : AdPopconfirm::VisibilityMode::Automatic;
    context.visible = popover && popover->isVisible();
    context.enabled = enabled && (!sourceWidget || sourceWidget->isEnabled());
    context.arrowVisible = popover ? popover->arrowVisible() : true;
    context.standardButtons = standardButtons;
    context.icon = icon;
    const AdPopconfirm::SemanticStyles effectiveSemantic =
        semanticStyleResolver ? semanticStyleResolver(context) : semanticStyles;

    applySemanticSlot(effectiveSemantic.title, &style.titleColor, nullptr, nullptr);
    applySemanticSlot(effectiveSemantic.description, &style.descriptionColor, nullptr, nullptr);
    applySemanticSlot(effectiveSemantic.icon, &style.iconColor, nullptr, nullptr);
    applySemanticSlot(effectiveSemantic.root, nullptr, &style.popoverBackgroundColor,
                      &style.popoverBorderColor);
    applySemanticSlot(effectiveSemantic.container, nullptr, &style.popoverBackgroundColor,
                      &style.popoverBorderColor);
    applySemanticSlot(effectiveSemantic.arrow, nullptr, &style.popoverBackgroundColor,
                      &style.popoverBorderColor);

    return style;
  }

  void clearSourceObservation() {
    if (sourceWidget) {
      sourceWidget->removeEventFilter(q);
    }
  }

  void refreshSourceObservation() {
    if (sourceWidget) {
      sourceWidget->installEventFilter(q);
    }
  }

  bool watchesPopupObject(const QObject* object) const {
    if (!object) {
      return false;
    }
    return std::any_of(
        watchedPopupObjects.cbegin(), watchedPopupObjects.cend(),
        [object](const QPointer<QObject>& watchedObject) { return watchedObject == object; });
  }

  void clearObservedObjects() {
    for (auto it = watchedPopupObjects.begin(); it != watchedPopupObjects.end(); ++it) {
      if (it->data()) {
        it->data()->removeEventFilter(q);
      }
    }
    watchedPopupObjects.clear();
  }

  void refreshObservedObjects() {
    clearObservedObjects();
    if (!panel) {
      return;
    }
    traverseObjectTree(panel, [this](QObject* object) {
      if (!object) {
        return;
      }
      object->installEventFilter(q);
      watchedPopupObjects.append(object);
    });
  }

  void refreshAccessibility() {
    if (!panel) {
      return;
    }

    const QString accessibleTitle = plainTextForAccessibility(text);
    const QString accessibleDescriptionText = plainTextForAccessibility(informativeText);
    const StandardButton fallbackButton = resolvedDefaultButton() != StandardButton::NoButton
                                              ? resolvedDefaultButton()
                                              : resolvedEscapeButton();
    const QString fallbackButtonText =
        plainTextForAccessibility(resolvedButtonText(fallbackButton));
    const QString accessibleName =
        firstNonEmpty({accessibleTitle, accessibleDescriptionText, fallbackButtonText,
                       AdPopconfirm::tr("Confirmation")});
    const QString accessibleDescription =
        firstNonEmpty({accessibleDescriptionText, accessibleTitle});

    panel->setAccessibleName(accessibleName);
    panel->setAccessibleDescription(accessibleDescription);
    panel->titleLabel()->setAccessibleName(accessibleTitle);
    panel->titleLabel()->setAccessibleDescription(accessibleDescriptionText);
    panel->descriptionLabel()->setAccessibleName(accessibleDescriptionText);
    panel->descriptionLabel()->setAccessibleDescription(accessibleDescriptionText);

    const QList<StandardButton> orderedButtons = orderedVisibleButtons();
    for (StandardButton buttonType : orderedButtons) {
      if (AdButton* control = panel->button(buttonType)) {
        const QString buttonAccessibleText =
            plainTextForAccessibility(resolvedButtonText(buttonType));
        control->setAccessibleName(buttonAccessibleText);
        control->setAccessibleDescription(accessibleDescription);
      }
    }
  }

  void ensurePanel() {
    if (panel) {
      return;
    }

    auto* contentPanel = new PopconfirmPanel();
    contentPanel->setLayoutDirection(effectiveLayoutDirection());
    panel = contentPanel;

    QObject::connect(contentPanel->escapeShortcut(), &QShortcut::activated, q, [this]() {
      const StandardButton escape = resolvedEscapeButton();
      if (escape != StandardButton::NoButton) {
        handleButtonClicked(escape, true);
      }
    });
    QObject::connect(contentPanel->buttonBox(), &QDialogButtonBox::clicked, q,
                     [this](QAbstractButton* button) {
                       if (!panel) {
                         return;
                       }
                       const StandardButton standardButton = panel->standardButtonFor(button);
                       if (standardButton != StandardButton::NoButton) {
                         handleButtonClicked(standardButton, false);
                       }
                     });

    if (popover) {
      popover->setContentWidget(panel);
    }
    refreshObservedObjects();
    refreshAccessibility();
  }

  void syncButtons(const DerivedVisualStyle& style) {
    if (!panel) {
      return;
    }

    const QList<StandardButton> visibleButtons = orderedVisibleButtons();
    const StandardButton defaultAction = resolvedDefaultButton();
    const bool effectiveEnabled = enabled && (!sourceWidget || sourceWidget->isEnabled());
    const bool rebuildButtons =
        renderedButtons != visibleButtons ||
        std::any_of(visibleButtons.cbegin(), visibleButtons.cend(),
                    [this](StandardButton buttonType) { return !panel->button(buttonType); });

    if (rebuildButtons) {
      panel->clearButtons();
      renderedButtons = visibleButtons;

      for (StandardButton buttonType : renderedButtons) {
        panel->addButton(buttonType, roleForStandardButton(buttonType));
      }

      QList<StandardButton> visualButtons = renderedButtons;
      std::stable_partition(visualButtons.begin(), visualButtons.end(), [](StandardButton button) {
        return isRejectedRole(roleForStandardButton(button));
      });
      panel->setButtonOrder(visualButtons);
    }

    for (StandardButton buttonType : renderedButtons) {
      AdButton* control = panel->button(buttonType);
      if (!control) {
        continue;
      }
      control->setText(resolvedButtonText(buttonType));
      control->setButtonStyle(
          buttonConfig(buttonType).style.value_or(defaultStyleForStandardButton(buttonType)));
      control->setAccentRole(
          buttonConfig(buttonType)
              .accentRole.value_or(defaultAccentRoleForStandardButton(buttonType)));
      control->setBusy(buttonConfig(buttonType).busy);
      control->setEnabled(effectiveEnabled);

      const bool isDefault = buttonType == defaultAction;
      control->setAutoDefault(isDefault);
      control->setDefault(isDefault);
    }

    if (QLayout* buttonLayout = panel->buttonBox()->layout()) {
      buttonLayout->setContentsMargins(0, 0, 0, 0);
      buttonLayout->setSpacing(std::max(0, style.buttonGap));
    }
    panel->buttonBox()->setVisible(!renderedButtons.isEmpty());
  }

  void refreshVisualStyle() {
    if (!popover) {
      return;
    }

    const bool hasTitleText = !text.trimmed().isEmpty();
    const bool hasDescriptionText = !informativeText.trimmed().isEmpty();
    const bool hasOverlayContent = hasTitleText || hasDescriptionText;

    if (!hasOverlayContent) {
      clearObservedObjects();
      if (popover->contentWidget() == panel) {
        QWidget* detached = popover->takeContentWidget();
        if (detached) {
          detached->hide();
          detached->setParent(nullptr);
        }
      }
      if (panel) {
        panel->hide();
      }
      return;
    }

    ensurePanel();
    if (!panel) {
      return;
    }

    panel->setLayoutDirection(effectiveLayoutDirection());
    panel->show();
    panel->titleLabel()->setText(text);
    panel->titleLabel()->setVisible(hasTitleText);
    panel->descriptionLabel()->setText(informativeText);
    panel->descriptionLabel()->setVisible(hasDescriptionText);
    panel->messageHost()->setVisible(true);

    if (popover->contentWidget() != panel) {
      popover->setContentWidget(panel);
    }

    const DerivedVisualStyle style = deriveVisualStyle();
    panel->textLayout()->setSpacing(
        hasTitleText && hasDescriptionText ? std::max(0, style.descriptionGap) : 0);

    panel->titleLabel()->setFont(hasDescriptionText ? style.titleFont : style.titleOnlyFont);
    QPalette titlePalette = panel->titleLabel()->palette();
    titlePalette.setColor(QPalette::WindowText, style.titleColor);
    panel->titleLabel()->setPalette(titlePalette);

    panel->descriptionLabel()->setFont(style.descriptionFont);
    panel->descriptionLabel()->setContentsMargins(0, 0, 0, 0);
    QPalette descriptionPalette = panel->descriptionLabel()->palette();
    descriptionPalette.setColor(QPalette::WindowText, style.descriptionColor);
    panel->descriptionLabel()->setPalette(descriptionPalette);

    const adqt::icons::IconRef iconRef = resolvedIconRef(style);
    const bool showIcon = adqt::icons::isValid(iconRef);
    panel->iconLabel()->setVisible(showIcon);
    if (showIcon) {
      const QSize iconSize(std::max(10, style.iconSize), std::max(10, style.iconSize));
      const qreal dpr =
          styleOwnerWidget() ? std::max(1.0, styleOwnerWidget()->devicePixelRatioF()) : 1.0;
      panel->iconLabel()->setPixmap(adqt::icons::renderIconPixmap(iconRef, {iconSize, dpr}));
      panel->iconLabel()->setFixedSize(iconSize);
    } else {
      panel->iconLabel()->setPixmap(QPixmap());
      panel->iconLabel()->setFixedSize(QSize(0, 0));
    }

    panel->messageLayout()->setSpacing(showIcon ? std::max(0, style.messageGap) : 0);

    syncButtons(style);
    panel->rootLayout()->setSpacing(
        panel->buttonBox()->isVisible() ? std::max(0, style.messageBottom) : 0);
    const auto intrinsicTextWidth = [](const QLabel* label, const QString& value) {
      if (!label || !label->isVisible()) {
        return 0;
      }
      return measureIntrinsicTextWidth(value, label->font(), label->textFormat());
    };
    const int preferredTextWidth =
        std::max(intrinsicTextWidth(panel->titleLabel(), text),
                 intrinsicTextWidth(panel->descriptionLabel(), informativeText));
    int preferredMessageWidth = preferredTextWidth;
    int iconWidth = 0;
    if (showIcon) {
      iconWidth = panel->iconLabel()->width() > 0 ? panel->iconLabel()->width()
                                                  : panel->iconLabel()->sizeHint().width();
      preferredMessageWidth += iconWidth;
      if (preferredTextWidth > 0) {
        preferredMessageWidth += std::max(0, style.messageGap);
      }
    }

    int preferredPanelWidth = std::max(0, style.titleMinWidth);
    preferredPanelWidth = std::max(preferredPanelWidth, preferredMessageWidth);
    if (panel->buttonBox()->isVisible()) {
      const QSize buttonBoxSize =
          panel->buttonBox()->sizeHint().expandedTo(panel->buttonBox()->minimumSizeHint());
      preferredPanelWidth = std::max(preferredPanelWidth, buttonBoxSize.width());
    }

    int contentMaximumWidth = std::max(0, style.popupMaximumWidth);
    if (popover) {
      const QMargins margins = popover->contentMargins();
      const int horizontalMargins = std::max(0, margins.left()) + std::max(0, margins.right());
      contentMaximumWidth = std::max(0, style.popupMaximumWidth - horizontalMargins);
    }
    contentMaximumWidth = std::max(contentMaximumWidth, std::max(0, style.titleMinWidth));
    const int panelWidth = std::min(preferredPanelWidth, contentMaximumWidth);
    panel->setFixedWidth(panelWidth);

    const int availableTextWidth =
        std::max(1, panelWidth - (showIcon ? iconWidth + std::max(0, style.messageGap) : 0));
    const auto measuredLabelHeight = [&](QLabel* label, const QString& value) {
      if (!label || !label->isVisible()) {
        return 0;
      }
      return measureIntrinsicTextHeight(value, label->font(), label->textFormat(),
                                        availableTextWidth + 1) +
             labelVerticalPadding(label);
    };

    const int titleHeight = measuredLabelHeight(panel->titleLabel(), text);
    const int descriptionHeight = measuredLabelHeight(panel->descriptionLabel(), informativeText);
    panel->titleLabel()->setFixedHeight(titleHeight);
    panel->descriptionLabel()->setFixedHeight(descriptionHeight);

    const int textHeight =
        titleHeight + (hasTitleText && hasDescriptionText ? std::max(0, style.descriptionGap) : 0) +
        descriptionHeight;
    panel->textHost()->setFixedHeight(textHeight);

    const int messageHeight = std::max(textHeight, showIcon ? panel->iconLabel()->height() : 0);
    panel->messageHost()->setFixedHeight(messageHeight);

    const int buttonHeight = panel->buttonBox()->isVisible()
                                 ? panel->buttonBox()
                                       ->sizeHint()
                                       .expandedTo(panel->buttonBox()->minimumSizeHint())
                                       .height()
                                 : 0;
    panel->setFixedHeight(messageHeight +
                          (buttonHeight > 0 ? std::max(0, style.messageBottom) + buttonHeight : 0));
    if (QLayout* panelLayout = panel->layout()) {
      panelLayout->activate();
    }
    panel->updateGeometry();

    refreshAccessibility();
    refreshObservedObjects();

    popover->setTitleMinimumWidth(style.titleMinWidth);
    popover->setMaximumWidth(style.popupMaximumWidth);
    popover->setZIndex(style.zIndexPopup);
    popover->setBackgroundColor(style.popoverBackgroundColor);
    popover->setBorderColor(style.popoverBorderColor);
    popover->setEnabled(enabled);
    popover->refreshPopupLayout();
  }

  void scheduleInitialFocus() {
    const StandardButton defaultAction = resolvedDefaultButton();
    const QList<StandardButton> orderedButtons = orderedVisibleButtons();
    QPointer<AdPopconfirm> self(q);
    QTimer::singleShot(0, q, [self, defaultAction, orderedButtons]() {
      if (!self || !self->isVisible()) {
        return;
      }

      const auto focusIfReady = [](AdButton* control) {
        if (!control || !control->isVisible() || !control->isEnabled()) {
          return false;
        }
        control->setFocus(Qt::PopupFocusReason);
        return true;
      };

      if (focusIfReady(self->button(defaultAction))) {
        return;
      }

      for (StandardButton buttonType : orderedButtons) {
        if (focusIfReady(self->button(buttonType))) {
          return;
        }
      }
    });
  }

  void requestHideAfterAction() {
    if (!popover) {
      return;
    }

    QPointer<AdPopconfirm> self(q);
    QPointer<AdPopover> popoverGuard(popover);
    QTimer::singleShot(0, q, [self, popoverGuard]() {
      if (!self || !popoverGuard) {
        return;
      }
      if (popoverGuard->visibilityPolicy() == AdPopover::VisibilityPolicy::Manual) {
        emit self->visibilityRequested(false);
        return;
      }
      popoverGuard->setVisible(false);
    });
  }

  void handleButtonClicked(StandardButton buttonType, bool forceClose) {
    QPointer<AdPopconfirm> guard(q);
    emit q->clicked(buttonType);
    if (!guard) {
      return;
    }

    const QDialogButtonBox::ButtonRole role = roleForStandardButton(buttonType);
    if (isAcceptedRole(role)) {
      emit q->accepted();
      if (!guard) {
        return;
      }
    } else if (isRejectedRole(role)) {
      emit q->rejected();
      if (!guard) {
        return;
      }
    } else if (isHelpRole(role)) {
      emit q->helpRequested();
      if (!guard) {
        return;
      }
    }

    if (forceClose || autoCloseButtons.testFlag(buttonType)) {
      requestHideAfterAction();
    }
  }

  AdPopconfirm* q = nullptr;
  QPointer<AdPopover> popover;
  QPointer<QWidget> sourceWidget;
  bool enabled = true;

  QPointer<PopconfirmPanel> panel;
  QList<StandardButton> renderedButtons;
  QList<QPointer<QObject>> watchedPopupObjects;

  QString text;
  QString informativeText;
  StandardButtons standardButtons = StandardButton::Ok | StandardButton::Cancel;
  StandardButton defaultButton = StandardButton::NoButton;
  StandardButton escapeButton = StandardButton::NoButton;
  StandardButtons autoCloseButtons =
      StandardButton::Ok | StandardButton::Save | StandardButton::SaveAll | StandardButton::Open |
      StandardButton::Yes | StandardButton::YesToAll | StandardButton::No |
      StandardButton::NoToAll | StandardButton::Abort | StandardButton::Retry |
      StandardButton::Ignore | StandardButton::Close | StandardButton::Cancel |
      StandardButton::Discard;
  Icon icon = Icon::Warning;
  std::optional<adqt::icons::IconRef> customIconRef;
  QHash<int, ButtonConfig> buttonConfigs;

  AdPopconfirm::ComponentTokens componentTokens;
  AdPopconfirm::SemanticStyles semanticStyles;
  AdPopconfirm::SemanticStyleResolver semanticStyleResolver;
};

AdPopconfirm::AdPopconfirm(QObject* parent)
    : QObject(parent), d_(std::make_unique<AdPopconfirmPrivate>(this)) {
  if (qApp) {
    qApp->installEventFilter(this);
  }

  d_->popover = new AdPopover(this);
  d_->popover->setTitle(QString());
  d_->popover->setText(QString());
  d_->popover->setTriggers(Trigger::Click);
  d_->popover->setEnabled(d_->enabled);

  connect(d_->popover, &AdPopover::placementChanged, this, [this](AdPopover::Placement value) {
    emit placementChanged(value);
    d_->refreshVisualStyle();
  });
  connect(d_->popover, &AdPopover::triggersChanged, this, [this](AdPopover::Triggers value) {
    emit triggersChanged(value);
    d_->refreshVisualStyle();
  });
  connect(d_->popover, &AdPopover::visibleChanged, this, [this](bool value) {
    emit visibleChanged(value);
    if (value) {
      d_->scheduleInitialFocus();
    }
    d_->refreshVisualStyle();
  });
  connect(d_->popover, &AdPopover::visibilityRequested, this, &AdPopconfirm::visibilityRequested);
  connect(d_->popover, &AdPopover::visibilityPolicyChanged, this,
          [this](AdPopover::VisibilityPolicy value) { emit visibilityModeChanged(value); });
  connect(d_->popover, &AdPopover::popupLifetimeChanged, this, &AdPopconfirm::popupLifetimeChanged);
  connect(d_->popover, &AdPopover::popupLayerModeChanged, this,
          &AdPopconfirm::popupLayerModeChanged);
  connect(d_->popover, &AdPopover::defaultVisibleChanged, this,
          &AdPopconfirm::defaultVisibleChanged);
  connect(d_->popover, &AdPopover::autoAdjustOverflowChanged, this, [this](bool value) {
    emit autoAdjustOverflowChanged(value);
    d_->refreshVisualStyle();
  });
  connect(d_->popover, &AdPopover::arrowVisibleChanged, this, [this](bool value) {
    emit arrowVisibleChanged(value);
    d_->refreshVisualStyle();
  });
  connect(d_->popover, &AdPopover::arrowPointAtCenterChanged, this,
          &AdPopconfirm::arrowPointAtCenterChanged);
  connect(d_->popover, &AdPopover::hoverOpenDelayMsChanged, this,
          &AdPopconfirm::hoverOpenDelayMsChanged);
  connect(d_->popover, &AdPopover::hoverCloseDelayMsChanged, this,
          &AdPopconfirm::hoverCloseDelayMsChanged);
  connect(d_->popover, &AdPopover::enabledChanged, this, [this](bool value) {
    if (d_->enabled == value) {
      return;
    }
    d_->enabled = value;
    emit enabledChanged(d_->enabled);
    d_->refreshVisualStyle();
  });
  connect(d_->popover, &AdPopover::sourceWidgetChanged, this, [this](QWidget* value) {
    d_->clearSourceObservation();
    d_->sourceWidget = value;
    d_->refreshSourceObservation();
    emit sourceWidgetChanged(value);
    d_->refreshVisualStyle();
  });
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { d_->refreshVisualStyle(); });

  d_->refreshVisualStyle();
}

AdPopconfirm::~AdPopconfirm() {
  if (qApp) {
    qApp->removeEventFilter(this);
  }
}

AdPopconfirm::Placement AdPopconfirm::placement() const {
  return d_->popover ? d_->popover->placement() : Placement::Top;
}

void AdPopconfirm::setPlacement(Placement value) {
  if (d_->popover) {
    d_->popover->setPlacement(value);
  }
}

AdPopconfirm::Triggers AdPopconfirm::triggers() const {
  return d_->popover ? d_->popover->triggers() : Trigger::Click;
}

void AdPopconfirm::setTriggers(Triggers value) {
  if (d_->popover) {
    d_->popover->setTriggers(value);
  }
}

bool AdPopconfirm::isVisible() const { return d_->popover && d_->popover->isVisible(); }

void AdPopconfirm::setVisible(bool value) {
  if (d_->popover) {
    d_->popover->setVisible(value);
  }
}

void AdPopconfirm::show() { setVisible(true); }

void AdPopconfirm::hide() { setVisible(false); }

void AdPopconfirm::toggle() { setVisible(!isVisible()); }

AdPopconfirm::VisibilityMode AdPopconfirm::visibilityMode() const {
  return d_->popover ? d_->popover->visibilityPolicy() : VisibilityMode::Automatic;
}

void AdPopconfirm::setVisibilityMode(VisibilityMode value) {
  if (d_->popover) {
    d_->popover->setVisibilityPolicy(value);
  }
}

AdPopconfirm::PopupLifetime AdPopconfirm::popupLifetime() const {
  return d_->popover ? d_->popover->popupLifetime() : PopupLifetime::Retained;
}

void AdPopconfirm::setPopupLifetime(PopupLifetime value) {
  if (d_->popover) {
    d_->popover->setPopupLifetime(value);
  }
}

AdPopconfirm::PopupLayerMode AdPopconfirm::popupLayerMode() const {
  return d_->popover ? d_->popover->popupLayerMode() : PopupLayerMode::InWindow;
}

void AdPopconfirm::setPopupLayerMode(PopupLayerMode value) {
  if (d_->popover) {
    d_->popover->setPopupLayerMode(value);
  }
}

bool AdPopconfirm::defaultVisible() const { return d_->popover && d_->popover->defaultVisible(); }

void AdPopconfirm::setDefaultVisible(bool value) {
  if (d_->popover) {
    d_->popover->setDefaultVisible(value);
  }
}

bool AdPopconfirm::autoAdjustOverflow() const {
  return d_->popover && d_->popover->autoAdjustOverflow();
}

void AdPopconfirm::setAutoAdjustOverflow(bool value) {
  if (d_->popover) {
    d_->popover->setAutoAdjustOverflow(value);
  }
}

bool AdPopconfirm::arrowVisible() const { return d_->popover && d_->popover->arrowVisible(); }

void AdPopconfirm::setArrowVisible(bool value) {
  if (d_->popover) {
    d_->popover->setArrowVisible(value);
  }
}

bool AdPopconfirm::arrowPointAtCenter() const {
  return d_->popover && d_->popover->arrowPointAtCenter();
}

void AdPopconfirm::setArrowPointAtCenter(bool value) {
  if (d_->popover) {
    d_->popover->setArrowPointAtCenter(value);
  }
}

int AdPopconfirm::hoverOpenDelayMs() const {
  return d_->popover ? d_->popover->hoverOpenDelayMs() : 100;
}

void AdPopconfirm::setHoverOpenDelayMs(int value) {
  if (d_->popover) {
    d_->popover->setHoverOpenDelayMs(value);
  }
}

int AdPopconfirm::hoverCloseDelayMs() const {
  return d_->popover ? d_->popover->hoverCloseDelayMs() : 100;
}

void AdPopconfirm::setHoverCloseDelayMs(int value) {
  if (d_->popover) {
    d_->popover->setHoverCloseDelayMs(value);
  }
}

bool AdPopconfirm::isEnabled() const { return d_->enabled; }

void AdPopconfirm::setEnabled(bool value) {
  if (d_->enabled == value) {
    return;
  }
  d_->enabled = value;
  emit enabledChanged(d_->enabled);
  if (d_->popover) {
    d_->popover->setEnabled(d_->enabled);
  }
  d_->refreshVisualStyle();
}

QString AdPopconfirm::text() const { return d_->text; }

void AdPopconfirm::setText(const QString& value) {
  if (d_->text == value) {
    return;
  }
  d_->text = value;
  emit textChanged(d_->text);
  d_->refreshVisualStyle();
}

QString AdPopconfirm::informativeText() const { return d_->informativeText; }

void AdPopconfirm::setInformativeText(const QString& value) {
  if (d_->informativeText == value) {
    return;
  }
  d_->informativeText = value;
  emit informativeTextChanged(d_->informativeText);
  d_->refreshVisualStyle();
}

AdPopconfirm::StandardButtons AdPopconfirm::standardButtons() const { return d_->standardButtons; }

void AdPopconfirm::setStandardButtons(StandardButtons value) {
  const StandardButtons normalized = value & allSupportedStandardButtons();
  if (d_->standardButtons == normalized) {
    return;
  }
  d_->standardButtons = normalized;
  emit standardButtonsChanged(d_->standardButtons);
  d_->refreshVisualStyle();
}

AdPopconfirm::StandardButton AdPopconfirm::defaultButton() const { return d_->defaultButton; }

void AdPopconfirm::setDefaultButton(StandardButton value) {
  const StandardButton normalized =
      isSupportedStandardButton(value) ? value : StandardButton::NoButton;
  if (d_->defaultButton == normalized) {
    return;
  }
  d_->defaultButton = normalized;
  emit defaultButtonChanged(d_->defaultButton);
  d_->refreshVisualStyle();
}

AdPopconfirm::StandardButton AdPopconfirm::escapeButton() const { return d_->escapeButton; }

void AdPopconfirm::setEscapeButton(StandardButton value) {
  const StandardButton normalized =
      isSupportedStandardButton(value) ? value : StandardButton::NoButton;
  if (d_->escapeButton == normalized) {
    return;
  }
  d_->escapeButton = normalized;
  emit escapeButtonChanged(d_->escapeButton);
  d_->refreshVisualStyle();
}

AdPopconfirm::StandardButtons AdPopconfirm::autoCloseButtons() const {
  return d_->autoCloseButtons;
}

void AdPopconfirm::setAutoCloseButtons(StandardButtons value) {
  const StandardButtons normalized = value & allSupportedStandardButtons();
  if (d_->autoCloseButtons == normalized) {
    return;
  }
  d_->autoCloseButtons = normalized;
  emit autoCloseButtonsChanged(d_->autoCloseButtons);
}

AdPopconfirm::Icon AdPopconfirm::icon() const { return d_->icon; }

void AdPopconfirm::setIcon(Icon value) {
  if (d_->icon == value) {
    return;
  }
  d_->icon = value;
  emit iconChanged(d_->icon);
  d_->refreshVisualStyle();
}

adqt::icons::IconRef AdPopconfirm::customIconRef() const {
  return d_->customIconRef.value_or(adqt::icons::IconRef{});
}

void AdPopconfirm::setCustomIconRef(const adqt::icons::IconRef& value) {
  if (!adqt::icons::isValid(value)) {
    clearCustomIconRef();
    return;
  }
  if (d_->customIconRef.has_value() && iconRefsEqual(d_->customIconRef.value(), value)) {
    return;
  }
  d_->customIconRef = value;
  emit customIconRefChanged(d_->customIconRef.value());
  d_->refreshVisualStyle();
}

void AdPopconfirm::clearCustomIconRef() {
  if (!d_->customIconRef.has_value()) {
    return;
  }
  d_->customIconRef.reset();
  emit customIconRefChanged(adqt::icons::IconRef{});
  d_->refreshVisualStyle();
}

bool AdPopconfirm::hasCustomIconRef() const {
  return d_->customIconRef.has_value() && adqt::icons::isValid(d_->customIconRef.value());
}

QString AdPopconfirm::buttonText(StandardButton buttonType) const {
  return d_->resolvedButtonText(buttonType);
}

void AdPopconfirm::setButtonText(StandardButton buttonType, const QString& value) {
  AdPopconfirmPrivate::ButtonConfig* config = d_->mutableButtonConfig(buttonType);
  if (!config) {
    return;
  }
  if (config->text.has_value() && config->text.value() == value) {
    return;
  }
  config->text = value;
  d_->refreshVisualStyle();
}

AdButton::AccentRole AdPopconfirm::buttonAccentRole(StandardButton buttonType) const {
  const AdPopconfirmPrivate::ButtonConfig config = d_->buttonConfig(buttonType);
  return config.accentRole.value_or(defaultAccentRoleForStandardButton(buttonType));
}

void AdPopconfirm::setButtonAccentRole(StandardButton buttonType, AdButton::AccentRole value) {
  AdPopconfirmPrivate::ButtonConfig* config = d_->mutableButtonConfig(buttonType);
  if (!config) {
    return;
  }
  if (config->accentRole.has_value() && config->accentRole.value() == value) {
    return;
  }
  config->accentRole = value;
  d_->refreshVisualStyle();
}

AdButton::ButtonStyle AdPopconfirm::buttonStyle(StandardButton buttonType) const {
  const AdPopconfirmPrivate::ButtonConfig config = d_->buttonConfig(buttonType);
  return config.style.value_or(defaultStyleForStandardButton(buttonType));
}

void AdPopconfirm::setButtonStyle(StandardButton buttonType, AdButton::ButtonStyle value) {
  AdPopconfirmPrivate::ButtonConfig* config = d_->mutableButtonConfig(buttonType);
  if (!config) {
    return;
  }
  if (config->style.has_value() && config->style.value() == value) {
    return;
  }
  config->style = value;
  d_->refreshVisualStyle();
}

bool AdPopconfirm::buttonBusy(StandardButton buttonType) const {
  return d_->buttonConfig(buttonType).busy;
}

void AdPopconfirm::setButtonBusy(StandardButton buttonType, bool value) {
  AdPopconfirmPrivate::ButtonConfig* config = d_->mutableButtonConfig(buttonType);
  if (!config) {
    return;
  }
  if (config->busy == value) {
    return;
  }
  config->busy = value;
  d_->refreshVisualStyle();
}

QWidget* AdPopconfirm::sourceWidget() const { return d_->sourceWidget; }

void AdPopconfirm::setSourceWidget(QWidget* widget) {
  if (d_->popover) {
    d_->popover->setSourceWidget(widget);
  }
}

AdButton* AdPopconfirm::button(StandardButton buttonType) const {
  return d_->panel ? d_->panel->button(buttonType) : nullptr;
}

AdPopconfirm::ComponentTokens AdPopconfirm::componentTokens() const { return d_->componentTokens; }

void AdPopconfirm::setComponentTokens(const ComponentTokens& tokens) {
  d_->componentTokens = tokens;
  emit componentTokensChanged();
  d_->refreshVisualStyle();
}

void AdPopconfirm::resetComponentTokens() {
  d_->componentTokens = ComponentTokens{};
  emit componentTokensChanged();
  d_->refreshVisualStyle();
}

AdPopconfirm::SemanticStyles AdPopconfirm::semanticStyles() const { return d_->semanticStyles; }

void AdPopconfirm::setSemanticStyles(const SemanticStyles& styles) {
  d_->semanticStyles = styles;
  emit semanticStylesChanged();
  d_->refreshVisualStyle();
}

void AdPopconfirm::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  d_->semanticStyleResolver = std::move(resolver);
  emit semanticStylesChanged();
  d_->refreshVisualStyle();
}

bool AdPopconfirm::eventFilter(QObject* watched, QEvent* event) {
  if (event && event->type() == QEvent::LanguageChange && d_->watchesPopupObject(watched)) {
    d_->refreshAccessibility();
    d_->refreshVisualStyle();
  }

  if (event && event->type() == QEvent::KeyPress && d_->popover && d_->popover->isVisible()) {
    auto* keyEvent = static_cast<QKeyEvent*>(event);
    if (!keyEvent->isAutoRepeat() && keyEvent->key() == Qt::Key_Escape) {
      QWidget* watchedWidget = qobject_cast<QWidget*>(watched);
      QWidget* focusWidget = QApplication::focusWidget();
      if (widgetInTree(watchedWidget, d_->panel) || widgetInTree(focusWidget, d_->panel)) {
        const StandardButton escape = d_->resolvedEscapeButton();
        if (escape != StandardButton::NoButton) {
          d_->handleButtonClicked(escape, true);
          return true;
        }
      }
    }
  }

  if (watched && event && d_->watchesPopupObject(watched)) {
    if (event->type() == QEvent::MouseButtonPress) {
      emit popupClicked();
    }
  } else if (watched == d_->sourceWidget && event && isVisualRefreshEvent(event->type())) {
    if (d_->popover) {
      d_->popover->setEnabled(d_->enabled);
    }
    d_->refreshVisualStyle();
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace adqt::widgets
