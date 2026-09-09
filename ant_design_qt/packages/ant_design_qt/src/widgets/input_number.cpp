#include "input_number.h"

#include "detail/timing_hub.h"
#include "detail/overlay_accessibility.h"
#include "antd_icons.h"
#include "input_internal.h"
#include "input_number_style.h"
#include "input_number_value_model.h"
#include "interaction_overlay_manager.h"
#include "theme/theme.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegion>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace adqt::widgets {

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;
constexpr char kSuffixSpinnerFrameKey[] = "AdInputNumber.SuffixSpinnerFrame";

bool iconRefsEqual(const adqt::icons::IconRef& lhs, const adqt::icons::IconRef& rhs) {
  return lhs == rhs;
}

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

bool hasInteractiveControls(bool stepButtonsVisible, bool readOnly, bool disabled) {
  return stepButtonsVisible && !readOnly && !disabled;
}

int affixPadding(const detail::InputNumberVisualStyle& style) {
  return std::max(0, style.metrics.affixPadding);
}

int affixItemGap(const detail::InputNumberVisualStyle& style) {
  return std::max(affixPadding(style), style.metrics.affixItemGap);
}

Qt::Alignment defaultTextAlignmentForLayout(AdInputNumber::StepButtonLayout mode) {
  return mode == AdInputNumber::StepButtonLayout::Split ? Qt::AlignCenter
                                                        : (Qt::AlignLeft | Qt::AlignVCenter);
}

QPoint mouseEventPos(const QMouseEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return event ? event->position().toPoint() : QPoint();
#else
  return event ? event->pos() : QPoint();
#endif
}

bool isLeftMouseActivationEvent(const QEvent* event) {
  if (!event) {
    return false;
  }
  if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick) {
    return false;
  }
  const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
  return mouseEvent->button() == Qt::LeftButton;
}

QColor compositeOn(const QColor& foreground, const QColor& background) {
  if (!foreground.isValid()) {
    return background;
  }
  if (!background.isValid()) {
    QColor opaque = foreground;
    opaque.setAlpha(255);
    return opaque;
  }

  const float fgAlpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
  if (fgAlpha >= 0.999F) {
    return foreground;
  }

  QColor mixed;
  mixed.setRedF(foreground.redF() * fgAlpha + background.redF() * (1.0F - fgAlpha));
  mixed.setGreenF(foreground.greenF() * fgAlpha + background.greenF() * (1.0F - fgAlpha));
  mixed.setBlueF(foreground.blueF() * fgAlpha + background.blueF() * (1.0F - fgAlpha));
  mixed.setAlpha(255);
  return mixed;
}

QColor parseThemeColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
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
    path.arcTo(QRectF(right - 2.0 * topRight, top, 2.0 * topRight, 2.0 * topRight), 90.0, -90.0);
  }
  path.lineTo(right, bottom - bottomRight);
  if (bottomRight > 0.0) {
    path.arcTo(QRectF(right - 2.0 * bottomRight, bottom - 2.0 * bottomRight, 2.0 * bottomRight,
                      2.0 * bottomRight),
               0.0, -90.0);
  }
  path.lineTo(left + bottomLeft, bottom);
  if (bottomLeft > 0.0) {
    path.arcTo(QRectF(left, bottom - 2.0 * bottomLeft, 2.0 * bottomLeft, 2.0 * bottomLeft), 270.0,
               -90.0);
  }
  path.lineTo(left, top + topLeft);
  if (topLeft > 0.0) {
    path.arcTo(QRectF(left, top, 2.0 * topLeft, 2.0 * topLeft), 180.0, -90.0);
  }
  path.closeSubpath();
  return path;
}

