#include "switch.h"

#include "detail/animated_scalar.h"
#include "detail/overlay_accessibility.h"
#include "antd_icons.h"
#include "interaction_overlay_manager.h"
#include "switch_style.h"
#include "theme/theme.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <tuple>
#include <utility>

namespace adqt::widgets {

namespace {

constexpr char kThumbFrameKey[] = "AdSwitch.ThumbFrame";
constexpr char kPressStateFrameKey[] = "AdSwitch.PressStateFrame";
constexpr char kSpinnerFrameKey[] = "AdSwitch.SpinnerFrame";

enum class LogicalEdge : std::uint8_t {
  Start,
  End,
};

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

bool colorTokensEqual(const AdSwitch::ColorTokens& lhs, const AdSwitch::ColorTokens& rhs) {
  return std::tie(lhs.uncheckedTrack, lhs.uncheckedTrackHover, lhs.checkedTrack,
                  lhs.checkedTrackHover, lhs.thumb, lhs.thumbBorder, lhs.thumbShadow, lhs.content,
                  lhs.loadingIndicator, lhs.checkedLoadingIndicator, lhs.focusRing, lhs.wave) ==
         std::tie(rhs.uncheckedTrack, rhs.uncheckedTrackHover, rhs.checkedTrack,
                  rhs.checkedTrackHover, rhs.thumb, rhs.thumbBorder, rhs.thumbShadow, rhs.content,
                  rhs.loadingIndicator, rhs.checkedLoadingIndicator, rhs.focusRing, rhs.wave);
}

bool metricTokensEqual(const AdSwitch::MetricTokens& lhs, const AdSwitch::MetricTokens& rhs) {
  return std::tie(lhs.trackHeight, lhs.smallTrackHeight, lhs.trackMinWidth, lhs.smallTrackMinWidth,
                  lhs.trackPadding, lhs.thumbSize, lhs.smallThumbSize, lhs.loadingIndicatorSize,
                  lhs.disabledOpacity) ==
         std::tie(rhs.trackHeight, rhs.smallTrackHeight, rhs.trackMinWidth, rhs.smallTrackMinWidth,
                  rhs.trackPadding, rhs.thumbSize, rhs.smallThumbSize, rhs.loadingIndicatorSize,
                  rhs.disabledOpacity);
}

bool componentTokensEqual(const AdSwitch::ComponentTokens& lhs,
                          const AdSwitch::ComponentTokens& rhs) {
  return colorTokensEqual(lhs.colors, rhs.colors) && metricTokensEqual(lhs.metrics, rhs.metrics);
}

struct ResolvedStateContent {
  QString text;
  adqt::icons::IconRef iconRef;
  bool hasIconRef = false;
};

struct SwitchLayout {
  QRectF indicatorRect;
  QRect labelRect;
  int indicatorWidth = 0;
  int indicatorHeight = 0;
  int spacing = 0;
};

ResolvedStateContent resolveStateContent(const AdSwitch& sw, bool checkedState) {
  ResolvedStateContent content;

  const QString text = checkedState ? sw.checkedText() : sw.uncheckedText();
  if (!text.trimmed().isEmpty()) {
    content.text = text;
  }

  const adqt::icons::IconRef token = checkedState ? sw.checkedIconRef() : sw.uncheckedIconRef();
  if (adqt::icons::isValid(token)) {
    content.iconRef = token;
    content.hasIconRef = true;
  }

  return content;
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

bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

bool isEnterKey(int key) { return key == Qt::Key_Return || key == Qt::Key_Enter; }

QColor blendColor(const QColor& from, const QColor& to, qreal t) {
  const float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
  return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * x,
                          from.greenF() + (to.greenF() - from.greenF()) * x,
                          from.blueF() + (to.blueF() - from.blueF()) * x,
                          from.alphaF() + (to.alphaF() - from.alphaF()) * x);
}

QPainterPath roundedRectPath(const QRectF& rect, qreal topLeft, qreal topRight, qreal bottomRight,
                             qreal bottomLeft) {
  const qreal w = std::max(rect.width(), 0.0);
  const qreal h = std::max(rect.height(), 0.0);
  const qreal maxRadius = std::min(w, h) / 2.0;

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

qreal snapToDevicePixelSize(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  const qreal snapped = qRound(value * dpr) / dpr;
  return std::max(snapped, 1.0 / dpr);
}

qreal snapToDevicePixelCoord(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  return qRound(value * dpr) / dpr;
}

QRectF snapRectToDevicePixels(const QRectF& rect, qreal dpr) {
  if (dpr <= 0.0) {
    return rect;
  }

  const qreal left = snapToDevicePixelCoord(rect.left(), dpr);
  const qreal top = snapToDevicePixelCoord(rect.top(), dpr);
  const qreal right = snapToDevicePixelCoord(rect.left() + rect.width(), dpr);
  const qreal bottom = snapToDevicePixelCoord(rect.top() + rect.height(), dpr);
  const qreal minSize = 1.0 / dpr;

  return QRectF(left, top, std::max(minSize, right - left), std::max(minSize, bottom - top));
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

qreal xFromLogicalEdge(const QRectF& rect, Qt::LayoutDirection direction, LogicalEdge edge,
                       qreal inset, qreal width) {
  const bool leftAligned = (direction == Qt::LeftToRight && edge == LogicalEdge::Start) ||
                           (direction == Qt::RightToLeft && edge == LogicalEdge::End);
  if (leftAligned) {
    return rect.left() + inset;
  }
  return rect.right() - inset - width;
}

template <typename T>
void mergeOptional(std::optional<T>* target, const std::optional<T>& source) {
  if (target && source.has_value()) {
    *target = source;
  }
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

int switchLabelSpacing(const QWidget* widget) {
  if (!widget || !widget->style()) {
    return 6;
  }
  const int spacing =
      widget->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, widget);
  return std::max(0, spacing >= 0 ? spacing : 6);
}

QSize indicatorSizeHint(const AdSwitch& sw, const detail::SwitchAppearance& appearance) {
  const bool small = sw.controlSize() == AdSwitch::ControlSize::Small;
  const int trackHeight =
      small ? appearance.metrics.trackHeightSmall : appearance.metrics.trackHeight;
  const int thumbSize = small ? appearance.metrics.thumbSizeSmall : appearance.metrics.thumbSize;
  const int height = std::max(trackHeight, thumbSize);
  const int minWidth =
      small ? appearance.metrics.trackMinWidthSmall : appearance.metrics.trackMinWidth;
  const int minTrackWidth = thumbSize + appearance.metrics.trackPadding * 2;

  const ResolvedStateContent unchecked = resolveStateContent(sw, false);
  const ResolvedStateContent checked = resolveStateContent(sw, true);
  const int uncheckedWidth =
      detail::switchContentWidth(unchecked.text, unchecked.hasIconRef, appearance, sw.font());
  const int checkedWidth =
      detail::switchContentWidth(checked.text, checked.hasIconRef, appearance, sw.font());
  const int contentWidth = std::max(uncheckedWidth, checkedWidth);

  const int contentInsetNear =
      small ? appearance.metrics.contentInsetNearSmall : appearance.metrics.contentInsetNear;
  const int contentInsetFar =
      small ? appearance.metrics.contentInsetFarSmall : appearance.metrics.contentInsetFar;
  const int width =
      std::max({minWidth, minTrackWidth, contentWidth + contentInsetNear + contentInsetFar});
  return QSize(std::max(width, height), height);
}

QSize labelSizeHint(const AdSwitch& sw) {
  const QString rawText = sw.text();
  if (rawText.isEmpty()) {
    return QSize();
  }
  const QString displayText = stripMnemonicMarkers(rawText);
  if (displayText.isEmpty()) {
    return QSize();
  }
  const QFontMetrics fm(sw.font());
  return QSize(std::max(fm.horizontalAdvance(displayText), fm.boundingRect(displayText).width()),
               fm.height());
}

SwitchLayout buildSwitchLayout(const AdSwitch& sw, const detail::SwitchAppearance& appearance,
                               const QRect& bounds) {
  SwitchLayout layout;
  const QSize indicatorSize = indicatorSizeHint(sw, appearance);
  const QSize labelSize = labelSizeHint(sw);
  layout.indicatorWidth = indicatorSize.width();
  layout.indicatorHeight = indicatorSize.height();
  layout.spacing = labelSize.isValid() ? switchLabelSpacing(&sw) : 0;

  const qreal indicatorTop = bounds.top() + (bounds.height() - layout.indicatorHeight) / 2.0;
  const bool rtl = sw.layoutDirection() == Qt::RightToLeft;
  const int indicatorX = rtl ? bounds.right() - layout.indicatorWidth + 1 : bounds.left();
  layout.indicatorRect =
      QRectF(indicatorX, indicatorTop, layout.indicatorWidth, layout.indicatorHeight);

  if (!labelSize.isValid()) {
    return layout;
  }

  if (rtl) {
    const int labelWidth = std::max(0, bounds.width() - layout.indicatorWidth - layout.spacing);
    layout.labelRect = QRect(bounds.left(), bounds.top(), labelWidth, bounds.height());
  } else {
    const int labelLeft = bounds.left() + layout.indicatorWidth + layout.spacing;
    const int labelWidth = std::max(0, bounds.right() - labelLeft + 1);
    layout.labelRect = QRect(labelLeft, bounds.top(), labelWidth, bounds.height());
  }

  return layout;
}

qreal dragThumbPositionForPoint(const AdSwitch& sw, const detail::SwitchAppearance& appearance,
                                const QRectF& indicatorRect, const QPointF& point,
                                qreal devicePixelRatio) {
  const detail::SwitchGeometry uncheckedGeometry =
      detail::buildSwitchGeometry(indicatorRect, sw.layoutDirection(), appearance, sw.controlSize(),
                                  0.0, 0.0, 1.0, devicePixelRatio);
  const detail::SwitchGeometry checkedGeometry =
      detail::buildSwitchGeometry(indicatorRect, sw.layoutDirection(), appearance, sw.controlSize(),
                                  1.0, 0.0, 1.0, devicePixelRatio);

  const qreal startCenter = uncheckedGeometry.thumbRect.center().x();
  const qreal endCenter = checkedGeometry.thumbRect.center().x();
  if (qFuzzyCompare(startCenter, endCenter)) {
    return sw.isChecked() ? 1.0 : 0.0;
  }

  const qreal minCenter = std::min(startCenter, endCenter);
  const qreal maxCenter = std::max(startCenter, endCenter);
  const qreal clampedCenter = std::clamp(point.x(), minCenter, maxCenter);
  const qreal progress = (clampedCenter - startCenter) / (endCenter - startCenter);
  return std::clamp(progress, 0.0, 1.0);
}

QString switchDerivedAccessibleName(const AdSwitch* sw) {
  if (!sw) {
    return QString();
  }

  QString explicitName = sw->accessibleName().trimmed();
  if (!explicitName.isEmpty()) {
    return explicitName;
  }

  QString labelText = stripMnemonicMarkers(sw->text()).trimmed();
  if (!labelText.isEmpty()) {
    return labelText;
  }

  return (sw->isChecked() ? sw->checkedText() : sw->uncheckedText()).trimmed();
}

QString switchAccessibleValueText(const AdSwitch* sw) {
  if (!sw) {
    return QString();
  }
  QString stateText = (sw->isChecked() ? sw->checkedText() : sw->uncheckedText()).trimmed();
  if (!stateText.isEmpty()) {
    return stateText;
  }
  return sw->isChecked() ? AdSwitch::tr("On") : AdSwitch::tr("Off");
}

class AdSwitchAccessible final : public QAccessibleWidget {
 public:
  explicit AdSwitchAccessible(AdSwitch* sw) : QAccessibleWidget(sw, QAccessible::CheckBox) {}

  QString text(QAccessible::Text t) const override {
    const auto* sw = qobject_cast<AdSwitch*>(object());
    if (!sw) {
      return QAccessibleWidget::text(t);
    }

    switch (t) {
      case QAccessible::Name: {
        const QString name = switchDerivedAccessibleName(sw);
        return name.isEmpty() ? AdSwitch::tr("Switch") : name;
      }
      case QAccessible::Description:
        return sw->accessibleDescription().trimmed();
      case QAccessible::Value:
        return switchAccessibleValueText(sw);
      default:
        return QAccessibleWidget::text(t);
    }
  }

  QAccessible::State state() const override {
    QAccessible::State st = QAccessibleWidget::state();
    const auto* sw = qobject_cast<AdSwitch*>(object());
    if (!sw) {
      return st;
    }

    st.checkable = true;
    st.checked = sw->isChecked();
    st.busy = sw->loading();
    st.focusable = true;
    return st;
  }
};

QAccessibleInterface* switchAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* sw = qobject_cast<AdSwitch*>(object)) {
    return new AdSwitchAccessible(sw);
  }
  return nullptr;
}

void ensureSwitchAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(switchAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

}  // namespace

struct AdSwitch::Private {
  struct AccessibleSnapshot {
    QString name;
    QString value;
    bool checked = false;
    bool busy = false;
    bool disabled = false;
    bool initialized = false;
  };

