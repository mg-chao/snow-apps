#include "message.h"
#include "detail/pointer_region.h"

#include "antd_icons.h"
#include "detail/animated_scalar.h"
#include "detail/popup_shadow.h"
#include "detail/timing_hub.h"
#include "message_style.h"
#include "theme/theme.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QChildEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QResizeEvent>
#include <QSet>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace adqt::widgets {

namespace {

constexpr char kExpiryTaskKey[] = "AdMessage.Expiry";
constexpr char kFinalizeTaskKey[] = "AdMessage.Finalize";
constexpr char kRaiseTaskKey[] = "AdMessage.Raise";
constexpr char kSpinnerFrameKey[] = "AdMessage.Spinner";
constexpr char kClickResetTaskKey[] = "AdMessage.ClickReset";

quint64 nextMessageHostSequence() {
  static quint64 sequence = 1;
  return sequence++;
}

AdMessage::ComponentTokens mergeComponentTokens(const AdMessage::ComponentTokens& base,
                                                const AdMessage::ComponentTokens& overlay) {
  AdMessage::ComponentTokens result = base;
#define ADQT_MERGE_MESSAGE_TOKEN(name) \
  if (overlay.name.has_value()) {      \
    result.name = overlay.name;        \
  }
  ADQT_MERGE_MESSAGE_TOKEN(zIndexPopup)
  ADQT_MERGE_MESSAGE_TOKEN(contentBg)
  ADQT_MERGE_MESSAGE_TOKEN(contentPaddingHorizontal)
  ADQT_MERGE_MESSAGE_TOKEN(contentPaddingVertical)
  ADQT_MERGE_MESSAGE_TOKEN(borderRadius)
  ADQT_MERGE_MESSAGE_TOKEN(iconSize)
  ADQT_MERGE_MESSAGE_TOKEN(iconContentGap)
#undef ADQT_MERGE_MESSAGE_TOKEN
  return result;
}

AdMessage::SemanticSlotStyle mergeSemanticSlot(const AdMessage::SemanticSlotStyle& base,
                                               const AdMessage::SemanticSlotStyle& overlay) {
  AdMessage::SemanticSlotStyle result = base;
  if (overlay.textColor.has_value()) {
    result.textColor = overlay.textColor;
  }
  if (overlay.backgroundColor.has_value()) {
    result.backgroundColor = overlay.backgroundColor;
  }
  if (overlay.borderColor.has_value()) {
    result.borderColor = overlay.borderColor;
  }
  return result;
}

AdMessage::SemanticStyles mergeSemanticStyles(const AdMessage::SemanticStyles& base,
                                              const AdMessage::SemanticStyles& overlay) {
  AdMessage::SemanticStyles result;
  result.root = mergeSemanticSlot(base.root, overlay.root);
  result.icon = mergeSemanticSlot(base.icon, overlay.icon);
  result.content = mergeSemanticSlot(base.content, overlay.content);
  return result;
}

adqt::icons::IconRef coloredIcon(adqt::icons::IconRef icon, const QColor& color) {
  if (!icon.isValid() || !color.isValid()) {
    return icon;
  }
  return icon.withColors(icon.colors().withPrimary(color));
}

adqt::icons::IconRef defaultIconForType(AdMessage::Type type) {
  using namespace adqt::icons::antd;
  switch (type) {
    case AdMessage::Type::Info:
      return filled::InfoCircle();
    case AdMessage::Type::Success:
      return filled::CheckCircle();
    case AdMessage::Type::Warning:
      return filled::ExclamationCircle();
    case AdMessage::Type::Error:
      return filled::CloseCircle();
    case AdMessage::Type::Loading:
      return outlined::Loading3Quarters();
    case AdMessage::Type::None:
      return {};
  }
  return {};
}

QAccessible::AnnouncementPoliteness announcementPoliteness(AdMessage::Type type) {
  return type == AdMessage::Type::Warning || type == AdMessage::Type::Error
             ? QAccessible::AnnouncementPoliteness::Assertive
             : QAccessible::AnnouncementPoliteness::Polite;
}

void announceMessage(QWidget* widget, const QString& content, AdMessage::Type type) {
  if (!widget || content.trimmed().isEmpty()) {
    return;
  }
  QAccessibleAnnouncementEvent event(widget, content.trimmed());
  event.setPoliteness(announcementPoliteness(type));
  QAccessible::updateAccessibility(&event);
}

class MessageSlotHost final : public QWidget {
 public:
  explicit MessageSlotHost(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground, true);
  }

  void setSemanticColors(const std::optional<QColor>& background,
                         const std::optional<QColor>& border) {
    background_ = background;
    border_ = border;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QWidget::paintEvent(event);
    if ((!background_.has_value() || !background_->isValid()) &&
        (!border_.has_value() || !border_->isValid())) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (background_.has_value() && background_->isValid()) {
      painter.fillRect(bounds, background_.value());
    }
    if (border_.has_value() && border_->isValid()) {
      painter.setPen(QPen(border_.value(), 1.0));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(bounds);
    }
  }

 private:
  std::optional<QColor> background_;
  std::optional<QColor> border_;
};

class MessageIconWidget final : public QWidget {
 public:
  explicit MessageIconWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
  }

  ~MessageIconWidget() override {
    detail::clearFrameSubscription(this, QString::fromLatin1(kSpinnerFrameKey));
  }

  void setVisual(const adqt::icons::IconRef& icon, int iconSize, bool spinning,
                 int spinnerCycleMs) {
    icon_ = icon;
    iconSize_ = std::max(1, iconSize);
    spinnerCycleMs_ = std::max(1, spinnerCycleMs);
    spinning_ = spinning;
    setFixedSize(iconSize_, iconSize_);

    if (spinning) {
      detail::setFrameSubscription(
          this, QString::fromLatin1(kSpinnerFrameKey), true, [this](qint64 nowMs, qint64) {
            angle_ = std::fmod((static_cast<qreal>(nowMs % spinnerCycleMs_) /
                                static_cast<qreal>(spinnerCycleMs_)) *
                                   360.0,
                               360.0);
            update();
          });
    } else {
      detail::clearFrameSubscription(this, QString::fromLatin1(kSpinnerFrameKey));
      angle_ = 0.0;
    }
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    if (!icon_.isValid()) {
      return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QRectF iconBounds(rect());
    if (spinning_ &&
        adqt::icons::describeIcon(icon_).key.name == QStringLiteral("loading-3-quarters")) {
      // The loading SVG reaches its view-box edge. Leave room for its antialiased stroke
      // so Qt does not crop the rotating 3/4 circle.
      const qreal smallestSide = std::min(iconBounds.width(), iconBounds.height());
      const qreal inset = std::min<qreal>(1.0, std::max<qreal>(0.0, (smallestSide - 1.0) / 2.0));
      iconBounds.adjust(inset, inset, -inset, -inset);
    }

    if (std::abs(angle_) > 0.01) {
      // Rotate around the exact center of the painted icon, rather than QRect::center().
      // QRect::center() is integer-rounded for even dimensions and differs from the
      // center of the inset loading-icon bounds by one logical pixel.
      const QPointF rotationCenter = iconBounds.center();
      painter.translate(rotationCenter);
      painter.rotate(angle_);
      painter.translate(-rotationCenter);
    }
    adqt::icons::paintIcon(&painter, icon_, iconBounds);
  }

 private:
  adqt::icons::IconRef icon_;
  int iconSize_ = 16;
  int spinnerCycleMs_ = 1000;
  qreal angle_ = 0.0;
  bool spinning_ = false;
};

