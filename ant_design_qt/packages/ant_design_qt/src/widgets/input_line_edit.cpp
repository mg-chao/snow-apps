#include "input_line_edit.h"

#include "antd_icons.h"
#include "detail/timing_hub.h"
#include "input_internal.h"
#include "input_style.h"
#include "interaction_overlay_manager.h"
#include "theme/theme_manager.h"

#include <QDynamicPropertyChangeEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QLabel>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QToolButton>
#include <QVariant>

#include <algorithm>
#include <limits>

namespace adqt::widgets {

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;
namespace filled_icons = adqt::icons::antd::filled;

using detail::InputVisualStyle;
using detail::input_internal::InputFramePaintStyle;
using detail::input_internal::InputIconButton;

struct ActionIconColors {
  QColor normal;
  QColor hover;
  QColor pressed;
};

constexpr char kCompactHeightProperty[] = "ad-input-height";
constexpr char kCompactHorizontalPaddingProperty[] = "ad-input-horizontal-padding";
constexpr char kCompactFontPixelSizeProperty[] = "ad-input-font-pixel-size";
constexpr char kCompactIconSizeProperty[] = "ad-input-icon-size";
constexpr char kSemanticBackgroundColorProperty[] = "ad-input-background-color";
constexpr char kSemanticHoverBackgroundColorProperty[] = "ad-input-hover-background-color";
constexpr char kSemanticActiveBackgroundColorProperty[] = "ad-input-active-background-color";
constexpr char kSemanticBorderColorProperty[] = "ad-input-border-color";
constexpr char kSemanticHoverBorderColorProperty[] = "ad-input-hover-border-color";
constexpr char kSemanticActiveBorderColorProperty[] = "ad-input-active-border-color";
constexpr char kSemanticTextColorProperty[] = "ad-input-text-color";
constexpr char kSemanticPlaceholderColorProperty[] = "ad-input-placeholder-color";
constexpr char kSemanticPrefixColorProperty[] = "ad-input-prefix-color";
constexpr char kSemanticSuffixColorProperty[] = "ad-input-suffix-color";
constexpr char kSemanticSuffixActionColorProperty[] = "ad-input-suffix-action-color";
constexpr char kFeedbackSpinnerFrameKey[] = "AdLineEdit.FeedbackSpinnerFrame";

int scaledMinimum(int value, qreal scale) { return std::max(1, qRound(value * scale)); }

bool isLoadingIcon(const adqt::icons::IconRef& icon) {
  const auto metadata = adqt::icons::describeIcon(icon);
  return metadata.key.pack == QStringLiteral("antd") &&
         metadata.key.variant == QStringLiteral("outlined") &&
         metadata.key.name == QStringLiteral("loading");
}

int sharedSpinnerAngle() {
  const int cycleMs = detail::spinnerCycleDurationMs();
  if (cycleMs <= 0) {
    return 0;
  }
  qint64 phaseMs = detail::timingNowMs() % cycleMs;
  if (phaseMs < 0) {
    phaseMs += cycleMs;
  }
  return static_cast<int>((phaseMs * 360) / cycleMs);
}

QPixmap renderIconPixmap(const adqt::icons::IconRef& icon, int iconSide, qreal dpr,
                         int rotationDegrees = 0) {
  if (!adqt::icons::isValid(icon)) {
    return {};
  }

  if (rotationDegrees == 0) {
    return adqt::icons::renderIconPixmap(icon, {QSize(iconSide, iconSide), dpr});
  }

  QPixmap pixmap(QSize(qCeil(iconSide * dpr), qCeil(iconSide * dpr)));
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  const QRectF rect(0, 0, iconSide, iconSide);
  painter.translate(rect.center());
  painter.rotate(rotationDegrees);
  adqt::icons::paintIcon(&painter, icon,
                         QRectF(-iconSide / 2.0, -iconSide / 2.0, iconSide, iconSide));
  return pixmap;
}

QColor resolvedBackgroundColor(const InputVisualStyle& style, bool focused, bool hovered) {
  if (focused) {
    return style.selectorActiveBg;
  }
  if (hovered) {
    return style.selectorHoverBg;
  }
  return style.selectorBg;
}

QColor resolvedBorderColor(const InputVisualStyle& style, bool focused, bool hovered) {
  if (focused) {
    return style.selectorActiveBorderColor;
  }
  if (hovered) {
    return style.selectorHoverBorderColor;
  }
  return style.selectorBorderColor;
}

int affixPadding(const InputVisualStyle& style) { return std::max(0, style.metrics.affixPadding); }

int affixItemGap(const InputVisualStyle& style) {
  return std::max(affixPadding(style), style.metrics.affixItemGap);
}

int horizontalTextWidth(const QFontMetrics& fm, const QString& text) {
  return text.isEmpty() ? 0 : fm.horizontalAdvance(text);
}

ActionIconColors suffixActionColors(const InputVisualStyle& style) {
  // The password visibility toggle is hosted in the shared suffix-action button,
  // but its visual treatment follows Ant Design's weak action icon semantics.
  return {style.suffixActionColor, style.suffixActionHoverColor, style.suffixActionHoverColor};
}

QVariant dynamicProperty(const QObject* object, const char* name) {
  return object ? object->property(name) : QVariant();
}

QColor dynamicColorProperty(const QObject* object, const char* name) {
  const QVariant value = dynamicProperty(object, name);
  if (!value.isValid()) {
    return QColor();
  }
  if (value.canConvert<QColor>()) {
    const QColor color = qvariant_cast<QColor>(value);
    if (color.isValid()) {
      return color;
    }
  }
  if (value.canConvert<QString>()) {
    const QColor color(value.toString());
    if (color.isValid()) {
      return color;
    }
  }
  return QColor();
}

void applyDynamicColor(QColor* target, const QObject* object, const char* name) {
  if (!target) {
    return;
  }
  const QColor color = dynamicColorProperty(object, name);
  if (color.isValid()) {
    *target = color;
  }
}

InputVisualStyle applyDynamicOverrides(InputVisualStyle style, const QObject* object) {
  const QVariant heightValue = dynamicProperty(object, kCompactHeightProperty);
  if (heightValue.isValid()) {
    style.metrics.height = std::max(18, heightValue.toInt());
  }

  const QVariant paddingValue = dynamicProperty(object, kCompactHorizontalPaddingProperty);
  if (paddingValue.isValid()) {
    style.metrics.horizontalPadding = std::max(2, paddingValue.toInt());
  }

  const QVariant fontPixelValue = dynamicProperty(object, kCompactFontPixelSizeProperty);
  if (fontPixelValue.isValid()) {
    style.metrics.font.setPixelSize(std::max(8, fontPixelValue.toInt()));
  }

  const QVariant iconSizeValue = dynamicProperty(object, kCompactIconSizeProperty);
  if (iconSizeValue.isValid()) {
    const int iconSize = std::max(8, iconSizeValue.toInt());
    style.metrics.affixIconSize = iconSize;
    style.metrics.clearIconSize = iconSize;
  } else if (style.metrics.font.pixelSize() > 0) {
    style.metrics.affixIconSize = std::max(8, style.metrics.font.pixelSize());
  }

  const QColor backgroundColor = dynamicColorProperty(object, kSemanticBackgroundColorProperty);
  if (backgroundColor.isValid()) {
    style.selectorBg = backgroundColor;
    style.selectorHoverBg = backgroundColor;
    style.selectorActiveBg = backgroundColor;
  }
  applyDynamicColor(&style.selectorHoverBg, object, kSemanticHoverBackgroundColorProperty);
  applyDynamicColor(&style.selectorActiveBg, object, kSemanticActiveBackgroundColorProperty);

  const QColor borderColor = dynamicColorProperty(object, kSemanticBorderColorProperty);
  if (borderColor.isValid()) {
    style.selectorBorderColor = borderColor;
    style.selectorHoverBorderColor = borderColor;
    style.selectorActiveBorderColor = borderColor;
  }
  applyDynamicColor(&style.selectorHoverBorderColor, object, kSemanticHoverBorderColorProperty);
  applyDynamicColor(&style.selectorActiveBorderColor, object, kSemanticActiveBorderColorProperty);

  applyDynamicColor(&style.selectorTextColor, object, kSemanticTextColorProperty);
  applyDynamicColor(&style.placeholderColor, object, kSemanticPlaceholderColorProperty);
  applyDynamicColor(&style.prefixColor, object, kSemanticPrefixColorProperty);
  applyDynamicColor(&style.suffixColor, object, kSemanticSuffixColorProperty);
  applyDynamicColor(&style.suffixActionColor, object, kSemanticSuffixActionColorProperty);
  applyDynamicColor(&style.suffixActionHoverColor, object, kSemanticSuffixActionColorProperty);

  return style;
}

void applyControlScale(InputVisualStyle* style, qreal scale) {
  if (!style) {
    return;
  }

  auto& metrics = style->metrics;
  const auto scaled = [scale](int value) {
    return value > 0 ? std::max(1, qRound(value * scale)) : 0;
  };
  metrics.height = scaled(metrics.height);
  metrics.borderRadius = scaled(metrics.borderRadius);
  metrics.borderWidth = scaled(metrics.borderWidth);
  metrics.horizontalPadding = scaled(metrics.horizontalPadding);
  metrics.verticalPadding = scaled(metrics.verticalPadding);
  metrics.textLineHeight = scaled(metrics.textLineHeight);
  metrics.affixPadding = scaled(metrics.affixPadding);
  metrics.affixItemGap = scaled(metrics.affixItemGap);
  metrics.affixIconSize = scaled(metrics.affixIconSize);
  metrics.clearIconSize = scaled(metrics.clearIconSize);
  metrics.multilineAffixTopInset = scaled(metrics.multilineAffixTopInset);
  metrics.multilineInlineStartCompensation = scaled(metrics.multilineInlineStartCompensation);
  metrics.countTopMargin = scaled(metrics.countTopMargin);
  metrics.countHeight = scaled(metrics.countHeight);
  metrics.focusOutlineWidth *= scale;
  metrics.focusOutlineOffset *= scale;
  if (metrics.font.pixelSize() > 0) {
    metrics.font.setPixelSize(scaled(metrics.font.pixelSize()));
  } else if (metrics.font.pointSizeF() > 0.0) {
    metrics.font.setPointSizeF(metrics.font.pointSizeF() * scale);
  }
}

bool isDynamicStyleProperty(const QByteArray& propertyName) {
  return propertyName == kCompactHeightProperty ||
         propertyName == kCompactHorizontalPaddingProperty ||
         propertyName == kCompactFontPixelSizeProperty ||
         propertyName == kCompactIconSizeProperty ||
         propertyName == kSemanticBackgroundColorProperty ||
         propertyName == kSemanticHoverBackgroundColorProperty ||
         propertyName == kSemanticActiveBackgroundColorProperty ||
         propertyName == kSemanticBorderColorProperty ||
         propertyName == kSemanticHoverBorderColorProperty ||
         propertyName == kSemanticActiveBorderColorProperty ||
         propertyName == kSemanticTextColorProperty ||
         propertyName == kSemanticPlaceholderColorProperty ||
         propertyName == kSemanticPrefixColorProperty ||
         propertyName == kSemanticSuffixColorProperty ||
         propertyName == kSemanticSuffixActionColorProperty;
}

void setLabelTextColor(QLabel* label, const QColor& color) {
  if (!label) {
    return;
  }
  QPalette palette = label->palette();
  palette.setColor(QPalette::Text, color);
  palette.setColor(QPalette::WindowText, color);
  palette.setColor(QPalette::Disabled, QPalette::Text, color);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
  label->setPalette(palette);
}

void setLabelIcon(QLabel* label, const adqt::icons::IconRef& token, const QColor& color,
                  int iconSide, qreal devicePixelRatio) {
  if (!label) {
    return;
  }

  if (!adqt::icons::isValid(token) || iconSide <= 0) {
    label->clear();
    label->hide();
    return;
  }

  const QPixmap pixmap =
      detail::input_internal::renderTintedIcon(token, color, iconSide, devicePixelRatio);
  label->setPixmap(pixmap);
  label->setFixedSize(iconSide, iconSide);
  label->show();
}

void setButtonIcon(QToolButton* button, const adqt::icons::IconRef& token,
                   const QColor& normalColor, const QColor& hoverColor, const QColor& pressedColor,
                   int iconSide, qreal devicePixelRatio) {
  if (!button) {
    return;
  }

  if (!adqt::icons::isValid(token) || iconSide <= 0) {
    button->setIcon(QIcon());
    return;
  }

  const QPixmap normalPixmap =
      detail::input_internal::renderTintedIcon(token, normalColor, iconSide, devicePixelRatio);
  const QPixmap activePixmap =
      detail::input_internal::renderTintedIcon(token, hoverColor, iconSide, devicePixelRatio);
  const QPixmap pressedPixmap =
      detail::input_internal::renderTintedIcon(token, pressedColor, iconSide, devicePixelRatio);

  QIcon icon;
  icon.addPixmap(normalPixmap, QIcon::Normal, QIcon::Off);
  icon.addPixmap(activePixmap, QIcon::Active, QIcon::Off);
  icon.addPixmap(pressedPixmap.isNull() ? activePixmap : pressedPixmap, QIcon::Selected,
                 QIcon::Off);
  icon.addPixmap(normalPixmap, QIcon::Disabled, QIcon::Off);

  button->setIcon(icon);
  button->setIconSize(QSize(iconSide, iconSide));
}

}  // namespace

