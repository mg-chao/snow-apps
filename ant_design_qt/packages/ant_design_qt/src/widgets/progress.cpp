#include "progress.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <utility>

#include "progress_style.h"
#include "theme/theme_manager.h"
#include "widgets/detail/timing_hub.h"

namespace adqt::widgets {
namespace {

constexpr char kFrameKey[] = "AdProgress.Animation";
constexpr qreal kPi = 3.14159265358979323846;

qreal boundedPercent(qreal value) { return std::clamp(value, 0.0, 100.0); }

class ProgressAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
 public:
  explicit ProgressAccessible(AdProgress* widget)
      : QAccessibleWidget(widget, QAccessible::ProgressBar) {}
  QString text(QAccessible::Text role) const override {
    const auto* progress = qobject_cast<AdProgress*>(object());
    if (!progress) return QAccessibleWidget::text(role);
    if (role == QAccessible::Name && progress->accessibleName().isEmpty())
      return AdProgress::tr("Progress");
    if (role == QAccessible::Value) return progress->formattedText();
    if (role == QAccessible::Description && progress->accessibleDescription().isEmpty()) {
      if (progress->effectiveStatus() == AdProgress::Status::Exception)
        return AdProgress::tr("Failed");
      if (progress->effectiveStatus() == AdProgress::Status::Success)
        return AdProgress::tr("Completed");
    }
    return QAccessibleWidget::text(role);
  }
  QAccessible::State state() const override {
    auto result = QAccessibleWidget::state();
    result.readOnly = true;
    result.focusable = false;
    return result;
  }
  void* interface_cast(QAccessible::InterfaceType type) override {
    return type == QAccessible::ValueInterface ? static_cast<QAccessibleValueInterface*>(this)
                                               : QAccessibleWidget::interface_cast(type);
  }
  QVariant currentValue() const override { return static_cast<AdProgress*>(object())->percent(); }
  void setCurrentValue(const QVariant&) override {}
  QVariant minimumValue() const override { return 0.0; }
  QVariant maximumValue() const override { return 100.0; }
  QVariant minimumStepSize() const override { return QVariant(); }
};

QAccessibleInterface* accessibleFactory(const QString&, QObject* object) {
  if (auto* progress = qobject_cast<AdProgress*>(object)) return new ProgressAccessible(progress);
  return nullptr;
}

Qt::PenCapStyle penCap(AdProgress::StrokeLinecap cap) {
  if (cap == AdProgress::StrokeLinecap::Butt) return Qt::FlatCap;
  if (cap == AdProgress::StrokeLinecap::Square) return Qt::SquareCap;
  return Qt::RoundCap;
}

void paintStatusIcon(QPainter& painter, const QRectF& bounds, bool success, bool filled,
                     const QColor& color) {
  const qreal side = std::min(bounds.width(), bounds.height());
  QRectF box(bounds.center().x() - side / 2, bounds.center().y() - side / 2, side, side);
  painter.save();
  painter.translate(box.topLeft());
  painter.scale(side / 16, side / 16);
  painter.setBrush(filled ? QBrush(color) : Qt::NoBrush);
  painter.setPen(Qt::NoPen);
  if (filled) painter.drawEllipse(QRectF(0, 0, 16, 16));
  painter.setPen(QPen(filled ? QColor(Qt::white) : color, filled ? 1.6 : 1.8, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  QPainterPath path;
  if (success) {
    path.moveTo(4.1, 8.2);
    path.lineTo(6.8, 10.8);
    path.lineTo(12, 5.3);
  } else {
    path.moveTo(5, 5);
    path.lineTo(11, 11);
    path.moveTo(11, 5);
    path.lineTo(5, 11);
  }
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(path);
  painter.restore();
}

void drawRing(QPainter& painter, const QRectF& circle, qreal start, qreal sweep, qreal from,
              qreal amount, qreal width, const QBrush& brush, Qt::PenCapStyle cap) {
  amount = std::clamp(amount, 0.0, 100.0 - from);
  if (amount <= 0 || width <= 0 || circle.isEmpty()) return;
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(brush, width, Qt::SolidLine, cap));
  if (amount >= 100 && sweep >= 360) {
    painter.drawEllipse(circle);
    return;
  }
  qreal begin = start - sweep * from / 100;
  qreal span = sweep * amount / 100;
  if (cap == Qt::RoundCap && amount < 100) {
    const qreal capAngle = width / (circle.width() / 2) * 90 / kPi;
    if (span <= capAngle) {
      const qreal angle = begin * kPi / 180;
      const QPointF point(circle.center().x() + circle.width() / 2 * std::cos(angle),
                          circle.center().y() - circle.height() / 2 * std::sin(angle));
      painter.setPen(Qt::NoPen);
      painter.setBrush(brush);
      painter.drawEllipse(point, width / 2, width / 2);
      return;
    }
    span -= capAngle;
  }
  // QPainter's arc API uses sixteenths of a degree, independent of the device scale.
  painter.drawArc(circle, qRound(begin * 16), -qRound(span * 16));
}

}  // namespace

struct AdProgress::Private {
  Type type = Type::Line;
  Status status = Status::Automatic;
  Status effectiveStatus = Status::Normal;
  Size size = Size::Middle;
  StrokeLinecap cap = StrokeLinecap::Round;
  GapPlacement gapPlacement = GapPlacement::Bottom;
  PercentAlignment alignment = PercentAlignment::End;
  PercentPlacement placement = PercentPlacement::Outer;
  qreal percent = 0;
  qreal displayed = 0;
  qreal successPercent = -1;
  qreal strokeWidth = 0;
  qreal stepGap = 2;
  qreal gapDegree = -1;
  int steps = 0;
  bool showInfo = true;
  bool animationEnabled = true;
  bool reversedGradient = false;
  QSize sizeOverride;
  QColor fill, rail, success;
  QGradientStops gradient;
  QList<QColor> stepColors;
  Format format;
  StepRounding rounding;
  ComponentTokens tokens;
  SemanticStyles semantic;
  detail::ProgressVisualStyle style;
  std::optional<theme::ResolvedTheme> resolvedTheme;
  QBrush lineBrush;
  QBrush ringBrush;
  QImage raster;
  quint64 paintRevision = 1;
  quint64 rasterRevision = 0;
  quint64 lastPaintRevision = 0;
  QSize lastPaintPixels;
  qreal lastPaintScale = 0;
  QRectF ringBrushBounds;
  qreal ringBrushStart = -1000;
  qreal ringBrushSweep = -1;
  bool ringBrushDirty = true;
  QString text;
  QString autoTooltip;
  QFont infoFont;
  qreal textWidth = 0;
  qreal textHeight = 0;
  bool statusIcon = false;
  qreal iconSize = 28;
  bool transition = false;
  bool subscribed = false;
  qreal transitionFrom = 0;
  qint64 transitionStart = 0;
  qint64 shineStart = 0;
  qreal shine = 0;
  QEasingCurve easing{QEasingCurve::BezierSpline};
};

AdProgress::AdProgress(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>()) {
  static const bool installed = [] {
    QAccessible::installFactory(accessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
  setFocusPolicy(Qt::NoFocus);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  d_->easing.addCubicBezierSegment(QPointF(0.78, 0.14), QPointF(0.15, 0.86), QPointF(1, 1));
  connect(&theme::ThemeManager::instance(), &theme::ThemeManager::themeChanged, this,
          [this] { refreshAppearance(true); });
  refreshAppearance();
}

AdProgress::~AdProgress() {
  if (d_->subscribed) detail::clearFrameSubscription(this, QString::fromLatin1(kFrameKey));
}

AdProgress::Type AdProgress::type() const { return d_->type; }
void AdProgress::setType(Type value) {
  if (d_->type == value) return;
  d_->type = value;
  QSizePolicy policy(
      value == Type::Line && d_->steps == 0 ? QSizePolicy::Expanding : QSizePolicy::Preferred,
      value == Type::Line ? QSizePolicy::Fixed : QSizePolicy::Preferred);
  policy.setHeightForWidth(value != Type::Line);
  setSizePolicy(policy);
  refreshAppearance();
  emit typeChanged(value);
}
AdProgress::Status AdProgress::status() const { return d_->status; }
void AdProgress::setStatus(Status value) {
  if (d_->status == value) return;
  d_->status = value;
  refreshAppearance();
  emit statusChanged(value);
}
AdProgress::Status AdProgress::effectiveStatus() const { return d_->effectiveStatus; }
qreal AdProgress::percent() const { return d_->percent; }
qreal AdProgress::displayedPercent() const { return d_->displayed; }
void AdProgress::setPercent(qreal value) {
  if (!std::isfinite(value)) return;
  value = boundedPercent(value);
  if (d_->percent == value) return;
  d_->percent = value;
  d_->transitionFrom = d_->displayed;
  d_->transition =
      d_->style.motion && isVisible() && d_->style.animationDurationMs > 0 && d_->steps == 0;
  if (d_->transition) d_->transitionStart = detail::timingNowMs();
  if (!d_->transition) {
    d_->displayed = value;
    emit displayedPercentChanged(value);
  }
  refreshValue();
  emit percentChanged(value);
  notifyAccessibleValueChange();
}
qreal AdProgress::successPercent() const { return d_->successPercent; }
void AdProgress::setSuccessPercent(qreal value) {
  if (!std::isfinite(value)) return;
  value = value < 0 ? -1 : boundedPercent(value);
  if (d_->successPercent == value) return;
  d_->successPercent = value;
  refreshValue();
  emit successPercentChanged(value);
}
void AdProgress::clearSuccess() { setSuccessPercent(-1); }

#define ADQT_PROGRESS_SETTER(TypeName, Getter, Setter, Member, Signal) \
  TypeName AdProgress::Getter() const { return d_->Member; }           \
  void AdProgress::Setter(TypeName value) {                            \
    if (d_->Member == value) return;                                   \
    d_->Member = value;                                                \
    refreshAppearance();                                               \
    emit Signal(value);                                                \
  }
ADQT_PROGRESS_SETTER(bool, showInfo, setShowInfo, showInfo, showInfoChanged)
ADQT_PROGRESS_SETTER(AdProgress::Size, sizeClass, setSizeClass, size, sizeClassChanged)
ADQT_PROGRESS_SETTER(AdProgress::StrokeLinecap, strokeLinecap, setStrokeLinecap, cap,
                     strokeLinecapChanged)
ADQT_PROGRESS_SETTER(AdProgress::GapPlacement, gapPlacement, setGapPlacement, gapPlacement,
                     gapPlacementChanged)
ADQT_PROGRESS_SETTER(AdProgress::PercentAlignment, percentAlignment, setPercentAlignment, alignment,
                     percentAlignmentChanged)
ADQT_PROGRESS_SETTER(AdProgress::PercentPlacement, percentPlacement, setPercentPlacement, placement,
                     percentPlacementChanged)
ADQT_PROGRESS_SETTER(bool, animationEnabled, setAnimationEnabled, animationEnabled,
                     animationEnabledChanged)
#undef ADQT_PROGRESS_SETTER

QSize AdProgress::progressSize() const { return d_->sizeOverride; }
void AdProgress::setProgressSize(const QSize& value) {
  const QSize normalized(std::max(0, value.width()), std::max(0, value.height()));
  if (normalized == d_->sizeOverride) return;
  d_->sizeOverride = normalized;
  refreshAppearance();
  emit progressSizeChanged(normalized);
}
qreal AdProgress::strokeWidth() const { return d_->strokeWidth; }
void AdProgress::setStrokeWidth(qreal value) {
  if (!std::isfinite(value)) return;
  value = std::clamp(value, 0.0, 10000.0);
  if (d_->strokeWidth == value) return;
  d_->strokeWidth = value;
  refreshAppearance();
  emit strokeWidthChanged(value);
}
#define ADQT_PROGRESS_COLOR(Getter, Setter, Member, Signal) \
  QColor AdProgress::Getter() const { return d_->Member; }  \
  void AdProgress::Setter(const QColor& value) {            \
    if (d_->Member == value) return;                        \
    d_->Member = value;                                     \
    refreshAppearance();                                    \
    emit Signal(value);                                     \
  }
ADQT_PROGRESS_COLOR(strokeColor, setStrokeColor, fill, strokeColorChanged)
ADQT_PROGRESS_COLOR(railColor, setRailColor, rail, railColorChanged)
ADQT_PROGRESS_COLOR(successColor, setSuccessColor, success, successColorChanged)
#undef ADQT_PROGRESS_COLOR

QGradientStops AdProgress::strokeGradient() const { return d_->gradient; }
void AdProgress::setStrokeGradient(const QGradientStops& stops) {
  QGradientStops normalized;
  for (const auto& stop : stops) {
    if (std::isfinite(stop.first) && stop.second.isValid())
      normalized.append({std::clamp(stop.first, 0.0, 1.0), stop.second});
  }
  std::stable_sort(normalized.begin(), normalized.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  // Keep the final color at duplicate positions, so interpolation is deterministic.
  QGradientStops unique;
  for (const auto& stop : normalized) {
    if (!unique.isEmpty() && unique.last().first == stop.first)
      unique.last() = stop;
    else
      unique.append(stop);
  }
  if (d_->gradient == unique) return;
  d_->gradient = unique;
  refreshAppearance();
  emit strokeGradientChanged();
}
bool AdProgress::gradientReversed() const { return d_->reversedGradient; }
void AdProgress::setGradientReversed(bool value) {
  if (d_->reversedGradient == value) return;
  d_->reversedGradient = value;
  refreshAppearance();
  emit strokeGradientChanged();
}
int AdProgress::steps() const { return d_->steps; }
void AdProgress::setSteps(int value) {
  value = std::clamp(value, 0, 10000);
  if (d_->steps == value) return;
  d_->steps = value;
  QSizePolicy policy(
      d_->type == Type::Line && value == 0 ? QSizePolicy::Expanding : QSizePolicy::Preferred,
      d_->type == Type::Line ? QSizePolicy::Fixed : QSizePolicy::Preferred);
  policy.setHeightForWidth(d_->type != Type::Line);
  setSizePolicy(policy);
  refreshAppearance();
  emit stepsChanged(value);
}
qreal AdProgress::stepGap() const { return d_->stepGap; }
void AdProgress::setStepGap(qreal value) {
  if (!std::isfinite(value)) return;
  value = std::clamp(value, 0.0, 10000.0);
  if (d_->stepGap == value) return;
  d_->stepGap = value;
  updateGeometry();
  invalidatePainting();
  emit stepGapChanged(value);
}
QList<QColor> AdProgress::stepColors() const { return d_->stepColors; }
void AdProgress::setStepColors(const QList<QColor>& value) {
  if (d_->stepColors == value) return;
  d_->stepColors = value;
  invalidatePainting();
  emit stepColorsChanged();
}
void AdProgress::setStepRounding(StepRounding value) {
  d_->rounding = std::move(value);
  invalidatePainting();
}
void AdProgress::resetStepRounding() { setStepRounding({}); }
qreal AdProgress::gapDegree() const { return d_->gapDegree; }
void AdProgress::setGapDegree(qreal value) {
  if (!std::isfinite(value)) return;
  value = value < 0 ? -1 : std::clamp(value, 0.0, 295.0);
  if (d_->gapDegree == value) return;
  d_->gapDegree = value;
  invalidatePainting();
  emit gapDegreeChanged(value);
}
void AdProgress::setFormat(Format value) {
  d_->format = std::move(value);
  refreshContent();
  updateGeometry();
  invalidatePainting();
  emit formatChanged();
}
void AdProgress::resetFormat() { setFormat({}); }
QString AdProgress::formattedText() const { return d_->text; }
AdProgress::ComponentTokens AdProgress::componentTokens() const { return d_->tokens; }
void AdProgress::setComponentTokens(const ComponentTokens& value) {
  d_->tokens = value;
  refreshAppearance();
  emit componentTokensChanged();
}
void AdProgress::resetComponentTokens() { setComponentTokens({}); }
AdProgress::SemanticStyles AdProgress::semanticStyles() const { return d_->semantic; }
void AdProgress::setSemanticStyles(const SemanticStyles& value) {
  d_->semantic = value;
  refreshAppearance();
  emit semanticStylesChanged();
}
void AdProgress::resetSemanticStyles() { setSemanticStyles({}); }

void AdProgress::refreshValue() {
  const Status effective =
      d_->status == Status::Automatic
          ? ((d_->successPercent >= 0 ? d_->successPercent : d_->percent) >= 100 ? Status::Success
                                                                                 : Status::Normal)
          : d_->status;
  if (effective != d_->effectiveStatus) {
    refreshAppearance();
    return;
  }
  const qreal oldWidth = d_->textWidth;
  const qreal oldHeight = d_->textHeight;
  refreshContent();
  refreshAnimation();
  if (oldWidth != d_->textWidth || oldHeight != d_->textHeight) updateGeometry();
  invalidatePainting();
}

void AdProgress::refreshAppearance(bool resolveTheme) {
  const Status effective =
      d_->status == Status::Automatic
          ? ((d_->successPercent >= 0 ? d_->successPercent : d_->percent) >= 100 ? Status::Success
                                                                                 : Status::Normal)
          : d_->status;
  const bool statusChanged = effective != d_->effectiveStatus;
  d_->effectiveStatus = effective;
  if (resolveTheme || !d_->resolvedTheme)
    d_->resolvedTheme = theme::ThemeManager::instance().resolve(this);
  d_->style = detail::resolveProgressVisualStyle(this, *d_->resolvedTheme);
  d_->ringBrushDirty = true;
  d_->lineBrush = QBrush(d_->style.fill);
  if (!d_->gradient.isEmpty()) {
    const bool reverse = (layoutDirection() == Qt::RightToLeft) != d_->reversedGradient;
    QLinearGradient gradient(reverse ? QPointF(1, 0) : QPointF(0, 0),
                             reverse ? QPointF(0, 0) : QPointF(1, 0));
    gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
    gradient.setStops(d_->gradient);
    d_->lineBrush = QBrush(gradient);
  }
  refreshContent();
  refreshAnimation();
  updateGeometry();
  invalidatePainting();
  if (statusChanged) {
    emit effectiveStatusChanged(effective);
    QAccessibleEvent event(this, QAccessible::DescriptionChanged);
    QAccessible::updateAccessibility(&event);
  }
}

void AdProgress::refreshContent() {
  d_->text = d_->format ? d_->format(d_->percent, std::max(0.0, d_->successPercent))
                        : tr("%1%").arg(QString::number(d_->percent, 'g', 12));
  d_->statusIcon =
      !d_->format &&
      (d_->effectiveStatus == Status::Success || d_->effectiveStatus == Status::Exception) &&
      !(d_->type == Type::Line && d_->steps == 0 && d_->placement == PercentPlacement::Inner);
  d_->infoFont = d_->style.font;
  if (d_->type != Type::Line) {
    qreal textSize = d_->style.circleTextSize;
    if (!d_->tokens.metrics.circleTextFontSize && !d_->semantic.indicator.font) {
      const int diameter = std::min(contentsRect().width(), contentsRect().height());
      if (diameter > 0) textSize = diameter * 0.15 + 6;
    }
    d_->infoFont.setPixelSize(std::max(1, qRound(textSize)));
    d_->iconSize = d_->tokens.metrics.circleIconFontSize.value_or(textSize * 14 / 12);
    if (!std::isfinite(d_->iconSize) || d_->iconSize <= 0) d_->iconSize = textSize * 14 / 12;
  }
  const QFontMetricsF metrics(d_->infoFont);
  d_->textWidth =
      d_->statusIcon
          ? (d_->type == Type::Line
                 ? (d_->infoFont.pixelSize() > 0 ? d_->infoFont.pixelSize()
                                                 : d_->infoFont.pointSizeF() * logicalDpiY() / 72)
                 : d_->iconSize)
          : metrics.horizontalAdvance(d_->text);
  d_->textHeight = metrics.height();
  // Preserve an application-supplied tooltip when switching to or from a micro circle.
  if (toolTip().isEmpty() || toolTip() == d_->autoTooltip) {
    const int diameter = std::min(contentsRect().width(), contentsRect().height());
    d_->autoTooltip =
        d_->showInfo && d_->type != Type::Line && diameter <= 20
            ? (d_->statusIcon
                   ? (d_->effectiveStatus == Status::Success ? tr("Completed") : tr("Failed"))
                   : d_->text)
            : QString();
    setToolTip(d_->autoTooltip);
  }
}

void AdProgress::refreshAnimation() {
  const bool visibleMotion = d_->style.motion && isVisible();
  if ((!visibleMotion || d_->steps > 0 || d_->style.animationDurationMs == 0) && d_->transition) {
    d_->transition = false;
    if (d_->displayed != d_->percent) {
      d_->displayed = d_->percent;
      emit displayedPercentChanged(d_->displayed);
    }
  }
  const bool shine = visibleMotion && d_->effectiveStatus == Status::Active &&
                     d_->type == Type::Line && d_->steps == 0 && d_->percent > 0 &&
                     d_->percent < 100;
  const bool subscribed = shine || d_->transition;
  if (d_->subscribed == subscribed) return;
  d_->subscribed = subscribed;
  detail::setFrameSubscription(this, QString::fromLatin1(kFrameKey), subscribed,
                               [this](qint64 nowMs, qint64) { animationFrame(nowMs); });
}

void AdProgress::animationFrame(qint64 nowMs) {
  if (d_->transition) {
    const qreal fraction = std::clamp(static_cast<qreal>(nowMs - d_->transitionStart) /
                                          std::max(1, d_->style.animationDurationMs),
                                      0.0, 1.0);
    d_->displayed = d_->transitionFrom +
                    (d_->percent - d_->transitionFrom) * d_->easing.valueForProgress(fraction);
    if (fraction >= 1) {
      d_->displayed = d_->percent;
      d_->transition = false;
      refreshAnimation();
    }
    emit displayedPercentChanged(d_->displayed);
  }
  d_->shine = static_cast<qreal>((nowMs - d_->shineStart) % 2400) / 2400;
  invalidatePainting();
}

void AdProgress::notifyAccessibleValueChange() {
  QAccessibleValueChangeEvent event(this, d_->percent);
  QAccessible::updateAccessibility(&event);
}

QSize AdProgress::sizeHint() const {
  const bool small = d_->size == Size::Small;
  if (d_->type != Type::Line) {
    const int diameter =
        d_->sizeOverride.width() > 0 ? d_->sizeOverride.width() : (small ? 60 : 120);
    return QSize(diameter, d_->sizeOverride.height() > 0 ? d_->sizeOverride.height() : diameter);
  }
  const qreal height =
      d_->steps > 0 ? (d_->sizeOverride.height() > 0 ? d_->sizeOverride.height()
                                                     : (d_->strokeWidth > 0 ? d_->strokeWidth : 8))
                    : d_->style.lineHeight;
  const qreal labelWidth =
      d_->showInfo && (d_->steps > 0 || d_->placement == PercentPlacement::Outer)
          ? d_->textWidth + (d_->steps > 0 ? 10 : 8)
          : 0;
  qreal width = d_->sizeOverride.width() > 0 ? d_->sizeOverride.width() : 200;
  if (d_->steps > 0) {
    const qreal unit =
        d_->sizeOverride.width() > 0 ? std::max(2, d_->sizeOverride.width()) : (small ? 2 : 14);
    width = d_->steps * unit + std::max(0, d_->steps - 1) * d_->stepGap + labelWidth;
  }
  qreal totalHeight = d_->showInfo ? std::max(height, d_->textHeight) : height;
  if (d_->showInfo && d_->placement == PercentPlacement::Inner)
    totalHeight = std::max(height, d_->textHeight);
  if (d_->steps == 0 && d_->showInfo && d_->placement == PercentPlacement::Outer &&
      d_->alignment == PercentAlignment::Center)
    totalHeight = height + 4 + d_->textHeight;
  return QSize(qCeil(width), qCeil(totalHeight));
}
QSize AdProgress::minimumSizeHint() const {
  if (d_->type != Type::Line) return QSize(3, 3);
  return QSize(d_->showInfo && (d_->steps > 0 || d_->placement == PercentPlacement::Outer)
                   ? qCeil(d_->textWidth + 10)
                   : 2,
               sizeHint().height());
}
bool AdProgress::hasHeightForWidth() const { return d_->type != Type::Line; }
int AdProgress::heightForWidth(int width) const {
  return d_->type == Type::Line ? sizeHint().height() : std::max(0, width);
}

void AdProgress::invalidatePainting() {
  ++d_->paintRevision;
  update();
}

void AdProgress::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  // deviceTransform also accounts for QWidget::render into a higher-DPI image
  // or a scaled painter, where the widget's screen DPR alone is insufficient.
  const QTransform transform = painter.deviceTransform();
  const qreal dpr = std::max(std::hypot(transform.m11(), transform.m12()),
                             std::hypot(transform.m21(), transform.m22()));
  const QSize pixels(qCeil(width() * dpr), qCeil(height() * dpr));
  // The single-entry cache is bounded to 4 MiB. Animated geometry draws directly
  // to avoid allocating an intermediate surface on each frame.
  const bool uniform = qFuzzyIsNull(transform.m12()) && qFuzzyIsNull(transform.m21()) &&
                       qFuzzyCompare(std::abs(transform.m11()), std::abs(transform.m22()));
  const bool cacheable = uniform && d_->type != Type::Line && !d_->transition &&
                         !pixels.isEmpty() &&
                         static_cast<qint64>(pixels.width()) * pixels.height() <= 1024 * 1024;
  if (!cacheable) {
    d_->raster = QImage();
    d_->lastPaintRevision = 0;
    paintProgress(painter);
    return;
  }
  // Rasterize only after a paint is actually repeated. Frequently changing
  // values keep the direct path, paying neither image allocation nor an extra blit.
  const bool repeated = d_->lastPaintRevision == d_->paintRevision &&
                        d_->lastPaintPixels == pixels && d_->lastPaintScale == dpr;
  d_->lastPaintRevision = d_->paintRevision;
  d_->lastPaintPixels = pixels;
  d_->lastPaintScale = dpr;
  if (!repeated) {
    paintProgress(painter);
    return;
  }
  if (d_->raster.size() != pixels || d_->raster.devicePixelRatio() != dpr) {
    d_->raster = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
    d_->raster.setDevicePixelRatio(dpr);
    d_->rasterRevision = 0;
  }
  if (d_->raster.isNull()) {
    paintProgress(painter);
    return;
  }
  if (d_->rasterRevision != d_->paintRevision) {
    d_->raster.fill(Qt::transparent);
    QPainter cached(&d_->raster);
    paintProgress(cached);
    cached.end();
    d_->rasterRevision = d_->paintRevision;
  }
  painter.drawImage(QPointF(0, 0), d_->raster);
}

void AdProgress::paintProgress(QPainter& painter) {
  painter.setRenderHint(QPainter::Antialiasing);
  const auto& style = d_->style;
  QRectF bounds(contentsRect());
  if (d_->type == Type::Line && d_->steps > 0 && bounds.width() > sizeHint().width()) {
    const qreal width = sizeHint().width();
    if (layoutDirection() == Qt::RightToLeft)
      bounds.setLeft(bounds.right() - width);
    else
      bounds.setWidth(width);
  }
  if (style.rootBackground.isValid()) painter.fillRect(rect(), style.rootBackground);
  if (style.bodyBackground.isValid()) painter.fillRect(bounds, style.bodyBackground);
  const bool rtl = layoutDirection() == Qt::RightToLeft;
  const bool inner =
      d_->type == Type::Line && d_->steps == 0 && d_->placement == PercentPlacement::Inner;
  const bool showInfo =
      d_->showInfo && (d_->type == Type::Line || std::min(bounds.width(), bounds.height()) > 20);
  QRectF indicator;

  if (d_->type == Type::Line) {
    const bool centered =
        d_->steps == 0 && showInfo && !inner && d_->alignment == PercentAlignment::Center;
    const qreal h = std::min(
        bounds.height(),
        d_->steps > 0
            ? (d_->sizeOverride.height() > 0 ? d_->sizeOverride.height()
                                             : (d_->strokeWidth > 0 ? d_->strokeWidth : 8))
            : (showInfo && inner ? std::max(style.lineHeight, d_->textHeight) : style.lineHeight));
    const qreal labelWidth =
        showInfo && !inner && !centered ? std::min(bounds.width(), d_->textWidth) : 0;
    const qreal gap = labelWidth > 0 ? std::min(d_->steps > 0 ? 10.0 : 8.0,
                                                std::max(0.0, bounds.width() - labelWidth))
                                     : 0;
    QRectF rail(bounds.left(), centered ? bounds.top() : bounds.center().y() - h / 2,
                std::max(0.0, bounds.width() - labelWidth - gap), h);
    if (labelWidth > 0) {
      const bool labelLeft = (d_->steps == 0 && d_->alignment == PercentAlignment::Start) != rtl;
      indicator = QRectF(labelLeft ? bounds.left() : bounds.right() - labelWidth, bounds.top(),
                         labelWidth, bounds.height());
      if (labelLeft) rail.translate(labelWidth + gap, 0);
    } else if (centered)
      indicator = QRectF(bounds.left(), rail.bottom() + 4, bounds.width(), d_->textHeight);
    if (d_->steps > 0) {
      const qreal unit = (rail.width() - (d_->steps - 1) * d_->stepGap) / d_->steps;
      const qreal stepWidth = std::max(0.0, unit);
      const qreal scaledGap = std::min(d_->stepGap, rail.width() / d_->steps);
      const qreal pitch = stepWidth + scaledGap;
      const qreal rawCount = d_->steps * d_->percent / 100;
      const int active =
          std::clamp(d_->rounding ? d_->rounding(rawCount) : qRound(rawCount), 0, d_->steps);
      const QColor stepFill = style.normalFill;
      painter.setPen(Qt::NoPen);
      // A collapsed rail has no segments to rasterize. Zero-gap steps can be
      // painted as two quantized spans instead of thousands of tiny rectangles.
      if (d_->stepGap == 0 && d_->stepColors.isEmpty()) {
        painter.fillRect(rail, style.rail);
        const qreal fillWidth = rail.width() * active / d_->steps;
        painter.fillRect(
            QRectF(rtl ? rail.right() - fillWidth : rail.left(), rail.top(), fillWidth, h),
            stepFill);
      } else if (stepWidth > 0) {
        for (int i = 0; i < d_->steps; ++i) {
          const qreal x = rtl ? rail.right() - i * pitch - stepWidth : rail.left() + i * pitch;
          const QColor color =
              i >= active
                  ? style.rail
                  : (i < d_->stepColors.size() && d_->stepColors[i].isValid() ? d_->stepColors[i]
                                                                              : stepFill);
          painter.fillRect(QRectF(x, rail.top(), stepWidth, h), color);
        }
      }
    } else {
      const qreal radius = d_->cap == StrokeLinecap::Round ? std::min(h / 2, style.lineRadius) : 0;
      painter.setPen(Qt::NoPen);
      painter.setBrush(style.rail);
      painter.drawRoundedRect(rail, radius, radius);
      qreal filledWidth = rail.width() * d_->displayed / 100;
      if (showInfo && inner)
        filledWidth = std::max(filledWidth, std::min(rail.width(), d_->textWidth + 8));
      QRectF fill(rtl ? rail.right() - filledWidth : rail.left(), rail.top(), filledWidth, h);
      if (filledWidth > 0) {
        painter.setBrush(d_->lineBrush);
        painter.drawRoundedRect(fill, radius, radius);
      }
      if (d_->successPercent > 0) {
        const qreal successWidth = rail.width() * d_->successPercent / 100;
        painter.setBrush(style.success);
        painter.drawRoundedRect(
            QRectF(rtl ? rail.right() - successWidth : rail.left(), rail.top(), successWidth, h),
            radius, radius);
      }
      if (d_->style.motion && d_->effectiveStatus == Status::Active && d_->percent > 0 &&
          d_->percent < 100 && !fill.isEmpty()) {
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(fill, radius, radius);
        painter.setClipPath(clip);
        const qreal phase = std::max(0.0, (d_->shine - 0.2) / 0.8);
        const qreal eased = 1 - std::pow(1 - phase, 5);
        const qreal shineWidth = fill.width() * eased;
        QColor shineColor(style.shimmer);
        shineColor.setAlphaF(static_cast<float>(0.5 * (1 - eased)));
        painter.fillRect(
            QRectF(rtl ? fill.right() - shineWidth : fill.left(), fill.top(), shineWidth, h),
            shineColor);
        painter.restore();
      }
      if (showInfo && inner) {
        indicator =
            fill.adjusted(std::min(4.0, fill.width() / 2), 0, -std::min(4.0, fill.width() / 2), 0);
      }
    }
  } else {
    const qreal diameter = std::min(bounds.width(), bounds.height());
    QRectF canvas(bounds.center().x() - diameter / 2, bounds.center().y() - diameter / 2, diameter,
                  diameter);
    const qreal stroke = std::min(diameter, d_->strokeWidth > 0 ? diameter * d_->strokeWidth / 100
                                                                : std::max(3.0, diameter * 0.06));
    const QRectF circle = canvas.adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2);
    const qreal gap = d_->gapDegree >= 0 ? d_->gapDegree : (d_->type == Type::Dashboard ? 75 : 0);
    const qreal sweep = 360 - gap;
    qreal gapCenter = 270;
    if (d_->gapPlacement == GapPlacement::Top) gapCenter = 90;
    if (d_->gapPlacement == GapPlacement::Start) gapCenter = rtl ? 0 : 180;
    if (d_->gapPlacement == GapPlacement::End) gapCenter = rtl ? 180 : 0;
    const qreal start = gap > 0 ? gapCenter - gap / 2 : 90;
    const Qt::PenCapStyle cap = d_->gradient.isEmpty() ? penCap(d_->cap) : Qt::FlatCap;
    if (d_->ringBrushDirty || d_->ringBrushBounds != circle || d_->ringBrushStart != start ||
        d_->ringBrushSweep != sweep) {
      d_->ringBrushDirty = false;
      d_->ringBrushBounds = circle;
      d_->ringBrushStart = start;
      d_->ringBrushSweep = sweep;
      d_->ringBrush = QBrush(style.fill);
      if (!d_->gradient.isEmpty()) {
        QConicalGradient gradient(circle.center(), start);
        QGradientStops stops;
        stops.append(
            {0, d_->reversedGradient ? d_->gradient.last().second : d_->gradient.first().second});
        for (auto it = d_->gradient.crbegin(); it != d_->gradient.crend(); ++it) {
          const qreal position = d_->reversedGradient ? 1 - it->first : it->first;
          stops.append({1 - position * sweep / 360, it->second});
        }
        std::sort(stops.begin(), stops.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        gradient.setStops(stops);
        d_->ringBrush = QBrush(gradient);
      }
    }
    const QBrush& brush = d_->ringBrush;
    if (d_->steps > 0) {
      const int active = std::clamp(qRound(d_->steps * d_->percent / 100), 0, d_->steps);
      const qreal slice = sweep / d_->steps;
      const qreal angularGap =
          circle.width() > 0
              ? std::min(slice, d_->stepGap * diameter / 100 / (circle.width() / 2) * 180 / kPi)
              : slice;
      painter.setBrush(Qt::NoBrush);
      if (angularGap == 0) {
        drawRing(painter, circle, start, sweep, 0, 100, stroke, style.rail, Qt::FlatCap);
        drawRing(painter, circle, start, sweep, 0, 100.0 * active / d_->steps, stroke, brush,
                 Qt::FlatCap);
      } else if (slice > angularGap) {
        for (int i = 0; i < d_->steps; ++i) {
          painter.setPen(
              QPen(i < active ? brush : QBrush(style.rail), stroke, Qt::SolidLine, Qt::FlatCap));
          painter.drawArc(circle, qRound((start - i * slice) * 16),
                          -qRound((slice - angularGap) * 16));
        }
      }
    } else {
      drawRing(painter, circle, start, sweep, 0, 100, stroke, style.rail, cap);
      const qreal success = std::max(0.0, d_->successPercent);
      drawRing(painter, circle, start, sweep, success, std::max(0.0, d_->displayed - success),
               stroke, brush, cap);
      drawRing(painter, circle, start, sweep, 0, success, stroke, style.success, cap);
    }
    indicator = canvas;
  }

  if (showInfo && !indicator.isEmpty()) {
    if (style.indicatorBackground.isValid()) painter.fillRect(indicator, style.indicatorBackground);
    QColor textColor =
        d_->type == Type::Line ? (inner ? style.innerText : style.text) : style.circleText;
    if (!inner && !d_->semantic.indicator.textColor) {
      if (d_->effectiveStatus == Status::Success) textColor = style.success;
      if (d_->effectiveStatus == Status::Exception) textColor = style.exception;
    }
    if (d_->statusIcon) {
      if (!d_->semantic.indicator.textColor)
        textColor = d_->effectiveStatus == Status::Success ? style.success : style.exception;
      const qreal side = std::min({d_->textWidth, indicator.width(), indicator.height()});
      paintStatusIcon(
          painter,
          QRectF(indicator.center().x() - side / 2, indicator.center().y() - side / 2, side, side),
          d_->effectiveStatus == Status::Success, d_->type == Type::Line, textColor);
    } else {
      painter.setFont(d_->infoFont);
      painter.setPen(textColor);
      Qt::Alignment alignment = Qt::AlignCenter;
      if (d_->type == Type::Line && inner && d_->alignment != PercentAlignment::Center)
        alignment =
            Qt::AlignVCenter |
            ((d_->alignment == PercentAlignment::Start) != rtl ? Qt::AlignLeft : Qt::AlignRight);
      const QString text =
          QFontMetricsF(d_->infoFont).elidedText(d_->text, Qt::ElideRight, indicator.width());
      painter.drawText(indicator, static_cast<int>(alignment.toInt()), text);
    }
  }
}

bool AdProgress::event(QEvent* event) {
  const bool result = QWidget::event(event);
  if (event->type() == QEvent::Show) {
    d_->shineStart = detail::timingNowMs();
    refreshAnimation();
  }
  if (event->type() == QEvent::Hide) {
    refreshAnimation();
    d_->raster = QImage();
  }
  if (event->type() == QEvent::Resize) {
    refreshContent();
    invalidatePainting();
  }
  if (event->type() == QEvent::ParentChange || event->type() == QEvent::ContentsRectChange)
    refreshAppearance(true);
  return result;
}
void AdProgress::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::LayoutDirectionChange ||
      event->type() == QEvent::LanguageChange || event->type() == QEvent::EnabledChange)
    refreshAppearance(true);
}

}  // namespace adqt::widgets
