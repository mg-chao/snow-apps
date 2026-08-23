#include "slider.h"

#include "detail/overlay_accessibility.h"
#include "slider_style.h"
#include "theme/theme.h"
#include "tooltip.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QGradient>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>

namespace adqt::widgets {

namespace {

constexpr double kEpsilon = 1e-6;
constexpr double kEditableHandleDeleteThresholdPx = 24.0;

struct HandleEntry {
  double value = 0.0;
  qint64 id = -1;
  int sourceIndex = -1;
};

bool fuzzyEq(double lhs, double rhs) { return std::abs(lhs - rhs) <= kEpsilon; }

int scaledControlMetric(int value, qreal scale) {
  if (value == 0) {
    return 0;
  }
  return qMax(1, qRound(static_cast<qreal>(value) * scale));
}

qreal scaledControlMetric(qreal value, qreal scale) { return value * scale; }

void scaleControlFont(QFont* font, qreal scale) {
  if (font == nullptr) {
    return;
  }
  if (font->pixelSize() > 0) {
    font->setPixelSize(scaledControlMetric(font->pixelSize(), scale));
  } else if (font->pointSizeF() > 0.0) {
    font->setPointSizeF(font->pointSizeF() * scale);
  }
}

void scaleSliderControlMetrics(detail::SliderMetrics* metrics, qreal scale) {
  if (metrics == nullptr) {
    return;
  }
  metrics->controlSize = scaledControlMetric(metrics->controlSize, scale);
  metrics->railSize = scaledControlMetric(metrics->railSize, scale);
  metrics->handleSize = scaledControlMetric(metrics->handleSize, scale);
  metrics->handleSizeHover = scaledControlMetric(metrics->handleSizeHover, scale);
  metrics->handleLineWidth = scaledControlMetric(metrics->handleLineWidth, scale);
  metrics->handleLineWidthHover = scaledControlMetric(metrics->handleLineWidthHover, scale);
  metrics->dotSize = scaledControlMetric(metrics->dotSize, scale);
  metrics->marginMain = scaledControlMetric(metrics->marginMain, scale);
  metrics->marginCross = scaledControlMetric(metrics->marginCross, scale);
  metrics->markGap = scaledControlMetric(metrics->markGap, scale);
  metrics->focusOutlineSize = scaledControlMetric(metrics->focusOutlineSize, scale);
  scaleControlFont(&metrics->font, scale);
}

double clampValue(double value, double minValue, double maxValue) {
  if (maxValue < minValue) {
    return minValue;
  }
  return std::clamp(value, minValue, maxValue);
}

bool listFuzzyEquals(const QList<double>& lhs, const QList<double>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (int i = 0; i < lhs.size(); ++i) {
    if (!fuzzyEq(lhs.at(i), rhs.at(i))) {
      return false;
    }
  }
  return true;
}

bool marksEqual(const AdMultiSlider::MarkMap& lhs, const AdMultiSlider::MarkMap& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto it = lhs.cbegin(); it != lhs.cend(); ++it) {
    if (!rhs.contains(it.key())) {
      return false;
    }
    if (!(it.value() == rhs.value(it.key()))) {
      return false;
    }
  }
  return true;
}

bool semanticSlotStylesEqual(const AdMultiSlider::SemanticSlotStyle& lhs,
                             const AdMultiSlider::SemanticSlotStyle& rhs) {
  return lhs.textColor == rhs.textColor && lhs.backgroundColor == rhs.backgroundColor &&
         lhs.borderColor == rhs.borderColor && lhs.brush == rhs.brush;
}

bool semanticStylesEqual(const AdMultiSlider::SemanticStyles& lhs,
                         const AdMultiSlider::SemanticStyles& rhs) {
  return semanticSlotStylesEqual(lhs.root, rhs.root) &&
         semanticSlotStylesEqual(lhs.rail, rhs.rail) &&
         semanticSlotStylesEqual(lhs.track, rhs.track) &&
         semanticSlotStylesEqual(lhs.tracks, rhs.tracks) &&
         semanticSlotStylesEqual(lhs.handle, rhs.handle) &&
         semanticSlotStylesEqual(lhs.mark, rhs.mark) &&
         semanticSlotStylesEqual(lhs.markActive, rhs.markActive);
}

bool isKeyboardFocusReason(Qt::FocusReason reason) {
  return reason != Qt::MouseFocusReason && reason != Qt::NoFocusReason;
}

QString formatNumber(double value) {
  if (!std::isfinite(value)) {
    return QStringLiteral("0");
  }
  QString text = QString::number(value, 'f', 4);
  while (text.contains(QLatin1Char('.')) &&
         (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) {
    text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) {
      text.chop(1);
      break;
    }
  }
  if (text.isEmpty() || text == QStringLiteral("-0")) {
    return QStringLiteral("0");
  }
  return text;
}

int maxMarkLabelHeight(const AdMultiSlider::MarkMap& marks, const QFont& fallbackFont) {
  int maxHeight = 0;
  for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
    const QFont markFont = it->font.has_value() ? it->font.value() : fallbackFont;
    maxHeight = std::max(maxHeight, QFontMetrics(markFont).height());
  }
  return maxHeight;
}

int maxMarkLabelWidth(const AdMultiSlider::MarkMap& marks, const QFont& fallbackFont) {
  int maxWidth = 0;
  for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
    const QFont markFont = it->font.has_value() ? it->font.value() : fallbackFont;
    maxWidth = std::max(maxWidth, QFontMetrics(markFont).horizontalAdvance(it->label));
  }
  return maxWidth;
}

AdTooltip::Placement toTooltipComponentPlacement(AdMultiSlider::TooltipPlacement value) {
  switch (value) {
    case AdMultiSlider::TooltipPlacement::Top:
      return AdTooltip::Placement::Top;
    case AdMultiSlider::TooltipPlacement::Bottom:
      return AdTooltip::Placement::Bottom;
    case AdMultiSlider::TooltipPlacement::Left:
      return AdTooltip::Placement::Left;
    case AdMultiSlider::TooltipPlacement::Right:
      return AdTooltip::Placement::Right;
  }
  return AdTooltip::Placement::Top;
}

QColor lerpColor(const QColor& start, const QColor& end, double ratio) {
  const double t = std::clamp(ratio, 0.0, 1.0);
  return QColor(std::clamp(qRound(start.red() + (end.red() - start.red()) * t), 0, 255),
                std::clamp(qRound(start.green() + (end.green() - start.green()) * t), 0, 255),
                std::clamp(qRound(start.blue() + (end.blue() - start.blue()) * t), 0, 255),
                std::clamp(qRound(start.alpha() + (end.alpha() - start.alpha()) * t), 0, 255));
}

QColor sampleGradientStops(const QGradientStops& stops, double position, const QColor& fallback) {
  if (stops.isEmpty()) {
    return fallback;
  }

  const double target = std::clamp(position, 0.0, 1.0);
  if (target <= stops.constFirst().first) {
    return stops.constFirst().second;
  }
  if (target >= stops.constLast().first) {
    return stops.constLast().second;
  }

  for (int i = 0; i + 1 < stops.size(); ++i) {
    const auto& lhs = stops.at(i);
    const auto& rhs = stops.at(i + 1);
    if (target < lhs.first || target > rhs.first) {
      continue;
    }
    const double span = rhs.first - lhs.first;
    if (span <= kEpsilon) {
      return rhs.second;
    }
    return lerpColor(lhs.second, rhs.second, (target - lhs.first) / span);
  }

  return fallback;
}

QColor sampleBrushColor(const QBrush& brush, double position, const QColor& fallback) {
  const QGradient* gradient = brush.gradient();
  if (!gradient) {
    return fallback;
  }
  return sampleGradientStops(gradient->stops(), position, fallback);
}

double sliderAccessibleValue(const AdMultiSlider* slider) {
  if (!slider) {
    return 0.0;
  }
  const QList<double> values = slider->handleValues();
  const int activeIndex = slider->activeHandleIndex();
  if (activeIndex >= 0 && activeIndex < values.size()) {
    return values.at(activeIndex);
  }
  return slider->value();
}

QString sliderAccessibleDescription(const AdMultiSlider* slider) {
  if (!slider) {
    return QString();
  }
  const bool managedDescription =
      slider->property("_adqt_overlay_managed_accessible_description").toBool();
  QString explicitDescription = slider->accessibleDescription().trimmed();
  if (!managedDescription && !explicitDescription.isEmpty()) {
    return explicitDescription;
  }

  const QList<double> values = slider->handleValues();
  if (values.size() > 1) {
    const int activeIndex =
        std::clamp(slider->activeHandleIndex(), 0, static_cast<int>(values.size()) - 1);
    return AdMultiSlider::tr("Handle %1 of %2, Range %3 to %4")
        .arg(activeIndex + 1)
        .arg(values.size())
        .arg(formatNumber(slider->minimum()))
        .arg(formatNumber(slider->maximum()));
  }

  return AdMultiSlider::tr("Range %1 to %2")
      .arg(formatNumber(slider->minimum()))
      .arg(formatNumber(slider->maximum()));
}

bool isKeyAdjustAction(int key) {
  switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
      return true;
    default:
      return false;
  }
}

double effectiveSingleStepForSlider(const AdMultiSlider* slider) {
  if (!slider) {
    return 1.0;
  }
  if (slider->singleStep() > 0.0) {
    return slider->singleStep();
  }
  return std::max(1.0, (slider->maximum() - slider->minimum()) / 100.0);
}

double effectivePageStepForSlider(const AdMultiSlider* slider) {
  if (!slider) {
    return 10.0;
  }
  if (slider->pageStep() > 0.0) {
    return slider->pageStep();
  }
  return effectiveSingleStepForSlider(slider) * 10.0;
}

int adjustmentDirectionForKey(const AdMultiSlider* slider, int key) {
  int direction = 0;
  switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Down:
    case Qt::Key_PageDown:
      direction = -1;
      break;
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_PageUp:
      direction = 1;
      break;
    default:
      break;
  }
  if (slider && slider->orientation() == Qt::Horizontal &&
      slider->layoutDirection() == Qt::RightToLeft &&
      (key == Qt::Key_Left || key == Qt::Key_Right)) {
    direction *= -1;
  }
  if (slider && slider->invertedControls()) {
    direction *= -1;
  }
  return direction;
}

QAbstractSlider::SliderAction sliderActionForKey(const AdMultiSlider* slider, int key) {
  switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Down:
    case Qt::Key_Right:
    case Qt::Key_Up:
      return adjustmentDirectionForKey(slider, key) < 0 ? QAbstractSlider::SliderSingleStepSub
                                                        : QAbstractSlider::SliderSingleStepAdd;
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
      return adjustmentDirectionForKey(slider, key) < 0 ? QAbstractSlider::SliderPageStepSub
                                                        : QAbstractSlider::SliderPageStepAdd;
    case Qt::Key_Home:
      return QAbstractSlider::SliderToMinimum;
    case Qt::Key_End:
      return QAbstractSlider::SliderToMaximum;
    default:
      return QAbstractSlider::SliderNoAction;
  }
}

class AdMultiSliderAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
 public:
  explicit AdMultiSliderAccessible(AdMultiSlider* slider)
      : QAccessibleWidget(slider, QAccessible::Slider) {}

  QString text(QAccessible::Text t) const override {
    const auto* slider = qobject_cast<AdMultiSlider*>(object());
    if (!slider) {
      return QAccessibleWidget::text(t);
    }

    switch (t) {
      case QAccessible::Name: {
        const QString name = slider->accessibleName().trimmed();
        return name.isEmpty() ? AdMultiSlider::tr("Slider") : name;
      }
      case QAccessible::Description:
        return sliderAccessibleDescription(slider);
      case QAccessible::Value:
        return formatNumber(sliderAccessibleValue(slider));
      default:
        return QAccessibleWidget::text(t);
    }
  }

  void* interface_cast(QAccessible::InterfaceType type) override {
    if (type == QAccessible::ValueInterface) {
      return static_cast<QAccessibleValueInterface*>(this);
    }
    return QAccessibleWidget::interface_cast(type);
  }

  QVariant currentValue() const override {
    const auto* slider = qobject_cast<AdMultiSlider*>(object());
    return slider ? QVariant(sliderAccessibleValue(slider)) : QVariant();
  }

  void setCurrentValue(const QVariant& value) override {
    auto* slider = qobject_cast<AdMultiSlider*>(object());
    if (!slider) {
      return;
    }
    bool ok = false;
    const double next = value.toDouble(&ok);
    if (ok) {
      const QList<double> currentValues = slider->handleValues();
      const int activeIndex = slider->activeHandleIndex();
      if (activeIndex >= 0 && activeIndex < currentValues.size()) {
        QList<double> updatedValues = currentValues;
        updatedValues[activeIndex] = next;
        slider->setHandleValues(updatedValues);
        return;
      }
      slider->setValue(next);
    }
  }

  QVariant maximumValue() const override {
    const auto* slider = qobject_cast<AdMultiSlider*>(object());
    return slider ? QVariant(slider->maximum()) : QVariant();
  }

  QVariant minimumValue() const override {
    const auto* slider = qobject_cast<AdMultiSlider*>(object());
    return slider ? QVariant(slider->minimum()) : QVariant();
  }

  QVariant minimumStepSize() const override {
    const auto* slider = qobject_cast<AdMultiSlider*>(object());
    if (!slider) {
      return QVariant();
    }
    const double step = slider->singleStep() > 0.0 ? slider->singleStep() : 1.0;
    return QVariant(step);
  }
};