class MessageHostWidget final : public QWidget {
 public:
  explicit MessageHostWidget(QWidget* parent = nullptr)
      : QWidget(parent), sequence_(nextMessageHostSequence()) {
    setObjectName(QStringLiteral("ad-message-host"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
  }

  void setNoticeMask(const QList<QWidget*>& notices) {
    QRegion interactiveRegion;
    for (QWidget* notice : notices) {
      if (notice && notice->isVisible()) {
        interactiveRegion += notice->geometry();
      }
    }
    setMask(interactiveRegion);
  }

  int zIndex() const { return zIndex_; }
  void setZIndex(int value) { zIndex_ = std::max(0, value); }
  quint64 sequence() const { return sequence_; }

 private:
  int zIndex_ = 2010;
  quint64 sequence_ = 0;
};

class MessageNoticeWidget final : public QWidget {
 public:
  using VoidCallback = std::function<void()>;

  MessageNoticeWidget(AdMessageHandle* handle, VoidCallback timeoutCallback,
                      VoidCallback layoutCallback, QWidget* parent)
      : QWidget(parent),
        handle_(handle),
        timeoutCallback_(std::move(timeoutCallback)),
        layoutCallback_(std::move(layoutCallback)) {
    setObjectName(QStringLiteral("ad-message-notice"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);

    rootLayout_ = new QHBoxLayout(this);
    rootLayout_->setSpacing(8);

    iconHost_ = new MessageSlotHost(this);
    iconHost_->setObjectName(QStringLiteral("ad-message-icon"));
    iconLayout_ = new QVBoxLayout(iconHost_);
    iconLayout_->setContentsMargins(0, 0, 0, 0);
    iconLayout_->setSpacing(0);

    defaultIcon_ = new MessageIconWidget(iconHost_);
    defaultIcon_->setObjectName(QStringLiteral("ad-message-default-icon"));
    iconLayout_->addWidget(defaultIcon_, 0, Qt::AlignCenter);

    contentHost_ = new MessageSlotHost(this);
    contentHost_->setObjectName(QStringLiteral("ad-message-content-host"));
    contentLayout_ = new QVBoxLayout(contentHost_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);

    contentLabel_ = new QLabel(contentHost_);
    contentLabel_->setObjectName(QStringLiteral("ad-message-content"));
    contentLabel_->setTextFormat(Qt::PlainText);
    contentLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    contentLabel_->setWordWrap(true);
    contentLayout_->addWidget(contentLabel_);

    rootLayout_->addWidget(iconHost_, 0, Qt::AlignVCenter);
    rootLayout_->addWidget(contentHost_, 0, Qt::AlignVCenter);

    opacityEffect_ = new QGraphicsOpacityEffect(this);
    opacityEffect_->setOpacity(0.0);
    setGraphicsEffect(opacityEffect_);

    progress_.configure(this, QStringLiteral("AdMessage.Motion"), [this]() {
      if (opacityEffect_) {
        opacityEffect_->setOpacity(std::clamp(progress_.value(), 0.0, 1.0));
      }
      if (layoutCallback_) {
        layoutCallback_();
      }
    });
    progress_.snapTo(0.0);
    installEventFilter(this);
    installClickTracking(iconHost_);
    installClickTracking(contentHost_);
  }

  ~MessageNoticeWidget() override {
    cancelExpiry();
    detail::cancelTimingTask(this, QString::fromLatin1(kFinalizeTaskKey));
  }

  void setRequest(AdMessage::Request request, const AdMessage::Config& config,
                  const detail::MessageVisualStyle& style, int maximumFrameWidth,
                  bool animateUpdate) {
    request_ = std::move(request);
    config_ = config;
    style_ = style;
    onClick_ = request_.onClick;
    onClose_ = request_.onClose;
    maximumFrameWidth_ = std::max(1, maximumFrameWidth);

    syncIconWidget();
    syncContentWidget();
    refreshSemantics();
    refreshLayoutMetrics();
    restartExpiry();

    if (!animateUpdate) {
      progress_.snapTo(0.0);
      if (opacityEffect_) {
        opacityEffect_->setOpacity(0.0);
      }
    }
    updateGeometry();
    adjustSize();
    update();
  }

  void refreshAppearance(const AdMessage::Config& config, const detail::MessageVisualStyle& style,
                         int maximumFrameWidth) {
    const bool previouslyPausedByHover = detail::widgetHovered(this) && effectivePauseOnHover();
    config_ = config;
    style_ = style;
    maximumFrameWidth_ = std::max(1, maximumFrameWidth);
    syncDefaultIconVisual();
    refreshSemantics();
    refreshLayoutMetrics();
    updateGeometry();
    adjustSize();
    update();

    const bool nowPausedByHover = detail::widgetHovered(this) && effectivePauseOnHover();
    if (!closing_ && previouslyPausedByHover != nowPausedByHover && remainingDurationMs_ > 0) {
      if (nowPausedByHover) {
        const qint64 now = detail::timingNowMs();
        if (expiryStartedMs_ > 0) {
          remainingDurationMs_ =
              std::max(1, remainingDurationMs_ - static_cast<int>(now - expiryStartedMs_));
        }
        cancelExpiry();
      } else {
        scheduleExpiry(remainingDurationMs_);
      }
    }
  }

  void animateIn() {
    closing_ = false;
    show();
    const int duration = style_.metrics.motionDurationMs;
    progress_.animateTo(1.0, duration, [easing = style_.metrics.motionEasing](qreal value) {
      return easing.valueForProgress(value);
    });
    if (duration <= 0) {
      if (opacityEffect_) {
        opacityEffect_->setOpacity(1.0);
      }
      if (layoutCallback_) {
        layoutCallback_();
      }
    }
  }

  int animateOut() {
    closing_ = true;
    cancelExpiry();
    const int duration = style_.metrics.motionDurationMs;
    progress_.animateTo(0.0, duration, [easing = style_.metrics.motionEasing](qreal value) {
      return easing.valueForProgress(value);
    });
    return duration;
  }

  void cancelExpiry() {
    detail::cancelTimingTask(this, QString::fromLatin1(kExpiryTaskKey));
    expiryStartedMs_ = 0;
  }

  qreal progress() const { return std::clamp(progress_.value(), 0.0, 1.0); }
  bool isClosing() const { return closing_; }
  bool isHovered() const { return detail::widgetHovered(this); }
  int noticePadding() const { return style_.metrics.noticePadding; }
  int zIndexPopup() const { return style_.metrics.zIndexPopup; }

  QRect visualRect() const { return detail::antPopupShadowVisualRect(rect()).toAlignedRect(); }

  QSize visualSizeHint() const { return detail::removeAntPopupShadowMargins(sizeHint()); }

  AdMessage::Callback takeCloseCallback() {
    AdMessage::Callback callback = std::move(onClose_);
    onClose_ = {};
    return callback;
  }

  QString accessibleContent() const {
    if (!request_.content.trimmed().isEmpty()) {
      return request_.content.trimmed();
    }
    if (request_.contentWidget) {
      if (!request_.contentWidget->accessibleName().trimmed().isEmpty()) {
        return request_.contentWidget->accessibleName().trimmed();
      }
      return request_.contentWidget->accessibleDescription().trimmed();
    }
    return {};
  }

  QWidget* hostedContentWidget() const { return customContent_; }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!event) {
      return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::ChildAdded) {
      auto* childEvent = static_cast<QChildEvent*>(event);
      if (auto* childWidget = qobject_cast<QWidget*>(childEvent->child())) {
        installClickTracking(childWidget);
      }
    } else if (event->type() == QEvent::MouseButtonRelease && watched != this) {
      const auto* mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton && !closing_) {
        dispatchClick();
      }
    }
    return QWidget::eventFilter(watched, event);
  }

