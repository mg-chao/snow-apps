#include "button.h"

#include "button_style.h"
#include "detail/button_grouping.h"
#include "detail/button_rendering.h"
#include "detail/timing_hub.h"
#include "antd_icons.h"
#include "interaction_overlay_manager.h"
#include "theme/theme.h"

#include <QDebug>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QStyle>
#include <QStyleOptionButton>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace adqt::widgets {

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;
constexpr char kLoadingDelayTaskKey[] = "AdButton.LoadingDelay";
constexpr char kSpinnerFrameKey[] = "AdButton.SpinnerFrame";

using CornerRadii = detail::ButtonCornerRadii;

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

bool buttonStyleInputsEqual(const detail::ButtonStyleInput& lhs,
                            const detail::ButtonStyleInput& rhs) {
  return lhs.buttonStyle == rhs.buttonStyle && lhs.accentRole == rhs.accentRole &&
         lhs.sizeClass == rhs.sizeClass && lhs.flat == rhs.flat &&
         lhs.defaultButton == rhs.defaultButton && lhs.hasMenu == rhs.hasMenu &&
         lhs.baseFont == rhs.baseFont;
}

struct ButtonIconRenderState {
  bool busyIndicatorVisible = false;
  bool hasTokenIcon = false;
  bool hasFallbackIcon = false;
  adqt::icons::IconRef token;

  bool hasVisibleIcon() const { return busyIndicatorVisible || hasTokenIcon || hasFallbackIcon; }
  bool drawBuiltinSpinner() const { return busyIndicatorVisible && !hasTokenIcon; }
};

ButtonIconRenderState resolveIconRenderState(const AdButton& button, bool busyIndicatorVisible) {
  ButtonIconRenderState state;
  state.busyIndicatorVisible = busyIndicatorVisible;
  state.token = state.busyIndicatorVisible ? button.busyIconRef() : button.iconRef();
  state.hasTokenIcon = adqt::icons::isValid(state.token);
  state.hasFallbackIcon =
      !state.busyIndicatorVisible && !state.hasTokenIcon && !button.icon().isNull();
  return state;
}

QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft) {
  return detail::roundedButtonPath(rect, topLeft, topRight, bottomRight, bottomLeft);
}

bool isTwoChineseCharacters(const QString& text) {
  static const QRegularExpression re(QStringLiteral("^[\\x{4e00}-\\x{9fa5}]{2}$"));
  return re.match(text).hasMatch();
}

bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

bool isActivationKey(int key) {
  return key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter;
}

bool isEnterKey(int key) { return key == Qt::Key_Return || key == Qt::Key_Enter; }

Qt::WindowFlags busyIndicatorWindowFlags(const AdButton* button) {
  Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                          Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput;
  const QWidget* owner = button ? button->window() : nullptr;
  if (owner && owner->windowFlags().testFlag(Qt::WindowStaysOnTopHint)) {
    flags |= Qt::WindowStaysOnTopHint;
  }
  return flags;
}

QPoint mouseEventPos(const QMouseEvent* event) {
  return event ? event->position().toPoint() : QPoint();
}

bool hasVisibleColor(const QColor& color) { return color.isValid() && color.alpha() > 0; }

int mnemonicTextFlags(const QWidget* widget) {
  int flags = Qt::TextSingleLine | Qt::TextShowMnemonic;
  if (widget && widget->style() &&
      widget->style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, widget) == 0) {
    flags |= Qt::TextHideMnemonic;
  }
  return flags;
}

QString stripMnemonicMarkers(const QString& text) {
  QString result;
  result.reserve(text.size());
  for (int i = 0; i < text.size(); ++i) {
    const QChar ch = text.at(i);
    if (ch != QLatin1Char('&')) {
      result.append(ch);
      continue;
    }

    if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
      result.append(QLatin1Char('&'));
      ++i;
    }
  }
  return result;
}

bool hasMnemonicMarker(const QString& text) {
  for (int i = 0; i < text.size(); ++i) {
    if (text.at(i) != QLatin1Char('&')) {
      continue;
    }
    if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
      ++i;
      continue;
    }
    return true;
  }
  return false;
}

bool shouldInheritCurrentColor(const adqt::icons::IconRef& icon) {
  if (!adqt::icons::isValid(icon)) {
    return false;
  }
  if (!icon.colors().isEmpty()) {
    return false;
  }
  const adqt::icons::IconMetadata meta = adqt::icons::describeIcon(icon);
  return meta.colorModel == adqt::icons::IconColorModel::Monochrome;
}

QColor parseThemeColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QColor compositeOn(const QColor& foreground, const QColor& background) {
  if (!foreground.isValid()) {
    return background;
  }
  const float fgAlpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
  QColor mixed;
  mixed.setRedF(foreground.redF() * fgAlpha + background.redF() * (1.0F - fgAlpha));
  mixed.setGreenF(foreground.greenF() * fgAlpha + background.greenF() * (1.0F - fgAlpha));
  mixed.setBlueF(foreground.blueF() * fgAlpha + background.blueF() * (1.0F - fgAlpha));
  mixed.setAlpha(255);
  return mixed;
}

int resolveIconSide(const QAbstractButton* button, const QFontMetrics& fm,
                    const QFont& contentFont) {
  int iconSide = contentFont.pixelSize();
  if (iconSide <= 0) {
    const qreal pointSize = contentFont.pointSizeF();
    if (pointSize > 0.0) {
      iconSide = qRound(pointSize);
    }
  }
  if (iconSide <= 0) {
    iconSide = fm.height();
  }
  iconSide = std::max(10, iconSide);

  const QSize requestedIconSize = button->iconSize();
  if (!requestedIconSize.isValid() || requestedIconSize.isEmpty()) {
    return iconSide;
  }

  const int requestedSide = std::max(requestedIconSize.width(), requestedIconSize.height());
  int defaultStyleSide = -1;
  if (button->style()) {
    defaultStyleSide = button->style()->pixelMetric(QStyle::PM_ButtonIconSize, nullptr,
                                                    const_cast<QAbstractButton*>(button));
  }
  if (defaultStyleSide > 0 && requestedSide == defaultStyleSide) {
    return iconSide;
  }
  return requestedSide;
}

int measureTextWidth(const QFontMetrics& fm, const QString& text, int flags = 0) {
  if (text.isEmpty()) {
    return 0;
  }
  int width = 0;
  if (flags != 0) {
    const QSize measured = fm.size(flags, text);
    if (measured.width() > 0) {
      width = std::max(width, measured.width());
    }
  }
  const bool mnemonicAware = (flags & Qt::TextShowMnemonic) || (flags & Qt::TextHideMnemonic);
  const QString displayText = mnemonicAware ? stripMnemonicMarkers(text) : text;
  const int advance = fm.horizontalAdvance(displayText);
  if (advance > 0) {
    width = std::max(width, advance);
  }
  width = std::max(width, fm.boundingRect(displayText).width());
  return width;
}

int resolveContentPixelSize(const QFontMetrics& fm, const QFont& contentFont) {
  int contentPx = contentFont.pixelSize();
  if (contentPx <= 0) {
    const qreal pointSize = contentFont.pointSizeF();
    if (pointSize > 0.0) {
      contentPx = qRound(pointSize);
    }
  }
  if (contentPx <= 0) {
    contentPx = fm.height();
  }
  return std::max(contentPx, 1);
}

int twoCnLetterSpacingPx(const QFontMetrics& fm, const QFont& contentFont) {
  return std::max(1, qRound(resolveContentPixelSize(fm, contentFont) * 0.34));
}

int measureDisplayTextWidth(const QFontMetrics& fm, const QFont& contentFont, const QString& text,
                            bool twoCnAutoSpacing, int textFlags) {
  if (text.isEmpty()) {
    return 0;
  }
  if (!twoCnAutoSpacing || !isTwoChineseCharacters(text)) {
    return measureTextWidth(fm, text, textFlags);
  }
  const int first = measureTextWidth(fm, QString(text.at(0)), textFlags);
  const int second = measureTextWidth(fm, QString(text.at(1)), textFlags);
  return std::max(0, first) + std::max(0, second) + twoCnLetterSpacingPx(fm, contentFont);
}

qreal snapToDevicePixel(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  return qRound(value * dpr) / dpr;
}

