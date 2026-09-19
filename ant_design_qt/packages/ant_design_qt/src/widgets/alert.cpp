#include "alert.h"

#include "alert_style.h"
#include "antd_icons.h"
#include "theme/theme.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QAbstractAnimation>
#include <QCloseEvent>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QParallelAnimationGroup>
#include <QPen>
#include <QPropertyAnimation>
#include <QShowEvent>
#include <QSpacerItem>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>

namespace adqt::widgets {

namespace {

QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft) {
  const qreal width = std::max(rect.width(), 0.0);
  const qreal height = std::max(rect.height(), 0.0);
  const qreal maxRadius = std::min(width, height) / 2.0;

  topLeft = std::clamp(topLeft, 0.0, maxRadius);
  topRight = std::clamp(topRight, 0.0, maxRadius);
  bottomRight = std::clamp(bottomRight, 0.0, maxRadius);
  bottomLeft = std::clamp(bottomLeft, 0.0, maxRadius);

  const qreal left = rect.left();
  const qreal top = rect.top();
  const qreal right = left + rect.width();
  const qreal bottom = top + rect.height();

  QPainterPath path;
  path.moveTo(left + topLeft, top);
  path.lineTo(right - topRight, top);
  if (topRight > 0.0) {
    path.quadTo(right, top, right, top + topRight);
  }
  path.lineTo(right, bottom - bottomRight);
  if (bottomRight > 0.0) {
    path.quadTo(right, bottom, right - bottomRight, bottom);
  }
  path.lineTo(left + bottomLeft, bottom);
  if (bottomLeft > 0.0) {
    path.quadTo(left, bottom, left, bottom - bottomLeft);
  }
  path.lineTo(left, top + topLeft);
  if (topLeft > 0.0) {
    path.quadTo(left, top, left + topLeft, top);
  }
  path.closeSubpath();
  return path;
}

void notifyAccessibilityEvent(QWidget* widget, QAccessible::Event eventType) {
  if (!widget) {
    return;
  }
  QAccessibleEvent event(widget, eventType);
  QAccessible::updateAccessibility(&event);
}

QString firstNonEmptyString(const std::initializer_list<QString>& values) {
  for (const QString& value : values) {
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty()) {
      return trimmed;
    }
  }
  return {};
}

QString accessibleInterfaceText(const QWidget* widget, QAccessible::Text textType) {
  if (!widget) {
    return {};
  }
  if (QAccessibleInterface* accessible =
          QAccessible::queryAccessibleInterface(const_cast<QWidget*>(widget))) {
    return accessible->text(textType).trimmed();
  }
  return {};
}

QString semanticTextFromWidget(const QWidget* widget, bool preferDescription) {
  if (!widget) {
    return {};
  }
  if (preferDescription) {
    return firstNonEmptyString({
        widget->accessibleDescription(),
        accessibleInterfaceText(widget, QAccessible::Description),
        widget->accessibleName(),
        accessibleInterfaceText(widget, QAccessible::Name),
    });
  }
  return firstNonEmptyString({
      widget->accessibleName(),
      accessibleInterfaceText(widget, QAccessible::Name),
      widget->accessibleDescription(),
      accessibleInterfaceText(widget, QAccessible::Description),
  });
}

QString resolvedAccessibleName(const AdAlert* alert) {
  if (!alert) {
    return {};
  }
  return firstNonEmptyString({
      alert->QWidget::accessibleName(),
      semanticTextFromWidget(alert->textWidget(), false),
      alert->text(),
  });
}

QString resolvedAccessibleDescription(const AdAlert* alert) {
  if (!alert) {
    return {};
  }
  return firstNonEmptyString({
      alert->QWidget::accessibleDescription(),
      semanticTextFromWidget(alert->informativeWidget(), true),
      alert->informativeText(),
  });
}

QAccessible::AnnouncementPoliteness announcementPolitenessForSeverity(AdAlert::Severity severity) {
  switch (severity) {
    case AdAlert::Severity::Success:
    case AdAlert::Severity::Info:
      return QAccessible::AnnouncementPoliteness::Polite;
    case AdAlert::Severity::Warning:
    case AdAlert::Severity::Error:
      return QAccessible::AnnouncementPoliteness::Assertive;
  }
  return QAccessible::AnnouncementPoliteness::Polite;
}

void notifyAccessibilityAnnouncement(QWidget* widget, const QString& message,
                                     QAccessible::AnnouncementPoliteness politeness) {
  if (!widget || message.trimmed().isEmpty()) {
    return;
  }
  QAccessibleAnnouncementEvent event(widget, message.trimmed());
  event.setPoliteness(politeness);
  QAccessible::updateAccessibility(&event);
}

adqt::icons::IconRef defaultSeverityIcon(AdAlert::Severity severity) {
  switch (severity) {
    case AdAlert::Severity::Success:
      return adqt::icons::antd::filled::CheckCircle();
    case AdAlert::Severity::Info:
      return adqt::icons::antd::filled::InfoCircle();
    case AdAlert::Severity::Warning:
      return adqt::icons::antd::filled::ExclamationCircle();
    case AdAlert::Severity::Error:
      return adqt::icons::antd::filled::CloseCircle();
  }
  return adqt::icons::antd::filled::InfoCircle();
}

bool usesCustomPalette(const QWidget* widget) {
  if (!widget || !widget->testAttribute(Qt::WA_SetPalette)) {
    return false;
  }
  const adqt::theme::ResolvedTheme resolved = adqt::theme::ThemeManager::instance().resolve(widget);
  return widget->palette() != resolved.palette;
}

class AlertCloseButton final : public QToolButton {
 public:
  explicit AlertCloseButton(QWidget* parent = nullptr) : QToolButton(parent) {
    setAttribute(Qt::WA_Hover, true);
  }