  bool event(QEvent* event) override {
    detail::resetWidgetHoverOnLifecycle(this, event);
    return QWidget::event(event);
  }

  void enterEvent(QEnterEvent* event) override {
    QWidget::enterEvent(event);

    refreshSemantics();
    if (effectivePauseOnHover() && !closing_ && remainingDurationMs_ > 0) {
      const qint64 now = detail::timingNowMs();
      if (expiryStartedMs_ > 0) {
        remainingDurationMs_ =
            std::max(1, remainingDurationMs_ - static_cast<int>(now - expiryStartedMs_));
      }
      cancelExpiry();
    }
  }

  void leaveEvent(QEvent* event) override {
    QWidget::leaveEvent(event);

    refreshSemantics();
    if (effectivePauseOnHover() && !closing_ && remainingDurationMs_ > 0) {
      scheduleExpiry(remainingDurationMs_);
    }
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event && event->button() == Qt::LeftButton &&
        visualRect().contains(event->position().toPoint()) && !closing_) {
      dispatchClick();
    }
    QWidget::mouseReleaseEvent(event);
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    const QRectF surface = detail::antPopupShadowVisualRect(rect());
    if (surface.isEmpty()) {
      return;
    }

    QPainterPath path;
    path.addRoundedRect(surface, style_.metrics.borderRadius, style_.metrics.borderRadius);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    detail::paintAntPopupBoxShadowSecondary(painter, path);
    painter.fillPath(path, resolvedContentBackground_);
    if (resolvedBorderWidth_ > 0 && resolvedBorderColor_.isValid() &&
        resolvedBorderColor_.alpha() > 0) {
      painter.setPen(QPen(resolvedBorderColor_, resolvedBorderWidth_));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(path);
    }
  }

 private:
  bool effectivePauseOnHover() const {
    return request_.pauseOnHover.value_or(config_.pauseOnHover);
  }

  int effectiveDurationMs() const {
    return request_.durationMs.value_or(config_.defaultDurationMs);
  }

  void restartExpiry() {
    cancelExpiry();
    remainingDurationMs_ = std::max(0, effectiveDurationMs());
    if (remainingDurationMs_ > 0 && !(detail::widgetHovered(this) && effectivePauseOnHover())) {
      scheduleExpiry(remainingDurationMs_);
    }
  }

  void scheduleExpiry(int delayMs) {
    if (delayMs <= 0 || closing_) {
      return;
    }
    remainingDurationMs_ = delayMs;
    expiryStartedMs_ = detail::timingNowMs();
    detail::scheduleTimingTask(this, QString::fromLatin1(kExpiryTaskKey), delayMs, [this]() {
      expiryStartedMs_ = 0;
      remainingDurationMs_ = 0;
      if (timeoutCallback_) {
        timeoutCallback_();
      }
    });
  }

  void syncIconWidget() {
    QWidget* nextCustom = request_.iconWidget;
    if (customIcon_ && customIcon_ != nextCustom) {
      iconLayout_->removeWidget(customIcon_);
      customIcon_->deleteLater();
      customIcon_.clear();
    }

    if (nextCustom) {
      hasResolvedIcon_ = false;
      resolvedIcon_ = {};
      syncDefaultIconVisual();
      nextCustom->setParent(iconHost_);
      customIcon_ = nextCustom;
      if (iconLayout_->indexOf(nextCustom) < 0) {
        iconLayout_->addWidget(nextCustom, 0, Qt::AlignCenter);
      }
      defaultIcon_->hide();
      nextCustom->show();
      iconHost_->show();
      installClickTracking(nextCustom);
      return;
    }

    adqt::icons::IconRef icon;
    if (request_.icon.has_value()) {
      icon = request_.icon.value();
    } else {
      icon = defaultIconForType(request_.type);
    }
    hasResolvedIcon_ = icon.isValid();
    resolvedIcon_ = icon;
    syncDefaultIconVisual();
    defaultIcon_->setVisible(hasResolvedIcon_);
    iconHost_->setVisible(hasResolvedIcon_);
  }

  void syncDefaultIconVisual() {
    if (!defaultIcon_) {
      return;
    }
    defaultIcon_->setVisual(coloredIcon(resolvedIcon_, resolvedIconColor_), style_.metrics.iconSize,
                            request_.type == AdMessage::Type::Loading && hasResolvedIcon_,
                            style_.metrics.spinnerCycleMs);
  }

  void syncContentWidget() {
    QWidget* nextCustom = request_.contentWidget;
    if (customContent_ && customContent_ != nextCustom) {
      contentLayout_->removeWidget(customContent_);
      customContent_->deleteLater();
      customContent_.clear();
    }

    if (nextCustom) {
      nextCustom->setParent(contentHost_);
      customContent_ = nextCustom;
      if (contentLayout_->indexOf(nextCustom) < 0) {
        contentLayout_->addWidget(nextCustom);
      }
      contentLabel_->hide();
      nextCustom->show();
      installClickTracking(nextCustom);
    } else {
      contentLabel_->setText(request_.content);
      contentLabel_->show();
    }
  }

  void refreshSemantics() {
    AdMessage::SemanticStyles semantics = config_.semanticStyles;
    if (config_.semanticStyleResolver) {
      semantics = mergeSemanticStyles(
          semantics, config_.semanticStyleResolver(
                         {request_.type, request_.key, detail::widgetHovered(this)}));
    }
    semantics = mergeSemanticStyles(semantics, request_.semanticStyles);
    if (request_.semanticStyleResolver) {
      semantics = mergeSemanticStyles(
          semantics, request_.semanticStyleResolver(
                         {request_.type, request_.key, detail::widgetHovered(this)}));
    }

    resolvedContentBackground_ = semantics.root.backgroundColor.value_or(style_.contentBackground);
    resolvedBorderColor_ = semantics.root.borderColor.value_or(style_.borderColor);
    resolvedBorderWidth_ = semantics.root.borderColor.has_value()
                               ? std::max(1, style_.metrics.borderWidth)
                               : style_.metrics.borderWidth;
    resolvedContentTextColor_ = semantics.content.textColor.value_or(
        semantics.root.textColor.value_or(style_.contentTextColor));
    resolvedIconColor_ = semantics.icon.textColor.value_or(style_.iconColor);

    iconHost_->setSemanticColors(semantics.icon.backgroundColor, semantics.icon.borderColor);
    contentHost_->setSemanticColors(semantics.content.backgroundColor,
                                    semantics.content.borderColor);

    QPalette contentPalette = contentHost_->palette();
    contentPalette.setColor(QPalette::WindowText, resolvedContentTextColor_);
    contentPalette.setColor(QPalette::Text, resolvedContentTextColor_);
    contentHost_->setPalette(contentPalette);
    contentLabel_->setPalette(contentPalette);

    QPalette iconPalette = iconHost_->palette();
    iconPalette.setColor(QPalette::WindowText, resolvedIconColor_);
    iconPalette.setColor(QPalette::Text, resolvedIconColor_);
    iconHost_->setPalette(iconPalette);
    syncDefaultIconVisual();
    update();
  }

  void refreshLayoutMetrics() {
    const QMargins shadow = detail::antPopupShadowSecondaryMargins();
    const bool hasIcon = !iconHost_->isHidden();
    rootLayout_->setContentsMargins(shadow.left() + style_.metrics.contentPaddingHorizontal,
                                    shadow.top() + style_.metrics.contentPaddingVertical,
                                    shadow.right() + style_.metrics.contentPaddingHorizontal,
                                    shadow.bottom() + style_.metrics.contentPaddingVertical);
    rootLayout_->setSpacing(hasIcon ? style_.metrics.iconContentGap : 0);
    contentLabel_->setFont(style_.metrics.contentFont);
    contentHost_->setMinimumHeight(style_.metrics.contentLineHeight);

    const int surfaceWidth = std::max(1, maximumFrameWidth_ - shadow.left() - shadow.right());
    const int iconWidth = hasIcon ? iconHost_->sizeHint().width() : 0;
    const int gap = hasIcon ? style_.metrics.iconContentGap : 0;
    const int contentMaximum =
        std::max(24, surfaceWidth - 2 * style_.metrics.contentPaddingHorizontal - iconWidth - gap);
    contentLabel_->setMaximumWidth(contentMaximum);
    int naturalTextWidth = 0;
    const QFontMetrics textMetrics(contentLabel_->font());
    const QStringList lines = request_.content.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
      naturalTextWidth = std::max(naturalTextWidth, textMetrics.horizontalAdvance(line));
    }
    contentLabel_->setMinimumWidth(std::min(contentMaximum, naturalTextWidth + 8));
    if (customContent_) {
      customContent_->setMaximumWidth(contentMaximum);
    }
    setMaximumWidth(maximumFrameWidth_);
  }

  void installClickTracking(QWidget* widget) {
    if (!widget || trackedWidgets_.contains(widget)) {
      return;
    }
    trackedWidgets_.insert(widget);
    widget->installEventFilter(this);
    const auto children = widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : children) {
      installClickTracking(child);
    }
  }

  void dispatchClick() {
    if (clickDispatchedThisTurn_) {
      return;
    }
    clickDispatchedThisTurn_ = true;
    detail::deferTimingTask(this, QString::fromLatin1(kClickResetTaskKey),
                            [this]() { clickDispatchedThisTurn_ = false; });
    if (handle_) {
      emit handle_->clicked();
    }
    if (onClick_) {
      onClick_();
    }
  }

  QPointer<AdMessageHandle> handle_;
  AdMessage::Request request_;
  AdMessage::Config config_;
  detail::MessageVisualStyle style_;
  VoidCallback timeoutCallback_;
  VoidCallback layoutCallback_;
  AdMessage::Callback onClick_;
  AdMessage::Callback onClose_;

  QPointer<QHBoxLayout> rootLayout_;
  QPointer<MessageSlotHost> iconHost_;
  QPointer<QVBoxLayout> iconLayout_;
  QPointer<MessageIconWidget> defaultIcon_;
  QPointer<QWidget> customIcon_;
  QPointer<MessageSlotHost> contentHost_;
  QPointer<QVBoxLayout> contentLayout_;
  QPointer<QLabel> contentLabel_;
  QPointer<QWidget> customContent_;
  QPointer<QGraphicsOpacityEffect> opacityEffect_;

  detail::AnimatedScalar progress_;
  QSet<QWidget*> trackedWidgets_;
  adqt::icons::IconRef resolvedIcon_;
  QColor resolvedContentBackground_ = QColor("#ffffff");
  QColor resolvedContentTextColor_ = QColor("#141414");
  QColor resolvedIconColor_ = QColor("#1677ff");
  QColor resolvedBorderColor_ = QColor(Qt::transparent);
  int resolvedBorderWidth_ = 0;
  int maximumFrameWidth_ = 640;
  int remainingDurationMs_ = 0;
  qint64 expiryStartedMs_ = 0;
  bool hasResolvedIcon_ = false;

  bool closing_ = false;
  bool clickDispatchedThisTurn_ = false;
};