void drawCenteredPixmap(QPainter& painter, const QRect& iconRect, const QPixmap& pixmap,
                        qreal pixmapDpr, qreal rotationDegrees = 0.0) {
  if (pixmap.isNull()) {
    return;
  }

  const QRectF iconRectF(iconRect);
  const QPointF iconCenter = iconRectF.center();
  const QSizeF drawSize = pixmap.deviceIndependentSize();

  if (std::abs(rotationDegrees) > 0.001) {
    painter.save();
    painter.translate(iconCenter);
    painter.rotate(rotationDegrees);
    painter.drawPixmap(QPointF(-drawSize.width() / 2.0, -drawSize.height() / 2.0), pixmap);
    painter.restore();
    return;
  }

  QPointF drawTopLeft(iconCenter.x() - drawSize.width() / 2.0,
                      iconCenter.y() - drawSize.height() / 2.0);
  drawTopLeft.setX(snapToDevicePixel(drawTopLeft.x(), pixmapDpr));
  drawTopLeft.setY(snapToDevicePixel(drawTopLeft.y(), pixmapDpr));
  painter.drawPixmap(drawTopLeft, pixmap);
}

void drawRotatingTokenIcon(QPainter& painter, const QRect& iconRect,
                           const adqt::icons::IconRef& token, qreal rotationDegrees) {
  if (!adqt::icons::isValid(token) || iconRect.isEmpty()) {
    return;
  }

  const QRectF iconRectF(iconRect);
  const QPointF iconCenter = iconRectF.center();
  painter.save();
  painter.translate(iconCenter);
  painter.rotate(rotationDegrees);
  adqt::icons::paintIcon(&painter, token,
                         QRectF(-iconRectF.width() / 2.0, -iconRectF.height() / 2.0,
                                iconRectF.width(), iconRectF.height()));
  painter.restore();
}

void drawMenuIndicator(QPainter& painter, const QRect& indicatorRect, const QColor& color) {
  if (indicatorRect.isEmpty()) {
    return;
  }

  const qreal left = indicatorRect.left() + indicatorRect.width() * 0.2;
  const qreal centerX = indicatorRect.center().x();
  const qreal right = indicatorRect.right() - indicatorRect.width() * 0.2;
  const qreal top = indicatorRect.top() + indicatorRect.height() * 0.35;
  const qreal bottom = indicatorRect.bottom() - indicatorRect.height() * 0.25;

  QPen pen(color, std::clamp(static_cast<qreal>(indicatorRect.height()) * 0.16, 1.0, 1.8),
           Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.save();
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawLine(QPointF(left, top), QPointF(centerX, bottom));
  painter.drawLine(QPointF(centerX, bottom), QPointF(right, top));
  painter.restore();
}

QRectF joinedBorderRect(const QRect& bounds, qreal borderWidth, bool joinedLeft, bool joinedRight) {
  return detail::joinedButtonBorderRect(bounds, borderWidth, joinedLeft, joinedRight);
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

QRectF centeredSquareRect(const QRectF& rect) {
  const qreal side = std::max<qreal>(0.0, std::min(rect.width(), rect.height()));
  return QRectF(rect.center().x() - side / 2.0, rect.center().y() - side / 2.0, side, side);
}

QRectF resolveShapeRect(const QRectF& rect, AdButton::Shape shape) {
  return detail::resolveButtonShapeRect(rect, shape);
}

bool stateHasVisibleBorder(const detail::ButtonStateStyle& state, int borderWidth) {
  return borderWidth > 0 && state.border.alpha() > 0;
}

bool styleHasVisibleBorder(const detail::ButtonVisualStyle& style) {
  return stateHasVisibleBorder(style.normal, style.metrics.borderWidth) ||
         stateHasVisibleBorder(style.hover, style.metrics.borderWidth) ||
         stateHasVisibleBorder(style.active, style.metrics.borderWidth) ||
         stateHasVisibleBorder(style.checked, style.metrics.borderWidth) ||
         stateHasVisibleBorder(style.disabled, style.metrics.borderWidth);
}

int resolveHorizontalFrameWidth(const detail::ButtonVisualStyle& style, AdButton::Shape shape,
                                bool joinedLeft, bool joinedRight, int horizontalPadding) {
  if (shape == AdButton::Shape::Circle) {
    return horizontalPadding * 2 + style.metrics.borderWidth * 2;
  }

  constexpr int kProbeWidth = 512;
  const QRect probeRect(0, 0, kProbeWidth, std::max(1, style.metrics.height));
  const bool hasVisibleBorder = styleHasVisibleBorder(style);
  const QRectF rawBorderRect =
      joinedBorderRect(probeRect, style.metrics.borderWidth, joinedLeft, joinedRight);
  const QRectF rawShapeRect = hasVisibleBorder ? rawBorderRect : QRectF(probeRect);
  const QRectF shapeRect = resolveShapeRect(rawShapeRect, shape);
  const QRectF rawContentRect = shapeRect.adjusted(
      horizontalPadding + style.metrics.borderWidth, style.metrics.borderWidth,
      -(horizontalPadding + style.metrics.borderWidth), -style.metrics.borderWidth);
  const QRect contentRect = rawContentRect.toAlignedRect();
  return std::max(0, probeRect.width() - contentRect.width());
}

CornerRadii resolveCorners(AdButton::Shape shape, const QRectF& rect, int borderRadius,
                           bool joinedLeft, bool joinedRight) {
  return detail::resolveButtonCorners(shape, rect, borderRadius, joinedLeft, joinedRight);
}

struct InteractionOutline {
  QRectF shapeRect;
  CornerRadii corners;
};

class DisabledCursorOverlay final : public QWidget {
 public:
  explicit DisabledCursorOverlay(QWidget* parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("ad-button-disabled-cursor-overlay"));
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
  }

 protected:
  void paintEvent(QPaintEvent* event) override { Q_UNUSED(event) }
};

InteractionOutline resolveInteractionOutline(const QRect& bounds,
                                             const detail::ButtonVisualStyle& style,
                                             const detail::ButtonStateStyle& state,
                                             AdButton::Shape shape, bool joinedLeft,
                                             bool joinedRight) {
  InteractionOutline outline;
  const bool hasVisibleBorder = stateHasVisibleBorder(state, style.metrics.borderWidth);
  const QRectF rawBorderRect =
      joinedBorderRect(bounds, style.metrics.borderWidth, joinedLeft, joinedRight);
  const QRectF rawShapeRect = hasVisibleBorder ? rawBorderRect : QRectF(bounds);
  outline.shapeRect = resolveShapeRect(rawShapeRect, shape);
  if (!outline.shapeRect.isEmpty()) {
    outline.corners = resolveCorners(shape, outline.shapeRect, style.metrics.borderRadius,
                                     joinedLeft, joinedRight);
  }
  return outline;
}

}  // namespace

struct AdButton::ContentLayout {
  bool hasText = false;
  bool hasIcon = false;
  QString text;
  QRect iconRect;
  QRect textRect;
};

struct AdButton::Private {
  ButtonStyle buttonStyle = ButtonStyle::Outline;
  AccentRole accentRole = AccentRole::Neutral;
  Shape shape = Shape::Rounded;
  SizeClass sizeClass = SizeClass::Medium;
  IconPosition iconPosition = IconPosition::Leading;
  detail::SegmentPosition segmentPosition = detail::SegmentPosition::Standalone;
  bool interactionBackgroundVisible = true;

  bool busy = false;
  int busyDelayMs = -1;
  BusyIndicatorPresentation busyIndicatorPresentation = BusyIndicatorPresentation::Inline;
  bool busyIndicatorVisible = false;
  bool hovered = false;
  bool focusVisible = false;
  bool enterPressed = false;
  bool busyDefaultSuspended = false;
  bool suspendedDefault = false;
  bool suspendedAutoDefault = false;
  bool explicitCursorOverride = false;
  bool autoCursorManaged = false;
  bool applyingAutoCursor = false;
  mutable bool circleTextWarningIssued = false;
  QWidget* disabledCursorOverlay = nullptr;

  adqt::icons::IconRef iconRef;
  adqt::icons::IconRef busyIconRef;
  bool spinnerSubscribed = false;
  quint64 busyIndicatorFrameCount = 0;
  std::unique_ptr<detail::BusyIndicatorSurface> busyIndicatorSurface;

  mutable std::optional<detail::ButtonVisualStyle> resolvedStyleCache;
  mutable detail::ButtonStyleInput resolvedStyleInput;
  mutable quint64 resolvedStyleThemeRevision = 0;
  mutable quint64 resolvedStylePaletteKey = 0;
  mutable quint64 resolvedStyleRevision = 0;

  mutable bool sizeHintCacheValid = false;
  mutable quint64 sizeHintStyleRevision = 0;
  mutable QString sizeHintText;
  mutable adqt::icons::IconRef sizeHintIconRef;
  mutable adqt::icons::IconRef sizeHintBusyIconRef;
  mutable quint64 sizeHintFallbackIconKey = 0;
  mutable QSize sizeHintIconSize;
  mutable Qt::LayoutDirection sizeHintLayoutDirection = Qt::LeftToRight;
  mutable Shape sizeHintShape = Shape::Rounded;
  mutable detail::SegmentPosition sizeHintSegmentPosition = detail::SegmentPosition::Standalone;
  mutable bool sizeHintBusyIndicatorVisible = false;
  mutable bool sizeHintHasMenu = false;
  mutable QSize cachedSizeHint;
  AdControlScaleContext controlScale;
  QFont referenceFont;
  QSize referenceIconSize;
  bool referenceMetricsCaptured = false;
};