  struct ResolvedTokensCache {
    bool valid = false;
    ComponentTokens tokens;
  };

  struct AppearanceCache {
    bool valid = false;
    quint64 themeRevision = 0;
    qint64 paletteCacheKey = 0;
    detail::SwitchAppearance appearance;
  };

  struct LayoutCache {
    bool valid = false;
    QRect bounds;
    QString text;
    QFont font;
    SwitchLayout layout;
  };

  struct SizeHintCache {
    bool valid = false;
    QString text;
    QFont font;
    QSize size;
  };

  ControlSize controlSize = ControlSize::Medium;
  bool loading = false;
  QString checkedText;
  QString uncheckedText;
  adqt::icons::IconRef checkedIconRef;
  adqt::icons::IconRef uncheckedIconRef;
  ComponentTokens componentTokens;
  ComponentTokenResolver componentTokenResolver;
  bool hovered = false;
  bool pressed = false;
  bool focusVisible = false;
  bool enterPressed = false;
  bool dragPending = false;
  bool dragActive = false;
  bool explicitCursorOverride = false;
  bool autoCursorManaged = false;
  bool applyingAutoCursor = false;
  qreal pressDirection = 1.0;
  qreal dragThumbPosition = 0.0;
  QPoint dragPressPosition;
  AccessibleSnapshot accessible;
  mutable ResolvedTokensCache resolvedTokensCache;
  mutable AppearanceCache appearanceCache;
  mutable LayoutCache layoutCache;
  mutable SizeHintCache sizeHintCache;
  std::unique_ptr<detail::AnimatedScalar> thumbAnimator =
      std::make_unique<detail::AnimatedScalar>();
  std::unique_ptr<detail::AnimatedScalar> pressAnimator =
      std::make_unique<detail::AnimatedScalar>();
  std::unique_ptr<detail::FrameLoop> spinnerLoop = std::make_unique<detail::FrameLoop>();