AdLineEdit::AdLineEdit(QWidget* parent) : QLineEdit(parent) {
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed));
  setFocusPolicy(Qt::StrongFocus);
  setFrame(false);
  setClearButtonEnabled(false);

  prefixIconLabel_ = new QLabel(this);
  prefixIconLabel_->setObjectName(QStringLiteral("ad-input-prefix-icon"));
  prefixIconLabel_->setProperty("adqt.semanticSlot", QStringLiteral("prefix"));
  prefixIconLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  prefixIconLabel_->setAlignment(Qt::AlignCenter);
  prefixIconLabel_->hide();
  prefixIconLabel_->installEventFilter(this);

  prefixLabel_ = new QLabel(this);
  prefixLabel_->setObjectName(QStringLiteral("ad-input-prefix"));
  prefixLabel_->setProperty("adqt.semanticSlot", QStringLiteral("prefix"));
  prefixLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  prefixLabel_->hide();
  prefixLabel_->installEventFilter(this);

  suffixLabel_ = new QLabel(this);
  suffixLabel_->setObjectName(QStringLiteral("ad-input-suffix"));
  suffixLabel_->setProperty("adqt.semanticSlot", QStringLiteral("suffix"));
  suffixLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  suffixLabel_->hide();
  suffixLabel_->installEventFilter(this);

  suffixIconLabel_ = new QLabel(this);
  suffixIconLabel_->setObjectName(QStringLiteral("ad-input-suffix-icon"));
  suffixIconLabel_->setProperty("adqt.semanticSlot", QStringLiteral("suffix"));
  suffixIconLabel_->setAlignment(Qt::AlignCenter);
  suffixIconLabel_->hide();
  suffixIconLabel_->installEventFilter(this);

  feedbackIconLabel_ = new QLabel(this);
  feedbackIconLabel_->setObjectName(QStringLiteral("ad-input-feedback-icon"));
  feedbackIconLabel_->setProperty("adqt.semanticSlot", QStringLiteral("suffix"));
  feedbackIconLabel_->setAlignment(Qt::AlignCenter);
  feedbackIconLabel_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
  feedbackIconLabel_->hide();

  clearButton_ = new InputIconButton(this);
  clearButton_->setObjectName(QStringLiteral("ad-input-clear"));
  clearButton_->setFocusPolicy(Qt::NoFocus);
  clearButton_->setAutoRaise(true);
  clearButton_->hide();
  clearButton_->installEventFilter(this);

  suffixActionButton_ = new InputIconButton(this);
  suffixActionButton_->setObjectName(QStringLiteral("ad-input-suffix-action"));
  suffixActionButton_->setFocusPolicy(Qt::NoFocus);
  suffixActionButton_->setAutoRaise(true);
  suffixActionButton_->hide();
  suffixActionButton_->installEventFilter(this);

  countLabel_ = new QLabel(this);
  countLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  countLabel_->hide();
  countLabel_->installEventFilter(this);

  connect(this, &QLineEdit::textChanged, this, [this](const QString&) {
    if (!internalTextUpdate_) {
      const int cursor = cursorPosition();
      const QString current = text();
      const QString next = normalizedText(current);
      if (next != current) {
        internalTextUpdate_ = true;
        QLineEdit::setText(next);
        setCursorPosition(std::min(cursor, static_cast<int>(next.size())));
        internalTextUpdate_ = false;
      }
    }

    updateCountLabel();
    updateClearButton();
    refreshVisualState(false);
  });

  connect(clearButton_, &QToolButton::clicked, this, [this]() {
    if (text().isEmpty()) {
      return;
    }
    internalTextUpdate_ = true;
    QLineEdit::clear();
    internalTextUpdate_ = false;
    emit cleared();
    focusEditor(FocusSelection::Start, false);
  });

  connect(suffixActionButton_, &QToolButton::clicked, this, [this]() {
    if (!isEnabled()) {
      return;
    }
    if (!hasFocus()) {
      focusEditor(FocusSelection::Preserve, false);
    }
  });

  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { refreshVisualState(true); });

  refreshVisualState(true);
}