class MessageNoticeAccessible final : public QAccessibleWidget {
 public:
  explicit MessageNoticeAccessible(MessageNoticeWidget* notice)
      : QAccessibleWidget(notice, QAccessible::AlertMessage) {}

  QString text(QAccessible::Text type) const override {
    const auto* notice = dynamic_cast<const MessageNoticeWidget*>(object());
    if (notice && type == QAccessible::Name) {
      return notice->accessibleContent();
    }
    return QAccessibleWidget::text(type);
  }
};

QAccessibleInterface* messageAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* notice = dynamic_cast<MessageNoticeWidget*>(object)) {
    return new MessageNoticeAccessible(notice);
  }
  return nullptr;
}

void ensureMessageAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(messageAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

QWidget* resolveMessageOwner(QWidget* requested) {
  QWidget* owner = requested;
  if (!owner) {
    owner = QApplication::activeWindow();
  }
  if (!owner) {
    const auto topLevels = QApplication::topLevelWidgets();
    for (QWidget* candidate : topLevels) {
      if (candidate && candidate->isVisible() && candidate->isWindow()) {
        owner = candidate;
        break;
      }
    }
  }
  return owner ? owner->window() : nullptr;
}

struct GlobalMessageState {
  AdMessage::Config config;
  QHash<QWidget*, QPointer<AdMessage>> services;
};

GlobalMessageState& globalMessageState() {
  static auto* state = new GlobalMessageState;
  return *state;
}

}  // namespace