QAccessibleInterface* sliderAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* slider = qobject_cast<AdMultiSlider*>(object)) {
    return new AdMultiSliderAccessible(slider);
  }
  return nullptr;
}

void ensureSliderAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(sliderAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

}  // namespace

namespace {

struct SliderAxisGeometry {
  Qt::Orientation orientation = Qt::Horizontal;
  qreal visualStart = 0.0;
  qreal visualEnd = 1.0;
  qreal handleStart = 0.0;
  qreal handleEnd = 1.0;
  bool minimumAtVisualStart = true;

  qreal coordinate(const QPoint& point) const {
    return orientation == Qt::Horizontal ? point.x() : point.y();
  }

  qreal handleLength() const { return std::max<qreal>(1.0, handleEnd - handleStart); }

  int handleSpan() const { return std::max(1, qRound(handleLength())); }

  void setMainExtents(qreal extent, qreal handleInset, qreal displayExtension) {
    handleStart = handleInset;
    handleEnd = extent - handleInset;
    visualStart = std::max<qreal>(0.0, handleStart - displayExtension);
    visualEnd = std::min<qreal>(extent, handleEnd + displayExtension);
  }

  qreal visualMinimumPosition() const { return minimumAtVisualStart ? visualStart : visualEnd; }

  qreal visualMaximumPosition() const { return minimumAtVisualStart ? visualEnd : visualStart; }

  qreal handleMinimumPosition() const { return minimumAtVisualStart ? handleStart : handleEnd; }

  qreal valueDirectedDelta(const QPoint& from, const QPoint& to) const {
    const qreal visualDelta = coordinate(to) - coordinate(from);
    return minimumAtVisualStart ? visualDelta : -visualDelta;
  }

  int sliderUnitsFromHandlePosition(qreal position, int maximumUnits) const {
    const int offset = std::clamp(qRound(position - handleStart), 0, handleSpan());
    return QStyle::sliderValueFromPosition(0, maximumUnits, offset, handleSpan(),
                                           !minimumAtVisualStart);
  }

  int handlePositionFromSliderUnits(int sliderUnits, int maximumUnits) const {
    const int offset = QStyle::sliderPositionFromValue(0, maximumUnits, sliderUnits, handleSpan(),
                                                       !minimumAtVisualStart);
    return qRound(handleStart + offset);
  }
};

enum class TrackEndpoint : std::uint8_t {
  HandleCenter,
  VisualMinimum,
  VisualMaximum,
};

}  // namespace

struct AdMultiSlider::LayoutInfo {
  detail::SliderVisualStyle baseStyle;
  detail::SliderVisualStyle style;
  QRectF contentRect;
  QRectF railRect;
  QList<QRectF> handleRects;
  QList<QRectF> handleAnchorRects;
  QList<QPointF> markCenters;
  QList<double> markValues;
  bool vertical = false;
  SliderAxisGeometry axis;
  qreal crossCenter = 0.0;
  int activeHandleSize = 0;
  int normalHandleSize = 0;
  int markLabelOffset = 0;
};

class AdMultiSlider::TooltipHost final {
 public:
  explicit TooltipHost(QWidget* parent) {
    tooltip_ = new AdTooltip(parent);
    tooltip_->setObjectName(QStringLiteral("ad-slider-tooltip-host"));
    tooltip_->setActivationMode(AdTooltip::ActivationMode::Manual);
    // Keep the popup out of the slider's widget tree. An in-window tooltip can
    // cover its handle, producing a leave/enter cycle that repeatedly closes
    // and reopens the hover tooltip.
    tooltip_->setLayerMode(AdTooltip::LayerMode::TopLevelTransient);
    tooltip_->setArrowPointAtCenter(true);
    tooltip_->setHoverOpenDelayMs(0);
    tooltip_->setHoverCloseDelayMs(0);
    tooltip_->setTargetWidget(parent);
    tooltip_->setAnchorWidget(parent);
  }

  TooltipHost(const TooltipHost&) = delete;
  TooltipHost& operator=(const TooltipHost&) = delete;

  ~TooltipHost() {
    if (tooltip_) {
      tooltip_->setVisible(false);
      delete tooltip_;
      tooltip_ = nullptr;
    }
  }

  AdTooltip* component() const { return tooltip_; }

  void applyBaseState(bool disabled, AdTooltip::Placement placement, const QFont& textFont,
                      QWidget* targetWidget) {
    if (!tooltip_) {
      return;
    }
    tooltip_->setEnabled(!disabled);
    tooltip_->setPlacement(placement);
    AdTooltip::ComponentTokens tokens = tooltip_->componentTokens();
    tokens.textFont = textFont;
    tooltip_->setComponentTokens(tokens);
    tooltip_->setTargetWidget(targetWidget);
    tooltip_->setAnchorWidget(targetWidget);
  }

  void setAnchorRect(const QRect& rawAnchorRect, const QRect& boundsRect,
                     QWidget* coordinateWidget) {
    if (!tooltip_) {
      return;
    }

    const QRect anchorRect = rawAnchorRect.intersected(boundsRect);
    if (anchorRect.isEmpty()) {
      tooltip_->clearAnchorRect();
      return;
    }
    tooltip_->setAnchorWidget(coordinateWidget);
    tooltip_->setAnchorRect(anchorRect);
  }

  void setContentAndOpen(const QString& text, bool open) {
    if (!tooltip_) {
      return;
    }
    tooltip_->setText(text);
    tooltip_->setVisible(open);
  }

 private:
  QPointer<AdTooltip> tooltip_;
};

AdMultiSlider::AdMultiSlider(QWidget* parent) : QWidget(parent) {
  ensureSliderAccessibleFactoryInstalled();
  setAttribute(Qt::WA_Hover, true);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  handles_ = {minimum_, minimum_};
  committedHandles_ = handles_;
  handleIds_ = {nextHandleId_++, nextHandleId_++};
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  lastDisabledState_ = disabled();
  syncInteractionCursor();
  syncAccessibleState();
}

AdMultiSlider::~AdMultiSlider() {
  tooltipSyncPending_ = false;
  clearTooltipHosts();
}

AdMultiSlider::Mode AdMultiSlider::mode() const { return mode_; }

void AdMultiSlider::setMode(Mode value) {
  if (mode_ == value) {
    return;
  }
  mode_ = value;
  setHandlesInternal(handles_, true, false);
  refreshAfterPropertyChange();
}

double AdMultiSlider::minimum() const { return minimum_; }

void AdMultiSlider::setMinimum(double value) {
  if (fuzzyEq(minimum_, value)) {
    return;
  }
  const double previousMinimum = minimum_;
  const double previousMaximum = maximum_;
  minimum_ = value;
  if (maximum_ < minimum_) {
    maximum_ = minimum_;
    emit maximumChanged(maximum_);
  }
  setHandlesInternal(handles_, true, false);
  emit minimumChanged(minimum_);
  if (!fuzzyEq(previousMinimum, minimum_) || !fuzzyEq(previousMaximum, maximum_)) {
    emit rangeChanged(minimum_, maximum_);
  }
  refreshAfterPropertyChange();
}

double AdMultiSlider::maximum() const { return maximum_; }

void AdMultiSlider::setMaximum(double value) {
  if (fuzzyEq(maximum_, value)) {
    return;
  }
  const double previousMinimum = minimum_;
  const double previousMaximum = maximum_;
  maximum_ = value;
  if (minimum_ > maximum_) {
    minimum_ = maximum_;
    emit minimumChanged(minimum_);
  }
  setHandlesInternal(handles_, true, false);
  emit maximumChanged(maximum_);
  if (!fuzzyEq(previousMinimum, minimum_) || !fuzzyEq(previousMaximum, maximum_)) {
    emit rangeChanged(minimum_, maximum_);
  }
  refreshAfterPropertyChange();
}

double AdMultiSlider::singleStep() const { return step_; }

void AdMultiSlider::setSingleStep(double value) {
  const double normalized = value <= 0.0 ? 0.0 : value;
  if (fuzzyEq(step_, normalized)) {
    return;
  }
  step_ = normalized;
  setHandlesInternal(handles_, true, false);
  emit singleStepChanged(step_);
  refreshAfterPropertyChange(false);
}

double AdMultiSlider::pageStep() const { return pageStep_; }

void AdMultiSlider::setPageStep(double value) {
  const double normalized = value <= 0.0 ? 0.0 : value;
  if (fuzzyEq(pageStep_, normalized)) {
    return;
  }
  pageStep_ = normalized;
  emit pageStepChanged(pageStep_);
}

bool AdMultiSlider::tracking() const { return tracking_; }

void AdMultiSlider::setTracking(bool value) {
  if (tracking_ == value) {
    return;
  }
  tracking_ = value;
  emit trackingChanged(tracking_);
}

bool AdMultiSlider::invertedAppearance() const { return invertedAppearance_; }

void AdMultiSlider::setInvertedAppearance(bool value) {
  setInversionState(value, invertedControls_);
}

bool AdMultiSlider::invertedControls() const { return invertedControls_; }

void AdMultiSlider::setInvertedControls(bool value) {
  setInversionState(invertedAppearance_, value);
}

bool AdMultiSlider::markSnapEnabled() const { return marksOnly_; }

void AdMultiSlider::setMarkSnapEnabled(bool value) {
  if (marksOnly_ == value) {
    return;
  }
  marksOnly_ = value;
  setHandlesInternal(handles_, true, false);
  emit markSnapEnabledChanged(marksOnly_);
  refreshAfterPropertyChange(false);
}

bool AdMultiSlider::markIndicatorsVisible() const { return markIndicatorsVisible_; }

void AdMultiSlider::setMarkIndicatorsVisible(bool value) {
  if (markIndicatorsVisible_ == value) {
    return;
  }
  markIndicatorsVisible_ = value;
  emit markIndicatorsVisibleChanged(markIndicatorsVisible_);
  update();
}

bool AdMultiSlider::markStepSnapEnabled() const { return markStepSnapEnabled_; }

void AdMultiSlider::setMarkStepSnapEnabled(bool value) {
  if (markStepSnapEnabled_ == value) {
    return;
  }
  markStepSnapEnabled_ = value;
  setHandlesInternal(handles_, true, false);
  emit markStepSnapEnabledChanged(markStepSnapEnabled_);
  refreshAfterPropertyChange(false);
}

bool AdMultiSlider::selectionHighlightVisible() const { return included_; }

void AdMultiSlider::setSelectionHighlightVisible(bool value) {
  if (included_ == value) {
    return;
  }
  included_ = value;
  emit selectionHighlightVisibleChanged(included_);
  update();
}

Qt::Orientation AdMultiSlider::orientation() const { return orientation_; }

void AdMultiSlider::setOrientation(Qt::Orientation value) {
  if (orientation_ == value) {
    return;
  }
  orientation_ = value;
  setSizePolicy(orientation_ == Qt::Horizontal ? QSizePolicy::Expanding : QSizePolicy::Fixed,
                orientation_ == Qt::Horizontal ? QSizePolicy::Fixed : QSizePolicy::Expanding);
  emit orientationChanged(orientation_);
  refreshAfterPropertyChange();
}

bool AdMultiSlider::disabled() const { return !isEnabled(); }

