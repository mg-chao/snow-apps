#include "color_picker.h"

#include "color_picker_style.h"
#include "combo_box.h"
#include "detail/color_picker_value_model.h"
#include "detail/timing_hub.h"
#include "interaction_overlay_manager.h"
#include "input.h"
#include "input_number.h"
#include "slider.h"
#include "theme/theme_manager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QBrush>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMargins>
#include <QMap>
#include <QMetaType>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace adqt::widgets {

using ColorValue = AdColorSelection;
using GradientStop = AdColorGradientStop;
using QtColorValue = AdColorValue;

namespace {

QtColorValue toQtColorValue(const ColorValue& value) { return toColorValue(value); }

ColorValue toLegacyColorValue(const QtColorValue& value) { return toColorSelection(value); }

void setInputNumberValueIfChanged(AdInputNumber* input, double value) {
  if (!input) {
    return;
  }
  if (!input->hasValue() || !qFuzzyCompare(input->value() + 1.0, value + 1.0)) {
    input->setValue(value);
    return;
  }

  // While a channel editor owns focus, retain the original setter behavior so
  // partially typed text can still be normalized. Otherwise the displayed
  // numeric state is already current and a full input visual refresh is wasted.
  QWidget* focused = QApplication::focusWidget();
  const bool editing =
      input->hasFocus() || focused == input || (focused && input->isAncestorOf(focused));
  if (editing) {
    input->setValue(value);
  }
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

qreal snapToDevicePixelCoord(qreal value, qreal dpr) {
  if (dpr <= 0.0) {
    return value;
  }
  return qRound(value * dpr) / dpr;
}

constexpr int kCheckerBrushCacheMaxEntries = 32;
constexpr int kInteractiveEditorRefreshIntervalMs = 16;

struct CheckerBrushCacheKey {
  int cellSize = 0;
  QRgb light = 0;
  QRgb dark = 0;

  bool operator==(const CheckerBrushCacheKey& other) const {
    return cellSize == other.cellSize && light == other.light && dark == other.dark;
  }
};

size_t qHash(const CheckerBrushCacheKey& key, size_t seed) {
  return qHashMulti(seed, key.cellSize, key.light, key.dark);
}

QHash<CheckerBrushCacheKey, QBrush>& checkerBrushCache() {
  static QHash<CheckerBrushCacheKey, QBrush> cache;
  return cache;
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

void stabilizeWidgetLayoutTree(QWidget* widget) {
  if (!widget) {
    return;
  }

  widget->ensurePolished();
  if (QLayout* layout = widget->layout()) {
    layout->invalidate();
    layout->activate();
  }

  const QList<QWidget*> directChildren =
      widget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
  for (QWidget* child : directChildren) {
    stabilizeWidgetLayoutTree(child);
  }

  if (QLayout* layout = widget->layout()) {
    layout->activate();
  }
  widget->updateGeometry();
}

}  // namespace

class ColorSaturationPanel final : public QWidget {
 public:
  explicit ColorSaturationPanel(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(AdColorPicker::tr("Color selection area"));
    refreshAccessibleDescription();
  }

  void setHue(int value) {
    const int clamped = ((value % 360) + 360) % 360;
    if (hue_ == clamped) {
      return;
    }
    hue_ = clamped;
    // A hue change invalidates the static layer. Do not eagerly rebuild it:
    // hue-slider drags change this value every frame and are faster on the
    // direct path than rasterizing a one-frame cache.
    backgroundCacheRequested_ = false;
    update();
  }

  void setSaturationBrightness(double saturation, double brightness) {
    const double sat = std::clamp(saturation, 0.0, 1.0);
    const double bri = std::clamp(brightness, 0.0, 1.0);
    const bool changed = !qFuzzyCompare(saturation_ + 1.0, sat + 1.0) ||
                         !qFuzzyCompare(brightness_ + 1.0, bri + 1.0);
    saturation_ = sat;
    brightness_ = bri;
    if (changed) {
      backgroundCacheRequested_ = true;
      update();
    }
    refreshAccessibleDescription();
  }

  int hue() const { return hue_; }
  double saturation() const { return saturation_; }
  double brightness() const { return brightness_; }

  void setChangeCallback(
      std::function<void(double saturation, double brightness, bool completed)> callback) {
    changeCallback_ = std::move(callback);
  }

 protected:
  void changeEvent(QEvent* event) override {
    QWidget::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange) {
      setAccessibleName(AdColorPicker::tr("Color selection area"));
      refreshAccessibleDescription();
    }
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF panelRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (panelRect.width() <= 1.0 || panelRect.height() <= 1.0) {
      return;
    }

    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    if (!hasCurrentBackgroundCache(size(), dpr) && backgroundCacheRequested_) {
      ensureBackgroundCache(size(), dpr, panelRect);
    }
    if (hasCurrentBackgroundCache(size(), dpr)) {
      backgroundCacheRequested_ = false;
      painter.drawImage(0, 0, backgroundCache_);
    } else {
      drawStaticBackground(painter, panelRect);
    }

    // Handler size from Ant Design tokens (colorPickerHandlerSize = 16).
    constexpr qreal kHandlerOuterRadius = 8.0;
    constexpr qreal kHandlerInnerRadius = 5.0;
    const qreal handleX = panelRect.left() + saturation_ * panelRect.width();
    const qreal handleY = panelRect.top() + (1.0 - brightness_) * panelRect.height();
    const QPointF handleCenter(handleX, handleY);

    // Draw outer shadow/border effect (AntD uses box-shadow)
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 25));
    painter.drawEllipse(handleCenter, kHandlerOuterRadius, kHandlerOuterRadius);