  void setVisuals(const adqt::icons::IconRef& iconRef, const QColor& normalIconColor,
                  const QColor& hoverIconColor, const QColor& hoverBackground,
                  const QColor& pressedBackground, const QColor& focusOutline,
                  int focusOutlineWidth, const QSize& iconSize) {
    iconRef_ = iconRef;
    normalIconColor_ = normalIconColor;
    hoverIconColor_ = hoverIconColor;
    hoverBackground_ = hoverBackground;
    pressedBackground_ = pressedBackground;
    focusOutline_ = focusOutline;
    focusOutlineWidth_ = std::max(0, focusOutlineWidth);
    iconSize_ = iconSize;
    update();
  }

 protected:
  void changeEvent(QEvent* event) override {
    QToolButton::changeEvent(event);
    if (event && event->type() == QEvent::EnabledChange) {
      update();
    }
  }

  void enterEvent(QEnterEvent* event) override {
    QToolButton::enterEvent(event);
    update();
  }

  void leaveEvent(QEvent* event) override {
    QToolButton::leaveEvent(event);
    update();
  }

  void focusInEvent(QFocusEvent* event) override {
    QToolButton::focusInEvent(event);
    update();
  }

  void focusOutEvent(QFocusEvent* event) override {
    QToolButton::focusOutEvent(event);
    update();
  }

  void mousePressEvent(QMouseEvent* event) override {
    QToolButton::mousePressEvent(event);
    update();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    QToolButton::mouseReleaseEvent(event);
    update();
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = rect();
    const qreal radius = std::max<qreal>(0.0, std::min(width(), height()) / 2.0);
    QColor background = QColor(Qt::transparent);
    if (isEnabled()) {
      if (isDown()) {
        background = pressedBackground_.isValid() ? pressedBackground_ : hoverBackground_;
      } else if (underMouse() || hasFocus()) {
        background = hoverBackground_;
      }
    }

    if (background.alpha() > 0) {
      painter.fillPath(roundedRectPath(bounds, radius, radius, radius, radius), background);
    }

    if (isEnabled() && hasFocus() && focusOutlineWidth_ > 0 && focusOutline_.isValid()) {
      const qreal inset = focusOutlineWidth_ / 2.0;
      const QRectF outlineRect = bounds.adjusted(inset, inset, -inset, -inset);
      const qreal outlineRadius = std::max<qreal>(0.0, radius - inset);
      painter.strokePath(
          roundedRectPath(outlineRect, outlineRadius, outlineRadius, outlineRadius, outlineRadius),
          QPen(focusOutline_, focusOutlineWidth_, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    }

    adqt::icons::IconRef token = iconRef_;
    const QColor iconColor =
        isEnabled() && (underMouse() || hasFocus() || isDown()) && hoverIconColor_.isValid()
            ? hoverIconColor_
            : normalIconColor_;
    if (!token.colors().primarySlot()) {
      token = token.withColors(token.colors().withPrimary(iconColor));
    }
    const qreal dpr = std::max(1.0, devicePixelRatioF());
    const QPixmap pixmap = adqt::icons::renderIconPixmap(token, {iconSize_, dpr});
    const QRect iconRect((width() - iconSize_.width()) / 2, (height() - iconSize_.height()) / 2,
                         iconSize_.width(), iconSize_.height());
    painter.drawPixmap(iconRect, pixmap);
  }

 private:
  adqt::icons::IconRef iconRef_;
  QColor normalIconColor_;
  QColor hoverIconColor_;
  QColor hoverBackground_;
  QColor pressedBackground_;
  QColor focusOutline_;
  int focusOutlineWidth_ = 0;
  QSize iconSize_;
};

class AdAlertAccessible final : public QAccessibleWidget {
 public:
  explicit AdAlertAccessible(AdAlert* alert)
      : QAccessibleWidget(alert, QAccessible::AlertMessage) {}

  QString text(QAccessible::Text t) const override {
    const auto* alert = qobject_cast<AdAlert*>(object());
    if (!alert) {
      return QAccessibleWidget::text(t);
    }

    switch (t) {
      case QAccessible::Name:
        return resolvedAccessibleName(alert);
      case QAccessible::Description:
        return resolvedAccessibleDescription(alert);
      default:
        return QAccessibleWidget::text(t);
    }
  }
};

QAccessibleInterface* alertAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* alert = qobject_cast<AdAlert*>(object)) {
    return new AdAlertAccessible(alert);
  }
  return nullptr;
}

void ensureAlertAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(alertAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

}  // namespace

class AdAlertPrivate {
  Q_DECLARE_PUBLIC(AdAlert)

 public:
  enum class HostedSlot : std::uint8_t {
    Leading,
    Text,
    Informative,
    Actions,
  };

  enum class CloseState : std::uint8_t {
    Idle,
    Requested,
    Animating,
    Finalizing,
  };

  struct DerivedState {
    AdAlert::Severity severity = AdAlert::Severity::Info;
    bool hasText = false;
    bool hasInformativeText = false;
    bool hasActions = false;
    bool iconVisible = false;
  };

  struct SectionSlot {
    QPointer<QWidget> widget;
    QPointer<QWidget> host;
    QPointer<QVBoxLayout> layout;
    QMetaObject::Connection destroyedConnection;
  };

  explicit AdAlertPrivate(AdAlert* q);

  void ensureUi();
  DerivedState deriveState() const;
  detail::AlertVisualStyle resolveVisualStyle(const DerivedState& state) const;
  const DerivedState& resolvedState() const;
  const detail::AlertVisualStyle& resolvedVisualStyle() const;
  bool hasTextContent() const;
  bool hasInformativeTextContent() const;
  bool closeAnimationEnabled(const detail::AlertVisualStyle& style) const;
  QPalette::ColorGroup effectivePaletteGroup() const;
  bool hasPaletteOverride() const;
  void invalidateResolvedCache();
  void ensureResolvedCache() const;
  void refresh();
  void syncContent(const DerivedState& state);
  void applyVisualStyle(const DerivedState& state, const detail::AlertVisualStyle& style);
  void syncAccessibleState();
  QString effectiveAccessibleName() const;
  QString effectiveAccessibleDescription() const;
  QString announcementText() const;
  void announceVisibleContent() const;
  void updateCloseButtonIcon(const detail::AlertVisualStyle& style);
  SectionSlot& section(HostedSlot slot);
  const SectionSlot& section(HostedSlot slot) const;
  void emitHostedWidgetChanged(HostedSlot slot, QWidget* value);
  void clearSectionConnection(SectionSlot& slot);
  QWidget* releaseHostedWidget(HostedSlot slot);
  void clearHostedWidgetReferences(QWidget* widget, HostedSlot exceptSlot);
  void setHostedWidget(HostedSlot slot, QWidget* widget);
  QWidget* takeHostedWidget(HostedSlot slot);
  void deleteHostedWidgetLater(QWidget* widget);
  void bindHostedWidgetDestroyed(HostedSlot slot, QWidget* widget);
  void hostedWidgetDestroyed(HostedSlot slot, QObject* destroyed);
  void startCloseAnimation(int durationMs, const QEasingCurve& easing);
  void completeAnimatedClose();
  void resetCloseAnimationState();
  static void syncSingleChildLayout(QVBoxLayout* layout, QWidget* host, QWidget* child);

