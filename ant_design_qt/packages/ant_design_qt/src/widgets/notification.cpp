#include "notification.h"
#include "detail/pointer_region.h"

#include "antd_icons.h"
#include "detail/animated_scalar.h"
#include "detail/popup_shadow.h"
#include "detail/timing_hub.h"
#include "notification_style.h"
#include "theme/theme.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QAbstractButton>
#include <QChildEvent>
#include <QEvent>
#include <QFocusEvent>
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
#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace adqt::widgets {

namespace {

constexpr char kExpiryTaskKey[] = "AdNotification.Expiry";
constexpr char kFinalizeTaskKey[] = "AdNotification.Finalize";
constexpr char kRaiseTaskKey[] = "AdNotification.Raise";
constexpr char kSpinnerFrameKey[] = "AdNotification.Spinner";
constexpr char kProgressFrameKey[] = "AdNotification.Progress";
constexpr char kClickResetTaskKey[] = "AdNotification.ClickReset";

AdNotification::Config normalizedConfig(AdNotification::Config config) {
  config.topOffset = std::max(0, config.topOffset);
  config.bottomOffset = std::max(0, config.bottomOffset);
  config.defaultDurationMs = std::max(0, config.defaultDurationMs);
  config.maximumCount = std::max(0, config.maximumCount);
  config.stackThreshold = std::max(1, config.stackThreshold);
  return config;
}

AdNotification::Request textRequest(const QString& title, const QString& description,
                                    int durationMs) {
  AdNotification::Request request;
  request.title = title;
  request.description = description;
  if (durationMs >= 0) {
    request.durationMs = durationMs;
  }
  return request;
}

quint64 nextNotificationHostSequence() {
  static quint64 sequence = 1;
  return sequence++;
}

AdNotification::ComponentTokens mergeComponentTokens(
    const AdNotification::ComponentTokens& base, const AdNotification::ComponentTokens& overlay) {
  AdNotification::ComponentTokens result = base;
#define ADQT_MERGE_NOTIFICATION_TOKEN(name) \
  if (overlay.name.has_value()) {           \
    result.name = overlay.name;             \
  }
  ADQT_MERGE_NOTIFICATION_TOKEN(zIndexPopup)
  ADQT_MERGE_NOTIFICATION_TOKEN(width)
  ADQT_MERGE_NOTIFICATION_TOKEN(backgroundColor)
  ADQT_MERGE_NOTIFICATION_TOKEN(successBackgroundColor)
  ADQT_MERGE_NOTIFICATION_TOKEN(infoBackgroundColor)
  ADQT_MERGE_NOTIFICATION_TOKEN(warningBackgroundColor)
  ADQT_MERGE_NOTIFICATION_TOKEN(errorBackgroundColor)
  ADQT_MERGE_NOTIFICATION_TOKEN(progressColor)
  ADQT_MERGE_NOTIFICATION_TOKEN(paddingHorizontal)
  ADQT_MERGE_NOTIFICATION_TOKEN(paddingVertical)
  ADQT_MERGE_NOTIFICATION_TOKEN(borderRadius)
  ADQT_MERGE_NOTIFICATION_TOKEN(iconSize)
  ADQT_MERGE_NOTIFICATION_TOKEN(iconContentGap)
  ADQT_MERGE_NOTIFICATION_TOKEN(titleDescriptionGap)
  ADQT_MERGE_NOTIFICATION_TOKEN(marginBottom)
  ADQT_MERGE_NOTIFICATION_TOKEN(edgeMargin)
  ADQT_MERGE_NOTIFICATION_TOKEN(progressHeight)
  ADQT_MERGE_NOTIFICATION_TOKEN(stackOffset)
  ADQT_MERGE_NOTIFICATION_TOKEN(stackGap)
#undef ADQT_MERGE_NOTIFICATION_TOKEN
  return result;
}

AdNotification::SemanticSlotStyle mergeSemanticSlot(
    const AdNotification::SemanticSlotStyle& base,
    const AdNotification::SemanticSlotStyle& overlay) {
  AdNotification::SemanticSlotStyle result = base;
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

AdNotification::SemanticStyles mergeSemanticStyles(const AdNotification::SemanticStyles& base,
                                                   const AdNotification::SemanticStyles& overlay) {
  AdNotification::SemanticStyles result;
  result.root = mergeSemanticSlot(base.root, overlay.root);
  result.icon = mergeSemanticSlot(base.icon, overlay.icon);
  result.title = mergeSemanticSlot(base.title, overlay.title);
  result.description = mergeSemanticSlot(base.description, overlay.description);
  result.actions = mergeSemanticSlot(base.actions, overlay.actions);
  return result;
}

adqt::icons::IconRef coloredIcon(adqt::icons::IconRef icon, const QColor& color) {
  if (!icon.isValid() || !color.isValid()) {
    return icon;
  }
  return icon.withColors(icon.colors().withPrimary(color));
}

adqt::icons::IconRef defaultIconForType(AdNotification::Type type) {
  using namespace adqt::icons::antd;
  switch (type) {
    case AdNotification::Type::Info:
      return filled::InfoCircle();
    case AdNotification::Type::Success:
      return filled::CheckCircle();
    case AdNotification::Type::Warning:
      return filled::ExclamationCircle();
    case AdNotification::Type::Error:
      return filled::CloseCircle();
    case AdNotification::Type::None:
      return {};
  }
  return {};
}

QAccessible::AnnouncementPoliteness announcementPoliteness(AdNotification::AccessibilityRole role) {
  return role == AdNotification::AccessibilityRole::Alert
             ? QAccessible::AnnouncementPoliteness::Assertive
             : QAccessible::AnnouncementPoliteness::Polite;
}

void announceNotification(QWidget* widget, const QString& content,
                          AdNotification::AccessibilityRole role) {
  if (!widget || content.trimmed().isEmpty()) {
    return;
  }
  QAccessibleAnnouncementEvent event(widget, content.trimmed());
  event.setPoliteness(announcementPoliteness(role));
  QAccessible::updateAccessibility(&event);
}

class NotificationSlotHost final : public QWidget {
 public:
  explicit NotificationSlotHost(QWidget* parent = nullptr) : QWidget(parent) {
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

class NotificationIconWidget final : public QWidget {
 public:
  explicit NotificationIconWidget(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
  }

  ~NotificationIconWidget() override {
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

class NotificationCloseButton final : public QAbstractButton {
 public:
  explicit NotificationCloseButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setObjectName(QStringLiteral("ad-notification-close"));
    setAccessibleName(AdNotification::tr("Close notification"));
    setToolTip(AdNotification::tr("Close"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    contentLayout_ = new QVBoxLayout(this);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
  }

  void setVisual(const adqt::icons::IconRef& icon, const QColor& color, const QColor& hoverColor,
                 const QColor& hoverBackground, const QColor& focusColor, int size, int radius,
                 qreal focusOutlineWidth) {
    icon_ = icon;
    color_ = color;
    hoverColor_ = hoverColor;
    hoverBackground_ = hoverBackground;
    focusColor_ = focusColor;
    radius_ = std::max(0, radius);
    focusOutlineWidth_ = std::max<qreal>(1.0, focusOutlineWidth);
    setFixedSize(std::max(1, size), std::max(1, size));
    update();
  }

  void setCustomContent(QWidget* content) {
    if (customContent_ == content) {
      return;
    }
    if (customContent_) {
      contentLayout_->removeWidget(customContent_);
      customContent_->deleteLater();
      customContent_.clear();
    }
    if (!content) {
      update();
      return;
    }

    content->setParent(this);
    content->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    content->setFocusPolicy(Qt::NoFocus);
    contentLayout_->addWidget(content, 0, Qt::AlignCenter);
    customContent_ = content;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (underMouse() || isDown()) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(hoverBackground_);
      painter.drawRoundedRect(rect(), radius_, radius_);
    }
    if (!customContent_) {
      adqt::icons::IconRef icon = coloredIcon(icon_, underMouse() ? hoverColor_ : color_);
      const int side = std::max(1, std::min(width(), height()) - 8);
      const QRectF bounds((width() - side) / 2.0, (height() - side) / 2.0, side, side);
      adqt::icons::paintIcon(&painter, icon, bounds);
    }
    if (focusVisible_ && focusColor_.isValid() && focusColor_.alpha() > 0) {
      const qreal inset = focusOutlineWidth_ / 2.0;
      painter.setPen(QPen(focusColor_, focusOutlineWidth_));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                              std::max<qreal>(0.0, radius_ - inset),
                              std::max<qreal>(0.0, radius_ - inset));
    }
  }

  void focusInEvent(QFocusEvent* event) override {
    QAbstractButton::focusInEvent(event);
    focusVisible_ =
        event && event->reason() != Qt::MouseFocusReason && event->reason() != Qt::NoFocusReason;
    update();
  }

  void focusOutEvent(QFocusEvent* event) override {
    QAbstractButton::focusOutEvent(event);
    focusVisible_ = false;
    update();
  }

  void enterEvent(QEnterEvent* event) override {
    QAbstractButton::enterEvent(event);
    update();
  }

  void leaveEvent(QEvent* event) override {
    QAbstractButton::leaveEvent(event);
    update();
  }

 private:
  adqt::icons::IconRef icon_;
  QPointer<QVBoxLayout> contentLayout_;
  QPointer<QWidget> customContent_;
  QColor color_;
  QColor hoverColor_;
  QColor hoverBackground_;
  QColor focusColor_;
  int radius_ = 4;
  qreal focusOutlineWidth_ = 2.0;
  bool focusVisible_ = false;
};

class NotificationHostWidget final : public QWidget {
 public:
  explicit NotificationHostWidget(QWidget* parent = nullptr)
      : QWidget(parent), sequence_(nextNotificationHostSequence()) {
    setObjectName(QStringLiteral("ad-notification-host"));
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

class NotificationNoticeWidget final : public QWidget {
 public:
  using VoidCallback = std::function<void()>;

  NotificationNoticeWidget(AdNotificationHandle* handle, VoidCallback timeoutCallback,
                           VoidCallback manualCloseCallback, VoidCallback layoutCallback,
                           QWidget* parent)
      : QWidget(parent),
        handle_(handle),
        timeoutCallback_(std::move(timeoutCallback)),
        manualCloseCallback_(std::move(manualCloseCallback)),
        layoutCallback_(std::move(layoutCallback)) {
    setObjectName(QStringLiteral("ad-notification-notice"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);

    rootLayout_ = new QHBoxLayout(this);
    iconHost_ = new NotificationSlotHost(this);
    iconHost_->setObjectName(QStringLiteral("ad-notification-icon"));
    iconLayout_ = new QVBoxLayout(iconHost_);
    iconLayout_->setContentsMargins(0, 0, 0, 0);
    defaultIcon_ = new NotificationIconWidget(iconHost_);
    defaultIcon_->setObjectName(QStringLiteral("ad-notification-default-icon"));
    iconLayout_->addWidget(defaultIcon_, 0, Qt::AlignTop);

    bodyHost_ = new NotificationSlotHost(this);
    bodyHost_->setObjectName(QStringLiteral("ad-notification-body"));
    bodyLayout_ = new QVBoxLayout(bodyHost_);
    bodyLayout_->setContentsMargins(0, 0, 0, 0);

    titleHost_ = new NotificationSlotHost(bodyHost_);
    titleHost_->setObjectName(QStringLiteral("ad-notification-title-host"));
    titleLayout_ = new QVBoxLayout(titleHost_);
    titleLayout_->setContentsMargins(0, 0, 0, 0);
    titleLabel_ = new QLabel(titleHost_);
    titleLabel_->setObjectName(QStringLiteral("ad-notification-title"));
    titleLabel_->setTextFormat(Qt::PlainText);
    titleLabel_->setWordWrap(true);
    titleLayout_->addWidget(titleLabel_);

    descriptionHost_ = new NotificationSlotHost(bodyHost_);
    descriptionHost_->setObjectName(QStringLiteral("ad-notification-description-host"));
    descriptionLayout_ = new QVBoxLayout(descriptionHost_);
    descriptionLayout_->setContentsMargins(0, 0, 0, 0);
    descriptionLabel_ = new QLabel(descriptionHost_);
    descriptionLabel_->setObjectName(QStringLiteral("ad-notification-description"));
    descriptionLabel_->setTextFormat(Qt::PlainText);
    descriptionLabel_->setWordWrap(true);
    descriptionLayout_->addWidget(descriptionLabel_);

    actionsHost_ = new NotificationSlotHost(bodyHost_);
    actionsHost_->setObjectName(QStringLiteral("ad-notification-actions"));
    actionsLayout_ = new QHBoxLayout(actionsHost_);
    actionsLayout_->setContentsMargins(0, 0, 0, 0);
    actionsLayout_->addStretch();

    bodyLayout_->addWidget(titleHost_);
    bodyLayout_->addWidget(descriptionHost_);
    bodyLayout_->addWidget(actionsHost_);

    closeHost_ = new NotificationSlotHost(this);
    auto* closeLayout = new QVBoxLayout(closeHost_);
    closeLayout->setContentsMargins(0, 0, 0, 0);
    defaultCloseButton_ = new NotificationCloseButton(closeHost_);
    closeLayout->addWidget(defaultCloseButton_, 0, Qt::AlignTop);
    QObject::connect(defaultCloseButton_, &QAbstractButton::clicked, this,
                     [this]() { dispatchManualClose(); });

    rootLayout_->addWidget(iconHost_, 0, Qt::AlignTop);
    rootLayout_->addWidget(bodyHost_, 1, Qt::AlignTop);
    rootLayout_->addWidget(closeHost_, 0, Qt::AlignTop);

    opacityEffect_ = new QGraphicsOpacityEffect(this);
    opacityEffect_->setOpacity(0.0);
    setGraphicsEffect(opacityEffect_);
    motionProgress_.configure(this, QStringLiteral("AdNotification.Motion"), [this]() {
      if (opacityEffect_) {
        opacityEffect_->setOpacity(std::clamp(motionProgress_.value(), 0.0, 1.0));
      }
      if (layoutCallback_) {
        layoutCallback_();
      }
    });
    motionProgress_.snapTo(0.0);
    installEventFilter(this);
    installClickTracking(iconHost_);
    installClickTracking(bodyHost_);
  }

  ~NotificationNoticeWidget() override {
    cancelExpiry();
    detail::cancelTimingTask(this, QString::fromLatin1(kFinalizeTaskKey));
  }

  void setRequest(AdNotification::Request request, const AdNotification::Config& config,
                  const detail::NotificationVisualStyle& style, int maximumFrameWidth,
                  bool animateUpdate) {
    request_ = std::move(request);
    config_ = config;
    style_ = style;
    onClick_ = request_.onClick;
    onClose_ = request_.onClose;
    onCloseButton_ = request_.onCloseButton;
    maximumFrameWidth_ = std::max(1, maximumFrameWidth);
    syncIconWidget();
    syncTextWidget(request_.titleWidget, customTitle_, titleLayout_, titleLabel_, request_.title);
    syncTextWidget(request_.descriptionWidget, customDescription_, descriptionLayout_,
                   descriptionLabel_, request_.description);
    syncActionsWidget();
    syncCloseWidget();
    refreshSemantics();
    refreshLayoutMetrics();
    restartExpiry();
    if (!animateUpdate) {
      motionProgress_.snapTo(0.0);
      opacityEffect_->setOpacity(0.0);
    }
    updateGeometry();
    adjustSize();
    update();
  }

  void refreshAppearance(const AdNotification::Config& config,
                         const detail::NotificationVisualStyle& style, int maximumFrameWidth) {
    const bool wasPaused = detail::widgetHovered(this) && effectivePauseOnHover();
    config_ = config;
    style_ = style;
    maximumFrameWidth_ = std::max(1, maximumFrameWidth);
    syncDefaultIconVisual();
    syncCloseWidget();
    refreshSemantics();
    refreshLayoutMetrics();
    const bool isPaused = detail::widgetHovered(this) && effectivePauseOnHover();
    if (!closing_ && wasPaused != isPaused && remainingDurationMs_ > 0) {
      if (isPaused) {
        captureRemainingDuration();
        cancelExpiry(false);
      } else {
        scheduleExpiry(remainingDurationMs_);
      }
    }
    adjustSize();
    update();
  }

  void animateIn() {
    closing_ = false;
    show();
    const int duration = style_.metrics.motionDurationMs;
    motionProgress_.animateTo(1.0, duration, [easing = style_.metrics.motionEasing](qreal value) {
      return easing.valueForProgress(value);
    });
    if (duration <= 0) {
      opacityEffect_->setOpacity(1.0);
      if (layoutCallback_) {
        layoutCallback_();
      }
    }
  }

  int animateOut() {
    closing_ = true;
    cancelExpiry();
    const int duration = style_.metrics.motionDurationMs;
    motionProgress_.animateTo(0.0, duration, [easing = style_.metrics.motionEasing](qreal value) {
      return easing.valueForProgress(value);
    });
    return duration;
  }

  qreal progress() const { return std::clamp(motionProgress_.value(), 0.0, 1.0); }
  bool isClosing() const { return closing_; }
  bool isHovered() const { return detail::widgetHovered(this); }
  int zIndexPopup() const { return style_.metrics.zIndexPopup; }
  int marginBottom() const { return style_.metrics.marginBottom; }
  int edgeMargin() const { return style_.metrics.edgeMargin; }
  int stackOffset() const { return style_.metrics.stackOffset; }
  int stackGap() const { return style_.metrics.stackGap; }
  AdNotification::Placement placement() const {
    return request_.placement.value_or(config_.defaultPlacement);
  }
  AdNotification::AccessibilityRole accessibilityRole() const { return request_.accessibilityRole; }

  QRect visualRect() const { return detail::antPopupShadowVisualRect(rect()).toAlignedRect(); }
  QSize visualSizeHint() const { return detail::removeAntPopupShadowMargins(sizeHint()); }

  AdNotification::Callback takeCloseCallback() {
    AdNotification::Callback callback = std::move(onClose_);
    onClose_ = {};
    return callback;
  }

  QString accessibleContent() const {
    QStringList parts;
    if (!request_.title.trimmed().isEmpty()) {
      parts.append(request_.title.trimmed());
    } else if (customTitle_ && !customTitle_->accessibleName().trimmed().isEmpty()) {
      parts.append(customTitle_->accessibleName().trimmed());
    }
    if (!request_.description.trimmed().isEmpty()) {
      parts.append(request_.description.trimmed());
    } else if (customDescription_ && !customDescription_->accessibleName().trimmed().isEmpty()) {
      parts.append(customDescription_->accessibleName().trimmed());
    }
    return parts.join(QStringLiteral(". "));
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!event) {
      return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Destroy) {
      trackedWidgets_.remove(qobject_cast<QWidget*>(watched));
    } else if (event->type() == QEvent::ChildAdded) {
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
      captureRemainingDuration();
      cancelExpiry(false);
    }
    if (layoutCallback_) {
      layoutCallback_();
    }
  }

  void leaveEvent(QEvent* event) override {
    QWidget::leaveEvent(event);

    refreshSemantics();
    if (effectivePauseOnHover() && !closing_ && remainingDurationMs_ > 0) {
      scheduleExpiry(remainingDurationMs_);
    }
    if (layoutCallback_) {
      layoutCallback_();
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
    painter.fillPath(path, resolvedBackgroundColor_);
    if (resolvedBorderWidth_ > 0 && resolvedBorderColor_.alpha() > 0) {
      painter.setPen(QPen(resolvedBorderColor_, resolvedBorderWidth_));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(path);
    }
    if (effectiveShowProgress() && totalDurationMs_ > 0 && style_.metrics.progressHeight > 0) {
      const qreal ratio = remainingRatio();
      const QRectF track(surface.left() + style_.metrics.borderRadius,
                         surface.bottom() - style_.metrics.progressHeight + 1,
                         std::max<qreal>(0, surface.width() - 2 * style_.metrics.borderRadius),
                         style_.metrics.progressHeight);
      painter.setPen(Qt::NoPen);
      painter.setBrush(style_.progressTrackColor);
      painter.drawRoundedRect(track, style_.metrics.progressHeight / 2.0,
                              style_.metrics.progressHeight / 2.0);
      QRectF value = track;
      value.setWidth(track.width() * ratio);
      painter.setBrush(style_.progressColor);
      painter.drawRoundedRect(value, style_.metrics.progressHeight / 2.0,
                              style_.metrics.progressHeight / 2.0);
    }
  }

 private:
  bool effectivePauseOnHover() const {
    return request_.pauseOnHover.value_or(config_.pauseOnHover);
  }
  bool effectiveShowProgress() const {
    return request_.showProgress.value_or(config_.showProgress);
  }
  bool effectiveClosable() const { return request_.closable.value_or(config_.closable); }
  int effectiveDurationMs() const {
    return request_.durationMs.value_or(config_.defaultDurationMs);
  }

  void captureRemainingDuration() {
    if (expiryStartedMs_ <= 0) {
      return;
    }
    remainingDurationMs_ = std::max(
        1, remainingDurationMs_ - static_cast<int>(detail::timingNowMs() - expiryStartedMs_));
  }

  qreal remainingRatio() const {
    if (totalDurationMs_ <= 0) {
      return 0.0;
    }
    int remaining = remainingDurationMs_;
    if (expiryStartedMs_ > 0) {
      remaining =
          std::max(0, remaining - static_cast<int>(detail::timingNowMs() - expiryStartedMs_));
    }
    return std::clamp(static_cast<qreal>(remaining) / totalDurationMs_, 0.0, 1.0);
  }

  void restartExpiry() {
    cancelExpiry();
    totalDurationMs_ = std::max(0, effectiveDurationMs());
    remainingDurationMs_ = totalDurationMs_;
    if (remainingDurationMs_ > 0 && !(detail::widgetHovered(this) && effectivePauseOnHover())) {
      scheduleExpiry(remainingDurationMs_);
    }
  }

  void cancelExpiry(bool clearStart = true) {
    detail::cancelTimingTask(this, QString::fromLatin1(kExpiryTaskKey));
    detail::clearFrameSubscription(this, QString::fromLatin1(kProgressFrameKey));
    if (clearStart) {
      expiryStartedMs_ = 0;
    }
  }

  void scheduleExpiry(int delayMs) {
    if (delayMs <= 0 || closing_) {
      return;
    }
    remainingDurationMs_ = delayMs;
    expiryStartedMs_ = detail::timingNowMs();
    if (effectiveShowProgress()) {
      detail::setFrameSubscription(this, QString::fromLatin1(kProgressFrameKey), true,
                                   [this](qint64, qint64) { update(); });
    }
    detail::scheduleTimingTask(this, QString::fromLatin1(kExpiryTaskKey), delayMs, [this]() {
      expiryStartedMs_ = 0;
      remainingDurationMs_ = 0;
      detail::clearFrameSubscription(this, QString::fromLatin1(kProgressFrameKey));
      update();
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
      nextCustom->setParent(iconHost_);
      customIcon_ = nextCustom;
      if (iconLayout_->indexOf(nextCustom) < 0) {
        iconLayout_->addWidget(nextCustom, 0, Qt::AlignTop);
      }
      defaultIcon_->hide();
      nextCustom->show();
      iconHost_->show();
      installClickTracking(nextCustom);
      return;
    }
    resolvedIcon_ = request_.icon.value_or(defaultIconForType(request_.type));
    syncDefaultIconVisual();
    defaultIcon_->setVisible(resolvedIcon_.isValid());
    iconHost_->setVisible(resolvedIcon_.isValid());
  }

  void syncDefaultIconVisual() {
    defaultIcon_->setVisual(coloredIcon(resolvedIcon_, resolvedIconColor_), style_.metrics.iconSize,
                            false, 1000);
  }

  void syncTextWidget(QWidget* next, QPointer<QWidget>& current, QVBoxLayout* layout, QLabel* label,
                      const QString& text) {
    if (current && current != next) {
      layout->removeWidget(current);
      current->deleteLater();
      current.clear();
    }
    if (next) {
      next->setParent(layout->parentWidget());
      current = next;
      if (layout->indexOf(next) < 0) {
        layout->addWidget(next);
      }
      label->hide();
      next->show();
      installClickTracking(next);
    } else {
      label->setText(text);
      label->show();
    }
  }

  void syncActionsWidget() {
    QWidget* next = request_.actionsWidget;
    if (customActions_ && customActions_ != next) {
      actionsLayout_->removeWidget(customActions_);
      customActions_->deleteLater();
      customActions_.clear();
    }
    if (next) {
      next->setParent(actionsHost_);
      customActions_ = next;
      if (actionsLayout_->indexOf(next) < 0) {
        actionsLayout_->addWidget(next);
      }
      next->show();
      actionsHost_->show();
      installClickTracking(next);
    } else {
      actionsHost_->hide();
    }
  }

  void syncCloseWidget() {
    const bool closable = effectiveClosable();
    QWidget* next = request_.closeIconWidget;
    using namespace adqt::icons::antd;
    const adqt::icons::IconRef icon = request_.closeIcon.value_or(outlined::Close());
    defaultCloseButton_->setVisual(icon, style_.closeColor, style_.closeHoverColor,
                                   style_.closeHoverBackground, style_.closeFocusColor,
                                   style_.metrics.closeButtonSize, style_.metrics.borderRadius / 2,
                                   style_.metrics.focusOutlineWidth);
    defaultCloseButton_->setCustomContent(next);
    defaultCloseButton_->show();
    closeHost_->setVisible(closable);
  }

  void refreshSemantics() {
    const AdNotification::Placement resolvedPlacement = placement();
    AdNotification::SemanticStyles semantics = config_.semanticStyles;
    if (config_.semanticStyleResolver) {
      semantics = mergeSemanticStyles(
          semantics, config_.semanticStyleResolver({request_.type, resolvedPlacement, request_.key,
                                                    detail::widgetHovered(this)}));
    }
    semantics = mergeSemanticStyles(semantics, request_.semanticStyles);
    if (request_.semanticStyleResolver) {
      semantics = mergeSemanticStyles(
          semantics, request_.semanticStyleResolver({request_.type, resolvedPlacement, request_.key,
                                                     detail::widgetHovered(this)}));
    }
    resolvedBackgroundColor_ = semantics.root.backgroundColor.value_or(style_.backgroundColor);
    resolvedBorderColor_ = semantics.root.borderColor.value_or(style_.borderColor);
    resolvedBorderWidth_ = semantics.root.borderColor.has_value()
                               ? std::max(1, style_.metrics.borderWidth)
                               : style_.metrics.borderWidth;
    resolvedIconColor_ = semantics.icon.textColor.value_or(style_.iconColor);
    const QColor titleColor =
        semantics.title.textColor.value_or(semantics.root.textColor.value_or(style_.titleColor));
    const QColor descriptionColor = semantics.description.textColor.value_or(
        semantics.root.textColor.value_or(style_.descriptionColor));
    auto applyTextColor = [](QWidget* host, QWidget* content, const QColor& color) {
      QPalette palette = host->palette();
      palette.setColor(QPalette::WindowText, color);
      palette.setColor(QPalette::Text, color);
      host->setPalette(palette);
      if (content) {
        content->setPalette(palette);
      }
    };
    iconHost_->setSemanticColors(semantics.icon.backgroundColor, semantics.icon.borderColor);
    titleHost_->setSemanticColors(semantics.title.backgroundColor, semantics.title.borderColor);
    descriptionHost_->setSemanticColors(semantics.description.backgroundColor,
                                        semantics.description.borderColor);
    actionsHost_->setSemanticColors(semantics.actions.backgroundColor,
                                    semantics.actions.borderColor);
    applyTextColor(titleHost_, customTitle_ ? customTitle_.data() : titleLabel_.data(), titleColor);
    applyTextColor(descriptionHost_,
                   customDescription_ ? customDescription_.data() : descriptionLabel_.data(),
                   descriptionColor);
    applyTextColor(actionsHost_, customActions_,
                   semantics.actions.textColor.value_or(
                       semantics.root.textColor.value_or(style_.descriptionColor)));
    syncDefaultIconVisual();
    update();
  }

  void refreshLayoutMetrics() {
    const QMargins shadow = detail::antPopupShadowSecondaryMargins();
    const bool hasIcon = iconHost_->isVisible();
    const bool hasClose = closeHost_->isVisible();
    rootLayout_->setContentsMargins(shadow.left() + style_.metrics.paddingHorizontal,
                                    shadow.top() + style_.metrics.paddingVertical,
                                    shadow.right() + style_.metrics.paddingHorizontal,
                                    shadow.bottom() + style_.metrics.paddingVertical);
    rootLayout_->setSpacing(style_.metrics.iconContentGap);
    bodyLayout_->setSpacing(style_.metrics.titleDescriptionGap);
    titleLabel_->setFont(style_.metrics.titleFont);
    descriptionLabel_->setFont(style_.metrics.descriptionFont);
    titleHost_->setMinimumHeight(style_.metrics.titleLineHeight);
    if (descriptionHost_->isVisible()) {
      descriptionHost_->setMinimumHeight(style_.metrics.descriptionLineHeight);
    }
    const int surfaceWidth = std::min(
        style_.metrics.width, std::max(1, maximumFrameWidth_ - shadow.left() - shadow.right()));
    const int bodyMaximum = std::max(
        24, surfaceWidth - 2 * style_.metrics.paddingHorizontal -
                (hasIcon ? style_.metrics.iconSize + style_.metrics.iconContentGap : 0) -
                (hasClose ? style_.metrics.closeButtonSize + style_.metrics.iconContentGap : 0));
    titleLabel_->setMaximumWidth(bodyMaximum);
    descriptionLabel_->setMaximumWidth(bodyMaximum);
    if (customTitle_) customTitle_->setMaximumWidth(bodyMaximum);
    if (customDescription_) customDescription_->setMaximumWidth(bodyMaximum);
    if (customActions_) customActions_->setMaximumWidth(bodyMaximum);
    descriptionHost_->setVisible(customDescription_ || !request_.description.isEmpty());
    titleHost_->setVisible(customTitle_ || !request_.title.isEmpty());
    setFixedWidth(surfaceWidth + shadow.left() + shadow.right());
  }

  void installClickTracking(QWidget* widget) {
    if (!widget || widget == closeHost_ || widget == defaultCloseButton_ ||
        trackedWidgets_.contains(widget)) {
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
    if (handle_) emit handle_->clicked();
    if (onClick_) onClick_();
  }

  void dispatchManualClose() {
    if (onCloseButton_) onCloseButton_();
    if (manualCloseCallback_) manualCloseCallback_();
  }

  QPointer<AdNotificationHandle> handle_;
  AdNotification::Request request_;
  AdNotification::Config config_;
  detail::NotificationVisualStyle style_;
  VoidCallback timeoutCallback_;
  VoidCallback manualCloseCallback_;
  VoidCallback layoutCallback_;
  AdNotification::Callback onClick_;
  AdNotification::Callback onClose_;
  AdNotification::Callback onCloseButton_;
  QPointer<QHBoxLayout> rootLayout_;
  QPointer<NotificationSlotHost> iconHost_;
  QPointer<QVBoxLayout> iconLayout_;
  QPointer<NotificationIconWidget> defaultIcon_;
  QPointer<QWidget> customIcon_;
  QPointer<NotificationSlotHost> bodyHost_;
  QPointer<QVBoxLayout> bodyLayout_;
  QPointer<NotificationSlotHost> titleHost_;
  QPointer<QVBoxLayout> titleLayout_;
  QPointer<QLabel> titleLabel_;
  QPointer<QWidget> customTitle_;
  QPointer<NotificationSlotHost> descriptionHost_;
  QPointer<QVBoxLayout> descriptionLayout_;
  QPointer<QLabel> descriptionLabel_;
  QPointer<QWidget> customDescription_;
  QPointer<NotificationSlotHost> actionsHost_;
  QPointer<QHBoxLayout> actionsLayout_;
  QPointer<QWidget> customActions_;
  QPointer<NotificationSlotHost> closeHost_;
  QPointer<NotificationCloseButton> defaultCloseButton_;
  QPointer<QGraphicsOpacityEffect> opacityEffect_;
  detail::AnimatedScalar motionProgress_;
  QSet<QWidget*> trackedWidgets_;
  adqt::icons::IconRef resolvedIcon_;
  QColor resolvedBackgroundColor_ = QColor("#ffffff");
  QColor resolvedIconColor_ = QColor("#1677ff");
  QColor resolvedBorderColor_ = QColor(Qt::transparent);
  int resolvedBorderWidth_ = 0;
  int maximumFrameWidth_ = 640;
  int totalDurationMs_ = 0;
  int remainingDurationMs_ = 0;
  qint64 expiryStartedMs_ = 0;

  bool closing_ = false;
  bool clickDispatchedThisTurn_ = false;
};

class NotificationNoticeAccessible final : public QAccessibleWidget {
 public:
  explicit NotificationNoticeAccessible(NotificationNoticeWidget* notice)
      : QAccessibleWidget(notice, QAccessible::AlertMessage) {}

  QAccessible::Role role() const override {
    const auto* notice = dynamic_cast<const NotificationNoticeWidget*>(object());
    return notice && notice->accessibilityRole() == AdNotification::AccessibilityRole::Status
               ? QAccessible::StaticText
               : QAccessible::AlertMessage;
  }

  QString text(QAccessible::Text type) const override {
    const auto* notice = dynamic_cast<const NotificationNoticeWidget*>(object());
    if (notice && type == QAccessible::Name) {
      return notice->accessibleContent();
    }
    return QAccessibleWidget::text(type);
  }
};

QAccessibleInterface* notificationAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* notice = dynamic_cast<NotificationNoticeWidget*>(object)) {
    return new NotificationNoticeAccessible(notice);
  }
  return nullptr;
}

void ensureNotificationAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(notificationAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

QWidget* resolveNotificationOwner(QWidget* requested) {
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

struct GlobalNotificationState {
  AdNotification::Config config;
  QHash<QWidget*, QPointer<AdNotification>> services;
};

GlobalNotificationState& globalNotificationState() {
  static auto* state = new GlobalNotificationState;
  return *state;
}

}  // namespace

class AdNotificationPrivate {
  Q_DECLARE_PUBLIC(AdNotification)

 public:
  struct Entry {
    QPointer<AdNotificationHandle> handle;
    QPointer<NotificationNoticeWidget> notice;
  };

  explicit AdNotificationPrivate(AdNotification* q) : q_ptr(q) {}

  void attachOwner(QWidget* requested) {
    detachOwner();
    logicalOwner = requested;
    ownerWindow = requested ? requested->window() : nullptr;
    if (!ownerWindow) {
      return;
    }

    ownerWindow->installEventFilter(q_ptr);
    host = new NotificationHostWidget(ownerWindow);
    host->setGeometry(ownerWindow->rect());
    host->hide();
    ownerDestroyedConnection = QObject::connect(ownerWindow, &QObject::destroyed, q_ptr, [this]() {
      acceptingRequests = false;
      closeAll(AdNotification::CloseReason::OwnerDestroyed, true);
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
    return std::max(1, host->width() - 16);
  }

  detail::NotificationVisualStyle styleFor(const AdNotification::Request& request) const {
    const AdNotification::ComponentTokens mergedTokens =
        mergeComponentTokens(config.componentTokens, request.componentTokens);
    const QFont baseFont = logicalOwner
                               ? logicalOwner->font()
                               : (ownerWindow ? ownerWindow->font() : QApplication::font());
    return detail::resolveNotificationVisualStyle(request.type, baseFont, mergedTokens,
                                                  resolvedTheme());
  }

  Entry* entryForHandle(const AdNotificationHandle* handle) {
    for (Entry& entry : entries) {
      if (entry.handle == handle) {
        return &entry;
      }
    }
    return nullptr;
  }

  const Entry* entryForHandle(const AdNotificationHandle* handle) const {
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

  void updateHandleFromRequest(AdNotificationHandle* handle,
                               const AdNotification::Request& request) {
    if (!handle) {
      return;
    }
    const AdNotification::Type previousType = handle->type();
    const QString previousTitle = handle->title_;
    const QString previousDescription = handle->description_;
    handle->typeValue_ = static_cast<int>(request.type);
    handle->title_ = request.title;
    handle->description_ = request.description;
    if (previousType != request.type) {
      emit handle->typeChanged(request.type);
    }
    if (previousTitle != request.title) {
      emit handle->titleChanged(request.title);
    }
    if (previousDescription != request.description) {
      emit handle->descriptionChanged(request.description);
    }
  }

  AdNotificationHandle* open(AdNotification::Request request) {
    Q_Q(AdNotification);
    if (!acceptingRequests || !ownerWindow || !host) {
      return nullptr;
    }

    if (request.key.isEmpty()) {
      request.key = QStringLiteral("adqt-notification-%1").arg(++nextKey);
    }

    if (AdNotificationHandle* existing = byKey.value(request.key)) {
      if (existing->open_) {
        Entry* entry = entryForHandle(existing);
        if (entry && entry->notice) {
          requests[existing] = request;
          updateHandleFromRequest(existing, request);
          entry->notice->setRequest(request, config, styleFor(request), maximumFrameWidth(), true);
          relayout();
          announceNotification(entry->notice, entry->notice->accessibleContent(),
                               request.accessibilityRole);
          return existing;
        }
      }
    }

    auto* handle = new AdNotificationHandle(q, request.key);
    requests.insert(handle, request);
    updateHandleFromRequest(handle, request);

    auto* notice = new NotificationNoticeWidget(
        handle,
        [guard = QPointer<AdNotification>(q), handle]() {
          if (guard) {
            guard->closeHandle(handle, AdNotification::CloseReason::Timeout);
          }
        },
        [guard = QPointer<AdNotification>(q), handle]() {
          if (guard) {
            guard->closeHandle(handle, AdNotification::CloseReason::Manual);
          }
        },
        [this]() { relayout(); }, host);
    handle->noticeWidget_ = notice;
    notice->setRequest(request, config, styleFor(request), maximumFrameWidth(), false);
    entries.append({handle, notice});
    byKey.insert(request.key, handle);
    host->show();
    restack();
    relayout();
    notice->animateIn();
    enforceMaximumCount();

    emit q->countChanged(activeCount());
    emit q->notificationOpened(handle);
    announceNotification(notice, notice->accessibleContent(), request.accessibilityRole);
    return handle;
  }

  void enforceMaximumCount() {
    if (config.maximumCount <= 0) {
      return;
    }
    while (activeCount() > config.maximumCount) {
      AdNotificationHandle* oldest = nullptr;
      for (const Entry& entry : entries) {
        if (entry.handle && entry.handle->open_) {
          oldest = entry.handle;
          break;
        }
      }
      if (!oldest) {
        break;
      }
      q_ptr->closeHandle(oldest, AdNotification::CloseReason::MaxCount);
    }
  }

  void close(AdNotificationHandle* handle, AdNotification::CloseReason reason,
             bool immediate = false) {
    Q_Q(AdNotification);
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
                               [this, guard = QPointer<AdNotificationHandle>(handle), reason]() {
                                 if (guard) {
                                   finalize(guard, reason);
                                 }
                               });
  }

  void finalize(AdNotificationHandle* handle, AdNotification::CloseReason reason) {
    if (!handle) {
      return;
    }

    int index = -1;
    QPointer<NotificationNoticeWidget> notice;
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

    AdNotification::Callback closeCallback;
    if (notice) {
      detail::cancelTimingTask(notice, QString::fromLatin1(kFinalizeTaskKey));
      closeCallback = notice->takeCloseCallback();
      notice->hide();
    }
    entries.removeAt(index);
    requests.remove(handle);
    handle->noticeWidget_.clear();

    relayout();
    QPointer<AdNotificationHandle> handleGuard = handle;
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

  void closeAll(AdNotification::CloseReason reason, bool immediate = false) {
    QList<QPointer<AdNotificationHandle>> handles;
    handles.reserve(entries.size());
    for (const Entry& entry : entries) {
      if (entry.handle) {
        handles.append(entry.handle);
      }
    }
    for (const QPointer<AdNotificationHandle>& handle : handles) {
      if (handle) {
        if (handle->open_) {
          close(handle, reason, immediate);
        } else if (immediate) {
          finalize(handle, reason);
        }
      }
    }
  }

  void relayout() {
    if (!host) {
      return;
    }
    host->setGeometry(ownerWindow ? ownerWindow->rect() : QRect());
    const int frameWidthLimit = maximumFrameWidth();
    QList<QWidget*> maskedNotices;
    int highestZIndex = 0;

    const std::array<AdNotification::Placement, 6> placements = {
        AdNotification::Placement::Top,        AdNotification::Placement::TopLeft,
        AdNotification::Placement::TopRight,   AdNotification::Placement::Bottom,
        AdNotification::Placement::BottomLeft, AdNotification::Placement::BottomRight};
    for (AdNotification::Placement placement : placements) {
      QVector<Entry*> group;
      for (Entry& entry : entries) {
        if (entry.notice && entry.notice->placement() == placement) {
          group.append(&entry);
        }
      }
      if (group.isEmpty()) {
        continue;
      }

      const bool top = placement == AdNotification::Placement::Top ||
                       placement == AdNotification::Placement::TopLeft ||
                       placement == AdNotification::Placement::TopRight;
      bool hovered = false;
      for (const Entry* entry : group) {
        hovered = hovered || (entry->notice && entry->notice->isHovered());
      }
      const int groupCount = static_cast<int>(group.size());
      const int threshold = std::max(1, config.stackThreshold);
      const bool collapsed = config.stackEnabled && groupCount > threshold && !hovered;
      qreal cursor = top ? config.topOffset : host->height() - config.bottomOffset;

      for (int i = 0; i < groupCount; ++i) {
        NotificationNoticeWidget* notice = group.at(i)->notice;
        highestZIndex = std::max(highestZIndex, notice->zIndexPopup());
        notice->setMaximumWidth(frameWidthLimit);
        notice->adjustSize();
        QSize frameSize = notice->sizeHint()
                              .expandedTo(notice->minimumSize())
                              .boundedTo(QSize(frameWidthLimit, host->height() * 2));
        frameSize.setWidth(std::max(1, frameSize.width()));
        frameSize.setHeight(std::max(1, frameSize.height()));
        const QSize visualSize = detail::removeAntPopupShadowMargins(frameSize);
        const qreal motion = notice->progress();
        const int fromNewest = groupCount - 1 - i;
        const bool stackVisible = !collapsed || fromNewest < threshold;

        qreal visualY = 0;
        if (collapsed) {
          const qreal layer = std::min(fromNewest, threshold - 1) * notice->stackOffset();
          visualY = top ? cursor + layer : cursor - visualSize.height() - layer;
        } else if (top) {
          visualY = cursor;
          const int gap = config.stackEnabled && groupCount > threshold && hovered
                              ? notice->stackGap()
                              : notice->marginBottom();
          cursor += (visualSize.height() + gap) * motion;
        } else {
          visualY = cursor - visualSize.height();
          const int gap = config.stackEnabled && groupCount > threshold && hovered
                              ? notice->stackGap()
                              : notice->marginBottom();
          cursor -= (visualSize.height() + gap) * motion;
        }

        qreal visualX = 0;
        if (placement == AdNotification::Placement::TopLeft ||
            placement == AdNotification::Placement::BottomLeft) {
          visualX = notice->edgeMargin();
        } else if (placement == AdNotification::Placement::TopRight ||
                   placement == AdNotification::Placement::BottomRight) {
          visualX = host->width() - notice->edgeMargin() - visualSize.width();
        } else {
          visualX = (host->width() - visualSize.width()) / 2.0;
        }

        visualX =
            std::clamp(visualX, 0.0, std::max<qreal>(0.0, host->width() - visualSize.width()));
        visualY =
            std::clamp(visualY, 0.0, std::max<qreal>(0.0, host->height() - visualSize.height()));
        if (!notice->isClosing()) {
          if (placement == AdNotification::Placement::TopLeft ||
              placement == AdNotification::Placement::BottomLeft) {
            visualX -= (1.0 - motion) * visualSize.width();
          } else if (placement == AdNotification::Placement::TopRight ||
                     placement == AdNotification::Placement::BottomRight) {
            visualX += (1.0 - motion) * visualSize.width();
          } else {
            visualY += (top ? -1.0 : 1.0) * (1.0 - motion) * visualSize.height();
          }
        }

        const QMargins shadow = detail::antPopupShadowSecondaryMargins();
        notice->setGeometry(qRound(visualX - shadow.left()), qRound(visualY - shadow.top()),
                            frameSize.width(), frameSize.height());
        const bool visible = stackVisible && (motion > 0.001 || !notice->isClosing());
        notice->setVisible(visible);
        if (visible && motion > 0.001) {
          maskedNotices.append(notice);
        }
      }
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

    QVector<NotificationHostWidget*> notificationHosts;
    const QObjectList children = ownerWindow->children();
    for (QObject* child : children) {
      if (auto* candidate = dynamic_cast<NotificationHostWidget*>(child)) {
        notificationHosts.append(candidate);
      }
    }
    std::sort(notificationHosts.begin(), notificationHosts.end(),
              [](const NotificationHostWidget* lhs, const NotificationHostWidget* rhs) {
                if (lhs->zIndex() != rhs->zIndex()) {
                  return lhs->zIndex() < rhs->zIndex();
                }
                return lhs->sequence() < rhs->sequence();
              });
    for (NotificationHostWidget* notificationHost : notificationHosts) {
      notificationHost->raise();
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
        closeAll(AdNotification::CloseReason::ScopeHidden, true);
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

  AdNotification* const q_ptr;
  AdNotification::Config config;
  QPointer<QWidget> logicalOwner;
  QPointer<QWidget> ownerWindow;
  QPointer<NotificationHostWidget> host;
  QVector<Entry> entries;
  QHash<QString, QPointer<AdNotificationHandle>> byKey;
  QHash<AdNotificationHandle*, AdNotification::Request> requests;
  QMetaObject::Connection ownerDestroyedConnection;
  QMetaObject::Connection themeChangedConnection;
  quint64 nextKey = 0;
  bool acceptingRequests = true;
};

AdNotificationHandle::AdNotificationHandle(AdNotification* owner, const QString& key)
    : QObject(owner), owner_(owner), key_(key) {}

AdNotificationHandle::~AdNotificationHandle() = default;

QString AdNotificationHandle::key() const { return key_; }

AdNotification::Type AdNotificationHandle::type() const {
  return static_cast<AdNotification::Type>(typeValue_);
}

QString AdNotificationHandle::title() const { return title_; }

QString AdNotificationHandle::description() const { return description_; }

bool AdNotificationHandle::isOpen() const { return open_; }

QWidget* AdNotificationHandle::noticeWidget() const { return noticeWidget_; }

void AdNotificationHandle::close() {
  if (owner_) {
    owner_->closeHandle(this, AdNotification::CloseReason::Manual);
  }
}

AdNotification::AdNotification(QWidget* ownerWindow, QObject* parent)
    : QObject(parent), d_ptr(new AdNotificationPrivate(this)) {
  ensureNotificationAccessibleFactoryInstalled();
  Q_D(AdNotification);
  d->attachOwner(ownerWindow);
}

AdNotification::~AdNotification() {
  Q_D(AdNotification);
  d->acceptingRequests = false;
  d->closeAll(CloseReason::OwnerDestroyed, true);
  d->detachOwner();
}

QWidget* AdNotification::ownerWindow() const {
  Q_D(const AdNotification);
  return d->logicalOwner;
}

void AdNotification::setOwnerWindow(QWidget* value) {
  Q_D(AdNotification);
  if (d->logicalOwner == value) {
    return;
  }
  d->acceptingRequests = false;
  d->closeAll(CloseReason::ScopeHidden, true);
  d->attachOwner(value);
  d->acceptingRequests = true;
  emit ownerWindowChanged(value);
}

AdNotification::Config AdNotification::config() const {
  Q_D(const AdNotification);
  return d->config;
}

void AdNotification::setConfig(const Config& value) {
  Q_D(AdNotification);
  const int oldTop = d->config.topOffset;
  const int oldBottom = d->config.bottomOffset;
  const int oldDuration = d->config.defaultDurationMs;
  const int oldMaximum = d->config.maximumCount;
  const bool oldPause = d->config.pauseOnHover;
  const bool oldProgress = d->config.showProgress;
  const bool oldStack = d->config.stackEnabled;

  d->config = normalizedConfig(value);
  d->refreshAllAppearance();
  d->enforceMaximumCount();

  if (oldTop != d->config.topOffset) {
    emit topOffsetChanged(d->config.topOffset);
  }
  if (oldBottom != d->config.bottomOffset) {
    emit bottomOffsetChanged(d->config.bottomOffset);
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
  if (oldProgress != d->config.showProgress) {
    emit showProgressChanged(d->config.showProgress);
  }
  if (oldStack != d->config.stackEnabled) {
    emit stackEnabledChanged(d->config.stackEnabled);
  }
  emit componentTokensChanged();
  emit semanticStylesChanged();
  emit configChanged();
}

int AdNotification::topOffset() const {
  Q_D(const AdNotification);
  return d->config.topOffset;
}

void AdNotification::setTopOffset(int value) {
  Q_D(AdNotification);
  value = std::max(0, value);
  if (d->config.topOffset == value) {
    return;
  }
  d->config.topOffset = value;
  d->relayout();
  emit topOffsetChanged(value);
  emit configChanged();
}

int AdNotification::bottomOffset() const {
  Q_D(const AdNotification);
  return d->config.bottomOffset;
}

void AdNotification::setBottomOffset(int value) {
  Q_D(AdNotification);
  value = std::max(0, value);
  if (d->config.bottomOffset == value) {
    return;
  }
  d->config.bottomOffset = value;
  d->relayout();
  emit bottomOffsetChanged(value);
  emit configChanged();
}

int AdNotification::defaultDurationMs() const {
  Q_D(const AdNotification);
  return d->config.defaultDurationMs;
}

void AdNotification::setDefaultDurationMs(int value) {
  Q_D(AdNotification);
  value = std::max(0, value);
  if (d->config.defaultDurationMs == value) {
    return;
  }
  d->config.defaultDurationMs = value;
  emit defaultDurationMsChanged(value);
  emit configChanged();
}

int AdNotification::maximumCount() const {
  Q_D(const AdNotification);
  return d->config.maximumCount;
}

void AdNotification::setMaximumCount(int value) {
  Q_D(AdNotification);
  value = std::max(0, value);
  if (d->config.maximumCount == value) {
    return;
  }
  d->config.maximumCount = value;
  d->enforceMaximumCount();
  emit maximumCountChanged(value);
  emit configChanged();
}

bool AdNotification::pauseOnHover() const {
  Q_D(const AdNotification);
  return d->config.pauseOnHover;
}

void AdNotification::setPauseOnHover(bool value) {
  Q_D(AdNotification);
  if (d->config.pauseOnHover == value) {
    return;
  }
  d->config.pauseOnHover = value;
  d->refreshAllAppearance();
  emit pauseOnHoverChanged(value);
  emit configChanged();
}

bool AdNotification::showProgress() const {
  Q_D(const AdNotification);
  return d->config.showProgress;
}

void AdNotification::setShowProgress(bool value) {
  Q_D(AdNotification);
  if (d->config.showProgress == value) {
    return;
  }
  d->config.showProgress = value;
  d->refreshAllAppearance();
  emit showProgressChanged(value);
  emit configChanged();
}

bool AdNotification::stackEnabled() const {
  Q_D(const AdNotification);
  return d->config.stackEnabled;
}

void AdNotification::setStackEnabled(bool value) {
  Q_D(AdNotification);
  if (d->config.stackEnabled == value) {
    return;
  }
  d->config.stackEnabled = value;
  d->relayout();
  emit stackEnabledChanged(value);
  emit configChanged();
}

AdNotification::ComponentTokens AdNotification::componentTokens() const {
  Q_D(const AdNotification);
  return d->config.componentTokens;
}

void AdNotification::setComponentTokens(const ComponentTokens& value) {
  Q_D(AdNotification);
  d->config.componentTokens = value;
  d->refreshAllAppearance();
  emit componentTokensChanged();
  emit configChanged();
}

void AdNotification::resetComponentTokens() { setComponentTokens({}); }

AdNotification::SemanticStyles AdNotification::semanticStyles() const {
  Q_D(const AdNotification);
  return d->config.semanticStyles;
}

void AdNotification::setSemanticStyles(const SemanticStyles& value) {
  Q_D(AdNotification);
  d->config.semanticStyles = value;
  d->refreshAllAppearance();
  emit semanticStylesChanged();
  emit configChanged();
}

void AdNotification::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  Q_D(AdNotification);
  d->config.semanticStyleResolver = std::move(resolver);
  d->refreshAllAppearance();
  emit semanticStylesChanged();
  emit configChanged();
}

int AdNotification::count() const {
  Q_D(const AdNotification);
  return d->activeCount();
}

AdNotificationHandle* AdNotification::open(Request request) {
  Q_D(AdNotification);
  return d->open(std::move(request));
}

AdNotificationHandle* AdNotification::open(const QString& title, const QString& description,
                                           int durationMs) {
  return open(textRequest(title, description, durationMs));
}

namespace {

AdNotificationHandle* openTyped(AdNotification* notifications, AdNotification::Request request,
                                AdNotification::Type type) {
  request.type = type;
  return notifications ? notifications->open(std::move(request)) : nullptr;
}

}  // namespace

AdNotificationHandle* AdNotification::info(Request request) {
  return openTyped(this, std::move(request), Type::Info);
}

AdNotificationHandle* AdNotification::info(const QString& title, const QString& description,
                                           int durationMs) {
  return info(textRequest(title, description, durationMs));
}

AdNotificationHandle* AdNotification::success(Request request) {
  return openTyped(this, std::move(request), Type::Success);
}

AdNotificationHandle* AdNotification::success(const QString& title, const QString& description,
                                              int durationMs) {
  return success(textRequest(title, description, durationMs));
}

AdNotificationHandle* AdNotification::warning(Request request) {
  return openTyped(this, std::move(request), Type::Warning);
}

AdNotificationHandle* AdNotification::warning(const QString& title, const QString& description,
                                              int durationMs) {
  return warning(textRequest(title, description, durationMs));
}

AdNotificationHandle* AdNotification::error(Request request) {
  return openTyped(this, std::move(request), Type::Error);
}

AdNotificationHandle* AdNotification::error(const QString& title, const QString& description,
                                            int durationMs) {
  return error(textRequest(title, description, durationMs));
}

void AdNotification::destroy(const QString& key) {
  Q_D(AdNotification);
  if (AdNotificationHandle* handle = d->byKey.value(key)) {
    closeHandle(handle, CloseReason::Destroyed);
  }
}

void AdNotification::destroyAll() {
  Q_D(AdNotification);
  d->closeAll(CloseReason::Destroyed);
}

bool AdNotification::eventFilter(QObject* watched, QEvent* event) {
  Q_D(AdNotification);
  if (watched == d->ownerWindow) {
    d->handleOwnerEvent(event);
  }
  return QObject::eventFilter(watched, event);
}

void AdNotification::closeHandle(AdNotificationHandle* handle, CloseReason reason) {
  Q_D(AdNotification);
  d->close(handle, reason);
}

AdNotification* AdNotificationService::instance(QWidget* ownerWindow) {
  QWidget* resolvedOwner = resolveNotificationOwner(ownerWindow);
  if (!resolvedOwner) {
    return nullptr;
  }

  GlobalNotificationState& state = globalNotificationState();
  if (AdNotification* existing = state.services.value(resolvedOwner)) {
    return existing;
  }

  auto* notifications =
      new AdNotification(ownerWindow ? ownerWindow : resolvedOwner, resolvedOwner);
  notifications->setConfig(state.config);
  state.services.insert(resolvedOwner, notifications);
  QObject::connect(resolvedOwner, &QObject::destroyed,
                   [resolvedOwner]() { globalNotificationState().services.remove(resolvedOwner); });
  return notifications;
}

AdNotification::Config AdNotificationService::config() { return globalNotificationState().config; }

void AdNotificationService::setConfig(const AdNotification::Config& value) {
  GlobalNotificationState& state = globalNotificationState();
  state.config = normalizedConfig(value);
  const auto services = state.services.values();
  for (const QPointer<AdNotification>& service : services) {
    if (service) {
      service->setConfig(state.config);
    }
  }
}

AdNotificationHandle* AdNotificationService::open(AdNotification::Request request,
                                                  QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->open(std::move(request)) : nullptr;
}

AdNotificationHandle* AdNotificationService::open(const QString& title, const QString& description,
                                                  int durationMs, QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->open(title, description, durationMs) : nullptr;
}

AdNotificationHandle* AdNotificationService::info(AdNotification::Request request,
                                                  QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->info(std::move(request)) : nullptr;
}

AdNotificationHandle* AdNotificationService::info(const QString& title, const QString& description,
                                                  int durationMs, QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->info(title, description, durationMs) : nullptr;
}

AdNotificationHandle* AdNotificationService::success(AdNotification::Request request,
                                                     QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->success(std::move(request)) : nullptr;
}

AdNotificationHandle* AdNotificationService::success(const QString& title,
                                                     const QString& description, int durationMs,
                                                     QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->success(title, description, durationMs) : nullptr;
}

AdNotificationHandle* AdNotificationService::warning(AdNotification::Request request,
                                                     QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->warning(std::move(request)) : nullptr;
}

AdNotificationHandle* AdNotificationService::warning(const QString& title,
                                                     const QString& description, int durationMs,
                                                     QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->warning(title, description, durationMs) : nullptr;
}

AdNotificationHandle* AdNotificationService::error(AdNotification::Request request,
                                                   QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->error(std::move(request)) : nullptr;
}

AdNotificationHandle* AdNotificationService::error(const QString& title, const QString& description,
                                                   int durationMs, QWidget* ownerWindow) {
  AdNotification* notifications = instance(ownerWindow);
  return notifications ? notifications->error(title, description, durationMs) : nullptr;
}

void AdNotificationService::destroy(const QString& key, QWidget* ownerWindow) {
  QWidget* resolvedOwner = resolveNotificationOwner(ownerWindow);
  if (AdNotification* notifications = globalNotificationState().services.value(resolvedOwner)) {
    notifications->destroy(key);
  }
}

void AdNotificationService::destroyAll(QWidget* ownerWindow) {
  if (ownerWindow) {
    QWidget* resolvedOwner = resolveNotificationOwner(ownerWindow);
    if (AdNotification* notifications = globalNotificationState().services.value(resolvedOwner)) {
      notifications->destroyAll();
    }
    return;
  }

  const auto services = globalNotificationState().services.values();
  for (const QPointer<AdNotification>& service : services) {
    if (service) {
      service->destroyAll();
    }
  }
}

}  // namespace adqt::widgets