    // Draw inner white ring
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor("#ffffff"), 1.5));
    painter.drawEllipse(handleCenter, kHandlerInnerRadius, kHandlerInnerRadius);
  }

  void mousePressEvent(QMouseEvent* event) override {
    QWidget::mousePressEvent(event);
    if (!event || event->button() != Qt::LeftButton || !isEnabled()) {
      return;
    }
    setFocus(Qt::MouseFocusReason);
    dragging_ = true;
    updateFromPoint(event->position(), false);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    QWidget::mouseMoveEvent(event);
    if (!event || !dragging_ || !isEnabled()) {
      return;
    }
    updateFromPoint(event->position(), false);
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    QWidget::mouseReleaseEvent(event);
    if (!event || event->button() != Qt::LeftButton || !isEnabled()) {
      return;
    }
    const bool wasDragging = dragging_;
    dragging_ = false;
    if (!wasDragging) {
      return;
    }
    updateFromPoint(event->position(), true);
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (!event || !isEnabled()) {
      QWidget::keyPressEvent(event);
      return;
    }

    const bool fastStep = event->modifiers().testFlag(Qt::ShiftModifier);
    const double step = fastStep ? 0.1 : 0.01;
    double nextSat = saturation_;
    double nextBri = brightness_;
    bool handled = true;

    switch (event->key()) {
      case Qt::Key_Left:
        nextSat -= step;
        break;
      case Qt::Key_Right:
        nextSat += step;
        break;
      case Qt::Key_Up:
        nextBri += step;
        break;
      case Qt::Key_Down:
        nextBri -= step;
        break;
      case Qt::Key_Home:
        nextSat = 0.0;
        break;
      case Qt::Key_End:
        nextSat = 1.0;
        break;
      case Qt::Key_PageUp:
        nextBri = 1.0;
        break;
      case Qt::Key_PageDown:
        nextBri = 0.0;
        break;
      default:
        handled = false;
        break;
    }

    if (!handled) {
      QWidget::keyPressEvent(event);
      return;
    }

    const double clampedSat = std::clamp(nextSat, 0.0, 1.0);
    const double clampedBri = std::clamp(nextBri, 0.0, 1.0);
    const bool changed = !qFuzzyCompare(saturation_ + 1.0, clampedSat + 1.0) ||
                         !qFuzzyCompare(brightness_ + 1.0, clampedBri + 1.0);
    saturation_ = clampedSat;
    brightness_ = clampedBri;
    if (changed) {
      backgroundCacheRequested_ = true;
      update();
      refreshAccessibleDescription();
      if (changeCallback_) {
        changeCallback_(saturation_, brightness_, true);
      }
    }
    event->accept();
  }

 private:
  // The hue field, overlays, and rounded border are unchanged while saturation or
  // brightness is dragged. Keep that static layer in a DPR-aware image so a drag
  // frame only has to composite it and draw the selection handle.
  bool hasCurrentBackgroundCache(const QSize& logicalSize, qreal dpr) const {
    return !backgroundCache_.isNull() && backgroundCacheSize_ == logicalSize &&
           qFuzzyCompare(backgroundCacheDpr_ + 1.0, dpr + 1.0) && backgroundCacheHue_ == hue_ &&
           backgroundCacheBase_ == palette().color(backgroundRole());
  }

  void ensureBackgroundCache(const QSize& logicalSize, qreal dpr, const QRectF& panelRect) {
    const QColor cacheBackground = palette().color(backgroundRole());
    // A transparent widget background must remain composited by the real paint
    // device. In that uncommon case, bypass the cache rather than baking in an
    // incorrect backdrop.
    if (cacheBackground.alpha() < 255) {
      backgroundCache_ = QImage();
      backgroundCacheSize_ = QSize();
      backgroundCacheDpr_ = 0.0;
      backgroundCacheHue_ = -1;
      backgroundCacheBase_ = QColor();
      backgroundCacheRequested_ = false;
      return;
    }

    if (hasCurrentBackgroundCache(logicalSize, dpr)) {
      backgroundCacheRequested_ = false;
      return;
    }

    if (logicalSize.width() <= 0 || logicalSize.height() <= 0 || dpr <= 0.0) {
      backgroundCache_ = QImage();
      backgroundCacheSize_ = QSize();
      backgroundCacheDpr_ = 0.0;
      backgroundCacheHue_ = -1;
      backgroundCacheBase_ = QColor();
      backgroundCacheRequested_ = false;
      return;
    }

    QImage cache(logicalSize * dpr, QImage::Format_ARGB32_Premultiplied);
    cache.setDevicePixelRatio(dpr);
    cache.fill(cacheBackground);

    QPainter cachePainter(&cache);
    cachePainter.setRenderHint(QPainter::Antialiasing, true);
    drawStaticBackground(cachePainter, panelRect);
    cachePainter.end();

    backgroundCache_ = cache;
    backgroundCacheSize_ = logicalSize;
    backgroundCacheDpr_ = dpr;
    backgroundCacheHue_ = hue_;
    backgroundCacheBase_ = cacheBackground;
    backgroundCacheRequested_ = false;
  }

  void drawStaticBackground(QPainter& painter, const QRectF& panelRect) const {
    constexpr qreal kSaturationPanelRadius = 4.0;
    QPainterPath clipPath;
    clipPath.addRoundedRect(panelRect, kSaturationPanelRadius, kSaturationPanelRadius);
    painter.setClipPath(clipPath);
    drawBackgroundLayer(painter, panelRect, panelRect);
    drawBorder(painter, panelRect);
  }

  void drawBackgroundLayer(QPainter& painter, const QRectF& gradientRect,
                           const QRectF& fillRect) const {
    painter.fillRect(fillRect, QColor::fromHsv(hue_, 255, 255));

    QLinearGradient whiteOverlay(gradientRect.topLeft(), gradientRect.topRight());
    whiteOverlay.setColorAt(0.0, QColor(255, 255, 255));
    whiteOverlay.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(fillRect, whiteOverlay);

    QLinearGradient blackOverlay(gradientRect.topLeft(), gradientRect.bottomLeft());
    blackOverlay.setColorAt(0.0, QColor(0, 0, 0, 0));
    blackOverlay.setColorAt(1.0, QColor(0, 0, 0));
    painter.fillRect(fillRect, blackOverlay);
  }

  void drawBorder(QPainter& painter, const QRectF& panelRect) const {
    // Preserve the original unclipped border pass: this avoids blending the
    // rounded edge into the cached pixmap and keeps its rasterization exact.
    constexpr qreal kSaturationPanelRadius = 4.0;
    constexpr qreal kBorderAlpha = 0.08;
    painter.setClipping(false);
    painter.setPen(QPen(QColor(0, 0, 0, static_cast<int>(255 * kBorderAlpha)), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(panelRect, kSaturationPanelRadius, kSaturationPanelRadius);
  }

  void refreshAccessibleDescription() {
    setAccessibleDescription(
        AdColorPicker::tr("Hue %1, saturation %2 percent, brightness %3 percent")
            .arg(hue_)
            .arg(qRound(saturation_ * 100.0))
            .arg(qRound(brightness_ * 100.0)));
  }

  void updateFromPoint(const QPointF& point, bool completed) {
    const QRectF panelRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (panelRect.width() <= 1.0 || panelRect.height() <= 1.0) {
      return;
    }

    const qreal clampedX = std::clamp(point.x(), panelRect.left(), panelRect.right());
    const qreal clampedY = std::clamp(point.y(), panelRect.top(), panelRect.bottom());
    const double nextSat = std::clamp((clampedX - panelRect.left()) / panelRect.width(), 0.0, 1.0);
    const double nextBri =
        std::clamp(1.0 - (clampedY - panelRect.top()) / panelRect.height(), 0.0, 1.0);

    const bool changed = !qFuzzyCompare(saturation_ + 1.0, nextSat + 1.0) ||
                         !qFuzzyCompare(brightness_ + 1.0, nextBri + 1.0);
    saturation_ = nextSat;
    brightness_ = nextBri;
    if (changed) {
      backgroundCacheRequested_ = true;
      update();
    }

    // Pointer moves can arrive much faster than the display refresh rate. The
    // live color callback still runs for every move, while accessibility text
    // is published once for the completed value to avoid flooding assistive
    // technology with transient descriptions.
    if (completed) {
      refreshAccessibleDescription();
    }

    if (changeCallback_ && (changed || completed)) {
      changeCallback_(saturation_, brightness_, completed);
    }
  }

  int hue_ = 215;
  double saturation_ = 0.91;
  double brightness_ = 1.0;
  bool dragging_ = false;
  std::function<void(double saturation, double brightness, bool completed)> changeCallback_;
  QImage backgroundCache_;
  QSize backgroundCacheSize_;
  qreal backgroundCacheDpr_ = 0.0;
  int backgroundCacheHue_ = -1;
  QColor backgroundCacheBase_;
  bool backgroundCacheRequested_ = false;
};

namespace {

class ColorPickerTriggerFrame final : public QAbstractButton {
 public:
  explicit ColorPickerTriggerFrame(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(AdColorPicker::tr("Color picker trigger"));
    setAccessibleDescription(AdColorPicker::tr("Open or close the color picker popup"));
  }

  void setVisualStyle(const QColor& background, const QColor& border, qreal borderWidth,
                      qreal radius) {
    const qreal normalizedWidth = std::max<qreal>(0.0, borderWidth);
    const qreal normalizedRadius = std::max<qreal>(0.0, radius);
    const bool changed = background_ != background || border_ != border ||
                         !qFuzzyCompare(borderWidth_ + 1.0, normalizedWidth + 1.0) ||
                         !qFuzzyCompare(radius_ + 1.0, normalizedRadius + 1.0);
    background_ = background;
    border_ = border;
    borderWidth_ = normalizedWidth;
    radius_ = normalizedRadius;
    if (changed) {
      update();
    }
  }

  QColor borderColor() const { return border_; }
  qreal borderWidth() const { return borderWidth_; }
  qreal radius() const { return radius_; }

 protected:
  void changeEvent(QEvent* event) override {
    QAbstractButton::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange) {
      setAccessibleName(AdColorPicker::tr("Color picker trigger"));
      setAccessibleDescription(AdColorPicker::tr("Open or close the color picker popup"));
    }
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    const QRectF fillRect(rect());
    const qreal borderWidth = std::max<qreal>(0.0, borderWidth_);
    const bool hasVisibleBorder = borderWidth > 0.0 && border_.alpha() > 0;
    const qreal half = borderWidth / 2.0;
    const QRectF rawBorderRect =
        fillRect.adjusted(half + 0.5, half + 0.5, -half - 0.5, -half - 0.5);
    if (!fillRect.isValid() || fillRect.width() <= 0.0 || fillRect.height() <= 0.0) {
      return;
    }
    if (hasVisibleBorder && (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 ||
                             rawBorderRect.height() <= 0.0)) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    const QRectF borderRect =
        hasVisibleBorder ? snapRectToDevicePixels(rawBorderRect, dpr) : rawBorderRect;

    const qreal topLeft = std::max<qreal>(0.0, radius_);
    const qreal topRight = std::max<qreal>(0.0, radius_);
    const qreal bottomRight = std::max<qreal>(0.0, radius_);
    const qreal bottomLeft = std::max<qreal>(0.0, radius_);

    const QRectF shapeRect = hasVisibleBorder ? borderRect : fillRect;
    const QPainterPath fillPath =
        roundedRectPath(shapeRect, topLeft, topRight, bottomRight, bottomLeft);
    painter.fillPath(fillPath, background_);

    if (hasVisibleBorder) {
      QPen borderPen(border_, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
      painter.setPen(borderPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(roundedRectPath(borderRect, topLeft, topRight, bottomRight, bottomLeft));
    }
  }

 private:
  QColor background_ = QColor(255, 255, 255);
  QColor border_ = QColor(217, 217, 217);
  qreal borderWidth_ = 1.0;
  qreal radius_ = 6.0;
};

constexpr char kPresetSwatchObjectName[] = "ad-color-picker-preset-swatch";
constexpr char kPresetGroupObjectName[] = "ad-color-picker-preset-group";
constexpr char kPresetHeaderObjectName[] = "ad-color-picker-preset-header";
constexpr char kPresetBodyObjectName[] = "ad-color-picker-preset-body";
constexpr char kPresetItemsObjectName[] = "ad-color-picker-preset-items";
constexpr char kPresetEmptyObjectName[] = "ad-color-picker-preset-empty";
constexpr char kFormatSelectObjectName[] = "ad-color-picker-format-select";
constexpr char kFormatSelectGapObjectName[] = "ad-color-picker-format-select-gap";
constexpr char kFormatAlphaGapObjectName[] = "ad-color-picker-format-alpha-gap";
constexpr int kTransparencyCell = 6;
constexpr int kFormatSelectPopupWidth = 68;

QString stripInlineUnitText(const QString& text, const QString& unitText) {
  if (unitText.isEmpty()) {
    return text.trimmed();
  }

  QString stripped = text;
  const QRegularExpression unitPattern(
      QStringLiteral("\\s*%1\\s*").arg(QRegularExpression::escape(unitText)));
  stripped.remove(unitPattern);
  return stripped.trimmed();
}

void clampInlineUnitCursor(QLineEdit* editor, const QString& unitText) {
  if (!editor || unitText.isEmpty() || editor->hasSelectedText()) {
    return;
  }

  const QString text = editor->text();
  if (!text.endsWith(unitText)) {
    return;
  }

  const int maxCursorPosition = std::max(0, static_cast<int>(text.size() - unitText.size()));
  if (editor->cursorPosition() > maxCursorPosition) {
    editor->setCursorPosition(maxCursorPosition);
  }
}

class InlineUnitCursorGuard final : public QObject {
 public:
  InlineUnitCursorGuard(QLineEdit* editor, QString unitText)
      : QObject(editor), editor_(editor), unitText_(std::move(unitText)) {
    if (!editor_ || unitText_.isEmpty()) {
      return;
    }

    editor_->installEventFilter(this);
    QObject::connect(editor_, &QLineEdit::cursorPositionChanged, editor_,
                     [editor = editor_, unitText = unitText_](int, int) {
                       clampInlineUnitCursor(editor, unitText);
                     });
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == editor_ && event &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonDblClick ||
         event->type() == QEvent::MouseButtonRelease)) {
      QPointer<QLineEdit> editor = editor_;
      const QString unitText = unitText_;
      QTimer::singleShot(0, editor_.data(),
                         [editor, unitText]() { clampInlineUnitCursor(editor, unitText); });
    }

    return QObject::eventFilter(watched, event);
  }

 private:
  QPointer<QLineEdit> editor_;
  QString unitText_;
};

class InlineUnitInputNumberTextPolicy final : public AdInputNumberTextPolicy {
 public:
  InlineUnitInputNumberTextPolicy(AdInputNumber* input, QString unitText)
      : AdInputNumberTextPolicy(input), input_(input), unitText_(std::move(unitText)) {}

  QString formatText(const QString& canonicalText, bool editing,
                     const QString& inputText) const override {
    if (canonicalText.trimmed().isEmpty()) {
      return editing ? inputText : QString();
    }

    bool ok = false;
    const qlonglong numericValue = canonicalText.toLongLong(&ok);
    QString text = (ok && input_) ? input_->locale().toString(numericValue) : canonicalText;
    if (text.isEmpty()) {
      text = stripInlineUnitText(inputText, unitText_);
    }
    return text.isEmpty() ? QString() : text + unitText_;
  }

  QString parseText(const QString& text) const override {
    return stripInlineUnitText(text, unitText_);
  }

 private:
  QPointer<AdInputNumber> input_;
  QString unitText_;
};

void configureColorPickerInlineUnitInput(AdInputNumber* input, const QString& unitText) {
  if (!input || unitText.isEmpty()) {
    return;
  }

  input->setTextPolicy(new InlineUnitInputNumberTextPolicy(input, unitText));

  if (QLineEdit* editor = input->findChild<QLineEdit*>()) {
    new InlineUnitCursorGuard(editor, unitText);
    clampInlineUnitCursor(editor, unitText);
  }
}

AdInputNumber* createColorPickerStepperInput(QWidget* parent, const QString& objectName, int min,
                                             int max, int step = 1, int precision = 0,
                                             const QString& inlineUnitText = QString(),
                                             const QString& placeholder = QString(),
                                             int fixedWidth = 0) {
  auto* input = new AdInputNumber(parent);
  input->setObjectName(objectName);
  input->setProperty("ad-flex-min-width-zero", true);
  input->setMinimum(min);
  input->setMaximum(max);
  input->setSingleStep(step);
  input->setDecimals(precision);
  if (!inlineUnitText.isEmpty()) {
    configureColorPickerInlineUnitInput(input, inlineUnitText);
  }
  if (!placeholder.isEmpty()) {
    input->setPlaceholderText(placeholder);
  }
  if (fixedWidth > 0) {
    input->setFixedWidth(fixedWidth);
  }
  return input;
}

void bindColorPickerStepperInput(QObject* context, AdInputNumber* input,
                                 const std::function<void()>& onValueChanged,
                                 const std::function<void()>& onEditingFinished) {
  if (!context || !input) {
    return;
  }

  QObject::connect(input, &AdInputNumber::valueChanged, context, [onValueChanged](double) {
    if (onValueChanged) {
      onValueChanged();
    }
  });
  QObject::connect(input, &AdInputNumber::editingFinished, context, [onEditingFinished]() {
    if (onEditingFinished) {
      onEditingFinished();
    }
  });
}

void applyColorPickerStepperStyle(AdInputNumber* input, int controlHeight, int horizontalPadding,
                                  int inputFontSize, int controlWidth, int handleWidth,
                                  bool disabled, int fixedWidth = 0, bool visible = true) {
  if (!input) {
    return;
  }

  AdInputNumber::AppearanceOverrides overrides = input->appearanceOverrides();
  bool overridesChanged = false;
  if (!overrides.metrics.controlHeight.has_value() ||
      overrides.metrics.controlHeight.value() != controlHeight) {
    overrides.metrics.controlHeight = controlHeight;
    overridesChanged = true;
  }
  if (!overrides.metrics.horizontalPadding.has_value() ||
      overrides.metrics.horizontalPadding.value() != horizontalPadding) {
    overrides.metrics.horizontalPadding = horizontalPadding;
    overridesChanged = true;
  }
  if (!overrides.metrics.inputFontSize.has_value() ||
      overrides.metrics.inputFontSize.value() != inputFontSize) {
    overrides.metrics.inputFontSize = inputFontSize;
    overridesChanged = true;
  }
  if (!overrides.metrics.controlWidth.has_value() ||
      overrides.metrics.controlWidth.value() != controlWidth) {
    overrides.metrics.controlWidth = controlWidth;
    overridesChanged = true;
  }
  if (!overrides.metrics.handleWidth.has_value() ||
      overrides.metrics.handleWidth.value() != handleWidth) {
    overrides.metrics.handleWidth = handleWidth;
    overridesChanged = true;
  }
  if (overridesChanged) {
    input->setAppearanceOverrides(overrides);
  }

  input->setControlSize(AdInputNumber::ControlSize::Small);
  input->setMinimumHeight(controlHeight);
  input->setMaximumHeight(controlHeight);
  input->setEnabled(!disabled);
  input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  input->setMinimumWidth(0);
  if (fixedWidth > 0) {
    input->setFixedWidth(fixedWidth);
    input->setVisible(visible);
  }
}

int formatSelectIconSizeFromMap(const adqt::theme::ThemeMapToken& map) {
  return std::max(10, qRound(map.fontSizeSM));
}

int sliderGroupGapFromMetrics(int marginSM, int sliderMarginCross) {
  // AntD slider root height is rail-height-based and handle ring can overflow.
  // Qt sliders include cross padding in widget height, so subtract it here.
  return std::max(0, marginSM - sliderMarginCross * 2);
}

int sliderSectionGapFromMetrics(int marginSM, int sliderMarginCross) {
  // Keep perceived block spacing consistent with AntD's overflow visuals.
  return std::max(0, marginSM - sliderMarginCross);
}

int sliderContainerHeightFromMetrics(int previewSwatchSize, int sliderVisualHeight,
                                     int sliderGroupGap, bool disabledAlpha) {
  const int visibleSliderCount = disabledAlpha ? 1 : 2;
  const int groupHeight =
      sliderVisualHeight * visibleSliderCount + ((visibleSliderCount > 1) ? sliderGroupGap : 0);
  return std::max(previewSwatchSize, groupHeight);
}

QString formatPercent(double value) {
  QString text = QString::number(value, 'f', 3);
  while (text.contains(QLatin1Char('.')) &&
         (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) {
    text.chop(1);
    if (text.endsWith(QLatin1Char('.'))) {
      text.chop(1);
      break;
    }
  }
  if (text.isEmpty()) {
    return QStringLiteral("0");
  }
  return text;
}

QString colorToHexRgbLower(const QColor& color) { return color.name(QColor::HexRgb).toLower(); }

bool parseCssHexColor(const QString& input, QColor* out) {
  if (!out) {
    return false;
  }

  const QString trimmed = input.trimmed();
  if (!trimmed.startsWith(QLatin1Char('#'))) {
    return false;
  }

  const QString hex = trimmed.mid(1);
  const qsizetype length = hex.size();
  if (length != 3 && length != 4 && length != 6 && length != 8) {
    return false;
  }

  static const QRegularExpression kHexPattern(QStringLiteral("^[0-9a-fA-F]+$"));
  if (!kHexPattern.match(hex).hasMatch()) {
    return false;
  }

  bool ok = false;
  int red = 0;
  int green = 0;
  int blue = 0;
  int alpha = 255;

  if (length == 3 || length == 4) {
    red = QString(hex.at(0)).repeated(2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    green = QString(hex.at(1)).repeated(2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    blue = QString(hex.at(2)).repeated(2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    if (length == 4) {
      alpha = QString(hex.at(3)).repeated(2).toInt(&ok, 16);
      if (!ok) {
        return false;
      }
    }
  } else {
    red = hex.mid(0, 2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    green = hex.mid(2, 2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    blue = hex.mid(4, 2).toInt(&ok, 16);
    if (!ok) {
      return false;
    }
    if (length == 8) {
      alpha = hex.mid(6, 2).toInt(&ok, 16);
      if (!ok) {
        return false;
      }
    }
  }

  const QColor parsed(red, green, blue, alpha);
  if (!parsed.isValid()) {
    return false;
  }
  *out = parsed;
  return true;
}

QString colorToRgbCssCompact(const QColor& color) {
  if (!color.isValid()) {
    return QString();
  }
  if (color.alpha() >= 255) {
    return QStringLiteral("rgb(%1,%2,%3)").arg(color.red()).arg(color.green()).arg(color.blue());
  }
  return QStringLiteral("rgba(%1,%2,%3,%4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(formatPercent(color.alphaF()));
}

QString colorToTriggerHexText(const QColor& color) {
  if (!color.isValid()) {
    return QString();
  }

  QString hex = colorToHexRgbLower(color).toUpper();
  if (color.alpha() >= 255) {
    return hex;
  }

  const int alphaPercent = std::clamp(qRound(color.alphaF() * 100.0), 0, 100);
  return QStringLiteral("%1,%2%").arg(hex).arg(alphaPercent);
}

QBrush makeCheckerBrush(int cellSize, const QColor& light = QColor(255, 255, 255),
                        const QColor& dark = QColor(0, 0, 0, 20)) {
  const int cell = std::max(2, cellSize);
  const CheckerBrushCacheKey key{cell, light.rgba(), dark.rgba()};
  auto& cache = checkerBrushCache();
  const auto cached = cache.constFind(key);
  if (cached != cache.constEnd()) {
    return cached.value();
  }

  QPixmap pixmap(cell * 2, cell * 2);
  pixmap.fill(light);

  QPainter painter(&pixmap);
  painter.fillRect(QRect(0, 0, cell, cell), dark);
  painter.fillRect(QRect(cell, cell, cell, cell), dark);
  painter.end();

  QBrush brush(pixmap);
  brush.setStyle(Qt::TexturePattern);
  if (cache.size() >= kCheckerBrushCacheMaxEntries) {
    cache.clear();
  }
  cache.insert(key, brush);
  return brush;
}

QBrush makeHueBrush() {
  static const QBrush brush = []() {
    QLinearGradient gradient(0.0, 0.0, 1.0, 0.0);
    gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
    gradient.setColorAt(0.0, QColor("#ff0000"));
    gradient.setColorAt(1.0 / 6.0, QColor("#ffff00"));
    gradient.setColorAt(2.0 / 6.0, QColor("#00ff00"));
    gradient.setColorAt(3.0 / 6.0, QColor("#00ffff"));
    gradient.setColorAt(4.0 / 6.0, QColor("#0000ff"));
    gradient.setColorAt(5.0 / 6.0, QColor("#ff00ff"));
    gradient.setColorAt(1.0, QColor("#ff0000"));
    return QBrush(gradient);
  }();
  return brush;
}

QBrush makeAlphaBrush(const QColor& color) {
  QColor transparent = color;
  transparent.setAlpha(0);
  QColor opaque = color;
  opaque.setAlpha(255);

  QLinearGradient gradient(0.0, 0.0, 1.0, 0.0);
  gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
  gradient.setColorAt(0.0, transparent);
  gradient.setColorAt(1.0, opaque);
  return QBrush(gradient);
}

class ColorPickerSwatch final : public QWidget {
 public:
  explicit ColorPickerSwatch(QWidget* parent = nullptr) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  void setFrameStyle(const QColor& border, qreal borderWidth, qreal radius,
                     bool dashedBorder = false) {
    const qreal normalizedWidth = std::max<qreal>(0.0, borderWidth);
    const qreal normalizedRadius = std::max<qreal>(0.0, radius);
    const bool changed = border_ != border || dashedBorder_ != dashedBorder ||
                         !qFuzzyCompare(borderWidth_ + 1.0, normalizedWidth + 1.0) ||
                         !qFuzzyCompare(radius_ + 1.0, normalizedRadius + 1.0);
    border_ = border;
    borderWidth_ = normalizedWidth;
    radius_ = normalizedRadius;
    dashedBorder_ = dashedBorder;
    if (changed) {
      update();
    }
  }

  void setCheckerColors(const QColor& light, const QColor& dark, int cellSize) {
    const int normalizedCell = std::max(2, cellSize);
    const bool changed =
        checkerLight_ != light || checkerDark_ != dark || checkerCellSize_ != normalizedCell;
    checkerLight_ = light;
    checkerDark_ = dark;
    checkerCellSize_ = normalizedCell;
    if (changed) {
      update();
    }
  }

  void setSolidFill(const QColor& color) {
    const bool changed = fillMode_ != FillMode::Solid || solidFill_ != color;
    fillMode_ = FillMode::Solid;
    solidFill_ = color;
    if (changed) {
      update();
    }
  }

  void setGradientFill(const QVector<QPair<qreal, QColor>>& stops) {
    const bool changed = fillMode_ != FillMode::Gradient || gradientStops_ != stops;
    fillMode_ = FillMode::Gradient;
    gradientStops_ = stops;
    if (changed) {
      update();
    }
  }

  void setClearedTriggerFill() {
    if (fillMode_ == FillMode::ClearTrigger) {
      return;
    }
    fillMode_ = FillMode::ClearTrigger;
    update();
  }

  void setClearedPreviewFill() {
    if (fillMode_ == FillMode::ClearPreview) {
      return;
    }
    fillMode_ = FillMode::ClearPreview;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF fillRect(rect());
    if (!fillRect.isValid() || fillRect.width() <= 0.0 || fillRect.height() <= 0.0) {
      return;
    }

    const qreal borderWidth = std::max<qreal>(0.0, borderWidth_);
    const bool hasVisibleBorder = borderWidth > 0.0 && border_.alpha() > 0;
    const qreal half = borderWidth / 2.0;
    const QRectF rawBorderRect =
        fillRect.adjusted(half + 0.5, half + 0.5, -half - 0.5, -half - 0.5);
    if (hasVisibleBorder && (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 ||
                             rawBorderRect.height() <= 0.0)) {
      return;
    }

    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    const QRectF borderRect =
        hasVisibleBorder ? snapRectToDevicePixels(rawBorderRect, dpr) : rawBorderRect;
    const QRectF shapeRect = hasVisibleBorder ? borderRect : fillRect;
    const qreal radius = std::max<qreal>(0.0, radius_);
    const QPainterPath fillPath = roundedRectPath(shapeRect, radius, radius, radius, radius);

    switch (fillMode_) {
      case FillMode::Solid: {
        painter.fillPath(fillPath, makeCheckerBrush(checkerCellSize_, checkerLight_, checkerDark_));
        painter.fillPath(fillPath, solidFill_);
        break;
      }
      case FillMode::Gradient: {
        painter.fillPath(fillPath, makeCheckerBrush(checkerCellSize_, checkerLight_, checkerDark_));
        if (!gradientStops_.isEmpty()) {
          QLinearGradient gradient(0.0, 0.0, 1.0, 0.0);
          gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
          for (const auto& stop : gradientStops_) {
            gradient.setColorAt(std::clamp(static_cast<double>(stop.first), 0.0, 1.0), stop.second);
          }
          painter.fillPath(fillPath, QBrush(gradient));
        }
        break;
      }
      case FillMode::ClearTrigger: {
        painter.fillPath(fillPath, QColor("#ffffff"));
        painter.save();
        painter.setClipPath(fillPath);
        const qreal inset = std::max<qreal>(
            1.0, std::round(std::min(shapeRect.width(), shapeRect.height()) * 0.12));
        const qreal slashWidth = std::max<qreal>(
            2.0, std::round(std::min(shapeRect.width(), shapeRect.height()) * 0.14));
        QPen slashPen(QColor("#ff4d4f"), slashWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(slashPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(shapeRect.left() + inset, shapeRect.top() + inset),
                         QPointF(shapeRect.right() - inset, shapeRect.bottom() - inset));
        painter.restore();
        break;
      }
      case FillMode::ClearPreview:
      default:
        break;
    }

    if (hasVisibleBorder) {
      QPen borderPen(border_, borderWidth, dashedBorder_ ? Qt::DashLine : Qt::SolidLine,
                     Qt::SquareCap, Qt::MiterJoin);
      if (dashedBorder_) {
        borderPen.setDashPattern({2.0, 2.0});
      }
      painter.setPen(borderPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(roundedRectPath(borderRect, radius, radius, radius, radius));
    }
  }

 private:
  enum class FillMode : std::uint8_t {
    Solid,
    Gradient,
    ClearTrigger,
    ClearPreview,
  };

  QColor border_ = QColor("#f0f0f0");
  qreal borderWidth_ = 1.0;
  qreal radius_ = 4.0;
  bool dashedBorder_ = false;

  QColor checkerLight_ = QColor("#ffffff");
  QColor checkerDark_ = QColor("#f0f0f0");
  int checkerCellSize_ = 6;

  FillMode fillMode_ = FillMode::Solid;
  QColor solidFill_ = QColor("#1677ff");
  QVector<QPair<qreal, QColor>> gradientStops_;
};

class ColorPickerSegmentedFrame final : public QWidget {
 public:
  explicit ColorPickerSegmentedFrame(QWidget* parent = nullptr) : QWidget(parent) {}

  void setVisualStyle(const QColor& background, qreal radius) {
    const qreal normalizedRadius = std::max<qreal>(0.0, radius);
    const bool changed =
        background_ != background || !qFuzzyCompare(radius_ + 1.0, normalizedRadius + 1.0);
    background_ = background;
    radius_ = normalizedRadius;
    if (changed) {
      update();
    }
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)
    const QRectF fillRect(rect());
    if (!fillRect.isValid() || fillRect.width() <= 0.0 || fillRect.height() <= 0.0 ||
        background_.alpha() <= 0) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(roundedRectPath(fillRect, radius_, radius_, radius_, radius_), background_);
  }

 private:
  QColor background_ = QColor("#f5f5f5");
  qreal radius_ = 6.0;
};

class ColorPickerSegmentedButton final : public QPushButton {
 public:
  explicit ColorPickerSegmentedButton(QWidget* parent = nullptr) : QPushButton(parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFlat(true);
    setFocusPolicy(Qt::TabFocus);
  }

  void setVisualStyle(const QColor& textColor, const QColor& checkedTextColor,
                      const QColor& disabledTextColor, const QColor& hoverBackground,
                      const QColor& checkedBackground, const QColor& checkedBorderColor,
                      qreal checkedBorderWidth, qreal radius, int horizontalPadding) {
    const qreal normalizedBorderWidth = std::max<qreal>(0.0, checkedBorderWidth);
    const qreal normalizedRadius = std::max<qreal>(0.0, radius);
    const int normalizedPadding = std::max(0, horizontalPadding);
    const bool changed =
        textColor_ != textColor || checkedTextColor_ != checkedTextColor ||
        disabledTextColor_ != disabledTextColor || hoverBackground_ != hoverBackground ||
        checkedBackground_ != checkedBackground || checkedBorderColor_ != checkedBorderColor ||
        !qFuzzyCompare(checkedBorderWidth_ + 1.0, normalizedBorderWidth + 1.0) ||
        !qFuzzyCompare(radius_ + 1.0, normalizedRadius + 1.0) ||
        horizontalPadding_ != normalizedPadding;
    textColor_ = textColor;
    checkedTextColor_ = checkedTextColor;
    disabledTextColor_ = disabledTextColor;
    hoverBackground_ = hoverBackground;
    checkedBackground_ = checkedBackground;
    checkedBorderColor_ = checkedBorderColor;
    checkedBorderWidth_ = normalizedBorderWidth;
    radius_ = normalizedRadius;
    horizontalPadding_ = normalizedPadding;
    if (changed) {
      updateGeometry();
      update();
    }
  }

  QSize sizeHint() const override {
    const QFontMetrics metrics(font());
    const int width = std::max(QPushButton::sizeHint().width(),
                               metrics.horizontalAdvance(text()) + horizontalPadding_ * 2);
    const int height = std::max(QPushButton::sizeHint().height(), metrics.height() + 8);
    return QSize(width, height);
  }

  QSize minimumSizeHint() const override { return sizeHint(); }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool checked = isChecked();
    const bool hovered = underMouse() || isDown();
    const QRectF outerRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (!outerRect.isValid()) {
      return;
    }

    QColor fillColor(0, 0, 0, 0);
    QColor borderColor(0, 0, 0, 0);
    qreal borderWidth = 0.0;
    if (checked) {
      fillColor = checkedBackground_;
      borderColor = checkedBorderColor_;
      borderWidth = checkedBorderWidth_;
    } else if (hovered) {
      fillColor = hoverBackground_;
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(fillColor);
    if (fillColor.alpha() > 0) {
      painter.drawRoundedRect(outerRect, radius_, radius_);
    }

    if (borderWidth > 0.0 && borderColor.alpha() > 0) {
      painter.setPen(QPen(borderColor, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(outerRect, radius_, radius_);
    }

    QColor textColor = textColor_;
    if (!isEnabled()) {
      textColor = disabledTextColor_;
    } else if (checked) {
      textColor = checkedTextColor_;
    }

    painter.setFont(font());
    painter.setPen(textColor);
    painter.drawText(rect().adjusted(horizontalPadding_, 0, -horizontalPadding_, 0),
                     Qt::AlignCenter, text());
  }

 private:
  QColor textColor_ = QColor("#000000");
  QColor checkedTextColor_ = QColor("#000000");
  QColor disabledTextColor_ = QColor("#bfbfbf");
  QColor hoverBackground_ = QColor(Qt::transparent);
  QColor checkedBackground_ = QColor(Qt::transparent);
  QColor checkedBorderColor_ = QColor(Qt::transparent);
  qreal checkedBorderWidth_ = 0.0;
  qreal radius_ = 0.0;
  int horizontalPadding_ = 0;
};

class ColorPickerPresetsPanel final : public QWidget {
 public:
  explicit ColorPickerPresetsPanel(QWidget* parent = nullptr) : QWidget(parent) {}

  void setVisualStyle(qreal topBorderWidth, const QColor& topBorderColor) {
    const qreal normalizedWidth = std::max<qreal>(0.0, topBorderWidth);
    const bool changed = !qFuzzyCompare(topBorderWidth_ + 1.0, normalizedWidth + 1.0) ||
                         topBorderColor_ != topBorderColor;
    topBorderWidth_ = normalizedWidth;
    topBorderColor_ = topBorderColor;
    if (changed) {
      update();
    }
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    QWidget::paintEvent(event);
    if (topBorderWidth_ <= 0.0 || topBorderColor_.alpha() <= 0) {
      return;
    }
    QPainter painter(this);
    painter.setClipRect(event->rect().intersected(rect()));
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(
        QPen(topBorderColor_, topBorderWidth_, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    const qreal y = topBorderWidth_ * 0.5;
    painter.drawLine(QPointF(0.0, y), QPointF(width(), y));
  }

 private:
  qreal topBorderWidth_ = 0.0;
  QColor topBorderColor_ = QColor(Qt::transparent);
};

class PresetCollapseHeaderButton final : public QAbstractButton {
 public:
  explicit PresetCollapseHeaderButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }

  void setLabel(const QString& label) {
    if (label_ == label) {
      return;
    }
    label_ = label;
    setAccessibleName(AdColorPicker::tr("Preset group %1").arg(label_));
    setAccessibleDescription(AdColorPicker::tr("Expand or collapse preset group"));
    updateGeometry();
    update();
  }

  void setExpanded(bool expanded) {
    if (expanded_ == expanded) {
      return;
    }
    expanded_ = expanded;
    update();
  }

  bool expanded() const { return expanded_; }

  void setVisualStyle(const QFont& font, const QColor& textColor, const QColor& arrowColor,
                      int iconSize, int gap, int height) {
    const int normalizedIconSize = std::max(8, iconSize);
    const int normalizedGap = std::max(0, gap);
    const int normalizedHeight = std::max(16, height);
    const bool changed = font_ != font || textColor_ != textColor || arrowColor_ != arrowColor ||
                         iconSize_ != normalizedIconSize || gap_ != normalizedGap ||
                         height_ != normalizedHeight;
    font_ = font;
    textColor_ = textColor;
    arrowColor_ = arrowColor;
    iconSize_ = normalizedIconSize;
    gap_ = normalizedGap;
    height_ = normalizedHeight;
    setMinimumHeight(height_);
    setMaximumHeight(height_);
    if (changed) {
      updateGeometry();
      update();
    }
  }

  QSize sizeHint() const override {
    const QFontMetrics metrics(font_);
    const int width = std::max(24, iconSize_ + gap_ + metrics.horizontalAdvance(label_) + 8);
    const int height = std::max(16, height_ > 0 ? height_ : metrics.height());
    return QSize(width, height);
  }

  QSize minimumSizeHint() const override { return sizeHint(); }

 protected:
  void changeEvent(QEvent* event) override {
    QAbstractButton::changeEvent(event);
    if (event && event->type() == QEvent::LanguageChange) {
      setAccessibleName(AdColorPicker::tr("Preset group %1").arg(label_));
      setAccessibleDescription(AdColorPicker::tr("Expand or collapse preset group"));
    }
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF contentRect(rect());
    if (!contentRect.isValid() || contentRect.width() <= 0.0 || contentRect.height() <= 0.0) {
      return;
    }

    const int arrowExtent = std::max(8, iconSize_);
    const qreal arrowLeft = contentRect.left();
    const qreal arrowTop = contentRect.center().y() - arrowExtent / 2.0;
    const QRectF arrowRect(arrowLeft, arrowTop, arrowExtent, arrowExtent);

    const qreal inset = std::max<qreal>(1.5, arrowExtent * 0.22);
    painter.setPen(QPen(isEnabled() ? arrowColor_ : textColor_, 1.5, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (expanded_) {
      painter.drawPolyline(QPolygonF(
          {QPointF(arrowRect.left() + inset, arrowRect.top() + arrowRect.height() * 0.35),
           QPointF(arrowRect.center().x(), arrowRect.bottom() - arrowRect.height() * 0.28),
           QPointF(arrowRect.right() - inset, arrowRect.top() + arrowRect.height() * 0.35)}));
    } else {
      painter.drawPolyline(QPolygonF(
          {QPointF(arrowRect.left() + arrowRect.width() * 0.35, arrowRect.top() + inset),
           QPointF(arrowRect.right() - arrowRect.width() * 0.28, arrowRect.center().y()),
           QPointF(arrowRect.left() + arrowRect.width() * 0.35, arrowRect.bottom() - inset)}));
    }

    painter.setFont(font_);
    painter.setPen(isEnabled() ? textColor_ : arrowColor_);
    const QRectF textRect = contentRect.adjusted(arrowExtent + gap_, 0.0, 0.0, 0.0);
    const QFontMetrics textMetrics(painter.font());
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                     textMetrics.elidedText(label_, Qt::ElideRight, qRound(textRect.width())));
  }

 private:
  QString label_;
  QFont font_;
  QColor textColor_ = QColor("#141414");
  QColor arrowColor_ = QColor("#8c8c8c");
  int iconSize_ = 12;
  int gap_ = 4;
  int height_ = 16;
  bool expanded_ = true;
};

class PresetColorButton final : public QAbstractButton {
 public:
  explicit PresetColorButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  void setColorValue(const ColorValue& value) {
    value_ = value;
    setAccessibleName(
        QCoreApplication::translate("adqt::widgets::AdColorPicker", "Preset color %1")
            .arg(detail::ColorPickerValueModel::cssValue(value_)));
    setAccessibleDescription(
        QCoreApplication::translate("adqt::widgets::AdColorPicker", "Apply preset color"));
  }

  const ColorValue& colorValue() const { return value_; }

  void setFrameStyle(const QColor& border, qreal borderWidth, int radius) {
    const qreal normalizedWidth = std::max<qreal>(0.0, borderWidth);
    const int normalizedRadius = std::max(0, radius);
    const bool changed = border_ != border ||
                         !qFuzzyCompare(borderWidth_ + 1.0, normalizedWidth + 1.0) ||
                         radius_ != normalizedRadius;
    border_ = border;
    borderWidth_ = normalizedWidth;
    radius_ = normalizedRadius;
    if (changed) {
      update();
    }
  }

  void setCheckerColors(const QColor& light, const QColor& dark, int cellSize) {
    const int normalizedCell = std::max(2, cellSize);
    const bool changed =
        checkerLight_ != light || checkerDark_ != dark || checkerCellSize_ != normalizedCell;
    checkerLight_ = light;
    checkerDark_ = dark;
    checkerCellSize_ = normalizedCell;
    if (changed) {
      update();
    }
  }

  void setHoverOutlineColor(const QColor& color) {
    if (hoverOutlineColor_ == color) {
      return;
    }
    hoverOutlineColor_ = color;
    update();
  }

  void setCheckmarkColors(const QColor& normalColor, const QColor& brightColor) {
    const bool changed = checkmarkColor_ != normalColor || brightCheckmarkColor_ != brightColor;
    checkmarkColor_ = normalColor;
    brightCheckmarkColor_ = brightColor;
    if (changed) {
      update();
    }
  }

  void setOuterPadding(int padding) {
    const int normalizedPadding = std::max(0, padding);
    if (outerPadding_ == normalizedPadding) {
      return;
    }
    outerPadding_ = normalizedPadding;
    updateGeometry();
    update();
  }

  int outerPadding() const { return outerPadding_; }

  void setSolidFill(const QColor& color) {
    const bool changed = fillMode_ != FillMode::Solid || solidFill_ != color;
    fillMode_ = FillMode::Solid;
    solidFill_ = color;
    if (changed) {
      update();
    }
  }

  void setGradientFill(const QVector<QPair<qreal, QColor>>& stops) {
    const bool changed = fillMode_ != FillMode::Gradient || gradientStops_ != stops;
    fillMode_ = FillMode::Gradient;
    gradientStops_ = stops;
    if (changed) {
      update();
    }
  }

  void setCheckedVisual(bool checkedVisual) {
    if (checkedVisual_ == checkedVisual) {
      return;
    }
    checkedVisual_ = checkedVisual;
    update();
  }

  void setBright(bool bright) {
    if (bright_ == bright) {
      return;
    }
    bright_ = bright;
    update();
  }

 protected:
  void enterEvent(QEnterEvent* event) override {
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF outerRect(rect());
    if (!outerRect.isValid() || outerRect.width() <= 0.0 || outerRect.height() <= 0.0) {
      return;
    }

    const QRectF fillRect =
        outerRect.adjusted(outerPadding_, outerPadding_, -outerPadding_, -outerPadding_);
    if (!fillRect.isValid() || fillRect.width() <= 0.0 || fillRect.height() <= 0.0) {
      return;
    }

    const qreal borderWidth = std::max<qreal>(0.0, borderWidth_);
    const bool hasVisibleBorder = borderWidth > 0.0 && border_.alpha() > 0;
    const qreal half = borderWidth / 2.0;
    const QRectF rawBorderRect =
        fillRect.adjusted(half + 0.5, half + 0.5, -half - 0.5, -half - 0.5);
    if (hasVisibleBorder && (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 ||
                             rawBorderRect.height() <= 0.0)) {
      return;
    }

    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    const QRectF borderRect =
        hasVisibleBorder ? snapRectToDevicePixels(rawBorderRect, dpr) : rawBorderRect;
    const QRectF shapeRect = hasVisibleBorder ? borderRect : fillRect;
    const qreal radius = std::max<qreal>(0.0, radius_);
    const QPainterPath fillPath = roundedRectPath(shapeRect, radius, radius, radius, radius);

    painter.fillPath(fillPath, makeCheckerBrush(checkerCellSize_, checkerLight_, checkerDark_));
    if (fillMode_ == FillMode::Gradient) {
      if (!gradientStops_.isEmpty()) {
        QLinearGradient gradient(0.0, 0.0, 1.0, 0.0);
        gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
        for (const auto& stop : gradientStops_) {
          gradient.setColorAt(std::clamp(static_cast<double>(stop.first), 0.0, 1.0), stop.second);
        }
        painter.fillPath(fillPath, QBrush(gradient));
      }
    } else {
      painter.fillPath(fillPath, solidFill_);
    }

    if (hasVisibleBorder) {
      painter.setPen(QPen(border_, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(roundedRectPath(borderRect, radius, radius, radius, radius));
    }

    if (hovered_ && isEnabled()) {
      const qreal outlineWidth = std::max<qreal>(1.0, borderWidth_);
      const qreal outlineHalf = outlineWidth / 2.0;
      const QRectF rawOutlineRect = outerRect.adjusted(outlineHalf + 0.5, outlineHalf + 0.5,
                                                       -outlineHalf - 0.5, -outlineHalf - 0.5);
      const QRectF outlineRect = snapRectToDevicePixels(rawOutlineRect, dpr);
      const qreal outlineRadius = std::max<qreal>(0.0, radius_ + outerPadding_);
      painter.setPen(
          QPen(hoverOutlineColor_, outlineWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(
          roundedRectPath(outlineRect, outlineRadius, outlineRadius, outlineRadius, outlineRadius));
    }

    if (checkedVisual_) {
      const QColor markColor = bright_ ? brightCheckmarkColor_ : checkmarkColor_;
      painter.save();
      painter.setClipPath(fillPath);
      const qreal width = shapeRect.width();
      const qreal height = shapeRect.height();
      const QPointF start(shapeRect.left() + width * 0.23, shapeRect.top() + height * 0.54);
      const QPointF mid(shapeRect.left() + width * 0.43, shapeRect.top() + height * 0.74);
      const QPointF end(shapeRect.left() + width * 0.76, shapeRect.top() + height * 0.34);
      const qreal penWidth = std::max<qreal>(2.0, std::round(std::min(width, height) * 0.12));
      painter.setPen(QPen(markColor, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawLine(start, mid);
      painter.drawLine(mid, end);
      painter.restore();
    }
  }

 private:
  enum class FillMode : std::uint8_t {
    Solid,
    Gradient,
  };

  ColorValue value_;
  QColor border_ = QColor("#f0f0f0");
  qreal borderWidth_ = 1.0;
  int radius_ = 4;
  QColor checkerLight_ = QColor("#ffffff");
  QColor checkerDark_ = QColor("#f0f0f0");
  int checkerCellSize_ = 6;
  QColor hoverOutlineColor_ = QColor(0, 0, 0, 38);
  QColor checkmarkColor_ = QColor("#ffffff");
  QColor brightCheckmarkColor_ = QColor(0, 0, 0, 115);
  int outerPadding_ = 2;
  FillMode fillMode_ = FillMode::Solid;
  QColor solidFill_ = QColor("#1677ff");
  QVector<QPair<qreal, QColor>> gradientStops_;
  bool hovered_ = false;
  bool checkedVisual_ = false;
  bool bright_ = false;
};

void clearLayoutItems(QLayout* layout) {
  if (!layout) {
    return;
  }

  while (QLayoutItem* item = layout->takeAt(0)) {
    delete item;
  }
}

void reflowPresetItems(QWidget* itemsHost, int columnCount, int spacing) {
  if (!itemsHost) {
    return;
  }

  auto* grid = qobject_cast<QGridLayout*>(itemsHost->layout());
  if (!grid) {
    return;
  }

  clearLayoutItems(grid);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(std::max(0, spacing));
  grid->setVerticalSpacing(std::max(0, spacing));

  QList<PresetColorButton*> buttons;
  for (QAbstractButton* child : itemsHost->findChildren<QAbstractButton*>(
           QString::fromLatin1(kPresetSwatchObjectName), Qt::FindDirectChildrenOnly)) {
    if (auto* button = dynamic_cast<PresetColorButton*>(child)) {
      buttons.append(button);
    }
  }
  std::sort(buttons.begin(), buttons.end(), [](PresetColorButton* lhs, PresetColorButton* rhs) {
    const int lhsOrder =
        lhs ? lhs->property("ad-color-picker-order").toInt() : std::numeric_limits<int>::max();
    const int rhsOrder =
        rhs ? rhs->property("ad-color-picker-order").toInt() : std::numeric_limits<int>::max();
    return lhsOrder < rhsOrder;
  });

  const int columns = std::max(1, columnCount);
  for (int index = 0; index < buttons.size(); ++index) {
    if (PresetColorButton* button = buttons.at(index)) {
      grid->addWidget(button, index / columns, index % columns);
    }
  }
}

class ColorPickerClearButton final : public QAbstractButton {
 public:
  explicit ColorPickerClearButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setObjectName(QStringLiteral("ad-color-picker-clear"));
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
    setAccessibleName(
        QCoreApplication::translate("adqt::widgets::AdColorPicker", "Clear color"));
    setAccessibleDescription(QCoreApplication::translate(
        "adqt::widgets::AdColorPicker", "Reset the current color selection"));
  }

  void setVisualStyle(const QColor& background, const QColor& border, const QColor& borderHover,
                      const QColor& slash, qreal borderWidth, int radius) {
    const qreal normalizedBorder = std::max<qreal>(1.0, borderWidth);
    const int normalizedRadius = std::max(0, radius);
    const bool changed = background_ != background || border_ != border ||
                         borderHover_ != borderHover || slash_ != slash ||
                         !qFuzzyCompare(borderWidth_ + 1.0, normalizedBorder + 1.0) ||
                         radius_ != normalizedRadius;
    background_ = background;
    border_ = border;
    borderHover_ = borderHover;
    slash_ = slash;
    borderWidth_ = normalizedBorder;
    radius_ = normalizedRadius;
    if (changed) {
      invalidateCache();
      update();
    }
  }

 protected:
  void enterEvent(QEnterEvent* event) override {
    hovered_ = true;
    update();
    QAbstractButton::enterEvent(event);
  }

  void leaveEvent(QEvent* event) override {
    hovered_ = false;
    update();
    QAbstractButton::leaveEvent(event);
  }

  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    const QSize logicalSize = size();
    const qreal dpr = devicePixelRatioF();
    const bool cacheStale = cachedPixmap_.isNull() || cachedLogicalSize_ != logicalSize ||
                            !qFuzzyCompare(cachedDpr_ + 1.0, dpr + 1.0) ||
                            cachedHovered_ != hovered_ || cachedEnabled_ != isEnabled();
    if (cacheStale) {
      renderCache(logicalSize, dpr);
    }

    QPainter painter(this);
    if (!cachedPixmap_.isNull()) {
      painter.drawPixmap(0, 0, cachedPixmap_);
    }
  }

 private:
  void invalidateCache() {
    cachedPixmap_ = QPixmap();
    cachedLogicalSize_ = QSize();
    cachedDpr_ = 0.0;
  }

  void renderCache(const QSize& logicalSize, qreal dpr) {
    if (logicalSize.width() <= 0 || logicalSize.height() <= 0) {
      invalidateCache();
      return;
    }

    QPixmap pixmap(logicalSize * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF fillRect(QRect(QPoint(0, 0), logicalSize));
    const qreal borderWidth = std::max<qreal>(1.0, borderWidth_);
    const qreal half = borderWidth / 2.0;
    const QRectF rawBorderRect =
        fillRect.adjusted(half + 0.5, half + 0.5, -half - 0.5, -half - 0.5);
    if (!rawBorderRect.isValid() || rawBorderRect.width() <= 0.0 || rawBorderRect.height() <= 0.0) {
      invalidateCache();
      return;
    }

    const QRectF borderRect = snapRectToDevicePixels(rawBorderRect, dpr);
    const qreal radius = std::max<qreal>(0.0, radius_);
    const QPainterPath fillPath = roundedRectPath(borderRect, radius, radius, radius, radius);

    QColor borderColor = hovered_ && isEnabled() ? borderHover_ : border_;
    QColor slashColor = slash_;
    if (!isEnabled()) {
      borderColor.setAlphaF(borderColor.alphaF() * 0.8F);
      slashColor.setAlphaF(slashColor.alphaF() * 0.45F);
    }

    painter.fillPath(fillPath, background_);
    painter.setPen(QPen(borderColor, borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(roundedRectPath(borderRect, radius, radius, radius, radius));

    painter.save();
    painter.setClipPath(fillPath);
    const qreal inset =
        std::max<qreal>(1.0, std::round(std::min(borderRect.width(), borderRect.height()) * 0.2));
    const qreal slashWidth =
        std::max<qreal>(2.0, std::round(std::min(borderRect.width(), borderRect.height()) * 0.12));
    painter.setPen(QPen(slashColor, slashWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(borderRect.right() - inset, borderRect.top() + inset),
                     QPointF(borderRect.left() + inset, borderRect.bottom() - inset));
    painter.restore();

    cachedPixmap_ = pixmap;
    cachedLogicalSize_ = logicalSize;
    cachedDpr_ = dpr;
    cachedHovered_ = hovered_;
    cachedEnabled_ = isEnabled();
  }

  QColor background_ = QColor("#ffffff");
  QColor border_ = QColor("#f0f0f0");
  QColor borderHover_ = QColor("#d9d9d9");
  QColor slash_ = QColor("#ff4d4f");
  qreal borderWidth_ = 1.0;
  int radius_ = 4;
  bool hovered_ = false;
  QPixmap cachedPixmap_;
  QSize cachedLogicalSize_;
  qreal cachedDpr_ = 0.0;
  bool cachedHovered_ = false;
  bool cachedEnabled_ = true;
};

int controlHeightForSize(AdColorPicker::Size size, const detail::ColorPickerVisualStyle& style) {
  switch (size) {
    case AdColorPicker::Size::Small:
      return style.metrics.controlHeightSM;
    case AdColorPicker::Size::Large:
      return style.metrics.controlHeightLG;
    case AdColorPicker::Size::Middle:
    default:
      return style.metrics.controlHeight;
  }
}

int swatchSizeForSize(AdColorPicker::Size size, const detail::ColorPickerVisualStyle& style) {
  switch (size) {
    case AdColorPicker::Size::Small:
      return style.metrics.swatchSizeSM;
    case AdColorPicker::Size::Large:
      return style.metrics.swatchSizeLG;
    case AdColorPicker::Size::Middle:
    default:
      return style.metrics.swatchSize;
  }
}

int triggerRadiusForSize(AdColorPicker::Size size, const detail::ColorPickerVisualStyle& style) {
  switch (size) {
    case AdColorPicker::Size::Small:
      return style.metrics.triggerRadiusSM;
    case AdColorPicker::Size::Large:
      return style.metrics.triggerRadiusLG;
    case AdColorPicker::Size::Middle:
    default:
      return style.metrics.triggerRadius;
  }
}

int swatchRadiusForSize(AdColorPicker::Size size, const detail::ColorPickerVisualStyle& style) {
  switch (size) {
    case AdColorPicker::Size::Small:
      return style.metrics.swatchRadiusSM;
    case AdColorPicker::Size::Large:
      return style.metrics.swatchRadiusLG;
    case AdColorPicker::Size::Middle:
    default:
      return style.metrics.swatchRadius;
  }
}

bool modeListContains(const QVector<AdColorPicker::Mode>& modes, AdColorPicker::Mode value) {
  return std::find(modes.cbegin(), modes.cend(), value) != modes.cend();
}

int formatSelectWidthHint(const QFont& font, int iconSize, int arrowGap) {
  const QFontMetrics metrics(font);
  int widestLabel = 0;
  for (const QString& label :
       {QStringLiteral("HEX"), QStringLiteral("RGB"), QStringLiteral("HSB")}) {
    widestLabel = std::max(widestLabel, std::max(metrics.horizontalAdvance(label),
                                                 metrics.boundingRect(label).width()));
  }

  constexpr int kWidthSafety = 2;
  return std::max(40, widestLabel + std::max(10, iconSize) + std::max(0, arrowGap) + kWidthSafety);
}

void applyModeSegmentedStyle(QWidget* modeSegmented, const detail::ColorPickerVisualStyle& style,
                             const adqt::theme::ThemeMapToken& mapToken) {
  if (!modeSegmented) {
    return;
  }

  modeSegmented->setMinimumHeight(style.metrics.inputHeight);
  modeSegmented->setMaximumHeight(style.metrics.inputHeight);

  const int segmentedTrackPadding = std::max(1, qRound(mapToken.lineWidthBold));
  const int segmentedRadius = std::max(0, qRound(mapToken.borderRadiusSM));
  const int segmentedItemRadius = std::max(0, qRound(mapToken.borderRadiusXS));
  const int segmentedPadding = std::max(4, qRound(mapToken.sizeXS - mapToken.lineWidth));
  const int segmentedBorderWidth = std::max(1, qRound(style.metrics.borderWidth));
  const int modeFontSize = std::max(10, qRound(mapToken.fontSize));

  if (auto* segmentedFrame = dynamic_cast<ColorPickerSegmentedFrame*>(modeSegmented)) {
    segmentedFrame->setVisualStyle(style.segmentedBackground, segmentedRadius);
  }

  auto* modeLayout = qobject_cast<QHBoxLayout*>(modeSegmented->layout());
  if (!modeLayout) {
    return;
  }

  modeLayout->setContentsMargins(segmentedTrackPadding, segmentedTrackPadding,
                                 segmentedTrackPadding, segmentedTrackPadding);
  modeLayout->setSpacing(0);

  const int buttonHeight =
      std::max(12, style.metrics.inputHeight - modeLayout->contentsMargins().top() -
                       modeLayout->contentsMargins().bottom());

  QFont modeFont = style.metrics.font;
  modeFont.setPixelSize(modeFontSize);

  for (QPushButton* button :
       modeSegmented->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly)) {
    if (!button) {
      continue;
    }
    button->setFont(modeFont);
    button->setMinimumHeight(buttonHeight);
    button->setMaximumHeight(buttonHeight);
    if (auto* segmentedButton = dynamic_cast<ColorPickerSegmentedButton*>(button)) {
      segmentedButton->setVisualStyle(
          style.segmentedText, style.segmentedTextChecked, style.segmentedTextDisabled,
          style.segmentedItemHoverBackground, style.segmentedItemBackground, style.panelBorder,
          segmentedBorderWidth, segmentedItemRadius, segmentedPadding);
    }
  }
}

QVector<AdColorPicker::Mode> normalizeModeOptions(const QVector<AdColorPicker::Mode>& options) {
  QVector<AdColorPicker::Mode> normalized;
  normalized.reserve(options.size());
  for (AdColorPicker::Mode value : options) {
    if (modeListContains(normalized, value)) {
      continue;
    }
    normalized.append(value);
  }
  if (normalized.isEmpty()) {
    normalized.append(AdColorPicker::Mode::Solid);
  }
  return normalized;
}

detail::ColorPickerValueModel::State createValueModelState(
    const AdColorSelection& selection, AdColorPicker::Mode mode,
    const QVector<AdColorPicker::Mode>& modeOptions, int activeStopIndex) {
  detail::ColorPickerValueModel::State state;
  state.selection = selection;
  state.mode = mode;
  state.modeOptions = modeOptions;
  state.activeStopIndex = activeStopIndex;
  return state;
}

bool presetItemsEqual(const QVector<AdColorPicker::PresetItem>& lhs,
                      const QVector<AdColorPicker::PresetItem>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (int i = 0; i < lhs.size(); ++i) {
    const AdColorPicker::PresetItem& left = lhs.at(i);
    const AdColorPicker::PresetItem& right = rhs.at(i);
    if (left.label != right.label || left.colors != right.colors ||
        left.defaultOpen != right.defaultOpen || left.key != right.key) {
      return false;
    }
  }

  return true;
}

QString sanitizeHexInput(QString text, int maxLength) {
  text = text.trimmed().toUpper();
  if (text.startsWith(QLatin1Char('#'))) {
    text.remove(0, 1);
  }

  QString filtered;
  filtered.reserve(text.size());
  for (const QChar ch : text) {
    if (ch.isDigit() || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))) {
      filtered.append(ch);
    }
  }

  if (maxLength >= 0 && filtered.size() > maxLength) {
    filtered.truncate(maxLength);
  }
  return filtered;
}

bool parseBoundedInt(const QString& text, int minValue, int maxValue, int* out) {
  if (!out) {
    return false;
  }

  bool ok = false;
  const int parsed = text.trimmed().toInt(&ok);
  if (!ok) {
    return false;
  }
  if (parsed < minValue || parsed > maxValue) {
    return false;
  }
  *out = parsed;
  return true;
}

bool parseBoundedInt(const QVariant& value, int minValue, int maxValue, int* out) {
  if (!out) {
    return false;
  }

  bool ok = false;
  const int parsed = value.toInt(&ok);
  if (!ok) {
    const QString text = value.toString();
    if (text.trimmed().isEmpty()) {
      return false;
    }
    return parseBoundedInt(text, minValue, maxValue, out);
  }
  if (parsed < minValue || parsed > maxValue) {
    return false;
  }
  *out = parsed;
  return true;
}

QString modeLabel(AdColorPicker::Mode value) {
  switch (value) {
    case AdColorPicker::Mode::Solid:
      return AdColorPicker::tr("Solid");
    case AdColorPicker::Mode::Gradient:
      return AdColorPicker::tr("Gradient");
  }
  return AdColorPicker::tr("Solid");
}

}  // namespace

struct AdColorPicker::StyleCache {
  bool visualValid = false;
  detail::ColorPickerVisualStyle visual;
  bool metricsValid = false;
  detail::ColorPickerMetrics metrics;
};

AdColorPickerState::AdColorPickerState(QObject* parent) : QObject(parent) {
  applyState(colorValue_, mode_, modeOptions_, format_, allowClear_, alphaChannelEnabled_,
             formatSelectorEnabled_, activeStopIndex_, presets_, false);
}

AdColorPickerState::~AdColorPickerState() = default;

AdColorPickerState::Mode AdColorPickerState::mode() const { return mode_; }

void AdColorPickerState::setMode(Mode value) {
  applyState(colorValue_, value, modeOptions_, format_, allowClear_, alphaChannelEnabled_,
             formatSelectorEnabled_, activeStopIndex_, presets_);
}

QVector<AdColorPickerState::Mode> AdColorPickerState::modeOptions() const { return modeOptions_; }

void AdColorPickerState::setModeOptions(const QVector<Mode>& options) {
  applyState(colorValue_, mode_, options, format_, allowClear_, alphaChannelEnabled_,
             formatSelectorEnabled_, activeStopIndex_, presets_);
}

AdColorPickerState::Format AdColorPickerState::format() const { return format_; }

void AdColorPickerState::setFormat(Format value) {
  applyState(colorValue_, mode_, modeOptions_, value, allowClear_, alphaChannelEnabled_,
             formatSelectorEnabled_, activeStopIndex_, presets_);
}

bool AdColorPickerState::allowClear() const { return allowClear_; }

void AdColorPickerState::setAllowClear(bool value) {
  applyState(colorValue_, mode_, modeOptions_, format_, value, alphaChannelEnabled_,
             formatSelectorEnabled_, activeStopIndex_, presets_);
}

bool AdColorPickerState::alphaChannelEnabled() const { return alphaChannelEnabled_; }

void AdColorPickerState::setAlphaChannelEnabled(bool value) {
  applyState(colorValue_, mode_, modeOptions_, format_, allowClear_, value, formatSelectorEnabled_,
             activeStopIndex_, presets_);
}

bool AdColorPickerState::formatSelectorEnabled() const { return formatSelectorEnabled_; }

void AdColorPickerState::setFormatSelectorEnabled(bool value) {
  applyState(colorValue_, mode_, modeOptions_, format_, allowClear_, alphaChannelEnabled_, value,
             activeStopIndex_, presets_);
}

int AdColorPickerState::activeStopIndex() const { return activeStopIndex_; }

void AdColorPickerState::setActiveStopIndex(int value) {
  applyState(colorValue_, mode_, modeOptions_, format_, allowClear_, alphaChannelEnabled_,
             formatSelectorEnabled_, value, presets_);
}

QString AdColorPickerState::cssText() const { return cachedCssText_; }

void AdColorPickerState::setCssText(const QString& value) {
  bool ok = false;
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::stateFromCssValue(value, modeOptions_, mode_, activeStopIndex_,
                                                       &ok);
  if (!ok) {
    return;
  }

  applyState(nextState.selection, nextState.mode, nextState.modeOptions, format_, allowClear_,
             alphaChannelEnabled_, formatSelectorEnabled_, nextState.activeStopIndex, presets_);
}

QString AdColorPickerState::displayText() const { return cachedDisplayText_; }

AdColorValue AdColorPickerState::value() const { return toQtColorValue(colorValue_); }

void AdColorPickerState::setValue(const AdColorValue& value) {
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::stateFromSelection(toLegacyColorValue(value), modeOptions_,
                                                        mode_, activeStopIndex_);
  applyState(nextState.selection, nextState.mode, nextState.modeOptions, format_, allowClear_,
             alphaChannelEnabled_, formatSelectorEnabled_, nextState.activeStopIndex, presets_);
}

QVector<AdColorPickerState::PresetItem> AdColorPickerState::presets() const { return presets_; }

void AdColorPickerState::setPresets(const QVector<PresetItem>& presets) {
  applyState(colorValue_, mode_, modeOptions_, format_, allowClear_, alphaChannelEnabled_,
             formatSelectorEnabled_, activeStopIndex_, presets);
}

QColor AdColorPickerState::editableColor() const {
  return detail::ColorPickerValueModel::editableColor(
      createValueModelState(colorValue_, mode_, modeOptions_, activeStopIndex_));
}

void AdColorPickerState::setEditableColor(const QColor& color) {
  const detail::ColorPickerValueModel::State currentState =
      createValueModelState(colorValue_, mode_, modeOptions_, activeStopIndex_);
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::withEditableColor(currentState, color);
  applyState(nextState.selection, nextState.mode, nextState.modeOptions, format_, allowClear_,
             alphaChannelEnabled_, formatSelectorEnabled_, nextState.activeStopIndex, presets_);
}

void AdColorPickerState::setGradientStopPositions(const QList<double>& values) {
  const detail::ColorPickerValueModel::State currentState =
      createValueModelState(colorValue_, mode_, modeOptions_, activeStopIndex_);
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::withGradientStopPositions(currentState, values);
  applyState(nextState.selection, nextState.mode, nextState.modeOptions, format_, allowClear_,
             alphaChannelEnabled_, formatSelectorEnabled_, nextState.activeStopIndex, presets_);
}

void AdColorPickerState::clearSelection() {
  const detail::ColorPickerValueModel::State currentState =
      createValueModelState(colorValue_, mode_, modeOptions_, activeStopIndex_);
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::clearedState(currentState);
  applyState(nextState.selection, nextState.mode, nextState.modeOptions, format_, allowClear_,
             alphaChannelEnabled_, formatSelectorEnabled_, nextState.activeStopIndex, presets_);
}

void AdColorPickerState::applyState(const AdColorSelection& selection, Mode mode,
                                    const QVector<Mode>& modeOptions, Format format,
                                    bool allowClear, bool alphaChannelEnabled,
                                    bool formatSelectorEnabled, int activeStopIndex,
                                    const QVector<PresetItem>& presets, bool emitClearedSignal) {
  detail::ColorPickerValueModel::State nextState =
      createValueModelState(selection, mode, modeOptions, activeStopIndex);
  nextState = detail::ColorPickerValueModel::normalizedState(nextState);

  const QString nextCss = detail::ColorPickerValueModel::cssValue(nextState);
  const QString nextFormatted = detail::ColorPickerValueModel::formattedValue(nextState, format);

  applyNormalizedState(nextState.selection, nextState.mode, nextState.modeOptions, format,
                       allowClear, alphaChannelEnabled, formatSelectorEnabled,
                       nextState.activeStopIndex, presets, nextCss, nextFormatted,
                       emitClearedSignal);
}

void AdColorPickerState::applyNormalizedState(const AdColorSelection& selection, Mode mode,
                                              const QVector<Mode>& modeOptions, Format format,
                                              bool allowClear, bool alphaChannelEnabled,
                                              bool formatSelectorEnabled, int activeStopIndex,
                                              const QVector<PresetItem>& presets,
                                              const QString& cssText, const QString& displayText,
                                              bool emitClearedSignal) {
  const QString previousCss = cachedCssText_;
  const QString previousFormatted = cachedDisplayText_;

  const bool modeChangedFlag = mode_ != mode;
  const bool modeOptionsChangedFlag = modeOptions_ != modeOptions;
  const bool formatChangedFlag = format_ != format;
  const bool allowClearChangedFlag = allowClear_ != allowClear;
  const bool alphaChangedFlag = alphaChannelEnabled_ != alphaChannelEnabled;
  const bool formatSelectorChangedFlag = formatSelectorEnabled_ != formatSelectorEnabled;
  const bool activeStopIndexChangedFlag = activeStopIndex_ != activeStopIndex;
  const bool colorValueChangedFlag = colorValue_ != selection;
  const bool presetsChangedFlag = !presetItemsEqual(presets_, presets);
  const bool cssChangedFlag = previousCss != cssText;
  const bool formattedChangedFlag = previousFormatted != displayText;
  const bool wasCleared = colorValue_.isEmpty();

  if (!modeChangedFlag && !modeOptionsChangedFlag && !formatChangedFlag && !allowClearChangedFlag &&
      !alphaChangedFlag && !formatSelectorChangedFlag && !activeStopIndexChangedFlag &&
      !colorValueChangedFlag && !presetsChangedFlag) {
    cachedCssText_ = cssText;
    cachedDisplayText_ = displayText;
    return;
  }

  mode_ = mode;
  modeOptions_ = modeOptions;
  format_ = format;
  allowClear_ = allowClear;
  alphaChannelEnabled_ = alphaChannelEnabled;
  formatSelectorEnabled_ = formatSelectorEnabled;
  activeStopIndex_ = activeStopIndex;
  colorValue_ = selection;
  presets_ = presets;
  cachedCssText_ = cssText;
  cachedDisplayText_ = displayText;

  if (modeOptionsChangedFlag) {
    emit modeOptionsChanged(modeOptions_);
  }
  if (modeChangedFlag) {
    emit modeChanged(mode_);
  }
  if (formatChangedFlag) {
    emit formatChanged(format_);
  }
  if (allowClearChangedFlag) {
    emit allowClearChanged(allowClear_);
  }
  if (alphaChangedFlag) {
    emit alphaChannelEnabledChanged(alphaChannelEnabled_);
  }
  if (formatSelectorChangedFlag) {
    emit formatSelectorEnabledChanged(formatSelectorEnabled_);
  }
  if (activeStopIndexChangedFlag) {
    emit activeStopIndexChanged(activeStopIndex_);
  }
  if (colorValueChangedFlag) {
    emit valueChanged(toQtColorValue(colorValue_));
  }
  if (presetsChangedFlag) {
    emit presetsChanged();
  }
  if (cssChangedFlag) {
    emit cssTextChanged(cssText);
  }
  if (formattedChangedFlag) {
    emit displayTextChanged(displayText);
  }
  emit stateChanged();
  if (emitClearedSignal && !wasCleared && colorValue_.isEmpty()) {
    emit cleared();
  }
}

AdColorPicker::AdColorPicker(QWidget* parent) : AdColorPicker(HostMode::WithTrigger, parent) {}

AdColorPicker::AdColorPicker(HostMode hostMode, QWidget* parent)
    : QWidget(parent), hostMode_(hostMode) {
  setAttribute(Qt::WA_Hover, true);
  setSizePolicy(hostMode_ == HostMode::PanelOnly ? QSizePolicy::Preferred : QSizePolicy::Minimum,
                hostMode_ == HostMode::PanelOnly ? QSizePolicy::Preferred : QSizePolicy::Fixed);

  qRegisterMetaType<GradientStop>("adqt::widgets::AdColorGradientStop");
  qRegisterMetaType<ColorValue>("adqt::widgets::AdColorSelection");
  qRegisterMetaType<QtColorValue>("adqt::widgets::AdColorValue");

  gradientStops_ = {
      InternalGradientStop{solidColor_, 0},
      InternalGradientStop{solidColor_, 100},
  };

  ensureRootLayout();
  if (hostMode_ == HostMode::WithTrigger) {
    ensureTriggerUi();
    ensurePopover();
    refreshTriggerDisplay();
  } else {
    ensureEditorUi();
    if (panelHost_) {
      setHostedRootWidget(panelHost_);
    }
  }
  refreshStyle();

  ownedState_ = std::make_unique<AdColorPickerState>();
  ownedState_->applyState(exportColorValue(), mode_, modeOptions_, format_, allowClear_,
                          !disabledAlpha_, !disabledFormat_, activeStopIndex_, presets_, false);
  setState(ownedState_.get());
}

AdColorPicker::~AdColorPicker() {
  // Releasing the popover can reparent a focused editor and synchronously emit
  // editingFinished. Do that while the picker's derived state is still alive;
  // QObject would otherwise destroy the popover after these members are gone.
  if (popover_) {
    delete popover_.data();
  }
  if (triggerContent_) {
    triggerContent_->removeEventFilter(this);
  }
  disconnect(triggerContentDestroyedConnection_);
  triggerContentDestroyedConnection_ = {};
  triggerContent_ = nullptr;
  disconnect(previewContentDestroyedConnection_);
  previewContentDestroyedConnection_ = {};
  previewContent_ = nullptr;
  if (state_) {
    disconnect(state_.data(), nullptr, this, nullptr);
  }
  state_ = nullptr;
  ownedState_.reset();
  stopInteractionWaveForOwner(this);
  stopInteractionFocusForOwner(this);
}

AdColorPicker::Size AdColorPicker::size() const { return size_; }

void AdColorPicker::setSize(Size value) {
  if (size_ == value) {
    return;
  }
  size_ = value;
  emit sizeChanged(size_);
  refreshStyle();
}

AdColorPicker::Mode AdColorPicker::mode() const { return mode_; }

void AdColorPicker::setMode(Mode value) {
  const ColorValue previousSelection = exportColorValue();
  const SelectionState currentState = selectionState();
  const detail::ColorPickerValueModel::State valueModelState =
      createValueModelState(currentState.selection, currentState.mode, currentState.modeOptions,
                            currentState.activeStopIndex);
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::withMode(valueModelState, value);
  const bool modeChangedByModel = mode_ != nextState.mode;
  const bool selectionChanged = previousSelection != nextState.selection;
  const bool activeChanged = activeStopIndex_ != nextState.activeStopIndex;
  if (!modeChangedByModel && !selectionChanged && !activeChanged) {
    return;
  }

  applySelectionState(createSelectionState(nextState.selection, nextState.mode,
                                           nextState.modeOptions, nextState.activeStopIndex));
  if (mode_ == Mode::Gradient && pickerPanel_) {
    ensureGradientUi();
  }
  syncStateObject();

  if (modeChangedByModel) {
    emit modeChanged(mode_);
  }
  if (selectionChanged) {
    emit valueChanged(toQtColorValue(exportColorValue()));
  }
  refreshPanelControlsFromState();
  refreshTriggerDisplay();
  emit cssTextChanged(this->cssText());
  emit displayTextChanged(this->displayText());
}

QVector<AdColorPicker::Mode> AdColorPicker::modeOptions() const { return modeOptions_; }

void AdColorPicker::setModeOptions(const QVector<Mode>& options) {
  const QVector<Mode> normalized = detail::ColorPickerValueModel::normalizeModeOptions(options);
  if (modeOptions_ == normalized) {
    return;
  }

  const ColorValue previousSelection = exportColorValue();
  const SelectionState currentState = selectionState();
  const detail::ColorPickerValueModel::State valueModelState =
      createValueModelState(currentState.selection, currentState.mode, currentState.modeOptions,
                            currentState.activeStopIndex);
  const detail::ColorPickerValueModel::State nextState =
      detail::ColorPickerValueModel::withModeOptions(valueModelState, normalized);
  const bool modeChangedByModel = mode_ != nextState.mode;
  const bool selectionChanged = previousSelection != nextState.selection;
  applySelectionState(createSelectionState(nextState.selection, nextState.mode,
                                           nextState.modeOptions, nextState.activeStopIndex));
  syncStateObject();
  emit modeOptionsChanged(modeOptions_);
  if (modeChangedByModel) {
    emit modeChanged(mode_);
  }
  if (selectionChanged) {
    emit valueChanged(toQtColorValue(exportColorValue()));
  }
  if (modeChangedByModel || selectionChanged) {
    emit cssTextChanged(this->cssText());
    emit displayTextChanged(this->displayText());
  }

  updateModeSegmentedOptions();
  refreshPanelControlsFromState();
}

AdColorPicker::Format AdColorPicker::format() const { return format_; }

void AdColorPicker::setFormat(Format value) {
  if (format_ == value) {
    return;
  }

  format_ = value;
  syncStateObject();
  emit formatChanged(format_);
  if (pickerPanel_) {
    ensureFormatInputUi(format_);
  }
  {
    // Format switching should not mutate the current color via input change handlers.
    QScopedValueRollback<bool> guard(syncingControls_, true);
    if (formatCombo_) {
      const QString currentFormat = formatName(format_);
      if (formatCombo_->currentData().toString() != currentFormat) {
        QSignalBlocker blocker(formatCombo_);
        formatCombo_->setCurrentData(currentFormat);
      }
    }
    updateFormatInputVisibility();
    updateFormatInputText();
  }
  refreshTriggerDisplay();
  if (pickerPanel_) {
    refreshStyle(false);
  }
  emit displayTextChanged(displayText());
}

bool AdColorPicker::allowClear() const { return allowClear_; }

void AdColorPicker::setAllowClear(bool value) {
  if (allowClear_ == value) {
    return;
  }
  allowClear_ = value;
  syncStateObject();
  emit allowClearChanged(allowClear_);
  refreshStyle();
}

bool AdColorPicker::triggerTextVisible() const { return triggerTextVisible_; }

void AdColorPicker::setTriggerTextVisible(bool value) {
  if (triggerTextVisible_ == value) {
    return;
  }
  triggerTextVisible_ = value;
  emit triggerTextVisibleChanged(triggerTextVisible_);
  refreshStyle();
}

bool AdColorPicker::popupVisible() const { return popover_ && popover_->isVisible(); }

void AdColorPicker::setPopupVisible(bool value) {
  if (value && !popupVisible()) {
    emit popupOpening();
  }
  if (value) {
    ensurePopover();
    ensureEditorUi();
    attachPanelHostToPopover();
  } else if (!popover_) {
    return;
  }
  if (popupVisible() == value) {
    return;
  }
  popover_->setVisible(value);
}

bool AdColorPicker::disabled() const { return !isEnabled(); }

void AdColorPicker::setDisabled(bool value) {
  if (disabled() == value) {
    return;
  }
  setEnabled(!value);
  if (popover_) {
    popover_->setEnabled(!value);
  }
  if (value && popupVisible()) {
    setPopupVisible(false);
  }
}

bool AdColorPicker::alphaChannelEnabled() const { return !disabledAlpha_; }

void AdColorPicker::setAlphaChannelEnabled(bool value) {
  const bool disabledAlpha = !value;
  if (disabledAlpha_ == disabledAlpha) {
    return;
  }
  disabledAlpha_ = disabledAlpha;
  syncStateObject();
  emit alphaChannelEnabledChanged(!disabledAlpha_);
  refreshStyle();
  refreshPanelControlsFromState();
}

bool AdColorPicker::formatSelectorEnabled() const { return !disabledFormat_; }

void AdColorPicker::setFormatSelectorEnabled(bool value) {
  const bool disabledFormat = !value;
  if (disabledFormat_ == disabledFormat) {
    return;
  }
  disabledFormat_ = disabledFormat;
  syncStateObject();
  emit formatSelectorEnabledChanged(!disabledFormat_);
  refreshStyle();
  refreshPanelControlsFromState();
}

AdColorPicker::Trigger AdColorPicker::trigger() const { return trigger_; }

void AdColorPicker::setTrigger(Trigger value) {
  if (trigger() == value) {
    return;
  }
  trigger_ = value;
  if (popover_) {
    popover_->setTriggers(toPopoverTriggers(value));
  }
  emit triggerChanged(value);
}

AdColorPicker::Placement AdColorPicker::placement() const { return placement_; }

void AdColorPicker::setPlacement(Placement value) {
  if (placement() == value) {
    return;
  }
  placement_ = value;
  if (popover_) {
    popover_->setPlacement(toPopoverPlacement(value));
  }
  emit placementChanged(value);
}

AdColorPicker::PopupLayerMode AdColorPicker::popupLayerMode() const { return popupLayerMode_; }

void AdColorPicker::setPopupLayerMode(PopupLayerMode value) {
  if (popupLayerMode_ == value) {
    return;
  }
  popupLayerMode_ = value;
  if (popover_) {
    popover_->setPopupLayerMode(value);
  }
  emit popupLayerModeChanged(popupLayerMode_);
}

QString AdColorPicker::cssText() const { return colorValueToCss(exportColorValue()); }

void AdColorPicker::setCssText(const QString& value) {
  bool ok = false;
  const ColorValue next = detail::ColorPickerValueModel::parseCssValue(value, &ok);
  if (!ok) {
    return;
  }
  importColorValue(next, false, false, true);
}

QString AdColorPicker::displayText() const {
  return detail::ColorPickerValueModel::formattedValue(exportColorValue(), format_,
                                                       activeStopIndex_);
}

AdColorValue AdColorPicker::value() const { return toQtColorValue(exportColorValue()); }

void AdColorPicker::setValue(const AdColorValue& value) {
  importColorValue(toLegacyColorValue(value), false, false, true);
}

void AdColorPicker::commitValue(const AdColorValue& value) {
  importColorValue(toLegacyColorValue(value), true, true, true);
}

QVector<AdColorPicker::PresetItem> AdColorPicker::presets() const { return presets_; }

void AdColorPicker::setPresets(const QVector<PresetItem>& presets) {
  presets_ = presets;
  syncStateObject();
  emit presetsChanged();
  rebuildPresetsPanel();
  refreshStyle();
}

AdColorPickerState* AdColorPicker::state() const { return state_; }

void AdColorPicker::setState(AdColorPickerState* state) {
  AdColorPickerState* nextStateObject = state;
  if (!nextStateObject) {
    if (!ownedState_) {
      ownedState_ = std::make_unique<AdColorPickerState>();
    }
    ownedState_->applyState(exportColorValue(), mode_, modeOptions_, format_, allowClear_,
                            !disabledAlpha_, !disabledFormat_, activeStopIndex_, presets_, false);
    nextStateObject = ownedState_.get();
  }

  if (state_ == nextStateObject) {
    return;
  }

  if (state_) {
    disconnect(state_.data(), nullptr, this, nullptr);
  }

  state_ = nextStateObject;
  if (state_) {
    connect(state_, &QObject::destroyed, this, [this, nextStateObject]() {
      if (state_ == nextStateObject || state_.isNull()) {
        state_ = nullptr;
        setState(nullptr);
      }
    });
    connect(state_, &AdColorPickerState::stateChanged, this, [this]() { applyStateObject(); });
  }

  applyStateObject();
  emit stateChanged(state_);
}

QWidget* AdColorPicker::triggerContent() const { return triggerContent_; }

void AdColorPicker::setTriggerContent(QWidget* widget) {
  ensureTriggerUi();
  if (widget == this || widget == triggerHost_ || widget == defaultTrigger_) {
    return;
  }
  if (triggerContent_ == widget) {
    return;
  }

  if (triggerContent_) {
    triggerContent_->removeEventFilter(this);
  }
  disconnect(triggerContentDestroyedConnection_);
  triggerContentDestroyedConnection_ = {};

  triggerContent_ = widget;
  if (triggerContent_) {
    triggerContent_->installEventFilter(this);
    triggerContentDestroyedConnection_ =
        connect(triggerContent_, &QObject::destroyed, this, [this]() {
          triggerContent_.clear();
          // Releasing this handle while its own signal is being dispatched
          // leaves Qt's internal connection teardown with a live receiver.
          setActiveTriggerWidget(defaultTrigger_);
          syncPopoverSourceWidget();
          emit triggerContentChanged(nullptr);
          refreshStyle();
        });
  }

  setActiveTriggerWidget(currentTriggerWidget());
  syncPopoverSourceWidget();
  emit triggerContentChanged(triggerContent_);
  refreshStyle();
}

QWidget* AdColorPicker::previewContent() const { return previewContent_; }

void AdColorPicker::setPreviewContent(QWidget* widget) {
  ensureEditorUi();
  if (widget == this || widget == sliderContainer_ || widget == sliderGroup_ ||
      widget == previewSwatch_) {
    return;
  }
  if (previewContent_ == widget) {
    return;
  }

  disconnect(previewContentDestroyedConnection_);
  previewContentDestroyedConnection_ = {};
  if (previewContent_) {
    previewContent_->hide();
    if (auto* layout = qobject_cast<QHBoxLayout*>(sliderContainer_->layout())) {
      layout->removeWidget(previewContent_);
    }
  }

  previewContent_ = widget;
  if (previewContent_) {
    previewContentDestroyedConnection_ =
        connect(previewContent_, &QObject::destroyed, this, [this]() {
          previewContent_.clear();
          previewContentDestroyedConnection_ = {};
          // QWidget removes itself from its parent layout after QObject emits
          // destroyed. Defer the replacement so the layout is no longer
          // traversing the widget being deleted.
          QTimer::singleShot(0, this, [this]() {
            if (previewContent_) {
              return;
            }
            syncPreviewContentWidget();
            emit previewContentChanged(nullptr);
            refreshStyle();
          });
        });
  }

  syncPreviewContentWidget();
  emit previewContentChanged(previewContent_);
  refreshStyle();
}

QWidget* AdColorPicker::popupContent() const { return popupContent_; }

void AdColorPicker::setPopupContent(QWidget* widget) {
  if (popupContent_ == widget) {
    return;
  }

  if (popupContent_) {
    if (auto* hostLayout =
            panelHost_ ? qobject_cast<QVBoxLayout*>(panelHost_->layout()) : nullptr) {
      hostLayout->removeWidget(popupContent_);
    }
    popupContent_->hide();
    popupContent_->setParent(this);
  }

  popupContent_ = widget;
  if (popupContent_) {
    if (panelHost_) {
      attachPopupContentToPanel();
    } else {
      popupContent_->setParent(this);
      popupContent_->hide();
    }
  }

  if (panelHost_) {
    stabilizeWidgetLayoutTree(panelHost_);
    panelHost_->adjustSize();
  }
  if (popover_) {
    popover_->refreshPopupLayout();
  }
  emit popupContentChanged(popupContent_.data());
}

AdColorPicker::PopupContentPlacement AdColorPicker::popupContentPlacement() const {
  return popupContentPlacement_;
}

void AdColorPicker::setPopupContentPlacement(PopupContentPlacement placement) {
  if (popupContentPlacement_ == placement) {
    return;
  }
  popupContentPlacement_ = placement;
  attachPopupContentToPanel();
  if (panelHost_) {
    stabilizeWidgetLayoutTree(panelHost_);
    panelHost_->adjustSize();
  }
  if (popover_) {
    popover_->refreshPopupLayout();
  }
  emit popupContentPlacementChanged(placement);
}

AdColorPicker::ShowTextFormatter AdColorPicker::showTextFormatter() const {
  return showTextFormatter_;
}

void AdColorPicker::setShowTextFormatter(ShowTextFormatter formatter) {
  showTextFormatter_ = std::move(formatter);
  emit showTextFormatterChanged();
  refreshTriggerDisplay();
}

AdColorPicker::ComponentTokens AdColorPicker::componentTokens() const { return componentTokens_; }

void AdColorPicker::setComponentTokens(const ComponentTokens& tokens) {
  componentTokens_ = tokens;
  emit componentTokensChanged();
  refreshStyle();
}

void AdColorPicker::resetComponentTokens() {
  componentTokens_ = {};
  emit componentTokensChanged();
  refreshStyle();
}

void AdColorPicker::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }

  if (event->type() == QEvent::LanguageChange) {
    refreshStyle();
  } else if (event->type() == QEvent::EnabledChange) {
    if (disabled()) {
      stopInteractionWaveForOwner(this);
      stopInteractionFocusForOwner(this);
    }
    if (popover_) {
      popover_->setEnabled(isEnabled());
    }
    emit disabledChanged(disabled());
    refreshStyle();
  } else if (event->type() == QEvent::Hide) {
    triggerHovered_ = false;
    stopInteractionFocusForOwner(this);
  } else if (event->type() == QEvent::FontChange ||
             event->type() == QEvent::ApplicationFontChange ||
             event->type() == QEvent::PaletteChange ||
             event->type() == QEvent::ApplicationPaletteChange ||
             event->type() == QEvent::StyleChange ||
             event->type() == QEvent::LayoutDirectionChange) {
    refreshStyle();
  } else if (event->type() == QEvent::Show) {
    updateTriggerFocusOverlay();
  }
}

void AdColorPicker::moveEvent(QMoveEvent* event) {
  QWidget::moveEvent(event);
  updateTriggerFocusOverlay();
}

void AdColorPicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateTriggerFocusOverlay();
}

void AdColorPicker::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (hostMode_ != HostMode::WithTrigger || pickerPanel_ || editorPrewarmScheduled_) {
    return;
  }

  editorPrewarmScheduled_ = true;
  detail::deferTimingTask(this, QStringLiteral("AdColorPicker.PrewarmPopup"), [this]() {
    editorPrewarmScheduled_ = false;
    if (!isVisible() || pickerPanel_) {
      return;
    }
    ensureEditorUi();
    if (popover_) {
      popover_->preparePopup();
      if (popover_->popupLayerMode() == PopupLayerMode::InWindow) {
        // A retained second cycle moves widget and backing-store first-use work off the open path.
        popover_->preparePopup();
      }
    }
  });
}

bool AdColorPicker::eventFilter(QObject* watched, QEvent* event) {
  const bool watchedDefaultTrigger =
      watched == triggerFrame_ || watched == triggerSwatch_ || watched == triggerTextLabel_;
  const bool watchedThemeOwner =
      watched == currentTriggerWidget() && watched != this && watched != defaultTrigger_;
  if (watchedDefaultTrigger && event) {
    const QEvent::Type type = event->type();
    if (watched == triggerFrame_) {
      if (type == QEvent::Enter || type == QEvent::HoverEnter) {
        if (!triggerHovered_) {
          triggerHovered_ = true;
          refreshTriggerInteractionStyle();
        }
      } else if (type == QEvent::Leave || type == QEvent::HoverLeave) {
        if (triggerHovered_) {
          triggerHovered_ = false;
          refreshTriggerInteractionStyle();
        }
      } else if (type == QEvent::FocusIn || type == QEvent::FocusOut) {
        updateTriggerFocusOverlay();
      } else if (type == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent && !disabled()) {
          const bool togglePopup =
              keyEvent->key() == Qt::Key_F4 ||
              (keyEvent->key() == Qt::Key_Down && keyEvent->modifiers().testFlag(Qt::AltModifier));
          const bool closePopup =
              keyEvent->key() == Qt::Key_Up && keyEvent->modifiers().testFlag(Qt::AltModifier);
          if (togglePopup) {
            setPopupVisible(!popupVisible());
            keyEvent->accept();
            return true;
          }
          if (closePopup && popupVisible()) {
            setPopupVisible(false);
            keyEvent->accept();
            return true;
          }
        }
      }
    }
  }
  if (watchedThemeOwner && event) {
    switch (event->type()) {
      case QEvent::FontChange:
      case QEvent::ApplicationFontChange:
      case QEvent::PaletteChange:
      case QEvent::ApplicationPaletteChange:
      case QEvent::StyleChange:
      case QEvent::LayoutDirectionChange:
        refreshStyle();
        break;
      case QEvent::KeyPress: {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (!keyEvent || disabled()) {
          break;
        }
        const bool togglePopup =
            keyEvent->key() == Qt::Key_F4 ||
            (keyEvent->key() == Qt::Key_Down && keyEvent->modifiers().testFlag(Qt::AltModifier));
        const bool closePopup =
            keyEvent->key() == Qt::Key_Up && keyEvent->modifiers().testFlag(Qt::AltModifier);
        if (togglePopup) {
          setPopupVisible(!popupVisible());
          keyEvent->accept();
          return true;
        }
        if (closePopup && popupVisible()) {
          setPopupVisible(false);
          keyEvent->accept();
          return true;
        }
        break;
      }
      case QEvent::FocusIn:
      case QEvent::FocusOut:
        updateTriggerFocusOverlay();
        break;
      default:
        break;
    }
  }
  return QWidget::eventFilter(watched, event);
}

const QWidget* AdColorPicker::themeLogicalOwner() const {
  if (QWidget* triggerWidget = currentTriggerWidget()) {
    return triggerWidget;
  }
  return this;
}

detail::ColorPickerVisualStyle AdColorPicker::visualStyle() const {
  if (!styleCache_) {
    styleCache_ = std::make_unique<StyleCache>();
  }
  if (styleCache_->visualValid) {
    return styleCache_->visual;
  }

  detail::ColorPickerStyleInput input;
  input.size = size_;
  input.open = popupVisible();
  input.disabled = disabled();
  input.showText = triggerTextVisible_;
  input.cleared = cleared_;
  input.baseFont = font();
  input.componentTokens = componentTokens_;

  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(this, themeLogicalOwner());
  styleCache_->visual = detail::resolveColorPickerVisualStyle(input, resolvedTheme);
  styleCache_->visualValid = true;
  return styleCache_->visual;
}

detail::ColorPickerMetrics AdColorPicker::metrics() const {
  if (!styleCache_) {
    styleCache_ = std::make_unique<StyleCache>();
  }
  if (!styleCache_->metricsValid) {
    styleCache_->metrics = visualStyle().metrics;
    styleCache_->metricsValid = true;
  }
  return styleCache_->metrics;
}

void AdColorPicker::invalidateStyleCache() const { styleCache_.reset(); }

void AdColorPicker::refreshTriggerInteractionStyle() {
  if (!triggerFrame_) {
    return;
  }

  const detail::ColorPickerVisualStyle style = visualStyle();
  QColor border = style.triggerBorder;
  if (!disabled()) {
    border = popupVisible() ? style.triggerBorderActive
                            : (triggerHovered_ ? style.triggerBorderHover : style.triggerBorder);
  }
  triggerFrame_->setProperty("ad-color-picker-border-color", border.name(QColor::HexArgb));
  if (auto* paintedTrigger = dynamic_cast<ColorPickerTriggerFrame*>(triggerFrame_.data())) {
    paintedTrigger->setVisualStyle(
        disabled() ? style.triggerBackgroundDisabled : style.triggerBackground, border,
        style.metrics.borderWidth, triggerRadiusForSize(size_, style));
  }
  updateTriggerFocusOverlay();
}

void AdColorPicker::updateTriggerFocusOverlay() {
  if (disabled() || triggerContent_ || !triggerFrame_ || !isVisible()) {
    stopInteractionFocusForOwner(this);
    return;
  }

  if (!popupVisible() && !triggerFrame_->hasFocus()) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const detail::ColorPickerVisualStyle style = visualStyle();
  if (style.triggerFocusOutline.alpha() <= 0 || style.metrics.focusOutlineWidth <= 0.0) {
    stopInteractionFocusForOwner(this);
    return;
  }

  const qreal borderWidth = std::max<qreal>(0.0, style.metrics.borderWidth);
  const qreal half = borderWidth / 2.0;
  QRectF baseRect =
      QRectF(triggerFrame_->rect()).adjusted(half + 0.5, half + 0.5, -half - 0.5, -half - 0.5);
  if (!baseRect.isValid() || baseRect.width() <= 0.0 || baseRect.height() <= 0.0) {
    baseRect = QRectF(triggerFrame_->rect());
  }

  QWidget* hostWindow = window();
  if (!hostWindow) {
    stopInteractionFocusForOwner(this);
    return;
  }
  const QPoint origin = triggerFrame_->mapTo(hostWindow, QPoint(0, 0));
  const QRectF baseRectInWindow = baseRect.translated(origin.x(), origin.y());

  const qreal radius = std::max<qreal>(0.0, triggerRadiusForSize(size_, style));
  InteractionFocusRequest request;
  request.owner = this;
  request.baseRectInWindow = baseRectInWindow;
  request.topLeft = radius;
  request.topRight = radius;
  request.bottomRight = radius;
  request.bottomLeft = radius;
  request.color = style.triggerFocusOutline;
  request.strokeWidth = std::max<qreal>(1.0, style.metrics.focusOutlineWidth);
  request.offset = std::max<qreal>(0.0, style.metrics.focusOutlineOffset);
  triggerInteractionFocus(request);
}

QString AdColorPicker::modeName(Mode value) {
  switch (value) {
    case Mode::Solid:
      return QStringLiteral("solid");
    case Mode::Gradient:
      return QStringLiteral("gradient");
  }
  return QStringLiteral("solid");
}

AdColorPicker::Mode AdColorPicker::parseModeName(const QString& value, Mode fallback) {
  const QString normalized = value.trimmed().toLower();
  if (normalized == QStringLiteral("solid") || normalized == QStringLiteral("single")) {
    return Mode::Solid;
  }
  if (normalized == QStringLiteral("gradient")) {
    return Mode::Gradient;
  }
  return fallback;
}

QString AdColorPicker::formatName(Format value) {
  switch (value) {
    case Format::Hex:
      return QStringLiteral("hex");
    case Format::Rgb:
      return QStringLiteral("rgb");
    case Format::Hsb:
      return QStringLiteral("hsb");
  }
  return QStringLiteral("hex");
}

AdColorPicker::Format AdColorPicker::parseFormatName(const QString& value, Format fallback) {
  const QString normalized = value.trimmed().toLower();
  if (normalized == QStringLiteral("hex")) {
    return Format::Hex;
  }
  if (normalized == QStringLiteral("rgb")) {
    return Format::Rgb;
  }
  if (normalized == QStringLiteral("hsb")) {
    return Format::Hsb;
  }
  return fallback;
}

AdPopover::Placement AdColorPicker::toPopoverPlacement(Placement value) {
  switch (value) {
    case Placement::Top:
      return AdPopover::Placement::Top;
    case Placement::TopLeft:
      return AdPopover::Placement::TopLeft;
    case Placement::TopRight:
      return AdPopover::Placement::TopRight;
    case Placement::Bottom:
      return AdPopover::Placement::Bottom;
    case Placement::BottomLeft:
      return AdPopover::Placement::BottomLeft;
    case Placement::BottomRight:
      return AdPopover::Placement::BottomRight;
    case Placement::Left:
      return AdPopover::Placement::Left;
    case Placement::LeftTop:
      return AdPopover::Placement::LeftTop;
    case Placement::LeftBottom:
      return AdPopover::Placement::LeftBottom;
    case Placement::Right:
      return AdPopover::Placement::Right;
    case Placement::RightTop:
      return AdPopover::Placement::RightTop;
    case Placement::RightBottom:
      return AdPopover::Placement::RightBottom;
  }
  return AdPopover::Placement::BottomLeft;
}

AdColorPicker::Placement AdColorPicker::fromPopoverPlacement(AdPopover::Placement value) {
  switch (value) {
    case AdPopover::Placement::Top:
      return Placement::Top;
    case AdPopover::Placement::TopLeft:
      return Placement::TopLeft;
    case AdPopover::Placement::TopRight:
      return Placement::TopRight;
    case AdPopover::Placement::Bottom:
      return Placement::Bottom;
    case AdPopover::Placement::BottomLeft:
      return Placement::BottomLeft;
    case AdPopover::Placement::BottomRight:
      return Placement::BottomRight;
    case AdPopover::Placement::Left:
      return Placement::Left;
    case AdPopover::Placement::LeftTop:
      return Placement::LeftTop;
    case AdPopover::Placement::LeftBottom:
      return Placement::LeftBottom;
    case AdPopover::Placement::Right:
      return Placement::Right;
    case AdPopover::Placement::RightTop:
      return Placement::RightTop;
    case AdPopover::Placement::RightBottom:
      return Placement::RightBottom;
  }
  return Placement::BottomLeft;
}

AdPopover::Triggers AdColorPicker::toPopoverTriggers(Trigger value) {
  switch (value) {
    case Trigger::Hover:
      return AdPopover::Trigger::Hover;
    case Trigger::Click:
    default:
      return AdPopover::Trigger::Click;
  }
}

AdColorPicker::Trigger AdColorPicker::fromPopoverTriggers(AdPopover::Triggers value,
                                                          Trigger fallback) {
  if (value.testFlag(AdPopover::Trigger::Hover)) {
    return Trigger::Hover;
  }
  if (value.testFlag(AdPopover::Trigger::Click)) {
    return Trigger::Click;
  }
  return fallback;
}

void AdColorPicker::ensureRootLayout() {
  if (rootLayout_) {
    return;
  }

  auto* root = new QHBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  rootLayout_ = root;
}

void AdColorPicker::ensureTriggerHost() {
  if (triggerHost_) {
    return;
  }

  ensureRootLayout();

  triggerHost_ = new QWidget(this);
  triggerHost_->setObjectName(QStringLiteral("ad-color-picker-trigger-host"));
  triggerHost_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

  auto* layout = new QHBoxLayout(triggerHost_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  triggerHostLayout_ = layout;

  setHostedRootWidget(triggerHost_);
}

void AdColorPicker::ensureTriggerUi() {
  if (triggerFrame_) {
    return;
  }

  ensureTriggerHost();

  triggerFrame_ = new ColorPickerTriggerFrame(triggerHost_);
  triggerFrame_->setObjectName(QStringLiteral("ad-color-picker-trigger-frame"));
  triggerFrame_->setAttribute(Qt::WA_Hover, true);
  triggerFrame_->setMouseTracking(true);
  triggerFrame_->setCursor(Qt::PointingHandCursor);
  // Match Ant Design trigger behavior: intrinsic-width inline trigger that does not
  // stretch with parent layout width.
  triggerFrame_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  triggerFrame_->installEventFilter(this);
  auto* triggerLayout = new QHBoxLayout(triggerFrame_);
  triggerLayout->setContentsMargins(8, 4, 8, 4);
  triggerLayout->setSpacing(8);
  triggerLayout->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  triggerSwatch_ = new ColorPickerSwatch(triggerFrame_);
  triggerSwatch_->setObjectName(QStringLiteral("ad-color-picker-trigger-swatch"));
  triggerSwatch_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  triggerSwatch_->installEventFilter(this);

  triggerTextLabel_ = new QLabel(triggerFrame_);
  triggerTextLabel_->setObjectName(QStringLiteral("ad-color-picker-trigger-text"));
  triggerTextLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  triggerTextLabel_->setWordWrap(false);
  triggerTextLabel_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
  triggerTextLabel_->installEventFilter(this);

  triggerLayout->addWidget(triggerSwatch_);
  triggerLayout->addWidget(triggerTextLabel_);
  triggerLayout->setAlignment(triggerSwatch_, Qt::AlignVCenter);
  triggerLayout->setAlignment(triggerTextLabel_, Qt::AlignVCenter);

  defaultTrigger_ = triggerFrame_;

  setActiveTriggerWidget(defaultTrigger_);
}

void AdColorPicker::ensurePopover() {
  if (popover_) {
    return;
  }

  ensureTriggerUi();

  popover_ = new AdPopover(this);
  popover_->setPlacement(toPopoverPlacement(placement_));
  popover_->setTriggers(toPopoverTriggers(trigger_));
  popover_->setPopupLayerMode(popupLayerMode_);
  popover_->setVisibilityPolicy(AdPopover::VisibilityPolicy::Manual);
  popover_->setArrowVisible(true);
  popover_->setEnabled(!disabled());

  syncPopoverSourceWidget();
  if (panelHost_) {
    attachPanelHostToPopover();
  } else {
    if (!popoverContentStub_) {
      popoverContentStub_ = new QWidget(this);
      popoverContentStub_->setObjectName(QStringLiteral("ad-color-picker-popover-content-stub"));
      popoverContentStub_->setFixedSize(1, 1);
    }
    popover_->setContentWidget(popoverContentStub_);
  }

  connect(popover_, &AdPopover::visibilityRequested, this, [this](bool openValue) {
    if (openValue && popover_ && !popover_->isVisible()) {
      emit popupOpening();
    }
    if (openValue) {
      if (!pickerPanel_) {
        ensureEditorUi();
      }
      if (panelHost_) {
        attachPanelHostToPopover();
      }
    }
    if (!popover_ || popover_->isVisible() == openValue) {
      return;
    }
    popover_->setVisible(openValue);
  });

  connect(popover_, &AdPopover::visibleChanged, this, [this](bool openValue) {
    if (!openValue) {
      resumeTriggerUpdatesAfterInteraction();
      if (pendingEditingFinished_) {
        pendingEditingFinished_ = false;
        emit editingFinished(toQtColorValue(pendingFinishedValue_));
      }
    }
    emit popupVisibleChanged(openValue);
    refreshTriggerInteractionStyle();
  });
}

QWidget* AdColorPicker::currentTriggerWidget() const {
  return triggerContent_ ? triggerContent_.data() : defaultTrigger_.data();
}

void AdColorPicker::setActiveTriggerWidget(QWidget* widget) {
  ensureTriggerHost();
  if (!triggerHostLayout_ || activeTriggerWidget_ == widget) {
    return;
  }

  if (activeTriggerWidget_) {
    triggerHostLayout_->removeWidget(activeTriggerWidget_);
    activeTriggerWidget_->hide();
  }

  activeTriggerWidget_ = widget;
  if (activeTriggerWidget_) {
    if (activeTriggerWidget_->parentWidget() != triggerHost_) {
      activeTriggerWidget_->setParent(triggerHost_);
    }
    triggerHostLayout_->addWidget(activeTriggerWidget_);
    activeTriggerWidget_->show();
  }

  triggerHostLayout_->invalidate();
  triggerHostLayout_->activate();
  triggerHost_->updateGeometry();
  updateGeometry();
}

void AdColorPicker::syncPopoverSourceWidget() {
  if (popover_) {
    popover_->setSourceWidget(currentTriggerWidget());
  }
}

void AdColorPicker::setHostedRootWidget(QWidget* widget) {
  ensureRootLayout();
  if (!rootLayout_ || hostedRootWidget_ == widget) {
    return;
  }

  while (rootLayout_->count() > 0) {
    QLayoutItem* item = rootLayout_->takeAt(0);
    delete item;
  }

  hostedRootWidget_ = widget;
  if (!widget) {
    return;
  }

  if (widget->parentWidget() != this) {
    widget->setParent(this);
  }
  rootLayout_->addWidget(widget);
  widget->show();
}

void AdColorPicker::attachPopupContentToPanel() {
  if (!popupContent_ || !panelHost_) {
    return;
  }

  auto* hostLayout = qobject_cast<QVBoxLayout*>(panelHost_->layout());
  if (!hostLayout) {
    return;
  }

  if (popupContent_->parentWidget() != panelHost_) {
    popupContent_->setParent(panelHost_);
  }
  const int currentIndex = hostLayout->indexOf(popupContent_);
  const bool alreadyPlaced = popupContentPlacement_ == PopupContentPlacement::Top
                                 ? currentIndex == 0
                                 : currentIndex == hostLayout->count() - 1;
  if (!alreadyPlaced) {
    if (currentIndex >= 0) {
      hostLayout->removeWidget(popupContent_);
    }
    if (popupContentPlacement_ == PopupContentPlacement::Bottom) {
      hostLayout->addWidget(popupContent_);
    } else {
      hostLayout->insertWidget(0, popupContent_);
    }
  }
  popupContent_->show();
}

QWidget* AdColorPicker::detachPanelHost(QWidget* newParent) {
  ensureEditorUi();
  if (!panelHost_) {
    return nullptr;
  }
  if (popover_ && popover_->contentWidget() == panelHost_) {
    QWidget* detached = popover_->takeContentWidget();
    Q_UNUSED(detached)
  }
  if (newParent && panelHost_->parentWidget() != newParent) {
    panelHost_->setParent(newParent);
  }
  return panelHost_;
}

void AdColorPicker::attachPanelHostToPopover() {
  if (!popover_ || !panelHost_) {
    return;
  }
  QPointer<QWidget> staleStub = popoverContentStub_;
  if (popover_->contentWidget() != panelHost_) {
    popover_->setContentWidget(panelHost_);
  }
  if (staleStub) {
    staleStub->deleteLater();
    if (popoverContentStub_ == staleStub) {
      popoverContentStub_.clear();
    }
  }
}

void AdColorPicker::ensureOperationUi() {
  if (!pickerPanel_) {
    return;
  }

  auto* pickerLayout = qobject_cast<QVBoxLayout*>(pickerPanel_->layout());
  if (!pickerLayout) {
    return;
  }

  if (!operationRow_) {
    operationRow_ = new QWidget(pickerPanel_);
    operationRow_->setObjectName(QStringLiteral("ad-color-picker-operation-row"));
    operationRow_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* operationRow = new QHBoxLayout(operationRow_);
    operationRow->setContentsMargins(0, 0, 0, 0);
    operationRow->setSpacing(8);
    operationRow->addStretch(1);

    int insertIndex = -1;
    if (gradientSection_) {
      insertIndex = pickerLayout->indexOf(gradientSection_);
    }
    if (insertIndex < 0 && saturationPanel_) {
      insertIndex = pickerLayout->indexOf(saturationPanel_);
    }
    if (insertIndex < 0) {
      insertIndex = 0;
    }
    pickerLayout->insertWidget(insertIndex, operationRow_);
  }

  if (!operationGap_) {
    operationGap_ = new QWidget(pickerPanel_);
    operationGap_->setObjectName(QStringLiteral("ad-color-picker-operation-gap"));
    operationGap_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    operationGap_->setFixedHeight(8);
    const int rowIndex = pickerLayout->indexOf(operationRow_);
    pickerLayout->insertWidget(rowIndex >= 0 ? rowIndex + 1 : 0, operationGap_);
  }

  auto* operationLayout = qobject_cast<QHBoxLayout*>(operationRow_->layout());
  if (!operationLayout) {
    return;
  }

  const bool showModeSwitch = normalizeModeOptions(modeOptions_).size() > 1;
  if (showModeSwitch && !modeSegmented_) {
    modeSegmented_ = new ColorPickerSegmentedFrame(operationRow_);
    modeSegmented_->setObjectName(QStringLiteral("ad-color-picker-mode-segmented"));
    modeSegmented_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* modeLayout = new QHBoxLayout(modeSegmented_);
    modeLayout->setContentsMargins(2, 2, 2, 2);
    modeLayout->setSpacing(2);
    modeButtonGroup_ = new QButtonGroup(modeSegmented_);
    modeButtonGroup_->setExclusive(true);
    operationLayout->insertWidget(0, modeSegmented_, 0);

    connect(modeButtonGroup_, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked), this,
            [this](QAbstractButton* button) {
              if (syncingControls_ || !button) {
                return;
              }
              const QString value = button->property("ad-color-picker-mode-value").toString();
              if (value.isEmpty()) {
                return;
              }
              setMode(parseModeName(value, mode_));
            });
  }
}

void AdColorPicker::ensureClearButtonUi() {
  ensureOperationUi();
  if (!operationRow_ || clearButton_) {
    return;
  }

  auto* operationLayout = qobject_cast<QHBoxLayout*>(operationRow_->layout());
  if (!operationLayout) {
    return;
  }

  clearButton_ = new ColorPickerClearButton(operationRow_);
  QSizePolicy clearPolicy = clearButton_->sizePolicy();
  clearPolicy.setRetainSizeWhenHidden(true);
  clearButton_->setSizePolicy(clearPolicy);
  operationLayout->addWidget(clearButton_, 0, Qt::AlignRight | Qt::AlignVCenter);

  connect(clearButton_, &QAbstractButton::clicked, this, [this]() {
    if (cleared_) {
      return;
    }
    auto state = detail::ColorPickerValueModel::State{exportColorValue(), mode_, modeOptions_,
                                                      activeStopIndex_};
    importColorValue(detail::ColorPickerValueModel::clearedState(state).selection, true, true,
                     true);
  });
}

void AdColorPicker::ensurePresetsUi() {
  if (!panelHost_ || presetsPanel_) {
    return;
  }

  presetsPanel_ = new ColorPickerPresetsPanel(panelHost_);
  presetsPanel_->setObjectName(QStringLiteral("ad-color-picker-presets-panel"));
  presetsLayout_ = new QVBoxLayout(presetsPanel_);
  presetsLayout_->setContentsMargins(0, 0, 0, 0);
  presetsLayout_->setSpacing(8);

  if (auto* hostLayout = qobject_cast<QVBoxLayout*>(panelHost_->layout())) {
    hostLayout->addWidget(presetsPanel_);
  }
}

void AdColorPicker::ensureFormatInputUi(Format format) {
  if (!formatInputHost_) {
    return;
  }

  switch (format) {
    case Format::Hex:
      return;
    case Format::Rgb:
      ensureRgbInputUi();
      return;
    case Format::Hsb:
      ensureHsbInputUi();
      return;
  }
}

void AdColorPicker::ensureRgbInputUi() {
  if (!formatInputHost_ || rgbInputHost_) {
    return;
  }

  if (!formatInputStack_) {
    return;
  }

  rgbInputHost_ = new QWidget(formatInputHost_);
  rgbInputHost_->setObjectName(QStringLiteral("ad-color-picker-rgb-input-host"));
  rgbInputHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  rgbInputHost_->setMinimumWidth(0);
  auto* rgbRow = new QHBoxLayout(rgbInputHost_);
  rgbRow->setContentsMargins(0, 0, 0, 0);
  rgbRow->setSpacing(4);

  rgbInputR_ = createColorPickerStepperInput(rgbInputHost_,
                                             QStringLiteral("ad-color-picker-rgb-r-input"), 0, 255);
  rgbInputG_ = createColorPickerStepperInput(rgbInputHost_,
                                             QStringLiteral("ad-color-picker-rgb-g-input"), 0, 255);
  rgbInputB_ = createColorPickerStepperInput(rgbInputHost_,
                                             QStringLiteral("ad-color-picker-rgb-b-input"), 0, 255);
  rgbInputR_->setTextAlignment(Qt::AlignCenter);
  rgbInputG_->setTextAlignment(Qt::AlignCenter);
  rgbInputB_->setTextAlignment(Qt::AlignCenter);
  rgbRow->addWidget(rgbInputR_, 1);
  rgbRow->addWidget(rgbInputG_, 1);
  rgbRow->addWidget(rgbInputB_, 1);
  formatInputStack_->addWidget(rgbInputHost_);

  bindColorPickerStepperInput(
      this, rgbInputR_, [this]() { previewFormatInputs(); }, [this]() { commitFormatInputs(); });
  bindColorPickerStepperInput(
      this, rgbInputG_, [this]() { previewFormatInputs(); }, [this]() { commitFormatInputs(); });
  bindColorPickerStepperInput(
      this, rgbInputB_, [this]() { previewFormatInputs(); }, [this]() { commitFormatInputs(); });
}

void AdColorPicker::ensureHsbInputUi() {
  if (!formatInputHost_ || hsbInputHost_) {
    return;
  }

  if (!formatInputStack_) {
    return;
  }

  hsbInputHost_ = new QWidget(formatInputHost_);
  hsbInputHost_->setObjectName(QStringLiteral("ad-color-picker-hsb-input-host"));
  hsbInputHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  hsbInputHost_->setMinimumWidth(0);
  auto* hsbRow = new QHBoxLayout(hsbInputHost_);
  hsbRow->setContentsMargins(0, 0, 0, 0);
  hsbRow->setSpacing(4);

  hsbInputH_ = createColorPickerStepperInput(hsbInputHost_,
                                             QStringLiteral("ad-color-picker-hsb-h-input"), 0, 360);
  hsbInputS_ =
      createColorPickerStepperInput(hsbInputHost_, QStringLiteral("ad-color-picker-hsb-s-input"), 0,
                                    100, 1, 0, QStringLiteral("%"));
  hsbInputB_ =
      createColorPickerStepperInput(hsbInputHost_, QStringLiteral("ad-color-picker-hsb-b-input"), 0,
                                    100, 1, 0, QStringLiteral("%"));
  hsbInputH_->setTextAlignment(Qt::AlignCenter);
  hsbInputS_->setTextAlignment(Qt::AlignCenter);
  hsbInputB_->setTextAlignment(Qt::AlignCenter);
  hsbRow->addWidget(hsbInputH_, 1);
  hsbRow->addWidget(hsbInputS_, 1);
  hsbRow->addWidget(hsbInputB_, 1);
  formatInputStack_->addWidget(hsbInputHost_);

  bindColorPickerStepperInput(
      this, hsbInputH_, [this]() { previewFormatInputs(); }, [this]() { commitFormatInputs(); });
  bindColorPickerStepperInput(
      this, hsbInputS_, [this]() { previewFormatInputs(); }, [this]() { commitFormatInputs(); });
  bindColorPickerStepperInput(
      this, hsbInputB_, [this]() { previewFormatInputs(); }, [this]() { commitFormatInputs(); });
}

void AdColorPicker::ensureGradientUi() {
  if (!pickerPanel_ || gradientSection_) {
    return;
  }

  auto* pickerLayout = qobject_cast<QVBoxLayout*>(pickerPanel_->layout());
  if (!pickerLayout) {
    return;
  }

  const detail::ColorPickerMetrics currentMetrics = metrics();

  gradientSection_ = new QWidget(pickerPanel_);
  gradientSection_->setObjectName(QStringLiteral("ad-color-picker-gradient-section"));
  auto* gradientRow = new QHBoxLayout(gradientSection_);
  gradientRow->setContentsMargins(0, 0, 0, 0);
  gradientRow->setSpacing(0);

  gradientSlider_ = new AdMultiSlider(gradientSection_);
  gradientSlider_->setMinimum(0);
  gradientSlider_->setMaximum(100);
  gradientSlider_->setSingleStep(1);
  gradientSlider_->setSelectionHighlightVisible(false);
  gradientSlider_->setTooltipEnabled(false);
  gradientSlider_->setHandleEditingEnabled(true);
  gradientSlider_->setMinimumHandleCount(2);
  gradientSlider_->setHandleValues({0.0, 100.0});
  AdSliderComponentTokens gradientTokens;
  gradientTokens.controlSize = currentMetrics.sliderControlSize;
  gradientTokens.railSize = currentMetrics.sliderHeight;
  gradientTokens.handleSize = currentMetrics.gradientHandleSize;
  gradientTokens.handleSizeHover = currentMetrics.gradientHandleSizeHover;
  gradientTokens.handleLineWidth = currentMetrics.sliderHandleLineWidth;
  gradientTokens.handleLineWidthHover = currentMetrics.sliderHandleLineWidthHover;
  gradientTokens.marginMain = currentMetrics.sliderMarginMain;
  gradientTokens.marginCross = currentMetrics.sliderMarginCross;
  gradientTokens.focusOutlineSize = 0;
  gradientTokens.markGap = 0;
  gradientSlider_->setComponentTokens(gradientTokens);
  gradientSlider_->setMinimumHeight(currentMetrics.sliderVisualHeight);
  gradientSlider_->setMaximumHeight(currentMetrics.sliderVisualHeight);
  gradientSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  gradientRow->addWidget(gradientSlider_);

  int insertIndex =
      saturationPanel_ ? pickerLayout->indexOf(saturationPanel_) : pickerLayout->count();
  if (insertIndex < 0) {
    insertIndex = 0;
  }
  pickerLayout->insertWidget(insertIndex, gradientSection_);

  gradientGap_ = new QWidget(pickerPanel_);
  gradientGap_->setObjectName(QStringLiteral("ad-color-picker-gradient-gap"));
  gradientGap_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  gradientGap_->setFixedHeight(8);
  pickerLayout->insertWidget(insertIndex + 1, gradientGap_);

  connect(gradientSlider_, &AdMultiSlider::activeHandleIndexChanged, this, [this](int index) {
    if (syncingControls_ || mode_ != Mode::Gradient) {
      return;
    }
    const QVector<InternalGradientStop> normalized = normalizeGradientStops(gradientStops_);
    if (normalized.isEmpty()) {
      return;
    }
    const int nextIndex = std::clamp(index, 0, static_cast<int>(normalized.size()) - 1);
    if (activeStopIndex_ == nextIndex) {
      return;
    }
    activeStopIndex_ = nextIndex;
    solidColor_ = normalized.at(activeStopIndex_).color;
    refreshPanelControlsFromState(true);
    refreshTriggerDisplay();
    syncStateObject();
  });
  connect(gradientSlider_, &AdMultiSlider::handleValuesChanged, this,
          [this](const QList<double>& values) { setGradientStopsFromSlider(values, false); });
  connect(gradientSlider_, &AdMultiSlider::editingFinished, this,
          [this]() { setGradientStopsFromSlider(gradientSlider_->handleValues(), true); });
}

void AdColorPicker::commitFormatInputs() {
  if (syncingControls_) {
    return;
  }

  if (format_ == Format::Hex) {
    if (!hexInput_) {
      return;
    }
    QString visibleHex = sanitizeHexInput(hexInput_->text(), 8);
    if (visibleHex != hexInput_->text()) {
      hexInput_->setText(visibleHex);
    }
    if (visibleHex.size() != 6 && visibleHex.size() != 8) {
      refreshPanelControlsFromState();
      return;
    }
    QColor parsed;
    if (!parseCssHexColor(QStringLiteral("#%1").arg(visibleHex), &parsed) || !parsed.isValid()) {
      refreshPanelControlsFromState();
      return;
    }
    setCurrentEditableColor(parsed, true, true, true);
    return;
  }

  QColor current = currentEditableColor().toHsv();
  if (format_ == Format::Rgb) {
    if (!rgbInputR_ || !rgbInputG_ || !rgbInputB_) {
      return;
    }

    int r = 0;
    int g = 0;
    int b = 0;
    if (!parseBoundedInt(rgbInputR_->value(), 0, 255, &r) ||
        !parseBoundedInt(rgbInputG_->value(), 0, 255, &g) ||
        !parseBoundedInt(rgbInputB_->value(), 0, 255, &b)) {
      refreshPanelControlsFromState();
      return;
    }

    setCurrentEditableColor(QColor(r, g, b, current.alpha()), true, true, true);
    return;
  }

  if (format_ != Format::Hsb || !hsbInputH_ || !hsbInputS_ || !hsbInputB_) {
    return;
  }

  int h = 0;
  int s = 0;
  int v = 0;
  if (!parseBoundedInt(hsbInputH_->value(), 0, 360, &h) ||
      !parseBoundedInt(hsbInputS_->value(), 0, 100, &s) ||
      !parseBoundedInt(hsbInputB_->value(), 0, 100, &v)) {
    refreshPanelControlsFromState();
    return;
  }

  const int normalizedHue = (h == 360) ? 0 : h;
  const int sat = std::clamp(qRound(static_cast<double>(s) * 255.0 / 100.0), 0, 255);
  const int bri = std::clamp(qRound(static_cast<double>(v) * 255.0 / 100.0), 0, 255);
  setCurrentEditableColor(QColor::fromHsv(normalizedHue, sat, bri, current.alpha()), true, true,
                          true);
}

void AdColorPicker::previewFormatInputs() {
  if (syncingControls_) {
    return;
  }

  QScopedValueRollback<LivePanelSyncSource> sourceGuard(livePanelSyncSource_,
                                                        LivePanelSyncSource::FormatInputs);

  if (format_ == Format::Hex) {
    if (!hexInput_) {
      return;
    }
    const QString visibleHex = sanitizeHexInput(hexInput_->text(), 8);
    if (visibleHex.size() != 6 && visibleHex.size() != 8) {
      return;
    }
    QColor parsed;
    if (!parseCssHexColor(QStringLiteral("#%1").arg(visibleHex), &parsed) || !parsed.isValid()) {
      return;
    }
    setCurrentEditableColor(parsed, true, false, true);
    return;
  }

  QColor current = currentEditableColor().toHsv();
  if (format_ == Format::Rgb) {
    if (!rgbInputR_ || !rgbInputG_ || !rgbInputB_) {
      return;
    }

    int r = 0;
    int g = 0;
    int b = 0;
    if (!parseBoundedInt(rgbInputR_->value(), 0, 255, &r) ||
        !parseBoundedInt(rgbInputG_->value(), 0, 255, &g) ||
        !parseBoundedInt(rgbInputB_->value(), 0, 255, &b)) {
      return;
    }

    setCurrentEditableColor(QColor(r, g, b, current.alpha()), true, false, true);
    return;
  }

  if (format_ != Format::Hsb || !hsbInputH_ || !hsbInputS_ || !hsbInputB_) {
    return;
  }

  int h = 0;
  int s = 0;
  int v = 0;
  if (!parseBoundedInt(hsbInputH_->value(), 0, 360, &h) ||
      !parseBoundedInt(hsbInputS_->value(), 0, 100, &s) ||
      !parseBoundedInt(hsbInputB_->value(), 0, 100, &v)) {
    return;
  }

  const int normalizedHue = (h == 360) ? 0 : h;
  const int sat = std::clamp(qRound(static_cast<double>(s) * 255.0 / 100.0), 0, 255);
  const int bri = std::clamp(qRound(static_cast<double>(v) * 255.0 / 100.0), 0, 255);
  setCurrentEditableColor(QColor::fromHsv(normalizedHue, sat, bri, current.alpha()), true, false,
                          true);
}

void AdColorPicker::previewAlphaInput() {
  if (syncingControls_ || !alphaInput_) {
    return;
  }

  QScopedValueRollback<LivePanelSyncSource> sourceGuard(livePanelSyncSource_,
                                                        LivePanelSyncSource::AlphaInput);

  int percent = 0;
  if (!parseBoundedInt(alphaInput_->value(), 0, 100, &percent)) {
    return;
  }

  QColor current = currentEditableColor().toHsv();
  int hue = current.hue();
  if (hue < 0) {
    hue = 0;
  }
  const int alpha = std::clamp(qRound(percent * 255.0 / 100.0), 0, 255);
  setCurrentEditableColor(QColor::fromHsv(hue, current.saturation(), current.value(), alpha), true,
                          false, true);
}

void AdColorPicker::ensureEditorUi() {
  if (pickerPanel_) {
    if (hostMode_ == HostMode::PanelOnly && panelHost_) {
      setHostedRootWidget(panelHost_);
    } else {
      attachPanelHostToPopover();
    }
    return;
  }

  const detail::ColorPickerMetrics currentMetrics = metrics();

  if (!panelHost_) {
    panelHost_ = new QWidget(this);
    panelHost_->setObjectName(QStringLiteral("ad-color-picker-panel-host"));
    panelHost_->setAttribute(Qt::WA_StyledBackground, true);
    panelHost_->setAutoFillBackground(false);
    panelHost_->setAttribute(Qt::WA_TranslucentBackground, true);
    panelHost_->setAttribute(Qt::WA_NoSystemBackground, true);
    auto* hostLayout = new QVBoxLayout(panelHost_);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(8);
  }

  pickerPanel_ = new QWidget(panelHost_);
  pickerPanel_->setObjectName(QStringLiteral("ad-color-picker-picker-panel"));
  pickerPanel_->setAttribute(Qt::WA_StyledBackground, true);

  auto* pickerLayout = new QVBoxLayout(pickerPanel_);
  pickerLayout->setContentsMargins(0, 0, 0, 0);
  pickerLayout->setSpacing(0);

  if (normalizeModeOptions(modeOptions_).size() > 1) {
    ensureOperationUi();
  }

  saturationPanel_ = new ColorSaturationPanel(pickerPanel_);
  saturationPanel_->setObjectName(QStringLiteral("ad-color-picker-saturation-panel"));
  saturationPanel_->setMinimumHeight(currentMetrics.saturationPanelHeight);
  saturationPanel_->setMaximumHeight(currentMetrics.saturationPanelHeight);
  pickerLayout->addWidget(saturationPanel_);
  if (mode_ == Mode::Gradient) {
    ensureGradientUi();
  }

  saturationGap_ = new QWidget(pickerPanel_);
  saturationGap_->setObjectName(QStringLiteral("ad-color-picker-saturation-gap"));
  saturationGap_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  saturationGap_->setFixedHeight(
      sliderSectionGapFromMetrics(currentMetrics.marginSM, currentMetrics.sliderMarginCross));
  pickerLayout->addWidget(saturationGap_);

  sliderContainer_ = new QWidget(pickerPanel_);
  sliderContainer_->setObjectName(QStringLiteral("ad-color-picker-slider-container"));
  auto* sliderContainerLayout = new QHBoxLayout(sliderContainer_);
  sliderContainerLayout->setContentsMargins(0, 0, 0, 0);
  sliderContainerLayout->setSpacing(currentMetrics.marginSM);

  sliderGroup_ = new QWidget(sliderContainer_);
  auto* sliderGroupLayout = new QVBoxLayout(sliderGroup_);
  sliderGroupLayout->setContentsMargins(0, 0, 0, 0);
  sliderGroupLayout->setSpacing(
      sliderGroupGapFromMetrics(currentMetrics.marginSM, currentMetrics.sliderMarginCross));

  hueSlider_ = new AdSlider(sliderGroup_);
  hueSlider_->setMinimum(0);
  hueSlider_->setMaximum(359);
  hueSlider_->setSingleStep(1);
  hueSlider_->setSelectionHighlightVisible(false);
  hueSlider_->setTooltipEnabled(false);
  AdSliderComponentTokens hueTokens;
  hueTokens.controlSize = currentMetrics.sliderControlSize;
  hueTokens.railSize = currentMetrics.sliderHeight;
  hueTokens.handleSize = currentMetrics.sliderHandleSize;
  hueTokens.handleSizeHover = currentMetrics.sliderHandleSizeHover;
  hueTokens.handleLineWidth = currentMetrics.sliderHandleLineWidth;
  hueTokens.handleLineWidthHover = currentMetrics.sliderHandleLineWidthHover;
  hueTokens.marginMain = currentMetrics.sliderMarginMain;
  hueTokens.marginCross = currentMetrics.sliderMarginCross;
  hueTokens.focusOutlineSize = 0;
  hueTokens.markGap = 0;
  hueSlider_->setComponentTokens(hueTokens);
  hueSlider_->setMinimumHeight(currentMetrics.sliderVisualHeight);
  hueSlider_->setMaximumHeight(currentMetrics.sliderVisualHeight);
  hueSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  sliderGroupLayout->addWidget(hueSlider_);

  alphaSection_ = new QWidget(sliderGroup_);
  alphaSection_->setObjectName(QStringLiteral("ad-color-picker-alpha-section"));
  auto* alphaRow = new QHBoxLayout(alphaSection_);
  alphaRow->setContentsMargins(0, 0, 0, 0);
  alphaRow->setSpacing(0);
  alphaSlider_ = new AdSlider(alphaSection_);
  alphaSlider_->setMinimum(0);
  alphaSlider_->setMaximum(100);
  alphaSlider_->setSingleStep(1);
  alphaSlider_->setSelectionHighlightVisible(false);
  alphaSlider_->setTooltipEnabled(false);
  AdSliderComponentTokens alphaTokens;
  alphaTokens.controlSize = currentMetrics.sliderControlSize;
  alphaTokens.railSize = currentMetrics.sliderHeight;
  alphaTokens.handleSize = currentMetrics.sliderHandleSize;
  alphaTokens.handleSizeHover = currentMetrics.sliderHandleSizeHover;
  alphaTokens.handleLineWidth = currentMetrics.sliderHandleLineWidth;
  alphaTokens.handleLineWidthHover = currentMetrics.sliderHandleLineWidthHover;
  alphaTokens.marginMain = currentMetrics.sliderMarginMain;
  alphaTokens.marginCross = currentMetrics.sliderMarginCross;
  alphaTokens.focusOutlineSize = 0;
  alphaTokens.markGap = 0;
  alphaSlider_->setComponentTokens(alphaTokens);
  alphaSlider_->setMinimumHeight(currentMetrics.sliderVisualHeight);
  alphaSlider_->setMaximumHeight(currentMetrics.sliderVisualHeight);
  alphaSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  alphaRow->addWidget(alphaSlider_);
  sliderGroupLayout->addWidget(alphaSection_);

  sliderContainerLayout->addWidget(sliderGroup_, 1);

  previewSwatch_ = new ColorPickerSwatch(sliderContainer_);
  previewSwatch_->setObjectName(QStringLiteral("ad-color-picker-preview-swatch"));
  previewSwatch_->setFixedSize(currentMetrics.previewSwatchSize, currentMetrics.previewSwatchSize);
  sliderContainerLayout->addWidget(previewSwatch_, 0, Qt::AlignCenter);
  syncPreviewContentWidget();
  sliderContainer_->setFixedHeight(sliderContainerHeightFromMetrics(
      currentMetrics.previewSwatchSize, currentMetrics.sliderVisualHeight,
      sliderGroupGapFromMetrics(currentMetrics.marginSM, currentMetrics.sliderMarginCross),
      disabledAlpha_));
  pickerLayout->addWidget(sliderContainer_);

  sliderGap_ = new QWidget(pickerPanel_);
  sliderGap_->setObjectName(QStringLiteral("ad-color-picker-slider-gap"));
  sliderGap_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  sliderGap_->setFixedHeight(
      sliderSectionGapFromMetrics(currentMetrics.marginSM, currentMetrics.sliderMarginCross));
  pickerLayout->addWidget(sliderGap_);

  auto* formatRowWidget = new QWidget(pickerPanel_);
  formatRowWidget->setObjectName(QStringLiteral("ad-color-picker-format-row"));
  auto* formatRow = new QHBoxLayout(formatRowWidget);
  if (formatRowWidget->layoutDirection() == Qt::RightToLeft) {
    formatRow->setContentsMargins(0, 0, currentMetrics.sliderMarginMain, 0);
  } else {
    formatRow->setContentsMargins(currentMetrics.sliderMarginMain, 0, 0, 0);
  }
  formatRow->setSpacing(0);
  formatCombo_ = new AdComboBox(formatRowWidget);
  formatCombo_->setObjectName(QString::fromLatin1(kFormatSelectObjectName));
  formatCombo_->setControlSize(AdComboBox::ControlSize::Small);
  formatCombo_->setVariant(AdComboBox::Variant::Borderless);
  formatCombo_->setSearchable(false);
  formatCombo_->setAllowClear(false);
  formatCombo_->setPlacement(AdComboBox::Placement::BottomRight);
  formatCombo_->setPopupWidthMode(AdComboBox::PopupWidthMode::FixedWidth);
  formatCombo_->setPopupWidth(kFormatSelectPopupWidth);
  const adqt::theme::ResolvedTheme resolvedFormat =
      adqt::theme::ThemeManager::instance().resolve(formatCombo_, themeLogicalOwner());
  const auto& mapForFormat = resolvedFormat.values;
  AdComboBox::ComponentTokens formatSelectTokens;
  formatSelectTokens.metrics.horizontalPadding = 0;
  formatSelectTokens.metrics.borderWidth = 0;
  formatSelectTokens.metrics.iconSize = formatSelectIconSizeFromMap(mapForFormat);
  formatCombo_->setComponentTokens(formatSelectTokens);
  auto* formatModel = new QStandardItemModel(formatCombo_);
  for (const QPair<QString, QString>& item :
       {QPair<QString, QString>{QStringLiteral("hex"), QStringLiteral("HEX")},
        QPair<QString, QString>{QStringLiteral("hsb"), QStringLiteral("HSB")},
        QPair<QString, QString>{QStringLiteral("rgb"), QStringLiteral("RGB")}}) {
    auto* row = new QStandardItem(item.second);
    row->setData(item.second, Qt::DisplayRole);
    row->setData(item.first, AdComboBox::DefaultValueRole);
    row->setData(item.second, AdComboBox::DefaultLabelRole);
    formatModel->appendRow(row);
  }
  formatCombo_->setModel(formatModel);
  formatInputHost_ = new QWidget(formatRowWidget);
  formatInputHost_->setObjectName(QStringLiteral("ad-color-picker-format-input-host"));
  formatInputHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  formatInputHost_->setMinimumWidth(0);
  formatInputStack_ = new QStackedLayout(formatInputHost_);
  formatInputStack_->setContentsMargins(0, 0, 0, 0);
  formatInputStack_->setSpacing(0);
  formatInputStack_->setStackingMode(QStackedLayout::StackOne);

  hexInput_ = new AdLineEdit(formatInputHost_);
  hexInput_->setObjectName(QStringLiteral("ad-color-picker-hex-input"));
  hexInput_->setProperty("ad-flex-min-width-zero", true);
  hexInput_->setPrefixText(QStringLiteral("#"));
  hexInput_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  hexInput_->setMinimumWidth(0);
  formatInputStack_->addWidget(hexInput_);

  alphaInput_ = createColorPickerStepperInput(
      formatRowWidget, QStringLiteral("ad-color-picker-alpha-input"), 0, 100, 1, 0,
      QStringLiteral("%"), QStringLiteral("100"), currentMetrics.alphaInputWidth);
  formatRow->addWidget(formatCombo_);
  auto* formatSelectGap = new QWidget(formatRowWidget);
  formatSelectGap->setObjectName(QString::fromLatin1(kFormatSelectGapObjectName));
  formatSelectGap->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  formatRow->addWidget(formatSelectGap);
  formatRow->addWidget(formatInputHost_, 1);
  auto* formatAlphaGap = new QWidget(formatRowWidget);
  formatAlphaGap->setObjectName(QString::fromLatin1(kFormatAlphaGapObjectName));
  formatAlphaGap->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  formatRow->addWidget(formatAlphaGap);
  formatRow->addWidget(alphaInput_);
  pickerLayout->addWidget(formatRowWidget);

  if (saturationPanel_) {
    saturationPanel_->setChangeCallback([this](double, double, bool completed) {
      if (syncingControls_) {
        return;
      }
      setCurrentFromControls(completed, LivePanelSyncSource::SaturationPanel);
    });
  }

  auto bindChannel = [this](AdSlider* slider, LivePanelSyncSource source) {
    connect(slider, &AdSlider::valueChanged, this, [this, source](double) {
      if (syncingControls_) {
        return;
      }
      setCurrentFromControls(false, source);
    });
    connect(slider, &AdSlider::editingFinished, this, [this, source]() {
      if (syncingControls_) {
        return;
      }
      setCurrentFromControls(true, source);
    });
  };

  bindChannel(hueSlider_, LivePanelSyncSource::HueSlider);
  bindChannel(alphaSlider_, LivePanelSyncSource::AlphaSlider);

  connect(formatCombo_, &AdComboBox::currentDataChanged, this, [this](const QVariant& value) {
    if (syncingControls_ || !formatCombo_) {
      return;
    }
    setFormat(parseFormatName(value.toString(), format_));
  });

  if (hexInput_) {
    connect(hexInput_, &AdLineEdit::textEdited, this, [this](const QString& text) {
      if (syncingControls_ || !hexInput_ || format_ != Format::Hex) {
        return;
      }
      const QString visibleHex = sanitizeHexInput(text, 8);
      if (visibleHex != text) {
        hexInput_->setText(visibleHex);
      }
      previewFormatInputs();
    });
    connect(hexInput_, &AdLineEdit::editingFinished, this, [this]() { commitFormatInputs(); });
  }

  bindColorPickerStepperInput(
      this, alphaInput_, [this]() { previewAlphaInput(); },
      [this]() {
        if (syncingControls_ || !alphaInput_) {
          return;
        }
        int percent = 0;
        if (!parseBoundedInt(alphaInput_->value(), 0, 100, &percent)) {
          refreshPanelControlsFromState();
          return;
        }

        QColor current = currentEditableColor().toHsv();
        int hue = current.hue();
        if (hue < 0) {
          hue = 0;
        }
        const int alpha = std::clamp(qRound(percent * 255.0 / 100.0), 0, 255);
        const QColor next = QColor::fromHsv(hue, current.saturation(), current.value(), alpha);
        setCurrentEditableColor(next, true, true, true);
      });

  ensureFormatInputUi(format_);

  if (auto* hostLayout = qobject_cast<QVBoxLayout*>(panelHost_->layout())) {
    hostLayout->addWidget(pickerPanel_);
  }
  attachPopupContentToPanel();

  updateModeSegmentedOptions();
  rebuildPresetsPanel();

  refreshStyle();
  stabilizeWidgetLayoutTree(panelHost_);
  panelHost_->adjustSize();
  if (hostMode_ == HostMode::PanelOnly) {
    setHostedRootWidget(panelHost_);
  } else {
    attachPanelHostToPopover();
  }
}

void AdColorPicker::rebuildPresetsPanel() {
  if ((!presetsPanel_ || !presetsLayout_) && !presets_.isEmpty()) {
    ensurePresetsUi();
  }
  if (!presetsPanel_ || !presetsLayout_) {
    return;
  }

  while (QLayoutItem* item = presetsLayout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }

  if (presets_.isEmpty()) {
    presetsPanel_->setVisible(false);
    return;
  }

  for (int presetIndex = 0; presetIndex < presets_.size(); ++presetIndex) {
    const PresetItem& preset = presets_.at(presetIndex);
    auto* group = new QWidget(presetsPanel_);
    group->setObjectName(QString::fromLatin1(kPresetGroupObjectName));
    group->setProperty("ad-color-picker-preset-index", presetIndex);
    group->setProperty("ad-color-picker-preset-key",
                       preset.key.trimmed().isEmpty() ? QString::number(presetIndex) : preset.key);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(0, 0, 0, 0);
    groupLayout->setSpacing(0);

    QWidget* body = new QWidget(group);
    body->setObjectName(QString::fromLatin1(kPresetBodyObjectName));
    body->setVisible(preset.defaultOpen || preset.label.trimmed().isEmpty());
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    if (!preset.label.trimmed().isEmpty()) {
      auto* header = new PresetCollapseHeaderButton(group);
      header->setObjectName(QString::fromLatin1(kPresetHeaderObjectName));
      header->setLabel(preset.label);
      header->setExpanded(preset.defaultOpen);
      connect(header, &QAbstractButton::clicked, this, [header, body]() {
        if (!header || !body) {
          return;
        }
        const bool nextExpanded = !header->expanded();
        header->setExpanded(nextExpanded);
        body->setVisible(nextExpanded);
      });
      groupLayout->addWidget(header);
    }

    if (preset.colors.isEmpty()) {
      auto* empty = new QLabel(tr("No preset colors"), body);
      empty->setObjectName(QString::fromLatin1(kPresetEmptyObjectName));
      bodyLayout->addWidget(empty);
    } else {
      auto* itemsHost = new QWidget(body);
      itemsHost->setObjectName(QString::fromLatin1(kPresetItemsObjectName));
      auto* itemsLayout = new QGridLayout(itemsHost);
      itemsLayout->setContentsMargins(0, 0, 0, 0);
      itemsLayout->setSpacing(0);

      for (int colorIndex = 0; colorIndex < preset.colors.size(); ++colorIndex) {
        const QtColorValue& value = preset.colors.at(colorIndex);
        const ColorValue selectionValue = toLegacyColorValue(value);
        auto* swatch = new PresetColorButton(itemsHost);
        swatch->setObjectName(QString::fromLatin1(kPresetSwatchObjectName));
        swatch->setToolTip(colorValueToCss(selectionValue));
        swatch->setProperty("ad-color-picker-css", colorValueToCss(selectionValue));
        swatch->setProperty("ad-color-picker-order", colorIndex);
        swatch->setColorValue(selectionValue);
        connect(swatch, &QAbstractButton::clicked, this, [this, value]() { applyPreset(value); });
      }

      bodyLayout->addWidget(itemsHost);
    }

    groupLayout->addWidget(body);
    presetsLayout_->addWidget(group);
  }

  presetsPanel_->setVisible(true);
}

void AdColorPicker::refreshStyle(bool preserveCurrentTriggerWidth) {
  invalidateStyleCache();
  const detail::ColorPickerVisualStyle style = visualStyle();
  const adqt::theme::ThemeMapToken mapToken =
      adqt::theme::ThemeManager::instance().resolve(this, themeLogicalOwner()).values;

  const int controlHeight = controlHeightForSize(size_, style);
  const int swatchSize = swatchSizeForSize(size_, style);
  const int triggerRadius = triggerRadiusForSize(size_, style);
  const int triggerMinWidth =
      componentTokens_.triggerMinWidth.has_value() ? style.metrics.triggerMinWidth : controlHeight;
  syncTriggerWidthLock(preserveCurrentTriggerWidth);

  if (defaultTrigger_) {
    defaultTrigger_->setMinimumHeight(controlHeight);
    defaultTrigger_->setMaximumHeight(controlHeight);
    const int effectiveTriggerWidth = std::max(triggerMinWidth, lockedTriggerWidth_);
    defaultTrigger_->setMinimumWidth(effectiveTriggerWidth);
    defaultTrigger_->setMaximumWidth(lockedTriggerWidth_ > 0 ? effectiveTriggerWidth
                                                             : QWIDGETSIZE_MAX);
    defaultTrigger_->setFont(style.metrics.font);
  }

  if (triggerFrame_) {
    QColor border = style.triggerBorder;
    if (!disabled()) {
      border = popupVisible() ? style.triggerBorderActive
                              : (triggerHovered_ ? style.triggerBorderHover : style.triggerBorder);
    }
    triggerFrame_->setProperty("ad-color-picker-border-color", border.name(QColor::HexArgb));

    if (auto* triggerLayout = qobject_cast<QHBoxLayout*>(triggerFrame_->layout())) {
      const int pad = std::max(0, style.metrics.triggerPadding);
      // Keep the swatch leading inset consistent between color-only and showText modes.
      const int startPad =
          triggerTextVisible_ ? std::max(pad, std::max(0, (controlHeight - swatchSize) / 2)) : pad;
      const int endPad = triggerTextVisible_
                             ? std::max(0, pad + std::max(0, style.metrics.triggerTextMarginEnd))
                             : pad;
      triggerLayout->setContentsMargins(startPad, pad, endPad, pad);
      triggerLayout->setSpacing(triggerTextVisible_ ? std::max(0, style.metrics.triggerTextGap)
                                                    : 0);
      triggerLayout->setAlignment(triggerTextVisible_ ? (Qt::AlignLeft | Qt::AlignVCenter)
                                                      : (Qt::AlignHCenter | Qt::AlignVCenter));
    }

    triggerFrame_->setCursor(disabled() ? Qt::ForbiddenCursor : Qt::PointingHandCursor);
    if (auto* paintedTrigger = dynamic_cast<ColorPickerTriggerFrame*>(triggerFrame_.data())) {
      paintedTrigger->setVisualStyle(
          disabled() ? style.triggerBackgroundDisabled : style.triggerBackground, border,
          style.metrics.borderWidth, triggerRadius);
    }
  }

  if (triggerSwatch_) {
    triggerSwatch_->setFixedSize(swatchSize, swatchSize);
  }

  if (triggerTextLabel_) {
    QFont textFont = style.metrics.font;
    if (size_ == Size::Large) {
      textFont.setPixelSize(std::max(textFont.pixelSize(), style.metrics.triggerTextFontSizeLG));
    }
    triggerTextLabel_->setFont(textFont);
    triggerTextLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (size_ == Size::Small) {
      const int lineHeight = std::max(0, style.metrics.triggerTextLineHeightSM);
      triggerTextLabel_->setMinimumHeight(lineHeight);
      triggerTextLabel_->setMaximumHeight(lineHeight);
    } else {
      triggerTextLabel_->setMinimumHeight(0);
      triggerTextLabel_->setMaximumHeight(QWIDGETSIZE_MAX);
    }

    QPalette palette = triggerTextLabel_->palette();
    palette.setColor(QPalette::WindowText,
                     disabled() ? style.triggerTextDisabled : style.triggerText);
    triggerTextLabel_->setPalette(palette);
    triggerTextLabel_->setVisible(triggerTextVisible_);
  }

  int panelContentWidth = style.metrics.panelWidth;
  if (panelHost_) {
    if (componentTokens_.panelWidth.has_value()) {
      // Align with antd `styles.popupOverlayInner.width`: explicit width targets
      // popup container width, so content width should exclude popup paddings.
      const int popupPadding = std::max(0, style.metrics.panelPadding);
      panelContentWidth = std::max(1, style.metrics.panelWidth - popupPadding * 2);
    }
    panelHost_->setFixedWidth(panelContentWidth);
  }

  if (pickerPanel_) {
    pickerPanel_->setFont(style.metrics.font);
    if (auto* pickerLayout = qobject_cast<QVBoxLayout*>(pickerPanel_->layout())) {
      pickerLayout->setSpacing(0);
    }
  }
  if (presetsPanel_) {
    presetsPanel_->setFont(style.metrics.font);
    const int presetBodyPadding = std::max(0, qRound(mapToken.sizeXS));
    if (presetsLayout_) {
      presetsLayout_->setSpacing(std::max(0, qRound(mapToken.sizeXXS)));
      presetsLayout_->setContentsMargins(0, presetBodyPadding, 0, 0);
    }
    if (auto* presetsFrame = dynamic_cast<ColorPickerPresetsPanel*>(presetsPanel_.data())) {
      presetsFrame->setVisualStyle(style.metrics.borderWidth, style.panelBorder);
    }

    QFont presetLabelFont = style.metrics.font;
    presetLabelFont.setPixelSize(std::max(12, qRound(mapToken.fontSizeSM)));
    const QColor presetLabelColor = style.presetText;
    const QColor presetArrowColor = style.presetArrow;
    const QColor presetEmptyColor = style.presetEmptyText;

    const int presetHeaderGap = std::max(0, qRound(mapToken.sizeXXS));
    const int presetHeaderHeight = std::max(
        16, std::max(qRound(mapToken.fontHeightSM), QFontMetrics(presetLabelFont).height()));

    for (QWidget* group : presetsPanel_->findChildren<QWidget*>(
             QString::fromLatin1(kPresetGroupObjectName), Qt::FindDirectChildrenOnly)) {
      if (!group) {
        continue;
      }
      if (auto* groupLayout = qobject_cast<QVBoxLayout*>(group->layout())) {
        groupLayout->setSpacing(0);
      }
    }

    for (QAbstractButton* headerButton : presetsPanel_->findChildren<QAbstractButton*>(
             QString::fromLatin1(kPresetHeaderObjectName), Qt::FindChildrenRecursively)) {
      auto* header = dynamic_cast<PresetCollapseHeaderButton*>(headerButton);
      if (!header) {
        continue;
      }
      header->setVisualStyle(presetLabelFont, presetLabelColor, presetArrowColor,
                             std::max(10, qRound(mapToken.fontHeightSM)), presetHeaderGap,
                             presetHeaderHeight);
      header->setEnabled(!disabled());
    }

    for (QWidget* body : presetsPanel_->findChildren<QWidget*>(
             QString::fromLatin1(kPresetBodyObjectName), Qt::FindChildrenRecursively)) {
      if (!body) {
        continue;
      }
      if (auto* bodyLayout = qobject_cast<QVBoxLayout*>(body->layout())) {
        bodyLayout->setContentsMargins(0, presetBodyPadding, 0, presetBodyPadding);
        bodyLayout->setSpacing(0);
      }
    }

    for (QLabel* empty : presetsPanel_->findChildren<QLabel*>(
             QString::fromLatin1(kPresetEmptyObjectName), Qt::FindChildrenRecursively)) {
      if (!empty) {
        continue;
      }
      empty->setFont(presetLabelFont);
      QPalette palette = empty->palette();
      palette.setColor(QPalette::WindowText, presetEmptyColor);
      empty->setPalette(palette);
    }
  }

  if (sliderContainer_) {
    const int sliderGroupGap =
        sliderGroupGapFromMetrics(style.metrics.marginSM, style.metrics.sliderMarginCross);
    const int sliderContainerHeight = sliderContainerHeightFromMetrics(
        style.metrics.previewSwatchSize, style.metrics.sliderVisualHeight, sliderGroupGap,
        disabledAlpha_);
    sliderContainer_->setMinimumHeight(sliderContainerHeight);
    sliderContainer_->setMaximumHeight(sliderContainerHeight);
    if (auto* sliderContainerLayout = qobject_cast<QHBoxLayout*>(sliderContainer_->layout())) {
      sliderContainerLayout->setSpacing(std::max(0, style.metrics.marginSM));
    }
  }

  if (sliderGroup_) {
    if (auto* sliderGroupLayout = qobject_cast<QVBoxLayout*>(sliderGroup_->layout())) {
      const int sliderGap =
          sliderGroupGapFromMetrics(style.metrics.marginSM, style.metrics.sliderMarginCross);
      sliderGroupLayout->setSpacing(sliderGap);
      if (hueSlider_) {
        sliderGroupLayout->setAlignment(hueSlider_,
                                        disabledAlpha_ ? Qt::AlignVCenter : Qt::Alignment());
      }
    }
  }

  const QVector<Mode> normalizedModes = normalizeModeOptions(modeOptions_);
  const bool showModeSwitch = normalizedModes.size() > 1;
  const bool showOperationRow = showModeSwitch || allowClear_;
  if (pickerPanel_ && showOperationRow) {
    ensureOperationUi();
  }
  if (pickerPanel_ && allowClear_) {
    ensureClearButtonUi();
  }

  if (operationRow_) {
    operationRow_->setVisible(showOperationRow);
  }
  if (modeSegmented_) {
    modeSegmented_->setVisible(showModeSwitch);
    modeSegmented_->setDisabled(!(showModeSwitch && !disabled()));
    applyModeSegmentedStyle(modeSegmented_, style, mapToken);
  }
  if (formatCombo_) {
    const int formatSelectFontSize = std::max(8, qRound(mapToken.fontSizeSM));
    const int formatSelectIconSize = formatSelectIconSizeFromMap(mapToken);
    const int formatSelectArrowGap = std::max(0, qRound(mapToken.sizeXXS));
    QFont formatSelectFont = style.metrics.font;
    formatSelectFont.setPixelSize(formatSelectFontSize);

    AdComboBox::ComponentTokens formatTokens = formatCombo_->componentTokens();
    bool formatTokensChanged = false;
    const bool formatControlHeightSynced =
        formatTokens.metrics.controlHeight.has_value() &&
        formatTokens.metrics.controlHeight.value() == style.metrics.inputHeight;
    if (!formatControlHeightSynced) {
      formatTokens.metrics.controlHeight = style.metrics.inputHeight;
      formatTokensChanged = true;
    }
    const bool formatSelectorFontSynced =
        formatTokens.metrics.selectorFontSize.has_value() &&
        formatTokens.metrics.selectorFontSize.value() == formatSelectFontSize;
    if (!formatSelectorFontSynced) {
      formatTokens.metrics.selectorFontSize = formatSelectFontSize;
      formatTokensChanged = true;
    }
    const bool formatOptionFontSynced =
        formatTokens.metrics.optionFontSize.has_value() &&
        formatTokens.metrics.optionFontSize.value() == formatSelectFontSize;
    if (!formatOptionFontSynced) {
      formatTokens.metrics.optionFontSize = formatSelectFontSize;
      formatTokensChanged = true;
    }
    const bool formatIconSynced = formatTokens.metrics.iconSize.has_value() &&
                                  formatTokens.metrics.iconSize.value() == formatSelectIconSize;
    if (!formatIconSynced) {
      formatTokens.metrics.iconSize = formatSelectIconSize;
      formatTokensChanged = true;
    }
    if (formatTokensChanged) {
      formatCombo_->setComponentTokens(formatTokens);
    }
    formatCombo_->setControlSize(AdComboBox::ControlSize::Small);
    formatCombo_->setVisible(!disabledFormat_);
    formatCombo_->setMinimumHeight(style.metrics.inputHeight);
    formatCombo_->setMaximumHeight(style.metrics.inputHeight);
    const int formatWidth =
        formatSelectWidthHint(formatSelectFont, formatSelectIconSize, formatSelectArrowGap);
    if (formatCombo_->minimumWidth() != formatWidth ||
        formatCombo_->maximumWidth() != formatWidth) {
      formatCombo_->setFixedWidth(formatWidth);
    }
    formatCombo_->setDisabled(disabledFormat_ || disabled());
  }
  if (auto* formatRowWidget = pickerPanel_ ? pickerPanel_->findChild<QWidget*>(
                                                 QStringLiteral("ad-color-picker-format-row"),
                                                 Qt::FindDirectChildrenOnly)
                                           : nullptr) {
    const int sliderInlineStartInset = std::max(0, style.metrics.sliderMarginMain);
    const int formatSelectGapWidth = std::max(0, style.metrics.marginXS);
    const int formatAlphaGapWidth = std::max(2, qRound(mapToken.sizeXXS));
    const bool showFormatSelect = formatCombo_ && !disabledFormat_;
    const bool showAlphaInput = alphaInput_ && !disabledAlpha_;
    QHBoxLayout* formatRowLayout = qobject_cast<QHBoxLayout*>(formatRowWidget->layout());
    if (formatRowLayout) {
      // AdSlider keeps a main-axis inset so handle rings are not clipped by QWidget bounds.
      // Compensate the input row inline-start with the same inset to align with AntD visuals.
      if (formatRowWidget->layoutDirection() == Qt::RightToLeft) {
        formatRowLayout->setContentsMargins(0, 0, sliderInlineStartInset, 0);
      } else {
        formatRowLayout->setContentsMargins(sliderInlineStartInset, 0, 0, 0);
      }
      formatRowLayout->setSpacing(0);
    }
    QWidget* formatSelectGap = formatRowWidget->findChild<QWidget*>(
        QString::fromLatin1(kFormatSelectGapObjectName), Qt::FindDirectChildrenOnly);
    if (formatSelectGap) {
      formatSelectGap->setFixedWidth(formatSelectGapWidth);
      formatSelectGap->setVisible(showFormatSelect && formatSelectGapWidth > 0);
    }
    QWidget* formatAlphaGap = formatRowWidget->findChild<QWidget*>(
        QString::fromLatin1(kFormatAlphaGapObjectName), Qt::FindDirectChildrenOnly);
    if (formatAlphaGap) {
      formatAlphaGap->setFixedWidth(formatAlphaGapWidth);
      formatAlphaGap->setVisible(showAlphaInput && formatAlphaGapWidth > 0);
    }
    if (formatInputHost_) {
      const QMargins rowMargins = formatRowLayout ? formatRowLayout->contentsMargins() : QMargins();
      // The row has only its provisional QWidget width during the first style pass.
      // Size it from the panel's target width so the initial popup matches later refreshes.
      int availableWidth = std::max(0, panelContentWidth - rowMargins.left() - rowMargins.right());
      if (showFormatSelect) {
        const int formatWidth = formatCombo_->maximumWidth() > 0 ? formatCombo_->maximumWidth()
                                                                 : formatCombo_->sizeHint().width();
        availableWidth = std::max(0, availableWidth - formatWidth);
      }
      if (formatSelectGap && showFormatSelect && formatSelectGapWidth > 0) {
        availableWidth = std::max(0, availableWidth - formatSelectGap->width());
      }
      if (showAlphaInput) {
        const int alphaWidth = alphaInput_->maximumWidth() > 0 ? alphaInput_->maximumWidth()
                                                               : style.metrics.alphaInputWidth;
        availableWidth = std::max(0, availableWidth - alphaWidth);
      }
      if (formatAlphaGap && showAlphaInput && formatAlphaGapWidth > 0) {
        availableWidth = std::max(0, availableWidth - formatAlphaGap->width());
      }
      formatInputHost_->setMinimumWidth(availableWidth);
      formatInputHost_->setMaximumWidth(availableWidth);
    }
  }
  const int colorInputFontSize = std::max(8, qRound(mapToken.fontSizeSM));
  const int colorHexHorizontalPadding = std::max(0, qRound(mapToken.sizeXS));
  const int compactHorizontalPadding = std::max(0, qRound(mapToken.sizeXXS));
  const int inputNumberHandleWidth = 16;

  auto applyFormatInputStyle = [&](AdLineEdit* input, int horizontalPadding) {
    if (!input) {
      return;
    }

    QFont compactFont = input->font();
    compactFont.setPixelSize(colorInputFontSize);
    input->setFont(compactFont);
    input->setProperty("ad-input-horizontal-padding", horizontalPadding);

    // Ant Design keeps color picker panel inputs compact regardless of trigger size.
    input->setControlSize(AdLineEdit::ControlSize::Small);
    input->setMinimumHeight(style.metrics.inputHeight);
    input->setMaximumHeight(style.metrics.inputHeight);
    input->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    input->setMinimumWidth(0);
    input->setDisabled(disabled());
  };

  auto applyFormatNumberInputStyle = [&](AdInputNumber* input, bool fixedAlphaWidth) {
    applyColorPickerStepperStyle(
        input, style.metrics.inputHeight, compactHorizontalPadding, colorInputFontSize, 48,
        inputNumberHandleWidth, disabled() || (fixedAlphaWidth && disabledAlpha_),
        fixedAlphaWidth ? style.metrics.alphaInputWidth : 0, !fixedAlphaWidth || !disabledAlpha_);
  };

  applyFormatInputStyle(hexInput_, colorHexHorizontalPadding);
  applyFormatNumberInputStyle(rgbInputR_, false);
  applyFormatNumberInputStyle(rgbInputG_, false);
  applyFormatNumberInputStyle(rgbInputB_, false);
  applyFormatNumberInputStyle(hsbInputH_, false);
  applyFormatNumberInputStyle(hsbInputS_, false);
  applyFormatNumberInputStyle(hsbInputB_, false);
  applyFormatNumberInputStyle(alphaInput_, true);

  if (formatInputHost_) {
    formatInputHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    if (formatInputStack_) {
      formatInputStack_->setContentsMargins(0, 0, 0, 0);
      formatInputStack_->setSpacing(0);
    }
    formatInputHost_->setMinimumWidth(0);
  }
  if (rgbInputHost_) {
    rgbInputHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    if (auto* rgbLayout = qobject_cast<QHBoxLayout*>(rgbInputHost_->layout())) {
      rgbLayout->setSpacing(std::max(2, qRound(mapToken.sizeXXS)));
    }
    rgbInputHost_->setMinimumWidth(0);
  }
  if (hsbInputHost_) {
    hsbInputHost_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    if (auto* hsbLayout = qobject_cast<QHBoxLayout*>(hsbInputHost_->layout())) {
      hsbLayout->setSpacing(std::max(2, qRound(mapToken.sizeXXS)));
    }
    hsbInputHost_->setMinimumWidth(0);
  }

  updateFormatInputVisibility();

  if (clearButton_) {
    clearButton_->setVisible(allowClear_);
    clearButton_->setDisabled(disabled());
    const int clearSize = std::max(12, style.metrics.presetSwatchSize);
    clearButton_->setMinimumSize(clearSize, clearSize);
    clearButton_->setMaximumSize(clearSize, clearSize);
    if (auto* clear = dynamic_cast<ColorPickerClearButton*>(clearButton_.data())) {
      clear->setVisualStyle(style.panelBackground, style.panelBorder, style.triggerBorder,
                            style.clearButtonSlash, style.metrics.borderWidth,
                            style.metrics.swatchRadius);
    }
  }

  if (gradientSection_) {
    gradientSection_->setVisible(mode_ == Mode::Gradient);
  }

  auto updateGapWidget = [](QWidget* gap, int height, bool visible) {
    if (!gap) {
      return;
    }
    const int targetHeight = std::max(0, height);
    if (gap->minimumHeight() != targetHeight || gap->maximumHeight() != targetHeight) {
      gap->setFixedHeight(targetHeight);
    }
    gap->setVisible(visible && targetHeight > 0);
  };
  const int sliderSectionGap =
      sliderSectionGapFromMetrics(style.metrics.marginSM, style.metrics.sliderMarginCross);
  updateGapWidget(operationGap_, style.metrics.marginXS, showOperationRow);
  updateGapWidget(gradientGap_, style.metrics.marginXS,
                  gradientSection_ && mode_ == Mode::Gradient);
  updateGapWidget(saturationGap_, sliderSectionGap, saturationPanel_);
  updateGapWidget(sliderGap_, sliderSectionGap, sliderContainer_);

  auto buildCommonSliderTokens = [&style, &mapToken]() {
    AdSliderComponentTokens tokens;
    tokens.controlSize = style.metrics.sliderControlSize;
    tokens.railSize = style.metrics.sliderHeight;
    tokens.handleLineWidth = style.metrics.sliderHandleLineWidth;
    tokens.handleLineWidthHover = style.metrics.sliderHandleLineWidthHover;
    tokens.handleShadowColor = mapToken.colorFillSecondary;
    tokens.handleActiveShadowColor = mapToken.colorPrimaryActive;
    tokens.marginMain = style.metrics.sliderMarginMain;
    tokens.marginCross = style.metrics.sliderMarginCross;
    tokens.markGap = 0;
    tokens.focusOutlineSize = 0;
    return tokens;
  };

  if (gradientSlider_) {
    AdSliderComponentTokens tokens = buildCommonSliderTokens();
    tokens.handleSize = style.metrics.gradientHandleSize;
    tokens.handleSizeHover = style.metrics.gradientHandleSizeHover;
    gradientSlider_->setComponentTokens(tokens);
    gradientSlider_->setMinimumHeight(style.metrics.sliderVisualHeight);
    gradientSlider_->setMaximumHeight(style.metrics.sliderVisualHeight);
    gradientSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  if (gradientSlider_) {
    gradientSlider_->setEnabled(!disabled());
  }

  if (saturationPanel_) {
    saturationPanel_->setEnabled(!disabled());
    saturationPanel_->setMinimumHeight(style.metrics.saturationPanelHeight);
    saturationPanel_->setMaximumHeight(style.metrics.saturationPanelHeight);
  }

  if (alphaSection_) {
    alphaSection_->setVisible(!disabledAlpha_);
  }
  if (hueSlider_) {
    AdSliderComponentTokens tokens = buildCommonSliderTokens();
    tokens.handleSize = style.metrics.sliderHandleSize;
    tokens.handleSizeHover = style.metrics.sliderHandleSizeHover;
    hueSlider_->setComponentTokens(tokens);
    hueSlider_->setMinimumHeight(style.metrics.sliderVisualHeight);
    hueSlider_->setMaximumHeight(style.metrics.sliderVisualHeight);
    hueSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    hueSlider_->setEnabled(!disabled());
  }
  if (alphaSlider_) {
    AdSliderComponentTokens tokens = buildCommonSliderTokens();
    tokens.handleSize = style.metrics.sliderHandleSize;
    tokens.handleSizeHover = style.metrics.sliderHandleSizeHover;
    alphaSlider_->setComponentTokens(tokens);
    alphaSlider_->setMinimumHeight(style.metrics.sliderVisualHeight);
    alphaSlider_->setMaximumHeight(style.metrics.sliderVisualHeight);
    alphaSlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    alphaSlider_->setEnabled(!disabledAlpha_ && !disabled());
  }
  if (previewSwatch_) {
    previewSwatch_->setFixedSize(style.metrics.previewSwatchSize, style.metrics.previewSwatchSize);
  }
  if (previewContent_) {
    previewContent_->setFixedSize(style.metrics.previewSwatchSize, style.metrics.previewSwatchSize);
  }

  if (popover_) {
    popover_->setBackgroundColor(style.panelBackground);
    popover_->setBorderColor(style.panelBorder);
    popover_->setCornerRadius(style.metrics.triggerRadius);
    popover_->setBorderWidth(qRound(style.metrics.borderWidth));
    const int panelPadding = std::max(0, style.metrics.panelPadding);
    popover_->setContentMargins(QMargins(panelPadding, panelPadding, panelPadding, panelPadding));
    popover_->setPlacement(toPopoverPlacement(placement()));
    popover_->setTriggers(toPopoverTriggers(trigger()));
    popover_->setEnabled(!disabled());
  }

  const int presetOutlinePadding = std::max(2, qRound(style.metrics.borderWidth) * 2);
  const int presetLayoutGap = std::max(2, qRound(mapToken.sizeXXS * 0.5));
  const int presetOuterSize = style.metrics.presetSwatchSize + presetOutlinePadding * 2;
  int presetContentWidth = panelHost_ ? panelHost_->width() : style.metrics.panelWidth;
  if (presetContentWidth <= 0) {
    presetContentWidth = style.metrics.panelWidth;
  }
  const int presetColumns =
      std::max(1, (presetContentWidth + presetLayoutGap) / (presetOuterSize + presetLayoutGap));

  const QColor hoverOutlineColor =
      QColor(mapToken.colorFill).isValid() ? QColor(mapToken.colorFill) : style.presetBorderHover;
  const QColor checkmarkColor =
      QColor(mapToken.colorWhite).isValid() ? QColor(mapToken.colorWhite) : QColor("#ffffff");
  const QColor brightCheckmarkColor(0, 0, 0, 115);

  auto compositeOn = [](const QColor& foreground, const QColor& background) {
    const float alpha = std::clamp(foreground.alphaF(), 0.0F, 1.0F);
    if (alpha >= 0.999) {
      return foreground;
    }
    QColor composite;
    composite.setRedF(foreground.redF() * alpha + background.redF() * (1.0F - alpha));
    composite.setGreenF(foreground.greenF() * alpha + background.greenF() * (1.0F - alpha));
    composite.setBlueF(foreground.blueF() * alpha + background.blueF() * (1.0F - alpha));
    composite.setAlphaF(1.0F);
    return composite;
  };

  auto samplePresetColor = [this, &compositeOn, &style](const ColorValue& value) {
    if (value.isGradient() && !value.gradientStops.isEmpty()) {
      QVector<InternalGradientStop> parsedStops;
      parsedStops.reserve(value.gradientStops.size());
      for (const GradientStop& stop : value.gradientStops) {
        if (!stop.color.isValid()) {
          continue;
        }
        parsedStops.append(InternalGradientStop{stop.color, std::clamp(stop.percent, 0, 100)});
      }

      const QVector<InternalGradientStop> normalized = normalizeGradientStops(parsedStops);
      if (!normalized.isEmpty()) {
        const double samplePercent = 50.0;
        if (normalized.size() == 1) {
          return compositeOn(normalized.constFirst().color, style.panelBackground);
        }

        for (int index = 0; index < normalized.size() - 1; ++index) {
          const InternalGradientStop& lhs = normalized.at(index);
          const InternalGradientStop& rhs = normalized.at(index + 1);
          if (samplePercent < lhs.percent || samplePercent > rhs.percent) {
            continue;
          }
          const double span = std::max(1.0, static_cast<double>(rhs.percent - lhs.percent));
          const double t = std::clamp((samplePercent - lhs.percent) / span, 0.0, 1.0);
          const float blend = static_cast<float>(t);
          QColor mixed;
          mixed.setRedF(lhs.color.redF() + (rhs.color.redF() - lhs.color.redF()) * blend);
          mixed.setGreenF(lhs.color.greenF() + (rhs.color.greenF() - lhs.color.greenF()) * blend);
          mixed.setBlueF(lhs.color.blueF() + (rhs.color.blueF() - lhs.color.blueF()) * blend);
          mixed.setAlphaF(lhs.color.alphaF() + (rhs.color.alphaF() - lhs.color.alphaF()) * blend);
          return compositeOn(mixed, style.panelBackground);
        }

        return compositeOn(normalized.constFirst().color, style.panelBackground);
      }
    }

    return value.solidColor.isValid() ? compositeOn(value.solidColor, style.panelBackground)
                                      : style.panelBackground;
  };

  auto isPresetBright = [&samplePresetColor](const ColorValue& value) {
    const QColor sample = samplePresetColor(value);
    const QColor hsv = sample.toHsv();
    if (sample.alphaF() <= 0.5) {
      return hsv.valueF() > 0.5;
    }
    return sample.redF() * 255.0 * 0.299 + sample.greenF() * 255.0 * 0.587 +
               sample.blueF() * 255.0 * 0.114 >
           192.0;
  };

  for (QWidget* itemsHost :
       presetsPanel_ ? presetsPanel_->findChildren<QWidget*>(
                           QString::fromLatin1(kPresetItemsObjectName), Qt::FindChildrenRecursively)
                     : QList<QWidget*>()) {
    reflowPresetItems(itemsHost, presetColumns, presetLayoutGap);
  }

  QList<PresetColorButton*> swatches;
  if (presetsPanel_) {
    for (QAbstractButton* button : presetsPanel_->findChildren<QAbstractButton*>(
             QString::fromLatin1(kPresetSwatchObjectName), Qt::FindChildrenRecursively)) {
      if (auto* swatch = dynamic_cast<PresetColorButton*>(button)) {
        swatches.append(swatch);
      }
    }
  }
  for (PresetColorButton* swatch : swatches) {
    if (!swatch) {
      continue;
    }
    swatch->setFixedSize(presetOuterSize, presetOuterSize);
    swatch->setOuterPadding(presetOutlinePadding);
    swatch->setFrameStyle(style.presetBorder, style.metrics.borderWidth,
                          style.metrics.swatchRadius);
    swatch->setCheckerColors(style.panelBackground, style.transparentCellB, kTransparencyCell);
    swatch->setHoverOutlineColor(hoverOutlineColor);
    swatch->setCheckmarkColors(checkmarkColor, brightCheckmarkColor);
    swatch->setBright(isPresetBright(swatch->colorValue()));
    swatch->setEnabled(!disabled());

    const ColorValue& value = swatch->colorValue();
    if (value.isGradient() && !value.gradientStops.isEmpty()) {
      QVector<QPair<qreal, QColor>> stops;
      stops.reserve(value.gradientStops.size());
      for (const GradientStop& stop : value.gradientStops) {
        if (!stop.color.isValid()) {
          continue;
        }
        stops.append(qMakePair(std::clamp(stop.percent / 100.0, 0.0, 1.0), stop.color));
      }
      if (stops.isEmpty()) {
        swatch->setSolidFill(style.invalidSwatchFill);
      } else {
        std::sort(stops.begin(), stops.end(),
                  [](const QPair<qreal, QColor>& lhs, const QPair<qreal, QColor>& rhs) {
                    return lhs.first < rhs.first;
                  });
        swatch->setGradientFill(stops);
      }
    } else {
      swatch->setSolidFill(value.solidColor.isValid() ? value.solidColor : style.invalidSwatchFill);
    }
  }

  refreshPresetSelectionState();

  refreshChannelVisuals();
  refreshPreviewSwatch();
  refreshTriggerDisplay();
  refreshPanelControlsFromState();
  updateTriggerFocusOverlay();
}

void AdColorPicker::refreshPresetSelectionState() {
  if (!presetsPanel_) {
    return;
  }

  const QString currentCss = colorValueToCss(exportColorValue());
  QList<PresetColorButton*> swatches;
  for (QAbstractButton* button : presetsPanel_->findChildren<QAbstractButton*>(
           QString::fromLatin1(kPresetSwatchObjectName), Qt::FindChildrenRecursively)) {
    if (auto* swatch = dynamic_cast<PresetColorButton*>(button)) {
      swatches.append(swatch);
    }
  }
  for (PresetColorButton* swatch : swatches) {
    if (!swatch) {
      continue;
    }
    swatch->setCheckedVisual(swatch->property("ad-color-picker-css").toString() == currentCss);
  }
}

void AdColorPicker::suppressTriggerUpdatesDuringInteraction() {
  if (triggerUpdatesSuppressed_) {
    return;
  }

  triggerUpdatesSuppressed_ = true;
}

void AdColorPicker::resumeTriggerUpdatesAfterInteraction() {
  if (!triggerUpdatesSuppressed_) {
    return;
  }

  triggerUpdatesSuppressed_ = false;
  update();
  if (triggerFrame_) {
    triggerFrame_->update();
  }
  if (triggerSwatch_) {
    triggerSwatch_->update();
  }
  if (triggerTextLabel_) {
    triggerTextLabel_->update();
  }
}

void AdColorPicker::syncTriggerWidthLock(bool preserveCurrentWidth) {
  const bool shouldLock =
      popupVisible() && triggerTextVisible_ && defaultTrigger_ && !triggerContent_;
  if (!shouldLock) {
    lockedTriggerWidth_ = 0;
    return;
  }

  const int sizeHintWidth = defaultTrigger_->sizeHint().width();
  const int minimumHintWidth = defaultTrigger_->minimumSizeHint().width();
  lockedTriggerWidth_ = std::max(0, std::max(sizeHintWidth, minimumHintWidth));
  if (preserveCurrentWidth) {
    lockedTriggerWidth_ = std::max(lockedTriggerWidth_, defaultTrigger_->width());
  }
}

void AdColorPicker::refreshTriggerWidthAfterDisplayChange() {
  if (!defaultTrigger_ || triggerContent_) {
    return;
  }

  if (triggerTextLabel_) {
    triggerTextLabel_->updateGeometry();
  }
  if (QLayout* triggerLayout = triggerFrame_ ? triggerFrame_->layout() : nullptr) {
    triggerLayout->invalidate();
    triggerLayout->activate();
  }
  defaultTrigger_->updateGeometry();

  if (popupVisible() && triggerTextVisible_) {
    refreshStyle(false);
    return;
  }

  if (QWidget* parent = defaultTrigger_->parentWidget()) {
    if (QLayout* parentLayout = parent->layout()) {
      parentLayout->invalidate();
      parentLayout->activate();
    }
  }
}

void AdColorPicker::refreshTriggerDisplay(bool deferTextUpdate) {
  if (triggerUpdatesSuppressed_) {
    return;
  }

  if (!triggerSwatch_) {
    return;
  }

  const detail::ColorPickerVisualStyle style = visualStyle();
  const int swatchRadius = swatchRadiusForSize(size_, style);
  triggerSwatch_->setProperty("ad-color-picker-swatch-radius", swatchRadius);

  if (auto* swatch = dynamic_cast<ColorPickerSwatch*>(triggerSwatch_.data())) {
    swatch->setFrameStyle(style.swatchBorder, style.metrics.borderWidth, swatchRadius);
    const QColor checkerLight =
        disabled() ? style.triggerBackgroundDisabled : style.triggerBackground;
    swatch->setCheckerColors(checkerLight, style.transparentCellB, kTransparencyCell);
    if (cleared_) {
      swatch->setClearedTriggerFill();
    } else if (mode_ == Mode::Gradient && !gradientStops_.isEmpty()) {
      QVector<QPair<qreal, QColor>> stops;
      const QVector<InternalGradientStop> normalized = normalizeGradientStops(gradientStops_);
      stops.reserve(normalized.size());
      for (const InternalGradientStop& stop : normalized) {
        const qreal stopPos =
            static_cast<qreal>(std::clamp(static_cast<double>(stop.percent) / 100.0, 0.0, 1.0));
        stops.append(qMakePair(stopPos, stop.color));
      }
      swatch->setGradientFill(stops);
    } else {
      swatch->setSolidFill(solidColor_);
    }
  }
  if (triggerTextLabel_) {
    if (!triggerTextVisible_) {
      triggerTextLabel_->clear();
      return;
    }

    // During high-frequency drag updates, defer trigger text mutation to the
    // completed step to avoid per-move relayout/paint churn.
    if (deferTextUpdate) {
      return;
    }

    const ColorValue displayValue = exportColorValue();
    QString text;
    bool useRichText = false;
    if (showTextFormatter_) {
      text = showTextFormatter_(toQtColorValue(displayValue), format_, activeStopIndex_);
    }

    if (text.trimmed().isEmpty()) {
      if (displayValue.isEmpty()) {
        text = tr("Transparent");
      } else if (mode_ == Mode::Gradient && !displayValue.gradientStops.isEmpty()) {
        QStringList cells;
        const QVector<InternalGradientStop> normalized = normalizeGradientStops(gradientStops_);
        cells.reserve(normalized.size());
        // Ant Design only dims inactive gradient stops while the popup is open.
        const bool markInactive =
            popupVisible() && activeStopIndex_ >= 0 && activeStopIndex_ < normalized.size();
        if (markInactive) {
          useRichText = true;
          for (int i = 0; i < normalized.size(); ++i) {
            const InternalGradientStop& stop = normalized.at(i);
            const QString cellText =
                QStringLiteral("%1 %2%").arg(formattedColorString(stop.color)).arg(stop.percent);
            const QColor cellColor =
                (i == activeStopIndex_) ? style.triggerText : style.triggerTextDisabled;
            cells.append(QStringLiteral("<span style=\"color:%1;\">%2</span>")
                             .arg(cellColor.name(QColor::HexArgb))
                             .arg(cellText.toHtmlEscaped()));
          }
        } else {
          for (const InternalGradientStop& stop : normalized) {
            cells.append(
                QStringLiteral("%1 %2%").arg(formattedColorString(stop.color)).arg(stop.percent));
          }
        }
        text = cells.join(QStringLiteral(", "));
      } else {
        const QColor current = currentEditableColor();
        switch (format_) {
          case Format::Hex:
            text = colorToTriggerHexText(current);
            break;
          case Format::Rgb:
            text = colorToRgbCssCompact(current);
            break;
          case Format::Hsb:
            text = colorToString(current, Format::Hsb);
            break;
        }
      }
    }
    Qt::TextFormat textFormat = useRichText ? Qt::RichText : Qt::AutoText;
    bool displayChanged = false;

    if (triggerTextLabel_->textFormat() != textFormat) {
      triggerTextLabel_->setTextFormat(textFormat);
      displayChanged = true;
    }
    if (triggerTextLabel_->text() != text) {
      triggerTextLabel_->setText(text);
      displayChanged = true;
    }
    if (displayChanged) {
      refreshTriggerWidthAfterDisplayChange();
    }
  }
}

void AdColorPicker::refreshPanelControlsFromState(bool minimal) {
  if (!pickerPanel_) {
    return;
  }

  if (mode_ == Mode::Gradient) {
    ensureGradientUi();
  }

  QScopedValueRollback<bool> guard(syncingControls_, true);

  if (minimal) {
    const QColor color = currentEditableColor().toHsv();
    int hue = color.hue();
    if (hue < 0) {
      hue = 0;
    }
    const int sat = qRound(color.saturationF() * 100.0);
    const int bri = qRound(color.valueF() * 100.0);
    const int alpha = qRound(color.alphaF() * 100.0);

    const bool fromSaturation = livePanelSyncSource_ == LivePanelSyncSource::SaturationPanel;
    const bool fromHue = livePanelSyncSource_ == LivePanelSyncSource::HueSlider;
    const bool fromAlpha = livePanelSyncSource_ == LivePanelSyncSource::AlphaSlider;
    const bool fromChannel = fromSaturation || fromHue || fromAlpha;

    if (!fromChannel) {
      if (hueSlider_) {
        hueSlider_->setValue(hue);
      }
      if (saturationPanel_) {
        saturationPanel_->setHue(hue);
        saturationPanel_->setSaturationBrightness(sat / 100.0, bri / 100.0);
      }
      if (alphaSlider_) {
        alphaSlider_->setValue(alpha);
      }
    } else if (fromHue && saturationPanel_) {
      saturationPanel_->setHue(hue);
    }

    refreshChannelVisuals(livePanelSyncSource_);
    refreshPreviewSwatch();
    scheduleInteractiveEditorRefresh();
    return;
  }

  interactiveEditorRefreshPending_ = false;

  const QVector<Mode> normalizedModes = normalizeModeOptions(modeOptions_);
  const bool showModeSwitch = normalizedModes.size() > 1;
  const bool showOperationRow = showModeSwitch || allowClear_;
  if (showOperationRow) {
    ensureOperationUi();
  }
  if (allowClear_) {
    ensureClearButtonUi();
  }

  updateModeSegmentedOptions();
  if (modeButtonGroup_) {
    const QString currentMode = modeName(mode_);
    const QList<QAbstractButton*> buttons = modeButtonGroup_->buttons();
    for (QAbstractButton* button : buttons) {
      if (!button) {
        continue;
      }
      const bool shouldCheck =
          button->property("ad-color-picker-mode-value").toString() == currentMode;
      if (button->isChecked() != shouldCheck) {
        button->setChecked(shouldCheck);
      }
    }
  }

  const QVector<InternalGradientStop> normalized = normalizeGradientStops(gradientStops_);
  if (gradientSlider_) {
    if (mode_ == Mode::Gradient) {
      QList<double> values;
      values.reserve(normalized.size());
      for (const InternalGradientStop& stop : normalized) {
        values.append(stop.percent);
      }
      const QSignalBlocker blocker(gradientSlider_);
      gradientSlider_->setHandleValues(values);
    }
    gradientSlider_->setEnabled(mode_ == Mode::Gradient && !disabled());
  }

  const QColor color = currentEditableColor().toHsv();
  int hue = color.hue();
  if (hue < 0) {
    hue = 0;
  }
  const int sat = qRound(color.saturationF() * 100.0);
  const int bri = qRound(color.valueF() * 100.0);
  const int alpha = qRound(color.alphaF() * 100.0);

  if (hueSlider_) {
    hueSlider_->setValue(hue);
    hueSlider_->setEnabled(!disabled());
  }
  if (saturationPanel_) {
    saturationPanel_->setHue(hue);
    saturationPanel_->setSaturationBrightness(sat / 100.0, bri / 100.0);
    saturationPanel_->setEnabled(!disabled());
  }
  if (alphaSlider_) {
    alphaSlider_->setValue(alpha);
    alphaSlider_->setEnabled(!disabledAlpha_ && !disabled());
  }

  bool needsLayoutRefresh = false;
  if (operationRow_) {
    const bool nextVisible = showOperationRow;
    needsLayoutRefresh = needsLayoutRefresh || operationRow_->isVisible() != nextVisible;
    operationRow_->setVisible(nextVisible);
  }
  if (modeSegmented_) {
    const bool nextVisible = showModeSwitch;
    needsLayoutRefresh = needsLayoutRefresh || modeSegmented_->isVisible() != nextVisible;
    modeSegmented_->setVisible(nextVisible);
    modeSegmented_->setDisabled(!(showModeSwitch && !disabled()));
  }
  if (gradientSection_) {
    const bool nextVisible = mode_ == Mode::Gradient;
    needsLayoutRefresh = needsLayoutRefresh || gradientSection_->isVisible() != nextVisible;
    gradientSection_->setVisible(nextVisible);
  }
  if (alphaSection_) {
    const bool nextVisible = !disabledAlpha_;
    needsLayoutRefresh = needsLayoutRefresh || alphaSection_->isVisible() != nextVisible;
    alphaSection_->setVisible(nextVisible);
  }
  if (formatCombo_) {
    const QString currentFormat = formatName(format_);
    if (formatCombo_->currentData().toString() != currentFormat) {
      formatCombo_->setCurrentData(currentFormat);
    }
    formatCombo_->setDisabled(disabledFormat_ || disabled());
    const bool nextVisible = !disabledFormat_;
    needsLayoutRefresh = needsLayoutRefresh || formatCombo_->isVisible() != nextVisible;
    formatCombo_->setVisible(nextVisible);
  }
  if (alphaInput_) {
    setInputNumberValueIfChanged(alphaInput_, alpha);
    alphaInput_->setEnabled(!(disabled() || disabledAlpha_));
    const bool nextVisible = !disabledAlpha_;
    needsLayoutRefresh = needsLayoutRefresh || alphaInput_->isVisible() != nextVisible;
    alphaInput_->setVisible(nextVisible);
  }

  auto syncGapVisibility = [](QWidget* gap, bool sectionVisible) {
    if (!gap) {
      return;
    }
    const int gapHeight = std::max(gap->minimumHeight(), gap->maximumHeight());
    gap->setVisible(sectionVisible && gapHeight > 0);
  };
  syncGapVisibility(operationGap_, showOperationRow);
  syncGapVisibility(gradientGap_, gradientSection_ && mode_ == Mode::Gradient);
  syncGapVisibility(saturationGap_, saturationPanel_);
  syncGapVisibility(sliderGap_, sliderContainer_);

  if (needsLayoutRefresh) {
    if (pickerPanel_) {
      if (QLayout* layout = pickerPanel_->layout()) {
        layout->invalidate();
        layout->activate();
      }
      pickerPanel_->updateGeometry();
    }
    if (panelHost_) {
      panelHost_->updateGeometry();
    }
  }

  refreshChannelVisuals();
  refreshPreviewSwatch();
  updateFormatInputText();
  refreshPresetSelectionState();
}

void AdColorPicker::refreshPanelControlsForOpenInteraction(bool commit) {
  QScopedValueRollback<bool> guard(syncingControls_, true);
  refreshPanelControlsFromState(true);
  if (!commit) {
    return;
  }

  const QColor color = currentEditableColor().toHsv();
  const int alpha = std::clamp(qRound(color.alphaF() * 100.0), 0, 100);
  if (alphaInput_) {
    setInputNumberValueIfChanged(alphaInput_, alpha);
    alphaInput_->setEnabled(!(disabled() || disabledAlpha_));
  }

  updateFormatInputText();
  refreshPresetSelectionState();
}

void AdColorPicker::refreshInteractiveEditorsFromState() {
  const QColor color = currentEditableColor().toHsv();
  const int alpha = std::clamp(qRound(color.alphaF() * 100.0), 0, 100);

  if (alphaInput_ && livePanelSyncSource_ != LivePanelSyncSource::AlphaInput) {
    setInputNumberValueIfChanged(alphaInput_, alpha);
    alphaInput_->setEnabled(!(disabled() || disabledAlpha_));
  }

  if (livePanelSyncSource_ == LivePanelSyncSource::FormatInputs) {
    return;
  }

  if (format_ == Format::Hex) {
    if (!hexInput_) {
      return;
    }
    QString text = colorToString(color, Format::Hex).toUpper();
    if (text.startsWith(QLatin1Char('#'))) {
      text.remove(0, 1);
    }
    hexInput_->setText(text);
    return;
  }

  if (format_ == Format::Rgb) {
    if (rgbInputR_) {
      setInputNumberValueIfChanged(rgbInputR_, color.red());
    }
    if (rgbInputG_) {
      setInputNumberValueIfChanged(rgbInputG_, color.green());
    }
    if (rgbInputB_) {
      setInputNumberValueIfChanged(rgbInputB_, color.blue());
    }
    return;
  }

  int hue = color.hsvHue();
  if (hue < 0) {
    hue = 0;
  }
  const int sat = std::clamp(qRound(color.saturationF() * 100.0), 0, 100);
  const int bri = std::clamp(qRound(color.valueF() * 100.0), 0, 100);
  if (hsbInputH_) {
    setInputNumberValueIfChanged(hsbInputH_, hue);
  }
  if (hsbInputS_) {
    setInputNumberValueIfChanged(hsbInputS_, sat);
  }
  if (hsbInputB_) {
    setInputNumberValueIfChanged(hsbInputB_, bri);
  }
}

void AdColorPicker::scheduleInteractiveEditorRefresh() {
  if (interactiveEditorRefreshPending_) {
    return;
  }

  interactiveEditorRefreshPending_ = true;
  QTimer::singleShot(kInteractiveEditorRefreshIntervalMs, this, [this]() {
    if (!interactiveEditorRefreshPending_) {
      return;
    }
    interactiveEditorRefreshPending_ = false;
    if (!pickerPanel_) {
      return;
    }

    QScopedValueRollback<bool> guard(syncingControls_, true);
    refreshInteractiveEditorsFromState();
  });
}

void AdColorPicker::updateModeSegmentedOptions() {
  const QVector<Mode> normalized = normalizeModeOptions(modeOptions_);
  if (normalized.size() > 1 && pickerPanel_) {
    ensureOperationUi();
  }
  if (!modeSegmented_ || !modeButtonGroup_) {
    return;
  }
  if (!modeListContains(normalized, mode_)) {
    mode_ = normalized.constFirst();
    emit modeChanged(mode_);
  }

  auto* modeLayout = qobject_cast<QHBoxLayout*>(modeSegmented_->layout());
  if (!modeLayout) {
    return;
  }

  QStringList expectedValues;
  expectedValues.reserve(normalized.size());
  for (Mode value : normalized) {
    expectedValues.append(modeName(value));
  }

  QStringList existingValues;
  const QList<QPushButton*> existingButtons =
      modeSegmented_->findChildren<QPushButton*>(QString(), Qt::FindDirectChildrenOnly);
  existingValues.reserve(existingButtons.size());
  for (QPushButton* button : existingButtons) {
    if (!button) {
      continue;
    }
    existingValues.append(button->property("ad-color-picker-mode-value").toString());
  }

  if (existingValues != expectedValues) {
    while (QLayoutItem* item = modeLayout->takeAt(0)) {
      QWidget* widget = item->widget();
      if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        modeButtonGroup_->removeButton(button);
      }
      delete widget;
      delete item;
    }

    for (Mode value : normalized) {
      const QString modeValue = modeName(value);
      auto* button = new ColorPickerSegmentedButton(modeSegmented_);
      button->setText(modeLabel(value));
      button->setCheckable(true);
      button->setCursor(Qt::PointingHandCursor);
      button->setFocusPolicy(Qt::NoFocus);
      button->setProperty("ad-color-picker-mode-value", modeValue);
      modeButtonGroup_->addButton(button);
      modeLayout->addWidget(button);
    }
  }

  const bool canInteract = normalized.size() > 1 && !disabled();
  const QString currentMode = modeName(mode_);
  const QList<QAbstractButton*> buttons = modeButtonGroup_->buttons();
  for (QAbstractButton* button : buttons) {
    if (!button) {
      continue;
    }
    const Mode buttonMode =
        parseModeName(button->property("ad-color-picker-mode-value").toString(), Mode::Solid);
    if (auto* push = qobject_cast<QPushButton*>(button)) {
      const QString label = modeLabel(buttonMode);
      if (push->text() != label) {
        push->setText(label);
      }
    }
    button->setEnabled(canInteract);
    const bool shouldCheck =
        button->property("ad-color-picker-mode-value").toString() == currentMode;
    if (button->isChecked() != shouldCheck) {
      button->setChecked(shouldCheck);
    }
  }

  const detail::ColorPickerVisualStyle style = visualStyle();
  const adqt::theme::ThemeMapToken mapToken =
      adqt::theme::ThemeManager::instance().resolve(this, themeLogicalOwner()).values;
  applyModeSegmentedStyle(modeSegmented_, style, mapToken);
}

void AdColorPicker::refreshChannelVisuals(LivePanelSyncSource source) {
  const detail::ColorPickerVisualStyle style = visualStyle();
  const QColor editableColor = currentEditableColor();
  const bool refreshHue = source != LivePanelSyncSource::SaturationPanel &&
                          source != LivePanelSyncSource::AlphaSlider &&
                          source != LivePanelSyncSource::AlphaInput;
  const bool refreshAlpha =
      source != LivePanelSyncSource::AlphaSlider && source != LivePanelSyncSource::AlphaInput;

  if (hueSlider_ && refreshHue) {
    AdSliderSemanticStyles hueStyles;
    hueStyles.rail.brush = makeHueBrush();
    hueStyles.handle.borderColor = style.channelHandleBorder;
    int hue = editableColor.hsvHue();
    if (hue < 0) {
      hue = 0;
    }
    hueStyles.handle.backgroundColor = QColor::fromHsv(hue, 255, 255);
    hueSlider_->setSemanticStyles(hueStyles);
  }

  if (alphaSlider_ && refreshAlpha) {
    AdSliderSemanticStyles alphaStyles;
    alphaStyles.rail.brush = makeCheckerBrush(kTransparencyCell);
    alphaStyles.tracks.brush = makeAlphaBrush(editableColor);
    alphaStyles.handle.borderColor = style.channelHandleBorder;
    QColor alphaHandleColor = editableColor.toRgb();
    alphaHandleColor.setAlpha(255);
    alphaStyles.handle.backgroundColor = alphaHandleColor;
    alphaSlider_->setSemanticStyles(alphaStyles);
  }

  // The gradient slider remains allocated after switching back to solid mode,
  // but its rail is hidden there. Avoid rebuilding an unused gradient on each
  // solid-color interaction; it is refreshed when gradient mode is re-entered.
  if (gradientSlider_ && mode_ == Mode::Gradient) {
    const QVector<InternalGradientStop> normalized = normalizeGradientStops(gradientStops_);
    QLinearGradient gradient(0.0, 0.0, 1.0, 0.0);
    gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
    for (const InternalGradientStop& stop : normalized) {
      const double stopPos = std::clamp(static_cast<double>(stop.percent) / 100.0, 0.0, 1.0);
      gradient.setColorAt(stopPos, stop.color);
    }

    AdSliderSemanticStyles gradientStyles;
    gradientStyles.rail.brush = QBrush(gradient);
    gradientStyles.handle.borderColor = style.channelHandleBorder;
    gradientStyles.handle.backgroundColor = QColor(0, 0, 0, 0);
    gradientSlider_->setSemanticStyles(gradientStyles);
  }
}

void AdColorPicker::refreshPreviewSwatch() {
  if (!previewSwatch_) {
    return;
  }

  const detail::ColorPickerVisualStyle style = visualStyle();

  const int radius = style.metrics.previewSwatchRadius;
  previewSwatch_->setProperty("ad-color-picker-swatch-radius", radius);
  if (auto* swatch = dynamic_cast<ColorPickerSwatch*>(previewSwatch_.data())) {
    swatch->setFrameStyle(style.swatchBorder, style.metrics.borderWidth, radius, cleared_);
    swatch->setCheckerColors(style.panelBackground, style.transparentCellB, kTransparencyCell);
    if (cleared_) {
      swatch->setClearedPreviewFill();
    } else {
      swatch->setSolidFill(currentEditableColor());
    }
  }
}

void AdColorPicker::syncPreviewContentWidget() {
  if (!sliderContainer_ || !previewSwatch_) {
    return;
  }
  auto* layout = qobject_cast<QHBoxLayout*>(sliderContainer_->layout());
  if (layout == nullptr) {
    return;
  }

  layout->removeWidget(previewSwatch_);
  previewSwatch_->hide();
  if (!previewContent_) {
    layout->addWidget(previewSwatch_, 0, Qt::AlignCenter);
    previewSwatch_->show();
    return;
  }

  if (previewContent_->parentWidget() != sliderContainer_) {
    previewContent_->setParent(sliderContainer_);
  }
  layout->removeWidget(previewContent_);
  layout->addWidget(previewContent_, 0, Qt::AlignCenter);
  previewContent_->show();
}

void AdColorPicker::updateFormatInputText() {
  const QColor color = currentEditableColor();
  if (!color.isValid()) {
    return;
  }

  if (format_ == Format::Hex) {
    if (!hexInput_) {
      return;
    }
    QString text = colorToString(color, Format::Hex).toUpper();
    if (text.startsWith(QLatin1Char('#'))) {
      text.remove(0, 1);
    }
    hexInput_->setText(text);
    return;
  }

  if (format_ == Format::Rgb) {
    if (rgbInputR_) {
      setInputNumberValueIfChanged(rgbInputR_, color.red());
    }
    if (rgbInputG_) {
      setInputNumberValueIfChanged(rgbInputG_, color.green());
    }
    if (rgbInputB_) {
      setInputNumberValueIfChanged(rgbInputB_, color.blue());
    }
    return;
  }

  int hue = color.hsvHue();
  if (hue < 0) {
    hue = 0;
  }
  const int sat = std::clamp(qRound(color.saturationF() * 100.0), 0, 100);
  const int bri = std::clamp(qRound(color.valueF() * 100.0), 0, 100);
  if (hsbInputH_) {
    setInputNumberValueIfChanged(hsbInputH_, hue);
  }
  if (hsbInputS_) {
    setInputNumberValueIfChanged(hsbInputS_, sat);
  }
  if (hsbInputB_) {
    setInputNumberValueIfChanged(hsbInputB_, bri);
  }
}

void AdColorPicker::updateFormatInputVisibility() {
  ensureFormatInputUi(format_);

  if (!formatInputStack_) {
    return;
  }

  QWidget* target = nullptr;
  switch (format_) {
    case Format::Hex:
      target = hexInput_;
      break;
    case Format::Rgb:
      target = rgbInputHost_;
      break;
    case Format::Hsb:
      target = hsbInputHost_;
      break;
  }

  if (!target) {
    target = hexInput_;
  }
  if (target) {
    formatInputStack_->setCurrentWidget(target);
  }
}

QColor AdColorPicker::currentEditableColor() const {
  if (mode_ == Mode::Gradient && !gradientStops_.isEmpty()) {
    const int maxIndex = std::max(0, static_cast<int>(gradientStops_.size()) - 1);
    const int index = std::clamp(activeStopIndex_, 0, maxIndex);
    return gradientStops_.at(index).color;
  }
  return solidColor_;
}

void AdColorPicker::setCurrentEditableColor(const QColor& color, bool fromUser, bool emitCompleted,
                                            bool emitCssTextSignal) {
  if (!color.isValid()) {
    return;
  }

  const bool clearedChanged = cleared_;
  cleared_ = false;
  if (mode_ == Mode::Gradient) {
    if (gradientStops_.isEmpty()) {
      gradientStops_ = {
          InternalGradientStop{color, 0},
          InternalGradientStop{color, 100},
      };
      activeStopIndex_ = 0;
    }
    const int maxIndex = std::max(0, static_cast<int>(gradientStops_.size()) - 1);
    activeStopIndex_ = std::clamp(activeStopIndex_, 0, maxIndex);
    gradientStops_[activeStopIndex_].color = color;
    solidColor_ = color;
  } else {
    solidColor_ = color;
  }

  if (clearedChanged) {
    invalidateStyleCache();
  }
  const bool deferTriggerRefresh = fromUser && !emitCompleted && popupVisible();
  refreshPanelControlsFromState(deferTriggerRefresh);
  if (deferTriggerRefresh) {
    suppressTriggerUpdatesDuringInteraction();
  } else {
    resumeTriggerUpdatesAfterInteraction();
    refreshTriggerDisplay();
  }
  emitChangeSignals(emitCompleted, emitCssTextSignal);
}

void AdColorPicker::setCurrentFromControls(bool emitCompleted, LivePanelSyncSource source) {
  if (syncingControls_ || !hueSlider_ || !saturationPanel_) {
    return;
  }

  QScopedValueRollback<LivePanelSyncSource> sourceGuard(livePanelSyncSource_, source);

  const int hue = std::clamp(qRound(hueSlider_->value()), 0, 359);
  const int sat = std::clamp(qRound(saturationPanel_->saturation() * 255.0), 0, 255);
  const int bri = std::clamp(qRound(saturationPanel_->brightness() * 255.0), 0, 255);
  const int alpha =
      alphaSlider_ ? std::clamp(qRound(alphaSlider_->value() * 255.0 / 100.0), 0, 255) : 255;

  QColor color = QColor::fromHsv(hue, sat, bri, alpha);
  setCurrentEditableColor(color, true, emitCompleted, true);
}

void AdColorPicker::setGradientStopsFromSlider(const QList<double>& values, bool emitCompleted) {
  if (syncingControls_ || values.isEmpty()) {
    return;
  }

  QScopedValueRollback<LivePanelSyncSource> sourceGuard(livePanelSyncSource_,
                                                        LivePanelSyncSource::GradientStops);

  QVector<int> percents;
  percents.reserve(values.size());
  for (double value : values) {
    percents.append(std::clamp(qRound(value), 0, 100));
  }
  std::sort(percents.begin(), percents.end());

  const QVector<InternalGradientStop> current = normalizeGradientStops(gradientStops_);
  auto mixColor = [](const QColor& lhs, const QColor& rhs, double ratio) {
    const double t = std::clamp(ratio, 0.0, 1.0);
    return QColor(std::clamp(qRound(lhs.red() + (rhs.red() - lhs.red()) * t), 0, 255),
                  std::clamp(qRound(lhs.green() + (rhs.green() - lhs.green()) * t), 0, 255),
                  std::clamp(qRound(lhs.blue() + (rhs.blue() - lhs.blue()) * t), 0, 255),
                  std::clamp(qRound(lhs.alpha() + (rhs.alpha() - lhs.alpha()) * t), 0, 255));
  };
  auto sampleColorAtPercent = [&](int percent) -> QColor {
    if (current.isEmpty()) {
      return solidColor_.isValid() ? solidColor_ : QColor("#1677ff");
    }
    const int target = std::clamp(percent, 0, 100);
    if (target <= current.constFirst().percent) {
      return current.constFirst().color;
    }
    if (target >= current.constLast().percent) {
      return current.constLast().color;
    }
    for (int i = 0; i + 1 < current.size(); ++i) {
      const InternalGradientStop& lhs = current.at(i);
      const InternalGradientStop& rhs = current.at(i + 1);
      if (target < lhs.percent || target > rhs.percent) {
        continue;
      }
      const int span = rhs.percent - lhs.percent;
      if (span <= 0) {
        return rhs.color;
      }
      return mixColor(lhs.color, rhs.color, static_cast<double>(target - lhs.percent) / span);
    }
    return current.constLast().color;
  };
  QVector<int> oldToNew(current.size(), -1);
  QVector<int> newToOld(percents.size(), -1);
  QVector<bool> oldUsed(current.size(), false);

  for (int newIndex = 0; newIndex < percents.size(); ++newIndex) {
    const int percent = percents.at(newIndex);
    for (int oldIndex = 0; oldIndex < current.size(); ++oldIndex) {
      if (oldUsed.at(oldIndex) || current.at(oldIndex).percent != percent) {
        continue;
      }
      oldUsed[oldIndex] = true;
      oldToNew[oldIndex] = newIndex;
      newToOld[newIndex] = oldIndex;
      break;
    }
  }

  QVector<int> unmatchedOld;
  unmatchedOld.reserve(current.size());
  for (int oldIndex = 0; oldIndex < current.size(); ++oldIndex) {
    if (oldToNew.at(oldIndex) < 0) {
      unmatchedOld.append(oldIndex);
    }
  }

  QVector<int> unmatchedNew;
  unmatchedNew.reserve(percents.size());
  for (int newIndex = 0; newIndex < percents.size(); ++newIndex) {
    if (newToOld.at(newIndex) < 0) {
      unmatchedNew.append(newIndex);
    }
  }

  int addedIndex = -1;
  int removedOldIndex = -1;
  if (unmatchedOld.size() == 1 && unmatchedNew.size() == 1) {
    // Drag move: preserve the moved stop color by index rather than by position.
    const int oldIndex = unmatchedOld.constFirst();
    const int newIndex = unmatchedNew.constFirst();
    oldToNew[oldIndex] = newIndex;
    newToOld[newIndex] = oldIndex;
  } else if (unmatchedOld.isEmpty() && unmatchedNew.size() == 1) {
    // Add: the new stop should use the interpolated color at insertion percent.
    addedIndex = unmatchedNew.constFirst();
  } else if (unmatchedOld.size() == 1 && unmatchedNew.isEmpty()) {
    // Delete: remaining stops keep their colors; only the removed index disappears.
    removedOldIndex = unmatchedOld.constFirst();
  } else {
    // Fallback for non-standard transitions: pair remaining indexes in order.
    const qsizetype pairCount = std::min(unmatchedOld.size(), unmatchedNew.size());
    for (qsizetype i = 0; i < pairCount; ++i) {
      const int oldIndex = unmatchedOld.at(i);
      const int newIndex = unmatchedNew.at(i);
      oldToNew[oldIndex] = newIndex;
      newToOld[newIndex] = oldIndex;
    }
  }

  QVector<InternalGradientStop> next;
  next.reserve(percents.size());
  const QColor fallbackColor = solidColor_.isValid() ? solidColor_ : QColor("#1677ff");
  for (int newIndex = 0; newIndex < percents.size(); ++newIndex) {
    InternalGradientStop stop;
    stop.percent = percents.at(newIndex);
    const int mappedOldIndex = newToOld.at(newIndex);
    if (mappedOldIndex >= 0 && mappedOldIndex < current.size()) {
      stop.color = current.at(mappedOldIndex).color;
    } else {
      stop.color = sampleColorAtPercent(stop.percent);
    }
    if (!stop.color.isValid()) {
      stop.color = fallbackColor;
    }
    next.append(stop);
  }

  gradientStops_ = normalizeGradientStops(next);

  int nextActiveIndex = activeStopIndex_;
  if (addedIndex >= 0) {
    nextActiveIndex = addedIndex;
  } else if (activeStopIndex_ >= 0 && activeStopIndex_ < oldToNew.size() &&
             oldToNew.at(activeStopIndex_) >= 0) {
    nextActiveIndex = oldToNew.at(activeStopIndex_);
  } else if (removedOldIndex >= 0 && activeStopIndex_ >= 0) {
    if (activeStopIndex_ > removedOldIndex) {
      nextActiveIndex = activeStopIndex_ - 1;
    } else if (activeStopIndex_ == removedOldIndex) {
      nextActiveIndex = removedOldIndex;
    }
  }
  if (!gradientStops_.isEmpty()) {
    nextActiveIndex = std::clamp(nextActiveIndex, 0, static_cast<int>(gradientStops_.size()) - 1);
  } else {
    nextActiveIndex = 0;
  }
  activeStopIndex_ = nextActiveIndex;
  if (!gradientStops_.isEmpty()) {
    solidColor_ = gradientStops_.at(activeStopIndex_).color;
  }

  const bool clearedChanged = cleared_;
  cleared_ = false;
  if (clearedChanged) {
    invalidateStyleCache();
  }
  const bool deferTriggerRefresh = !emitCompleted && popupVisible();
  refreshPanelControlsFromState(deferTriggerRefresh);
  if (deferTriggerRefresh) {
    suppressTriggerUpdatesDuringInteraction();
  } else {
    resumeTriggerUpdatesAfterInteraction();
    refreshTriggerDisplay();
  }
  emitChangeSignals(emitCompleted, true);
}

QColor AdColorPicker::parseColorString(const QString& value, bool* ok) const {
  return detail::ColorPickerValueModel::parseCssColor(value, ok);
}

QString AdColorPicker::colorToString(const QColor& color, Format format) const {
  return detail::ColorPickerValueModel::colorToString(color, format);
}

QString AdColorPicker::formattedColorString(const QColor& color) const {
  return detail::ColorPickerValueModel::formattedColorString(color, format_);
}

QString AdColorPicker::colorToCss(const QColor& color) const {
  return detail::ColorPickerValueModel::colorToCss(color);
}

QString AdColorPicker::colorValueToCss(const ColorValue& value) const {
  return detail::ColorPickerValueModel::cssValue(value);
}

AdColorPicker::SelectionState AdColorPicker::createSelectionState(const AdColorSelection& selection,
                                                                  Mode mode,
                                                                  const QVector<Mode>& modeOptions,
                                                                  int activeStopIndex) {
  SelectionState state;
  state.selection = selection;
  state.mode = mode;
  state.modeOptions = modeOptions;
  state.activeStopIndex = activeStopIndex;
  return state;
}

AdColorPicker::SelectionState AdColorPicker::selectionState() const {
  return createSelectionState(exportColorValue(), mode_, modeOptions_, activeStopIndex_);
}

void AdColorPicker::applySelectionState(const SelectionState& state) {
  mode_ = state.mode;
  modeOptions_ = state.modeOptions;
  activeStopIndex_ = state.activeStopIndex;
  cleared_ = state.selection.isEmpty();

  if (state.selection.isGradient() && state.mode == Mode::Gradient) {
    gradientStops_.clear();
    gradientStops_.reserve(state.selection.gradientStops.size());
    for (const GradientStop& stop : state.selection.gradientStops) {
      gradientStops_.append(InternalGradientStop{stop.color, stop.percent});
    }
    solidColor_ =
        state.selection.solidColor.isValid() ? state.selection.solidColor : currentEditableColor();
    return;
  }

  solidColor_ =
      state.selection.solidColor.isValid() ? state.selection.solidColor : QColor(0, 0, 0, 0);
  gradientStops_.clear();
}

ColorValue AdColorPicker::exportColorValue() const {
  if (cleared_) {
    return ColorValue::empty(solidColor_);
  }

  if (mode_ == Mode::Gradient && !gradientStops_.isEmpty()) {
    ColorValue out = ColorValue::gradient({});
    out.solidColor = solidColor_;
    out.gradientStops.reserve(gradientStops_.size());
    for (const InternalGradientStop& stop : normalizeGradientStops(gradientStops_)) {
      out.gradientStops.append(GradientStop{stop.color, stop.percent});
    }
    return out;
  }

  return ColorValue::solid(solidColor_);
}

void AdColorPicker::importColorValue(const ColorValue& value, bool fromUser, bool emitCompleted,
                                     bool emitCssTextSignal) {
  Q_UNUSED(fromUser)

  const ColorValue previousValue = exportColorValue();
  const bool wasCleared = cleared_;
  const Mode previousMode = mode_;
  const auto nextState = detail::ColorPickerValueModel::stateFromSelection(value, modeOptions_,
                                                                           mode_, activeStopIndex_);

  applySelectionState(createSelectionState(nextState.selection, nextState.mode,
                                           nextState.modeOptions, nextState.activeStopIndex));

  const bool selectionChanged = previousValue != exportColorValue();
  const bool modeChangedByModel = previousMode != mode_;
  if (!selectionChanged && !modeChangedByModel && wasCleared == cleared_ && !emitCssTextSignal &&
      !emitCompleted) {
    return;
  }

  invalidateStyleCache();
  if (modeChangedByModel) {
    emit modeChanged(mode_);
  }
  refreshPanelControlsFromState();
  refreshTriggerDisplay();
  emitChangeSignals(emitCompleted, emitCssTextSignal);
  if (!wasCleared && cleared_) {
    emit cleared();
  }
}

QVector<AdColorPicker::InternalGradientStop> AdColorPicker::normalizeGradientStops(
    const QVector<InternalGradientStop>& stops) const {
  QVector<InternalGradientStop> normalized;
  normalized.reserve(stops.size());
  for (const InternalGradientStop& stop : stops) {
    if (!stop.color.isValid()) {
      continue;
    }
    normalized.append(InternalGradientStop{stop.color, std::clamp(stop.percent, 0, 100)});
  }

  std::stable_sort(normalized.begin(), normalized.end(),
                   [](const InternalGradientStop& lhs, const InternalGradientStop& rhs) {
                     return lhs.percent < rhs.percent;
                   });

  if (normalized.isEmpty()) {
    normalized = {
        InternalGradientStop{solidColor_.isValid() ? solidColor_ : QColor("#1677ff"), 0},
        InternalGradientStop{solidColor_.isValid() ? solidColor_ : QColor("#1677ff"), 100},
    };
  }

  return normalized;
}

void AdColorPicker::emitChangeSignals(bool emitCompleted, bool emitCssTextSignal) {
  const ColorValue exported = exportColorValue();
  const QString css = colorValueToCss(exported);
  const QString formatted = displayText();

  syncStateObject(exported, css, formatted);
  emit valueChanged(toQtColorValue(exported));
  if (emitCssTextSignal) {
    emit cssTextChanged(css);
  }
  emit displayTextChanged(formatted);
  if (emitCompleted) {
    if (popupVisible()) {
      pendingFinishedValue_ = exported;
      pendingEditingFinished_ = true;
    } else {
      emit editingFinished(toQtColorValue(exported));
    }
  }
}

void AdColorPicker::applyPreset(const AdColorValue& value) {
  importColorValue(toLegacyColorValue(value), true, true, true);
}

void AdColorPicker::applyStateObject() {
  if (!state_ || syncingStateObject_) {
    return;
  }

  QScopedValueRollback<bool> guard(applyingStateObject_, true);

  const detail::ColorPickerValueModel::State previousState =
      createValueModelState(exportColorValue(), mode_, modeOptions_, activeStopIndex_);

  detail::ColorPickerValueModel::State nextState =
      createValueModelState(toLegacyColorValue(state_->value()), state_->mode(),
                            state_->modeOptions(), state_->activeStopIndex());
  nextState = detail::ColorPickerValueModel::normalizedState(nextState);

  const Format nextFormat = state_->format();
  const bool nextAllowClear = state_->allowClear();
  const bool nextAlphaChannelEnabled = state_->alphaChannelEnabled();
  const bool nextFormatSelectorEnabled = state_->formatSelectorEnabled();
  const QVector<PresetItem> nextPresets = state_->presets();

  const QString previousCss = colorValueToCss(previousState.selection);
  const QString previousFormatted =
      detail::ColorPickerValueModel::formattedValue(previousState, format_);

  const bool modeChangedFlag = mode_ != nextState.mode;
  const bool modeOptionsChangedFlag = modeOptions_ != nextState.modeOptions;
  const bool activeStopIndexChangedFlag = activeStopIndex_ != nextState.activeStopIndex;
  const bool colorValueChangedFlag = exportColorValue() != nextState.selection;
  const bool formatChangedFlag = format_ != nextFormat;
  const bool allowClearChangedFlag = allowClear_ != nextAllowClear;
  const bool alphaChangedFlag = alphaChannelEnabled() != nextAlphaChannelEnabled;
  const bool formatSelectorChangedFlag = formatSelectorEnabled() != nextFormatSelectorEnabled;
  const bool presetsChangedFlag = !presetItemsEqual(presets_, nextPresets);
  const bool wasCleared = cleared_;

  if (!modeChangedFlag && !modeOptionsChangedFlag && !activeStopIndexChangedFlag &&
      !colorValueChangedFlag && !formatChangedFlag && !allowClearChangedFlag && !alphaChangedFlag &&
      !formatSelectorChangedFlag && !presetsChangedFlag) {
    return;
  }

  format_ = nextFormat;
  allowClear_ = nextAllowClear;
  disabledAlpha_ = !nextAlphaChannelEnabled;
  disabledFormat_ = !nextFormatSelectorEnabled;
  presets_ = nextPresets;
  applySelectionState(createSelectionState(nextState.selection, nextState.mode,
                                           nextState.modeOptions, nextState.activeStopIndex));

  invalidateStyleCache();
  if (presetsChangedFlag) {
    rebuildPresetsPanel();
  }
  refreshStyle();

  if (modeOptionsChangedFlag) {
    emit modeOptionsChanged(modeOptions_);
  }
  if (modeChangedFlag) {
    emit modeChanged(mode_);
  }
  if (formatChangedFlag) {
    emit formatChanged(format_);
  }
  if (allowClearChangedFlag) {
    emit allowClearChanged(allowClear_);
  }
  if (alphaChangedFlag) {
    emit alphaChannelEnabledChanged(!disabledAlpha_);
  }
  if (formatSelectorChangedFlag) {
    emit formatSelectorEnabledChanged(!disabledFormat_);
  }
  if (presetsChangedFlag) {
    emit presetsChanged();
  }
  if (activeStopIndexChangedFlag || colorValueChangedFlag) {
    emit valueChanged(toQtColorValue(exportColorValue()));
  }

  const QString nextCss = cssText();
  const QString nextFormatted = displayText();
  if (previousCss != nextCss) {
    emit cssTextChanged(nextCss);
  }
  if (previousFormatted != nextFormatted) {
    emit displayTextChanged(nextFormatted);
  }
  if (!wasCleared && cleared_) {
    emit cleared();
  }
}

void AdColorPicker::syncStateObject() {
  const ColorValue selection = exportColorValue();
  syncStateObject(
      selection, colorValueToCss(selection),
      detail::ColorPickerValueModel::formattedValue(selection, format_, activeStopIndex_));
}

void AdColorPicker::syncStateObject(const ColorValue& selection, const QString& cssText,
                                    const QString& displayText) {
  if (!state_ || syncingStateObject_ || applyingStateObject_) {
    return;
  }

  QScopedValueRollback<bool> guard(syncingStateObject_, true);
  state_->applyNormalizedState(selection, mode_, modeOptions_, format_, allowClear_,
                               !disabledAlpha_, !disabledFormat_, activeStopIndex_, presets_,
                               cssText, displayText);
}

AdColorPickerPanel::AdColorPickerPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  core_ = new AdColorPicker(AdColorPicker::HostMode::PanelOnly, this);
  layout->addWidget(core_);

  connect(core_, &AdColorPicker::sizeChanged, this, &AdColorPickerPanel::sizeChanged);
  connect(core_, &AdColorPicker::modeChanged, this, &AdColorPickerPanel::modeChanged);
  connect(core_, &AdColorPicker::modeOptionsChanged, this, &AdColorPickerPanel::modeOptionsChanged);
  connect(core_, &AdColorPicker::formatChanged, this, &AdColorPickerPanel::formatChanged);
  connect(core_, &AdColorPicker::allowClearChanged, this, &AdColorPickerPanel::allowClearChanged);
  connect(core_, &AdColorPicker::disabledChanged, this, &AdColorPickerPanel::disabledChanged);
  connect(core_, &AdColorPicker::alphaChannelEnabledChanged, this,
          &AdColorPickerPanel::alphaChannelEnabledChanged);
  connect(core_, &AdColorPicker::formatSelectorEnabledChanged, this,
          &AdColorPickerPanel::formatSelectorEnabledChanged);
  connect(core_, &AdColorPicker::cssTextChanged, this, &AdColorPickerPanel::cssTextChanged);
  connect(core_, &AdColorPicker::displayTextChanged, this, &AdColorPickerPanel::displayTextChanged);
  connect(core_, &AdColorPicker::valueChanged, this, &AdColorPickerPanel::valueChanged);
  connect(core_, &AdColorPicker::presetsChanged, this, &AdColorPickerPanel::presetsChanged);
  connect(core_, &AdColorPicker::stateChanged, this, &AdColorPickerPanel::stateChanged);
  connect(core_, &AdColorPicker::componentTokensChanged, this,
          &AdColorPickerPanel::componentTokensChanged);
  connect(core_, &AdColorPicker::cleared, this, &AdColorPickerPanel::cleared);
  connect(core_, &AdColorPicker::editingFinished, this, &AdColorPickerPanel::editingFinished);
}

AdColorPickerPanel::~AdColorPickerPanel() = default;

AdColorPickerPanel::Size AdColorPickerPanel::size() const {
  return core_ ? core_->size() : Size::Middle;
}

void AdColorPickerPanel::setSize(Size value) {
  if (core_) {
    core_->setSize(value);
  }
}

AdColorPickerPanel::Mode AdColorPickerPanel::mode() const {
  return core_ ? core_->mode() : Mode::Solid;
}

void AdColorPickerPanel::setMode(Mode value) {
  if (core_) {
    core_->setMode(value);
  }
}

QVector<AdColorPickerPanel::Mode> AdColorPickerPanel::modeOptions() const {
  return core_ ? core_->modeOptions() : QVector<Mode>({Mode::Solid});
}

void AdColorPickerPanel::setModeOptions(const QVector<Mode>& options) {
  if (core_) {
    core_->setModeOptions(options);
  }
}

AdColorPickerPanel::Format AdColorPickerPanel::format() const {
  return core_ ? core_->format() : Format::Hex;
}

void AdColorPickerPanel::setFormat(Format value) {
  if (core_) {
    core_->setFormat(value);
  }
}

bool AdColorPickerPanel::allowClear() const { return core_ && core_->allowClear(); }

void AdColorPickerPanel::setAllowClear(bool value) {
  if (core_) {
    core_->setAllowClear(value);
  }
}

bool AdColorPickerPanel::disabled() const { return core_ && core_->disabled(); }

void AdColorPickerPanel::setDisabled(bool value) {
  if (core_) {
    core_->setDisabled(value);
  }
}

bool AdColorPickerPanel::alphaChannelEnabled() const {
  return core_ && core_->alphaChannelEnabled();
}

void AdColorPickerPanel::setAlphaChannelEnabled(bool value) {
  if (core_) {
    core_->setAlphaChannelEnabled(value);
  }
}

bool AdColorPickerPanel::formatSelectorEnabled() const {
  return core_ && core_->formatSelectorEnabled();
}

void AdColorPickerPanel::setFormatSelectorEnabled(bool value) {
  if (core_) {
    core_->setFormatSelectorEnabled(value);
  }
}

QString AdColorPickerPanel::cssText() const { return core_ ? core_->cssText() : QString(); }

void AdColorPickerPanel::setCssText(const QString& value) {
  if (core_) {
    core_->setCssText(value);
  }
}

QString AdColorPickerPanel::displayText() const { return core_ ? core_->displayText() : QString(); }

AdColorValue AdColorPickerPanel::value() const { return core_ ? core_->value() : AdColorValue(); }

void AdColorPickerPanel::setValue(const AdColorValue& value) {
  if (core_) {
    core_->setValue(value);
  }
}

QVector<AdColorPickerPanel::PresetItem> AdColorPickerPanel::presets() const {
  return core_ ? core_->presets() : QVector<PresetItem>();
}

void AdColorPickerPanel::setPresets(const QVector<PresetItem>& presets) {
  if (core_) {
    core_->setPresets(presets);
  }
}

AdColorPickerState* AdColorPickerPanel::state() const { return core_ ? core_->state() : nullptr; }

void AdColorPickerPanel::setState(AdColorPickerState* state) {
  if (core_) {
    core_->setState(state);
  }
}

AdColorPickerPanel::ComponentTokens AdColorPickerPanel::componentTokens() const {
  return core_ ? core_->componentTokens() : ComponentTokens();
}

void AdColorPickerPanel::setComponentTokens(const ComponentTokens& tokens) {
  if (core_) {
    core_->setComponentTokens(tokens);
  }
}

void AdColorPickerPanel::resetComponentTokens() {
  if (core_) {
    core_->resetComponentTokens();
  }
}

}  // namespace adqt::widgets