  AdAlert* const q_ptr;
  AdAlert::Severity severity = AdAlert::Severity::Info;
  AdAlert::DisplayMode displayMode = AdAlert::DisplayMode::Inline;
  AdAlert::IconMode iconMode = AdAlert::IconMode::Auto;
  bool closable = false;
  bool animated = true;
  QString text;
  QString informativeText;

  QPointer<QGridLayout> rootLayout;
  SectionSlot leadingSection;
  QPointer<QLabel> iconLabel;
  QSpacerItem* leadingSpacerItem = nullptr;
  QPointer<QWidget> contentHost;
  QPointer<QVBoxLayout> contentLayout;
  SectionSlot textSection;
  QPointer<QLabel> textLabel;
  SectionSlot informativeSection;
  QPointer<QLabel> informativeLabel;
  QSpacerItem* actionsSpacerItem = nullptr;
  SectionSlot actionsSection;
  QSpacerItem* dismissSpacerItem = nullptr;
  QPointer<AlertCloseButton> closeButton;

  QPointer<QGraphicsOpacityEffect> opacityEffect;
  QPointer<QParallelAnimationGroup> closeAnimation;
  QPointer<QPropertyAnimation> heightAnimation;
  QPointer<QPropertyAnimation> opacityAnimation;
  CloseState closeState = CloseState::Idle;
  AdAlert::CloseReason pendingCloseReason = AdAlert::CloseReason::Programmatic;
  int savedMaximumHeight = QWIDGETSIZE_MAX;
  bool suppressAccessibleEvents = false;
  QString lastAccessibleName;
  QString lastAccessibleDescription;
  mutable bool resolvedCacheValid = false;
  mutable DerivedState cachedState;
  mutable detail::AlertVisualStyle cachedStyle;
};

AdAlertPrivate::AdAlertPrivate(AdAlert* q) : q_ptr(q) {}

void AdAlertPrivate::ensureUi() {
  if (rootLayout) {
    return;
  }

  Q_Q(AdAlert);

  auto* layout = new QGridLayout(q);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setHorizontalSpacing(0);
  layout->setVerticalSpacing(0);
  layout->setColumnStretch(2, 1);

  auto* leadingHost = new QWidget(q);
  leadingHost->setVisible(false);
  leadingHost->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  auto* leadingLayout = new QVBoxLayout(leadingHost);
  leadingLayout->setContentsMargins(0, 0, 0, 0);
  leadingLayout->setSpacing(0);
  auto* icon = new QLabel(leadingHost);
  icon->setObjectName(QStringLiteral("ad-alert-icon"));
  icon->setVisible(false);
  icon->setAlignment(Qt::AlignCenter);

  auto* content = new QWidget(q);
  content->setVisible(false);
  content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto* contentLayoutValue = new QVBoxLayout(content);
  contentLayoutValue->setContentsMargins(0, 0, 0, 0);
  contentLayoutValue->setSpacing(4);

  auto* textHost = new QWidget(content);
  textHost->setVisible(false);
  auto* textLayoutValue = new QVBoxLayout(textHost);
  textLayoutValue->setContentsMargins(0, 0, 0, 0);
  textLayoutValue->setSpacing(0);
  auto* textValue = new QLabel(textHost);
  textValue->setObjectName(QStringLiteral("ad-alert-text"));
  textValue->setWordWrap(true);
  textValue->setTextFormat(Qt::PlainText);
  textValue->setTextInteractionFlags(Qt::NoTextInteraction);

  auto* informativeHost = new QWidget(content);
  informativeHost->setVisible(false);
  auto* informativeLayoutValue = new QVBoxLayout(informativeHost);
  informativeLayoutValue->setContentsMargins(0, 0, 0, 0);
  informativeLayoutValue->setSpacing(0);
  auto* informativeValue = new QLabel(informativeHost);
  informativeValue->setObjectName(QStringLiteral("ad-alert-informative-text"));
  informativeValue->setWordWrap(true);
  informativeValue->setTextFormat(Qt::PlainText);
  informativeValue->setTextInteractionFlags(Qt::NoTextInteraction);

  contentLayoutValue->addWidget(textHost);
  contentLayoutValue->addWidget(informativeHost);

  auto* actionsHost = new QWidget(q);
  actionsHost->setVisible(false);
  actionsHost->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
  auto* actionsLayoutValue = new QVBoxLayout(actionsHost);
  actionsLayoutValue->setContentsMargins(0, 0, 0, 0);
  actionsLayoutValue->setSpacing(0);

  auto* close = new AlertCloseButton(q);
  close->setObjectName(QStringLiteral("ad-alert-close"));
  close->setCursor(Qt::PointingHandCursor);
  close->setFocusPolicy(Qt::TabFocus);
  close->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  close->setVisible(false);
  close->setToolTip(AdAlert::tr("Close"));
  close->setAccessibleName(AdAlert::tr("Close alert"));

  leadingSpacerItem = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  actionsSpacerItem = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  dismissSpacerItem = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);

  layout->addWidget(leadingHost, 0, 0, Qt::AlignVCenter);
  layout->addItem(leadingSpacerItem, 0, 1);
  layout->addWidget(content, 0, 2, Qt::AlignVCenter);
  layout->addItem(actionsSpacerItem, 0, 3);
  layout->addWidget(actionsHost, 0, 4, Qt::AlignVCenter);
  layout->addItem(dismissSpacerItem, 0, 5);
  layout->addWidget(close, 0, 6, Qt::AlignVCenter);

  QObject::connect(close, &QToolButton::clicked, q, [this]() {
    pendingCloseReason = AdAlert::CloseReason::CloseButton;
    const bool closeStarted = q_ptr->close();
    if (!closeStarted && closeState == CloseState::Idle) {
      pendingCloseReason = AdAlert::CloseReason::Programmatic;
    }
  });

  rootLayout = layout;
  leadingSection.host = leadingHost;
  leadingSection.layout = leadingLayout;
  iconLabel = icon;
  contentHost = content;
  contentLayout = contentLayoutValue;
  textSection.host = textHost;
  textSection.layout = textLayoutValue;
  textLabel = textValue;
  informativeSection.host = informativeHost;
  informativeSection.layout = informativeLayoutValue;
  informativeLabel = informativeValue;
  actionsSection.host = actionsHost;
  actionsSection.layout = actionsLayoutValue;
  closeButton = close;
}