class AdMessagePrivate {
  Q_DECLARE_PUBLIC(AdMessage)

 public:
  struct Entry {
    QPointer<AdMessageHandle> handle;
    QPointer<MessageNoticeWidget> notice;
  };

  explicit AdMessagePrivate(AdMessage* q) : q_ptr(q) {}

  void attachOwner(QWidget* requested) {
    detachOwner();
    logicalOwner = requested;
    ownerWindow = requested ? requested->window() : nullptr;
    if (!ownerWindow) {
      return;
    }

    ownerWindow->installEventFilter(q_ptr);
    host = new MessageHostWidget(ownerWindow);
    host->setGeometry(ownerWindow->rect());
    host->hide();
    ownerDestroyedConnection = QObject::connect(ownerWindow, &QObject::destroyed, q_ptr, [this]() {
      acceptingRequests = false;
      closeAll(AdMessage::CloseReason::OwnerDestroyed, true);
      ownerDestroyedConnection = {};
      host.clear();
      ownerWindow.clear();
      logicalOwner.clear();
    });
    themeChangedConnection = QObject::connect(&adqt::theme::ThemeManager::instance(),
                                              &adqt::theme::ThemeManager::themeChanged, q_ptr,
                                              [this]() { refreshAllAppearance(); });
  }

  void detachOwner() {
    detail::cancelTimingTask(q_ptr, QString::fromLatin1(kRaiseTaskKey));
    QObject::disconnect(ownerDestroyedConnection);
    ownerDestroyedConnection = {};
    QObject::disconnect(themeChangedConnection);
    themeChangedConnection = {};
    if (ownerWindow) {
      ownerWindow->removeEventFilter(q_ptr);
    }
    delete host.data();
    host.clear();
    ownerWindow.clear();
    logicalOwner.clear();
  }

  adqt::theme::ResolvedTheme resolvedTheme() const {
    return adqt::theme::ThemeManager::instance().resolve(ownerWindow, logicalOwner);
  }

  int maximumFrameWidth() const {
    if (!host) {
      return 640;
    }
    return std::max(80, host->width() - 16);
  }

  detail::MessageVisualStyle styleFor(const AdMessage::Request& request) const {
    const AdMessage::ComponentTokens mergedTokens =
        mergeComponentTokens(config.componentTokens, request.componentTokens);
    const QFont baseFont = logicalOwner
                               ? logicalOwner->font()
                               : (ownerWindow ? ownerWindow->font() : QApplication::font());
    return detail::resolveMessageVisualStyle(request.type, baseFont, mergedTokens, resolvedTheme());
  }

  Entry* entryForHandle(const AdMessageHandle* handle) {
    for (Entry& entry : entries) {
      if (entry.handle == handle) {
        return &entry;
      }
    }
    return nullptr;
  }

  const Entry* entryForHandle(const AdMessageHandle* handle) const {
    for (const Entry& entry : entries) {
      if (entry.handle == handle) {
        return &entry;
      }
    }
    return nullptr;
  }

  int activeCount() const {
    int active = 0;
    for (const Entry& entry : entries) {
      if (entry.handle && entry.handle->open_) {
        ++active;
      }
    }
    return active;
  }

  void refreshAllAppearance() {
    for (Entry& entry : entries) {
      if (!entry.handle || !entry.notice) {
        continue;
      }
      entry.notice->refreshAppearance(config, styleFor(requests.value(entry.handle)),
                                      maximumFrameWidth());
    }
    relayout();
  }

  void updateHandleFromRequest(AdMessageHandle* handle, const AdMessage::Request& request) {
    if (!handle) {
      return;
    }
    const AdMessage::Type previousType = handle->type();
    const QString previousContent = handle->content_;
    handle->typeValue_ = static_cast<int>(request.type);
    handle->content_ = request.content;
    if (previousType != request.type) {
      emit handle->typeChanged(request.type);
    }
    if (previousContent != request.content) {
      emit handle->contentChanged(request.content);
    }
  }

  AdMessageHandle* open(AdMessage::Request request) {
    Q_Q(AdMessage);
    if (!acceptingRequests || !ownerWindow || !host) {
      return nullptr;
    }

    if (request.key.isEmpty()) {
      request.key = QStringLiteral("adqt-message-%1").arg(++nextKey);
    }

    if (AdMessageHandle* existing = byKey.value(request.key)) {
      if (existing->open_) {
        Entry* entry = entryForHandle(existing);
        if (entry && entry->notice) {
          requests[existing] = request;
          updateHandleFromRequest(existing, request);
          entry->notice->setRequest(request, config, styleFor(request), maximumFrameWidth(), true);
          existing->contentWidget_ = entry->notice->hostedContentWidget();
          relayout();
          announceMessage(entry->notice, entry->notice->accessibleContent(), request.type);
          return existing;
        }
      }
    }

    auto* handle = new AdMessageHandle(q, request.key);
    requests.insert(handle, request);
    updateHandleFromRequest(handle, request);

    auto* notice = new MessageNoticeWidget(
        handle,
        [guard = QPointer<AdMessage>(q), handle]() {
          if (guard) {
            guard->closeHandle(handle, AdMessage::CloseReason::Timeout);
          }
        },
        [this]() { relayout(); }, host);
    handle->noticeWidget_ = notice;
    notice->setRequest(request, config, styleFor(request), maximumFrameWidth(), false);
    handle->contentWidget_ = notice->hostedContentWidget();

    entries.append({handle, notice});
    byKey.insert(request.key, handle);
    host->show();
    restack();
    relayout();
    notice->animateIn();
    enforceMaximumCount();

    emit q->countChanged(activeCount());
    emit q->messageOpened(handle);
    announceMessage(notice, notice->accessibleContent(), request.type);
    return handle;
  }