QRectF joinedBorderRect(const QRect& bounds, qreal borderWidth, bool joinedLeft, bool joinedRight) {
  const qreal half = std::max<qreal>(0.0, borderWidth / 2.0);
  qreal leftInset = half + 0.5;
  qreal rightInset = half + 0.5;
  if (joinedLeft) {
    leftInset = half;
  }
  if (joinedRight) {
    rightInset = half;
  }
  return QRectF(bounds).adjusted(leftInset, half, -rightInset, -half);
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

QPixmap renderTintedIcon(const adqt::icons::IconRef& token, const QColor& color, int side,
                         qreal dpr, int rotationDegrees = 0) {
  if (!adqt::icons::isValid(token) || side <= 0) {
    return QPixmap();
  }
  adqt::icons::IconRef tinted = token;
  tinted = tinted.withColors(tinted.colors().withPrimary(color));
  if (rotationDegrees != 0) {
    QPixmap pixmap(QSize(qCeil(side * dpr), qCeil(side * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    const QRectF rect(0, 0, side, side);
    painter.translate(rect.center());
    painter.rotate(rotationDegrees);
    adqt::icons::paintIcon(&painter, tinted, QRectF(-side / 2.0, -side / 2.0, side, side));
    return pixmap;
  }
  return adqt::icons::renderIconPixmap(tinted, {QSize(side, side), dpr});
}

bool inputNumberValueChanged(const detail::InputNumberValueModel* model, const QVariant& before,
                             const QVariant& after) {
  if (!model) {
    return before.userType() != after.userType() || before != after;
  }
  return before.userType() != after.userType() || !model->equals(before, after);
}

QString inputNumberAccessibleName(const AdInputNumber* input) {
  if (!input) {
    return AdInputNumber::tr("Input number");
  }
  const QString explicitName = input->accessibleName().trimmed();
  return explicitName.isEmpty() ? AdInputNumber::tr("Input number") : explicitName;
}

QString inputNumberAccessibleValueText(const AdInputNumber* input) {
  if (!input) {
    return QString();
  }
  QString display = input->displayText().trimmed();
  if (!display.isEmpty()) {
    return display;
  }
  return input->exactValue();
}

QString inputNumberAccessibleDescription(const AdInputNumber* input) {
  if (!input) {
    return QString();
  }

  const bool managedDescription =
      input->property("_adqt_overlay_managed_accessible_description").toBool();
  QString explicitDescription = input->accessibleDescription().trimmed();
  if (!managedDescription && !explicitDescription.isEmpty()) {
    return explicitDescription;
  }

  QStringList parts;
  if (!input->hasValue()) {
    const QString placeholder = input->placeholderText().trimmed();
    if (!placeholder.isEmpty()) {
      parts.append(placeholder);
    }
  }

  const QString minimum = input->exactMinimum().trimmed();
  const QString maximum = input->exactMaximum().trimmed();
  if (!minimum.isEmpty() && !maximum.isEmpty()) {
    parts.append(AdInputNumber::tr("Range %1 to %2").arg(minimum, maximum));
  } else if (!minimum.isEmpty()) {
    parts.append(AdInputNumber::tr("Minimum %1").arg(minimum));
  } else if (!maximum.isEmpty()) {
    parts.append(AdInputNumber::tr("Maximum %1").arg(maximum));
  }

  return parts.join(QStringLiteral(", "));
}

QVariant inputNumberAccessibleCurrentValue(const AdInputNumber* input) {
  if (!input || !input->hasValue()) {
    return QVariant();
  }
  if (input->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
    return QVariant(input->exactValue());
  }
  return QVariant(input->value());
}

QVariant inputNumberAccessibleMinimumValue(const AdInputNumber* input) {
  if (!input) {
    return QVariant();
  }
  const QString minimum = input->exactMinimum();
  if (minimum.isEmpty()) {
    return QVariant();
  }
  if (input->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
    return QVariant(minimum);
  }
  return QVariant(input->minimum());
}

QVariant inputNumberAccessibleMaximumValue(const AdInputNumber* input) {
  if (!input) {
    return QVariant();
  }
  const QString maximum = input->exactMaximum();
  if (maximum.isEmpty()) {
    return QVariant();
  }
  if (input->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
    return QVariant(maximum);
  }
  return QVariant(input->maximum());
}

QVariant inputNumberAccessibleStepSize(const AdInputNumber* input) {
  if (!input) {
    return QVariant();
  }
  const QString step = input->exactSingleStep();
  if (step.isEmpty()) {
    return QVariant();
  }
  if (input->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
    return QVariant(step);
  }
  return QVariant(input->singleStep());
}

class AdInputNumberAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
 public:
  explicit AdInputNumberAccessible(AdInputNumber* input)
      : QAccessibleWidget(input, QAccessible::SpinBox) {}

  QString text(QAccessible::Text t) const override {
    const auto* input = qobject_cast<AdInputNumber*>(object());
    if (!input) {
      return QAccessibleWidget::text(t);
    }

    switch (t) {
      case QAccessible::Name:
        return inputNumberAccessibleName(input);
      case QAccessible::Description:
        return inputNumberAccessibleDescription(input);
      case QAccessible::Value:
        return inputNumberAccessibleValueText(input);
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

  QAccessible::State state() const override {
    QAccessible::State st = QAccessibleWidget::state();
    const auto* input = qobject_cast<AdInputNumber*>(object());
    if (input && input->readOnly()) {
      st.readOnly = true;
    }
    return st;
  }

  QVariant currentValue() const override {
    return inputNumberAccessibleCurrentValue(qobject_cast<AdInputNumber*>(object()));
  }

  void setCurrentValue(const QVariant& value) override {
    auto* input = qobject_cast<AdInputNumber*>(object());
    if (!input) {
      return;
    }
    if (input->valueMode() == AdInputNumber::ValueMode::ExactDecimal) {
      input->setExactValue(value.toString());
      return;
    }
    bool ok = false;
    const double number = value.toDouble(&ok);
    if (ok) {
      input->setValue(number);
    }
  }

  QVariant maximumValue() const override {
    return inputNumberAccessibleMaximumValue(qobject_cast<AdInputNumber*>(object()));
  }

  QVariant minimumValue() const override {
    return inputNumberAccessibleMinimumValue(qobject_cast<AdInputNumber*>(object()));
  }

  QVariant minimumStepSize() const override {
    return inputNumberAccessibleStepSize(qobject_cast<AdInputNumber*>(object()));
  }
};

QAccessibleInterface* inputNumberAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* input = qobject_cast<AdInputNumber*>(object)) {
    return new AdInputNumberAccessible(input);
  }
  return nullptr;
}

void ensureInputNumberAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(inputNumberAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

}  // namespace

class InputActionsBorderOverlay final : public QWidget {
 public:
  explicit InputActionsBorderOverlay(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAutoFillBackground(false);
  }

  void setBorderStyle(const QColor& border, qreal borderWidth, int radius, bool underlined) {
    const qreal normalizedBorderWidth = std::max<qreal>(0.0, borderWidth);
    const int normalizedRadius = std::max(0, radius);
    const bool changed = border_ != border ||
                         !qFuzzyCompare(borderWidth_ + 1.0, normalizedBorderWidth + 1.0) ||
                         radius_ != normalizedRadius || underlined_ != underlined;
    border_ = border;
    borderWidth_ = normalizedBorderWidth;
    radius_ = normalizedRadius;
    underlined_ = underlined;
    if (changed) {
      update();
    }
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    if (!rect().isValid()) {
      return;
    }

    const qreal borderWidth = std::max<qreal>(0.0, borderWidth_);
    if (borderWidth <= 0.0 || border_.alpha() <= 0) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    const QRectF rawBorderRect = joinedBorderRect(rect(), borderWidth, true, false);
    if (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 || rawBorderRect.height() <= 0.0) {
      return;
    }

    const QRectF borderRect = snapRectToDevicePixels(rawBorderRect, dpr);
    if (underlined_) {
      QPen underlinePen(border_, borderWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
      painter.setPen(underlinePen);
      painter.setBrush(Qt::NoBrush);
      const qreal underlineY = snapToDevicePixelCoord(borderRect.bottom(), dpr);
      painter.drawLine(QPointF(rect().left(), underlineY), QPointF(rect().right(), underlineY));
      return;
    }

    const qreal left = borderRect.left();
    const qreal top = borderRect.top();
    const qreal right = borderRect.right();
    const qreal bottom = borderRect.bottom();
    const qreal cornerRadius = std::max<qreal>(0.0, radius_);
    QPen borderPen(border_, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(left, top), QPointF(right - cornerRadius, top));
    painter.drawLine(QPointF(right, top + cornerRadius), QPointF(right, bottom - cornerRadius));
    painter.drawLine(QPointF(right - cornerRadius, bottom), QPointF(left, bottom));
    if (cornerRadius > 0.0) {
      painter.drawArc(
          QRectF(right - cornerRadius * 2.0, top, cornerRadius * 2.0, cornerRadius * 2.0), 0,
          90 * 16);
      painter.drawArc(QRectF(right - cornerRadius * 2.0, bottom - cornerRadius * 2.0,
                             cornerRadius * 2.0, cornerRadius * 2.0),
                      270 * 16, 90 * 16);
    }
  }

 private:
  QColor border_;
  qreal borderWidth_ = 0.0;
  int radius_ = 0;
  bool underlined_ = false;
};

struct StepButtonPaintStyle {
  QColor normalBackground = QColor(Qt::transparent);
  QColor hoverBackground = QColor(Qt::transparent);
  QColor pressedBackground = QColor(Qt::transparent);
  QColor disabledBackground = QColor(Qt::transparent);
  QColor borderColor = QColor(Qt::transparent);
  qreal borderWidth = 0.0;
  qreal topLeftRadius = 0.0;
  qreal topRightRadius = 0.0;
  qreal bottomRightRadius = 0.0;
  qreal bottomLeftRadius = 0.0;
  bool borderLeft = false;
  bool borderTop = false;
  bool borderRight = false;
  bool borderBottom = false;
};

class StepControlButton final : public QToolButton {
 public:
  explicit StepControlButton(QWidget* parent = nullptr) : QToolButton(parent) {
    setAutoFillBackground(false);
    setAttribute(Qt::WA_Hover, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setMouseTracking(true);
    setAutoRaise(false);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
  }

  void setPaintStyle(const StepButtonPaintStyle& style) {
    paintStyle_ = style;
    setProperty("ad-normal-background", style.normalBackground);
    setProperty("ad-hover-background", style.hoverBackground);
    setProperty("ad-pressed-background", style.pressedBackground);
    setProperty("ad-disabled-background", style.disabledBackground);
    setProperty("ad-top-left-radius", style.topLeftRadius);
    setProperty("ad-top-right-radius", style.topRightRadius);
    setProperty("ad-bottom-right-radius", style.bottomRightRadius);
    setProperty("ad-bottom-left-radius", style.bottomLeftRadius);
    update();
  }

 protected:
  void enterEvent(QEnterEvent* event) override {
    hovered_ = true;
    update();
    QToolButton::enterEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    hovered_ = false;
    update();
    QToolButton::leaveEvent(event);
  }

  void changeEvent(QEvent* event) override {
    QToolButton::changeEvent(event);
    if (event && event->type() == QEvent::EnabledChange) {
      update();
    }
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor background = isEnabled() ? paintStyle_.normalBackground : paintStyle_.disabledBackground;
    if (isEnabled() && isDown()) {
      background = paintStyle_.pressedBackground.isValid() ? paintStyle_.pressedBackground
                                                           : paintStyle_.hoverBackground;
    } else if (isEnabled() && hovered_) {
      background = paintStyle_.hoverBackground.isValid() ? paintStyle_.hoverBackground
                                                         : paintStyle_.normalBackground;
    }

    const QRectF fillRect(rect());
    if (background.isValid() && background.alpha() > 0 && fillRect.isValid() &&
        fillRect.width() > 0.0 && fillRect.height() > 0.0) {
      const QPainterPath fillPath =
          roundedRectPath(fillRect, paintStyle_.topLeftRadius, paintStyle_.topRightRadius,
                          paintStyle_.bottomRightRadius, paintStyle_.bottomLeftRadius);
      painter.fillPath(fillPath, background);
    }

    const qreal borderWidth = std::max<qreal>(0.0, paintStyle_.borderWidth);
    if (borderWidth > 0.0 && paintStyle_.borderColor.alpha() > 0) {
      QRectF borderRect = QRectF(rect());
      const qreal inset = borderWidth * 0.5;
      borderRect.adjust(inset, inset, -inset, -inset);
      const qreal dpr =
          painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
      borderRect = snapRectToDevicePixels(borderRect, dpr);
      if (borderRect.isValid() && borderRect.width() > 0.0 && borderRect.height() > 0.0) {
        const qreal left = borderRect.left();
        const qreal top = borderRect.top();
        const qreal right = borderRect.right();
        const qreal bottom = borderRect.bottom();
        const qreal tl = std::max<qreal>(0.0, paintStyle_.topLeftRadius);
        const qreal tr = std::max<qreal>(0.0, paintStyle_.topRightRadius);
        const qreal br = std::max<qreal>(0.0, paintStyle_.bottomRightRadius);
        const qreal bl = std::max<qreal>(0.0, paintStyle_.bottomLeftRadius);

        QPen pen(paintStyle_.borderColor, borderWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        if (paintStyle_.borderTop) {
          painter.drawLine(QPointF(left + tl, top), QPointF(right - tr, top));
        }
        if (paintStyle_.borderRight) {
          painter.drawLine(QPointF(right, top + tr), QPointF(right, bottom - br));
        }
        if (paintStyle_.borderBottom) {
          painter.drawLine(QPointF(right - br, bottom), QPointF(left + bl, bottom));
        }
        if (paintStyle_.borderLeft) {
          painter.drawLine(QPointF(left, bottom - bl), QPointF(left, top + tl));
        }

        if (paintStyle_.borderTop && paintStyle_.borderRight && tr > 0.0) {
          painter.drawArc(QRectF(right - tr * 2.0, top, tr * 2.0, tr * 2.0), 0, 90 * 16);
        }
        if (paintStyle_.borderRight && paintStyle_.borderBottom && br > 0.0) {
          painter.drawArc(QRectF(right - br * 2.0, bottom - br * 2.0, br * 2.0, br * 2.0), 270 * 16,
                          90 * 16);
        }
        if (paintStyle_.borderBottom && paintStyle_.borderLeft && bl > 0.0) {
          painter.drawArc(QRectF(left, bottom - bl * 2.0, bl * 2.0, bl * 2.0), 180 * 16, 90 * 16);
        }
        if (paintStyle_.borderLeft && paintStyle_.borderTop && tl > 0.0) {
          painter.drawArc(QRectF(left, top, tl * 2.0, tl * 2.0), 90 * 16, 90 * 16);
        }
      }
    }

    const QIcon currentIcon = icon();
    if (!currentIcon.isNull()) {
      const QIcon::Mode mode =
          !isEnabled() ? QIcon::Disabled : ((hovered_ || isDown()) ? QIcon::Active : QIcon::Normal);
      const QSize logicalSize = iconSize().isValid() ? iconSize() : QSize(12, 12);
      const QPixmap pixmap = currentIcon.pixmap(logicalSize, mode, QIcon::Off);
      if (!pixmap.isNull()) {
        const QSize pixmapSize = pixmap.deviceIndependentSize().toSize();
        const QPoint topLeft((width() - pixmapSize.width()) / 2,
                             (height() - pixmapSize.height()) / 2);
        painter.drawPixmap(topLeft, pixmap);
      }
    }
  }

 private:
  bool hovered_ = false;
  StepButtonPaintStyle paintStyle_;
};

struct AdInputNumber::ResolvedVisualState {
  StyleContext context;
  detail::InputNumberVisualStyle style;
  bool interactiveControls = false;
  bool outOfRange = false;
  bool inputShellBorderCanBeVisible = false;
  QColor background;
  QColor border;
  int borderInset = 0;
  int horizontalPadding = 0;
  int handleWidth = 0;
  int reservedHandleWidth = 0;
  int visibleHandleWidth = 0;
  int controlHeight = 0;
  int leftInset = 0;
  int rightInset = 0;
  int verticalInset = 0;
  int lineEditVerticalInset = 0;
  int inputActionSafetyInset = 0;
  int inputActionsInset = 0;
  int inputActionCornerRadius = 0;
  int rightInputActionCornerRadius = 0;
  int splitActionWidth = 0;
  int splitActionHeight = 0;
};

AdInputNumber::AdInputNumber(QWidget* parent)
    : QAbstractSpinBox(parent), valueModel_(std::make_unique<detail::InputNumberValueModel>()) {
  ensureInputNumberAccessibleFactoryInstalled();
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  setFrame(false);
  setButtonSymbols(QAbstractSpinBox::NoButtons);
  setKeyboardTracking(false);
  setAccelerated(true);

  auto* lineEdit = new QLineEdit(this);
  lineEdit->setFrame(false);
  lineEdit->setAutoFillBackground(false);
  lineEdit->setAttribute(Qt::WA_TranslucentBackground, true);
  lineEdit->setAttribute(Qt::WA_NoSystemBackground, true);
  lineEdit->setClearButtonEnabled(false);
  lineEdit->setAlignment(textAlignment_);
  setLineEdit(lineEdit);
  editor_ = lineEdit;
  editor_->installEventFilter(this);
  QAbstractSpinBox::setAlignment(textAlignment_);

  prefixIconLabel_ = new QLabel(this);
  prefixIconLabel_->setAlignment(Qt::AlignCenter);
  prefixIconLabel_->setVisible(false);
  prefixIconLabel_->installEventFilter(this);

  prefixLabel_ = new QLabel(this);
  prefixLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  prefixLabel_->setVisible(false);
  prefixLabel_->installEventFilter(this);

  suffixLabel_ = new QLabel(this);
  suffixLabel_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
  suffixLabel_->setVisible(false);
  suffixLabel_->installEventFilter(this);

  suffixIconLabel_ = new QLabel(this);
  suffixIconLabel_->setAlignment(Qt::AlignCenter);
  suffixIconLabel_->setVisible(false);
  suffixIconLabel_->installEventFilter(this);

  inputActionsWidget_ = new QWidget(this);
  inputActionsWidget_->setObjectName(QStringLiteral("ad-input-number-actions"));
  inputActionsWidget_->setAutoFillBackground(true);
  inputActionsWidget_->setAttribute(Qt::WA_StyledBackground, true);
  inputActionsWidget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  inputActionsWidget_->installEventFilter(this);

  inputActionsBorderOverlay_ = new InputActionsBorderOverlay(this);
  inputActionsBorderOverlay_->hide();

  inputActionsLayout_ = new QVBoxLayout(inputActionsWidget_);
  inputActionsLayout_->setContentsMargins(0, 0, 0, 0);
  inputActionsLayout_->setSpacing(0);

  inputUpButton_ = new StepControlButton(inputActionsWidget_);
  inputDownButton_ = new StepControlButton(inputActionsWidget_);
  inputUpButton_->setObjectName(QStringLiteral("ad-input-number-up"));
  inputDownButton_->setObjectName(QStringLiteral("ad-input-number-down"));
  inputUpButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
  inputDownButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
  inputUpButton_->setMinimumHeight(0);
  inputDownButton_->setMinimumHeight(0);
  inputUpButton_->setAutoRepeat(true);
  inputDownButton_->setAutoRepeat(true);
  inputUpButton_->setFocusPolicy(Qt::NoFocus);
  inputDownButton_->setFocusPolicy(Qt::NoFocus);
  inputUpButton_->setAccessibleName(tr("Increase value"));
  inputDownButton_->setAccessibleName(tr("Decrease value"));
  inputUpButton_->installEventFilter(this);
  inputDownButton_->installEventFilter(this);
  inputActionsLayout_->addWidget(inputUpButton_, 1);
  inputActionsLayout_->addWidget(inputDownButton_, 1);

  splitDownButton_ = new StepControlButton(this);
  splitUpButton_ = new StepControlButton(this);
  splitDownButton_->setObjectName(QStringLiteral("ad-input-number-spinner-down"));
  splitUpButton_->setObjectName(QStringLiteral("ad-input-number-spinner-up"));
  splitDownButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  splitUpButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
  splitDownButton_->setAutoRepeat(true);
  splitUpButton_->setAutoRepeat(true);
  splitDownButton_->setFocusPolicy(Qt::NoFocus);
  splitUpButton_->setFocusPolicy(Qt::NoFocus);
  splitDownButton_->setAccessibleName(tr("Decrease value"));
  splitUpButton_->setAccessibleName(tr("Increase value"));
  splitDownButton_->installEventFilter(this);
  splitUpButton_->installEventFilter(this);

  connect(inputUpButton_, &QToolButton::clicked, this,
          [this]() { performStep(+1, StepEmitter::Handler); });
  connect(inputDownButton_, &QToolButton::clicked, this,
          [this]() { performStep(-1, StepEmitter::Handler); });
  connect(splitUpButton_, &QToolButton::clicked, this,
          [this]() { performStep(+1, StepEmitter::Handler); });
  connect(splitDownButton_, &QToolButton::clicked, this,
          [this]() { performStep(-1, StepEmitter::Handler); });

  connect(editor_, &QLineEdit::textEdited, this, [this](const QString& text) {
    if (internalTextUpdate_) {
      return;
    }

    userTyping_ = true;
    emitTextChangedIfNeeded(text);
    const auto parser = [this](const QString& source) {
      return textPolicy_ ? textPolicy_->parseText(source) : source;
    };
    const QVariant parsed =
        valueModel_ ? valueModel_->parseInputText(text, parser, true) : QVariant();
    if (keyboardTracking() && (parsed.isValid() || text.trimmed().isEmpty())) {
      setCommittedValueInternal(parsed, true, true, false);
    } else {
      refreshVisualState(false);
    }
  });

  // QAbstractSpinBox consumes Return before the line edit can finish editing.
  // Commit first so external editingFinished slots observe the entered value.
  connect(this, &QAbstractSpinBox::editingFinished, this, [this]() {
    userTyping_ = false;
    commitFromEditor();
  });

  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() {
            refreshVisualState(true);
            syncAccessibleState();
          });

  syncValueModelConfig();
  singleStep_ = valueModel_->normalizeSingleStepValue(singleStep_);
  syncValueModelConfig();

  updateEditorTextFromValue(false);
  updateAffixVisual();
  updateActionVisibility();
  refreshVisualState(false);
  syncAccessibleState();
}

AdInputNumber::~AdInputNumber() {
  stopInteractionFocusForOwner(this);
  editor_ = nullptr;
  prefixIconLabel_ = nullptr;
  prefixLabel_ = nullptr;
  suffixLabel_ = nullptr;
  suffixIconLabel_ = nullptr;
  inputActionsWidget_ = nullptr;
  inputActionsBorderOverlay_ = nullptr;
  inputActionsLayout_ = nullptr;
  inputUpButton_ = nullptr;
  inputDownButton_ = nullptr;
  splitDownButton_ = nullptr;
  splitUpButton_ = nullptr;
}

AdInputNumber::ControlSize AdInputNumber::controlSize() const { return controlSize_; }

void AdInputNumber::setControlSize(ControlSize value) {
  if (controlSize_ == value) {
    return;
  }
  controlSize_ = value;
  refreshVisualState(true);
  emit controlSizeChanged(controlSize_);
}

AdInputNumber::Variant AdInputNumber::variant() const { return variant_; }

void AdInputNumber::setVariant(Variant value) {
  if (variant_ == value) {
    return;
  }
  variant_ = value;
  refreshVisualState(false);
  emit variantChanged(variant_);
}

AdInputNumber::Status AdInputNumber::status() const { return status_; }

void AdInputNumber::setStatus(Status value) {
  if (status_ == value) {
    return;
  }
  status_ = value;
  refreshVisualState(false);
  emit statusChanged(status_);
}

AdInputNumber::StepButtonLayout AdInputNumber::stepButtonLayout() const {
  return stepButtonLayout_;
}

void AdInputNumber::setStepButtonLayout(StepButtonLayout value) {
  if (stepButtonLayout_ == value) {
    return;
  }
  stepButtonLayout_ = value;
  if (!customTextAlignment_) {
    const Qt::Alignment nextAlignment = defaultTextAlignmentForLayout(stepButtonLayout_);
    if (textAlignment_ != nextAlignment) {
      textAlignment_ = nextAlignment;
      emit textAlignmentChanged(textAlignment_);
    }
  }
  updateAffixVisual();
  updateActionVisibility();
  refreshVisualState(true);
  syncAccessibleState();
  emit stepButtonLayoutChanged(stepButtonLayout_);
}

bool AdInputNumber::readOnly() const { return readOnly_; }

void AdInputNumber::setReadOnly(bool value) {
  if (readOnly_ == value) {
    return;
  }
  readOnly_ = value;
  QAbstractSpinBox::setReadOnly(readOnly_);
  updateReadOnlyState();
  updateActionVisibility();
  refreshVisualState(true);
  emit readOnlyChanged(readOnly_);
}

bool AdInputNumber::stepButtonsVisible() const { return stepButtonsVisible_; }

void AdInputNumber::setStepButtonsVisible(bool value) {
  if (stepButtonsVisible_ == value) {
    return;
  }
  stepButtonsVisible_ = value;
  updateActionVisibility();
  refreshVisualState(true);
  emit stepButtonsVisibleChanged(stepButtonsVisible_);
}

bool AdInputNumber::stepKeysEnabled() const { return stepKeysEnabled_; }

void AdInputNumber::setStepKeysEnabled(bool value) {
  if (stepKeysEnabled_ == value) {
    return;
  }
  stepKeysEnabled_ = value;
  emit stepKeysEnabledChanged(stepKeysEnabled_);
}

bool AdInputNumber::wheelStepEnabled() const { return wheelStepEnabled_; }

void AdInputNumber::setWheelStepEnabled(bool value) {
  if (wheelStepEnabled_ == value) {
    return;
  }
  wheelStepEnabled_ = value;
  emit wheelStepEnabledChanged(wheelStepEnabled_);
}

void AdInputNumber::setKeyboardTracking(bool value) {
  if (QAbstractSpinBox::keyboardTracking() == value) {
    return;
  }
  QAbstractSpinBox::setKeyboardTracking(value);
  emit keyboardTrackingChanged(value);
}

AdInputNumber::ValueMode AdInputNumber::valueMode() const { return valueMode_; }

void AdInputNumber::setValueMode(ValueMode value) {
  if (valueMode_ == value) {
    return;
  }

  const bool previousHasValue = hasValue();
  const QVariant previousValue = value_;
  const QVariant previousMinimum = minimum_;
  const QVariant previousMaximum = maximum_;
  const QVariant previousStep = singleStep_;

  valueMode_ = value;
  syncValueModelConfig();

  minimum_ = valueModel_->normalizeBoundaryValue(minimum_);
  maximum_ = valueModel_->normalizeBoundaryValue(maximum_);
  if (minimum_.isValid() && maximum_.isValid() && valueModel_->greaterThan(minimum_, maximum_)) {
    maximum_ = minimum_;
  }
  singleStep_ = valueModel_->normalizeSingleStepValue(singleStep_);
  value_ = valueModel_->normalizeInputValue(value_);
  value_ = valueModel_->committedValue(value_);

  syncValueModelConfig();
  updateEditorTextFromValue(false);
  refreshVisualState(false);

  emit valueModeChanged(valueMode_);
  if (inputNumberValueChanged(valueModel_.get(), previousMinimum, minimum_)) {
    emit minimumChanged(minimum());
    emit exactMinimumChanged(exactMinimum());
  }
  if (inputNumberValueChanged(valueModel_.get(), previousMaximum, maximum_)) {
    emit maximumChanged(maximum());
    emit exactMaximumChanged(exactMaximum());
  }
  if (inputNumberValueChanged(valueModel_.get(), previousStep, singleStep_)) {
    emit singleStepChanged(singleStep());
    emit exactSingleStepChanged(exactSingleStep());
  }
  if (inputNumberValueChanged(valueModel_.get(), previousValue, value_)) {
    emitCommittedValueSignals(previousValue, previousHasValue);
  }
  syncAccessibleState();
  notifyAccessibleDescriptionChange();
}

AdInputNumber::RangeMode AdInputNumber::rangeMode() const { return rangeMode_; }

void AdInputNumber::setRangeMode(RangeMode value) {
  if (rangeMode_ == value) {
    return;
  }

  const bool previousHasValue = hasValue();
  const QVariant previousValue = value_;
  rangeMode_ = value;
  syncValueModelConfig();
  if (rangeMode_ == RangeMode::Strict) {
    setCommittedValueInternal(value_, false, false, true);
  } else {
    updateEditorTextFromValue(false);
    refreshVisualState(false);
    syncAccessibleState();
  }

  emit rangeModeChanged(rangeMode_);
  if (inputNumberValueChanged(valueModel_.get(), previousValue, value_)) {
    emitCommittedValueSignals(previousValue, previousHasValue);
  }
}

bool AdInputNumber::hasValue() const { return value_.isValid(); }

double AdInputNumber::value() const { return value_.isValid() ? value_.toDouble() : 0.0; }

void AdInputNumber::setValue(double value) {
  setCommittedValueInternal(QVariant(value), true, false, false);
}

QString AdInputNumber::exactValue() const { return canonicalTextForValue(value_); }

void AdInputNumber::setExactValue(const QString& value) {
  setCommittedValueInternal(QVariant(value), true, false, false);
}

double AdInputNumber::minimum() const { return minimum_.isValid() ? minimum_.toDouble() : 0.0; }

void AdInputNumber::setMinimum(double value) {
  const QVariant normalized = normalizeBoundaryValue(QVariant(value));
  QVariant nextMaximum = maximum_;
  if (normalized.isValid() && nextMaximum.isValid() && valueModel_ &&
      valueModel_->greaterThan(normalized, nextMaximum)) {
    nextMaximum = normalized;
  }

  applyRangeValues(normalized, nextMaximum);
}

double AdInputNumber::maximum() const { return maximum_.isValid() ? maximum_.toDouble() : 0.0; }

void AdInputNumber::setMaximum(double value) {
  const QVariant normalized = normalizeBoundaryValue(QVariant(value));
  QVariant nextMinimum = minimum_;
  if (normalized.isValid() && nextMinimum.isValid() && valueModel_ &&
      valueModel_->lessThan(normalized, nextMinimum)) {
    nextMinimum = normalized;
  }

  applyRangeValues(nextMinimum, normalized);
}

void AdInputNumber::setRange(double minimumValue, double maximumValue) {
  const QVariant normalizedMinimum = normalizeBoundaryValue(QVariant(minimumValue));
  QVariant normalizedMaximum = normalizeBoundaryValue(QVariant(maximumValue));
  if (normalizedMinimum.isValid() && normalizedMaximum.isValid() && valueModel_ &&
      valueModel_->greaterThan(normalizedMinimum, normalizedMaximum)) {
    normalizedMaximum = normalizedMinimum;
  }

  applyRangeValues(normalizedMinimum, normalizedMaximum);
}

double AdInputNumber::singleStep() const {
  return singleStep_.isValid() ? singleStep_.toDouble() : 1.0;
}

void AdInputNumber::setSingleStep(double value) {
  applySingleStepValue(normalizeSingleStepValue(QVariant(value)));
}

int AdInputNumber::decimals() const { return decimals_; }

void AdInputNumber::setDecimals(int value) {
  const int normalized = std::max(-1, value);
  if (decimals_ == normalized) {
    return;
  }

  const bool previousHasValue = hasValue();
  const QVariant previousValue = value_;
  decimals_ = normalized;
  syncValueModelConfig();
  value_ = valueModel_->committedValue(value_);
  updateEditorTextFromValue(false);
  refreshVisualState(false);

  emit decimalsChanged(decimals_);
  if (inputNumberValueChanged(valueModel_.get(), previousValue, value_)) {
    emitCommittedValueSignals(previousValue, previousHasValue);
  } else {
    syncAccessibleState();
  }
}

QString AdInputNumber::exactMinimum() const { return canonicalTextForValue(minimum_); }

void AdInputNumber::setExactMinimum(const QString& value) {
  const QVariant normalized = normalizeBoundaryValue(QVariant(value));
  QVariant nextMaximum = maximum_;
  if (normalized.isValid() && nextMaximum.isValid() && valueModel_ &&
      valueModel_->greaterThan(normalized, nextMaximum)) {
    nextMaximum = normalized;
  }

  applyRangeValues(normalized, nextMaximum);
}

QString AdInputNumber::exactMaximum() const { return canonicalTextForValue(maximum_); }

void AdInputNumber::setExactMaximum(const QString& value) {
  const QVariant normalized = normalizeBoundaryValue(QVariant(value));
  QVariant nextMinimum = minimum_;
  if (normalized.isValid() && nextMinimum.isValid() && valueModel_ &&
      valueModel_->lessThan(normalized, nextMinimum)) {
    nextMinimum = normalized;
  }

  applyRangeValues(nextMinimum, normalized);
}

void AdInputNumber::setExactRange(const QString& minimumValue, const QString& maximumValue) {
  const QVariant normalizedMinimum = normalizeBoundaryValue(QVariant(minimumValue));
  QVariant normalizedMaximum = normalizeBoundaryValue(QVariant(maximumValue));
  if (normalizedMinimum.isValid() && normalizedMaximum.isValid() && valueModel_ &&
      valueModel_->greaterThan(normalizedMinimum, normalizedMaximum)) {
    normalizedMaximum = normalizedMinimum;
  }

  applyRangeValues(normalizedMinimum, normalizedMaximum);
}

QString AdInputNumber::exactSingleStep() const { return canonicalTextForValue(singleStep_); }

void AdInputNumber::setExactSingleStep(const QString& value) {
  applySingleStepValue(normalizeSingleStepValue(QVariant(value)));
}

QString AdInputNumber::placeholderText() const {
  return editor_ ? editor_->placeholderText() : QString();
}

void AdInputNumber::setPlaceholderText(const QString& value) {
  if (!editor_ || editor_->placeholderText() == value) {
    return;
  }
  editor_->setPlaceholderText(value);
  refreshVisualState(false);
  syncAccessibleState();
  notifyAccessibleDescriptionChange();
  emit placeholderTextChanged(value);
}

QString AdInputNumber::prefixText() const { return prefixText_; }

void AdInputNumber::setPrefixText(const QString& value) {
  if (prefixText_ == value) {
    return;
  }
  prefixText_ = value;
  updateAffixVisual();
  refreshVisualState(true);
  emit prefixTextChanged(prefixText_);
}

QString AdInputNumber::suffixText() const { return suffixText_; }

void AdInputNumber::setSuffixText(const QString& value) {
  if (suffixText_ == value) {
    return;
  }
  suffixText_ = value;
  updateAffixVisual();
  refreshVisualState(true);
  emit suffixTextChanged(suffixText_);
}

Qt::Alignment AdInputNumber::textAlignment() const { return textAlignment_; }

void AdInputNumber::setTextAlignment(Qt::Alignment value) {
  if (customTextAlignment_ && textAlignment_ == value) {
    return;
  }
  textAlignment_ = value;
  customTextAlignment_ = true;
  if (editor_) {
    editor_->setAlignment(textAlignment_);
  }
  QAbstractSpinBox::setAlignment(textAlignment_);
  emit textAlignmentChanged(textAlignment_);
}

bool AdInputNumber::joinedLeft() const { return joinedLeft_; }

void AdInputNumber::setJoinedLeft(bool value) {
  if (joinedLeft_ == value) {
    return;
  }
  joinedLeft_ = value;
  bumpJoinedZOrder();
  refreshVisualState(false);
  emit joinedLeftChanged(joinedLeft_);
}

bool AdInputNumber::joinedRight() const { return joinedRight_; }

void AdInputNumber::setJoinedRight(bool value) {
  if (joinedRight_ == value) {
    return;
  }
  joinedRight_ = value;
  bumpJoinedZOrder();
  refreshVisualState(false);
  emit joinedRightChanged(joinedRight_);
}

adqt::icons::IconRef AdInputNumber::prefixIconRef() const { return prefixIconRef_; }

void AdInputNumber::setPrefixIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(prefixIconRef_, token)) {
    return;
  }
  prefixIconRef_ = token;
  updateAffixVisual();
  refreshVisualState(true);
  emit prefixIconRefChanged(prefixIconRef_);
}

adqt::icons::IconRef AdInputNumber::suffixIconRef() const { return suffixIconRef_; }

void AdInputNumber::setSuffixIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(suffixIconRef_, token)) {
    return;
  }
  suffixIconRef_ = token;
  updateSuffixSpinnerState();
  updateAffixVisual();
  refreshVisualState(true);
  emit suffixIconRefChanged(suffixIconRef_);
}

adqt::icons::IconRef AdInputNumber::upIconRef() const { return upIconRef_; }

void AdInputNumber::setUpIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(upIconRef_, token)) {
    return;
  }
  upIconRef_ = token;
  refreshVisualState(false);
  emit upIconRefChanged(upIconRef_);
}