  void invalidateLayoutCache(bool invalidateSizeHint) const {
    layoutCache.valid = false;
    if (invalidateSizeHint) {
      sizeHintCache.valid = false;
    }
  }

  void invalidateAppearanceCache(bool invalidateSizeHint) const {
    appearanceCache.valid = false;
    invalidateLayoutCache(invalidateSizeHint);
  }

  void invalidateResolvedTokensCache() const {
    resolvedTokensCache.valid = false;
    appearanceCache.valid = false;
  }

  SwitchLayout layoutFor(const AdSwitch& sw, const detail::SwitchAppearance& appearance,
                         const QRect& bounds) const {
    const QString text = sw.text();
    const QFont& currentFont = sw.font();
    if (!layoutCache.valid || layoutCache.bounds != bounds || layoutCache.text != text ||
        layoutCache.font != currentFont) {
      layoutCache.valid = true;
      layoutCache.bounds = bounds;
      layoutCache.text = text;
      layoutCache.font = currentFont;
      layoutCache.layout = buildSwitchLayout(sw, appearance, bounds);
    }
    return layoutCache.layout;
  }

  QSize sizeHintFor(const AdSwitch& sw, const detail::SwitchAppearance& appearance) const {
    const QString text = sw.text();
    const QFont& currentFont = sw.font();
    if (!sizeHintCache.valid || sizeHintCache.text != text || sizeHintCache.font != currentFont) {
      const QSize indicatorSize = indicatorSizeHint(sw, appearance);
      const QSize labelSize = labelSizeHint(sw);
      const int spacing = labelSize.isValid() ? switchLabelSpacing(&sw) : 0;
      const int width =
          indicatorSize.width() + (labelSize.isValid() ? spacing + labelSize.width() : 0);
      const int height = std::max(indicatorSize.height(), labelSize.height());
      sizeHintCache.valid = true;
      sizeHintCache.text = text;
      sizeHintCache.font = currentFont;
      sizeHintCache.size = QSize(std::max(width, height), height);
    }
    return sizeHintCache.size;
  }
};

AdSwitch::AdSwitch(QWidget* parent) : QAbstractButton(parent), d_(std::make_unique<Private>()) {
  ensureSwitchAccessibleFactoryInstalled();

  setCheckable(true);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_Hover, true);
  setAttribute(Qt::WA_TranslucentBackground, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAutoFillBackground(false);

  QSizePolicy policy = sizePolicy();
  policy.setHorizontalPolicy(QSizePolicy::Preferred);
  policy.setVerticalPolicy(QSizePolicy::Fixed);
  setSizePolicy(policy);

  d_->thumbAnimator->configure(this, QString::fromLatin1(kThumbFrameKey), [this]() {
    refreshFocusOverlay();
    update();
  });
  d_->pressAnimator->configure(this, QString::fromLatin1(kPressStateFrameKey), [this]() {
    refreshFocusOverlay();
    update();
  });
  d_->spinnerLoop->configure(this, QString::fromLatin1(kSpinnerFrameKey), [this](qint64, qint64) {
    if (d_->loading && isVisible()) {
      update();
    }
  });

  connect(this, &QAbstractButton::toggled, this, [this](bool) {
    d_->dragThumbPosition = isChecked() ? 1.0 : 0.0;
    invalidateResolvedTokensCache();
    invalidateAppearanceCache(false);
    refreshAccessibleState();
    refreshThumbAnimation(false);
    refreshAutomaticCursor();
    refreshFocusOverlay();
    update();
  });

  d_->thumbAnimator->snapTo(isChecked() ? 1.0 : 0.0);
  d_->pressAnimator->snapTo(0.0);
  d_->dragThumbPosition = isChecked() ? 1.0 : 0.0;
  d_->pressDirection =
      (layoutDirection() == Qt::RightToLeft ? -1.0 : 1.0) * (isChecked() ? -1.0 : 1.0);
  d_->explicitCursorOverride = testAttribute(Qt::WA_SetCursor);
  refreshAutomaticCursor();
  refreshAccessibleState();
}

AdSwitch::~AdSwitch() {
  stopInteractionWaveForOwner(this);
  stopInteractionFocusForOwner(this);
  stopAnimations();
}

AdSwitch::ControlSize AdSwitch::controlSize() const { return d_->controlSize; }

void AdSwitch::setControlSize(ControlSize value) {
  if (d_->controlSize == value) {
    return;
  }
  d_->controlSize = value;
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(true);
  updateGeometry();
  refreshAccessibleState();
  refreshThumbAnimation(true);
  refreshPressAnimation(true);
  refreshSpinnerLoop();
  refreshAutomaticCursor();
  refreshFocusOverlay();
  update();
  emit controlSizeChanged(d_->controlSize);
}

bool AdSwitch::loading() const { return d_->loading; }

void AdSwitch::setLoading(bool value) {
  if (d_->loading == value) {
    return;
  }
  d_->loading = value;
  if (d_->loading) {
    d_->enterPressed = false;
    resetDragState();
    setPressedState(false, true);
    setDown(false);
    stopInteractionWaveForOwner(this);
  }
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(false);
  refreshAccessibleState();
  refreshThumbAnimation(true);
  refreshPressAnimation(true);
  refreshSpinnerLoop();
  refreshAutomaticCursor();
  refreshFocusOverlay();
  update();
  emit loadingChanged(d_->loading);
}

QString AdSwitch::checkedText() const { return d_->checkedText; }

void AdSwitch::setCheckedText(const QString& value) {
  if (d_->checkedText == value) {
    return;
  }
  d_->checkedText = value;
  invalidateLayoutCache(true);
  refreshAccessibleState();
  refreshFocusOverlay();
  updateGeometry();
  update();
  emit checkedTextChanged(d_->checkedText);
}

QString AdSwitch::uncheckedText() const { return d_->uncheckedText; }

void AdSwitch::setUncheckedText(const QString& value) {
  if (d_->uncheckedText == value) {
    return;
  }
  d_->uncheckedText = value;
  invalidateLayoutCache(true);
  refreshAccessibleState();
  refreshFocusOverlay();
  updateGeometry();
  update();
  emit uncheckedTextChanged(d_->uncheckedText);
}

adqt::icons::IconRef AdSwitch::checkedIconRef() const { return d_->checkedIconRef; }

void AdSwitch::setCheckedIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(d_->checkedIconRef, value)) {
    return;
  }
  d_->checkedIconRef = value;
  invalidateLayoutCache(true);
  refreshFocusOverlay();
  updateGeometry();
  update();
  emit checkedIconRefChanged(d_->checkedIconRef);
}