namespace detail {

class BusyIndicatorSurface final : public QWidget {
 public:
  explicit BusyIndicatorSurface(AdButton* button)
      : QWidget(nullptr, busyIndicatorWindowFlags(button)), button_(button) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
  }

  void syncAndShow() {
    if (!button_ || !button_->usesIsolatedBusyIndicator() || !button_->isVisible()) {
      hide();
      return;
    }

    const QRect indicatorRect = button_->busyIndicatorRect();
    if (indicatorRect.isEmpty()) {
      hide();
      return;
    }

    QWidget* owner = button_->window();
    if (owner) {
      owner->winId();
      winId();
      if (windowHandle() && owner->windowHandle() &&
          windowHandle()->transientParent() != owner->windowHandle()) {
        windowHandle()->setTransientParent(owner->windowHandle());
      }
    }

    const QRect globalRect(button_->mapToGlobal(indicatorRect.topLeft()), indicatorRect.size());
    if (geometry() != globalRect) {
      setGeometry(globalRect);
    }
    if (!isVisible()) {
      show();
    }
  }

  void advanceFrame() {
    syncAndShow();
    if (!isVisible() || !button_) {
      return;
    }
    ++button_->d_->busyIndicatorFrameCount;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    if (!button_) {
      return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    button_->drawBusyIndicator(painter, rect(), button_->busyIndicatorColor());
  }

 private:
  QPointer<AdButton> button_;
};

}  // namespace detail

AdButton::AdButton(QWidget* parent) : QPushButton(parent), d_(std::make_unique<Private>()) {
  setAttribute(Qt::WA_Hover, true);
  setFocusPolicy(Qt::TabFocus);

  connect(this, &QAbstractButton::toggled, this, [this]() {
    bumpSegmentZOrder();
    updateInteractionFocusOverlay();
    update();
  });

  refreshAfterPropertyChange();
}

AdButton::AdButton(const QString& text, QWidget* parent) : AdButton(parent) { setText(text); }

AdButton::~AdButton() {
  resetDisabledCursorOverlay();
  stopInteractionWaveForOwner(this);
  stopInteractionFocusForOwner(this);
  detail::cancelTimingTask(this, QString::fromLatin1(kLoadingDelayTaskKey));
  detail::clearFrameSubscription(this, QString::fromLatin1(kSpinnerFrameKey));
  d_->spinnerSubscribed = false;
  d_->busyIndicatorSurface.reset();
}

AdButton::ButtonStyle AdButton::buttonStyle() const { return d_->buttonStyle; }

void AdButton::setButtonStyle(ButtonStyle value) {
  if (d_->buttonStyle == value) {
    return;
  }
  d_->buttonStyle = value;
  refreshAfterPropertyChange();
  emit buttonStyleChanged(d_->buttonStyle);
}

AdButton::AccentRole AdButton::accentRole() const { return d_->accentRole; }

void AdButton::setAccentRole(AccentRole value) {
  if (d_->accentRole == value) {
    return;
  }
  d_->accentRole = value;
  refreshAfterPropertyChange();
  emit accentRoleChanged(d_->accentRole);
}

AdButton::Shape AdButton::shape() const { return d_->shape; }

void AdButton::setShape(Shape value) {
  if (d_->shape == value) {
    return;
  }
  d_->shape = value;
  refreshAfterPropertyChange();
  emit shapeChanged(d_->shape);
}

AdButton::SizeClass AdButton::sizeClass() const { return d_->sizeClass; }

void AdButton::setSizeClass(SizeClass value) {
  if (d_->sizeClass == value) {
    return;
  }
  d_->sizeClass = value;
  refreshAfterPropertyChange();
  emit sizeClassChanged(d_->sizeClass);
}

bool AdButton::interactionBackgroundVisible() const { return d_->interactionBackgroundVisible; }

void AdButton::setInteractionBackgroundVisible(bool value) {
  if (d_->interactionBackgroundVisible == value) {
    return;
  }
  d_->interactionBackgroundVisible = value;
  refreshAfterPropertyChange(false);
  emit interactionBackgroundVisibleChanged(d_->interactionBackgroundVisible);
}

bool AdButton::busy() const { return d_->busy; }

void AdButton::setBusy(bool value) {
  if (d_->busy == value) {
    return;
  }
  d_->busy = value;
  updateBusyVisualState();
  updateCursorForRole();
  emit busyChanged(d_->busy);
}

int AdButton::busyDelayMs() const { return d_->busyDelayMs; }

void AdButton::setBusyDelayMs(int value) {
  const int normalized = std::max(-1, value);
  if (d_->busyDelayMs == normalized) {
    return;
  }
  d_->busyDelayMs = normalized;
  updateBusyVisualState();
  emit busyDelayMsChanged(d_->busyDelayMs);
}

AdButton::BusyIndicatorPresentation AdButton::busyIndicatorPresentation() const {
  return d_->busyIndicatorPresentation;
}

void AdButton::setBusyIndicatorPresentation(BusyIndicatorPresentation value) {
  if (d_->busyIndicatorPresentation == value) {
    return;
  }
  d_->busyIndicatorPresentation = value;
  updateSpinnerState();
  update();
  emit busyIndicatorPresentationChanged(value);
}

quint64 AdButton::busyIndicatorFrameCount() const { return d_->busyIndicatorFrameCount; }

QWidget* AdButton::busyIndicatorSurface() const { return d_->busyIndicatorSurface.get(); }

AdButton::IconPosition AdButton::iconPosition() const { return d_->iconPosition; }

void AdButton::setIconPosition(IconPosition value) {
  if (d_->iconPosition == value) {
    return;
  }
  d_->iconPosition = value;
  refreshAfterPropertyChange(false);
  emit iconPositionChanged(d_->iconPosition);
}

adqt::icons::IconRef AdButton::iconRef() const { return d_->iconRef; }

void AdButton::setIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(d_->iconRef, value)) {
    return;
  }
  d_->iconRef = value;
  refreshAfterPropertyChange();
  emit iconRefChanged(d_->iconRef);
}

adqt::icons::IconRef AdButton::busyIconRef() const { return d_->busyIconRef; }

void AdButton::setBusyIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(d_->busyIconRef, value)) {
    return;
  }
  d_->busyIconRef = value;
  updateSpinnerState();
  refreshAfterPropertyChange();
  emit busyIconRefChanged(d_->busyIconRef);
}

bool AdButton::event(QEvent* event) {
  if (event) {
    if (interactionBlocked() && event->type() == QEvent::Shortcut) {
      event->accept();
      return true;
    }

    if (event->type() == QEvent::CursorChange && !d_->applyingAutoCursor) {
      d_->autoCursorManaged = false;
      d_->explicitCursorOverride = testAttribute(Qt::WA_SetCursor);
      if (d_->explicitCursorOverride) {
        syncDisabledCursorOverlay();
      } else {
        updateCursorForRole();
      }
    } else if (event->type() == QEvent::ToolTipChange) {
      syncAccessibleState();
    } else if (event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionRemoved) {
      updateGeometry();
      updateInteractionFocusOverlay();
      update();
    } else if (event->type() == QEvent::ParentAboutToChange) {
      resetDisabledCursorOverlay();
      if (d_->busyIndicatorSurface) {
        d_->busyIndicatorSurface->hide();
      }
    } else if (event->type() == QEvent::ParentChange) {
      syncDisabledCursorOverlay();
      syncIsolatedBusyIndicatorSurface();
    } else if (event->type() == QEvent::ZOrderChange) {
      updateDisabledCursorOverlayGeometry();
    }
  }
  return QPushButton::event(event);
}