adqt::icons::IconRef AdInputNumber::downIconRef() const { return downIconRef_; }

void AdInputNumber::setDownIconRef(const adqt::icons::IconRef& token) {
  if (iconRefsEqual(downIconRef_, token)) {
    return;
  }
  downIconRef_ = token;
  refreshVisualState(false);
  emit downIconRefChanged(downIconRef_);
}

AdInputNumberTextPolicy* AdInputNumber::textPolicy() const { return textPolicy_; }

void AdInputNumber::setTextPolicy(AdInputNumberTextPolicy* value) {
  if (textPolicy_ == value) {
    return;
  }
  textPolicy_ = value;
  updateEditorTextFromValue(false);
  refreshVisualState(true);
  syncAccessibleState();
  notifyAccessibleValueChange();
  emit textPolicyChanged(textPolicy_);
}

QString AdInputNumber::displayText() const { return editor_ ? editor_->text() : QString(); }

void AdInputNumber::clear() {
  userTyping_ = false;
  setCommittedValueInternal(QVariant(), true, false, false);
  if (editor_ && !editor_->text().isEmpty()) {
    QSignalBlocker blocker(editor_);
    internalTextUpdate_ = true;
    editor_->clear();
    internalTextUpdate_ = false;
    emitTextChangedIfNeeded(QString());
    refreshVisualState(false);
  }
}