void AdMultiSlider::setDisabled(bool value) {
  if (disabled() == value) {
    return;
  }
  setEnabled(!value);
  hovered_ = false;
  setHoverHandleIndex(-1);
  dragMode_ = DragMode::None;
  dragging_ = false;
  pressedHandleIndex_ = -1;
  setSliderDownInternal(false);
  dragChanged_ = false;
  pendingPrimaryValueChange_ = false;
  pendingValuesChange_ = false;
  setDragHandleIndex(-1);
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

bool AdMultiSlider::keyboardEnabled() const { return keyboardEnabled_; }

void AdMultiSlider::setKeyboardEnabled(bool value) {
  if (keyboardEnabled_ == value) {
    return;
  }
  keyboardEnabled_ = value;
  emit keyboardEnabledChanged(keyboardEnabled_);
}

bool AdMultiSlider::wheelEnabled() const { return wheelEnabled_; }

void AdMultiSlider::setWheelEnabled(bool value) {
  if (wheelEnabled_ == value) {
    return;
  }
  wheelEnabled_ = value;
  emit wheelEnabledChanged(wheelEnabled_);
}

double AdMultiSlider::value() const {
  return committedHandles_.isEmpty() ? minimum_ : committedHandles_.constFirst();
}

void AdMultiSlider::setValue(double value) {
  if (mode_ == Mode::Single) {
    setHandlesInternal({value}, true, false, true, true);
    return;
  }
  QList<double> next = handles_;
  if (next.isEmpty()) {
    next = {value};
  } else {
    next[0] = value;
  }
  setHandlesInternal(next, true, false, true, true);
}

double AdMultiSlider::sliderPosition() const {
  const int index = sliderHandleIndex();
  if (index >= 0 && index < handles_.size()) {
    return handles_.at(index);
  }
  return handles_.isEmpty() ? minimum_ : handles_.constFirst();
}

void AdMultiSlider::setSliderPosition(double value) {
  if (mode_ == Mode::Single) {
    setHandlesInternal({value}, true, false, true, true);
    return;
  }

  QList<double> next = handles_;
  int index = sliderHandleIndex();
  if (index < 0) {
    index = 0;
  }
  if (next.isEmpty()) {
    next = {value};
  } else if (index >= next.size()) {
    next.append(value);
  } else {
    next[index] = value;
  }
  setHandlesInternal(next, true, false, true, true);
}

bool AdMultiSlider::sliderDown() const { return sliderDown_; }

void AdMultiSlider::setSliderDown(bool value) { setSliderDownInternal(value); }

QList<double> AdMultiSlider::handleValues() const { return committedHandles_; }

void AdMultiSlider::setHandleValues(const QList<double>& values) {
  setHandlesInternal(values, true, false, true, true);
}

int AdMultiSlider::activeHandleIndex() const {
  if (dragHandleIndex_ >= 0) {
    return dragHandleIndex_;
  }
  if (hoverHandleIndex_ >= 0) {
    return hoverHandleIndex_;
  }
  return focusHandleIndex_;
}

int AdMultiSlider::currentHandle() const {
  if (focusHandleIndex_ >= 0) {
    return focusHandleIndex_;
  }
  return activeHandleIndex();
}

void AdMultiSlider::setCurrentHandle(int index) { setFocusHandleIndex(index); }

bool AdMultiSlider::selectionDragEnabled() const { return draggableTrack_; }

void AdMultiSlider::setSelectionDragEnabled(bool value) {
  if (draggableTrack_ == value) {
    return;
  }
  draggableTrack_ = value;
  if (draggableTrack_ && editableHandles_) {
    editableHandles_ = false;
    emit handleEditingEnabledChanged(editableHandles_);
  }
  emit selectionDragEnabledChanged(draggableTrack_);
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

bool AdMultiSlider::handleEditingEnabled() const { return editableHandles_; }

void AdMultiSlider::setHandleEditingEnabled(bool value) {
  if (editableHandles_ == value) {
    return;
  }
  editableHandles_ = value;
  if (editableHandles_ && draggableTrack_) {
    draggableTrack_ = false;
    emit selectionDragEnabledChanged(draggableTrack_);
  }
  setHandlesInternal(handles_, true, false);
  emit handleEditingEnabledChanged(editableHandles_);
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

int AdMultiSlider::minimumHandleCount() const { return minHandleCount_; }

void AdMultiSlider::setMinimumHandleCount(int value) {
  const int normalized = std::max(0, value);
  if (minHandleCount_ == normalized) {
    return;
  }
  minHandleCount_ = normalized;
  setHandlesInternal(handles_, true, false);
  emit minimumHandleCountChanged(minHandleCount_);
  update();
}

int AdMultiSlider::maximumHandleCount() const { return maxHandleCount_; }

void AdMultiSlider::setMaximumHandleCount(int value) {
  int normalized = value;
  if (normalized == 0) {
    normalized = -1;
  }
  if (normalized < -1) {
    normalized = -1;
  }
  if (maxHandleCount_ == normalized) {
    return;
  }
  maxHandleCount_ = normalized;
  setHandlesInternal(handles_, true, false);
  emit maximumHandleCountChanged(maxHandleCount_);
  update();
}

bool AdMultiSlider::tooltipEnabled() const { return tooltipEnabled_; }

void AdMultiSlider::setTooltipEnabled(bool value) {
  if (tooltipEnabled_ == value) {
    return;
  }
  tooltipEnabled_ = value;
  emit tooltipEnabledChanged(tooltipEnabled_);
  requestTooltipSync();
  update();
}

AdMultiSlider::TooltipVisibleMode AdMultiSlider::tooltipVisibleMode() const {
  return tooltipVisibleMode_;
}

void AdMultiSlider::setTooltipVisibleMode(TooltipVisibleMode value) {
  if (tooltipVisibleMode_ == value) {
    return;
  }
  tooltipVisibleMode_ = value;
  emit tooltipVisibleModeChanged(tooltipVisibleMode_);
  requestTooltipSync();
  update();
}

AdMultiSlider::TooltipPlacement AdMultiSlider::tooltipPlacement() const {
  return tooltipPlacement_;
}

void AdMultiSlider::setTooltipPlacement(TooltipPlacement value) {
  if (tooltipPlacement_ == value) {
    return;
  }
  tooltipPlacement_ = value;
  emit tooltipPlacementChanged(tooltipPlacement_);
  requestTooltipSync();
  update();
}

AdMultiSlider::MarkMap AdMultiSlider::marks() const { return marks_; }

void AdMultiSlider::setMarks(const MarkMap& marks) {
  if (marksEqual(marks_, marks)) {
    return;
  }
  marks_ = marks;
  setHandlesInternal(handles_, true, false);
  emit marksChanged();
  refreshAfterPropertyChange();
}

void AdMultiSlider::clearMarks() {
  if (marks_.isEmpty()) {
    return;
  }
  marks_.clear();
  setHandlesInternal(handles_, true, false);
  emit marksChanged();
  refreshAfterPropertyChange();
}

AdMultiSlider::TooltipFormatter AdMultiSlider::tooltipFormatter() const {
  return tooltipFormatter_;
}

void AdMultiSlider::setTooltipFormatter(TooltipFormatter formatter) {
  tooltipFormatter_ = std::move(formatter);
  requestTooltipSync();
  update();
}

void AdMultiSlider::setRange(double minimum, double maximum) {
  const double normalizedMinimum = minimum;
  const double normalizedMaximum = std::max(minimum, maximum);
  if (fuzzyEq(minimum_, normalizedMinimum) && fuzzyEq(maximum_, normalizedMaximum)) {
    return;
  }

  const bool minimumChangedState = !fuzzyEq(minimum_, normalizedMinimum);
  const bool maximumChangedState = !fuzzyEq(maximum_, normalizedMaximum);
  minimum_ = normalizedMinimum;
  maximum_ = normalizedMaximum;
  setHandlesInternal(handles_, true, false);
  if (minimumChangedState) {
    emit minimumChanged(minimum_);
  }
  if (maximumChangedState) {
    emit maximumChanged(maximum_);
  }
  emit rangeChanged(minimum_, maximum_);
  refreshAfterPropertyChange();
}

void AdMultiSlider::triggerAction(QAbstractSlider::SliderAction action) {
  if (disabled() || handles_.isEmpty() || action == QAbstractSlider::SliderNoAction) {
    return;
  }

  const int index = sliderHandleIndex() >= 0 ? sliderHandleIndex() : 0;
  double nextValue = handles_.value(index, minimum_);
  bool handled = true;
  switch (action) {
    case QAbstractSlider::SliderSingleStepAdd:
      nextValue += effectiveSingleStep();
      break;
    case QAbstractSlider::SliderSingleStepSub:
      nextValue -= effectiveSingleStep();
      break;
    case QAbstractSlider::SliderPageStepAdd:
      nextValue += effectivePageStep();
      break;
    case QAbstractSlider::SliderPageStepSub:
      nextValue -= effectivePageStep();
      break;
    case QAbstractSlider::SliderToMinimum:
      nextValue = minimum_;
      break;
    case QAbstractSlider::SliderToMaximum:
      nextValue = maximum_;
      break;
    case QAbstractSlider::SliderMove:
      if (!tracking_) {
        commitPendingChanges();
      }
      return;
    default:
      handled = false;
      break;
  }

  if (!handled) {
    return;
  }

  QList<double> next = handles_;
  next[index] = normalizeValue(nextValue);
  emitHandleActionTriggered(action, index);
  setHandlesInternal(next, true, true, true);
  setFocusHandleIndex(index);
}

AdMultiSlider::ComponentTokens AdMultiSlider::componentTokens() const { return componentTokens_; }

void AdMultiSlider::setComponentTokens(const ComponentTokens& tokens) {
  componentTokens_ = tokens;
  emit componentTokensChanged();
  refreshAfterPropertyChange();
}

void AdMultiSlider::resetComponentTokens() {
  componentTokens_ = {};
  emit componentTokensChanged();
  refreshAfterPropertyChange();
}

AdMultiSlider::SemanticStyles AdMultiSlider::semanticStyles() const { return semanticStyles_; }

void AdMultiSlider::setSemanticStyles(const SemanticStyles& styles) {
  if (semanticStylesEqual(semanticStyles_, styles)) {
    return;
  }
  semanticStyles_ = styles;
  emit semanticStylesChanged();
  // Semantic slots only carry colors and brushes. Apply them over the cached
  // base style instead of resolving theme and geometry again.
  invalidateSemanticStyleCache();
  requestTooltipSync();
  update();
}

void AdMultiSlider::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  emit semanticStylesChanged();
  // A resolver has the same color/brush-only semantic surface as direct styles.
  invalidateSemanticStyleCache();
  requestTooltipSync();
  update();
}

QSize AdMultiSlider::sizeHint() const {
  const qreal scale = controlScale_.logicalScale;
  const LayoutInfo layout = buildLayout();
  const MarkMap marks = effectiveMarks();
  const bool hasMarks = !marks.isEmpty();
  const int markLabelHeight = hasMarks ? maxMarkLabelHeight(marks, layout.style.metrics.font) : 0;
  const int markLabelWidth = hasMarks ? maxMarkLabelWidth(marks, layout.style.metrics.font) : 0;
  QStyleOptionSlider option;
  initStyleOption(&option);
  const int sliderLength = style()->pixelMetric(QStyle::PM_SliderLength, &option, this);
  const int sliderThickness = style()->pixelMetric(QStyle::PM_SliderThickness, &option, this);
  if (orientation_ == Qt::Horizontal) {
    const int height =
        std::max(std::max(scaledControlMetric(34, scale),
                          scaledControlMetric(sliderThickness, scale)),
                 layout.style.metrics.controlSize + layout.style.metrics.marginCross * 2 +
                     (hasMarks ? layout.style.metrics.markGap + markLabelHeight : 0));
    return QSize(std::max(scaledControlMetric(260, scale),
                          scaledControlMetric(sliderLength * 6, scale)),
                 height);
  }
  const int width =
      std::max(std::max(scaledControlMetric(52, scale),
                        scaledControlMetric(sliderThickness, scale)),
               layout.style.metrics.controlSize + layout.style.metrics.marginCross * 2 +
                   (hasMarks ? layout.style.metrics.markGap + markLabelWidth : 0));
  return QSize(width, std::max(scaledControlMetric(260, scale),
                               scaledControlMetric(sliderLength * 6, scale)));
}

QSize AdMultiSlider::minimumSizeHint() const {
  const QSize hint = sizeHint();
  QStyleOptionSlider option;
  initStyleOption(&option);
  const int sliderLength = style()->pixelMetric(QStyle::PM_SliderLength, &option, this);
  const int scaledMinimumLength =
      qMax(1, qRound(std::max(120, sliderLength * 2) * controlScale_.logicalScale));
  if (orientation_ == Qt::Horizontal) {
    return QSize(scaledMinimumLength, hint.height());
  }
  return QSize(hint.width(), scaledMinimumLength);
}

void AdMultiSlider::prepareControlScale(const AdControlScaleContext& context) {
  Q_UNUSED(context)
  invalidateLayoutCache();
  invalidateSemanticStyleCache();
}

void AdMultiSlider::commitControlScale(const AdControlScaleContext& context) {
  if (!referenceFontCaptured_) {
    referenceFont_ = font();
    referenceFontCaptured_ = true;
  }
  controlScale_ = context;
  QFont scaledFont = referenceFont_;
  if (scaledFont.pixelSize() > 0) {
    scaledFont.setPixelSize(qMax(1, qRound(scaledFont.pixelSize() * context.logicalScale)));
  } else if (scaledFont.pointSizeF() > 0.0) {
    scaledFont.setPointSizeF(scaledFont.pointSizeF() * context.logicalScale);
  }
  setFont(scaledFont);
  invalidateLayoutCache();
  requestTooltipSync();
}

AdMultiSlider::MarkMap AdMultiSlider::effectiveMarks() const {
  MarkMap out = marks_;
  for (auto it = out.begin(); it != out.end(); ++it) {
    if (it->label.trimmed().isEmpty()) {
      it->label = formatNumber(it.key());
    }
  }
  return out;
}

AdMultiSlider::SemanticStyles AdMultiSlider::resolvedSemanticStyles() const {
  SemanticStyles merged = semanticStyles_;
  if (!semanticStyleResolver_) {
    return merged;
  }

  StyleContext ctx;
  ctx.mode = mode_;
  ctx.orientation = orientation_;
  ctx.reverse = invertedAppearance_;
  ctx.disabled = disabled();
  ctx.dragging = sliderDown_;
  ctx.hovered = hovered_;
  ctx.focused = hasFocus() && focusVisible_;
  ctx.values = handles_;
  const SemanticStyles resolved = semanticStyleResolver_(ctx);

  auto mergeSlot = [](SemanticSlotStyle* target, const SemanticSlotStyle& source) {
    if (source.textColor.has_value()) {
      target->textColor = source.textColor;
    }
    if (source.backgroundColor.has_value()) {
      target->backgroundColor = source.backgroundColor;
    }
    if (source.borderColor.has_value()) {
      target->borderColor = source.borderColor;
    }
    if (source.brush.has_value()) {
      target->brush = source.brush;
    }
  };

  mergeSlot(&merged.root, resolved.root);
  mergeSlot(&merged.rail, resolved.rail);
  mergeSlot(&merged.track, resolved.track);
  mergeSlot(&merged.tracks, resolved.tracks);
  mergeSlot(&merged.handle, resolved.handle);
  mergeSlot(&merged.mark, resolved.mark);
  mergeSlot(&merged.markActive, resolved.markActive);
  return merged;
}

void AdMultiSlider::setInversionState(bool invertedAppearance, bool invertedControls) {
  const bool normalizedAppearance = invertedAppearance;
  const bool normalizedControls = invertedControls;
  const bool appearanceChanged = invertedAppearance_ != normalizedAppearance;
  const bool controlsChanged = invertedControls_ != normalizedControls;
  if (!appearanceChanged && !controlsChanged) {
    return;
  }

  invertedAppearance_ = normalizedAppearance;
  invertedControls_ = normalizedControls;

  if (appearanceChanged) {
    emit invertedAppearanceChanged(invertedAppearance_);
  }
  if (controlsChanged) {
    emit invertedControlsChanged(invertedControls_);
  }

  refreshAfterPropertyChange(false);
}

int AdMultiSlider::sliderHandleIndex() const {
  if (focusHandleIndex_ >= 0 && focusHandleIndex_ < handles_.size()) {
    return focusHandleIndex_;
  }
  const int activeIndex = activeHandleIndex();
  if (activeIndex >= 0 && activeIndex < handles_.size()) {
    return activeIndex;
  }
  return handles_.isEmpty() ? -1 : 0;
}

void AdMultiSlider::setSliderDownInternal(bool value) {
  if (sliderDown_ == value) {
    return;
  }
  sliderDown_ = value;
  emit sliderDownChanged(sliderDown_);
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

void AdMultiSlider::setPendingChangeStateFromVisuals() {
  pendingValuesChange_ = !listFuzzyEquals(committedHandles_, handles_);
  const double committedPrimary =
      committedHandles_.isEmpty() ? minimum_ : committedHandles_.constFirst();
  const double visualPrimary = handles_.isEmpty() ? minimum_ : handles_.constFirst();
  pendingPrimaryValueChange_ = !fuzzyEq(committedPrimary, visualPrimary);
}

void AdMultiSlider::commitPendingChanges() {
  if (!pendingPrimaryValueChange_ && !pendingValuesChange_) {
    return;
  }

  committedHandles_ = handles_;
  emitChangedSignalsForCurrentMode();
  notifyAccessibleValueChange();
  notifyAccessibleDescriptionChange();
}

void AdMultiSlider::emitActiveHandleIndexChangedIfNeeded(int previousIndex) {
  const int currentIndex = activeHandleIndex();
  if (previousIndex != currentIndex) {
    emit activeHandleIndexChanged(currentIndex);
    notifyAccessibleValueChange();
    notifyAccessibleDescriptionChange();
    if (hasFocus()) {
      notifyAccessibleFocusChange();
    }
  }
}

void AdMultiSlider::emitSliderPositionChangedIfNeeded(double previousPosition) {
  const double currentPosition = sliderPosition();
  if (!fuzzyEq(previousPosition, currentPosition)) {
    emit sliderPositionChanged(currentPosition);
  }
}

void AdMultiSlider::setFocusHandleIndex(int index) {
  const int previousActiveIndex = activeHandleIndex();
  const int previousCurrentIndex = currentHandle();
  const double previousSliderPosition = sliderPosition();
  int nextIndex = index;
  if (handles_.isEmpty()) {
    nextIndex = -1;
  } else {
    nextIndex = std::clamp(index, 0, static_cast<int>(handles_.size()) - 1);
  }

  if (focusHandleIndex_ == nextIndex) {
    return;
  }
  focusHandleIndex_ = nextIndex;
  focusHandleId_ = nextIndex >= 0 && nextIndex < handleIds_.size() ? handleIds_.at(nextIndex) : -1;
  invalidateLayoutCache();
  requestTooltipSync();
  emitActiveHandleIndexChangedIfNeeded(previousActiveIndex);
  if (previousCurrentIndex != currentHandle()) {
    emit currentHandleChanged(currentHandle());
  }
  emitSliderPositionChangedIfNeeded(previousSliderPosition);
}

void AdMultiSlider::setHoverHandleIndex(int index) {
  const int previousActiveIndex = activeHandleIndex();
  int nextIndex = index;
  if (handles_.isEmpty()) {
    nextIndex = -1;
  } else if (nextIndex >= 0) {
    nextIndex = std::clamp(index, 0, static_cast<int>(handles_.size()) - 1);
  }

  if (hoverHandleIndex_ == nextIndex) {
    return;
  }

  hoverHandleIndex_ = nextIndex;
  hoverHandleId_ = nextIndex >= 0 && nextIndex < handleIds_.size() ? handleIds_.at(nextIndex) : -1;
  invalidateLayoutCache();
  requestTooltipSync();
  emitActiveHandleIndexChangedIfNeeded(previousActiveIndex);
}

void AdMultiSlider::setDragHandleIndex(int index) {
  const int previousActiveIndex = activeHandleIndex();
  int nextIndex = index;
  if (handles_.isEmpty()) {
    nextIndex = -1;
  } else if (nextIndex >= 0) {
    nextIndex = std::clamp(index, 0, static_cast<int>(handles_.size()) - 1);
  }

  if (dragHandleIndex_ == nextIndex) {
    return;
  }

  dragHandleIndex_ = nextIndex;
  dragHandleId_ = nextIndex >= 0 && nextIndex < handleIds_.size() ? handleIds_.at(nextIndex) : -1;
  invalidateLayoutCache();
  requestTooltipSync();
  emitActiveHandleIndexChangedIfNeeded(previousActiveIndex);
}

void AdMultiSlider::emitHandleActionTriggered(QAbstractSlider::SliderAction action, int index) {
  if (action == QAbstractSlider::SliderNoAction) {
    return;
  }
  emit handleActionTriggered(action, index);
}

int AdMultiSlider::indexForHandleId(qint64 id) const {
  if (id < 0) {
    return -1;
  }
  for (int i = 0; i < handleIds_.size(); ++i) {
    if (handleIds_.at(i) == id) {
      return i;
    }
  }
  return -1;
}

void AdMultiSlider::syncTrackedHandleIndexes() {
  if (handles_.isEmpty()) {
    focusHandleId_ = -1;
    hoverHandleId_ = -1;
    dragHandleId_ = -1;
  }

  focusHandleIndex_ = indexForHandleId(focusHandleId_);
  hoverHandleIndex_ = indexForHandleId(hoverHandleId_);
  dragHandleIndex_ = indexForHandleId(dragHandleId_);

  if (focusHandleIndex_ < 0 && !handles_.isEmpty()) {
    focusHandleIndex_ = 0;
    focusHandleId_ = handleIds_.value(0, -1);
  }
}

QList<double> AdMultiSlider::normalizedValues(const QList<double>& values,
                                              bool forceRangeMode) const {
  const bool rangeMode = forceRangeMode || mode_ == Mode::Range;
  QList<double> normalized = values;

  if (!rangeMode) {
    if (normalized.isEmpty()) {
      normalized = {minimum_};
    } else {
      normalized = {normalized.constFirst()};
    }
    normalized[0] = normalizeValue(normalized.constFirst());
    return normalized;
  }

  if (normalized.isEmpty()) {
    normalized = {minimum_, minimum_};
  }

  const int requiredMinCount = editableHandles_ ? std::max(1, minHandleCount_) : 2;
  if (normalized.size() < requiredMinCount) {
    const double fillValue = normalized.isEmpty() ? minimum_ : normalized.constLast();
    while (normalized.size() < requiredMinCount) {
      normalized.append(fillValue);
    }
  }

  if (maxHandleCount_ > 0 && normalized.size() > maxHandleCount_) {
    normalized = normalized.mid(0, maxHandleCount_);
  }

  for (double& value : normalized) {
    value = normalizeValue(value);
  }
  std::sort(normalized.begin(), normalized.end());
  return normalized;
}

QList<double> AdMultiSlider::snapPoints() const {
  QList<double> points;
  points.reserve(marks_.size() + 2);
  points.append(minimum_);
  points.append(maximum_);
  for (auto it = marks_.cbegin(); it != marks_.cend(); ++it) {
    points.append(clampValue(it.key(), minimum_, maximum_));
  }
  std::sort(points.begin(), points.end());
  points.erase(std::unique(points.begin(), points.end(),
                           [](double lhs, double rhs) { return std::abs(lhs - rhs) <= kEpsilon; }),
               points.end());
  return points;
}

double AdMultiSlider::normalizeValue(double value) const {
  if (maximum_ <= minimum_) {
    return minimum_;
  }

  double normalized = clampValue(value, minimum_, maximum_);

  if (marksOnly_) {
    const QList<double> points = snapPoints();
    if (!points.isEmpty()) {
      double nearest = points.constFirst();
      double bestDistance = std::abs(normalized - nearest);
      for (double point : points) {
        const double dist = std::abs(normalized - point);
        if (dist < bestDistance) {
          bestDistance = dist;
          nearest = point;
        }
      }
      normalized = nearest;
    }
  } else if (step_ > 0.0) {
    const double snapped = minimum_ + std::round((normalized - minimum_) / step_) * step_;
    normalized = clampValue(snapped, minimum_, maximum_);
  }

  if (markStepSnapEnabled_ && !marks_.isEmpty()) {
    const QList<double> points = snapPoints();
    if (!points.isEmpty()) {
      double nearest = points.constFirst();
      double bestDistance = std::abs(normalized - nearest);
      for (double point : points) {
        const double dist = std::abs(normalized - point);
        if (dist < bestDistance) {
          bestDistance = dist;
          nearest = point;
        }
      }
      normalized = nearest;
    }
  }

  normalized = std::round(normalized * 1000000.0) / 1000000.0;
  return clampValue(normalized, minimum_, maximum_);
}

void AdMultiSlider::setHandlesInternal(const QList<double>& handles, bool emitValueChangedSignal,
                                       bool fromUserAction, bool commitResult,
                                       bool valueOnlyLayoutChange) {
  const double previousCommittedValue = value();
  const QList<double> previousCommittedValues = committedHandles_;
  const QList<double> previousVisualValues = handles_;
  const int previousActiveIndex = activeHandleIndex();
  const int previousCurrentIndex = currentHandle();
  const double previousSliderPosition = sliderPosition();
  QList<HandleEntry> normalized;
  normalized.reserve(handles.size() > 2 ? static_cast<int>(handles.size()) : 2);
  for (int i = 0; i < handles.size(); ++i) {
    const qint64 id = i < handleIds_.size() ? handleIds_.at(i) : nextHandleId_++;
    normalized.append(HandleEntry{handles.at(i), id, i});
  }

  const bool rangeMode = mode_ == Mode::Range;
  if (!rangeMode) {
    if (normalized.isEmpty()) {
      normalized = {HandleEntry{minimum_, nextHandleId_++, 0}};
    } else {
      normalized = {normalized.constFirst()};
    }
    normalized[0].value = normalizeValue(normalized.constFirst().value);
  } else {
    if (normalized.isEmpty()) {
      normalized = {HandleEntry{minimum_, nextHandleId_++, 0},
                    HandleEntry{minimum_, nextHandleId_++, 1}};
    }

    const int requiredMinCount = editableHandles_ ? std::max(1, minHandleCount_) : 2;
    if (normalized.size() < requiredMinCount) {
      const double fillValue = normalized.isEmpty() ? minimum_ : normalized.constLast().value;
      while (normalized.size() < requiredMinCount) {
        normalized.append(
            HandleEntry{fillValue, nextHandleId_++, static_cast<int>(normalized.size())});
      }
    }

    if (maxHandleCount_ > 0 && normalized.size() > maxHandleCount_) {
      normalized = normalized.mid(0, maxHandleCount_);
    }

    for (HandleEntry& entry : normalized) {
      entry.value = normalizeValue(entry.value);
    }
    std::stable_sort(normalized.begin(), normalized.end(),
                     [](const HandleEntry& lhs, const HandleEntry& rhs) {
                       if (!fuzzyEq(lhs.value, rhs.value)) {
                         return lhs.value < rhs.value;
                       }
                       return lhs.sourceIndex < rhs.sourceIndex;
                     });
  }

  QList<double> normalizedValuesList;
  QList<qint64> normalizedIds;
  normalizedValuesList.reserve(normalized.size());
  normalizedIds.reserve(normalized.size());
  for (const HandleEntry& entry : normalized) {
    normalizedValuesList.append(entry.value);
    normalizedIds.append(entry.id);
  }

  const bool visualValuesChanged = !listFuzzyEquals(handles_, normalizedValuesList);
  const QList<double> nextCommittedValues = commitResult ? normalizedValuesList : committedHandles_;
  const bool committedValuesChanged = !listFuzzyEquals(committedHandles_, nextCommittedValues);
  if (!visualValuesChanged && !committedValuesChanged) {
    return;
  }

  if (visualValuesChanged) {
    handles_ = normalizedValuesList;
    handleIds_ = normalizedIds;
    syncTrackedHandleIndexes();
    emitActiveHandleIndexChangedIfNeeded(previousActiveIndex);
    if (previousCurrentIndex != currentHandle()) {
      emit currentHandleChanged(currentHandle());
    }
  }

  if (commitResult) {
    committedHandles_ = normalizedValuesList;
  }

  emitSliderPositionChangedIfNeeded(previousSliderPosition);

  const bool primaryValueChanged =
      !fuzzyEq(previousCommittedValue,
               committedHandles_.isEmpty() ? minimum_ : committedHandles_.constFirst());
  if (emitValueChangedSignal && commitResult) {
    if (primaryValueChanged) {
      emit valueChanged(value());
    }
    if (committedValuesChanged) {
      emit handleValuesChanged(committedHandles_);
    }
  }

  if (fromUserAction && visualValuesChanged) {
    dragChanged_ = true;
    const int movedIndex = sliderHandleIndex();
    if (movedIndex >= 0 && movedIndex < handles_.size()) {
      emit handleMoved(movedIndex, handles_.at(movedIndex));
    }
  }

  if (!commitResult) {
    setPendingChangeStateFromVisuals();
  } else {
    pendingPrimaryValueChange_ = false;
    pendingValuesChange_ = false;
  }

  if (primaryValueChanged || committedValuesChanged) {
    notifyAccessibleValueChange();
    notifyAccessibleDescriptionChange();
  }
  syncAccessibleState();
  if (valueOnlyLayoutChange) {
    invalidateHandleLayoutCache();
    if (semanticStyleResolver_) {
      invalidateSemanticStyleCache();
    }
  } else {
    invalidateLayoutCache();
  }
  requestTooltipSync();
  update();
}

void AdMultiSlider::emitChangedSignalsForCurrentMode() {
  if (pendingPrimaryValueChange_) {
    emit valueChanged(value());
  }
  if (pendingValuesChange_) {
    emit handleValuesChanged(committedHandles_);
  }
}

void AdMultiSlider::emitCompletedSignalsForCurrentMode() {
  if (dragChanged_ || pendingPrimaryValueChange_ || pendingValuesChange_) {
    emit editingFinished();
  }
  pendingPrimaryValueChange_ = false;
  pendingValuesChange_ = false;
}

void AdMultiSlider::refreshAfterPropertyChange(bool updateGeometryHint) {
  if (updateGeometryHint) {
    updateGeometry();
  }
  syncAccessibleState();
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

void AdMultiSlider::syncAccessibleState() {
  detail::syncDerivedAccessibleDescription(this, sliderAccessibleDescription(this));
}

void AdMultiSlider::syncInteractionCursor() {
  const Qt::CursorShape shape = disabled() ? Qt::ForbiddenCursor : Qt::PointingHandCursor;
  if (cursor().shape() == shape) {
    return;
  }
  setCursor(shape);
}

double AdMultiSlider::effectiveSingleStep() const { return effectiveSingleStepForSlider(this); }

double AdMultiSlider::effectivePageStep() const { return effectivePageStepForSlider(this); }

void AdMultiSlider::notifyAccessibleValueChange() const {
  QAccessibleValueChangeEvent event(const_cast<AdMultiSlider*>(this), sliderAccessibleValue(this));
  QAccessible::updateAccessibility(&event);
}

void AdMultiSlider::notifyAccessibleDescriptionChange() const {
  detail::notifyAccessibilityEvent(const_cast<AdMultiSlider*>(this),
                                   QAccessible::DescriptionChanged);
}

void AdMultiSlider::notifyAccessibleFocusChange() const {
  detail::notifyAccessibilityEvent(const_cast<AdMultiSlider*>(this), QAccessible::Focus);
}

void AdMultiSlider::invalidateLayoutCache() {
  layoutCacheDirty_ = true;
  layoutStyleDirty_ = true;
  handleLayoutDirty_ = false;
  semanticStyleDirty_ = false;
}

void AdMultiSlider::invalidateHandleLayoutCache() {
  if (!layoutCacheDirty_ && !layoutStyleDirty_) {
    handleLayoutDirty_ = true;
  }
}

void AdMultiSlider::invalidateSemanticStyleCache() {
  if (!layoutCacheDirty_ && !layoutStyleDirty_) {
    semanticStyleDirty_ = true;
  }
}

void AdMultiSlider::refreshHandleRects(LayoutInfo* layout) const {
  if (!layout) {
    return;
  }

  layout->handleRects.clear();
  layout->handleAnchorRects.clear();
  layout->handleRects.reserve(handles_.size());
  layout->handleAnchorRects.reserve(handles_.size());

  const int normalHandleHalf = layout->style.metrics.handleSize / 2;
  for (int i = 0; i < handles_.size(); ++i) {
    const bool active = (i == dragHandleIndex_) ||
                        ((hasFocus() && focusVisible_) && i == focusHandleIndex_) ||
                        (!dragging_ && i == hoverHandleIndex_);
    const int handleSize = active ? layout->activeHandleSize : layout->style.metrics.handleSize;
    const int half = handleSize / 2;
    const int axisPos = handlePositionFromValue(handles_.at(i), *layout);
    if (!layout->vertical) {
      layout->handleAnchorRects.append(
          QRectF(axisPos - normalHandleHalf, layout->crossCenter - normalHandleHalf,
                 layout->style.metrics.handleSize, layout->style.metrics.handleSize));
      layout->handleRects.append(
          QRectF(axisPos - half, layout->crossCenter - half, handleSize, handleSize));
    } else {
      layout->handleAnchorRects.append(
          QRectF(layout->crossCenter - normalHandleHalf, axisPos - normalHandleHalf,
                 layout->style.metrics.handleSize, layout->style.metrics.handleSize));
      layout->handleRects.append(
          QRectF(layout->crossCenter - half, axisPos - half, handleSize, handleSize));
    }
  }
}

void AdMultiSlider::requestTooltipSync() {
  if (!tooltipEnabled_ || handles_.isEmpty()) {
    clearTooltipHosts();
  } else if (tooltipHosts_.size() != handles_.size()) {
    ensureTooltipHosts(static_cast<int>(handles_.size()));
  }

  if (tooltipSyncPending_) {
    return;
  }
  tooltipSyncPending_ = true;
  QMetaObject::invokeMethod(
      this,
      [this]() {
        tooltipSyncPending_ = false;
        syncTooltipHosts();
      },
      Qt::QueuedConnection);
}

void AdMultiSlider::initStyleOption(QStyleOptionSlider* option, int handleIndex) const {
  if (!option) {
    return;
  }

  option->initFrom(this);
  option->orientation = orientation_;
  option->direction = layoutDirection();
  option->minimum = 0;
  option->maximum = 1000000;
  const double range = std::max(kEpsilon, maximum_ - minimum_);
  const auto toSliderUnits = [range, this](double value) {
    const double ratio = (clampValue(value, minimum_, maximum_) - minimum_) / range;
    return std::clamp(qRound(ratio * 1000000.0), 0, 1000000);
  };

  const int resolvedIndex =
      handleIndex >= 0 && handleIndex < handles_.size() ? handleIndex : sliderHandleIndex();
  const double sliderPositionValue =
      resolvedIndex >= 0 && resolvedIndex < handles_.size() ? handles_.at(resolvedIndex) : minimum_;
  const double sliderValue = resolvedIndex >= 0 && resolvedIndex < committedHandles_.size()
                                 ? committedHandles_.at(resolvedIndex)
                                 : minimum_;
  option->sliderPosition = toSliderUnits(sliderPositionValue);
  option->sliderValue = toSliderUnits(sliderValue);
  option->singleStep = std::max(1, qRound((effectiveSingleStep() / range) * option->maximum));
  option->pageStep = std::max(1, qRound((effectivePageStep() / range) * option->maximum));
  option->upsideDown = invertedAppearance_ ^
                       (orientation_ == Qt::Horizontal && layoutDirection() == Qt::RightToLeft);
  option->state.setFlag(QStyle::State_Sunken, sliderDown_);
  option->state.setFlag(QStyle::State_MouseOver, hovered_);
  option->state.setFlag(QStyle::State_HasFocus, hasFocus() && focusVisible_);
  option->subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderHandle;
  option->activeSubControls = sliderDown_ ? QStyle::SC_SliderHandle : QStyle::SC_None;
  option->tickPosition = QSlider::NoTicks;
}

AdMultiSlider::LayoutInfo AdMultiSlider::buildLayout() const {
  if (!layoutCacheDirty_ && !layoutStyleDirty_ && layoutCache_ && layoutCacheSize_ == size()) {
    if (handleLayoutDirty_ || semanticStyleDirty_) {
      LayoutInfo& layout = *layoutCache_;
      if (semanticStyleDirty_) {
        layout.style = detail::applySliderSemanticStyles(layout.baseStyle, resolvedSemanticStyles(),
                                                         disabled());
      }
      if (handleLayoutDirty_) {
        refreshHandleRects(&layout);
      }
      handleLayoutDirty_ = false;
      semanticStyleDirty_ = false;
      return layout;
    }
    return *layoutCache_;
  }

  auto resolveBaseStyle = [this]() {
    detail::SliderStyleInput input;
    input.mode = mode_;
    input.orientation = orientation_;
    input.hovered = hovered_;
    input.dragging = sliderDown_;
    input.focused = hasFocus() && focusVisible_;
    input.disabled = disabled();
    input.reverse = invertedAppearance_;
    input.baseFont = font();
    input.componentTokens = componentTokens_;
    const adqt::theme::ResolvedTheme resolvedTheme =
        adqt::theme::ThemeManager::instance().resolve(this);
    detail::SliderVisualStyle style = detail::resolveSliderVisualStyle(input, resolvedTheme);
    scaleSliderControlMetrics(&style.metrics, controlScale_.logicalScale);
    return style;
  };

  auto applySemanticStyles = [this](const detail::SliderVisualStyle& baseStyle) {
    return detail::applySliderSemanticStyles(baseStyle, resolvedSemanticStyles(), disabled());
  };

  if (!layoutCacheDirty_ && layoutStyleDirty_ && layoutCache_ && layoutCacheSize_ == size()) {
    LayoutInfo& layout = *layoutCache_;
    layout.baseStyle = resolveBaseStyle();
    layout.style = applySemanticStyles(layout.baseStyle);
    layoutStyleDirty_ = false;
    semanticStyleDirty_ = false;
    return layout;
  }

  LayoutInfo layout;
  layout.baseStyle = resolveBaseStyle();
  layout.style = applySemanticStyles(layout.baseStyle);

  const MarkMap marks = effectiveMarks();
  const bool hasMarks = !marks.isEmpty();
  const int maxMarkHeight = hasMarks ? maxMarkLabelHeight(marks, layout.style.metrics.font) : 0;
  const int maxMarkWidth = hasMarks ? maxMarkLabelWidth(marks, layout.style.metrics.font) : 0;

  layout.vertical = orientation_ == Qt::Vertical;
  layout.axis.orientation = orientation_;
  layout.axis.minimumAtVisualStart =
      layout.vertical ? invertedAppearance_
                      : !((layoutDirection() == Qt::RightToLeft) ^ invertedAppearance_);
  const qreal minimumThumbRadius =
      std::min(layout.style.metrics.handleSize, layout.style.metrics.handleSizeHover) / 2.0;
  const int markSpan =
      hasMarks ? layout.style.metrics.markGap + (layout.vertical ? maxMarkWidth : maxMarkHeight)
               : 0;
  // Keep the handle on the clipping-safe axis. The minimum thumb diameter is
  // added back only as a visual rail extension around that logical span.
  if (!layout.vertical) {
    const qreal mainExtent = std::max<qreal>(1.0, width());
    const qreal maximumInset = std::max<qreal>(0.0, (mainExtent - 1.0) / 2.0);
    const qreal handleInset = std::min<qreal>(layout.style.metrics.marginMain, maximumInset);
    const qreal top = layout.style.metrics.marginCross;
    const qreal bottom = height() - layout.style.metrics.marginCross - markSpan;
    layout.contentRect = QRectF(0.0, top, mainExtent, std::max<qreal>(1.0, bottom - top));
    layout.crossCenter = layout.contentRect.center().y();
    layout.axis.setMainExtents(mainExtent, handleInset, minimumThumbRadius);
    layout.railRect =
        QRectF(layout.axis.visualStart, layout.crossCenter - layout.style.metrics.railSize / 2.0,
               layout.axis.visualEnd - layout.axis.visualStart, layout.style.metrics.railSize);
  } else {
    const qreal left = layout.style.metrics.marginCross;
    const qreal right = width() - layout.style.metrics.marginCross - markSpan;
    const qreal mainExtent = std::max<qreal>(1.0, height());
    const qreal maximumInset = std::max<qreal>(0.0, (mainExtent - 1.0) / 2.0);
    const qreal handleInset = std::min<qreal>(layout.style.metrics.marginMain, maximumInset);
    layout.contentRect = QRectF(left, 0.0, std::max<qreal>(1.0, right - left), mainExtent);
    layout.crossCenter = layout.contentRect.center().x();
    layout.axis.setMainExtents(mainExtent, handleInset, minimumThumbRadius);
    layout.railRect =
        QRectF(layout.crossCenter - layout.style.metrics.railSize / 2.0, layout.axis.visualStart,
               layout.style.metrics.railSize, layout.axis.visualEnd - layout.axis.visualStart);
  }

  const int activeHandleSize =
      disabled() ? layout.style.metrics.handleSize : layout.style.metrics.handleSizeHover;
  layout.activeHandleSize = activeHandleSize;
  layout.normalHandleSize = layout.style.metrics.handleSize;
  layout.markLabelOffset = layout.style.metrics.markGap;

  refreshHandleRects(&layout);

  layout.markCenters.reserve(marks.size());
  layout.markValues.reserve(marks.size());
  for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
    const double markValue = clampValue(it.key(), minimum_, maximum_);
    const int axisPos = handlePositionFromValue(markValue, layout);
    if (!layout.vertical) {
      layout.markCenters.append(QPointF(axisPos, layout.crossCenter));
    } else {
      layout.markCenters.append(QPointF(layout.crossCenter, axisPos));
    }
    layout.markValues.append(markValue);
  }

  layoutCache_ = std::make_unique<LayoutInfo>(layout);
  layoutCacheSize_ = size();
  layoutCacheDirty_ = false;
  layoutStyleDirty_ = false;
  handleLayoutDirty_ = false;
  semanticStyleDirty_ = false;
  return layout;
}

int AdMultiSlider::hitTestHandle(const QPoint& pos, const LayoutInfo& layout) const {
  for (int i = static_cast<int>(layout.handleRects.size()) - 1; i >= 0; --i) {
    const bool active = (i == dragHandleIndex_) ||
                        ((hasFocus() && focusVisible_) && i == focusHandleIndex_) ||
                        (!dragging_ && i == hoverHandleIndex_);
    const qreal borderWidth =
        std::max<qreal>(1.0, active ? layout.style.metrics.handleLineWidthHover
                                    : layout.style.metrics.handleLineWidth);
    const QRectF hitRect =
        layout.handleRects.at(i).adjusted(-borderWidth, -borderWidth, borderWidth, borderWidth);
    if (hitRect.contains(QPointF(pos))) {
      return i;
    }
  }
  return -1;
}

int AdMultiSlider::nearestHandleIndex(double value) const {
  if (handles_.isEmpty()) {
    return -1;
  }
  int bestIndex = 0;
  double bestDistance = std::abs(handles_.constFirst() - value);
  for (int i = 1; i < handles_.size(); ++i) {
    const double distance = std::abs(handles_.at(i) - value);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = i;
    }
  }
  return bestIndex;
}

double AdMultiSlider::valueFromHandlePosition(const QPoint& pos, const LayoutInfo& layout) const {
  if (maximum_ <= minimum_) {
    return minimum_;
  }

  constexpr int kSliderUnits = 1000000;
  const int sliderUnits =
      layout.axis.sliderUnitsFromHandlePosition(layout.axis.coordinate(pos), kSliderUnits);
  const double raw =
      minimum_ + (static_cast<double>(sliderUnits) / kSliderUnits) * (maximum_ - minimum_);
  return normalizeValue(raw);
}

int AdMultiSlider::handlePositionFromValue(double value, const LayoutInfo& layout) const {
  if (maximum_ <= minimum_) {
    return qRound(layout.axis.handleMinimumPosition());
  }

  constexpr int kSliderUnits = 1000000;
  const double ratio = std::clamp((value - minimum_) / (maximum_ - minimum_), 0.0, 1.0);
  const int sliderUnits = std::clamp(qRound(ratio * kSliderUnits), 0, kSliderUnits);
  return layout.axis.handlePositionFromSliderUnits(sliderUnits, kSliderUnits);
}

double AdMultiSlider::clampTrackDelta(double delta) const {
  if (dragStartValues_.isEmpty()) {
    return 0.0;
  }
  double minValue = std::numeric_limits<double>::max();
  double maxValue = std::numeric_limits<double>::lowest();
  for (double value : dragStartValues_) {
    minValue = std::min(minValue, value);
    maxValue = std::max(maxValue, value);
  }

  const double minDelta = minimum_ - minValue;
  const double maxDelta = maximum_ - maxValue;
  return std::clamp(delta, minDelta, maxDelta);
}

bool AdMultiSlider::isMarkActive(double markValue) const {
  if (!included_ || handles_.isEmpty()) {
    return false;
  }

  if (mode_ == Mode::Single) {
    return markValue <= handles_.constFirst() + kEpsilon;
  }

  const double low = handles_.constFirst();
  const double high = handles_.constLast();
  return markValue >= low - kEpsilon && markValue <= high + kEpsilon;
}

int AdMultiSlider::handleRailAction(const QPoint& pos, const LayoutInfo& layout,
                                    bool emitLiveSignals) {
  const double nextValue = valueFromHandlePosition(pos, layout);
  if (mode_ == Mode::Single) {
    setHandlesInternal({nextValue}, emitLiveSignals, true, emitLiveSignals);
    setFocusHandleIndex(0);
    return handles_.isEmpty() ? -1 : 0;
  }

  if (editableHandles_) {
    int insertedIndex = -1;
    if (addHandleAt(nextValue, &insertedIndex, emitLiveSignals)) {
      setFocusHandleIndex(insertedIndex);
      return insertedIndex;
    }
  }

  const int index = nearestHandleIndex(nextValue);
  if (index < 0) {
    return -1;
  }

  QList<double> nextValues = handles_;
  const qint64 targetHandleId = index < handleIds_.size() ? handleIds_.at(index) : -1;
  nextValues[index] = nextValue;
  setHandlesInternal(nextValues, emitLiveSignals, true, emitLiveSignals);
  const int activeIndex =
      targetHandleId >= 0 ? indexForHandleId(targetHandleId) : nearestHandleIndex(nextValue);
  setFocusHandleIndex(activeIndex >= 0 ? activeIndex : index);
  return activeHandleIndex();
}

bool AdMultiSlider::deleteHandleAt(int index, bool emitLiveSignals) {
  if (mode_ != Mode::Range || index < 0 || index >= handles_.size()) {
    return false;
  }
  const int requiredMinCount = editableHandles_ ? std::max(1, minHandleCount_) : 2;
  if (handles_.size() <= requiredMinCount) {
    return false;
  }

  QList<double> next = handles_;
  next.removeAt(index);
  const QList<double> before = handles_;
  setHandlesInternal(next, emitLiveSignals, true, emitLiveSignals);
  if (listFuzzyEquals(before, handles_)) {
    return false;
  }

  const int maxIndex = static_cast<int>(handles_.size()) - 1;
  setFocusHandleIndex(std::clamp(index, 0, maxIndex));
  return true;
}

bool AdMultiSlider::addHandleAt(double value, int* insertedIndex, bool emitLiveSignals) {
  if (mode_ != Mode::Range) {
    return false;
  }
  if (maxHandleCount_ > 0 && handles_.size() >= maxHandleCount_) {
    return false;
  }

  QList<double> next = handles_;
  next.append(normalizeValue(value));
  std::sort(next.begin(), next.end());

  const QList<double> before = handles_;
  setHandlesInternal(next, emitLiveSignals, true, emitLiveSignals);
  if (listFuzzyEquals(before, handles_)) {
    return false;
  }

  const int index = nearestHandleIndex(value);
  if (insertedIndex) {
    *insertedIndex = index;
  }
  return true;
}

QList<int> AdMultiSlider::tooltipHandleIndexes() const {
  QList<int> indexes;
  if (!tooltipEnabled_ || handles_.isEmpty()) {
    return indexes;
  }

  if (tooltipVisibleMode_ == TooltipVisibleMode::Never) {
    return indexes;
  }

  if (tooltipVisibleMode_ == TooltipVisibleMode::Always) {
    indexes.reserve(handles_.size());
    for (int i = 0; i < handles_.size(); ++i) {
      indexes.append(i);
    }
    return indexes;
  }

  const int primary =
      dragHandleIndex_ >= 0
          ? dragHandleIndex_
          : (hoverHandleIndex_ >= 0 ? hoverHandleIndex_
                                    : ((hasFocus() && focusVisible_) ? focusHandleIndex_ : -1));
  if (primary >= 0 && primary < handles_.size()) {
    indexes.append(primary);
  }
  return indexes;
}

QString AdMultiSlider::tooltipText(double value) const {
  if (tooltipFormatter_) {
    return tooltipFormatter_(value);
  }
  return formatNumber(value);
}

void AdMultiSlider::ensureTooltipHosts(int count) {
  const int targetCount = std::max(0, count);
  while (tooltipHosts_.size() > targetCount) {
    delete tooltipHosts_.takeLast();
  }

  while (tooltipHosts_.size() < targetCount) {
    tooltipHosts_.append(new TooltipHost(this));
  }
}

void AdMultiSlider::clearTooltipHosts() {
  while (!tooltipHosts_.isEmpty()) {
    delete tooltipHosts_.takeLast();
  }
}

void AdMultiSlider::syncTooltipHosts(const LayoutInfo* layout) {
  if (!tooltipEnabled_ || handles_.isEmpty()) {
    clearTooltipHosts();
    return;
  }

  ensureTooltipHosts(static_cast<int>(handles_.size()));
  if (tooltipHosts_.isEmpty()) {
    return;
  }

  const LayoutInfo resolvedLayout = layout ? *layout : buildLayout();
  const QList<int> visibleIndexes = tooltipHandleIndexes();
  const QSet<int> visibleIndexSet(visibleIndexes.cbegin(), visibleIndexes.cend());
  const bool allowTooltipOpen = isVisible() && tooltipEnabled_ && !handles_.isEmpty();
  const bool disabledState = disabled();
  const QFont tooltipFont = resolvedLayout.style.metrics.font;

  const AdTooltip::Placement placement = toTooltipComponentPlacement(tooltipPlacement_);
  for (int i = 0; i < tooltipHosts_.size(); ++i) {
    TooltipHost* host = tooltipHosts_.at(i);
    if (!host || !host->component()) {
      continue;
    }

    host->applyBaseState(disabledState, placement, tooltipFont, this);

    QRect anchorRect;
    if (i >= 0 && i < resolvedLayout.handleAnchorRects.size()) {
      anchorRect = resolvedLayout.handleAnchorRects.at(i).toAlignedRect();
    } else if (i >= 0 && i < resolvedLayout.handleRects.size()) {
      // Fallback keeps behavior intact if a layout path does not provide fixed anchors.
      anchorRect = resolvedLayout.handleRects.at(i).toAlignedRect();
    }
    host->setAnchorRect(anchorRect, rect(), this);

    const QString text = i < handles_.size() ? tooltipText(handles_.at(i)) : QString();
    const bool shouldOpen = allowTooltipOpen && visibleIndexSet.contains(i) && !text.isEmpty();
    host->setContentAndOpen(text, shouldOpen);
  }
}

void AdMultiSlider::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)

  const LayoutInfo layout = buildLayout();

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setFont(layout.style.metrics.font);

  QStyleOption option;
  option.initFrom(this);
  style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

  if (layout.style.rootBg.isValid() && layout.style.rootBg.alpha() > 0) {
    painter.fillRect(rect(), layout.style.rootBg);
  }

  const bool isDisabled = disabled();
  const QColor railColor =
      (!isDisabled && hovered_) ? layout.style.railHoverBg : layout.style.railBg;
  const QColor trackColor =
      isDisabled ? layout.style.trackBgDisabled
                 : ((hovered_ || sliderDown_) ? layout.style.trackHoverBg : layout.style.trackBg);

  painter.setPen(Qt::NoPen);
  painter.setBrush(layout.style.useRailBrush ? layout.style.railBrush : QBrush(railColor));
  painter.drawRoundedRect(layout.railRect, layout.style.metrics.railSize / 2.0,
                          layout.style.metrics.railSize / 2.0);

  auto trackEndpointPosition = [&](double value, TrackEndpoint endpoint) -> qreal {
    switch (endpoint) {
      case TrackEndpoint::VisualMinimum:
        return layout.axis.visualMinimumPosition();
      case TrackEndpoint::VisualMaximum:
        return layout.axis.visualMaximumPosition();
      case TrackEndpoint::HandleCenter:
        return handlePositionFromValue(value, layout);
    }
    return handlePositionFromValue(value, layout);
  };

  auto drawTrackSegment = [&](double fromValue, double toValue, TrackEndpoint fromEndpoint,
                              TrackEndpoint toEndpoint) {
    if (fuzzyEq(fromValue, toValue)) {
      return;
    }
    const qreal fromPos = trackEndpointPosition(fromValue, fromEndpoint);
    const qreal toPos = trackEndpointPosition(toValue, toEndpoint);
    if (!layout.vertical) {
      const qreal left = std::min(fromPos, toPos);
      const qreal width = std::max<qreal>(1.0, std::abs(toPos - fromPos));
      const QRectF segment(left, layout.crossCenter - layout.style.metrics.railSize / 2.0, width,
                           layout.style.metrics.railSize);
      painter.setBrush(layout.style.useTracksBrush ? layout.style.tracksBrush : QBrush(trackColor));
      painter.drawRoundedRect(segment, layout.style.metrics.railSize / 2.0,
                              layout.style.metrics.railSize / 2.0);
    } else {
      const qreal top = std::min(fromPos, toPos);
      const qreal height = std::max<qreal>(1.0, std::abs(toPos - fromPos));
      const QRectF segment(layout.crossCenter - layout.style.metrics.railSize / 2.0, top,
                           layout.style.metrics.railSize, height);
      painter.setBrush(layout.style.useTracksBrush ? layout.style.tracksBrush : QBrush(trackColor));
      painter.drawRoundedRect(segment, layout.style.metrics.railSize / 2.0,
                              layout.style.metrics.railSize / 2.0);
    }
  };

  if (mode_ == Mode::Single) {
    if (included_ && !handles_.isEmpty()) {
      const double handleValue = handles_.constFirst();
      const TrackEndpoint handleEndpoint = fuzzyEq(handleValue, maximum_)
                                               ? TrackEndpoint::VisualMaximum
                                               : TrackEndpoint::HandleCenter;
      drawTrackSegment(minimum_, handleValue, TrackEndpoint::VisualMinimum, handleEndpoint);
    } else if (!included_ && layout.style.useTracksBrush) {
      drawTrackSegment(minimum_, maximum_, TrackEndpoint::VisualMinimum,
                       TrackEndpoint::VisualMaximum);
    }
  } else if (handles_.size() >= 2) {
    for (int i = 0; i + 1 < handles_.size(); ++i) {
      const double fromValue = handles_.at(i);
      const double toValue = handles_.at(i + 1);
      const TrackEndpoint fromEndpoint = i == 0 && fuzzyEq(fromValue, minimum_)
                                             ? TrackEndpoint::VisualMinimum
                                             : TrackEndpoint::HandleCenter;
      const TrackEndpoint toEndpoint = i + 1 == handles_.size() - 1 && fuzzyEq(toValue, maximum_)
                                           ? TrackEndpoint::VisualMaximum
                                           : TrackEndpoint::HandleCenter;
      drawTrackSegment(fromValue, toValue, fromEndpoint, toEndpoint);
    }
  }

  const MarkMap marks = effectiveMarks();
  if (!marks.isEmpty()) {
    int markIndex = 0;
    for (auto it = marks.cbegin(); it != marks.cend(); ++it, ++markIndex) {
      if (markIndex < 0 || markIndex >= layout.markCenters.size() ||
          markIndex >= layout.markValues.size()) {
        continue;
      }

      const QPointF center = layout.markCenters.at(markIndex);
      const double markValue = layout.markValues.at(markIndex);
      const bool active = isMarkActive(markValue);

      QColor dotBorder = active ? layout.style.dotActiveBorderColor : layout.style.dotBorderColor;
      if (!active && hovered_ && !isDisabled) {
        dotBorder = layout.style.dotHoverBorderColor;
      }

      if (markIndicatorsVisible_) {
        const int dotSize = layout.style.metrics.dotSize;
        const QRectF dotRect(center.x() - dotSize / 2.0, center.y() - dotSize / 2.0, dotSize,
                             dotSize);
        painter.setBrush(layout.style.surfaceBg);
        painter.setPen(QPen(dotBorder, std::max<qreal>(1.0, layout.style.metrics.handleLineWidth)));
        painter.drawEllipse(dotRect);
      }

      QFont markFont = layout.style.metrics.font;
      if (it->font.has_value()) {
        markFont = it->font.value();
      }
      const QFontMetrics markMetrics(markFont);
      painter.setFont(markFont);
      QColor textColor = active ? layout.style.markActiveColor : layout.style.markColor;
      if (it->color.has_value()) {
        textColor = it->color.value();
      }
      painter.setPen(textColor);

      const QString label = it->label.isEmpty() ? formatNumber(it.key()) : it->label;
      if (!layout.vertical) {
        QString drawLabel = label;
        const int maxLabelWidth = std::max(1, width());
        if (markMetrics.horizontalAdvance(drawLabel) > maxLabelWidth) {
          drawLabel = markMetrics.elidedText(drawLabel, Qt::ElideRight, maxLabelWidth);
        }
        const int textWidth = markMetrics.horizontalAdvance(drawLabel);
        const qreal minX = 0.0;
        const qreal maxX = std::max<qreal>(0.0, width() - textWidth);
        const qreal textX = std::clamp(center.x() - textWidth / 2.0, minX, maxX);
        const QPointF textPos(textX, center.y() + layout.markLabelOffset + markMetrics.ascent());
        painter.drawText(textPos, drawLabel);
      } else {
        const qreal textX = center.x() + layout.markLabelOffset;
        const int maxLabelWidth = std::max(1, width() - static_cast<int>(std::ceil(textX)));
        const QString drawLabel = markMetrics.elidedText(label, Qt::ElideRight, maxLabelWidth);
        const QPointF textPos(textX, center.y() + markMetrics.ascent() / 2.0);
        painter.drawText(textPos, drawLabel);
      }
    }
    painter.setFont(layout.style.metrics.font);
  }

  for (int i = 0; i < layout.handleRects.size(); ++i) {
    const QRectF handleRect = layout.handleRects.at(i);
    const bool active = (i == dragHandleIndex_) ||
                        ((hasFocus() && focusVisible_) && i == focusHandleIndex_) ||
                        (!dragging_ && i == hoverHandleIndex_);
    const bool activeShadow =
        (i == dragHandleIndex_) || ((hasFocus() && focusVisible_) && i == focusHandleIndex_);
    const qreal borderWidth =
        active ? layout.style.metrics.handleLineWidthHover : layout.style.metrics.handleLineWidth;
    QColor borderColor = layout.style.handleColor;
    if (isDisabled) {
      borderColor = layout.style.handleColorDisabled;
    } else if (active) {
      borderColor = layout.style.handleActiveColor;
    } else if (hovered_) {
      borderColor = layout.style.handleHoverColor;
    }
    QColor outlineColor = isDisabled ? QColor(0, 0, 0, 0) : layout.style.handleActiveOutlineColor;
    QColor shadowColor = isDisabled ? QColor(0, 0, 0, 0)
                                    : (activeShadow ? layout.style.handleActiveShadowColor
                                                    : layout.style.handleShadowColor);
    const QPointF center = handleRect.center();
    const qreal coreRadius = std::max(handleRect.width(), handleRect.height()) / 2.0;
    const qreal ringOuterRadius = coreRadius + std::max<qreal>(0.0, borderWidth);

    if (active && outlineColor.isValid() && outlineColor.alpha() > 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(outlineColor);
      const qreal outlineRadius = coreRadius + layout.style.metrics.focusOutlineSize;
      const QRectF outlineRect(center.x() - outlineRadius, center.y() - outlineRadius,
                               outlineRadius * 2.0, outlineRadius * 2.0);
      painter.drawEllipse(outlineRect);
    }

    // Ant Design Slider uses box-shadow to draw the handle border, which expands outward.
    // Draw an explicit outer ring so the visual diameter matches handleSize + 2 * handleLineWidth.
    painter.setPen(Qt::NoPen);
    painter.setBrush(borderColor);
    painter.drawEllipse(QRectF(center.x() - ringOuterRadius, center.y() - ringOuterRadius,
                               ringOuterRadius * 2.0, ringOuterRadius * 2.0));

    if (shadowColor.isValid() && shadowColor.alpha() > 0) {
      // Align with Ant Design color-picker slider: a persistent 1px outer shadow ring that
      // turns primary when the handle is focused or dragged.
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(shadowColor, 1.0));
      painter.drawEllipse(QRectF(center.x() - (ringOuterRadius + 0.5),
                                 center.y() - (ringOuterRadius + 0.5),
                                 (ringOuterRadius + 0.5) * 2.0, (ringOuterRadius + 0.5) * 2.0));
    }

    // Reset pen after optional shadow ring so the core fill does not inherit an extra stroke.
    painter.setPen(Qt::NoPen);
    QBrush handleFill =
        layout.style.useHandleBrush ? layout.style.handleBrush : QBrush(layout.style.surfaceBg);
    const bool canSampleRailGradient =
        !layout.style.useHandleBrush && mode_ == Mode::Range && layout.style.useRailBrush &&
        layout.style.surfaceBg.alpha() == 0 && i >= 0 && i < handles_.size() && maximum_ > minimum_;
    if (canSampleRailGradient) {
      const double ratio =
          std::clamp((handles_.at(i) - minimum_) / (maximum_ - minimum_), 0.0, 1.0);
      handleFill = QBrush(sampleBrushColor(layout.style.railBrush, ratio, layout.style.surfaceBg));
    }
    painter.setBrush(handleFill);
    painter.drawEllipse(QRectF(center.x() - coreRadius, center.y() - coreRadius, coreRadius * 2.0,
                               coreRadius * 2.0));
  }
}