void AdButton::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  QStyleOptionButton option;
  initStyleOption(&option);

  const detail::ButtonVisualStyle style = resolvedStyle();
  detail::ButtonStateStyle state = currentStateStyle(style);
  const QString textToRender = option.text;
  const Shape visualShape = effectiveShape(textToRender);
  const bool hasMenuIndicator = option.features.testFlag(QStyleOptionButton::HasMenu);
  const bool defaultButton = option.features.testFlag(QStyleOptionButton::DefaultButton);

  if (style.role.buttonStyle == ButtonStyle::Tonal && (joinsLeftEdge() || joinsRightEdge()) &&
      state.background.isValid() && state.background.alpha() < 255) {
    const auto map = adqt::theme::ThemeManager::instance().resolveTheme(this);
    const QColor containerBg = parseThemeColor(map.colorBgContainer, QColor("#ffffff"));
    state.background = compositeOn(state.background, containerBg);
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();

  const bool hasVisibleBorder = style.metrics.borderWidth > 0 && state.border.alpha() > 0;
  const QRectF rawBorderRect =
      joinedBorderRect(rect(), style.metrics.borderWidth, joinsLeftEdge(), joinsRightEdge());
  const QRectF rawShapeRect = hasVisibleBorder ? rawBorderRect : QRectF(rect());
  const QRectF shapeRect = resolveShapeRect(rawShapeRect, visualShape);
  if (shapeRect.isEmpty()) {
    return;
  }
  const CornerRadii corners = resolveCorners(visualShape, shapeRect, style.metrics.borderRadius,
                                             joinsLeftEdge(), joinsRightEdge());

  QRectF fillRect = shapeRect;
  if (visualShape != Shape::Circle && hasVisibleBorder && (joinsLeftEdge() || joinsRightEdge())) {
    fillRect = QRectF(rect());
  }
  const QPainterPath fillPath = roundedRectPath(fillRect, corners.topLeft, corners.topRight,
                                                corners.bottomRight, corners.bottomLeft);

  if (isEnabled() && state.shadow.alpha() > 0 && !interactionBlocked()) {
    const qreal shadowOffsetY = std::max<qreal>(0.0, style.metrics.shadowOffsetY);
    const QPainterPath shadowPath =
        roundedRectPath(shapeRect.translated(0.0, shadowOffsetY), corners.topLeft, corners.topRight,
                        corners.bottomRight, corners.bottomLeft);
    painter.fillPath(shadowPath, state.shadow);
  }

  painter.fillPath(fillPath, state.background);

  if (visualShape != Shape::Circle && !hasVisibleBorder && (joinsLeftEdge() || joinsRightEdge())) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(state.background);
    const int joinedEdgeClearWidth = std::max(1, qCeil(1.0 / std::max<qreal>(1.0, dpr)));
    if (joinsLeftEdge()) {
      painter.drawRect(QRect(0, 0, joinedEdgeClearWidth, height()));
    }
    if (joinsRightEdge()) {
      painter.drawRect(
          QRect(std::max(0, width() - joinedEdgeClearWidth), 0, joinedEdgeClearWidth, height()));
    }
    painter.restore();
  }

  if (hasVisibleBorder) {
    const QPainterPath borderPath = roundedRectPath(shapeRect, corners.topLeft, corners.topRight,
                                                    corners.bottomRight, corners.bottomLeft);
    QPen borderPen =
        detail::makeButtonBorderPen(state.border, style.metrics.borderWidth, state.borderStyle);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(borderPath);
  }

  if (defaultButton && isEnabled() && style.metrics.defaultOutline.isValid() &&
      style.metrics.defaultOutline.alpha() > 0 && style.metrics.defaultOutlineWidth > 0.0) {
    const qreal outlineOffset = std::max<qreal>(0.0, style.metrics.defaultOutlineOffset);
    const QRectF outlineRect =
        shapeRect.adjusted(-outlineOffset, -outlineOffset, outlineOffset, outlineOffset);
    const QPainterPath outlinePath =
        roundedRectPath(outlineRect, corners.topLeft > 0.0 ? corners.topLeft + outlineOffset : 0.0,
                        corners.topRight > 0.0 ? corners.topRight + outlineOffset : 0.0,
                        corners.bottomRight > 0.0 ? corners.bottomRight + outlineOffset : 0.0,
                        corners.bottomLeft > 0.0 ? corners.bottomLeft + outlineOffset : 0.0);
    painter.save();
    painter.setPen(QPen(style.metrics.defaultOutline, style.metrics.defaultOutlineWidth,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(outlinePath);
    painter.restore();
  }

  painter.setFont(style.metrics.font);
  QFontMetrics fm(style.metrics.font);
  const bool twoCnAutoSpacing = shouldApplyTwoCjkSpacing(textToRender);
  const int textFlags = mnemonicTextFlags(this);
  const ButtonIconRenderState iconState = resolveIconRenderState(*this, d_->busyIndicatorVisible);
  const bool hasIcon = iconState.hasVisibleIcon();
  const bool iconOnly = hasIcon && textToRender.isEmpty();

  int iconSide = resolveIconSide(this, fm, style.metrics.font);
  if (visualShape == Shape::Circle || iconOnly) {
    iconSide = std::min(iconSide, std::max(8, qRound(shapeRect.height()) - 8));
  }

  const int horizontalPadding =
      (iconOnly || visualShape == Shape::Circle) ? 0 : style.metrics.horizontalPadding;
  const QRectF rawContentRect = shapeRect.adjusted(
      horizontalPadding + style.metrics.borderWidth, style.metrics.borderWidth,
      -(horizontalPadding + style.metrics.borderWidth), -style.metrics.borderWidth);
  QRect contentRect = rawContentRect.toAlignedRect();
  QRect menuIndicatorRect;
  if (hasMenuIndicator) {
    const int reserve = style.metrics.menuIndicatorSize + style.metrics.menuIndicatorGap;
    if (layoutDirection() == Qt::LeftToRight) {
      contentRect.adjust(0, 0, -reserve, 0);
      menuIndicatorRect =
          QRect(std::max(contentRect.right() + 1 + style.metrics.menuIndicatorGap,
                         rawContentRect.toAlignedRect().left()),
                rawContentRect.toAlignedRect().top() +
                    (rawContentRect.toAlignedRect().height() - style.metrics.menuIndicatorSize) / 2,
                style.metrics.menuIndicatorSize, style.metrics.menuIndicatorSize);
    } else {
      contentRect.adjust(reserve, 0, 0, 0);
      menuIndicatorRect =
          QRect(rawContentRect.toAlignedRect().left(),
                rawContentRect.toAlignedRect().top() +
                    (rawContentRect.toAlignedRect().height() - style.metrics.menuIndicatorSize) / 2,
                style.metrics.menuIndicatorSize, style.metrics.menuIndicatorSize);
    }
  }
  if (contentRect.width() < 0 || contentRect.height() < 0) {
    return;
  }

  const QSize layoutIconSize = hasIcon ? QSize(iconSide, iconSide) : QSize();
  const ContentLayout layout =
      computeContentLayout(contentRect, layoutIconSize, textToRender, fm, style.metrics.iconGap,
                           style.metrics.font, twoCnAutoSpacing, textFlags);

  QColor contentColor = state.text;
  if (iconState.busyIndicatorVisible) {
    contentColor.setAlphaF(contentColor.alphaF() * 0.72F);
  }
  painter.setPen(contentColor);

  if (layout.hasIcon) {
    if (iconState.busyIndicatorVisible && usesIsolatedBusyIndicator()) {
      // The icon slot remains reserved here while the animated pixels are
      // presented by a separate native surface.
    } else if (iconState.drawBuiltinSpinner()) {
      drawSpinner(painter, layout.iconRect, contentColor);
    } else if (iconState.hasTokenIcon) {
      adqt::icons::IconRef iconToRender = iconState.token;
      if (shouldInheritCurrentColor(iconToRender)) {
        iconToRender = iconToRender.withColors(adqt::icons::IconColors::primary(contentColor));
      }
      if (iconState.busyIndicatorVisible) {
        drawRotatingTokenIcon(painter, layout.iconRect, iconToRender,
                              static_cast<qreal>(sharedSpinnerAngle()));
      } else {
        const qreal pixmapDpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
        adqt::icons::IconRenderRequest request;
        request.logicalSize = layout.iconRect.size();
        request.devicePixelRatio = pixmapDpr;
        const QPixmap pixmap = adqt::icons::renderIconPixmap(iconToRender, request);
        if (!pixmap.isNull()) {
          drawCenteredPixmap(painter, layout.iconRect, pixmap, pixmapDpr);
        }
      }
    } else if (iconState.hasFallbackIcon) {
      const QIcon::Mode iconMode =
          !isEnabled()
              ? QIcon::Disabled
              : (isDown() ? QIcon::Selected : (d_->hovered ? QIcon::Active : QIcon::Normal));
      const QIcon::State fallbackIconState = isChecked() ? QIcon::On : QIcon::Off;
      const QPixmap pixmap =
          QAbstractButton::icon().pixmap(layout.iconRect.size(), iconMode, fallbackIconState);
      if (!pixmap.isNull()) {
        drawCenteredPixmap(painter, layout.iconRect, pixmap, pixmap.devicePixelRatio());
      }
    }
  }

  if (layout.hasText) {
    if (twoCnAutoSpacing && isTwoChineseCharacters(layout.text)) {
      const int spacingPx = twoCnLetterSpacingPx(fm, style.metrics.font);
      const int firstWidth = measureTextWidth(fm, QString(layout.text.at(0)), textFlags);
      const int baseline = layout.textRect.top() + fm.ascent();
      painter.drawText(layout.textRect.left(), baseline, QString(layout.text.at(0)));
      painter.drawText(layout.textRect.left() + firstWidth + spacingPx, baseline,
                       QString(layout.text.at(1)));
    } else {
      painter.drawText(layout.textRect, Qt::AlignLeft | Qt::AlignVCenter | textFlags, layout.text);
    }
  }

  if (hasMenuIndicator) {
    drawMenuIndicator(painter, menuIndicatorRect, contentColor);
  }
}

QSize AdButton::sizeHint() const {
  QStyleOptionButton option;
  initStyleOption(&option);

  const detail::ButtonVisualStyle style = resolvedStyle();

  const QString textToMeasure = option.text;
  const quint64 fallbackIconKey = QAbstractButton::icon().cacheKey();
  const bool hasMenu = option.features.testFlag(QStyleOptionButton::HasMenu);
  if (d_->sizeHintCacheValid && d_->sizeHintStyleRevision == d_->resolvedStyleRevision &&
      d_->sizeHintText == textToMeasure && iconRefsEqual(d_->sizeHintIconRef, d_->iconRef) &&
      iconRefsEqual(d_->sizeHintBusyIconRef, d_->busyIconRef) &&
      d_->sizeHintFallbackIconKey == fallbackIconKey && d_->sizeHintIconSize == iconSize() &&
      d_->sizeHintLayoutDirection == layoutDirection() && d_->sizeHintShape == d_->shape &&
      d_->sizeHintSegmentPosition == d_->segmentPosition &&
      d_->sizeHintBusyIndicatorVisible == d_->busyIndicatorVisible &&
      d_->sizeHintHasMenu == hasMenu) {
    const qreal scale = d_->controlScale.logicalScale;
    return QSize(qMax(1, qRound(d_->cachedSizeHint.width() * scale)),
                 qMax(1, qRound(d_->cachedSizeHint.height() * scale)));
  }

  QFontMetrics fm(style.metrics.font);
  const Shape visualShape = effectiveShape(textToMeasure);
  const bool hasMenuIndicator = hasMenu;
  const bool twoCnAutoSpacing = shouldApplyTwoCjkSpacing(textToMeasure);
  const int textFlags = mnemonicTextFlags(this);
  const ButtonIconRenderState iconState = resolveIconRenderState(*this, d_->busyIndicatorVisible);
  const bool hasIcon = iconState.hasVisibleIcon();
  const bool iconOnly = hasIcon && textToMeasure.isEmpty();
  int iconSide = resolveIconSide(this, fm, style.metrics.font);
  if (visualShape == Shape::Circle || iconOnly) {
    iconSide = std::min(iconSide, std::max(8, style.metrics.height - 8));
  }
  const int textWidth = textToMeasure.isEmpty()
                            ? 0
                            : measureDisplayTextWidth(fm, style.metrics.font, textToMeasure,
                                                      twoCnAutoSpacing, textFlags);
  const int horizontalPadding =
      (iconOnly || visualShape == Shape::Circle) ? 0 : style.metrics.horizontalPadding;
  const int horizontalFrameWidth = resolveHorizontalFrameWidth(style, visualShape, joinsLeftEdge(),
                                                               joinsRightEdge(), horizontalPadding);

  int width = horizontalFrameWidth + textWidth;
  if (hasIcon) {
    width += iconSide;
  }
  if (hasIcon && !textToMeasure.isEmpty()) {
    width += style.metrics.iconGap;
  }
  if (hasMenuIndicator) {
    width += style.metrics.menuIndicatorSize + style.metrics.menuIndicatorGap;
  }

  const int height = style.metrics.height;
  if (iconOnly || visualShape == Shape::Circle) {
    width = height;
  }
  d_->sizeHintStyleRevision = d_->resolvedStyleRevision;
  d_->sizeHintText = textToMeasure;
  d_->sizeHintIconRef = d_->iconRef;
  d_->sizeHintBusyIconRef = d_->busyIconRef;
  d_->sizeHintFallbackIconKey = fallbackIconKey;
  d_->sizeHintIconSize = iconSize();
  d_->sizeHintLayoutDirection = layoutDirection();
  d_->sizeHintShape = d_->shape;
  d_->sizeHintSegmentPosition = d_->segmentPosition;
  d_->sizeHintBusyIndicatorVisible = d_->busyIndicatorVisible;
  d_->sizeHintHasMenu = hasMenu;
  d_->cachedSizeHint = QSize(width, height);
  d_->sizeHintCacheValid = true;
  const qreal scale = d_->controlScale.logicalScale;
  return QSize(qMax(1, qRound(d_->cachedSizeHint.width() * scale)),
               qMax(1, qRound(d_->cachedSizeHint.height() * scale)));
}

QSize AdButton::minimumSizeHint() const { return sizeHint(); }

void AdButton::prepareControlScale(const AdControlScaleContext& context) {
  Q_UNUSED(context)
  d_->sizeHintCacheValid = false;
  d_->resolvedStyleCache.reset();
}

void AdButton::commitControlScale(const AdControlScaleContext& context) {
  if (!d_->referenceMetricsCaptured) {
    d_->referenceFont = font();
    d_->referenceIconSize = iconSize();
    d_->referenceMetricsCaptured = true;
  }
  d_->controlScale = context;
  const qreal scale = context.logicalScale;
  QFont scaledFont = d_->referenceFont;
  if (scaledFont.pixelSize() > 0) {
    scaledFont.setPixelSize(qMax(1, qRound(scaledFont.pixelSize() * scale)));
  } else if (scaledFont.pointSizeF() > 0.0) {
    scaledFont.setPointSizeF(scaledFont.pointSizeF() * scale);
  }
  setFont(scaledFont);
  if (d_->referenceIconSize.isValid()) {
    setIconSize(QSize(qMax(1, qRound(d_->referenceIconSize.width() * scale)),
                      qMax(1, qRound(d_->referenceIconSize.height() * scale))));
  }
  d_->sizeHintCacheValid = false;
  syncIsolatedBusyIndicatorSurface();
}

void AdButton::changeEvent(QEvent* event) {
  QPushButton::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::EnabledChange:
      if (!isEnabled()) {
        stopInteractionWaveForOwner(this);
        stopInteractionFocusForOwner(this);
      }
      syncAccessibleState();
      updateCursorForRole();
      syncDisabledCursorOverlay();
      updateInteractionFocusOverlay();
      update();
      break;
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::StyleChange:
      refreshAfterPropertyChange();
      break;
    default:
      break;
  }
}