AdInputNumber::AppearanceOverrides AdInputNumber::appearanceOverrides() const {
  return appearanceOverrides_;
}

void AdInputNumber::setAppearanceOverrides(const AppearanceOverrides& overrides) {
  appearanceOverrides_ = overrides;
  refreshVisualState(true);
  emit appearanceOverridesChanged();
}

void AdInputNumber::resetAppearanceOverrides() {
  appearanceOverrides_ = AppearanceOverrides();
  refreshVisualState(true);
  emit appearanceOverridesChanged();
}

QSize AdInputNumber::sizeHint() const {
  const ResolvedVisualState state = resolvedVisualState();
  QFontMetrics fm(state.style.metrics.font);

  int widthHint = std::max(state.style.metrics.width, state.style.metrics.height * 3);
  QString basis = editor_ ? editor_->text() : displayTextForValue(value_, false, QString());
  if (basis.isEmpty()) {
    basis = placeholderText();
  }
  widthHint = std::max(widthHint,
                       fm.horizontalAdvance(basis.isEmpty() ? QStringLiteral("000") : basis) + 96);

  const int groupGap = affixPadding(state.style);
  const int itemGap = affixItemGap(state.style);
  bool hasLeadingAffix = false;
  const auto appendLeadingWidth = [&](int width) {
    if (width <= 0) {
      return;
    }
    if (hasLeadingAffix) {
      widthHint += itemGap;
    }
    widthHint += width;
    hasLeadingAffix = true;
  };
  if (adqt::icons::isValid(prefixIconRef_)) {
    appendLeadingWidth(std::max(8, state.style.metrics.iconSize));
  }
  if (!prefixText_.isEmpty()) {
    appendLeadingWidth(fm.horizontalAdvance(prefixText_));
  }
  if (hasLeadingAffix) {
    widthHint += groupGap;
  }

  bool hasTrailingAffix = false;
  const auto appendTrailingWidth = [&](int width) {
    if (width <= 0) {
      return;
    }
    if (hasTrailingAffix) {
      widthHint += itemGap;
    }
    widthHint += width;
    hasTrailingAffix = true;
  };
  if (adqt::icons::isValid(suffixIconRef_)) {
    appendTrailingWidth(std::max(8, state.style.metrics.iconSize));
  }
  if (!suffixText_.isEmpty()) {
    appendTrailingWidth(fm.horizontalAdvance(suffixText_));
  }
  if (hasTrailingAffix) {
    widthHint += groupGap;
  }

  if (stepButtonLayout_ == StepButtonLayout::Split && stepButtonsVisible_) {
    widthHint += std::max(20, state.splitActionWidth) * 2;
  } else if (stepButtonLayout_ == StepButtonLayout::Compact) {
    widthHint += state.reservedHandleWidth;
  }

  return QSize(widthHint, state.controlHeight);
}

QSize AdInputNumber::minimumSizeHint() const {
  const QSize hint = sizeHint();
  if (property("ad-flex-min-width-zero").toBool()) {
    return QSize(0, hint.height());
  }
  return QSize(std::min(96, hint.width()), hint.height());
}

void AdInputNumber::focusEditor(FocusSelection selection) {
  if (!editor_) {
    return;
  }

  editor_->setFocus(Qt::OtherFocusReason);
  if (joinedLeft_ || joinedRight_) {
    raise();
  }

  const QString text = editor_->text();
  switch (selection) {
    case FocusSelection::Start:
      editor_->setCursorPosition(0);
      break;
    case FocusSelection::End:
      editor_->setCursorPosition(static_cast<int>(text.size()));
      break;
    case FocusSelection::SelectAll:
      editor_->selectAll();
      break;
    case FocusSelection::Preserve:
    default:
      break;
  }

  refreshVisualState(false);
  QTimer::singleShot(0, this, [this]() {
    if (!editor_) {
      return;
    }
    const bool effectiveFocused = hasFocus() || editor_->hasFocus();
    if (focused_ != effectiveFocused) {
      focused_ = effectiveFocused;
    }
    refreshVisualState(false);
  });
}

void AdInputNumber::blurInput() {
  if (editor_) {
    editor_->clearFocus();
  }
}

AdInputNumber::StyleContext AdInputNumber::buildStyleContext(bool interactiveControls,
                                                             const QVariant& visualValue) const {
  StyleContext context;
  context.controlSize = controlSize_;
  context.variant = variant_;
  context.status = status_;
  context.stepButtonLayout = stepButtonLayout_;
  context.valueMode = valueMode_;
  context.disabled = !isEnabled();
  context.readOnly = readOnly_;
  const bool effectiveFocused = focused_ || hasFocus() || (editor_ && editor_->hasFocus());
  context.focused = effectiveFocused;
  context.hovered = hovered_;
  context.stepButtonsVisible = interactiveControls;
  context.outOfRange = valueModel_ ? valueModel_->isOutOfRange(visualValue) : false;
  return context;
}

AdInputNumber::ResolvedVisualState AdInputNumber::resolvedVisualState() const {
  ResolvedVisualState state;
  const QVariant stateValue = visualValue();
  state.interactiveControls = hasInteractiveControls(stepButtonsVisible_, readOnly_, !isEnabled());
  state.context = buildStyleContext(state.interactiveControls, stateValue);

  detail::InputNumberStyleInput styleInput;
  styleInput.controlSize = controlSize_;
  styleInput.variant = variant_;
  styleInput.status = status_;
  styleInput.stepButtonLayout = stepButtonLayout_;
  styleInput.valueMode = valueMode_;
  styleInput.disabled = !isEnabled();
  styleInput.readOnly = readOnly_;
  styleInput.focused = state.context.focused;
  styleInput.hovered = hovered_;
  styleInput.stepButtonsVisible = state.interactiveControls;
  styleInput.outOfRange = state.context.outOfRange;
  styleInput.baseFont = font();
  styleInput.appearanceOverrides = appearanceOverrides_;
  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this);
  state.style = detail::resolveInputNumberVisualStyle(styleInput, resolvedTheme);
  state.outOfRange = state.context.outOfRange;

  state.background = state.style.selectorBg;
  state.border = state.style.selectorBorderColor;
  if (isEnabled()) {
    if (state.context.focused) {
      state.background = state.style.selectorActiveBg;
      state.border = state.style.selectorActiveBorderColor;
    } else if (hovered_) {
      state.background = state.style.selectorHoverBg;
      state.border = state.style.selectorHoverBorderColor;
    }
  }

  if (variant_ == Variant::Filled && state.background.isValid() && state.background.alpha() < 255) {
    const auto map = adqt::theme::ThemeManager::instance().resolveTheme(this);
    const QColor containerBg = parseThemeColor(map.colorBgContainer, QColor("#ffffff"));
    state.background = compositeOn(state.background, containerBg);
  }

  state.inputShellBorderCanBeVisible = state.style.underlined ||
                                       state.style.selectorBorderColor.alpha() > 0 ||
                                       state.style.selectorHoverBorderColor.alpha() > 0 ||
                                       state.style.selectorActiveBorderColor.alpha() > 0;
  state.borderInset = std::max(0, state.style.metrics.borderWidth);
  state.horizontalPadding = std::max(0, state.style.metrics.horizontalPadding);
  state.handleWidth = std::max(0, state.style.metrics.handleWidth);
  state.visibleHandleWidth = std::max(0, state.style.metrics.handleVisibleWidth);
  state.reservedHandleWidth = 0;
  state.controlHeight = std::max(20, state.style.metrics.height);

  state.leftInset = state.horizontalPadding + state.borderInset;
  state.rightInset = state.horizontalPadding + state.borderInset + state.reservedHandleWidth;
  state.verticalInset = 0;
  if (stepButtonLayout_ == StepButtonLayout::Split) {
    state.leftInset = state.borderInset;
    state.rightInset = state.borderInset;
    state.verticalInset = state.interactiveControls
                              ? state.borderInset + (state.inputShellBorderCanBeVisible ? 1 : 0)
                              : 0;
  } else if (state.interactiveControls) {
    state.verticalInset = state.borderInset;
  }

  state.lineEditVerticalInset =
      stepButtonLayout_ == StepButtonLayout::Split ? state.verticalInset : state.borderInset;
  state.inputActionSafetyInset = state.inputShellBorderCanBeVisible ? 1 : 0;
  state.inputActionsInset = state.borderInset + state.inputActionSafetyInset;
  state.inputActionCornerRadius =
      std::max(0, state.style.metrics.borderRadius - state.inputActionsInset);
  state.rightInputActionCornerRadius = joinedRight_ ? 0 : state.inputActionCornerRadius;
  state.splitActionWidth =
      std::max(state.handleWidth,
               std::max(8, state.style.metrics.inputFontSize) + state.horizontalPadding * 2);
  state.splitActionHeight = std::max(1, state.controlHeight - state.verticalInset * 2);
  return state;
}