adqt::icons::IconRef AdSwitch::uncheckedIconRef() const { return d_->uncheckedIconRef; }

void AdSwitch::setUncheckedIconRef(const adqt::icons::IconRef& value) {
  if (iconRefsEqual(d_->uncheckedIconRef, value)) {
    return;
  }
  d_->uncheckedIconRef = value;
  invalidateLayoutCache(true);
  refreshFocusOverlay();
  updateGeometry();
  update();
  emit uncheckedIconRefChanged(d_->uncheckedIconRef);
}

AdSwitch::ComponentTokens AdSwitch::componentTokens() const { return d_->componentTokens; }

void AdSwitch::setComponentTokens(const ComponentTokens& options) {
  if (componentTokensEqual(d_->componentTokens, options)) {
    return;
  }
  d_->componentTokens = options;
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(true);
  updateGeometry();
  refreshAccessibleState();
  refreshThumbAnimation(true);
  refreshPressAnimation(true);
  refreshSpinnerLoop();
  refreshAutomaticCursor();
  refreshFocusOverlay();
  update();
  emit componentTokensChanged();
}

void AdSwitch::resetComponentTokens() {
  if (componentTokensEqual(d_->componentTokens, ComponentTokens{})) {
    return;
  }
  d_->componentTokens = {};
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(true);
  updateGeometry();
  refreshAccessibleState();
  refreshThumbAnimation(true);
  refreshPressAnimation(true);
  refreshSpinnerLoop();
  refreshAutomaticCursor();
  refreshFocusOverlay();
  update();
  emit componentTokensChanged();
}

void AdSwitch::setComponentTokenResolver(ComponentTokenResolver resolver) {
  d_->componentTokenResolver = std::move(resolver);
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(true);
  updateGeometry();
  refreshAccessibleState();
  refreshThumbAnimation(true);
  refreshPressAnimation(true);
  refreshSpinnerLoop();
  refreshAutomaticCursor();
  refreshFocusOverlay();
  update();
  emit componentTokensChanged();
}

QSize AdSwitch::sizeHint() const { return d_->sizeHintFor(*this, resolvedAppearance()); }

QSize AdSwitch::minimumSizeHint() const { return sizeHint(); }

bool AdSwitch::event(QEvent* event) {
  if (event && interactionBlocked() && event->type() == QEvent::Shortcut) {
    event->accept();
    return true;
  }

  if (event && event->type() == QEvent::ParentAboutToChange) {
    stopInteractionFocusForOwner(this);
    stopInteractionWaveForOwner(this);
  }

  const bool handled = QAbstractButton::event(event);
  if (!event) {
    return handled;
  }

  if (!d_->applyingAutoCursor) {
    const bool explicitCursorOverride = testAttribute(Qt::WA_SetCursor);
    if (d_->explicitCursorOverride != explicitCursorOverride) {
      d_->autoCursorManaged = false;
      d_->explicitCursorOverride = explicitCursorOverride;
      refreshAutomaticCursor();
    }
  }

  switch (event->type()) {
    case QEvent::CursorChange:
      if (!d_->applyingAutoCursor) {
        d_->autoCursorManaged = false;
        d_->explicitCursorOverride = testAttribute(Qt::WA_SetCursor);
        refreshAutomaticCursor();
      }
      break;
    case QEvent::LayoutRequest:
      invalidateLayoutCache(true);
      refreshAccessibleState();
      refreshFocusOverlay();
      break;
    case QEvent::ParentChange:
    case QEvent::ZOrderChange:
      refreshFocusOverlay();
      break;
    default:
      break;
  }

  return handled;
}

bool AdSwitch::hitButton(const QPoint& pos) const { return rect().contains(pos); }