void AdButton::enterEvent(QEnterEvent* event) {
  QPushButton::enterEvent(event);
  d_->hovered = true;
  updateCursorForRole();
  bumpSegmentZOrder();
  update();
}

void AdButton::leaveEvent(QEvent* event) {
  QPushButton::leaveEvent(event);
  d_->hovered = false;
  update();
}

void AdButton::mousePressEvent(QMouseEvent* event) {
  if (interactionBlocked()) {
    event->ignore();
    return;
  }
  d_->focusVisible = false;
  updateInteractionFocusOverlay();
  QPushButton::mousePressEvent(event);
  bumpSegmentZOrder();
}

void AdButton::mouseReleaseEvent(QMouseEvent* event) {
  if (interactionBlocked()) {
    event->ignore();
    return;
  }
  const bool shouldTriggerWave =
      event && event->button() == Qt::LeftButton && isDown() && hitButton(mouseEventPos(event));
  QPushButton::mouseReleaseEvent(event);
  if (shouldTriggerWave && isEnabled() && !interactionBlocked()) {
    triggerInteractionWaveOverlay();
  }
}

void AdButton::keyPressEvent(QKeyEvent* event) {
  if (!event) {
    return;
  }

  if (interactionBlocked() && isActivationKey(event->key())) {
    d_->enterPressed = false;
    setDown(false);
    event->ignore();
    return;
  }

  if (isEnterKey(event->key())) {
    if (!event->isAutoRepeat()) {
      d_->enterPressed = true;
      setDown(true);
      update();
    }
    event->accept();
    return;
  }

  QPushButton::keyPressEvent(event);
}

void AdButton::keyReleaseEvent(QKeyEvent* event) {
  if (!event) {
    return;
  }

  const bool activationKey = event && isActivationKey(event->key());
  const bool enterKey = isEnterKey(event->key());
  if (interactionBlocked() && activationKey) {
    if (enterKey) {
      d_->enterPressed = false;
      setDown(false);
    }
    event->ignore();
    return;
  }

  if (enterKey) {
    const bool triggerClick = d_->enterPressed && !event->isAutoRepeat();
    d_->enterPressed = false;
    setDown(false);
    if (triggerClick) {
      click();
      triggerInteractionWaveOverlay();
    }
    event->accept();
    update();
    return;
  }

  QPushButton::keyReleaseEvent(event);
  if (activationKey && !event->isAutoRepeat() && isEnabled() && !interactionBlocked()) {
    triggerInteractionWaveOverlay();
  }
}