  void enforceMaximumCount() {
    if (config.maximumCount <= 0) {
      return;
    }
    while (activeCount() > config.maximumCount) {
      AdMessageHandle* oldest = nullptr;
      for (const Entry& entry : entries) {
        if (entry.handle && entry.handle->open_) {
          oldest = entry.handle;
          break;
        }
      }
      if (!oldest) {
        break;
      }
      q_ptr->closeHandle(oldest, AdMessage::CloseReason::MaxCount);
    }
  }

  void close(AdMessageHandle* handle, AdMessage::CloseReason reason, bool immediate = false) {
    Q_Q(AdMessage);
    if (!handle || !handle->open_) {
      return;
    }
    Entry* entry = entryForHandle(handle);
    if (!entry || !entry->notice) {
      finalize(handle, reason);
      return;
    }

    handle->open_ = false;
    if (byKey.value(handle->key_) == handle) {
      byKey.remove(handle->key_);
    }
    emit handle->openChanged(false);
    emit q->countChanged(activeCount());

    const int duration = immediate ? 0 : entry->notice->animateOut();
    if (duration <= 0) {
      finalize(handle, reason);
      return;
    }

    detail::scheduleTimingTask(entry->notice, QString::fromLatin1(kFinalizeTaskKey), duration,
                               [this, guard = QPointer<AdMessageHandle>(handle), reason]() {
                                 if (guard) {
                                   finalize(guard, reason);
                                 }
                               });
  }

  void finalize(AdMessageHandle* handle, AdMessage::CloseReason reason) {
    if (!handle) {
      return;
    }

    int index = -1;
    QPointer<MessageNoticeWidget> notice;
    for (int i = 0; i < entries.size(); ++i) {
      if (entries.at(i).handle == handle) {
        index = i;
        notice = entries.at(i).notice;
        break;
      }
    }
    if (index < 0) {
      return;
    }

    if (handle->open_) {
      handle->open_ = false;
      if (byKey.value(handle->key_) == handle) {
        byKey.remove(handle->key_);
      }
      emit handle->openChanged(false);
      emit q_ptr->countChanged(activeCount());
    }

    AdMessage::Callback closeCallback;
    if (notice) {
      closeCallback = notice->takeCloseCallback();
      notice->hide();
    }
    entries.removeAt(index);
    requests.remove(handle);
    handle->noticeWidget_.clear();
    handle->contentWidget_.clear();

    relayout();
    QPointer<AdMessageHandle> handleGuard = handle;
    emit handle->closed(reason);
    if (closeCallback) {
      closeCallback();
    }
    if (notice) {
      notice->deleteLater();
    }
    if (handleGuard) {
      handleGuard->owner_.clear();
      handleGuard->deleteLater();
    }
  }

  void closeAll(AdMessage::CloseReason reason, bool immediate = false) {
    QList<QPointer<AdMessageHandle>> handles;
    handles.reserve(entries.size());
    for (const Entry& entry : entries) {
      if (entry.handle && entry.handle->open_) {
        handles.append(entry.handle);
      }
    }
    for (const QPointer<AdMessageHandle>& handle : handles) {
      if (handle) {
        close(handle, reason, immediate);
      }
    }
  }

  void relayout() {
    if (!host) {
      return;
    }
    host->setGeometry(ownerWindow ? ownerWindow->rect() : QRect());
    const int frameWidthLimit = maximumFrameWidth();
    qreal visualY = config.topOffset;
    QList<QWidget*> maskedNotices;
    int highestZIndex = 0;

    for (Entry& entry : entries) {
      if (!entry.notice) {
        continue;
      }
      highestZIndex = std::max(highestZIndex, entry.notice->zIndexPopup());
      entry.notice->setMaximumWidth(frameWidthLimit);
      entry.notice->adjustSize();
      QSize frameSize = entry.notice->sizeHint();
      frameSize.setWidth(std::min(frameSize.width(), frameWidthLimit));
      frameSize.setWidth(std::max(1, frameSize.width()));
      frameSize.setHeight(std::max(1, frameSize.height()));

      const QSize visualSize = detail::removeAntPopupShadowMargins(frameSize);
      const qreal progress = entry.notice->progress();
      const int padding = entry.notice->noticePadding();
      const qreal wrapperHeight = visualSize.height() + padding * 2.0;
      qreal currentVisualY = visualY + padding;
      if (!entry.notice->isClosing()) {
        currentVisualY -= (1.0 - progress) * visualSize.height();
      }

      const int frameX = qRound((host->width() - frameSize.width()) / 2.0);
      const int frameY = qRound(currentVisualY - detail::antPopupShadowSecondaryMargins().top());
      entry.notice->setGeometry(frameX, frameY, frameSize.width(), frameSize.height());
      entry.notice->setVisible(progress > 0.001 || !entry.notice->isClosing());
      if (entry.notice->isVisible() && progress > 0.001) {
        maskedNotices.append(entry.notice);
      }
      visualY += wrapperHeight * progress;
    }

    host->setNoticeMask(maskedNotices);
    host->setZIndex(highestZIndex);
    host->setVisible(!entries.isEmpty());
    if (!entries.isEmpty()) {
      restack();
    }
  }

  void restack() {
    if (!host || !ownerWindow) {
      return;
    }

    QVector<MessageHostWidget*> messageHosts;
    const QObjectList children = ownerWindow->children();
    for (QObject* child : children) {
      if (auto* candidate = dynamic_cast<MessageHostWidget*>(child)) {
        messageHosts.append(candidate);
      }
    }
    std::sort(messageHosts.begin(), messageHosts.end(),
              [](const MessageHostWidget* lhs, const MessageHostWidget* rhs) {
                if (lhs->zIndex() != rhs->zIndex()) {
                  return lhs->zIndex() < rhs->zIndex();
                }
                return lhs->sequence() < rhs->sequence();
              });
    for (MessageHostWidget* messageHost : messageHosts) {
      messageHost->raise();
    }
    for (Entry& entry : entries) {
      if (entry.notice) {
        entry.notice->raise();
      }
    }
  }

  void deferRestack() {
    detail::deferTimingTask(q_ptr, QString::fromLatin1(kRaiseTaskKey), [this]() {
      restack();
      relayout();
    });
  }