AdAlertPrivate::DerivedState AdAlertPrivate::deriveState() const {
  DerivedState state;
  state.severity = severity;
  state.hasText = hasTextContent();
  state.hasInformativeText = hasInformativeTextContent();
  state.hasActions = actionsSection.widget != nullptr;
  switch (iconMode) {
    case AdAlert::IconMode::Auto:
      state.iconVisible = displayMode == AdAlert::DisplayMode::Banner;
      break;
    case AdAlert::IconMode::Visible:
      state.iconVisible = true;
      break;
    case AdAlert::IconMode::Hidden:
      state.iconVisible = false;
      break;
  }
  return state;
}

detail::AlertVisualStyle AdAlertPrivate::resolveVisualStyle(const DerivedState& state) const {
  Q_Q(const AdAlert);

  detail::AlertStyleInput input;
  input.severity = state.severity;
  input.displayMode = displayMode;
  input.enabled = q->isEnabled();
  input.hasPaletteOverride = hasPaletteOverride();
  input.paletteGroup = effectivePaletteGroup();
  input.baseFont = q->font();
  input.palette = q->palette();

  const adqt::theme::ResolvedTheme resolvedTheme = adqt::theme::ThemeManager::instance().resolve(q);
  return detail::resolveAlertVisualStyle(input, resolvedTheme);
}

const AdAlertPrivate::DerivedState& AdAlertPrivate::resolvedState() const {
  ensureResolvedCache();
  return cachedState;
}

const detail::AlertVisualStyle& AdAlertPrivate::resolvedVisualStyle() const {
  ensureResolvedCache();
  return cachedStyle;
}

bool AdAlertPrivate::hasTextContent() const {
  return textSection.widget || !text.trimmed().isEmpty();
}

bool AdAlertPrivate::hasInformativeTextContent() const {
  return informativeSection.widget || !informativeText.trimmed().isEmpty();
}

bool AdAlertPrivate::closeAnimationEnabled(const detail::AlertVisualStyle& style) const {
  Q_Q(const AdAlert);
  return animated && style.metrics.closeAnimationMs > 0 && q->isVisible();
}

QPalette::ColorGroup AdAlertPrivate::effectivePaletteGroup() const {
  Q_Q(const AdAlert);

  if (!q->isEnabled()) {
    return QPalette::Disabled;
  }
  const QPalette::ColorGroup group = q->palette().currentColorGroup();
  return group == QPalette::Disabled ? QPalette::Active : group;
}

bool AdAlertPrivate::hasPaletteOverride() const {
  Q_Q(const AdAlert);
  return usesCustomPalette(q);
}

void AdAlertPrivate::invalidateResolvedCache() { resolvedCacheValid = false; }

void AdAlertPrivate::ensureResolvedCache() const {
  if (resolvedCacheValid) {
    return;
  }
  cachedState = deriveState();
  cachedStyle = resolveVisualStyle(cachedState);
  resolvedCacheValid = true;
}

void AdAlertPrivate::refresh() {
  ensureUi();
  const DerivedState& state = resolvedState();
  const detail::AlertVisualStyle& style = resolvedVisualStyle();
  syncContent(state);
  applyVisualStyle(state, style);
  syncAccessibleState();
  q_ptr->updateGeometry();
  q_ptr->update();
}

void AdAlertPrivate::syncContent(const DerivedState& state) {
  ensureUi();

  Q_Q(AdAlert);

  if (textLabel) {
    textLabel->setText(text);
  }
  if (informativeLabel) {
    informativeLabel->setText(informativeText);
  }

  QWidget* leadingContent = leadingSection.widget.data();
  if (!leadingContent && state.iconVisible) {
    leadingContent = iconLabel;
  }
  syncSingleChildLayout(leadingSection.layout, leadingSection.host, leadingContent);

  QWidget* titleContent = textSection.widget.data();
  if (!titleContent && state.hasText) {
    titleContent = textLabel;
  }
  syncSingleChildLayout(textSection.layout, textSection.host, titleContent);

  QWidget* descriptionContent = informativeSection.widget.data();
  if (!descriptionContent && state.hasInformativeText) {
    descriptionContent = informativeLabel;
  }
  syncSingleChildLayout(informativeSection.layout, informativeSection.host, descriptionContent);

  syncSingleChildLayout(actionsSection.layout, actionsSection.host, actionsSection.widget);

  if (leadingSection.host) {
    leadingSection.host->setVisible(leadingContent != nullptr);
  }
  if (textSection.host) {
    textSection.host->setVisible(titleContent != nullptr);
  }
  if (informativeSection.host) {
    informativeSection.host->setVisible(descriptionContent != nullptr);
  }
  if (contentHost) {
    contentHost->setVisible(titleContent != nullptr || descriptionContent != nullptr);
  }
  if (actionsSection.host) {
    actionsSection.host->setVisible(actionsSection.widget != nullptr);
  }
  if (closeButton) {
    closeButton->setVisible(closable);
    closeButton->setEnabled(closable && q->isEnabled() && closeState == CloseState::Idle);
    closeButton->setToolTip(AdAlert::tr("Close"));
    closeButton->setAccessibleName(AdAlert::tr("Close alert"));
  }
}