bool AdButton::hitButton(const QPoint& pos) const {
  const Shape visualShape = effectiveShape(renderText());
  if (visualShape != Shape::Circle) {
    return QPushButton::hitButton(pos);
  }

  const detail::ButtonVisualStyle style = resolvedStyle();
  const detail::ButtonStateStyle state = currentStateStyle(style);
  const bool hasVisibleBorder = style.metrics.borderWidth > 0 && state.border.alpha() > 0;
  const QRectF rawBorderRect =
      joinedBorderRect(rect(), style.metrics.borderWidth, joinsLeftEdge(), joinsRightEdge());
  const QRectF shapeRect =
      resolveShapeRect(hasVisibleBorder ? rawBorderRect : QRectF(rect()), visualShape);
  const QRectF circleRect = centeredSquareRect(shapeRect);
  if (circleRect.isEmpty()) {
    return false;
  }

  const QPointF delta = QPointF(pos) - circleRect.center();
  const qreal radius = circleRect.width() / 2.0;
  return delta.x() * delta.x() + delta.y() * delta.y() <= radius * radius;
}

void AdButton::focusInEvent(QFocusEvent* event) {
  QPushButton::focusInEvent(event);
  d_->focusVisible = event && isKeyboardFocusReason(event->reason());
  updateInteractionFocusOverlay();
  update();
}

void AdButton::focusOutEvent(QFocusEvent* event) {
  QPushButton::focusOutEvent(event);
  d_->focusVisible = false;
  d_->enterPressed = false;
  setDown(false);
  updateInteractionFocusOverlay();
  update();
}

void AdButton::moveEvent(QMoveEvent* event) {
  QPushButton::moveEvent(event);
  updateDisabledCursorOverlayGeometry();
  updateInteractionFocusOverlay();
  syncIsolatedBusyIndicatorSurface();
}

void AdButton::resizeEvent(QResizeEvent* event) {
  QPushButton::resizeEvent(event);
  updateDisabledCursorOverlayGeometry();
  updateInteractionFocusOverlay();
  syncIsolatedBusyIndicatorSurface();
}

void AdButton::showEvent(QShowEvent* event) {
  QPushButton::showEvent(event);
  updateSpinnerState();
  updateCursorForRole();
  syncDisabledCursorOverlay();
  updateInteractionFocusOverlay();
  syncIsolatedBusyIndicatorSurface();
}

void AdButton::hideEvent(QHideEvent* event) {
  QPushButton::hideEvent(event);
  d_->enterPressed = false;
  setDown(false);
  updateSpinnerState();
  syncIsolatedBusyIndicatorSurface();
  syncDisabledCursorOverlay();
  stopInteractionFocusForOwner(this);
  stopInteractionWaveForOwner(this);
}

bool AdButton::interactionBlocked() const { return !isEnabled() || d_->busy; }

bool AdButton::hasUserIconRef() const { return adqt::icons::isValid(d_->iconRef) || hasBaseIcon(); }

bool AdButton::hasBaseIcon() const { return !QPushButton::icon().isNull(); }

bool AdButton::shouldApplyTwoCjkSpacing(const QString& sourceText) const {
  if (sourceText.isEmpty()) {
    return false;
  }
  if (hasMnemonicMarker(sourceText)) {
    return false;
  }

  const detail::ResolvedRole role = detail::resolveRole(buildStyleInput());
  if (role.unbordered || hasUserIconRef() || d_->busyIndicatorVisible) {
    return false;
  }
  return isTwoChineseCharacters(stripMnemonicMarkers(sourceText));
}

AdButton::Shape AdButton::effectiveShape(const QString& displayText) const {
  if (d_->shape != Shape::Circle) {
    return d_->shape;
  }
  if (displayText.isEmpty() && QPushButton::menu() == nullptr) {
    return Shape::Circle;
  }

#ifdef QT_DEBUG
  if (!displayText.isEmpty() && !d_->circleTextWarningIssued) {
    d_->circleTextWarningIssued = true;
    qWarning().nospace()
        << "AdButton: Shape::Circle only supports icon-only usage; falling back to "
        << "Shape::Pill for button text \"" << displayText << "\".";
  }
#endif
  return Shape::Pill;
}

QString AdButton::renderText() const { return QAbstractButton::text(); }

detail::ButtonStyleInput AdButton::buildStyleInput() const {
  detail::ButtonStyleInput input;
  input.buttonStyle = d_->buttonStyle;
  input.accentRole = d_->accentRole;
  input.sizeClass = d_->sizeClass;
  input.flat = isFlat();
  input.defaultButton = isDefault();
  input.hasMenu = QPushButton::menu() != nullptr;
  input.baseFont = font();
  return input;
}

detail::ButtonVisualStyle AdButton::resolvedStyle() const {
  const detail::ButtonStyleInput input = buildStyleInput();
  const auto& themeManager = adqt::theme::ThemeManager::instance();
  const quint64 themeRevision = themeManager.themeRevision();
  const quint64 paletteKey = palette().cacheKey();
  if (d_->resolvedStyleCache.has_value() && d_->resolvedStyleThemeRevision == themeRevision &&
      d_->resolvedStylePaletteKey == paletteKey &&
      buttonStyleInputsEqual(d_->resolvedStyleInput, input)) {
    return d_->resolvedStyleCache.value();
  }

  const adqt::theme::ResolvedTheme resolvedTheme = themeManager.resolve(this);
  d_->resolvedStyleCache = detail::resolveButtonVisualStyle(input, resolvedTheme);
  d_->resolvedStyleInput = input;
  d_->resolvedStyleThemeRevision = themeRevision;
  d_->resolvedStylePaletteKey = paletteKey;
  ++d_->resolvedStyleRevision;
  d_->sizeHintCacheValid = false;
  return d_->resolvedStyleCache.value();
}

detail::ButtonStateStyle AdButton::currentStateStyle(const detail::ButtonVisualStyle& style) const {
  detail::ButtonStateStyle state;
  if (!isEnabled()) {
    state = style.disabled;
  } else if (isDown()) {
    state = style.active;
  } else if (isChecked()) {
    state = style.checked;
  } else if (d_->hovered) {
    state = style.hover;
  } else {
    state = style.normal;
  }
  if (!d_->interactionBackgroundVisible && (d_->hovered || isDown() || isChecked())) {
    state.background = QColor(0, 0, 0, 0);
  }
  return state;
}

void AdButton::refreshAfterPropertyChange(bool updateGeometryHint) {
  updateSpinnerState();
  syncAccessibleState();
  updateCursorForRole();

  if (updateGeometryHint) {
    updateGeometry();
  }
  updateInteractionFocusOverlay();
  update();
}

void AdButton::updateBusyVisualState() {
  detail::cancelTimingTask(this, QString::fromLatin1(kLoadingDelayTaskKey));

  if (!d_->busy) {
    d_->busyIndicatorVisible = false;
  } else if (d_->busyDelayMs < 0 || detail::resolveLoadingDelayMs(d_->busyDelayMs) <= 0) {
    d_->busyIndicatorVisible = true;
  } else {
    d_->busyIndicatorVisible = false;
    const int delayMs = detail::resolveLoadingDelayMs(d_->busyDelayMs);
    detail::scheduleTimingTask(this, QString::fromLatin1(kLoadingDelayTaskKey), delayMs, [this]() {
      if (!d_->busy) {
        return;
      }
      d_->busyIndicatorVisible = true;
      updateSpinnerState();
      refreshAfterPropertyChange();
    });
  }

  if (interactionBlocked()) {
    d_->enterPressed = false;
    setDown(false);
    stopInteractionWaveForOwner(this);
  }

  syncBusyDefaultSuspension();
  updateSpinnerState();
  updateCursorForRole();
  updateInteractionFocusOverlay();
  refreshAfterPropertyChange();
}

void AdButton::syncBusyDefaultSuspension() {
  if (!d_->busy) {
    if (!d_->busyDefaultSuspended) {
      return;
    }

    const bool restoreAutoDefault = d_->suspendedAutoDefault;
    const bool restoreDefault = d_->suspendedDefault;
    d_->busyDefaultSuspended = false;
    d_->suspendedAutoDefault = false;
    d_->suspendedDefault = false;

    if (QPushButton::autoDefault() != restoreAutoDefault) {
      QPushButton::setAutoDefault(restoreAutoDefault);
    }
    if (QPushButton::isDefault() != restoreDefault) {
      QPushButton::setDefault(restoreDefault);
    }
    return;
  }

  if (!d_->busyDefaultSuspended) {
    d_->suspendedAutoDefault = QPushButton::autoDefault();
    d_->suspendedDefault = QPushButton::isDefault();
    d_->busyDefaultSuspended = d_->suspendedAutoDefault || d_->suspendedDefault;
  } else {
    d_->suspendedAutoDefault = d_->suspendedAutoDefault || QPushButton::autoDefault();
    d_->suspendedDefault = d_->suspendedDefault || QPushButton::isDefault();
  }

  if (QPushButton::autoDefault()) {
    QPushButton::setAutoDefault(false);
  }
  if (QPushButton::isDefault()) {
    QPushButton::setDefault(false);
  }
}