AdLineEdit::~AdLineEdit() { stopInteractionFocusForOwner(this); }

AdLineEdit::ControlSize AdLineEdit::controlSize() const { return controlSize_; }

void AdLineEdit::setControlSize(ControlSize value) {
  if (controlSize_ == value) {
    return;
  }
  controlSize_ = value;
  refreshVisualState(true);
  emit controlSizeChanged(controlSize_);
}

AdLineEdit::Variant AdLineEdit::variant() const { return variant_; }

void AdLineEdit::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  refreshVisualState(true);
  emit variantChanged(variant_);
}

AdLineEdit::Status AdLineEdit::status() const { return status_; }

void AdLineEdit::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  refreshVisualState(false);
  emit statusChanged(status_);
}

bool AdLineEdit::allowClear() const { return allowClear_; }

void AdLineEdit::setAllowClear(bool value) {
  if (allowClear_ == value) {
    return;
  }
  allowClear_ = value;
  refreshVisualState(true);
  emit allowClearChanged(allowClear_);
}

void AdLineEdit::setPlaceholderText(const QString& value) {
  if (placeholderText() == value) {
    return;
  }
  QLineEdit::setPlaceholderText(value);
  refreshVisualState(true);
  emit placeholderTextChanged(value);
}

void AdLineEdit::setText(const QString& value) {
  QString next = value;
  if (maxLength_ > 0 && next.size() > maxLength_) {
    next = next.left(maxLength_);
  }
  next = normalizedText(next);

  if (text() == next) {
    return;
  }

  internalTextUpdate_ = true;
  QLineEdit::setText(next);
  internalTextUpdate_ = false;
}

void AdLineEdit::clear() { QLineEdit::clear(); }

void AdLineEdit::setMaxLength(int value) {
  const int normalized = value < 0 ? -1 : value;
  if (maxLength_ == normalized) {
    return;
  }
  maxLength_ = normalized;
  QLineEdit::setMaxLength(maxLength_ > 0 ? maxLength_ : std::numeric_limits<int>::max());
  if (maxLength_ > 0 && text().size() > maxLength_) {
    setText(text().left(maxLength_));
  }
  refreshVisualState(true);
  emit maxLengthChanged(maxLength_);
}

void AdLineEdit::setReadOnly(bool value) {
  if (isReadOnly() == value) {
    return;
  }
  QLineEdit::setReadOnly(value);
  refreshVisualState(false);
  emit readOnlyChanged(value);
}

void AdLineEdit::setEchoMode(QLineEdit::EchoMode value) {
  if (echoMode() == value) {
    return;
  }
  QLineEdit::setEchoMode(value);
  refreshVisualState(false);
  emit echoModeChanged(value);
}

QString AdLineEdit::prefixText() const { return prefixText_; }

void AdLineEdit::setPrefixText(const QString& value) {
  if (prefixText_ == value) {
    return;
  }
  prefixText_ = value;
  refreshVisualState(true);
  emit prefixTextChanged(prefixText_);
}

QString AdLineEdit::suffixText() const { return suffixText_; }