QVariant AdInputNumber::normalizeBoundaryValue(const QVariant& value) const {
  return valueModel_ ? valueModel_->normalizeBoundaryValue(value) : value;
}

QVariant AdInputNumber::normalizeSingleStepValue(const QVariant& value) const {
  return valueModel_ ? valueModel_->normalizeSingleStepValue(value) : value;
}

void AdInputNumber::applyRangeValues(const QVariant& minimumValue, const QVariant& maximumValue) {
  const bool minimumChanged = inputNumberValueChanged(valueModel_.get(), minimum_, minimumValue);
  const bool maximumChanged = inputNumberValueChanged(valueModel_.get(), maximum_, maximumValue);
  if (!minimumChanged && !maximumChanged) {
    return;
  }

  minimum_ = minimumValue;
  maximum_ = maximumValue;
  syncValueModelConfig();

  emitRangeSignals(minimumChanged, maximumChanged);
  setCommittedValueInternal(value_, true, false, rangeMode_ == RangeMode::Strict);
  syncAccessibleState();
  notifyAccessibleDescriptionChange();
}

void AdInputNumber::emitRangeSignals(bool minimumDidChange, bool maximumDidChange) {
  if (minimumDidChange) {
    emit minimumChanged(minimum());
    emit exactMinimumChanged(exactMinimum());
  }
  if (maximumDidChange) {
    emit maximumChanged(maximum());
    emit exactMaximumChanged(exactMaximum());
  }
}

void AdInputNumber::applySingleStepValue(const QVariant& value) {
  if (!inputNumberValueChanged(valueModel_.get(), singleStep_, value)) {
    return;
  }

  singleStep_ = value;
  syncValueModelConfig();
  updateEditorTextFromValue(false);
  refreshVisualState(false);
  syncAccessibleState();
  notifyAccessibleDescriptionChange();
  emit singleStepChanged(singleStep());
  emit exactSingleStepChanged(exactSingleStep());
}

void AdInputNumber::syncValueModelConfig() {
  if (!valueModel_) {
    valueModel_ = std::make_unique<detail::InputNumberValueModel>();
  }
  detail::InputNumberValueModel::Config config;
  config.minimum = minimum_;
  config.maximum = maximum_;
  config.locale = locale();
  config.singleStep = singleStep_;
  config.decimals = decimals_;
  config.exactMode = valueMode_ == ValueMode::ExactDecimal;
  config.permissiveRange = rangeMode_ == RangeMode::Permissive;
  valueModel_->setConfig(config);
}

void AdInputNumber::updateEditorTextFromValue(bool userTyping) {
  if (!editor_ || internalTextUpdate_) {
    return;
  }

  const bool editing = userTyping || focused_ || hasFocus() || editor_->hasFocus();
  const QString currentInput = editor_->text();
  const QString nextText = displayTextForValue(value_, editing, currentInput);
  if (currentInput == nextText) {
    emitTextChangedIfNeeded(nextText);
    return;
  }

  const int oldCursor = editor_->cursorPosition();
  QSignalBlocker blocker(editor_);
  internalTextUpdate_ = true;
  editor_->setText(nextText);
  internalTextUpdate_ = false;

  if (userTyping) {
    editor_->setCursorPosition(std::clamp(oldCursor, 0, static_cast<int>(nextText.size())));
  } else {
    editor_->setCursorPosition(static_cast<int>(nextText.size()));
  }
  emitTextChangedIfNeeded(nextText);
}

void AdInputNumber::emitTextChangedIfNeeded(const QString& text) {
  if (lastEmittedText_ == text) {
    return;
  }
  lastEmittedText_ = text;
  emit textChanged(text);
}

void AdInputNumber::commitFromEditor() {
  if (!editor_ || !valueModel_) {
    return;
  }

  const QString text = editor_->text();
  const QVariant next = effectiveUserInputValue(true);
  if (!text.trimmed().isEmpty() && !next.isValid()) {
    updateEditorTextFromValue(false);
    refreshVisualState(false);
    return;
  }

  setCommittedValueInternal(next, true, false, false);
}

int AdInputNumber::consumeWheelSteps(int angleDelta) {
  wheelStepRemainder_ += angleDelta;
  const int steps = wheelStepRemainder_ / 120;
  wheelStepRemainder_ %= 120;
  return steps;
}

QVariant AdInputNumber::visualValue() const {
  if (!editor_ || !valueModel_ || !userTyping_) {
    return value_;
  }

  QVariant typedValue = effectiveUserInputValue(false);
  if (typedValue.isValid() || editor_->text().trimmed().isEmpty()) {
    return typedValue;
  }
  return value_;
}

QVariant AdInputNumber::effectiveUserInputValue(bool committed) const {
  if (!editor_ || !valueModel_) {
    return value_;
  }

  const QString text = editor_->text();
  if (text.trimmed().isEmpty()) {
    return QVariant();
  }

  const auto parser = [this](const QString& source) {
    return textPolicy_ ? textPolicy_->parseText(source) : source;
  };
  const QVariant parsed = valueModel_->parseInputText(text, parser, !committed);
  if (!parsed.isValid()) {
    return QVariant();
  }

  return committed ? valueModel_->committedValue(parsed)
                   : valueModel_->applyDecimals(valueModel_->normalizeInputValue(parsed));
}

QString AdInputNumber::canonicalTextForValue(const QVariant& value) const {
  if (!value.isValid()) {
    return QString();
  }
  if (!valueModel_) {
    return value.toString();
  }
  return valueModel_->canonicalText(value);
}

bool AdInputNumber::performStep(int steps, StepEmitter emitter) {
  if (steps == 0 || !isEnabled() || readOnly_ || !valueModel_) {
    return false;
  }

  if ((steps > 0 && valueModel_->isStepDisabled(value_, true)) ||
      (steps < 0 && valueModel_->isStepDisabled(value_, false))) {
    return false;
  }

  QVariant sourceValue = value_;
  if (editor_ && userTyping_) {
    const QVariant typedValue = effectiveUserInputValue(false);
    if (typedValue.isValid() || editor_->text().trimmed().isEmpty()) {
      sourceValue = typedValue;
    }
  }

  const QVariant next = valueModel_->steppedValue(sourceValue, steps);
  if (!next.isValid() || !inputNumberValueChanged(valueModel_.get(), value_, next)) {
    return false;
  }

  userTyping_ = false;
  setCommittedValueInternal(next, true, false, false);
  emit stepped(steps, steps > 0 ? StepType::Up : StepType::Down, emitter);
  return true;
}

void AdInputNumber::setCommittedValueInternal(const QVariant& value, bool emitSignal,
                                              bool preserveEditorText, bool clampToRange) {
  const bool previousHasValue = hasValue();
  const QVariant previousValue = value_;
  QVariant normalized = valueModel_ ? valueModel_->normalizeInputValue(value) : value;
  if (!normalized.isValid()) {
    normalized = QVariant();
  }
  if (!preserveEditorText && valueModel_) {
    if (clampToRange) {
      normalized = valueModel_->applyDecimals(valueModel_->clamp(normalized));
    } else {
      normalized = valueModel_->committedValue(normalized);
    }
  }

  if (!inputNumberValueChanged(valueModel_.get(), previousValue, normalized)) {
    if (!preserveEditorText) {
      updateEditorTextFromValue(false);
    } else {
      emitTextChangedIfNeeded(editor_ ? editor_->text() : QString());
    }
    refreshVisualState(false);
    syncAccessibleState();
    return;
  }

  value_ = normalized;
  if (!preserveEditorText) {
    updateEditorTextFromValue(false);
  } else {
    emitTextChangedIfNeeded(editor_ ? editor_->text() : QString());
  }
  refreshVisualState(false);
  if (emitSignal) {
    emitCommittedValueSignals(previousValue, previousHasValue);
  } else {
    syncAccessibleState();
  }
}

void AdInputNumber::emitCommittedValueSignals(const QVariant& previousValue,
                                              bool previousHasValue) {
  const bool currentHasValue = hasValue();
  if (previousHasValue != currentHasValue) {
    emit hasValueChanged(currentHasValue);
  }

  const bool numericChanged = previousHasValue != currentHasValue ||
                              !qFuzzyCompare(value() + 1.0, previousValue.toDouble() + 1.0);
  if (numericChanged) {
    emit valueChanged(value());
  }
  emit exactValueChanged(exactValue());
  syncAccessibleState();
  notifyAccessibleValueChange();
  notifyAccessibleDescriptionChange();
}

QString AdInputNumber::displayTextForValue(const QVariant& value, bool editing,
                                           const QString& input) const {
  const QString canonical = canonicalTextForValue(value);
  if (textPolicy_) {
    const QString formatted = textPolicy_->formatText(canonical, editing, input);
    if (!formatted.isNull()) {
      return formatted;
    }
  }
  if (!value.isValid()) {
    return editing ? input : QString();
  }
  if (!valueModel_) {
    return value.toString();
  }
  const QString text = valueModel_->defaultDisplayText(value, editing);
  return text.isEmpty() && editing ? input : text;
}