void AdMultiSlider::enterEvent(QEnterEvent* event) {
  hovered_ = true;
  invalidateLayoutCache();
  requestTooltipSync();
  update();
  QWidget::enterEvent(event);
}

void AdMultiSlider::leaveEvent(QEvent* event) {
  hovered_ = false;
  if (dragMode_ == DragMode::None) {
    setHoverHandleIndex(-1);
  }
  invalidateLayoutCache();
  requestTooltipSync();
  update();
  QWidget::leaveEvent(event);
}

void AdMultiSlider::mousePressEvent(QMouseEvent* event) {
  QWidget::mousePressEvent(event);
  if (!event || event->button() != Qt::LeftButton || disabled()) {
    return;
  }

  setFocus(Qt::MouseFocusReason);
  dragChanged_ = false;
  pendingPrimaryValueChange_ = false;
  pendingValuesChange_ = false;
  pressedHandleIndex_ = -1;

  const LayoutInfo layout = buildLayout();
  const int hitIndex = hitTestHandle(event->position().toPoint(), layout);
  if (hitIndex >= 0) {
    dragMode_ = DragMode::Handle;
    setDragHandleIndex(hitIndex);
    setHoverHandleIndex(hitIndex);
    setFocusHandleIndex(hitIndex);
    dragStartPos_ = event->position().toPoint();
    dragStartValues_ = handles_;
    dragging_ = true;
    pressedHandleIndex_ = hitIndex;
    setSliderDownInternal(true);
    emit handlePressed(hitIndex);
    invalidateLayoutCache();
    requestTooltipSync();
    update();
    return;
  }

  if (mode_ == Mode::Range && draggableTrack_ && handles_.size() >= 2) {
    const int p1 = handlePositionFromValue(handles_.constFirst(), layout);
    const int p2 = handlePositionFromValue(handles_.constLast(), layout);
    const int trackHalf = std::max(layout.style.metrics.railSize, layout.activeHandleSize / 2);
    QRectF trackRect;
    if (!layout.vertical) {
      trackRect = QRectF(std::min(p1, p2), layout.crossCenter - trackHalf, std::abs(p2 - p1),
                         trackHalf * 2.0);
    } else {
      trackRect = QRectF(layout.crossCenter - trackHalf, std::min(p1, p2), trackHalf * 2.0,
                         std::abs(p2 - p1));
    }
    if (trackRect.adjusted(-2, -2, 2, 2).contains(event->position())) {
      dragMode_ = DragMode::Track;
      setDragHandleIndex(-1);
      dragStartPos_ = event->position().toPoint();
      dragStartValues_ = handles_;
      dragging_ = true;
      pressedHandleIndex_ = sliderHandleIndex();
      setSliderDownInternal(true);
      emit handlePressed(pressedHandleIndex_);
      invalidateLayoutCache();
      requestTooltipSync();
      update();
      return;
    }
  }

  const int railHandleIndex = handleRailAction(event->position().toPoint(), layout, tracking_);
  if (railHandleIndex >= 0) {
    dragMode_ = DragMode::Handle;
    setDragHandleIndex(railHandleIndex);
    setHoverHandleIndex(railHandleIndex);
    setFocusHandleIndex(railHandleIndex);
    dragStartPos_ = event->position().toPoint();
    dragStartValues_ = handles_;
    dragging_ = true;
    pressedHandleIndex_ = railHandleIndex;
    setSliderDownInternal(true);
    emit handlePressed(railHandleIndex);
    invalidateLayoutCache();
    requestTooltipSync();
    update();
  }
}