void AdSwitch::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  const detail::SwitchAppearance appearance = resolvedAppearance();
  const SwitchLayout layout = d_->layoutFor(*this, appearance, rect());
  const Qt::LayoutDirection direction = layoutDirection();
  const qreal thumbPosition = visualThumbPosition();
  const qreal activePressProgress =
      interactionBlocked() || !d_->pressAnimator ? 0.0 : d_->pressAnimator->value();
  const bool small = d_->controlSize == ControlSize::Small;
  const int contentInsetNear =
      small ? appearance.metrics.contentInsetNearSmall : appearance.metrics.contentInsetNear;
  const int contentInsetFar =
      small ? appearance.metrics.contentInsetFarSmall : appearance.metrics.contentInsetFar;
  const int contentPressOffset =
      small ? appearance.metrics.contentPressOffsetSmall : appearance.metrics.contentPressOffset;
  const int contentTravel = std::max(0, contentInsetFar - contentInsetNear);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
  const detail::SwitchGeometry geometry =
      detail::buildSwitchGeometry(layout.indicatorRect, direction, appearance, d_->controlSize,
                                  thumbPosition, activePressProgress, d_->pressDirection, dpr);

  const QColor uncheckedTrackColor = d_->hovered && !interactionBlocked()
                                         ? appearance.uncheckedTrackHoverColor
                                         : appearance.uncheckedTrackColor;
  const QColor checkedTrackColor = d_->hovered && !interactionBlocked()
                                       ? appearance.checkedTrackHoverColor
                                       : appearance.checkedTrackColor;
  const QColor trackColor = blendColor(uncheckedTrackColor, checkedTrackColor, thumbPosition);
  const QPainterPath trackPath =
      roundedRectPath(geometry.trackRect, geometry.trackRadius, geometry.trackRadius,
                      geometry.trackRadius, geometry.trackRadius);
  painter.fillPath(trackPath, trackColor);
  if (appearance.trackBorderColor.alpha() > 0) {
    painter.setPen(QPen(appearance.trackBorderColor, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(trackPath);
  }

  auto drawContent = [&](bool checkedState, qreal alpha) {
    if (alpha <= 0.0) {
      return;
    }

    const ResolvedStateContent content = resolveStateContent(*this, checkedState);
    const bool hasIcon = content.hasIconRef;
    const bool hasText = !content.text.trimmed().isEmpty();
    if (!hasIcon && !hasText) {
      return;
    }

    QFont contentFont = font();
    contentFont.setPixelSize(appearance.metrics.fontSize);
    painter.setFont(contentFont);
    const QFontMetrics fm(contentFont);
    const int iconSide = std::max(10, appearance.metrics.fontSize);
    const int textWidth = hasText ? std::max(0, fm.horizontalAdvance(content.text)) : 0;
    const int gap = hasIcon && hasText ? appearance.metrics.contentGap : 0;
    const int width = (hasIcon ? iconSide : 0) + gap + textWidth;

    qreal x = 0.0;
    if (checkedState) {
      x = xFromLogicalEdge(geometry.trackRect, direction, LogicalEdge::Start,
                           contentInsetNear + (1.0 - thumbPosition) * contentTravel, width);
    } else {
      x = xFromLogicalEdge(geometry.trackRect, direction, LogicalEdge::End,
                           contentInsetNear + thumbPosition * contentTravel, width);
    }
    if (activePressProgress > 0.0 && checkedState == isChecked()) {
      x += d_->pressDirection * contentPressOffset * activePressProgress;
    }
    const int contentHeight = std::max(iconSide, fm.height());
    const qreal y = geometry.trackRect.top() + (geometry.trackRect.height() - contentHeight) / 2.0;
    const qreal boxLeft = snapToDevicePixelCoord(x, dpr);
    const qreal boxTop = snapToDevicePixelCoord(y, dpr);
    const qreal iconTop =
        snapToDevicePixelCoord(geometry.trackRect.center().y() - iconSide / 2.0, dpr);
    const Qt::Alignment horizontalAlignment =
        direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft;

    const qreal oldOpacity = painter.opacity();
    painter.setOpacity(oldOpacity * alpha);
    painter.setPen(appearance.contentColor);

    if (hasIcon) {
      const qreal iconLeft = direction == Qt::RightToLeft ? boxLeft + width - iconSide : boxLeft;
      QRectF iconRect = snapRectToDevicePixels(QRectF(iconLeft, iconTop, iconSide, iconSide), dpr);
      adqt::icons::IconRef iconToRender = content.iconRef;
      if (shouldInheritCurrentColor(iconToRender)) {
        iconToRender =
            iconToRender.withColors(adqt::icons::IconColors::primary(appearance.contentColor));
      }
      const QPixmap pixmap = adqt::icons::renderIconPixmap(
          iconToRender, {QSize(iconSide, iconSide), dpr, QIcon::Normal, QIcon::Off});
      if (!pixmap.isNull()) {
        painter.drawPixmap(iconRect.topLeft(), pixmap);
      }
    }
    if (hasText) {
      const qreal textLeft =
          direction == Qt::RightToLeft ? boxLeft : boxLeft + (hasIcon ? iconSide + gap : 0);
      QRectF textRect =
          snapRectToDevicePixels(QRectF(textLeft, boxTop, textWidth, contentHeight), dpr);
      painter.drawText(textRect, static_cast<int>(Qt::AlignVCenter | horizontalAlignment),
                       content.text);
    }
    painter.setOpacity(oldOpacity);
  };

  drawContent(false, 1.0 - thumbPosition);
  drawContent(true, thumbPosition);

  if (!interactionBlocked() && appearance.metrics.thumbShadowColor.alpha() > 0) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(appearance.metrics.thumbShadowColor);
    const qreal shadowOffsetY = snapToDevicePixelCoord(appearance.metrics.thumbShadowOffsetY, dpr);
    painter.drawEllipse(
        snapRectToDevicePixels(geometry.thumbVisualRect.translated(0.0, shadowOffsetY), dpr));
  }

  if (appearance.thumbBorderColor.alpha() > 0) {
    painter.setPen(QPen(appearance.thumbBorderColor, 1.0));
  } else {
    painter.setPen(Qt::NoPen);
  }
  painter.setBrush(appearance.thumbColor);
  painter.drawEllipse(geometry.thumbVisualRect);

  if (d_->loading) {
    const QColor spinnerColor = thumbPosition >= 0.5 ? appearance.checkedLoadingIndicatorColor
                                                     : appearance.loadingIndicatorColor;
    const qreal maxSpinnerSize = d_->controlSize == ControlSize::Small
                                     ? static_cast<qreal>(appearance.metrics.thumbSizeSmall)
                                     : static_cast<qreal>(appearance.metrics.thumbSize);
    const qreal spinnerSize =
        std::clamp<qreal>(appearance.metrics.loadingIndicatorSize, 6.0, maxSpinnerSize);
    drawSpinner(&painter, geometry.thumbRect, spinnerColor, spinnerSize);
  }

  if (!layout.labelRect.isEmpty() && !QAbstractButton::text().isEmpty()) {
    QPalette labelPalette = palette();
    const Qt::Alignment labelAlignment =
        Qt::AlignVCenter | (direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft);
    painter.setFont(font());
    style()->drawItemText(&painter, layout.labelRect,
                          static_cast<int>(labelAlignment | Qt::TextShowMnemonic), labelPalette,
                          isEnabled(), QAbstractButton::text(), QPalette::WindowText);
  }
}

void AdSwitch::nextCheckState() {
  if (interactionBlocked()) {
    return;
  }
  QAbstractButton::nextCheckState();
}

void AdSwitch::enterEvent(QEnterEvent* event) {
  d_->hovered = true;
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(false);
  refreshFocusOverlay();
  update();
  QAbstractButton::enterEvent(event);
}

void AdSwitch::leaveEvent(QEvent* event) {
  d_->hovered = false;
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(false);
  if (!d_->dragActive) {
    setPressedState(false);
  }
  refreshFocusOverlay();
  update();
  QAbstractButton::leaveEvent(event);
}

void AdSwitch::mousePressEvent(QMouseEvent* event) {
  if (interactionBlocked()) {
    if (event) {
      event->ignore();
    }
    return;
  }

  if (d_->focusVisible) {
    d_->focusVisible = false;
    invalidateResolvedTokensCache();
    invalidateAppearanceCache(false);
    refreshFocusOverlay();
  }

  if (event && event->button() == Qt::LeftButton) {
    const detail::SwitchAppearance appearance = resolvedAppearance();
    const SwitchLayout layout = d_->layoutFor(*this, appearance, rect());
    d_->dragPending = layout.indicatorRect.contains(event->position());
    d_->dragActive = false;
    d_->dragThumbPosition = visualThumbPosition();
    d_->dragPressPosition = event->position().toPoint();
    setPressedState(true);
    update();
  }
  QAbstractButton::mousePressEvent(event);
}

void AdSwitch::mouseMoveEvent(QMouseEvent* event) {
  if (interactionBlocked()) {
    if (event) {
      event->ignore();
    }
    return;
  }

  if (event && d_->dragPending) {
    const QPoint current = event->position().toPoint();
    const QPoint delta = current - d_->dragPressPosition;
    if (!d_->dragActive) {
      const int dragDistance = QApplication::startDragDistance();
      if (std::abs(delta.x()) >= dragDistance && std::abs(delta.x()) >= std::abs(delta.y())) {
        d_->dragActive = true;
      }
    }

    if (d_->dragActive) {
      const detail::SwitchAppearance appearance = resolvedAppearance();
      const SwitchLayout layout = d_->layoutFor(*this, appearance, rect());
      d_->dragThumbPosition = dragThumbPositionForPoint(*this, appearance, layout.indicatorRect,
                                                        event->position(), devicePixelRatioF());
      update();
      event->accept();
      return;
    }
  }

  QAbstractButton::mouseMoveEvent(event);
}