void AdInputNumber::refreshVisualState(bool geometryChanged) {
  if (!editor_) {
    return;
  }

  updateSuffixSpinnerState();
  const ResolvedVisualState state = resolvedVisualState();
  setFont(state.style.metrics.font);
  editor_->setFont(state.style.metrics.font);
  editor_->setAlignment(textAlignment_);
  QAbstractSpinBox::setAlignment(textAlignment_);

  if (prefixLabel_) {
    prefixLabel_->setFont(state.style.metrics.font);
  }
  if (suffixLabel_) {
    suffixLabel_->setFont(state.style.metrics.font);
  }

  editor_->setMinimumHeight(std::max(1, state.controlHeight - state.lineEditVerticalInset * 2));
  const int lineEditPaddingInline =
      stepButtonLayout_ == StepButtonLayout::Split ? state.horizontalPadding : 0;
  editor_->setTextMargins(lineEditPaddingInline, 0, lineEditPaddingInline, 0);
  QPalette linePalette = editor_->palette();
  const QColor transparent(0, 0, 0, 0);
  QColor textColor = state.style.selectorTextColor;
  if (isEnabled() && state.outOfRange) {
    textColor = state.style.outOfRangeTextColor;
  }
  linePalette.setColor(QPalette::Base, transparent);
  linePalette.setColor(QPalette::Window, transparent);
  linePalette.setColor(QPalette::Disabled, QPalette::Base, transparent);
  linePalette.setColor(QPalette::Disabled, QPalette::Window, transparent);
  linePalette.setColor(QPalette::Text, textColor);
  linePalette.setColor(QPalette::Disabled, QPalette::Text, state.style.disabledTextColor);
  linePalette.setColor(QPalette::PlaceholderText, state.style.placeholderColor);
  editor_->setPalette(linePalette);

  if (prefixLabel_) {
    QPalette palette = prefixLabel_->palette();
    palette.setColor(QPalette::WindowText, state.style.prefixColor);
    prefixLabel_->setPalette(palette);
  }
  if (suffixLabel_) {
    QPalette palette = suffixLabel_->palette();
    palette.setColor(QPalette::WindowText, state.style.suffixColor);
    suffixLabel_->setPalette(palette);
  }

  const int iconSide = std::max(8, state.style.metrics.iconSize);
  const qreal dpr = devicePixelRatioF();
  if (prefixIconLabel_) {
    prefixIconLabel_->setFixedSize(iconSide, iconSide);
    prefixIconLabel_->setPixmap(
        stepButtonLayout_ == StepButtonLayout::Compact && adqt::icons::isValid(prefixIconRef_)
            ? renderTintedIcon(prefixIconRef_, state.style.prefixColor, iconSide, dpr)
            : QPixmap());
  }
  if (suffixIconLabel_) {
    suffixIconLabel_->setFixedSize(iconSide, iconSide);
    suffixIconLabel_->setPixmap(
        stepButtonLayout_ == StepButtonLayout::Compact && adqt::icons::isValid(suffixIconRef_)
            ? renderTintedIcon(suffixIconRef_, state.style.suffixColor, iconSide, dpr,
                               isLoadingIcon(suffixIconRef_) ? sharedSpinnerAngle() : 0)
            : QPixmap());
  }

  if (inputActionsWidget_) {
    const QColor actionsBackground = stepButtonLayout_ == StepButtonLayout::Compact
                                         ? state.style.handleBg
                                         : QColor(Qt::transparent);
    QPalette actionsPalette = inputActionsWidget_->palette();
    actionsPalette.setColor(QPalette::Window, actionsBackground);
    actionsPalette.setColor(QPalette::Disabled, QPalette::Window, actionsBackground);
    inputActionsWidget_->setPalette(actionsPalette);
    if (inputActionsLayout_) {
      inputActionsLayout_->setContentsMargins(0, state.inputActionSafetyInset,
                                              state.inputActionSafetyInset,
                                              state.inputActionSafetyInset);
    }
  }
  if (inputActionsBorderOverlay_) {
    const bool showBorderOverlay =
        stepButtonLayout_ == StepButtonLayout::Compact && state.visibleHandleWidth > 0 &&
        state.inputShellBorderCanBeVisible && state.borderInset > 0 && !state.style.underlined;
    inputActionsBorderOverlay_->setVisible(showBorderOverlay);
    static_cast<InputActionsBorderOverlay*>(inputActionsBorderOverlay_)
        ->setBorderStyle(state.border, state.style.metrics.borderWidth,
                         joinedRight_ ? 0 : state.style.metrics.borderRadius,
                         state.style.underlined);
  }
  const int handleIconSide = std::max(6, state.style.metrics.handleIconSize);
  const int spinnerIconSide = std::max(6, state.style.metrics.splitIconSize);
  const int leftSplitCornerRadius = joinedLeft_ ? 0 : state.inputActionCornerRadius;
  if (inputUpButton_) {
    inputUpButton_->setMinimumWidth(std::max(state.handleWidth, state.visibleHandleWidth));
    inputUpButton_->setIconSize(QSize(handleIconSide, handleIconSide));
    StepButtonPaintStyle inputUpStyle;
    inputUpStyle.normalBackground = transparent;
    inputUpStyle.hoverBackground = transparent;
    inputUpStyle.pressedBackground = state.style.handleActiveBg;
    inputUpStyle.disabledBackground = transparent;
    inputUpStyle.borderColor = state.style.handleBorderColor;
    inputUpStyle.borderWidth = state.borderInset;
    inputUpStyle.topRightRadius = state.rightInputActionCornerRadius;
    inputUpStyle.borderLeft = true;
    static_cast<StepControlButton*>(inputUpButton_)->setPaintStyle(inputUpStyle);
  }
  if (inputDownButton_) {
    inputDownButton_->setMinimumWidth(std::max(state.handleWidth, state.visibleHandleWidth));
    inputDownButton_->setIconSize(QSize(handleIconSide, handleIconSide));
    StepButtonPaintStyle inputDownStyle;
    inputDownStyle.normalBackground = transparent;
    inputDownStyle.hoverBackground = transparent;
    inputDownStyle.pressedBackground = state.style.handleActiveBg;
    inputDownStyle.disabledBackground = transparent;
    inputDownStyle.borderColor = state.style.handleBorderColor;
    inputDownStyle.borderWidth = state.borderInset;
    inputDownStyle.bottomRightRadius = state.rightInputActionCornerRadius;
    inputDownStyle.borderLeft = true;
    inputDownStyle.borderTop = true;
    static_cast<StepControlButton*>(inputDownButton_)->setPaintStyle(inputDownStyle);
  }

  if (splitDownButton_) {
    splitDownButton_->setFixedWidth(std::max(20, state.splitActionWidth));
    splitDownButton_->setFixedHeight(state.splitActionHeight);
    splitDownButton_->setIconSize(QSize(spinnerIconSide, spinnerIconSide));
    StepButtonPaintStyle spinnerDownStyle;
    spinnerDownStyle.normalBackground = transparent;
    spinnerDownStyle.hoverBackground = transparent;
    spinnerDownStyle.pressedBackground = state.style.handleActiveBg;
    spinnerDownStyle.disabledBackground = transparent;
    spinnerDownStyle.borderColor = state.style.handleBorderColor;
    spinnerDownStyle.borderWidth = state.borderInset;
    spinnerDownStyle.topLeftRadius = leftSplitCornerRadius;
    spinnerDownStyle.bottomLeftRadius = leftSplitCornerRadius;
    spinnerDownStyle.borderRight = true;
    static_cast<StepControlButton*>(splitDownButton_)->setPaintStyle(spinnerDownStyle);
  }
  if (splitUpButton_) {
    splitUpButton_->setFixedWidth(std::max(20, state.splitActionWidth));
    splitUpButton_->setFixedHeight(state.splitActionHeight);
    splitUpButton_->setIconSize(QSize(spinnerIconSide, spinnerIconSide));
    StepButtonPaintStyle spinnerUpStyle;
    spinnerUpStyle.normalBackground = transparent;
    spinnerUpStyle.hoverBackground = transparent;
    spinnerUpStyle.pressedBackground = state.style.handleActiveBg;
    spinnerUpStyle.disabledBackground = transparent;
    spinnerUpStyle.borderColor = state.style.handleBorderColor;
    spinnerUpStyle.borderWidth = state.borderInset;
    spinnerUpStyle.topRightRadius = state.rightInputActionCornerRadius;
    spinnerUpStyle.bottomRightRadius = state.rightInputActionCornerRadius;
    spinnerUpStyle.borderLeft = true;
    static_cast<StepControlButton*>(splitUpButton_)->setPaintStyle(spinnerUpStyle);
  }

  updateAffixVisual();
  updateReadOnlyState();
  updateActionVisibility();
  layoutChildren(state);
  updateActionIcons(state);
  updateInteractiveCursor();
  updateInteractionFocusOverlay();
  update();
  if (geometryChanged) {
    updateGeometry();
  }
}

void AdInputNumber::layoutChildren(const ResolvedVisualState& state) {
  if (!editor_) {
    return;
  }

  const int contentTop = state.lineEditVerticalInset;
  const int contentHeight = std::max(1, height() - state.lineEditVerticalInset * 2);

  if (stepButtonLayout_ == StepButtonLayout::Split) {
    const int downWidth =
        (splitDownButton_ && splitDownButton_->isVisible()) ? splitDownButton_->width() : 0;
    const int upWidth =
        (splitUpButton_ && splitUpButton_->isVisible()) ? splitUpButton_->width() : 0;

    if (splitDownButton_) {
      splitDownButton_->setGeometry(0, state.verticalInset, downWidth, state.splitActionHeight);
    }
    if (splitUpButton_) {
      splitUpButton_->setGeometry(std::max(0, width() - upWidth), state.verticalInset, upWidth,
                                  state.splitActionHeight);
    }
    if (inputActionsWidget_) {
      inputActionsWidget_->setGeometry(0, 0, 0, 0);
      inputActionsWidget_->clearMask();
    }

    editor_->setGeometry(downWidth, state.lineEditVerticalInset,
                         std::max(1, width() - downWidth - upWidth),
                         std::max(1, height() - state.lineEditVerticalInset * 2));
    if (splitDownButton_ && splitDownButton_->isVisible()) {
      splitDownButton_->raise();
    }
    if (splitUpButton_ && splitUpButton_->isVisible()) {
      splitUpButton_->raise();
    }
    return;
  }

  int x = state.leftInset;
  const int editorRight = width() - state.rightInset;
  const int groupGap = affixPadding(state.style);
  const int itemGap = affixItemGap(state.style);
  const auto centeredRect = [contentTop, contentHeight](int left, const QSize& size) {
    return QRect(left, contentTop + (contentHeight - size.height()) / 2, size.width(),
                 size.height());
  };

  bool hasLeadingAffix = false;
  const auto layoutLeft = [&](QWidget* widget, const QSize& size) {
    if (!widget || !widget->isVisible() || !size.isValid() || size.width() <= 0 ||
        size.height() <= 0) {
      return;
    }
    if (hasLeadingAffix) {
      x += itemGap;
    }
    widget->setGeometry(centeredRect(x, size));
    x += size.width();
    hasLeadingAffix = true;
  };

  const bool hasTrailingAffixWidgets = (suffixIconLabel_ && suffixIconLabel_->isVisible()) ||
                                       (suffixLabel_ && suffixLabel_->isVisible());
  Q_UNUSED(hasTrailingAffixWidgets)
  int right = std::max(x, editorRight);
  bool hasTrailingAffix = false;
  const auto layoutRight = [&](QWidget* widget, const QSize& size) {
    if (!widget || !widget->isVisible() || !size.isValid() || size.width() <= 0 ||
        size.height() <= 0) {
      return;
    }
    if (hasTrailingAffix) {
      right -= itemGap;
    }
    right -= size.width();
    widget->setGeometry(centeredRect(right, size));
    hasTrailingAffix = true;
  };

  if (prefixIconLabel_ && prefixIconLabel_->isVisible()) {
    layoutLeft(prefixIconLabel_,
               prefixIconLabel_->sizeHint().expandedTo(prefixIconLabel_->minimumSize()));
  }
  if (prefixLabel_ && prefixLabel_->isVisible()) {
    layoutLeft(prefixLabel_, prefixLabel_->sizeHint());
  }
  if (hasLeadingAffix) {
    x += groupGap;
  }
  if (suffixIconLabel_ && suffixIconLabel_->isVisible()) {
    layoutRight(suffixIconLabel_,
                suffixIconLabel_->sizeHint().expandedTo(suffixIconLabel_->minimumSize()));
  }
  if (suffixLabel_ && suffixLabel_->isVisible()) {
    layoutRight(suffixLabel_, suffixLabel_->sizeHint());
  }
  if (hasTrailingAffix) {
    right -= groupGap;
  }

  editor_->setGeometry(x, state.lineEditVerticalInset, std::max(1, right - x),
                       std::max(1, height() - state.lineEditVerticalInset * 2));
  updateInputActionsGeometry(state);
  if (inputActionsWidget_ && inputActionsWidget_->isVisible()) {
    inputActionsWidget_->raise();
  }
  if (inputActionsBorderOverlay_ && inputActionsBorderOverlay_->isVisible()) {
    inputActionsBorderOverlay_->raise();
  }
}

void AdInputNumber::updateInteractionFocusOverlay() {
  const ResolvedVisualState state = resolvedVisualState();
  if (!state.context.focused || !isEnabled() || !isVisible() || state.style.underlined ||
      state.style.selectorFocusOutlineColor.alpha() <= 0 ||
      state.style.metrics.focusOutlineWidth <= 0.0) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const qreal borderWidth = std::max<qreal>(0.0, state.style.metrics.borderWidth);
  const QRectF shellRect = joinedBorderRect(rect(), borderWidth, joinedLeft_, joinedRight_);
  if (!shellRect.isValid() || shellRect.width() <= 0.0 || shellRect.height() <= 0.0) {
    stopInteractionFocusForOwner(this);
    return;
  }

  QWidget* hostWindow = window();
  if (!hostWindow) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const qreal cornerRadius = std::max<qreal>(0.0, state.style.metrics.borderRadius);
  InteractionFocusRequest request;
  request.owner = this;
  const QPoint origin = mapTo(hostWindow, QPoint(0, 0));
  request.baseRectInWindow = shellRect.translated(origin.x(), origin.y());
  request.topLeft = joinedLeft_ ? 0.0 : cornerRadius;
  request.topRight = joinedRight_ ? 0.0 : cornerRadius;
  request.bottomRight = joinedRight_ ? 0.0 : cornerRadius;
  request.bottomLeft = joinedLeft_ ? 0.0 : cornerRadius;
  request.color = state.style.selectorFocusOutlineColor;
  request.strokeWidth = std::max<qreal>(1.0, state.style.metrics.focusOutlineWidth);
  request.offset = std::max<qreal>(0.0, state.style.metrics.focusOutlineOffset);
  triggerInteractionFocus(request);
}