void AdMultiSlider::mouseMoveEvent(QMouseEvent* event) {
  QWidget::mouseMoveEvent(event);
  if (!event || disabled()) {
    return;
  }

  const LayoutInfo layout = buildLayout();
  const int hitIndex = hitTestHandle(event->position().toPoint(), layout);
  if (hoverHandleIndex_ != hitIndex && dragMode_ == DragMode::None) {
    setHoverHandleIndex(hitIndex);
    update();
  }

  if (dragMode_ == DragMode::None) {
    return;
  }

  if (dragMode_ == DragMode::Handle && dragHandleIndex_ >= 0 &&
      dragHandleIndex_ < handles_.size()) {
    if (mode_ == Mode::Range && editableHandles_) {
      const qreal mainAxisCoord = layout.axis.coordinate(event->position().toPoint());
      const qreal axisMin = layout.axis.handleStart;
      const qreal axisMax = layout.axis.handleEnd;
      const bool outOfAxisBounds = mainAxisCoord < (axisMin - kEditableHandleDeleteThresholdPx) ||
                                   mainAxisCoord > (axisMax + kEditableHandleDeleteThresholdPx);
      if (outOfAxisBounds) {
        const int requiredMinCount = std::max(1, minHandleCount_);
        if (handles_.size() > requiredMinCount && deleteHandleAt(dragHandleIndex_, tracking_)) {
          // Keep drag session alive until mouse up so completion signal timing
          // stays aligned with regular drag flows.
          setDragHandleIndex(-1);
          setHoverHandleIndex(-1);
          update();
          return;
        }
      }
    }

    QList<double> next = handles_;
    if (dragHandleIndex_ >= next.size()) {
      return;
    }
    const double nextValue = valueFromHandlePosition(event->position().toPoint(), layout);
    next[dragHandleIndex_] = nextValue;
    setHandlesInternal(next, tracking_, true, tracking_, true);
    const int activeIndex = dragHandleIndex_;
    if (activeIndex >= 0) {
      setDragHandleIndex(activeIndex);
      setHoverHandleIndex(activeIndex);
      setFocusHandleIndex(activeIndex);
    }
    return;
  }

  if (dragMode_ == DragMode::Track && !dragStartValues_.isEmpty()) {
    const qreal pixelDelta =
        layout.axis.valueDirectedDelta(dragStartPos_, event->position().toPoint());
    const double valueDelta =
        clampTrackDelta((pixelDelta / layout.axis.handleLength()) * (maximum_ - minimum_));
    QList<double> next = dragStartValues_;
    for (double& value : next) {
      value = normalizeValue(value + valueDelta);
    }
    setHandlesInternal(next, tracking_, true, tracking_, true);
  }
}