void AdSwitch::mouseReleaseEvent(QMouseEvent* event) {
  if (interactionBlocked()) {
    resetDragState();
    setPressedState(false);
    setDown(false);
    if (event) {
      event->ignore();
    }
    update();
    return;
  }

  const bool triggerWave = event && event->button() == Qt::LeftButton && d_->pressed &&
                           rect().contains(event->position().toPoint());
  const bool wasDragActive = d_->dragActive;
  const qreal releaseThumbPosition = d_->dragThumbPosition;
  setPressedState(false);

  if (wasDragActive) {
    resetDragState();
    const bool nextChecked = releaseThumbPosition >= 0.5;
    const bool stateChanged = nextChecked != isChecked();
    setDown(false);
    emit released();
    if (stateChanged) {
      QAbstractButton::setChecked(nextChecked);
      emit clicked(isChecked());
    }
    if (triggerWave) {
      triggerWaveOverlay();
    }
    refreshAccessibleState();
    update();
    if (event) {
      event->accept();
    }
    return;
  }

  resetDragState();
  QAbstractButton::mouseReleaseEvent(event);
  if (triggerWave) {
    triggerWaveOverlay();
  }
  update();
}

void AdSwitch::keyPressEvent(QKeyEvent* event) {
  if (!event) {
    return;
  }

  if (interactionBlocked()) {
    d_->enterPressed = false;
    setPressedState(false, true);
    setDown(false);
    event->ignore();
    return;
  }

  if (isEnterKey(event->key())) {
    if (!event->isAutoRepeat()) {
      d_->enterPressed = true;
      setPressedState(true);
      setDown(true);
      update();
    }
    event->accept();
    return;
  }

  if (event->key() == Qt::Key_Space) {
    setPressedState(true);
    update();
  }
  QAbstractButton::keyPressEvent(event);
}

void AdSwitch::keyReleaseEvent(QKeyEvent* event) {
  if (!event) {
    return;
  }

  const bool enterKey = isEnterKey(event->key());
  const bool interactiveKey = event->key() == Qt::Key_Space;
  const bool triggerWave = !interactionBlocked() && interactiveKey;
  setPressedState(false);
  if (interactionBlocked()) {
    d_->enterPressed = false;
    setDown(false);
    event->ignore();
    update();
    return;
  }

  if (enterKey) {
    const bool triggerClick = d_->enterPressed && !event->isAutoRepeat();
    d_->enterPressed = false;
    setDown(false);
    if (triggerClick) {
      click();
      triggerWaveOverlay();
    }
    event->accept();
    update();
    return;
  }

  QAbstractButton::keyReleaseEvent(event);
  if (triggerWave) {
    triggerWaveOverlay();
  }
  update();
}

void AdSwitch::focusInEvent(QFocusEvent* event) {
  const bool focusVisible = event && isKeyboardFocusReason(event->reason());
  if (d_->focusVisible != focusVisible) {
    d_->focusVisible = focusVisible;
    invalidateResolvedTokensCache();
    invalidateAppearanceCache(false);
  }
  QAbstractButton::focusInEvent(event);
  refreshFocusOverlay();
  update();
}

void AdSwitch::focusOutEvent(QFocusEvent* event) {
  d_->focusVisible = false;
  d_->enterPressed = false;
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(false);
  resetDragState();
  setPressedState(false, true);
  setDown(false);
  QAbstractButton::focusOutEvent(event);
  stopInteractionFocusForOwner(this);
  update();
}

void AdSwitch::moveEvent(QMoveEvent* event) {
  QAbstractButton::moveEvent(event);
  refreshFocusOverlay();
}

void AdSwitch::resizeEvent(QResizeEvent* event) {
  invalidateLayoutCache(false);
  QAbstractButton::resizeEvent(event);
  refreshFocusOverlay();
}

void AdSwitch::showEvent(QShowEvent* event) {
  QAbstractButton::showEvent(event);
  refreshSpinnerLoop();
  refreshThumbAnimation(true);
  refreshPressAnimation(true);
  refreshFocusOverlay();
  refreshAccessibleState();
  detail::notifyAccessibilityEvent(this, QAccessible::ObjectShow);
}

void AdSwitch::hideEvent(QHideEvent* event) {
  QAbstractButton::hideEvent(event);
  d_->enterPressed = false;
  resetDragState();
  setPressedState(false, true);
  setDown(false);
  stopInteractionFocusForOwner(this);
  stopInteractionWaveForOwner(this);
  stopAnimations();
  detail::notifyAccessibilityEvent(this, QAccessible::ObjectHide);
}

void AdSwitch::changeEvent(QEvent* event) {
  QAbstractButton::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::EnabledChange:
      if (!isEnabled()) {
        d_->enterPressed = false;
        resetDragState();
        setPressedState(false, true);
        setDown(false);
        stopInteractionFocusForOwner(this);
        stopInteractionWaveForOwner(this);
      }
      invalidateResolvedTokensCache();
      invalidateAppearanceCache(false);
      refreshAutomaticCursor();
      refreshAccessibleState();
      refreshFocusOverlay();
      update();
      break;
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::StyleChange:
      invalidateAppearanceCache(true);
      refreshAccessibleState();
      refreshThumbAnimation(true);
      refreshPressAnimation(true);
      refreshSpinnerLoop();
      refreshAutomaticCursor();
      refreshFocusOverlay();
      updateGeometry();
      update();
      break;
    case QEvent::LanguageChange:
      refreshAccessibleState();
      update();
      break;
    default:
      break;
  }
}

bool AdSwitch::interactionBlocked() const { return !isEnabled() || d_->loading; }

detail::SwitchAppearanceInput AdSwitch::buildAppearanceInput() const {
  detail::SwitchAppearanceInput input;
  input.controlSize = d_->controlSize;
  input.checked = isChecked();
  input.loading = d_->loading;
  input.disabled = !isEnabled();
  input.hovered = d_->hovered;
  input.pressed = d_->pressed;
  input.focused = hasFocus() && d_->focusVisible;
  input.componentTokens = resolvedComponentTokens();
  return input;
}

detail::SwitchAppearance AdSwitch::resolvedAppearance() const {
  const auto& themeManager = adqt::theme::ThemeManager::instance();
  const quint64 themeRevision = themeManager.themeRevision();
  const qint64 paletteCacheKey = palette().cacheKey();
  if (!d_->appearanceCache.valid || d_->appearanceCache.themeRevision != themeRevision ||
      d_->appearanceCache.paletteCacheKey != paletteCacheKey) {
    const adqt::theme::ResolvedTheme resolvedTheme = themeManager.resolve(this);
    d_->appearanceCache.appearance =
        detail::resolveSwitchAppearance(buildAppearanceInput(), resolvedTheme);
    d_->appearanceCache.valid = true;
    d_->appearanceCache.themeRevision = themeRevision;
    d_->appearanceCache.paletteCacheKey = paletteCacheKey;
  }
  return d_->appearanceCache.appearance;
}