void AdInputNumber::updateActionIcons(const ResolvedVisualState& state) {
  const adqt::icons::IconRef upToken =
      adqt::icons::isValid(upIconRef_)
          ? upIconRef_
          : (stepButtonLayout_ == StepButtonLayout::Split ? outlined_icons::Plus()
                                                          : outlined_icons::Up());
  const adqt::icons::IconRef downToken =
      adqt::icons::isValid(downIconRef_)
          ? downIconRef_
          : (stepButtonLayout_ == StepButtonLayout::Split ? outlined_icons::Minus()
                                                          : outlined_icons::Down());

  QColor upColor = state.style.handleIconColor;
  QColor downColor = state.style.handleIconColor;
  if (state.interactiveControls) {
    const bool upDisabled = valueModel_ && valueModel_->isStepDisabled(value_, true);
    const bool downDisabled = valueModel_ && valueModel_->isStepDisabled(value_, false);
    const bool upHovered =
        !upDisabled && ((stepButtonLayout_ == StepButtonLayout::Split && splitUpButton_ &&
                         splitUpButton_->isEnabled() && splitUpButton_->underMouse()) ||
                        (stepButtonLayout_ == StepButtonLayout::Compact && inputUpButton_ &&
                         inputUpButton_->isEnabled() && inputUpButton_->underMouse()));
    const bool downHovered =
        !downDisabled && ((stepButtonLayout_ == StepButtonLayout::Split && splitDownButton_ &&
                           splitDownButton_->isEnabled() && splitDownButton_->underMouse()) ||
                          (stepButtonLayout_ == StepButtonLayout::Compact && inputDownButton_ &&
                           inputDownButton_->isEnabled() && inputDownButton_->underMouse()));
    if (upHovered) {
      upColor = state.style.handleHoverColor;
    }
    if (downHovered) {
      downColor = state.style.handleHoverColor;
    }
  }

  const int handleIconSide = std::max(6, state.style.metrics.handleIconSize);
  const int spinnerIconSide = std::max(6, state.style.metrics.splitIconSize);
  const qreal dpr = devicePixelRatioF();

  if (inputUpButton_) {
    inputUpButton_->setIcon(QIcon(renderTintedIcon(upToken, upColor, handleIconSide, dpr)));
    inputUpButton_->setIconSize(QSize(handleIconSide, handleIconSide));
  }
  if (inputDownButton_) {
    inputDownButton_->setIcon(QIcon(renderTintedIcon(downToken, downColor, handleIconSide, dpr)));
    inputDownButton_->setIconSize(QSize(handleIconSide, handleIconSide));
  }
  if (splitUpButton_) {
    splitUpButton_->setIcon(QIcon(renderTintedIcon(upToken, upColor, spinnerIconSide, dpr)));
    splitUpButton_->setIconSize(QSize(spinnerIconSide, spinnerIconSide));
  }
  if (splitDownButton_) {
    splitDownButton_->setIcon(QIcon(renderTintedIcon(downToken, downColor, spinnerIconSide, dpr)));
    splitDownButton_->setIconSize(QSize(spinnerIconSide, spinnerIconSide));
  }
}

bool AdInputNumber::shouldShowInputActions(const ResolvedVisualState& state) const {
  return stepButtonLayout_ == StepButtonLayout::Compact && state.interactiveControls &&
         state.visibleHandleWidth > 0;
}

void AdInputNumber::updateActionVisibility() {
  const ResolvedVisualState state = resolvedVisualState();
  const bool interactive = state.interactiveControls;
  const bool showInputActions = shouldShowInputActions(state);
  const bool showSpinnerActions = stepButtonLayout_ == StepButtonLayout::Split && interactive;
  const bool upEnabled = !valueModel_ || !valueModel_->isStepDisabled(value_, true);
  const bool downEnabled = !valueModel_ || !valueModel_->isStepDisabled(value_, false);

  if (inputActionsWidget_) {
    inputActionsWidget_->setVisible(showInputActions);
  }
  if (inputUpButton_) {
    inputUpButton_->setVisible(showInputActions);
    inputUpButton_->setEnabled(showInputActions && upEnabled);
  }
  if (inputDownButton_) {
    inputDownButton_->setVisible(showInputActions);
    inputDownButton_->setEnabled(showInputActions && downEnabled);
  }
  if (splitUpButton_) {
    splitUpButton_->setVisible(showSpinnerActions);
    splitUpButton_->setEnabled(showSpinnerActions && upEnabled);
  }
  if (splitDownButton_) {
    splitDownButton_->setVisible(showSpinnerActions);
    splitDownButton_->setEnabled(showSpinnerActions && downEnabled);
  }
}

void AdInputNumber::updateAffixVisual() {
  if (!prefixLabel_ || !suffixLabel_ || !prefixIconLabel_ || !suffixIconLabel_) {
    return;
  }

  const bool inputMode = stepButtonLayout_ == StepButtonLayout::Compact;
  prefixLabel_->setText(prefixText_);
  suffixLabel_->setText(suffixText_);
  prefixLabel_->setVisible(inputMode && !prefixText_.trimmed().isEmpty());
  suffixLabel_->setVisible(inputMode && !suffixText_.trimmed().isEmpty());
  prefixIconLabel_->setVisible(inputMode && adqt::icons::isValid(prefixIconRef_));
  suffixIconLabel_->setVisible(inputMode && adqt::icons::isValid(suffixIconRef_));
}

void AdInputNumber::updateSuffixSpinnerState() {
  const bool spinning = isVisible() && stepButtonLayout_ == StepButtonLayout::Compact &&
                        isLoadingIcon(suffixIconRef_);
  if (spinning && !suffixSpinnerSubscribed_) {
    detail::setFrameSubscription(
        this, QString::fromLatin1(kSuffixSpinnerFrameKey), true, [this](qint64, qint64) {
          if (isVisible() && stepButtonLayout_ == StepButtonLayout::Compact &&
              isLoadingIcon(suffixIconRef_)) {
            updateActionIcons(resolvedVisualState());
          }
        });
    suffixSpinnerSubscribed_ = true;
  } else if (!spinning && suffixSpinnerSubscribed_) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kSuffixSpinnerFrameKey));
    suffixSpinnerSubscribed_ = false;
  }
}

void AdInputNumber::updateReadOnlyState() {
  const bool disabledNow = !isEnabled();
  QAbstractSpinBox::setReadOnly(readOnly_);

  if (editor_) {
    editor_->setReadOnly(readOnly_ || disabledNow);
    editor_->setEnabled(!disabledNow);
  }

  const bool buttonEnabled = stepButtonsVisible_ && !readOnly_ && !disabledNow;
  if (inputUpButton_) {
    inputUpButton_->setEnabled(buttonEnabled &&
                               (!valueModel_ || !valueModel_->isStepDisabled(value_, true)));
  }
  if (inputDownButton_) {
    inputDownButton_->setEnabled(buttonEnabled &&
                                 (!valueModel_ || !valueModel_->isStepDisabled(value_, false)));
  }
  if (splitUpButton_) {
    splitUpButton_->setEnabled(buttonEnabled &&
                               (!valueModel_ || !valueModel_->isStepDisabled(value_, true)));
  }
  if (splitDownButton_) {
    splitDownButton_->setEnabled(buttonEnabled &&
                                 (!valueModel_ || !valueModel_->isStepDisabled(value_, false)));
  }
}

void AdInputNumber::updateInteractiveCursor() {
  const bool disabledNow = !isEnabled();
  const Qt::CursorShape defaultCursor = disabledNow ? Qt::ForbiddenCursor : Qt::ArrowCursor;

  setCursor(defaultCursor);
  if (prefixLabel_) {
    prefixLabel_->setCursor(defaultCursor);
  }
  if (suffixLabel_) {
    suffixLabel_->setCursor(defaultCursor);
  }
  if (prefixIconLabel_) {
    prefixIconLabel_->setCursor(defaultCursor);
  }
  if (suffixIconLabel_) {
    suffixIconLabel_->setCursor(defaultCursor);
  }
  if (inputActionsWidget_) {
    inputActionsWidget_->setCursor(defaultCursor);
  }

  if (editor_) {
    editor_->setCursor(disabledNow ? Qt::ForbiddenCursor
                                   : (readOnly_ ? Qt::ArrowCursor : Qt::IBeamCursor));
  }

  auto applyButtonCursor = [disabledNow, defaultCursor](QToolButton* button) {
    if (!button) {
      return;
    }
    Qt::CursorShape buttonCursor = defaultCursor;
    if (!disabledNow && button->isVisible()) {
      buttonCursor = button->isEnabled() ? Qt::PointingHandCursor : Qt::ForbiddenCursor;
    }
    button->setCursor(buttonCursor);
  };

  applyButtonCursor(inputUpButton_);
  applyButtonCursor(inputDownButton_);
  applyButtonCursor(splitUpButton_);
  applyButtonCursor(splitDownButton_);
}

void AdInputNumber::updateInputActionsGeometry(const ResolvedVisualState& state) {
  if (!inputActionsWidget_) {
    return;
  }

  if (stepButtonLayout_ != StepButtonLayout::Compact || !inputActionsWidget_->isVisible()) {
    inputActionsWidget_->setGeometry(0, 0, 0, 0);
    inputActionsWidget_->clearMask();
    if (inputActionsBorderOverlay_) {
      inputActionsBorderOverlay_->setGeometry(0, 0, 0, 0);
      inputActionsBorderOverlay_->hide();
    }
    return;
  }

  const int insetX = std::max(0, state.inputActionsInset);
  const int insetY = std::max(0, state.inputActionsInset);
  const int widthHint = std::max(0, state.visibleHandleWidth);
  const int maxWidth = std::max(0, width() - insetX);
  const int overlayWidth = std::min(widthHint, maxWidth);
  const int overlayHeight = std::max(1, height() - insetY * 2);
  const int overlayX = std::max(0, width() - insetX - overlayWidth);

  inputActionsWidget_->setGeometry(overlayX, insetY, overlayWidth, overlayHeight);
  if (inputActionsBorderOverlay_ && inputActionsBorderOverlay_->isVisible()) {
    const int overlayShellX = std::max(0, overlayX - std::max(0, state.borderInset));
    inputActionsBorderOverlay_->setGeometry(overlayShellX, 0, std::max(0, width() - overlayShellX),
                                            height());
  }

  if (overlayWidth > 0) {
    const qreal clipWidth = std::max<qreal>(1.0, inputActionsWidget_->width() - 1.0);
    const qreal clipHeight = std::max<qreal>(1.0, inputActionsWidget_->height() - 1.0);
    const QPainterPath actionsPath =
        roundedRectPath(QRectF(0.0, 0.0, clipWidth, clipHeight), 0.0,
                        static_cast<qreal>(state.rightInputActionCornerRadius),
                        static_cast<qreal>(state.rightInputActionCornerRadius), 0.0);
    inputActionsWidget_->setMask(QRegion(actionsPath.toFillPolygon().toPolygon()));
  } else {
    inputActionsWidget_->clearMask();
  }
}

bool AdInputNumber::isHoverTrackedChild(const QObject* watched) const {
  return watched == editor_ || watched == prefixLabel_ || watched == suffixLabel_ ||
         watched == prefixIconLabel_ || watched == suffixIconLabel_ ||
         watched == inputActionsWidget_ || watched == inputUpButton_ ||
         watched == inputDownButton_ || watched == splitUpButton_ || watched == splitDownButton_;
}

void AdInputNumber::setChildHovered(const QObject* watched, bool hovered) {
  if (!watched || !isHoverTrackedChild(watched)) {
    return;
  }
  if (hovered) {
    hoveredChildren_.insert(watched);
  } else {
    hoveredChildren_.remove(watched);
  }
  syncHoveredState();
}

void AdInputNumber::syncHoveredState() {
  const bool effectiveHovered = selfHovered_ || !hoveredChildren_.isEmpty();
  if (hovered_ == effectiveHovered) {
    return;
  }
  hovered_ = effectiveHovered;
  if (hovered_) {
    bumpJoinedZOrder();
  }
  refreshVisualState(false);
}

void AdInputNumber::focusFromMouseGlobalPos(const QPoint& globalPos, Qt::FocusReason reason) {
  if (!isEnabled() || !editor_) {
    return;
  }

  const QPoint editPos = editor_->mapFromGlobal(globalPos);
  if (editor_->rect().contains(editPos)) {
    editor_->setCursorPosition(editor_->cursorPositionAt(editPos));
  } else if (editPos.x() <= 0) {
    editor_->setCursorPosition(0);
  } else if (editPos.x() >= editor_->width()) {
    editor_->setCursorPosition(static_cast<int>(editor_->text().size()));
  }

  editor_->setFocus(reason);
  bumpJoinedZOrder();
}

void AdInputNumber::bumpJoinedZOrder() {
  if (!(joinedLeft_ || joinedRight_)) {
    return;
  }
  const bool effectiveFocused = focused_ || hasFocus() || (editor_ && editor_->hasFocus());
  if (!(effectiveFocused || hovered_)) {
    return;
  }
  raise();
}