void AdMultiSlider::mouseReleaseEvent(QMouseEvent* event) {
  QWidget::mouseReleaseEvent(event);
  if (!event || event->button() != Qt::LeftButton) {
    return;
  }

  const bool hadDrag = dragMode_ != DragMode::None;
  const int releasedHandleIndex = pressedHandleIndex_;
  dragMode_ = DragMode::None;
  dragging_ = false;
  dragStartValues_.clear();
  setDragHandleIndex(-1);
  setSliderDownInternal(false);

  if (hadDrag && dragChanged_) {
    if (!tracking_) {
      commitPendingChanges();
    }
    emitCompletedSignalsForCurrentMode();
  }
  if (hadDrag && releasedHandleIndex >= 0) {
    emit handleReleased(releasedHandleIndex);
  }

  dragChanged_ = false;
  pressedHandleIndex_ = -1;
  if (!rect().contains(event->position().toPoint())) {
    setHoverHandleIndex(-1);
  }
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

void AdMultiSlider::wheelEvent(QWheelEvent* event) {
  if (!event) {
    return;
  }
  if (disabled() || !wheelEnabled_ || handles_.isEmpty()) {
    event->ignore();
    return;
  }

  const int angleDelta = event->angleDelta().y();
  if (angleDelta == 0) {
    event->ignore();
    return;
  }

  pendingPrimaryValueChange_ = false;
  pendingValuesChange_ = false;
  dragChanged_ = false;

  int index = focusHandleIndex_;
  if (index < 0 || index >= handles_.size()) {
    index = 0;
  }

  QList<double> next = handles_;
  next[index] = normalizeValue(next.at(index) +
                               (angleDelta > 0 ? effectiveSingleStep() : -effectiveSingleStep()));
  emitHandleActionTriggered(
      angleDelta > 0 ? QAbstractSlider::SliderSingleStepAdd : QAbstractSlider::SliderSingleStepSub,
      index);
  setHandlesInternal(next, tracking_, true, tracking_);
  setFocusHandleIndex(index);
  if (!tracking_) {
    commitPendingChanges();
  }
  emitCompletedSignalsForCurrentMode();
  dragChanged_ = false;
  event->accept();
}

void AdMultiSlider::keyPressEvent(QKeyEvent* event) {
  if (!event) {
    QWidget::keyPressEvent(event);
    return;
  }

  if (disabled() || !keyboardEnabled_) {
    QWidget::keyPressEvent(event);
    return;
  }

  const int key = event->key();
  const int handleCount = static_cast<int>(handles_.size());
  if ((key == Qt::Key_Tab || key == Qt::Key_Backtab) && handleCount > 1) {
    int index = focusHandleIndex_;
    if (index < 0 || index >= handleCount) {
      index = 0;
    } else if (key == Qt::Key_Tab) {
      index = (index + 1) % handleCount;
    } else {
      index = (index - 1 + handleCount) % handleCount;
    }
    setFocusHandleIndex(index);
    event->accept();
    return;
  }

  if (mode_ == Mode::Range && editableHandles_ &&
      (key == Qt::Key_Delete || key == Qt::Key_Backspace)) {
    pendingPrimaryValueChange_ = false;
    pendingValuesChange_ = false;
    dragChanged_ = false;
    if (focusHandleIndex_ < 0 || focusHandleIndex_ >= handleCount) {
      setFocusHandleIndex(handleCount - 1);
    }
    if (deleteHandleAt(focusHandleIndex_, true)) {
      emitCompletedSignalsForCurrentMode();
      dragChanged_ = false;
      event->accept();
      return;
    }
  }

  if (handles_.isEmpty()) {
    QWidget::keyPressEvent(event);
    return;
  }

  int index = focusHandleIndex_;
  if (index < 0 || index >= handles_.size()) {
    index = 0;
  }
  double nextValue = handles_.at(index);
  bool handled = true;

  switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Down:
    case Qt::Key_Right:
    case Qt::Key_Up:
      nextValue += adjustmentDirectionForKey(this, key) * effectiveSingleStep();
      break;
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
      nextValue += adjustmentDirectionForKey(this, key) * effectivePageStep();
      break;
    case Qt::Key_Home:
      nextValue = minimum_;
      break;
    case Qt::Key_End:
      nextValue = maximum_;
      break;
    default:
      handled = false;
      break;
  }

  if (!handled) {
    QWidget::keyPressEvent(event);
    return;
  }

  nextValue = normalizeValue(nextValue);
  QList<double> next = handles_;
  next[index] = nextValue;
  if (!keyboardInteractionActive_) {
    pendingPrimaryValueChange_ = false;
    pendingValuesChange_ = false;
    dragChanged_ = false;
  }
  keyboardInteractionActive_ = true;
  emitHandleActionTriggered(sliderActionForKey(this, key), index);
  const qint64 focusedHandleId = index < handleIds_.size() ? handleIds_.at(index) : -1;
  const QList<double> before = handles_;
  setHandlesInternal(next, tracking_, true, tracking_);
  const int activeIndex =
      focusedHandleId >= 0 ? indexForHandleId(focusedHandleId) : nearestHandleIndex(nextValue);
  setFocusHandleIndex(activeIndex);
  if (!listFuzzyEquals(before, handles_)) {
    dragChanged_ = true;
  }
  event->accept();
}