QRect AdSwitch::indicatorRect() const {
  return d_->layoutFor(*this, resolvedAppearance(), rect()).indicatorRect.toAlignedRect();
}

AdSwitch::ComponentTokens AdSwitch::resolvedComponentTokens() const {
  if (d_->resolvedTokensCache.valid) {
    return d_->resolvedTokensCache.tokens;
  }

  ComponentTokenContext ctx;
  ctx.controlSize = d_->controlSize;
  ctx.checked = isChecked();
  ctx.loading = d_->loading;
  ctx.disabled = !isEnabled();
  ctx.hovered = d_->hovered;
  ctx.pressed = d_->pressed;
  ctx.focused = hasFocus() && d_->focusVisible;

  ComponentTokens merged =
      d_->componentTokenResolver ? d_->componentTokenResolver(ctx) : ComponentTokens{};
  mergeOptional(&merged.colors.uncheckedTrack, d_->componentTokens.colors.uncheckedTrack);
  mergeOptional(&merged.colors.uncheckedTrackHover, d_->componentTokens.colors.uncheckedTrackHover);
  mergeOptional(&merged.colors.checkedTrack, d_->componentTokens.colors.checkedTrack);
  mergeOptional(&merged.colors.checkedTrackHover, d_->componentTokens.colors.checkedTrackHover);
  mergeOptional(&merged.colors.thumb, d_->componentTokens.colors.thumb);
  mergeOptional(&merged.colors.thumbBorder, d_->componentTokens.colors.thumbBorder);
  mergeOptional(&merged.colors.thumbShadow, d_->componentTokens.colors.thumbShadow);
  mergeOptional(&merged.colors.content, d_->componentTokens.colors.content);
  mergeOptional(&merged.colors.loadingIndicator, d_->componentTokens.colors.loadingIndicator);
  mergeOptional(&merged.colors.checkedLoadingIndicator,
                d_->componentTokens.colors.checkedLoadingIndicator);
  mergeOptional(&merged.colors.focusRing, d_->componentTokens.colors.focusRing);
  mergeOptional(&merged.colors.wave, d_->componentTokens.colors.wave);

  mergeOptional(&merged.metrics.trackHeight, d_->componentTokens.metrics.trackHeight);
  mergeOptional(&merged.metrics.smallTrackHeight, d_->componentTokens.metrics.smallTrackHeight);
  mergeOptional(&merged.metrics.trackMinWidth, d_->componentTokens.metrics.trackMinWidth);
  mergeOptional(&merged.metrics.smallTrackMinWidth, d_->componentTokens.metrics.smallTrackMinWidth);
  mergeOptional(&merged.metrics.trackPadding, d_->componentTokens.metrics.trackPadding);
  mergeOptional(&merged.metrics.thumbSize, d_->componentTokens.metrics.thumbSize);
  mergeOptional(&merged.metrics.smallThumbSize, d_->componentTokens.metrics.smallThumbSize);
  mergeOptional(&merged.metrics.loadingIndicatorSize,
                d_->componentTokens.metrics.loadingIndicatorSize);
  mergeOptional(&merged.metrics.disabledOpacity, d_->componentTokens.metrics.disabledOpacity);

  d_->resolvedTokensCache.valid = true;
  d_->resolvedTokensCache.tokens = merged;
  return merged;
}

QString AdSwitch::effectiveAccessibleName() const { return switchDerivedAccessibleName(this); }

QString AdSwitch::effectiveAccessibleValue() const { return switchAccessibleValueText(this); }

void AdSwitch::invalidateLayoutCache(bool invalidateSizeHint) const {
  d_->invalidateLayoutCache(invalidateSizeHint);
}

void AdSwitch::invalidateAppearanceCache(bool invalidateSizeHint) const {
  d_->invalidateAppearanceCache(invalidateSizeHint);
}

void AdSwitch::invalidateResolvedTokensCache() const { d_->invalidateResolvedTokensCache(); }

void AdSwitch::refreshAccessibleState() {
  const QString resolvedName = effectiveAccessibleName();
  const QString resolvedValue = effectiveAccessibleValue();
  const bool checked = isChecked();
  const bool busy = d_->loading;
  const bool disabled = !isEnabled();

  if (d_->accessible.initialized && isVisible()) {
    if (resolvedName != d_->accessible.name) {
      detail::notifyAccessibilityEvent(this, QAccessible::NameChanged);
    }
    if (resolvedValue != d_->accessible.value) {
      QAccessibleValueChangeEvent event(this, QVariant(resolvedValue));
      QAccessible::updateAccessibility(&event);
    }
    if (checked != d_->accessible.checked || busy != d_->accessible.busy ||
        disabled != d_->accessible.disabled) {
      QAccessible::State changedState;
      if (checked != d_->accessible.checked) {
        changedState.checked = true;
      }
      if (busy != d_->accessible.busy) {
        changedState.busy = true;
      }
      if (disabled != d_->accessible.disabled) {
        changedState.disabled = true;
      }
      QAccessibleStateChangeEvent event(this, changedState);
      QAccessible::updateAccessibility(&event);
    }
  }

  d_->accessible.name = resolvedName;
  d_->accessible.value = resolvedValue;
  d_->accessible.checked = checked;
  d_->accessible.busy = busy;
  d_->accessible.disabled = disabled;
  d_->accessible.initialized = true;
}

void AdSwitch::refreshAutomaticCursor() {
  if (d_->explicitCursorOverride) {
    return;
  }
  applyAutomaticCursor(interactionBlocked()
                           ? std::optional<Qt::CursorShape>{}
                           : std::optional<Qt::CursorShape>{Qt::PointingHandCursor});
}

void AdSwitch::applyAutomaticCursor(std::optional<Qt::CursorShape> cursorShape) {
  if (d_->explicitCursorOverride) {
    return;
  }

  if (cursorShape.has_value()) {
    if (!d_->autoCursorManaged || !testAttribute(Qt::WA_SetCursor) ||
        cursor().shape() != cursorShape.value()) {
      d_->applyingAutoCursor = true;
      QAbstractButton::setCursor(QCursor(cursorShape.value()));
      d_->applyingAutoCursor = false;
    }
    d_->autoCursorManaged = true;
    return;
  }

  if (d_->autoCursorManaged || testAttribute(Qt::WA_SetCursor)) {
    d_->applyingAutoCursor = true;
    QAbstractButton::unsetCursor();
    d_->applyingAutoCursor = false;
  }
  d_->autoCursorManaged = false;
}