void AdLineEdit::setSuffixText(const QString& value) {
  if (suffixText_ == value) {
    return;
  }
  suffixText_ = value;
  refreshVisualState(true);
  emit suffixTextChanged(suffixText_);
}

bool AdLineEdit::countVisible() const { return countVisible_; }

void AdLineEdit::setCountVisible(bool value) {
  if (countVisible_ == value) {
    return;
  }
  countVisible_ = value;
  refreshVisualState(true);
  emit countVisibleChanged(countVisible_);
}

int AdLineEdit::maximumCharacterCount() const { return maximumCharacterCount_; }

void AdLineEdit::setMaximumCharacterCount(int value) {
  const int normalized = value < 0 ? -1 : value;
  if (maximumCharacterCount_ == normalized) {
    return;
  }
  maximumCharacterCount_ = normalized;
  refreshVisualState(true);
  emit maximumCharacterCountChanged(maximumCharacterCount_);
}

bool AdLineEdit::joinedLeft() const { return joinedLeft_; }

void AdLineEdit::setJoinedLeft(bool value) {
  if (joinedLeft_ == value) {
    return;
  }
  joinedLeft_ = value;
  updateJoinedZOrder();
  refreshVisualState(false);
  emit joinedLeftChanged(joinedLeft_);
}

bool AdLineEdit::joinedRight() const { return joinedRight_; }

void AdLineEdit::setJoinedRight(bool value) {
  if (joinedRight_ == value) {
    return;
  }
  joinedRight_ = value;
  updateJoinedZOrder();
  refreshVisualState(false);
  emit joinedRightChanged(joinedRight_);
}

adqt::icons::IconRef AdLineEdit::prefixIconRef() const { return prefixIconRef_; }

void AdLineEdit::setPrefixIconRef(const adqt::icons::IconRef& token) {
  if (detail::input_internal::iconRefsEqual(prefixIconRef_, token)) {
    return;
  }
  prefixIconRef_ = token;
  refreshVisualState(true);
  emit prefixIconRefChanged(prefixIconRef_);
}

adqt::icons::IconRef AdLineEdit::suffixIconRef() const { return suffixIconRef_; }

void AdLineEdit::setSuffixIconRef(const adqt::icons::IconRef& token) {
  if (detail::input_internal::iconRefsEqual(suffixIconRef_, token)) {
    return;
  }
  suffixIconRef_ = token;
  refreshVisualState(true);
  emit suffixIconRefChanged(suffixIconRef_);
}

adqt::icons::IconRef AdLineEdit::feedbackIconRef() const { return feedbackIconRef_; }

void AdLineEdit::setFeedbackIconRef(const adqt::icons::IconRef& token) {
  if (detail::input_internal::iconRefsEqual(feedbackIconRef_, token)) {
    return;
  }
  feedbackIconRef_ = token;
  updateFeedbackSpinnerState();
  refreshVisualState(true);
  emit feedbackIconRefChanged(feedbackIconRef_);
}

adqt::icons::IconRef AdLineEdit::clearIconRef() const { return clearIconRef_; }

void AdLineEdit::setClearIconRef(const adqt::icons::IconRef& token) {
  if (detail::input_internal::iconRefsEqual(clearIconRef_, token)) {
    return;
  }
  clearIconRef_ = token;
  refreshVisualState(false);
  emit clearIconRefChanged(clearIconRef_);
}

AdInputTextPolicy* AdLineEdit::textPolicy() const { return textPolicy_; }

void AdLineEdit::setTextPolicy(AdInputTextPolicy* value) {
  if (textPolicy_ == value) {
    return;
  }

  textPolicy_ = value;
  emit textPolicyChanged(textPolicy_);

  const QString current = text();
  if (normalizedText(current) != current) {
    setText(current);
    return;
  }
  refreshVisualState(true);
}

QSize AdLineEdit::sizeHint() const {
  const InputVisualStyle style = resolvedStyle();
  QFontMetrics fm(style.metrics.font);
  const QMargins contentInsets = detail::input_internal::textControlContentMargins(style);
  const int contentWidth =
      std::max(horizontalTextWidth(fm, placeholderText()), horizontalTextWidth(fm, text()));
  const int minimumWidth = std::max(1, qRound(160 * controlScale_.logicalScale));
  const int baseWidth =
      std::max(minimumWidth, contentInsets.left() + contentInsets.right() + contentWidth +
                                 accessoryWidthHint(style) + style.metrics.height);
  return QSize(baseWidth, style.metrics.height);
}

QSize AdLineEdit::minimumSizeHint() const {
  const QSize hint = sizeHint();
  if (property("ad-flex-min-width-zero").toBool()) {
    return QSize(0, hint.height());
  }
  return QSize(std::min(std::max(1, qRound(96 * controlScale_.logicalScale)), hint.width()),
               hint.height());
}

void AdLineEdit::prepareControlScale(const AdControlScaleContext& context) { Q_UNUSED(context) }

void AdLineEdit::commitControlScale(const AdControlScaleContext& context) {
  controlScale_ = context;
  refreshVisualState(true);
}

void AdLineEdit::focusEditor(FocusSelection selection, bool preventScroll) {
  const QVector<detail::input_internal::PreservedScrollPosition> preserved =
      preventScroll ? detail::input_internal::captureAncestorScrollPositions(this)
                    : QVector<detail::input_internal::PreservedScrollPosition>();

  setFocus(Qt::OtherFocusReason);
  updateJoinedZOrder();

  const QString currentText = text();
  if (selection == FocusSelection::Start) {
    setCursorPosition(0);
  } else if (selection == FocusSelection::End) {
    setCursorPosition(static_cast<int>(currentText.size()));
  } else if (selection == FocusSelection::SelectAll) {
    selectAll();
  }

  if (!preserved.isEmpty()) {
    detail::input_internal::restoreAncestorScrollPositions(preserved);
    detail::input_internal::restoreAncestorScrollPositionsDeferred(this, preserved);
  }

  refreshVisualState(false);
}

void AdLineEdit::blurInput() { clearFocus(); }

void AdLineEdit::setTrailingActionLeading(bool value) {
  if (suffixActionLeading_ == value) {
    return;
  }
  suffixActionLeading_ = value;
  refreshVisualState(true);
}

void AdLineEdit::setTrailingActionVisible(bool value) {
  if (trailingActionVisible_ == value) {
    return;
  }
  trailingActionVisible_ = value;
  refreshVisualState(true);
}

void AdLineEdit::setTrailingActionIconRef(const adqt::icons::IconRef& token) {
  if (detail::input_internal::iconRefsEqual(trailingActionIconRef_, token)) {
    return;
  }
  trailingActionIconRef_ = token;
  refreshVisualState(true);
}

void AdLineEdit::setTrailingActionAccessibleName(const QString& value) {
  if (trailingActionAccessibleName_ == value) {
    return;
  }
  trailingActionAccessibleName_ = value;
  syncAccessibleState();
}