void AdMultiSlider::keyReleaseEvent(QKeyEvent* event) {
  QWidget::keyReleaseEvent(event);
  if (!event || disabled() || !keyboardEnabled_) {
    return;
  }

  if (event->isAutoRepeat()) {
    return;
  }

  if (!isKeyAdjustAction(event->key())) {
    return;
  }

  keyboardInteractionActive_ = false;
  if (dragChanged_) {
    if (!tracking_) {
      commitPendingChanges();
    }
    emitCompletedSignalsForCurrentMode();
    dragChanged_ = false;
  }
}

void AdMultiSlider::focusInEvent(QFocusEvent* event) {
  focusVisible_ = event && isKeyboardFocusReason(event->reason());
  if (focusHandleIndex_ < 0 && !handles_.isEmpty()) {
    setFocusHandleIndex(0);
  }
  QWidget::focusInEvent(event);
  notifyAccessibleFocusChange();
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

void AdMultiSlider::focusOutEvent(QFocusEvent* event) {
  focusVisible_ = false;
  if (keyboardInteractionActive_ && dragChanged_) {
    if (!tracking_) {
      commitPendingChanges();
    }
    emitCompletedSignalsForCurrentMode();
    dragChanged_ = false;
  }
  keyboardInteractionActive_ = false;
  QWidget::focusOutEvent(event);
  invalidateLayoutCache();
  requestTooltipSync();
  update();
}

void AdMultiSlider::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::EnabledChange) {
    const bool disabledNow = disabled();
    hovered_ = false;
    setHoverHandleIndex(-1);
    dragMode_ = DragMode::None;
    dragging_ = false;
    setSliderDownInternal(false);
    dragChanged_ = false;
    pendingPrimaryValueChange_ = false;
    pendingValuesChange_ = false;
    keyboardInteractionActive_ = false;
    pressedHandleIndex_ = -1;
    setDragHandleIndex(-1);
    syncInteractionCursor();
    invalidateLayoutCache();
    requestTooltipSync();
    if (lastDisabledState_ != disabledNow) {
      lastDisabledState_ = disabledNow;
      emit disabledChanged(disabledNow);
      QAccessible::State changedState;
      changedState.disabled = true;
      QAccessibleStateChangeEvent accessibilityEvent(this, changedState);
      QAccessible::updateAccessibility(&accessibilityEvent);
    }
    update();
  } else if (event->type() == QEvent::FontChange ||
             event->type() == QEvent::ApplicationFontChange ||
             event->type() == QEvent::PaletteChange ||
             event->type() == QEvent::ApplicationPaletteChange ||
             event->type() == QEvent::StyleChange ||
             event->type() == QEvent::LayoutDirectionChange) {
    refreshAfterPropertyChange();
  } else if (event->type() == QEvent::LanguageChange) {
    syncAccessibleState();
    notifyAccessibleDescriptionChange();
    update();
  }
}