  bool handleOwnerEvent(QEvent* event) {
    if (!event) {
      return false;
    }
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::Move:
      case QEvent::LayoutRequest:
      case QEvent::Show:
      case QEvent::WindowStateChange:
        relayout();
        break;
      case QEvent::Hide:
        closeAll(AdMessage::CloseReason::ScopeHidden, true);
        break;
      case QEvent::ChildAdded:
      case QEvent::ChildPolished:
      case QEvent::ZOrderChange:
        deferRestack();
        break;
      case QEvent::PaletteChange:
      case QEvent::FontChange:
      case QEvent::StyleChange:
      case QEvent::ApplicationPaletteChange:
      case QEvent::ApplicationFontChange:
        refreshAllAppearance();
        break;
      default:
        break;
    }
    return false;
  }

  AdMessage* const q_ptr;
  AdMessage::Config config;
  QPointer<QWidget> logicalOwner;
  QPointer<QWidget> ownerWindow;
  QPointer<MessageHostWidget> host;
  QVector<Entry> entries;
  QHash<QString, QPointer<AdMessageHandle>> byKey;
  QHash<AdMessageHandle*, AdMessage::Request> requests;
  QMetaObject::Connection ownerDestroyedConnection;
  QMetaObject::Connection themeChangedConnection;
  quint64 nextKey = 0;
  bool acceptingRequests = true;
};

AdMessageHandle::AdMessageHandle(AdMessage* owner, const QString& key)
    : QObject(owner), owner_(owner), key_(key) {}

AdMessageHandle::~AdMessageHandle() = default;

QString AdMessageHandle::key() const { return key_; }

AdMessage::Type AdMessageHandle::type() const { return static_cast<AdMessage::Type>(typeValue_); }

QString AdMessageHandle::content() const { return content_; }

bool AdMessageHandle::isOpen() const { return open_; }

QWidget* AdMessageHandle::contentWidget() const { return contentWidget_; }

QWidget* AdMessageHandle::noticeWidget() const { return noticeWidget_; }

void AdMessageHandle::close() {
  if (owner_) {
    owner_->closeHandle(this, AdMessage::CloseReason::Manual);
  }
}

AdMessage::AdMessage(QWidget* ownerWindow, QObject* parent)
    : QObject(parent), d_ptr(new AdMessagePrivate(this)) {
  ensureMessageAccessibleFactoryInstalled();
  Q_D(AdMessage);
  d->attachOwner(ownerWindow);
}

AdMessage::~AdMessage() {
  Q_D(AdMessage);
  d->acceptingRequests = false;
  d->closeAll(CloseReason::OwnerDestroyed, true);
  d->detachOwner();
}

QWidget* AdMessage::ownerWindow() const {
  Q_D(const AdMessage);
  return d->logicalOwner;
}

void AdMessage::setOwnerWindow(QWidget* value) {
  Q_D(AdMessage);
  if (d->logicalOwner == value) {
    return;
  }
  d->acceptingRequests = false;
  d->closeAll(CloseReason::ScopeHidden, true);
  d->attachOwner(value);
  d->acceptingRequests = true;
  emit ownerWindowChanged(value);
}

AdMessage::Config AdMessage::config() const {
  Q_D(const AdMessage);
  return d->config;
}

void AdMessage::setConfig(const Config& value) {
  Q_D(AdMessage);
  const int oldTop = d->config.topOffset;
  const int oldDuration = d->config.defaultDurationMs;
  const int oldMaximum = d->config.maximumCount;
  const bool oldPause = d->config.pauseOnHover;

  d->config = value;
  d->config.defaultDurationMs = std::max(0, d->config.defaultDurationMs);
  d->config.maximumCount = std::max(0, d->config.maximumCount);
  d->refreshAllAppearance();
  d->enforceMaximumCount();

  if (oldTop != d->config.topOffset) {
    emit topOffsetChanged(d->config.topOffset);
  }
  if (oldDuration != d->config.defaultDurationMs) {
    emit defaultDurationMsChanged(d->config.defaultDurationMs);
  }
  if (oldMaximum != d->config.maximumCount) {
    emit maximumCountChanged(d->config.maximumCount);
  }
  if (oldPause != d->config.pauseOnHover) {
    emit pauseOnHoverChanged(d->config.pauseOnHover);
  }
  emit componentTokensChanged();
  emit semanticStylesChanged();
  emit configChanged();
}

int AdMessage::topOffset() const {
  Q_D(const AdMessage);
  return d->config.topOffset;
}

void AdMessage::setTopOffset(int value) {
  Q_D(AdMessage);
  if (d->config.topOffset == value) {
    return;
  }
  d->config.topOffset = value;
  d->relayout();
  emit topOffsetChanged(value);
  emit configChanged();
}

int AdMessage::defaultDurationMs() const {
  Q_D(const AdMessage);
  return d->config.defaultDurationMs;
}

void AdMessage::setDefaultDurationMs(int value) {
  Q_D(AdMessage);
  value = std::max(0, value);
  if (d->config.defaultDurationMs == value) {
    return;
  }
  d->config.defaultDurationMs = value;
  emit defaultDurationMsChanged(value);
  emit configChanged();
}

int AdMessage::maximumCount() const {
  Q_D(const AdMessage);
  return d->config.maximumCount;
}

void AdMessage::setMaximumCount(int value) {
  Q_D(AdMessage);
  value = std::max(0, value);
  if (d->config.maximumCount == value) {
    return;
  }
  d->config.maximumCount = value;
  d->enforceMaximumCount();
  emit maximumCountChanged(value);
  emit configChanged();
}

bool AdMessage::pauseOnHover() const {
  Q_D(const AdMessage);
  return d->config.pauseOnHover;
}

void AdMessage::setPauseOnHover(bool value) {
  Q_D(AdMessage);
  if (d->config.pauseOnHover == value) {
    return;
  }
  d->config.pauseOnHover = value;
  d->refreshAllAppearance();
  emit pauseOnHoverChanged(value);
  emit configChanged();
}

AdMessage::ComponentTokens AdMessage::componentTokens() const {
  Q_D(const AdMessage);
  return d->config.componentTokens;
}

void AdMessage::setComponentTokens(const ComponentTokens& value) {
  Q_D(AdMessage);
  d->config.componentTokens = value;
  d->refreshAllAppearance();
  emit componentTokensChanged();
  emit configChanged();
}

void AdMessage::resetComponentTokens() { setComponentTokens({}); }

AdMessage::SemanticStyles AdMessage::semanticStyles() const {
  Q_D(const AdMessage);
  return d->config.semanticStyles;
}

void AdMessage::setSemanticStyles(const SemanticStyles& value) {
  Q_D(AdMessage);
  d->config.semanticStyles = value;
  d->refreshAllAppearance();
  emit semanticStylesChanged();
  emit configChanged();
}