void AdSwitch::setPressedState(bool value, bool immediate) {
  if (d_->pressed == value) {
    if (immediate) {
      refreshPressAnimation(true);
    }
    return;
  }
  if (value) {
    const qreal logicalDirection = layoutDirection() == Qt::RightToLeft ? -1.0 : 1.0;
    d_->pressDirection = logicalDirection * (isChecked() ? -1.0 : 1.0);
  }
  d_->pressed = value;
  invalidateResolvedTokensCache();
  invalidateAppearanceCache(false);
  refreshPressAnimation(immediate);
  refreshFocusOverlay();
}

void AdSwitch::refreshThumbAnimation(bool immediate) {
  if (!d_->thumbAnimator || d_->dragActive) {
    return;
  }
  const qreal target = isChecked() ? 1.0 : 0.0;
  const detail::SwitchAppearance appearance = resolvedAppearance();
  if (immediate || appearance.metrics.animationDurationMs <= 0 || !isVisible()) {
    d_->thumbAnimator->snapTo(target);
    d_->dragThumbPosition = target;
    return;
  }
  d_->thumbAnimator->animateTo(target, appearance.metrics.animationDurationMs);
}

void AdSwitch::refreshPressAnimation(bool immediate) {
  if (!d_->pressAnimator) {
    return;
  }
  const qreal target = (d_->pressed && !interactionBlocked()) ? 1.0 : 0.0;
  const detail::SwitchAppearance appearance = resolvedAppearance();
  if (immediate || appearance.metrics.animationDurationMs <= 0 || !isVisible()) {
    d_->pressAnimator->snapTo(target);
    return;
  }
  d_->pressAnimator->animateTo(target, appearance.metrics.animationDurationMs);
}

void AdSwitch::refreshSpinnerLoop() {
  if (!d_->spinnerLoop) {
    return;
  }
  d_->spinnerLoop->setRunning(d_->loading && isVisible());
}

void AdSwitch::refreshFocusOverlay() {
  if (!(hasFocus() && d_->focusVisible) || !isEnabled() || !isVisible()) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const detail::SwitchAppearance appearance = resolvedAppearance();
  const SwitchLayout layout = d_->layoutFor(*this, appearance, rect());
  const detail::SwitchGeometry geometry = detail::buildSwitchGeometry(
      layout.indicatorRect, layoutDirection(), appearance, d_->controlSize, visualThumbPosition(),
      0.0, d_->pressDirection, devicePixelRatioF());

  QWidget* hostWindow = window();
  if (!hostWindow) {
    stopInteractionFocusForOwner(this);
    return;
  }

  InteractionFocusRequest request;
  request.owner = this;
  const QPoint origin = mapTo(hostWindow, QPoint(0, 0));
  request.baseRectInWindow = geometry.trackRect.translated(origin.x(), origin.y());
  request.topLeft = geometry.trackRadius;
  request.topRight = geometry.trackRadius;
  request.bottomRight = geometry.trackRadius;
  request.bottomLeft = geometry.trackRadius;
  request.color = appearance.focusRingColor;
  request.strokeWidth = appearance.metrics.focusRingWidth;
  request.offset = appearance.metrics.focusRingOffset;
  triggerInteractionFocus(request);
}

void AdSwitch::triggerWaveOverlay() {
  if (interactionBlocked() || !isVisible()) {
    return;
  }

  const detail::SwitchAppearance appearance = resolvedAppearance();
  const SwitchLayout layout = d_->layoutFor(*this, appearance, rect());
  const detail::SwitchGeometry geometry = detail::buildSwitchGeometry(
      layout.indicatorRect, layoutDirection(), appearance, d_->controlSize, visualThumbPosition(),
      0.0, d_->pressDirection, devicePixelRatioF());

  QWidget* hostWindow = window();
  if (!hostWindow) {
    return;
  }

  InteractionWaveRequest request;
  request.owner = this;
  const QPoint origin = mapTo(hostWindow, QPoint(0, 0));
  request.baseRectInWindow = geometry.trackRect.translated(origin.x(), origin.y());
  request.topLeft = geometry.trackRadius;
  request.topRight = geometry.trackRadius;
  request.bottomRight = geometry.trackRadius;
  request.bottomLeft = geometry.trackRadius;
  request.color = appearance.waveColor;
  triggerInteractionWave(request);
}

void AdSwitch::stopAnimations() {
  if (d_->thumbAnimator) {
    d_->thumbAnimator->stop();
  }
  if (d_->pressAnimator) {
    d_->pressAnimator->stop();
  }
  if (d_->spinnerLoop) {
    d_->spinnerLoop->stop();
  }
}

void AdSwitch::resetDragState() {
  d_->dragPending = false;
  d_->dragActive = false;
  d_->dragThumbPosition = isChecked() ? 1.0 : 0.0;
}

qreal AdSwitch::visualThumbPosition() const {
  if (d_->dragActive) {
    return d_->dragThumbPosition;
  }
  return d_->thumbAnimator ? d_->thumbAnimator->value() : (isChecked() ? 1.0 : 0.0);
}

void AdSwitch::drawSpinner(QPainter* painter, const QRectF& rect, const QColor& color,
                           qreal preferredSize) const {
  if (!painter || !rect.isValid()) {
    return;
  }

  const qreal maxSide = std::max<qreal>(0.0, std::min(rect.width(), rect.height()) - 2.0);
  const qreal unclampedSide = std::max<qreal>(preferredSize + 3.0, 8.0);
  const qreal side = std::clamp(unclampedSide, 8.0,
                                maxSide > 0.0 ? maxSide : std::min(rect.width(), rect.height()));
  const qreal dpr =
      painter->device() ? painter->device()->devicePixelRatioF() : devicePixelRatioF();
  const QPointF center(snapToDevicePixelCoord(rect.center().x(), dpr),
                       snapToDevicePixelCoord(rect.center().y(), dpr));
  const qreal alignedSide = snapToDevicePixelSize(side, dpr);
  const qreal strokeWidth = snapToDevicePixelSize(std::clamp(side * 0.11, 1.1, 1.8), dpr);
  QRectF targetRect(center.x() - alignedSide / 2.0, center.y() - alignedSide / 2.0, alignedSide,
                    alignedSide);
  targetRect = targetRect.adjusted(strokeWidth / 2.0, strokeWidth / 2.0, -strokeWidth / 2.0,
                                   -strokeWidth / 2.0);
  targetRect = snapRectToDevicePixels(targetRect, dpr);

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->translate(center);
  painter->rotate(static_cast<qreal>(sharedSpinnerAngle()));
  painter->translate(-center);

  QPen pen(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter->setPen(pen);
  painter->setBrush(Qt::NoBrush);
  painter->drawArc(targetRect, 35 * 16, -155 * 16);
  painter->restore();
}

int AdSwitch::contentWidthForState(bool checkedState,
                                   const detail::SwitchAppearance& appearance) const {
  const ResolvedStateContent content = resolveStateContent(*this, checkedState);
  return detail::switchContentWidth(content.text, content.hasIconRef, appearance, font());
}

}  // namespace adqt::widgets