void AdMultiSlider::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  invalidateLayoutCache();
  requestTooltipSync();
}

void AdMultiSlider::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  detail::notifyAccessibilityEvent(this, QAccessible::ObjectShow);
  invalidateLayoutCache();
  requestTooltipSync();
}

void AdMultiSlider::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  detail::notifyAccessibilityEvent(this, QAccessible::ObjectHide);
}

AdSlider::AdSlider(QWidget* parent) : AdMultiSlider(parent) {
  setMode(Mode::Single);
  setHandleValues({minimum()});

  connect(this, &AdMultiSlider::handlePressed, this, [this](int index) {
    if (index == 0) {
      emit sliderPressed();
    }
  });
  connect(this, &AdMultiSlider::handleMoved, this, [this](int index, double value) {
    if (index == 0) {
      emit sliderMoved(value);
    }
  });
  connect(this, &AdMultiSlider::handleReleased, this, [this](int index) {
    if (index == 0) {
      emit sliderReleased();
    }
  });
  connect(this, &AdMultiSlider::handleActionTriggered, this,
          [this](QAbstractSlider::SliderAction action, int index) {
            if (index == 0) {
              emit actionTriggered(action);
            }
          });
}

void AdSlider::setRange(double minimum, double maximum) {
  AdMultiSlider::setRange(minimum, maximum);
}

AdRangeSlider::AdRangeSlider(QWidget* parent) : AdMultiSlider(parent) {
  setMode(Mode::Range);
  setHandleEditingEnabled(false);
  setHandleValues({minimum(), minimum()});
  lastLowerValue_ = lowerValue();
  lastUpperValue_ = upperValue();

  connect(this, &AdMultiSlider::handleValuesChanged, this, [this](const QList<double>& values) {
    const double lower = values.isEmpty() ? minimum() : values.constFirst();
    const double upper = values.size() < 2 ? lower : values.at(1);
    const bool lowerChanged = !fuzzyEq(lastLowerValue_, lower);
    const bool upperChanged = !fuzzyEq(lastUpperValue_, upper);
    if (lowerChanged) {
      lastLowerValue_ = lower;
      emit lowerValueChanged(lower);
    }
    if (upperChanged) {
      lastUpperValue_ = upper;
      emit upperValueChanged(upper);
    }
    if (lowerChanged || upperChanged) {
      emit valuesChanged(lower, upper);
    }
  });
  connect(this, &AdMultiSlider::handlePressed, this,
          [this](int index) { emit sliderPressed(index); });
  connect(this, &AdMultiSlider::handleMoved, this,
          [this](int index, double value) { emit sliderMoved(index, value); });
  connect(this, &AdMultiSlider::handleReleased, this,
          [this](int index) { emit sliderReleased(index); });
  connect(this, &AdMultiSlider::handleActionTriggered, this,
          [this](QAbstractSlider::SliderAction action, int index) {
            emit actionTriggered(action, index);
          });
}

double AdRangeSlider::lowerValue() const {
  const QList<double> currentValues = handleValues();
  return currentValues.isEmpty() ? minimum() : currentValues.constFirst();
}

void AdRangeSlider::setLowerValue(double value) {
  QList<double> currentValues = handleValues();
  if (currentValues.isEmpty()) {
    currentValues = {value, value};
  } else if (currentValues.size() == 1) {
    currentValues = {value, currentValues.constFirst()};
  } else {
    currentValues[0] = value;
  }
  AdMultiSlider::setHandleValues(currentValues);
}

double AdRangeSlider::upperValue() const {
  const QList<double> currentValues = handleValues();
  if (currentValues.size() >= 2) {
    return currentValues.at(1);
  }
  return currentValues.isEmpty() ? minimum() : currentValues.constFirst();
}

void AdRangeSlider::setUpperValue(double value) {
  QList<double> currentValues = handleValues();
  if (currentValues.isEmpty()) {
    currentValues = {minimum(), value};
  } else if (currentValues.size() == 1) {
    currentValues.append(value);
  } else {
    currentValues[1] = value;
  }
  AdMultiSlider::setHandleValues(currentValues);
}

void AdRangeSlider::setValues(double lowerValue, double upperValue) {
  AdMultiSlider::setHandleValues({lowerValue, upperValue});
}

void AdRangeSlider::setRange(double minimum, double maximum) {
  AdMultiSlider::setRange(minimum, maximum);
}

}  // namespace adqt::widgets