void AdLineEdit::setClearOverlaysTrailingAction(bool value) {
  if (clearOverlaysTrailingAction_ == value) {
    return;
  }
  clearOverlaysTrailingAction_ = value;
  refreshVisualState(true);
}

QToolButton* AdLineEdit::trailingActionButton() const { return suffixActionButton_; }

bool AdLineEdit::eventFilter(QObject* watched, QEvent* event) {
  if (!event) {
    return QLineEdit::eventFilter(watched, event);
  }

  if (watched == prefixIconLabel_ || watched == prefixLabel_ || watched == suffixLabel_ ||
      watched == suffixIconLabel_ || watched == feedbackIconLabel_ || watched == countLabel_) {
    if (detail::input_internal::isLeftMouseActivationEvent(event)) {
      if (isEnabled()) {
        focusEditor(FocusSelection::Preserve, false);
      }
      return true;
    }
  } else if (watched == clearButton_ || watched == suffixActionButton_) {
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
        event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
      refreshVisualState(false);
    }
  }

  return QLineEdit::eventFilter(watched, event);
}

bool AdLineEdit::event(QEvent* event) {
  const bool handled = QLineEdit::event(event);
  if (!event || event->type() != QEvent::DynamicPropertyChange) {
    return handled;
  }

  const auto* propertyEvent = static_cast<QDynamicPropertyChangeEvent*>(event);
  const QByteArray propertyName = propertyEvent->propertyName();
  if (isDynamicStyleProperty(propertyName)) {
    refreshVisualState(true);
  }

  return handled;
}

void AdLineEdit::paintEvent(QPaintEvent* event) {
  const InputVisualStyle style = resolvedStyle();
  InputFramePaintStyle frameStyle;
  frameStyle.background = resolvedBackgroundColor(style, focused_, hovered_);
  frameStyle.border = QColor(0, 0, 0, 0);
  frameStyle.borderWidth = std::max<qreal>(0.0, style.metrics.borderWidth);
  frameStyle.underlined = style.underlined;
  const qreal radius = style.underlined ? 0.0 : std::max<qreal>(0.0, style.metrics.borderRadius);
  frameStyle.topLeftRadius = joinedLeft_ ? 0.0 : radius;
  frameStyle.topRightRadius = joinedRight_ ? 0.0 : radius;
  frameStyle.bottomRightRadius = joinedRight_ ? 0.0 : radius;
  frameStyle.bottomLeftRadius = joinedLeft_ ? 0.0 : radius;
  frameStyle.joinedLeft = joinedLeft_;
  frameStyle.joinedRight = joinedRight_;

  {
    QPainter painter(this);
    detail::input_internal::paintInputFrame(&painter, rect(), frameStyle);
  }

  QLineEdit::paintEvent(event);

  frameStyle.background = QColor(0, 0, 0, 0);
  frameStyle.border = resolvedBorderColor(style, focused_, hovered_);
  QPainter painter(this);
  detail::input_internal::paintInputFrame(&painter, rect(), frameStyle);
}

void AdLineEdit::resizeEvent(QResizeEvent* event) {
  QLineEdit::resizeEvent(event);
  updateAccessoryGeometry();
  updateInteractionFocusOverlay();
}

void AdLineEdit::changeEvent(QEvent* event) {
  QLineEdit::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::LanguageChange || event->type() == QEvent::EnabledChange ||
      event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::FontChange ||
      event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::StyleChange) {
    refreshVisualState(true);
  }
}

void AdLineEdit::moveEvent(QMoveEvent* event) {
  QLineEdit::moveEvent(event);
  updateInteractionFocusOverlay();
}

void AdLineEdit::showEvent(QShowEvent* event) {
  QLineEdit::showEvent(event);
  // Rebuild affix pixmaps after the widget is attached to its final screen so
  // prefix/suffix icons use the correct DPR and geometry on first paint.
  updateFeedbackSpinnerState();
  refreshVisualState(false);
}

void AdLineEdit::hideEvent(QHideEvent* event) {
  QLineEdit::hideEvent(event);
  updateFeedbackSpinnerState();
  updateInteractionFocusOverlay();
}

void AdLineEdit::enterEvent(QEnterEvent* event) {
  hovered_ = true;
  updateJoinedZOrder();
  refreshVisualState(false);
  QLineEdit::enterEvent(event);
}

void AdLineEdit::leaveEvent(QEvent* event) {
  hovered_ = false;
  updateJoinedZOrder();
  refreshVisualState(false);
  QLineEdit::leaveEvent(event);
}

void AdLineEdit::focusInEvent(QFocusEvent* event) {
  focused_ = true;
  updateJoinedZOrder();
  refreshVisualState(false);
  QLineEdit::focusInEvent(event);
}

void AdLineEdit::focusOutEvent(QFocusEvent* event) {
  focused_ = false;
  updateJoinedZOrder();
  refreshVisualState(false);
  QLineEdit::focusOutEvent(event);
}

bool AdLineEdit::hasClearValue() const {
  return allowClear_ && !isReadOnly() && isEnabled() && !text().isEmpty();
}

bool AdLineEdit::clearButtonWantsVisible() const {
  if (!hasClearValue()) {
    return false;
  }
  if (!clearOverlaysTrailingAction_) {
    return true;
  }
  const bool childHovered = (clearButton_ && clearButton_->underMouse()) ||
                            (suffixActionButton_ && suffixActionButton_->underMouse()) ||
                            (suffixLabel_ && suffixLabel_->underMouse()) ||
                            (suffixIconLabel_ && suffixIconLabel_->underMouse()) ||
                            (feedbackIconLabel_ && feedbackIconLabel_->underMouse());
  return hovered_ || childHovered;
}

bool AdLineEdit::clearButtonReservesWidth() const {
  const bool hasTrailingAction =
      trailingActionVisible_ && adqt::icons::isValid(trailingActionIconRef_);
  const bool hasPassiveSuffix = adqt::icons::isValid(suffixIconRef_) ||
                                adqt::icons::isValid(feedbackIconRef_) || !suffixText_.isEmpty();
  return hasClearValue() &&
         (!clearOverlaysTrailingAction_ || (!hasTrailingAction && !hasPassiveSuffix));
}

void AdLineEdit::updateAccessoryVisibility() {
  if (!prefixLabel_ || !prefixIconLabel_ || !suffixLabel_ || !suffixIconLabel_ || !clearButton_ ||
      !suffixActionButton_ || !feedbackIconLabel_ || !countLabel_) {
    return;
  }

  prefixLabel_->setVisible(!prefixText_.isEmpty());
  prefixIconLabel_->setVisible(adqt::icons::isValid(prefixIconRef_));
  suffixLabel_->setVisible(!suffixText_.isEmpty());
  suffixIconLabel_->setVisible(adqt::icons::isValid(suffixIconRef_));
  feedbackIconLabel_->setVisible(adqt::icons::isValid(feedbackIconRef_));

  clearButton_->setVisible(clearButtonWantsVisible());

  const bool showSuffixAction =
      trailingActionVisible_ && adqt::icons::isValid(trailingActionIconRef_);
  suffixActionButton_->setVisible(showSuffixAction);

  countLabel_->setVisible(countVisible_);
}