void AdButton::updateSpinnerState() {
  const bool spinning = d_->busyIndicatorVisible && isVisible();
  if (spinning && usesIsolatedBusyIndicator()) {
    syncIsolatedBusyIndicatorSurface();
  } else if (d_->busyIndicatorSurface) {
    d_->busyIndicatorSurface->hide();
  }

  if (spinning && !d_->spinnerSubscribed) {
    detail::setFrameSubscription(this, QString::fromLatin1(kSpinnerFrameKey), true,
                                 [this](qint64, qint64) {
                                   if (!d_->busyIndicatorVisible || !isVisible()) {
                                     return;
                                   }
                                   if (usesIsolatedBusyIndicator()) {
                                     syncIsolatedBusyIndicatorSurface();
                                     if (d_->busyIndicatorSurface) {
                                       d_->busyIndicatorSurface->advanceFrame();
                                     }
                                   } else {
                                     update();
                                   }
                                 });
    d_->spinnerSubscribed = true;
  } else if (!spinning && d_->spinnerSubscribed) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kSpinnerFrameKey));
    d_->spinnerSubscribed = false;
  }
}

bool AdButton::usesIsolatedBusyIndicator() const {
  return d_->busyIndicatorVisible &&
         d_->busyIndicatorPresentation == BusyIndicatorPresentation::IsolatedSurface &&
         QGuiApplication::platformName().compare(QStringLiteral("windows"), Qt::CaseInsensitive) ==
             0;
}

void AdButton::syncIsolatedBusyIndicatorSurface() {
  if (!usesIsolatedBusyIndicator() || !isVisible()) {
    if (d_->busyIndicatorSurface) {
      d_->busyIndicatorSurface->hide();
    }
    return;
  }
  const bool surfaceCreated = !d_->busyIndicatorSurface;
  if (surfaceCreated) {
    d_->busyIndicatorSurface = std::make_unique<detail::BusyIndicatorSurface>(this);
  }
  d_->busyIndicatorSurface->syncAndShow();
  if (surfaceCreated) {
    emit busyIndicatorSurfaceChanged(d_->busyIndicatorSurface.get());
  }
}

QRect AdButton::busyIndicatorRect() const {
  if (!d_->busyIndicatorVisible) {
    return {};
  }

  QStyleOptionButton option;
  initStyleOption(&option);
  const detail::ButtonVisualStyle style = resolvedStyle();
  const QString displayText = option.text;
  const Shape visualShape = effectiveShape(displayText);
  const bool hasMenuIndicator = option.features.testFlag(QStyleOptionButton::HasMenu);
  const bool hasVisibleBorder =
      style.metrics.borderWidth > 0 && currentStateStyle(style).border.alpha() > 0;
  const QRectF rawBorderRect =
      joinedBorderRect(rect(), style.metrics.borderWidth, joinsLeftEdge(), joinsRightEdge());
  const QRectF shapeRect =
      resolveShapeRect(hasVisibleBorder ? rawBorderRect : QRectF(rect()), visualShape);
  if (shapeRect.isEmpty()) {
    return {};
  }

  QFontMetrics fm(style.metrics.font);
  int iconSide = resolveIconSide(this, fm, style.metrics.font);
  const bool iconOnly = displayText.isEmpty();
  if (visualShape == Shape::Circle || iconOnly) {
    iconSide = std::min(iconSide, std::max(8, qRound(shapeRect.height()) - 8));
  }
  const int horizontalPadding =
      (iconOnly || visualShape == Shape::Circle) ? 0 : style.metrics.horizontalPadding;
  QRect contentRect =
      shapeRect
          .adjusted(horizontalPadding + style.metrics.borderWidth, style.metrics.borderWidth,
                    -(horizontalPadding + style.metrics.borderWidth), -style.metrics.borderWidth)
          .toAlignedRect();
  if (hasMenuIndicator) {
    const int reserve = style.metrics.menuIndicatorSize + style.metrics.menuIndicatorGap;
    if (layoutDirection() == Qt::LeftToRight) {
      contentRect.adjust(0, 0, -reserve, 0);
    } else {
      contentRect.adjust(reserve, 0, 0, 0);
    }
  }
  if (contentRect.width() < 0 || contentRect.height() < 0) {
    return {};
  }

  return computeContentLayout(contentRect, QSize(iconSide, iconSide), displayText, fm,
                              style.metrics.iconGap, style.metrics.font,
                              shouldApplyTwoCjkSpacing(displayText), mnemonicTextFlags(this))
      .iconRect;
}

QColor AdButton::busyIndicatorColor() const {
  QColor color = currentStateStyle(resolvedStyle()).text;
  color.setAlphaF(color.alphaF() * 0.72F);
  return color;
}

void AdButton::drawBusyIndicator(QPainter& painter, const QRect& rect, const QColor& color) const {
  const ButtonIconRenderState iconState = resolveIconRenderState(*this, true);
  if (iconState.hasTokenIcon) {
    adqt::icons::IconRef iconToRender = iconState.token;
    if (shouldInheritCurrentColor(iconToRender)) {
      iconToRender = iconToRender.withColors(adqt::icons::IconColors::primary(color));
    }
    drawRotatingTokenIcon(painter, rect, iconToRender, static_cast<qreal>(sharedSpinnerAngle()));
    return;
  }
  drawSpinner(painter, rect, color);
}

void AdButton::bumpSegmentZOrder() {
  if (joinsLeftEdge() || joinsRightEdge() || isChecked()) {
    raise();
  }
  updateDisabledCursorOverlayGeometry();
}

std::optional<Qt::CursorShape> AdButton::automaticCursorShape() const {
  if (!isEnabled()) {
    return Qt::ForbiddenCursor;
  }
  if (d_->busy) {
    return Qt::ArrowCursor;
  }
  return Qt::PointingHandCursor;
}

void AdButton::syncDisabledCursorOverlay() {
  const bool needsOverlay = !isEnabled() && isVisible();
  QWidget* container = parentWidget();
  if (!needsOverlay || !container) {
    if (d_->disabledCursorOverlay) {
      d_->disabledCursorOverlay->hide();
    }
    return;
  }

  if (!d_->disabledCursorOverlay || d_->disabledCursorOverlay->parentWidget() != container) {
    resetDisabledCursorOverlay();
    d_->disabledCursorOverlay = new DisabledCursorOverlay(container);
  }

  d_->disabledCursorOverlay->setCursor(cursor());
  d_->disabledCursorOverlay->show();
  updateDisabledCursorOverlayGeometry();
}

void AdButton::updateDisabledCursorOverlayGeometry() {
  if (!d_->disabledCursorOverlay) {
    return;
  }

  QWidget* container = d_->disabledCursorOverlay->parentWidget();
  if (!container || isEnabled() || !isVisible() || size().isEmpty()) {
    d_->disabledCursorOverlay->hide();
    return;
  }

  d_->disabledCursorOverlay->setGeometry(QRect(mapTo(container, QPoint(0, 0)), size()));
  d_->disabledCursorOverlay->raise();
}

void AdButton::resetDisabledCursorOverlay() {
  if (!d_->disabledCursorOverlay) {
    return;
  }
  d_->disabledCursorOverlay->deleteLater();
  d_->disabledCursorOverlay = nullptr;
}

void AdButton::updateCursorForRole() {
  applyAutomaticCursor(automaticCursorShape());
  syncDisabledCursorOverlay();
}

void AdButton::updateInteractionFocusOverlay() {
  if (!(hasFocus() && isEnabled() && d_->focusVisible && isVisible())) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const detail::ButtonVisualStyle style = resolvedStyle();
  const detail::ButtonStateStyle state = currentStateStyle(style);
  const Shape visualShape = effectiveShape(renderText());
  const InteractionOutline outline = resolveInteractionOutline(rect(), style, state, visualShape,
                                                               joinsLeftEdge(), joinsRightEdge());
  if (outline.shapeRect.isEmpty()) {
    stopInteractionFocusForOwner(this);
    return;
  }

  QWidget* hostWindow = window();
  if (!hostWindow) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const QColor focusColor = style.metrics.focusOutline;
  const qreal focusWidth = std::max<qreal>(1.0, style.metrics.focusOutlineWidth);
  if (!focusColor.isValid() || focusColor.alpha() <= 0 || focusWidth <= 0.0) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const QPoint origin = mapTo(hostWindow, QPoint(0, 0));
  InteractionFocusRequest request;
  request.owner = this;
  request.baseRectInWindow = outline.shapeRect.translated(origin.x(), origin.y());
  request.topLeft = outline.corners.topLeft;
  request.topRight = outline.corners.topRight;
  request.bottomRight = outline.corners.bottomRight;
  request.bottomLeft = outline.corners.bottomLeft;
  request.color = focusColor;
  request.strokeWidth = focusWidth;
  request.offset = std::max<qreal>(0.0, style.metrics.focusOutlineOffset);
  triggerInteractionFocus(request);
}