void AdInputNumber::syncAccessibleState() {
  detail::syncDerivedAccessibleDescription(this, inputNumberAccessibleDescription(this));

  if (editor_) {
    editor_->setAccessibleName(inputNumberAccessibleName(this));
    editor_->setAccessibleDescription(inputNumberAccessibleDescription(this));
  }
  if (inputUpButton_) {
    inputUpButton_->setAccessibleName(tr("Increase value"));
  }
  if (inputDownButton_) {
    inputDownButton_->setAccessibleName(tr("Decrease value"));
  }
  if (splitUpButton_) {
    splitUpButton_->setAccessibleName(tr("Increase value"));
  }
  if (splitDownButton_) {
    splitDownButton_->setAccessibleName(tr("Decrease value"));
  }
}

void AdInputNumber::notifyAccessibleValueChange() const {
  QAccessibleValueChangeEvent event(const_cast<AdInputNumber*>(this),
                                    inputNumberAccessibleCurrentValue(this));
  QAccessible::updateAccessibility(&event);
}

void AdInputNumber::notifyAccessibleDescriptionChange() const {
  detail::notifyAccessibilityEvent(const_cast<AdInputNumber*>(this),
                                   QAccessible::DescriptionChanged);
}

void AdInputNumber::notifyAccessibleFocusChange() const {
  detail::notifyAccessibilityEvent(const_cast<AdInputNumber*>(this), QAccessible::Focus);
}

bool AdInputNumber::eventFilter(QObject* watched, QEvent* event) {
  if (!event) {
    return QAbstractSpinBox::eventFilter(watched, event);
  }

  if (isHoverTrackedChild(watched)) {
    if (event->type() == QEvent::Enter) {
      setChildHovered(watched, true);
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
      setChildHovered(watched, false);
    }
  }

  if (watched == editor_) {
    if (event->type() == QEvent::FocusIn) {
      focused_ = true;
      bumpJoinedZOrder();
      updateEditorTextFromValue(false);
      refreshVisualState(false);
      syncAccessibleState();
    } else if (event->type() == QEvent::FocusOut) {
      focused_ = false;
      userTyping_ = false;
      updateEditorTextFromValue(false);
      refreshVisualState(false);
      syncAccessibleState();
    } else if (event->type() == QEvent::KeyPress) {
      auto* keyEvent = static_cast<QKeyEvent*>(event);
      if (isEnabled() && !readOnly_) {
        if (stepKeysEnabled_) {
          if (keyEvent->key() == Qt::Key_Up) {
            performStep(+1, StepEmitter::KeyDown);
            return true;
          }
          if (keyEvent->key() == Qt::Key_Down) {
            performStep(-1, StepEmitter::KeyDown);
            return true;
          }
        } else if (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down) {
          keyEvent->accept();
          return true;
        }
      }
    } else if (event->type() == QEvent::Wheel && isEnabled() && !readOnly_ && wheelStepEnabled_) {
      auto* wheel = static_cast<QWheelEvent*>(event);
      const int steps = consumeWheelSteps(wheel->angleDelta().y());
      if (steps != 0) {
        performStep(steps, StepEmitter::Wheel);
        wheel->accept();
        return true;
      }
    }
  } else if (watched == inputUpButton_ || watched == inputDownButton_ ||
             watched == splitUpButton_ || watched == splitDownButton_) {
    if (isLeftMouseActivationEvent(event) && isEnabled() && editor_) {
      if (QWidget* source = qobject_cast<QWidget*>(watched)) {
        const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
        focusFromMouseGlobalPos(source->mapToGlobal(mouseEventPos(mouseEvent)),
                                Qt::MouseFocusReason);
      }
    }
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
        event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
      updateActionIcons(resolvedVisualState());
    }
  } else if (watched == prefixLabel_ || watched == suffixLabel_ || watched == prefixIconLabel_ ||
             watched == suffixIconLabel_ || watched == inputActionsWidget_) {
    if (isLeftMouseActivationEvent(event)) {
      if (!isEnabled() || !editor_) {
        return true;
      }
      if (QWidget* source = qobject_cast<QWidget*>(watched)) {
        const auto* mouseEvent = static_cast<const QMouseEvent*>(event);
        focusFromMouseGlobalPos(source->mapToGlobal(mouseEventPos(mouseEvent)),
                                Qt::MouseFocusReason);
      }
      return true;
    }
  }

  return QAbstractSpinBox::eventFilter(watched, event);
}

void AdInputNumber::focusInEvent(QFocusEvent* event) {
  QAbstractSpinBox::focusInEvent(event);
  const bool effectiveFocused = hasFocus() || (editor_ && editor_->hasFocus());
  if (focused_ != effectiveFocused) {
    focused_ = effectiveFocused;
  }
  bumpJoinedZOrder();
  updateEditorTextFromValue(false);
  refreshVisualState(false);
  syncAccessibleState();
  notifyAccessibleFocusChange();
}

void AdInputNumber::focusOutEvent(QFocusEvent* event) {
  QAbstractSpinBox::focusOutEvent(event);
  const bool effectiveFocused = hasFocus() || (editor_ && editor_->hasFocus());
  if (focused_ != effectiveFocused) {
    focused_ = effectiveFocused;
  }
  if (!effectiveFocused) {
    userTyping_ = false;
  }
  updateEditorTextFromValue(false);
  refreshVisualState(false);
  syncAccessibleState();
  notifyAccessibleFocusChange();
}

void AdInputNumber::enterEvent(QEnterEvent* event) {
  selfHovered_ = true;
  syncHoveredState();
  QAbstractSpinBox::enterEvent(event);
}

void AdInputNumber::leaveEvent(QEvent* event) {
  selfHovered_ = false;
  syncHoveredState();
  QAbstractSpinBox::leaveEvent(event);
}

void AdInputNumber::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  const ResolvedVisualState state = resolvedVisualState();

  const qreal borderWidth = std::max<qreal>(0.0, state.style.metrics.borderWidth);
  const bool hasVisibleBorder = borderWidth > 0.0 && state.border.alpha() > 0;
  const QRectF fillRect(rect());
  const QRectF rawBorderRect = joinedBorderRect(rect(), borderWidth, joinedLeft_, joinedRight_);
  if (!fillRect.isValid() || fillRect.width() <= 0.0 || fillRect.height() <= 0.0) {
    return;
  }
  if (hasVisibleBorder &&
      (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 || rawBorderRect.height() <= 0.0)) {
    return;
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
  const QRectF borderRect = snapRectToDevicePixels(rawBorderRect, dpr);

  const qreal cornerRadius =
      state.style.underlined ? 0.0 : std::max<qreal>(0.0, state.style.metrics.borderRadius);
  const qreal topLeftRadius = joinedLeft_ ? 0.0 : cornerRadius;
  const qreal topRightRadius = joinedRight_ ? 0.0 : cornerRadius;
  const qreal bottomRightRadius = joinedRight_ ? 0.0 : cornerRadius;
  const qreal bottomLeftRadius = joinedLeft_ ? 0.0 : cornerRadius;

  if (state.style.underlined) {
    if (state.background.alpha() > 0) {
      painter.fillRect(fillRect, state.background);
    }
    if (hasVisibleBorder) {
      QPen underlinePen(state.border, borderWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
      painter.setPen(underlinePen);
      painter.setBrush(Qt::NoBrush);
      const qreal underlineY = snapToDevicePixelCoord(borderRect.bottom(), dpr);
      painter.drawLine(QPointF(fillRect.left(), underlineY), QPointF(fillRect.right(), underlineY));
    }
    return;
  }

  const QPainterPath fillPath =
      roundedRectPath(fillRect, topLeftRadius, topRightRadius, bottomRightRadius, bottomLeftRadius);
  if (state.background.alpha() > 0) {
    painter.fillPath(fillPath, state.background);
  }

  if (hasVisibleBorder) {
    const QPainterPath borderPath = roundedRectPath(borderRect, topLeftRadius, topRightRadius,
                                                    bottomRightRadius, bottomLeftRadius);
    QPen borderPen(state.border, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
    painter.setPen(borderPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(borderPath);
  }
}

void AdInputNumber::changeEvent(QEvent* event) {
  QAbstractSpinBox::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::Hide) {
    selfHovered_ = false;
    hoveredChildren_.clear();
    hovered_ = false;
    stopInteractionFocusForOwner(this);
    return;
  }

  if (event->type() == QEvent::LocaleChange) {
    userTyping_ = false;
    syncValueModelConfig();
    updateEditorTextFromValue(false);
    refreshVisualState(true);
    syncAccessibleState();
    notifyAccessibleValueChange();
    notifyAccessibleDescriptionChange();
  }

  if (event->type() == QEvent::EnabledChange || event->type() == QEvent::FontChange ||
      event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange ||
      event->type() == QEvent::LanguageChange) {
    const bool geometryChanged =
        event->type() == QEvent::EnabledChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::StyleChange;
    updateReadOnlyState();
    updateActionVisibility();
    refreshVisualState(geometryChanged);
    syncAccessibleState();
  }

  if (event->type() == QEvent::EnabledChange) {
    QAccessible::State changedState;
    changedState.disabled = !isEnabled();
    QAccessibleStateChangeEvent accessibilityEvent(this, changedState);
    QAccessible::updateAccessibility(&accessibilityEvent);
  }
}

void AdInputNumber::moveEvent(QMoveEvent* event) {
  QAbstractSpinBox::moveEvent(event);
  updateInteractionFocusOverlay();
}

void AdInputNumber::resizeEvent(QResizeEvent* event) {
  QAbstractSpinBox::resizeEvent(event);
  refreshVisualState(true);
}

void AdInputNumber::showEvent(QShowEvent* event) {
  QAbstractSpinBox::showEvent(event);
  updateSuffixSpinnerState();
  refreshVisualState(true);
  syncAccessibleState();
  detail::notifyAccessibilityEvent(this, QAccessible::ObjectShow);
}

void AdInputNumber::hideEvent(QHideEvent* event) {
  QAbstractSpinBox::hideEvent(event);
  updateSuffixSpinnerState();
  stopInteractionFocusForOwner(this);
  detail::notifyAccessibilityEvent(this, QAccessible::ObjectHide);
}

void AdInputNumber::wheelEvent(QWheelEvent* event) {
  if (event && isEnabled() && !readOnly_ && wheelStepEnabled_) {
    const int steps = consumeWheelSteps(event->angleDelta().y());
    if (steps != 0) {
      performStep(steps, StepEmitter::Wheel);
      event->accept();
      return;
    }
  }
  if (event) {
    event->ignore();
  }
}

void AdInputNumber::mousePressEvent(QMouseEvent* event) {
  if (event && event->button() == Qt::LeftButton && isEnabled() && editor_) {
    focusFromMouseGlobalPos(mapToGlobal(mouseEventPos(event)), Qt::MouseFocusReason);
    event->accept();
    return;
  }
  QAbstractSpinBox::mousePressEvent(event);
}

QValidator::State AdInputNumber::validate(QString& text, int& pos) const {
  Q_UNUSED(pos)
  if (text.trimmed().isEmpty()) {
    return QValidator::Intermediate;
  }
  if (detail::InputNumberValueModel::isIntermediateText(text, locale())) {
    return QValidator::Intermediate;
  }
  const auto parser = [this](const QString& source) {
    return textPolicy_ ? textPolicy_->parseText(source) : source;
  };
  const QVariant parsed =
      valueModel_ ? valueModel_->parseInputText(text, parser, true) : QVariant();
  if (parsed.isValid()) {
    return QValidator::Acceptable;
  }
  return textPolicy_ ? QValidator::Intermediate : QValidator::Invalid;
}

void AdInputNumber::fixup(QString& text) const {
  if (!valueModel_) {
    return;
  }

  const auto parser = [this](const QString& source) {
    return textPolicy_ ? textPolicy_->parseText(source) : source;
  };
  const QVariant parsed = valueModel_->parseInputText(text, parser, false);
  if (!text.trimmed().isEmpty() && !parsed.isValid()) {
    text = displayTextForValue(value_, false, text);
    return;
  }

  const QVariant next = valueModel_->committedValue(parsed);
  text = displayTextForValue(next, false, text);
}

void AdInputNumber::stepBy(int steps) {
  if (steps == 0) {
    return;
  }
  performStep(steps, currentStepEmitter_);
  currentStepEmitter_ = StepEmitter::Handler;
}

AdInputNumber::StepEnabled AdInputNumber::stepEnabled() const {
  if (!isEnabled() || readOnly_) {
    return StepNone;
  }

  StepEnabled enabled = StepNone;
  if (!valueModel_ || !valueModel_->isStepDisabled(value_, true)) {
    enabled |= StepUpEnabled;
  }
  if (!valueModel_ || !valueModel_->isStepDisabled(value_, false)) {
    enabled |= StepDownEnabled;
  }
  return enabled;
}

}  // namespace adqt::widgets