void AdLineEdit::updateAccessoryGeometry() {
  const InputVisualStyle style = resolvedStyle();
  const QMargins contentInsets = detail::input_internal::textControlContentMargins(style);
  const int groupGap = affixPadding(style);
  const int itemGap = affixItemGap(style);
  const int contentTop = rect().top();
  const int contentHeight = rect().height();

  // `QRect::center()` rounds down for even heights, which lifts affix icons by 1 px.
  const auto centeredRect = [contentTop, contentHeight](int x, int width, int height) {
    return QRect(x, contentTop + (contentHeight - height) / 2, width, height);
  };

  int left = std::max(0, contentInsets.left());
  bool hasLeadingAffix = false;

  const auto placeLeading = [&centeredRect, &left, &hasLeadingAffix, itemGap](QWidget* widget,
                                                                              const QSize& size) {
    if (hasLeadingAffix) {
      left += itemGap;
    }
    widget->setGeometry(centeredRect(left, size.width(), size.height()));
    left += size.width();
    hasLeadingAffix = true;
  };

  if (prefixIconLabel_ && prefixIconLabel_->isVisible()) {
    const QSize size = prefixIconLabel_->sizeHint().expandedTo(prefixIconLabel_->minimumSize());
    placeLeading(prefixIconLabel_, size);
  }

  if (prefixLabel_ && prefixLabel_->isVisible()) {
    const QSize size = prefixLabel_->sizeHint();
    placeLeading(prefixLabel_, size);
  }

  if (hasLeadingAffix) {
    left += groupGap;
  }

  int right = std::max(0, width() - std::max(0, contentInsets.right()));
  const int trailingRightEdge = right;
  bool hasTrailingAffix = false;

  const auto placeTrailing = [&centeredRect, &right, &hasTrailingAffix](
                                 QWidget* widget, const QSize& size, int gapBefore) {
    if (hasTrailingAffix && gapBefore > 0) {
      right -= gapBefore;
    }
    widget->setGeometry(centeredRect(right - size.width(), size.width(), size.height()));
    right -= size.width();
    hasTrailingAffix = true;
  };

  const auto placeSuffixAction = [this, &placeTrailing]() {
    if (!suffixActionButton_ || !suffixActionButton_->isVisible()) {
      return false;
    }
    const QSize size =
        suffixActionButton_->sizeHint().expandedTo(suffixActionButton_->minimumSize());
    placeTrailing(suffixActionButton_, size, 0);
    return true;
  };

  const auto placePassiveSuffix = [this, &placeTrailing, itemGap](int firstGap) {
    bool placed = false;
    if (feedbackIconLabel_ && feedbackIconLabel_->isVisible()) {
      const QSize size =
          feedbackIconLabel_->sizeHint().expandedTo(feedbackIconLabel_->minimumSize());
      placeTrailing(feedbackIconLabel_, size, firstGap);
      placed = true;
      firstGap = itemGap;
    }

    if (suffixIconLabel_ && suffixIconLabel_->isVisible()) {
      const QSize size = suffixIconLabel_->sizeHint().expandedTo(suffixIconLabel_->minimumSize());
      placeTrailing(suffixIconLabel_, size, firstGap);
      placed = true;
      firstGap = itemGap;
    }

    if (suffixLabel_ && suffixLabel_->isVisible()) {
      const QSize size = suffixLabel_->sizeHint();
      placeTrailing(suffixLabel_, size, firstGap);
      placed = true;
    }
    return placed;
  };

  if (suffixActionLeading_) {
    const bool passivePlaced = placePassiveSuffix(0);
    if (suffixActionButton_ && suffixActionButton_->isVisible()) {
      const QSize size =
          suffixActionButton_->sizeHint().expandedTo(suffixActionButton_->minimumSize());
      placeTrailing(suffixActionButton_, size, passivePlaced ? itemGap : 0);
    }
  } else {
    const bool actionPlaced = placeSuffixAction();
    placePassiveSuffix(actionPlaced ? itemGap : 0);
  }

  if (clearButton_ && clearButton_->isVisible() && clearOverlaysTrailingAction_) {
    const QSize size = clearButton_->sizeHint().expandedTo(clearButton_->minimumSize());
    clearButton_->setGeometry(
        centeredRect(trailingRightEdge - size.width(), size.width(), size.height()));
    clearButton_->raise();
    if (clearButtonReservesWidth()) {
      right = std::min(right, trailingRightEdge - size.width());
      hasTrailingAffix = true;
    }
  } else if (clearButton_ && clearButtonReservesWidth()) {
    const QSize size = clearButton_->sizeHint().expandedTo(clearButton_->minimumSize());
    placeTrailing(clearButton_, size, groupGap);
  }

  if (countLabel_ && countLabel_->isVisible()) {
    const QSize size = countLabel_->sizeHint();
    placeTrailing(countLabel_, size, groupGap);
  }

  setTextMargins(std::max(0, left), std::max(0, contentInsets.top()), std::max(0, width() - right),
                 std::max(0, contentInsets.bottom()));
}

void AdLineEdit::updateTextMargins() { updateAccessoryGeometry(); }

void AdLineEdit::updateCountLabel() {
  if (!countLabel_) {
    return;
  }

  const int count = effectiveCount(text());
  const int maximum = effectiveCountMax();
  countLabel_->setText(countLabelText(text(), count, maximum));
  countLabel_->setVisible(countVisible_);
}

void AdLineEdit::updateClearButton() {
  if (!clearButton_) {
    return;
  }

  const InputVisualStyle style = resolvedStyle();
  const int iconSide =
      std::max(scaledMinimum(10, controlScale_.logicalScale), style.metrics.clearIconSize);
  auto* clearIconButton = static_cast<InputIconButton*>(clearButton_);
  clearIconButton->setSlotSize(QSize(iconSide, iconSide));
  clearIconButton->setContentAlignment(Qt::AlignCenter);
  const adqt::icons::IconRef icon =
      adqt::icons::isValid(clearIconRef_) ? clearIconRef_ : filled_icons::CloseCircle();
  setButtonIcon(clearButton_, icon, style.clearColor, style.clearHoverColor, style.clearActiveColor,
                iconSide, devicePixelRatioF());
  clearButton_->setEnabled(isEnabled() && !isReadOnly());
}