void AdButton::triggerInteractionWaveOverlay() {
  if (!isVisible() || interactionBlocked()) {
    return;
  }

  QWidget* hostWindow = window();
  if (!hostWindow) {
    return;
  }

  const detail::ButtonVisualStyle style = resolvedStyle();
  if (style.role.unbordered) {
    return;
  }

  const detail::ButtonStateStyle state = currentStateStyle(style);
  const Shape visualShape = effectiveShape(renderText());
  const InteractionOutline outline = resolveInteractionOutline(rect(), style, state, visualShape,
                                                               joinsLeftEdge(), joinsRightEdge());
  if (outline.shapeRect.isEmpty()) {
    return;
  }

  QColor waveColor;
  if (style.role.unbordered) {
    if (hasVisibleColor(state.text)) {
      waveColor = state.text;
    } else if (hasVisibleColor(state.background)) {
      waveColor = state.background;
    } else {
      waveColor = state.border;
    }
  } else if (stateHasVisibleBorder(state, style.metrics.borderWidth)) {
    waveColor = state.border;
  } else if (hasVisibleColor(state.background)) {
    waveColor = state.background;
  } else {
    waveColor = state.text;
  }

  if (!hasVisibleColor(waveColor)) {
    return;
  }

  if (waveColor.alpha() < 255) {
    const auto themeMap = adqt::theme::ThemeManager::instance().resolveTheme(this);
    const QColor containerBg = parseThemeColor(themeMap.colorBgContainer, QColor("#ffffff"));
    waveColor = compositeOn(waveColor, containerBg);
  }

  const QPoint origin = mapTo(hostWindow, QPoint(0, 0));
  InteractionWaveRequest request;
  request.owner = this;
  request.baseRectInWindow = outline.shapeRect.translated(origin.x(), origin.y());
  request.topLeft = outline.corners.topLeft;
  request.topRight = outline.corners.topRight;
  request.bottomRight = outline.corners.bottomRight;
  request.bottomLeft = outline.corners.bottomLeft;
  request.color = waveColor;
  triggerInteractionWave(request);
}

void AdButton::syncAccessibleState() {
  // Intentionally left blank: accessibility metadata should be set explicitly by callers
  // instead of being inferred from text or tooltips.
}

void AdButton::applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape) {
  if (d_->explicitCursorOverride) {
    return;
  }

  if (cursorShape.has_value()) {
    if (!d_->autoCursorManaged || !testAttribute(Qt::WA_SetCursor) ||
        cursor().shape() != cursorShape.value()) {
      d_->applyingAutoCursor = true;
      QPushButton::setCursor(QCursor(cursorShape.value()));
      d_->applyingAutoCursor = false;
    }
    d_->autoCursorManaged = true;
    return;
  }

  if (d_->autoCursorManaged || testAttribute(Qt::WA_SetCursor)) {
    d_->applyingAutoCursor = true;
    QPushButton::unsetCursor();
    d_->applyingAutoCursor = false;
  }
  d_->autoCursorManaged = false;
}

detail::SegmentPosition AdButton::segmentPosition() const { return d_->segmentPosition; }

void AdButton::setSegmentPosition(detail::SegmentPosition value) {
  if (d_->segmentPosition == value) {
    return;
  }
  d_->segmentPosition = value;
  updateGeometry();
  bumpSegmentZOrder();
  updateInteractionFocusOverlay();
  update();
}

bool AdButton::joinsLeftEdge() const {
  return d_->segmentPosition == detail::SegmentPosition::Middle ||
         d_->segmentPosition == detail::SegmentPosition::Trailing;
}

bool AdButton::joinsRightEdge() const {
  return d_->segmentPosition == detail::SegmentPosition::Leading ||
         d_->segmentPosition == detail::SegmentPosition::Middle;
}

AdButton::ContentLayout AdButton::computeContentLayout(
    const QRect& contentRect, const QSize& iconSize, const QString& displayText,
    const QFontMetrics& fm, int iconGap, const QFont& contentFont, bool twoCjkAutoSpacing,
    int textFlags) const {
  ContentLayout layout;
  layout.hasText = !displayText.isEmpty();
  layout.hasIcon = iconSize.isValid() && !iconSize.isEmpty();

  const int gap = (layout.hasIcon && layout.hasText) ? iconGap : 0;
  const int availableTextWidth =
      std::max(0, contentRect.width() - (layout.hasIcon ? iconSize.width() + gap : 0));
  if (layout.hasText) {
    const int fullTextWidth =
        measureDisplayTextWidth(fm, contentFont, displayText, twoCjkAutoSpacing, textFlags);
    layout.text = fullTextWidth <= availableTextWidth
                      ? displayText
                      : fm.elidedText(displayText, Qt::ElideRight, availableTextWidth, textFlags);
  }
  layout.hasText = !layout.text.isEmpty();

  const int textWidth = layout.hasText ? measureDisplayTextWidth(fm, contentFont, layout.text,
                                                                 twoCjkAutoSpacing, textFlags)
                                       : 0;
  const int textHeight = layout.hasText ? fm.height() : 0;

  int contentWidth = 0;
  if (layout.hasIcon) {
    contentWidth += iconSize.width();
  }
  if (layout.hasIcon && layout.hasText) {
    contentWidth += gap;
  }
  if (layout.hasText) {
    contentWidth += textWidth;
  }

  int startX = contentRect.left();
  if (contentRect.width() > contentWidth) {
    startX = contentRect.left() + (contentRect.width() - contentWidth) / 2;
  }

  if (!layout.hasIcon && !layout.hasText) {
    return layout;
  }

  const bool iconFirst = layoutDirection() == Qt::LeftToRight
                             ? d_->iconPosition == IconPosition::Leading
                             : d_->iconPosition == IconPosition::Trailing;

  if (layout.hasIcon && layout.hasText) {
    const int iconY = contentRect.top() + (contentRect.height() - iconSize.height()) / 2;
    const int textY = contentRect.top() + (contentRect.height() - textHeight) / 2;
    if (iconFirst) {
      layout.iconRect = QRect(startX, iconY, iconSize.width(), iconSize.height());
      layout.textRect = QRect(layout.iconRect.right() + 1 + gap, textY, textWidth, textHeight);
    } else {
      layout.textRect = QRect(startX, textY, textWidth, textHeight);
      layout.iconRect =
          QRect(layout.textRect.right() + 1 + gap, iconY, iconSize.width(), iconSize.height());
    }
    return layout;
  }

  if (layout.hasIcon) {
    const int iconX = contentRect.left() + (contentRect.width() - iconSize.width()) / 2;
    const int iconY = contentRect.top() + (contentRect.height() - iconSize.height()) / 2;
    layout.iconRect = QRect(iconX, iconY, iconSize.width(), iconSize.height());
  }

  if (layout.hasText) {
    const int textX = contentRect.left() + (contentRect.width() - textWidth) / 2;
    const int textY = contentRect.top() + (contentRect.height() - textHeight) / 2;
    layout.textRect = QRect(textX, textY, textWidth, textHeight);
  }

  return layout;
}

void AdButton::drawSpinner(QPainter& painter, const QRect& iconRect, const QColor& color) const {
  const int side = std::max(8, std::min(iconRect.width(), iconRect.height()) - 2);
  const QPointF center = QRectF(iconRect).center();
  const QRectF spinnerRect(center.x() - side / 2.0, center.y() - side / 2.0, side, side);
  const qreal strokeWidth = std::clamp(
      static_cast<qreal>(std::min(iconRect.width(), iconRect.height())) * 0.08, 1.0, 2.0);
  QPen pen(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawArc(spinnerRect, (90 - sharedSpinnerAngle()) * 16, -270 * 16);
}

namespace detail {

void setButtonSegmentPosition(AdButton* button, SegmentPosition value) {
  if (!button) {
    return;
  }
  button->setSegmentPosition(value);
}

SegmentPosition buttonSegmentPosition(const AdButton* button) {
  if (!button) {
    return SegmentPosition::Standalone;
  }
  return button->segmentPosition();
}

}  // namespace detail

}  // namespace adqt::widgets