void AdAlertPrivate::applyVisualStyle(const DerivedState& state,
                                      const detail::AlertVisualStyle& style) {
  ensureUi();

  const int paddingInline = state.hasInformativeText
                                ? style.metrics.paddingWithInformativeTextInline
                                : style.metrics.paddingInline;
  const int paddingBlock = state.hasInformativeText ? style.metrics.paddingWithInformativeTextBlock
                                                    : style.metrics.paddingBlock;

  if (rootLayout) {
    rootLayout->setContentsMargins(paddingInline, paddingBlock, paddingInline, paddingBlock);
  }
  if (contentLayout) {
    contentLayout->setSpacing(style.metrics.textInformativeTextGap);
  }

  const bool leadingVisible = leadingSection.host && leadingSection.host->isVisible();
  const bool contentVisible = contentHost && contentHost->isVisible();
  const bool actionsVisible = actionsSection.host && actionsSection.host->isVisible();
  const bool dismissVisible = closeButton && closeButton->isVisible();

  if (leadingSpacerItem) {
    const int width =
        leadingVisible && (contentVisible || actionsVisible || dismissVisible)
            ? (state.hasInformativeText ? style.metrics.gapLeadingContentWithInformativeText
                                        : style.metrics.gapLeadingContent)
            : 0;
    leadingSpacerItem->changeSize(width, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  }
  if (actionsSpacerItem) {
    const int width = contentVisible && actionsVisible ? style.metrics.gapContentActions : 0;
    actionsSpacerItem->changeSize(width, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  }
  if (dismissSpacerItem) {
    const bool showGap = dismissVisible && (actionsVisible || contentVisible);
    const int width = showGap ? style.metrics.gapActionsDismiss : 0;
    dismissSpacerItem->changeSize(width, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  }
  if (rootLayout) {
    rootLayout->invalidate();
  }

  const Qt::Alignment verticalAlignment =
      state.hasInformativeText ? Qt::AlignTop : Qt::AlignVCenter;
  if (rootLayout) {
    rootLayout->setAlignment(leadingSection.host, verticalAlignment);
    rootLayout->setAlignment(contentHost, verticalAlignment);
    rootLayout->setAlignment(actionsSection.host, verticalAlignment);
    rootLayout->setAlignment(closeButton, verticalAlignment);
  }

  if (textLabel) {
    textLabel->setFont(state.hasInformativeText ? style.metrics.textWithInformativeTextFont
                                                : style.metrics.textFont);
    QPalette palette = textLabel->palette();
    palette.setColor(QPalette::WindowText, style.textColor);
    textLabel->setPalette(palette);
  }
  if (informativeLabel) {
    informativeLabel->setFont(style.metrics.informativeTextFont);
    QPalette palette = informativeLabel->palette();
    palette.setColor(QPalette::WindowText, style.informativeTextColor);
    informativeLabel->setPalette(palette);
  }

  if (iconLabel) {
    if (state.iconVisible) {
      adqt::icons::IconRef icon = defaultSeverityIcon(state.severity);
      if (!icon.colors().primarySlot()) {
        icon = icon.withColors(icon.colors().withPrimary(style.iconColor));
      }
      const int iconSize = state.hasInformativeText ? style.metrics.iconSizeWithInformativeText
                                                    : style.metrics.iconSize;
      iconLabel->setFixedSize(iconSize, iconSize);
      iconLabel->setPixmap(adqt::icons::renderIconPixmap(
          icon, {QSize(iconSize, iconSize), std::max(1.0, q_ptr->devicePixelRatioF())}));
    } else {
      iconLabel->clear();
      iconLabel->setFixedSize(0, 0);
    }
  }

  if (closeButton) {
    closeButton->setFixedSize(style.metrics.closeButtonSize, style.metrics.closeButtonSize);
  }
  updateCloseButtonIcon(style);
}

void AdAlertPrivate::syncAccessibleState() {
  const QString resolvedName = effectiveAccessibleName();
  const QString resolvedDescription = effectiveAccessibleDescription();
  const QString previousName = lastAccessibleName;
  const QString previousDescription = lastAccessibleDescription;

  lastAccessibleName = resolvedName;
  lastAccessibleDescription = resolvedDescription;

  if (!q_ptr->isVisible() || suppressAccessibleEvents) {
    return;
  }

  if (resolvedName != previousName) {
    notifyAccessibilityEvent(q_ptr, QAccessible::NameChanged);
  }
  if (resolvedDescription != previousDescription) {
    notifyAccessibilityEvent(q_ptr, QAccessible::DescriptionChanged);
  }
  if (resolvedName != previousName || resolvedDescription != previousDescription) {
    announceVisibleContent();
  }
}

QString AdAlertPrivate::effectiveAccessibleName() const { return resolvedAccessibleName(q_ptr); }

QString AdAlertPrivate::effectiveAccessibleDescription() const {
  return resolvedAccessibleDescription(q_ptr);
}

QString AdAlertPrivate::announcementText() const {
  QString name = effectiveAccessibleName();
  QString description = effectiveAccessibleDescription();
  if (!name.isEmpty() && !description.isEmpty()) {
    return QStringLiteral("%1 %2").arg(name, description);
  }
  if (!name.isEmpty()) {
    return name;
  }
  return description;
}

void AdAlertPrivate::announceVisibleContent() const {
  notifyAccessibilityAnnouncement(q_ptr, announcementText(),
                                  announcementPolitenessForSeverity(severity));
}

void AdAlertPrivate::updateCloseButtonIcon(const detail::AlertVisualStyle& style) {
  if (!closeButton) {
    return;
  }

  closeButton->setVisuals(adqt::icons::antd::outlined::Close(), style.closeIconColor,
                          style.closeIconHoverColor, style.closeButtonHoverBackground,
                          style.closeButtonPressedBackground, style.closeButtonFocusOutlineColor,
                          style.metrics.closeButtonFocusOutlineWidth,
                          QSize(style.metrics.closeIconSize, style.metrics.closeIconSize));
}

AdAlertPrivate::SectionSlot& AdAlertPrivate::section(HostedSlot slot) {
  switch (slot) {
    case HostedSlot::Leading:
      return leadingSection;
    case HostedSlot::Text:
      return textSection;
    case HostedSlot::Informative:
      return informativeSection;
    case HostedSlot::Actions:
      return actionsSection;
  }
  return leadingSection;
}

const AdAlertPrivate::SectionSlot& AdAlertPrivate::section(HostedSlot slot) const {
  switch (slot) {
    case HostedSlot::Leading:
      return leadingSection;
    case HostedSlot::Text:
      return textSection;
    case HostedSlot::Informative:
      return informativeSection;
    case HostedSlot::Actions:
      return actionsSection;
  }
  return leadingSection;
}

void AdAlertPrivate::emitHostedWidgetChanged(HostedSlot slot, QWidget* value) {
  Q_Q(AdAlert);

  switch (slot) {
    case HostedSlot::Leading:
      emit q->leadingWidgetChanged(value);
      break;
    case HostedSlot::Text:
      emit q->textWidgetChanged(value);
      break;
    case HostedSlot::Informative:
      emit q->informativeWidgetChanged(value);
      break;
    case HostedSlot::Actions:
      emit q->actionsWidgetChanged(value);
      break;
  }
}

void AdAlertPrivate::clearSectionConnection(SectionSlot& slot) {
  if (slot.destroyedConnection) {
    QObject::disconnect(slot.destroyedConnection);
    slot.destroyedConnection = QMetaObject::Connection();
  }
}

QWidget* AdAlertPrivate::releaseHostedWidget(HostedSlot slot) {
  SectionSlot& slotState = section(slot);
  QWidget* widget = slotState.widget.data();
  clearSectionConnection(slotState);
  if (!widget) {
    slotState.widget.clear();
    return nullptr;
  }

  if (slotState.layout) {
    slotState.layout->removeWidget(widget);
  }
  widget->hide();
  if (widget->parentWidget()) {
    widget->setParent(nullptr);
  }
  slotState.widget.clear();
  return widget;
}

void AdAlertPrivate::clearHostedWidgetReferences(QWidget* widget, HostedSlot exceptSlot) {
  if (!widget) {
    return;
  }

  constexpr HostedSlot kSlots[] = {
      HostedSlot::Leading,
      HostedSlot::Text,
      HostedSlot::Informative,
      HostedSlot::Actions,
  };
  for (HostedSlot slot : kSlots) {
    if (slot == exceptSlot) {
      continue;
    }
    SectionSlot& slotState = section(slot);
    if (slotState.widget != widget) {
      continue;
    }
    releaseHostedWidget(slot);
    emitHostedWidgetChanged(slot, nullptr);
  }
}

void AdAlertPrivate::setHostedWidget(HostedSlot slot, QWidget* widget) {
  SectionSlot& slotState = section(slot);
  if (slotState.widget == widget) {
    return;
  }

  if (widget) {
    clearHostedWidgetReferences(widget, slot);
  }

  QWidget* previous = releaseHostedWidget(slot);
  slotState.widget = widget;
  if (widget) {
    widget->hide();
    if (widget->parentWidget()) {
      widget->setParent(nullptr);
    }
    bindHostedWidgetDestroyed(slot, widget);
  }

  emitHostedWidgetChanged(slot, slotState.widget);
  if (previous) {
    deleteHostedWidgetLater(previous);
  }
  invalidateResolvedCache();
  refresh();
}

QWidget* AdAlertPrivate::takeHostedWidget(HostedSlot slot) {
  QWidget* widget = releaseHostedWidget(slot);
  if (!widget) {
    return nullptr;
  }
  emitHostedWidgetChanged(slot, nullptr);
  invalidateResolvedCache();
  refresh();
  return widget;
}

void AdAlertPrivate::deleteHostedWidgetLater(QWidget* widget) {
  if (!widget) {
    return;
  }
  widget->deleteLater();
}

void AdAlertPrivate::bindHostedWidgetDestroyed(HostedSlot slot, QWidget* widget) {
  SectionSlot& slotState = section(slot);
  clearSectionConnection(slotState);
  if (!widget) {
    return;
  }

  Q_Q(AdAlert);
  slotState.destroyedConnection = QObject::connect(
      widget, &QObject::destroyed, q,
      [this, slot](QObject* destroyed) { hostedWidgetDestroyed(slot, destroyed); });
}

void AdAlertPrivate::hostedWidgetDestroyed(HostedSlot slot, QObject* destroyed) {
  SectionSlot& slotState = section(slot);
  if (slotState.widget && slotState.widget != destroyed) {
    return;
  }

  clearSectionConnection(slotState);
  if (QWidget* widget = qobject_cast<QWidget*>(destroyed)) {
    if (slotState.layout) {
      slotState.layout->removeWidget(widget);
    }
  }
  slotState.widget.clear();
  emitHostedWidgetChanged(slot, nullptr);
  invalidateResolvedCache();
  refresh();
}

void AdAlertPrivate::startCloseAnimation(int durationMs, const QEasingCurve& easing) {
  Q_Q(AdAlert);

  if (!opacityEffect) {
    opacityEffect = new QGraphicsOpacityEffect(q);
    opacityEffect->setOpacity(1.0);
    q->setGraphicsEffect(opacityEffect);
  }
  if (!closeAnimation) {
    closeAnimation = new QParallelAnimationGroup(q);
    heightAnimation = new QPropertyAnimation(q, "maximumHeight", closeAnimation);
    opacityAnimation = new QPropertyAnimation(opacityEffect, "opacity", closeAnimation);
    QObject::connect(closeAnimation, &QParallelAnimationGroup::finished, q,
                     [this]() { completeAnimatedClose(); });
  }

  if (!closeAnimation || !heightAnimation || !opacityAnimation || !opacityEffect) {
    closeState = CloseState::Finalizing;
    q->close();
    return;
  }

  savedMaximumHeight = q->maximumHeight();

  int startHeight = q->height();
  if (startHeight <= 0) {
    startHeight = q->sizeHint().height();
  }
  startHeight = std::max(1, startHeight);

  opacityEffect->setOpacity(1.0);
  q->setMaximumHeight(startHeight);
  closeAnimation->stop();
  closeState = CloseState::Animating;

  heightAnimation->setDuration(durationMs);
  heightAnimation->setStartValue(startHeight);
  heightAnimation->setEndValue(0);
  heightAnimation->setEasingCurve(easing);

  opacityAnimation->setTargetObject(opacityEffect);
  opacityAnimation->setDuration(durationMs);
  opacityAnimation->setStartValue(1.0);
  opacityAnimation->setEndValue(0.0);
  opacityAnimation->setEasingCurve(easing);

  closeAnimation->start();
}

void AdAlertPrivate::completeAnimatedClose() {
  if (closeAnimation && closeAnimation->state() == QAbstractAnimation::Running) {
    closeAnimation->stop();
  }
  closeState = CloseState::Finalizing;
  q_ptr->close();
}

void AdAlertPrivate::resetCloseAnimationState() {
  q_ptr->setMaximumHeight(savedMaximumHeight > 0 ? savedMaximumHeight : QWIDGETSIZE_MAX);
  savedMaximumHeight = QWIDGETSIZE_MAX;
  if (opacityEffect) {
    opacityEffect->setOpacity(1.0);
  }
}

void AdAlertPrivate::syncSingleChildLayout(QVBoxLayout* layout, QWidget* host, QWidget* child) {
  if (!layout || !host) {
    return;
  }

  for (int index = layout->count() - 1; index >= 0; --index) {
    QWidget* current = layout->itemAt(index) ? layout->itemAt(index)->widget() : nullptr;
    if (!current) {
      continue;
    }
    if (current == child) {
      continue;
    }
    layout->removeWidget(current);
    current->hide();
  }

  if (!child) {
    return;
  }

  if (child->parentWidget() != host) {
    child->setParent(host);
  }
  if (layout->indexOf(child) < 0) {
    layout->addWidget(child);
  }
  child->show();
}

AdAlert::AdAlert(QWidget* parent) : QFrame(parent), d_ptr(new AdAlertPrivate(this)) {
  ensureAlertAccessibleFactoryInstalled();
  setObjectName(QStringLiteral("ad-alert"));
  setAttribute(Qt::WA_Hover, true);
  setFrameStyle(QFrame::NoFrame);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

  Q_D(AdAlert);
  d->ensureUi();
  d->refresh();
}

AdAlert::~AdAlert() {
  Q_D(AdAlert);
  if (d->closeAnimation) {
    d->closeAnimation->stop();
  }
  d->clearSectionConnection(d->leadingSection);
  d->clearSectionConnection(d->textSection);
  d->clearSectionConnection(d->informativeSection);
  d->clearSectionConnection(d->actionsSection);
}

AdAlert::Severity AdAlert::severity() const {
  Q_D(const AdAlert);
  return d->severity;
}

void AdAlert::setSeverity(Severity value) {
  Q_D(AdAlert);
  if (d->severity == value) {
    return;
  }
  d->severity = value;
  emit severityChanged(d->severity);
  d->invalidateResolvedCache();
  d->refresh();
}

AdAlert::DisplayMode AdAlert::displayMode() const {
  Q_D(const AdAlert);
  return d->displayMode;
}

void AdAlert::setDisplayMode(DisplayMode value) {
  Q_D(AdAlert);
  if (d->displayMode == value) {
    return;
  }
  d->displayMode = value;
  emit displayModeChanged(d->displayMode);
  d->invalidateResolvedCache();
  d->refresh();
}

AdAlert::IconMode AdAlert::iconMode() const {
  Q_D(const AdAlert);
  return d->iconMode;
}

void AdAlert::setIconMode(IconMode value) {
  Q_D(AdAlert);
  if (d->iconMode == value) {
    return;
  }
  d->iconMode = value;
  emit iconModeChanged(d->iconMode);
  d->invalidateResolvedCache();
  d->refresh();
}

bool AdAlert::closable() const {
  Q_D(const AdAlert);
  return d->closable;
}

void AdAlert::setClosable(bool value) {
  Q_D(AdAlert);
  if (d->closable == value) {
    return;
  }
  d->closable = value;
  emit closableChanged(d->closable);
  d->invalidateResolvedCache();
  d->refresh();
}

bool AdAlert::animated() const {
  Q_D(const AdAlert);
  return d->animated;
}

void AdAlert::setAnimated(bool value) {
  Q_D(AdAlert);
  if (d->animated == value) {
    return;
  }
  d->animated = value;
  emit animatedChanged(d->animated);
}

QString AdAlert::text() const {
  Q_D(const AdAlert);
  return d->text;
}

void AdAlert::setText(const QString& value) {
  Q_D(AdAlert);
  if (d->text == value) {
    return;
  }
  d->text = value;
  emit textChanged(d->text);
  d->invalidateResolvedCache();
  d->refresh();
}

QString AdAlert::informativeText() const {
  Q_D(const AdAlert);
  return d->informativeText;
}

void AdAlert::setInformativeText(const QString& value) {
  Q_D(AdAlert);
  if (d->informativeText == value) {
    return;
  }
  d->informativeText = value;
  emit informativeTextChanged(d->informativeText);
  d->invalidateResolvedCache();
  d->refresh();
}

QWidget* AdAlert::leadingWidget() const {
  Q_D(const AdAlert);
  return d->leadingSection.widget.data();
}

void AdAlert::setLeadingWidget(QWidget* widget) {
  Q_D(AdAlert);
  d->setHostedWidget(AdAlertPrivate::HostedSlot::Leading, widget);
}

QWidget* AdAlert::takeLeadingWidget() {
  Q_D(AdAlert);
  return d->takeHostedWidget(AdAlertPrivate::HostedSlot::Leading);
}

QWidget* AdAlert::textWidget() const {
  Q_D(const AdAlert);
  return d->textSection.widget.data();
}

void AdAlert::setTextWidget(QWidget* widget) {
  Q_D(AdAlert);
  d->setHostedWidget(AdAlertPrivate::HostedSlot::Text, widget);
}

QWidget* AdAlert::takeTextWidget() {
  Q_D(AdAlert);
  return d->takeHostedWidget(AdAlertPrivate::HostedSlot::Text);
}

QWidget* AdAlert::informativeWidget() const {
  Q_D(const AdAlert);
  return d->informativeSection.widget.data();
}

void AdAlert::setInformativeWidget(QWidget* widget) {
  Q_D(AdAlert);
  d->setHostedWidget(AdAlertPrivate::HostedSlot::Informative, widget);
}

QWidget* AdAlert::takeInformativeWidget() {
  Q_D(AdAlert);
  return d->takeHostedWidget(AdAlertPrivate::HostedSlot::Informative);
}

QWidget* AdAlert::actionsWidget() const {
  Q_D(const AdAlert);
  return d->actionsSection.widget.data();
}

void AdAlert::setActionsWidget(QWidget* widget) {
  Q_D(AdAlert);
  d->setHostedWidget(AdAlertPrivate::HostedSlot::Actions, widget);
}

QWidget* AdAlert::takeActionsWidget() {
  Q_D(AdAlert);
  return d->takeHostedWidget(AdAlertPrivate::HostedSlot::Actions);
}

void AdAlert::changeEvent(QEvent* event) {
  QFrame::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::EnabledChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::LanguageChange: {
      Q_D(AdAlert);
      d->invalidateResolvedCache();
      d->refresh();
      break;
    }
    default:
      break;
  }
}

void AdAlert::closeEvent(QCloseEvent* event) {
  Q_D(AdAlert);
  if (!event) {
    return;
  }

  auto finalizeClose = [&]() {
    const CloseReason reason = d->pendingCloseReason;
    d->closeState = AdAlertPrivate::CloseState::Finalizing;

    this->QFrame::closeEvent(event);
    if (!event->isAccepted()) {
      d->closeState = AdAlertPrivate::CloseState::Idle;
      d->pendingCloseReason = CloseReason::Programmatic;
      d->resetCloseAnimationState();
      d->invalidateResolvedCache();
      d->refresh();
      return;
    }

    d->resetCloseAnimationState();
    d->invalidateResolvedCache();

    if (d->closeState == AdAlertPrivate::CloseState::Idle) {
      return;
    }

    if (!isVisible()) {
      d->closeState = AdAlertPrivate::CloseState::Idle;
      d->pendingCloseReason = CloseReason::Programmatic;
      emit closed(reason);
      return;
    }

    d->closeState = AdAlertPrivate::CloseState::Finalizing;
  };

  if (d->closeState == AdAlertPrivate::CloseState::Finalizing) {
    finalizeClose();
    return;
  }

  if (d->closeState != AdAlertPrivate::CloseState::Idle) {
    event->ignore();
    return;
  }

  d->closeState = AdAlertPrivate::CloseState::Requested;
  emit closeRequested(d->pendingCloseReason);

  const detail::AlertVisualStyle& style = d->resolvedVisualStyle();
  if (!d->closeAnimationEnabled(style)) {
    finalizeClose();
    return;
  }

  event->ignore();
  d->startCloseAnimation(style.metrics.closeAnimationMs, style.metrics.closeAnimationEasing);
}

void AdAlert::hideEvent(QHideEvent* event) {
  QFrame::hideEvent(event);

  Q_D(AdAlert);
  if (d->closeState == AdAlertPrivate::CloseState::Animating) {
    if (d->closeAnimation && d->closeAnimation->state() == QAbstractAnimation::Running) {
      d->closeAnimation->stop();
    }
    d->closeState = AdAlertPrivate::CloseState::Idle;
    d->pendingCloseReason = CloseReason::Programmatic;
    d->resetCloseAnimationState();
    d->invalidateResolvedCache();
    return;
  }

  if (d->closeState == AdAlertPrivate::CloseState::Requested ||
      d->closeState == AdAlertPrivate::CloseState::Finalizing) {
    const CloseReason reason = d->pendingCloseReason;
    d->closeState = AdAlertPrivate::CloseState::Idle;
    d->pendingCloseReason = CloseReason::Programmatic;
    d->resetCloseAnimationState();
    d->invalidateResolvedCache();
    emit closed(reason);
  }
}

void AdAlert::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  Q_D(AdAlert);
  const detail::AlertVisualStyle& style = d->resolvedVisualStyle();

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const bool hasVisibleBorder = style.metrics.borderWidth > 0 && style.border.alpha() > 0;
  const qreal inset = hasVisibleBorder ? style.metrics.borderWidth / 2.0 : 0.0;
  const QRectF shapeRect = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
  if (shapeRect.width() <= 0.0 || shapeRect.height() <= 0.0) {
    return;
  }

  const qreal radius = std::max<qreal>(0.0, style.metrics.borderRadius);
  const QPainterPath fillPath = roundedRectPath(shapeRect, radius, radius, radius, radius);
  painter.fillPath(fillPath, style.background);

  if (hasVisibleBorder) {
    painter.strokePath(fillPath, QPen(style.border, style.metrics.borderWidth, Qt::SolidLine,
                                      Qt::SquareCap, Qt::MiterJoin));
  }
}

void AdAlert::showEvent(QShowEvent* event) {
  QFrame::showEvent(event);

  Q_D(AdAlert);
  if (d->closeAnimation && d->closeAnimation->state() == QAbstractAnimation::Running) {
    d->closeAnimation->stop();
  }
  d->closeState = AdAlertPrivate::CloseState::Idle;
  d->pendingCloseReason = CloseReason::Programmatic;
  d->resetCloseAnimationState();

  d->suppressAccessibleEvents = true;
  d->invalidateResolvedCache();
  d->refresh();
  d->suppressAccessibleEvents = false;

  notifyAccessibilityEvent(this, QAccessible::ObjectShow);
  d->announceVisibleContent();
}

}  // namespace adqt::widgets