void AdLineEdit::updatePrefixVisual() {
  if (!prefixLabel_ || !prefixIconLabel_) {
    return;
  }

  const InputVisualStyle style = resolvedStyle();
  prefixLabel_->setText(prefixText_);
  prefixLabel_->setFont(style.metrics.font);
  setLabelTextColor(prefixLabel_, isEnabled() ? style.prefixColor : style.disabledTextColor);
  const int iconSide =
      std::max(scaledMinimum(10, controlScale_.logicalScale), style.metrics.affixIconSize);
  setLabelIcon(prefixIconLabel_, prefixIconRef_,
               isEnabled() ? style.prefixColor : style.disabledTextColor, iconSide,
               devicePixelRatioF());
}

void AdLineEdit::updateSuffixVisual() {
  if (!suffixLabel_ || !suffixIconLabel_ || !suffixActionButton_ || !feedbackIconLabel_) {
    return;
  }

  const InputVisualStyle style = resolvedStyle();
  const bool suppressTrailing = clearOverlaysTrailingAction_ && clearButtonWantsVisible();
  const auto transparent = [](QColor color) {
    if (!color.isValid()) {
      color = QColor(0, 0, 0);
    }
    color.setAlpha(0);
    return color;
  };
  const QColor resolvedSuffixColor = isEnabled() ? style.suffixColor : style.disabledTextColor;
  const QColor suffixColor =
      suppressTrailing ? transparent(resolvedSuffixColor) : resolvedSuffixColor;
  ActionIconColors actionColors = suffixActionColors(style);
  if (suppressTrailing) {
    actionColors.normal = transparent(actionColors.normal);
    actionColors.hover = transparent(actionColors.hover);
    actionColors.pressed = transparent(actionColors.pressed);
  }
  suffixLabel_->setText(suffixText_);
  suffixLabel_->setFont(style.metrics.font);
  setLabelTextColor(suffixLabel_, suffixColor);

  const int iconSide =
      std::max(scaledMinimum(10, controlScale_.logicalScale), style.metrics.affixIconSize);
  feedbackIconLabel_->setFixedSize(iconSide, iconSide);
  const int actionSlotWidth = iconSide + affixPadding(style);
  const int actionSlotHeight = std::max(iconSide, style.metrics.height);
  auto* suffixActionIconButton = static_cast<InputIconButton*>(suffixActionButton_);
  suffixActionIconButton->setSlotSize(QSize(actionSlotWidth, actionSlotHeight));
  suffixActionIconButton->setContentAlignment(Qt::AlignRight | Qt::AlignVCenter);
  setLabelIcon(suffixIconLabel_, suffixIconRef_, suffixColor, iconSide, devicePixelRatioF());
  if (adqt::icons::isValid(feedbackIconRef_)) {
    adqt::icons::IconRef feedbackIcon = feedbackIconRef_;
    if (suppressTrailing) {
      feedbackIcon = feedbackIcon.withColors(
          feedbackIcon.colors().withPrimary(transparent(resolvedSuffixColor)));
    } else if (!feedbackIcon.colors().primarySlot()) {
      feedbackIcon = feedbackIcon.withColors(feedbackIcon.colors().withPrimary(suffixColor));
    }
    const QPixmap pixmap = renderIconPixmap(feedbackIcon, iconSide, devicePixelRatioF(),
                                            isLoadingIcon(feedbackIcon) ? sharedSpinnerAngle() : 0);
    feedbackIconLabel_->setPixmap(pixmap);
    feedbackIconLabel_->setFixedSize(iconSide, iconSide);
    feedbackIconLabel_->setVisible(!pixmap.isNull());
  } else {
    feedbackIconLabel_->clear();
    feedbackIconLabel_->hide();
  }
  setButtonIcon(suffixActionButton_, trailingActionIconRef_, actionColors.normal,
                actionColors.hover, actionColors.pressed, iconSide, devicePixelRatioF());
  suffixActionButton_->setEnabled(isEnabled() && !suppressTrailing);
}

void AdLineEdit::updateFeedbackSpinnerState() {
  const bool spinning =
      isVisible() && adqt::icons::isValid(feedbackIconRef_) && isLoadingIcon(feedbackIconRef_);
  if (spinning && !feedbackSpinnerSubscribed_) {
    detail::setFrameSubscription(this, QString::fromLatin1(kFeedbackSpinnerFrameKey), true,
                                 [this](qint64, qint64) {
                                   if (isVisible() && isLoadingIcon(feedbackIconRef_)) {
                                     updateSuffixVisual();
                                   }
                                 });
    feedbackSpinnerSubscribed_ = true;
  } else if (!spinning && feedbackSpinnerSubscribed_) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kFeedbackSpinnerFrameKey));
    feedbackSpinnerSubscribed_ = false;
  }
}

void AdLineEdit::applyEditorPalette() {
  const InputVisualStyle style = resolvedStyle();
  const QColor transparent(0, 0, 0, 0);
  const QColor textColor = isEnabled() ? style.selectorTextColor : style.disabledTextColor;
  const QColor placeholderColor = isEnabled() ? style.placeholderColor : style.disabledTextColor;

  QPalette palette = this->palette();
  palette.setColor(QPalette::Base, transparent);
  palette.setColor(QPalette::Window, transparent);
  palette.setColor(QPalette::Text, textColor);
  palette.setColor(QPalette::Disabled, QPalette::Base, transparent);
  palette.setColor(QPalette::Disabled, QPalette::Window, transparent);
  palette.setColor(QPalette::Disabled, QPalette::Text, textColor);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
  palette.setColor(QPalette::PlaceholderText, placeholderColor);
  palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, placeholderColor);
#endif
  setPalette(palette);
}

void AdLineEdit::refreshVisualState(bool geometryChanged) {
  const InputVisualStyle style = resolvedStyle();
  if (font() != style.metrics.font) {
    QSignalBlocker blocker(this);
    QLineEdit::setFont(style.metrics.font);
  }

  updateCountLabel();
  updatePrefixVisual();
  updateSuffixVisual();
  updateClearButton();
  applyEditorPalette();
  if (countLabel_) {
    countLabel_->setFont(style.metrics.font);
    setLabelTextColor(countLabel_, isEnabled() ? style.countColor : style.disabledTextColor);
  }
  updateAccessoryVisibility();
  updateAccessoryGeometry();
  updateCursorForRole();
  syncAccessibleState();
  updateInteractionFocusOverlay();
  update();
  if (geometryChanged) {
    updateGeometry();
  }
}

void AdLineEdit::updateInteractionFocusOverlay() {
  if (!focused_) {
    stopInteractionFocusForOwner(this);
    return;
  }

  detail::input_internal::updateInputFocusOverlay(this, rect(), resolvedStyle(), joinedLeft_,
                                                  joinedRight_);
}

AdLineEdit::Status AdLineEdit::effectiveStatus() const {
  if (status_ != Status::None) {
    return status_;
  }
  const int maximum = effectiveCountMax();
  if (maximum > 0 && effectiveCount(text()) > maximum) {
    return Status::Warning;
  }
  return Status::None;
}