void AdMessage::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  Q_D(AdMessage);
  d->config.semanticStyleResolver = std::move(resolver);
  d->refreshAllAppearance();
  emit semanticStylesChanged();
  emit configChanged();
}

int AdMessage::count() const {
  Q_D(const AdMessage);
  return d->activeCount();
}

AdMessageHandle* AdMessage::open(Request request) {
  Q_D(AdMessage);
  return d->open(std::move(request));
}

AdMessageHandle* AdMessage::open(const QString& content, int durationMs) {
  Request request;
  request.content = content;
  if (durationMs >= 0) {
    request.durationMs = durationMs;
  }
  return open(std::move(request));
}

namespace {

AdMessageHandle* openTyped(AdMessage* messages, AdMessage::Request request, AdMessage::Type type) {
  request.type = type;
  return messages ? messages->open(std::move(request)) : nullptr;
}

AdMessage::Request textRequest(const QString& content, int durationMs) {
  AdMessage::Request request;
  request.content = content;
  if (durationMs >= 0) {
    request.durationMs = durationMs;
  }
  return request;
}

}  // namespace

AdMessageHandle* AdMessage::info(Request request) {
  return openTyped(this, std::move(request), Type::Info);
}

AdMessageHandle* AdMessage::info(const QString& content, int durationMs) {
  return info(textRequest(content, durationMs));
}

AdMessageHandle* AdMessage::success(Request request) {
  return openTyped(this, std::move(request), Type::Success);
}

AdMessageHandle* AdMessage::success(const QString& content, int durationMs) {
  return success(textRequest(content, durationMs));
}

AdMessageHandle* AdMessage::warning(Request request) {
  return openTyped(this, std::move(request), Type::Warning);
}

AdMessageHandle* AdMessage::warning(const QString& content, int durationMs) {
  return warning(textRequest(content, durationMs));
}

AdMessageHandle* AdMessage::error(Request request) {
  return openTyped(this, std::move(request), Type::Error);
}

AdMessageHandle* AdMessage::error(const QString& content, int durationMs) {
  return error(textRequest(content, durationMs));
}

AdMessageHandle* AdMessage::loading(Request request) {
  return openTyped(this, std::move(request), Type::Loading);
}

AdMessageHandle* AdMessage::loading(const QString& content, int durationMs) {
  return loading(textRequest(content, durationMs));
}

void AdMessage::destroy(const QString& key) {
  Q_D(AdMessage);
  if (AdMessageHandle* handle = d->byKey.value(key)) {
    closeHandle(handle, CloseReason::Destroyed);
  }
}

void AdMessage::destroyAll() {
  Q_D(AdMessage);
  d->closeAll(CloseReason::Destroyed);
}

bool AdMessage::eventFilter(QObject* watched, QEvent* event) {
  Q_D(AdMessage);
  if (watched == d->ownerWindow) {
    d->handleOwnerEvent(event);
  }
  return QObject::eventFilter(watched, event);
}

void AdMessage::closeHandle(AdMessageHandle* handle, CloseReason reason) {
  Q_D(AdMessage);
  d->close(handle, reason);
}

AdMessage* AdMessageService::instance(QWidget* ownerWindow) {
  QWidget* resolvedOwner = resolveMessageOwner(ownerWindow);
  if (!resolvedOwner) {
    return nullptr;
  }

  GlobalMessageState& state = globalMessageState();
  if (AdMessage* existing = state.services.value(resolvedOwner)) {
    return existing;
  }

  auto* messages = new AdMessage(ownerWindow ? ownerWindow : resolvedOwner, resolvedOwner);
  messages->setConfig(state.config);
  state.services.insert(resolvedOwner, messages);
  QObject::connect(resolvedOwner, &QObject::destroyed,
                   [resolvedOwner]() { globalMessageState().services.remove(resolvedOwner); });
  return messages;
}

AdMessage::Config AdMessageService::config() { return globalMessageState().config; }

void AdMessageService::setConfig(const AdMessage::Config& value) {
  GlobalMessageState& state = globalMessageState();
  state.config = value;
  const auto services = state.services.values();
  for (const QPointer<AdMessage>& service : services) {
    if (service) {
      service->setConfig(value);
    }
  }
}

AdMessageHandle* AdMessageService::open(AdMessage::Request request, QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->open(std::move(request)) : nullptr;
}

AdMessageHandle* AdMessageService::info(AdMessage::Request request, QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->info(std::move(request)) : nullptr;
}

AdMessageHandle* AdMessageService::info(const QString& content, int durationMs,
                                        QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->info(content, durationMs) : nullptr;
}

AdMessageHandle* AdMessageService::success(AdMessage::Request request, QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->success(std::move(request)) : nullptr;
}

AdMessageHandle* AdMessageService::success(const QString& content, int durationMs,
                                           QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->success(content, durationMs) : nullptr;
}

AdMessageHandle* AdMessageService::warning(AdMessage::Request request, QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->warning(std::move(request)) : nullptr;
}

AdMessageHandle* AdMessageService::warning(const QString& content, int durationMs,
                                           QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->warning(content, durationMs) : nullptr;
}

AdMessageHandle* AdMessageService::error(AdMessage::Request request, QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->error(std::move(request)) : nullptr;
}

AdMessageHandle* AdMessageService::error(const QString& content, int durationMs,
                                         QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->error(content, durationMs) : nullptr;
}

AdMessageHandle* AdMessageService::loading(AdMessage::Request request, QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->loading(std::move(request)) : nullptr;
}

AdMessageHandle* AdMessageService::loading(const QString& content, int durationMs,
                                           QWidget* ownerWindow) {
  AdMessage* messages = instance(ownerWindow);
  return messages ? messages->loading(content, durationMs) : nullptr;
}

void AdMessageService::destroy(const QString& key, QWidget* ownerWindow) {
  QWidget* resolvedOwner = resolveMessageOwner(ownerWindow);
  if (AdMessage* messages = globalMessageState().services.value(resolvedOwner)) {
    messages->destroy(key);
  }
}

void AdMessageService::destroyAll(QWidget* ownerWindow) {
  if (ownerWindow) {
    QWidget* resolvedOwner = resolveMessageOwner(ownerWindow);
    if (AdMessage* messages = globalMessageState().services.value(resolvedOwner)) {
      messages->destroyAll();
    }
    return;
  }

  const auto services = globalMessageState().services.values();
  for (const QPointer<AdMessage>& service : services) {
    if (service) {
      service->destroyAll();
    }
  }
}

}  // namespace adqt::widgets