InputVisualStyle AdLineEdit::resolvedStyle() const {
  detail::InputStyleInput input;
  input.controlSize = controlSize_;
  input.variant = variant_;
  input.status = effectiveStatus();
  input.disabled = !isEnabled();
  input.focused = focused_;
  input.hovered = hovered_;
  input.baseFont = font();
  InputVisualStyle style =
      applyDynamicOverrides(adqt::widgets::detail::resolveInputVisualStyle(
                                input, adqt::theme::ThemeManager::instance().resolve(this)),
                            this);
  applyControlScale(&style, controlScale_.logicalScale);
  return style;
}

void AdLineEdit::updateCursorForRole() {
  const Qt::CursorShape containerCursor = isEnabled() ? Qt::IBeamCursor : Qt::ForbiddenCursor;
  setCursor(containerCursor);

  const Qt::CursorShape passiveCursor = isEnabled() ? Qt::ArrowCursor : Qt::ForbiddenCursor;
  if (prefixLabel_) {
    prefixLabel_->setCursor(passiveCursor);
  }
  if (prefixIconLabel_) {
    prefixIconLabel_->setCursor(passiveCursor);
  }
  if (suffixLabel_) {
    suffixLabel_->setCursor(passiveCursor);
  }
  if (suffixIconLabel_) {
    suffixIconLabel_->setCursor(passiveCursor);
  }
  if (feedbackIconLabel_) {
    feedbackIconLabel_->setCursor(passiveCursor);
  }
  if (countLabel_) {
    countLabel_->setCursor(passiveCursor);
  }
  if (clearButton_) {
    clearButton_->setCursor((clearButton_->isVisible() && clearButton_->isEnabled())
                                ? Qt::PointingHandCursor
                                : passiveCursor);
  }
  if (suffixActionButton_) {
    suffixActionButton_->setCursor(
        (suffixActionButton_->isVisible() && suffixActionButton_->isEnabled())
            ? Qt::PointingHandCursor
            : passiveCursor);
  }
}

void AdLineEdit::updateJoinedZOrder() {
  if (!(joinedLeft_ || joinedRight_)) {
    return;
  }

  if (focused_ || hovered_) {
    raise();
    return;
  }

  lower();
}

int AdLineEdit::effectiveCount(const QString& value) const {
  if (textPolicy_) {
    return std::max(0, textPolicy_->characterCount(value));
  }
  return static_cast<int>(value.size());
}

int AdLineEdit::effectiveCountMax() const {
  if (maximumCharacterCount_ > 0) {
    return maximumCharacterCount_;
  }
  if (maxLength_ > 0) {
    return maxLength_;
  }
  return -1;
}

QString AdLineEdit::normalizedText(const QString& value) const {
  if (!textPolicy_) {
    return value;
  }
  return textPolicy_->normalizeText(value, effectiveCountMax());
}

QString AdLineEdit::countLabelText(const QString& value, int count, int maximum) const {
  if (textPolicy_) {
    return textPolicy_->formatCountLabel(value, count, maximum);
  }
  if (maximum > 0) {
    return QStringLiteral("%1 / %2").arg(count).arg(maximum);
  }
  return QString::number(count);
}

int AdLineEdit::accessoryWidthHint(const InputVisualStyle& style) const {
  const QFontMetrics fm(style.metrics.font);
  const int iconSide =
      std::max(scaledMinimum(10, controlScale_.logicalScale), style.metrics.affixIconSize);
  const int groupGap = affixPadding(style);
  const int itemGap = affixItemGap(style);

  int widthHint = 0;
  int leadingWidth = 0;
  bool hasLeading = false;
  if (adqt::icons::isValid(prefixIconRef_)) {
    leadingWidth += iconSide;
    hasLeading = true;
  }
  if (!prefixText_.isEmpty()) {
    if (hasLeading) {
      leadingWidth += itemGap;
    }
    leadingWidth += horizontalTextWidth(fm, prefixText_);
    hasLeading = true;
  }
  if (hasLeading) {
    widthHint += leadingWidth + groupGap;
  }

  int trailingWidth = 0;
  bool hasTrailing = false;
  if (countVisible_) {
    const QString labelText = countLabelText(text(), effectiveCount(text()), effectiveCountMax());
    trailingWidth += horizontalTextWidth(fm, labelText);
    hasTrailing = true;
  }

  int passiveSuffixWidth = 0;
  bool hasPassiveSuffix = false;
  const auto addPassiveSuffixWidth = [&passiveSuffixWidth, &hasPassiveSuffix, itemGap](int width) {
    if (width <= 0) {
      return;
    }
    if (hasPassiveSuffix) {
      passiveSuffixWidth += itemGap;
    }
    passiveSuffixWidth += width;
    hasPassiveSuffix = true;
  };
  addPassiveSuffixWidth(!suffixText_.isEmpty() ? horizontalTextWidth(fm, suffixText_) : 0);
  addPassiveSuffixWidth(adqt::icons::isValid(suffixIconRef_) ? iconSide : 0);
  addPassiveSuffixWidth(adqt::icons::isValid(feedbackIconRef_) ? iconSide : 0);
  const int actionWidth = trailingActionVisible_ && adqt::icons::isValid(trailingActionIconRef_)
                              ? iconSide + affixPadding(style)
                              : 0;
  const bool clearConsumesWidth =
      allowClear_ && !isReadOnly() &&
      (!clearOverlaysTrailingAction_ || (passiveSuffixWidth <= 0 && actionWidth <= 0));
  if (clearConsumesWidth) {
    if (hasTrailing) {
      trailingWidth += groupGap;
    }
    trailingWidth +=
        std::max(scaledMinimum(10, controlScale_.logicalScale), style.metrics.clearIconSize);
    hasTrailing = true;
  }

  if (passiveSuffixWidth > 0 || actionWidth > 0) {
    if (hasTrailing) {
      trailingWidth += groupGap;
    }
    if (suffixActionLeading_) {
      trailingWidth += passiveSuffixWidth;
      if (passiveSuffixWidth > 0 && actionWidth > 0) {
        trailingWidth += itemGap;
      }
      trailingWidth += actionWidth;
    } else {
      trailingWidth += actionWidth;
      if (actionWidth > 0 && passiveSuffixWidth > 0) {
        trailingWidth += itemGap;
      }
      trailingWidth += passiveSuffixWidth;
    }
  }

  if (hasTrailing || passiveSuffixWidth > 0 || actionWidth > 0) {
    widthHint += trailingWidth;
  }

  return widthHint;
}

void AdLineEdit::syncAccessibleState() {
  if (clearButton_) {
    clearButton_->setAccessibleName(tr("Clear input"));
    clearButton_->setAccessibleDescription(tr("Clear the current text"));
  }
  if (suffixActionButton_) {
    suffixActionButton_->setAccessibleName(trailingActionAccessibleName_.trimmed());
    suffixActionButton_->setAccessibleDescription(trailingActionAccessibleName_.trimmed());
  }
}

}  // namespace adqt::widgets
