#include "carousel.h"

#include "carousel_style.h"
#include "theme/theme.h"
#include "widgets/detail/overlay_accessibility.h"
#include "widgets/detail/timing_hub.h"

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QChildEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace adqt::widgets {

namespace {

template <typename T>
void overlayOptional(std::optional<T>& target, const std::optional<T>& source) {
  if (source) {
    target = source;
  }
}

AdCarousel::ComponentTokens mergeTokens(AdCarousel::ComponentTokens base,
                                        const AdCarousel::ComponentTokens& overlay) {
#define ADQT_OVERLAY_COLOR(name) overlayOptional(base.colors.name, overlay.colors.name)
  ADQT_OVERLAY_COLOR(arrowColor);
  ADQT_OVERLAY_COLOR(dotColor);
  ADQT_OVERLAY_COLOR(focusOutline);
#undef ADQT_OVERLAY_COLOR
#define ADQT_OVERLAY_METRIC(name) overlayOptional(base.metrics.name, overlay.metrics.name)
  ADQT_OVERLAY_METRIC(arrowSize);
  ADQT_OVERLAY_METRIC(arrowOffset);
  ADQT_OVERLAY_METRIC(dotWidth);
  ADQT_OVERLAY_METRIC(dotHeight);
  ADQT_OVERLAY_METRIC(dotGap);
  ADQT_OVERLAY_METRIC(dotOffset);
  ADQT_OVERLAY_METRIC(dotActiveWidth);
  ADQT_OVERLAY_METRIC(hitTargetSize);
  ADQT_OVERLAY_METRIC(focusOutlineWidth);
  ADQT_OVERLAY_METRIC(dragThreshold);
#undef ADQT_OVERLAY_METRIC
  return base;
}

bool isVertical(AdCarousel::DotPlacement placement) {
  return placement == AdCarousel::DotPlacement::Start || placement == AdCarousel::DotPlacement::End;
}

class CarouselTransitionLayer final : public QWidget {
 public:
  explicit CarouselTransitionLayer(QWidget* parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("ad-carousel-transition"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    hide();
  }

  void configure(const QPixmap& from, const QPixmap& to, AdCarousel::Effect effect, bool vertical,
                 int direction) {
    from_ = from;
    to_ = to;
    effect_ = effect;
    vertical_ = vertical;
    direction_ = direction < 0 ? -1 : 1;
    progress_ = 0.0;
    update();
  }

  void setProgress(qreal progress) {
    progress_ = std::clamp(progress, 0.0, 1.0);
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (effect_ == AdCarousel::Effect::Fade) {
      painter.setOpacity(1.0 - progress_);
      painter.drawPixmap(rect(), from_);
      painter.setOpacity(progress_);
      painter.drawPixmap(rect(), to_);
      return;
    }

    QRect fromRect = rect();
    QRect toRect = rect();
    if (vertical_) {
      const int offset = qRound(progress_ * height()) * direction_;
      fromRect.translate(0, -offset);
      toRect.translate(0, direction_ * height() - offset);
    } else {
      const int offset = qRound(progress_ * width()) * direction_;
      fromRect.translate(-offset, 0);
      toRect.translate(direction_ * width() - offset, 0);
    }
    painter.drawPixmap(fromRect, from_);
    painter.drawPixmap(toRect, to_);
  }

 private:
  QPixmap from_;
  QPixmap to_;
  AdCarousel::Effect effect_ = AdCarousel::Effect::Scroll;
  bool vertical_ = false;
  int direction_ = 1;
  qreal progress_ = 0.0;
};

class CarouselArrowButton final : public QAbstractButton {
 public:
  enum class Direction : std::uint8_t { Left, Right, Up, Down };

  explicit CarouselArrowButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
    setObjectName(QStringLiteral("ad-carousel-arrow"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
  }

  void configure(Direction direction, const detail::CarouselAppearance& appearance) {
    direction_ = direction;
    appearance_ = appearance;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal opacity = !isEnabled() ? 0.2 : (underMouse() || hasFocus() || isDown() ? 1.0 : 0.4);
    QColor color = appearance_.arrow;
    color.setAlphaF(static_cast<float>(static_cast<qreal>(color.alphaF()) * opacity));

    const int side = std::min({appearance_.metrics.arrowSize, width(), height()});
    const qreal length = std::max(3.0, side / std::sqrt(2.0));
    const QPointF center = rect().center();
    QPainterPath path;
    switch (direction_) {
      case Direction::Left:
        path.moveTo(center.x() + length / 3.0, center.y() - length / 2.0);
        path.lineTo(center.x() - length / 3.0, center.y());
        path.lineTo(center.x() + length / 3.0, center.y() + length / 2.0);
        break;
      case Direction::Right:
        path.moveTo(center.x() - length / 3.0, center.y() - length / 2.0);
        path.lineTo(center.x() + length / 3.0, center.y());
        path.lineTo(center.x() - length / 3.0, center.y() + length / 2.0);
        break;
      case Direction::Up:
        path.moveTo(center.x() - length / 2.0, center.y() + length / 3.0);
        path.lineTo(center.x(), center.y() - length / 3.0);
        path.lineTo(center.x() + length / 2.0, center.y() + length / 3.0);
        break;
      case Direction::Down:
        path.moveTo(center.x() - length / 2.0, center.y() - length / 3.0);
        path.lineTo(center.x(), center.y() + length / 3.0);
        path.lineTo(center.x() + length / 2.0, center.y() - length / 3.0);
        break;
    }
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(path);

    if (hasFocus()) {
      QPen focusPen(appearance_.focusOutline, appearance_.metrics.focusOutlineWidth);
      painter.setPen(focusPen);
      painter.setBrush(Qt::NoBrush);
      const qreal radius = std::max(2.0, width() / 6.0);
      painter.drawRoundedRect(rect().adjusted(2, 2, -3, -3), radius, radius);
    }
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
  Direction direction_ = Direction::Left;
  detail::CarouselAppearance appearance_;
};

class CarouselDotButton final : public QAbstractButton {
 public:
  using KeyHandler = std::function<void(int)>;

  explicit CarouselDotButton(int index, QWidget* parent = nullptr)
      : QAbstractButton(parent), index_(index) {
    setObjectName(QStringLiteral("ad-carousel-dot"));
    setProperty("slideIndex", index);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
  }

  int slideIndex() const { return index_; }
  void setKeyHandler(KeyHandler handler) { keyHandler_ = std::move(handler); }

  void configure(bool active, bool vertical, qreal progress, bool showProgress,
                 const detail::CarouselAppearance& appearance) {
    active_ = active;
    vertical_ = vertical;
    progress_ = std::clamp(progress, 0.0, 1.0);
    showProgress_ = showProgress;
    appearance_ = appearance;
    setChecked(active);
    setAccessibleDescription(active ? tr("Current slide") : tr("Slide selector"));
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int length = active_ ? appearance_.metrics.dotActiveWidth : appearance_.metrics.dotWidth;
    const int thickness = appearance_.metrics.dotHeight;
    QRectF indicator;
    if (vertical_) {
      indicator = QRectF((width() - thickness) / 2.0, (height() - length) / 2.0, thickness, length);
    } else {
      indicator = QRectF((width() - length) / 2.0, (height() - thickness) / 2.0, length, thickness);
    }

    QColor track = appearance_.dot;
    track.setAlphaF(track.alphaF() * (underMouse() ? 0.75F : 0.2F));
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(indicator, thickness / 2.0, thickness / 2.0);

    if (active_) {
      QRectF fill = indicator;
      if (showProgress_) {
        if (vertical_) {
          fill.setHeight(fill.height() * progress_);
        } else {
          fill.setWidth(fill.width() * progress_);
        }
      }
      painter.setBrush(appearance_.dot);
      painter.drawRoundedRect(fill, thickness / 2.0, thickness / 2.0);
    }

    if (hasFocus()) {
      QPen focusPen(appearance_.focusOutline, appearance_.metrics.focusOutlineWidth);
      painter.setPen(focusPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(indicator.adjusted(-2, -2, 2, 2), thickness, thickness);
    }
  }

  void enterEvent(QEnterEvent* event) override {
    QAbstractButton::enterEvent(event);
    update();
  }

  void leaveEvent(QEvent* event) override {
    QAbstractButton::leaveEvent(event);
    update();
  }

  void keyPressEvent(QKeyEvent* event) override {
    switch (event->key()) {
      case Qt::Key_Home:
      case Qt::Key_End:
      case Qt::Key_Left:
      case Qt::Key_Right:
      case Qt::Key_Up:
      case Qt::Key_Down:
        if (keyHandler_) {
          keyHandler_(event->key());
          event->accept();
          return;
        }
        break;
      default:
        break;
    }
    QAbstractButton::keyPressEvent(event);
  }

 private:
  int index_ = -1;
  bool active_ = false;
  bool vertical_ = false;
  bool showProgress_ = false;
  qreal progress_ = 0.0;
  detail::CarouselAppearance appearance_;
  KeyHandler keyHandler_;
};

QPixmap renderWidget(QWidget* widget, const QSize& size) {
  if (!widget || size.isEmpty()) {
    return {};
  }

  const qreal devicePixelRatio = std::max<qreal>(1.0, widget->devicePixelRatioF());
  const QSize pixelSize(qCeil(size.width() * devicePixelRatio),
                        qCeil(size.height() * devicePixelRatio));
  QPixmap result(pixelSize);
  result.setDevicePixelRatio(devicePixelRatio);
  result.fill(Qt::transparent);
  widget->render(&result, QPoint(), QRegion(),
                 QWidget::DrawWindowBackground | QWidget::DrawChildren);
  return result;
}

}  // namespace

struct AdCarousel::Private {
  explicit Private(AdCarousel* owner) : q(owner) {
    viewport = new QWidget(q);
    viewport->setObjectName(QStringLiteral("ad-carousel-viewport"));
    viewport->setAttribute(Qt::WA_StyledBackground, false);
    transitionLayer = new CarouselTransitionLayer(viewport);

    animation = new QVariantAnimation(q);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    QObject::connect(animation, &QVariantAnimation::valueChanged, q, [this](const QVariant& value) {
      transitionLayer->setProgress(value.toReal());
    });
    QObject::connect(animation, &QVariantAnimation::finished, q, [this]() { finishTransition(); });
  }

  AdCarousel* q = nullptr;
  QWidget* viewport = nullptr;
  CarouselTransitionLayer* transitionLayer = nullptr;
  QVariantAnimation* animation = nullptr;
  QList<QPointer<QWidget>> slides;
  QList<QMetaObject::Connection> slideDestroyedConnections;
  QList<CarouselDotButton*> dots;
  QPointer<QAbstractButton> previousArrow;
  QPointer<QAbstractButton> nextArrow;
  QMetaObject::Connection previousArrowClickConnection;
  QMetaObject::Connection nextArrowClickConnection;
  QMetaObject::Connection previousArrowDestroyedConnection;
  QMetaObject::Connection nextArrowDestroyedConnection;
  QMetaObject::Connection themeChangedConnection;
  QMetaObject::Connection applicationFocusChangedConnection;

  int current = -1;
  int initialSlide = 0;
  bool initialSlidePending = true;
  int transitionFrom = -1;
  int transitionTo = -1;
  bool arrowsVisible = false;
  bool autoplay = false;
  int autoplayInterval = 3000;
  bool autoplayProgressVisible = false;
  bool adaptiveHeight = false;
  DotPlacement dotPlacement = DotPlacement::Bottom;
  bool dotsVisible = true;
  bool draggable = false;
  Effect effect = Effect::Scroll;
  bool infinite = true;
  int transitionDuration = 500;
  QEasingCurve easingCurve = QEasingCurve(QEasingCurve::Linear);
  bool waitForAnimation = false;
  bool pauseOnHover = true;
  bool pauseOnFocus = true;
  bool hovered = false;
  bool dragging = false;
  bool dragExceeded = false;
  QPointF dragOrigin;
  bool running = false;
  qreal autoplayProgress = 0.0;
  qint64 autoplayStartedAt = 0;
  ComponentTokens componentTokens;
  ComponentTokenResolver componentTokenResolver;
  detail::CarouselAppearance appearance;
  QString generatedAccessibleName;

  void installSlideEventFilters(QWidget* root) {
    if (!root) {
      return;
    }
    root->installEventFilter(q);
    const QList<QWidget*> descendants = root->findChildren<QWidget*>();
    for (QWidget* descendant : descendants) {
      descendant->installEventFilter(q);
    }
  }

  void removeSlideEventFilters(QWidget* root) {
    if (!root) {
      return;
    }
    root->removeEventFilter(q);
    const QList<QWidget*> descendants = root->findChildren<QWidget*>();
    for (QWidget* descendant : descendants) {
      descendant->removeEventFilter(q);
    }
  }

  void beginDrag(const QPointF& position) {
    dragging = true;
    dragExceeded = false;
    dragOrigin = position;
  }

  void cancelDrag() {
    dragging = false;
    dragExceeded = false;
  }

  bool updateDrag(const QPointF& position) {
    if (!dragging) {
      return false;
    }
    const QPointF delta = position - dragOrigin;
    const qreal primary = isVertical(dotPlacement) ? delta.y() : delta.x();
    if (std::abs(primary) >= appearance.metrics.dragThreshold) {
      dragExceeded = true;
    }
    return dragExceeded;
  }

  bool finishDrag(const QPointF& position) {
    if (!dragging) {
      return false;
    }
    updateDrag(position);
    dragging = false;
    const bool handled = dragExceeded;
    dragExceeded = false;
    if (!handled) {
      return false;
    }
    const QPointF delta = position - dragOrigin;
    const qreal primary = isVertical(dotPlacement) ? delta.y() : delta.x();
    const bool rtl = !isVertical(dotPlacement) && q->layoutDirection() == Qt::RightToLeft;
    const bool forward =
        isVertical(dotPlacement) ? primary < 0.0 : (rtl ? primary > 0.0 : primary < 0.0);
    forward ? q->next() : q->previous();
    return true;
  }

  ComponentTokens resolvedComponentTokens() const {
    ComponentTokens result = componentTokens;
    if (componentTokenResolver) {
      ComponentTokenContext context;
      context.dotPlacement = dotPlacement;
      context.effect = effect;
      context.enabled = q->isEnabled();
      context.vertical = isVertical(dotPlacement);
      result = mergeTokens(result, componentTokenResolver(context));
    }
    return result;
  }

  void refreshAppearance() {
    appearance = detail::resolveCarouselAppearance(q, resolvedComponentTokens());
    relayout();
  }

  bool canGoPrevious() const { return slides.size() > 1 && (infinite || current > 0); }
  bool canGoNext() const { return slides.size() > 1 && (infinite || current < slides.size() - 1); }

  bool installArrow(bool previous, QAbstractButton* requested) {
    QPointer<QAbstractButton>& slot = previous ? previousArrow : nextArrow;
    QMetaObject::Connection& clickConnection =
        previous ? previousArrowClickConnection : nextArrowClickConnection;
    QMetaObject::Connection& destroyedConnection =
        previous ? previousArrowDestroyedConnection : nextArrowDestroyedConnection;
    QPointer<QAbstractButton>& other = previous ? nextArrow : previousArrow;

    if (requested && requested == slot) {
      return false;
    }
    if (!requested && dynamic_cast<CarouselArrowButton*>(slot.data())) {
      return false;
    }
    if (requested &&
        (requested == other || requested->isAncestorOf(q) || q->isAncestorOf(requested))) {
      return false;
    }

    if (slot) {
      QObject::disconnect(clickConnection);
      QObject::disconnect(destroyedConnection);
      QAbstractButton* old = slot;
      slot = nullptr;
      if (old->parentWidget() == q) {
        old->deleteLater();
      }
    }

    QAbstractButton* button = requested ? requested : new CarouselArrowButton(q);
    button->setParent(q);
    button->show();
    slot = button;
    clickConnection = QObject::connect(button, &QAbstractButton::clicked, q, [this, previous]() {
      const bool rtlHorizontal =
          !isVertical(dotPlacement) && q->layoutDirection() == Qt::RightToLeft;
      if (previous == rtlHorizontal) {
        q->next();
      } else {
        q->previous();
      }
    });
    destroyedConnection = QObject::connect(button, &QObject::destroyed, q, [this, previous]() {
      if (previous) {
        previousArrow = nullptr;
        emit q->previousArrowButtonChanged(nullptr);
      } else {
        nextArrow = nullptr;
        emit q->nextArrowButtonChanged(nullptr);
      }
      detail::deferTimingTask(q,
                              previous ? QStringLiteral("carousel-reset-previous-arrow")
                                       : QStringLiteral("carousel-reset-next-arrow"),
                              [this, previous]() {
                                const QPointer<QAbstractButton>& arrow =
                                    previous ? previousArrow : nextArrow;
                                if (!arrow && installArrow(previous, nullptr)) {
                                  emitArrowChanged(previous);
                                }
                              });
    });
    relayout();
    return true;
  }

  void emitArrowChanged(bool previous) {
    if (previous) {
      emit q->previousArrowButtonChanged(previousArrow);
    } else {
      emit q->nextArrowButtonChanged(nextArrow);
    }
  }

  QAbstractButton* takeArrow(bool previous) {
    QPointer<QAbstractButton>& slot = previous ? previousArrow : nextArrow;
    if (!slot) {
      return nullptr;
    }
    QMetaObject::Connection& clickConnection =
        previous ? previousArrowClickConnection : nextArrowClickConnection;
    QMetaObject::Connection& destroyedConnection =
        previous ? previousArrowDestroyedConnection : nextArrowDestroyedConnection;
    QObject::disconnect(clickConnection);
    QObject::disconnect(destroyedConnection);
    QAbstractButton* result = slot;
    slot = nullptr;
    result->hide();
    result->setParent(nullptr);
    installArrow(previous, nullptr);
    emitArrowChanged(previous);
    return result;
  }

  void navigateFromDot(int source, int key) {
    if (source < 0 || source >= dots.size()) {
      return;
    }

    int target = source;
    int direction = 1;
    if (key == Qt::Key_Home) {
      target = 0;
      direction = target >= current ? 1 : -1;
    } else if (key == Qt::Key_End) {
      target = static_cast<int>(dots.size()) - 1;
      direction = target >= current ? 1 : -1;
    } else {
      const bool verticalMode = isVertical(dotPlacement);
      const bool previousKey =
          verticalMode
              ? key == Qt::Key_Up
              : key == (q->layoutDirection() == Qt::RightToLeft ? Qt::Key_Right : Qt::Key_Left);
      const bool nextKey =
          verticalMode
              ? key == Qt::Key_Down
              : key == (q->layoutDirection() == Qt::RightToLeft ? Qt::Key_Left : Qt::Key_Right);
      if (!previousKey && !nextKey) {
        return;
      }
      direction = previousKey ? -1 : 1;
      target += direction;
      if (target < 0 || target >= dots.size()) {
        if (!infinite) {
          dots.at(source)->setFocus(Qt::TabFocusReason);
          return;
        }
        const int dotCount = static_cast<int>(dots.size());
        target = (target + dotCount) % dotCount;
      }
    }

    requestIndex(target, false, direction);
    if (current >= 0 && current < dots.size()) {
      dots.at(current)->setFocus(Qt::TabFocusReason);
    }
  }

  void rebuildDots() {
    qDeleteAll(dots);
    dots.clear();
    dots.reserve(slides.size());
    for (int index = 0; index < slides.size(); ++index) {
      auto* dot = new CarouselDotButton(index, q);
      QObject::connect(dot, &QAbstractButton::clicked, q,
                       [this, index]() { requestIndex(index, false, index >= current ? 1 : -1); });
      dot->setKeyHandler([this, index](int key) { navigateFromDot(index, key); });
      dots.append(dot);
    }
    relayout();
  }

  void updateAccessibleState() {
    const QString defaultName = AdCarousel::tr("Carousel");
    if (q->accessibleName().isEmpty() || q->accessibleName() == generatedAccessibleName) {
      q->setAccessibleName(defaultName);
      generatedAccessibleName = defaultName;
    }
    for (int index = 0; index < dots.size(); ++index) {
      dots.at(index)->setAccessibleName(AdCarousel::tr("Go to slide %1").arg(index + 1));
    }

    if (slides.isEmpty() || current < 0) {
      detail::syncDerivedAccessibleDescription(q, AdCarousel::tr("No slides"));
    } else {
      detail::syncDerivedAccessibleDescription(
          q, AdCarousel::tr("Slide %1 of %2").arg(current + 1).arg(slides.size()));
    }
  }

  void updatePages() {
    const QRect pageRect = viewport->rect();
    for (int index = 0; index < slides.size(); ++index) {
      QWidget* slide = slides.at(index);
      if (!slide) {
        continue;
      }
      slide->setGeometry(pageRect);
      slide->setVisible(!running && index == current && q->isVisible());
      if (index == current) {
        slide->raise();
      }
    }
    transitionLayer->setGeometry(pageRect);
    if (running) {
      transitionLayer->show();
      transitionLayer->raise();
    }
    updateAccessibleState();
  }

  void configureDefaultArrows() {
    const bool verticalMode = isVertical(dotPlacement);
    auto configure = [&](QAbstractButton* button, bool previous) {
      auto* defaultButton = dynamic_cast<CarouselArrowButton*>(button);
      if (!defaultButton) {
        return;
      }
      CarouselArrowButton::Direction direction;
      if (verticalMode) {
        direction =
            previous ? CarouselArrowButton::Direction::Up : CarouselArrowButton::Direction::Down;
      } else {
        direction =
            previous ? CarouselArrowButton::Direction::Left : CarouselArrowButton::Direction::Right;
      }
      defaultButton->configure(direction, appearance);
    };
    configure(previousArrow, true);
    configure(nextArrow, false);
  }

  void relayout() {
    if (!viewport) {
      return;
    }
    viewport->setGeometry(q->rect());
    updatePages();
    configureDefaultArrows();

    const bool verticalMode = isVertical(dotPlacement);
    const int hit = std::max(appearance.metrics.hitTargetSize, appearance.metrics.arrowSize);
    const int offset = appearance.metrics.arrowOffset;
    if (previousArrow && nextArrow) {
      if (verticalMode) {
        const int x = (q->width() - hit) / 2;
        previousArrow->setGeometry(x, offset, hit, hit);
        nextArrow->setGeometry(x, std::max(offset, q->height() - offset - hit), hit, hit);
      } else {
        const int y = (q->height() - hit) / 2;
        previousArrow->setGeometry(offset, y, hit, hit);
        nextArrow->setGeometry(std::max(offset, q->width() - offset - hit), y, hit, hit);
      }

      const bool showArrows = arrowsVisible && slides.size() > 1;
      const bool rtlHorizontal = !verticalMode && q->layoutDirection() == Qt::RightToLeft;
      const bool previousAvailable = rtlHorizontal ? canGoNext() : canGoPrevious();
      const bool nextAvailable = rtlHorizontal ? canGoPrevious() : canGoNext();
      previousArrow->setVisible(showArrows);
      nextArrow->setVisible(showArrows);
      previousArrow->setEnabled(previousAvailable);
      nextArrow->setEnabled(nextAvailable);
      previousArrow->setCursor(previousAvailable ? Qt::PointingHandCursor : Qt::ArrowCursor);
      nextArrow->setCursor(nextAvailable ? Qt::PointingHandCursor : Qt::ArrowCursor);
      previousArrow->setAccessibleName(rtlHorizontal ? AdCarousel::tr("Next slide")
                                                     : AdCarousel::tr("Previous slide"));
      nextArrow->setAccessibleName(rtlHorizontal ? AdCarousel::tr("Previous slide")
                                                 : AdCarousel::tr("Next slide"));
      previousArrow->raise();
      nextArrow->raise();
    }

    const bool showDots = dotsVisible && slides.size() > 1;
    const bool showProgress = autoplay && autoplayProgressVisible;
    int totalExtent = 0;
    for (int index = 0; index < dots.size(); ++index) {
      const bool active = index == current;
      const int length = active ? appearance.metrics.dotActiveWidth : appearance.metrics.dotWidth;
      totalExtent += length + appearance.metrics.dotGap * 2;
    }
    int cursor = verticalMode ? (q->height() - totalExtent) / 2 : (q->width() - totalExtent) / 2;
    for (int index = 0; index < dots.size(); ++index) {
      CarouselDotButton* dot = dots.at(index);
      const bool active = index == current;
      const int length = active ? appearance.metrics.dotActiveWidth : appearance.metrics.dotWidth;
      const int crossExtent =
          std::max(12, appearance.metrics.dotHeight + appearance.metrics.dotGap * 2);
      const int mainExtent = length + appearance.metrics.dotGap * 2;
      QRect geometry;
      if (verticalMode) {
        const bool atStart = dotPlacement == DotPlacement::Start;
        const bool atLeft = atStart != (q->layoutDirection() == Qt::RightToLeft);
        const int x = atLeft ? appearance.metrics.dotOffset - appearance.metrics.dotGap
                             : q->width() - appearance.metrics.dotOffset - crossExtent +
                                   appearance.metrics.dotGap;
        geometry = QRect(x, cursor, crossExtent, mainExtent);
      } else {
        const bool atTop = dotPlacement == DotPlacement::Top;
        const int y = atTop ? appearance.metrics.dotOffset - appearance.metrics.dotGap
                            : q->height() - appearance.metrics.dotOffset - crossExtent +
                                  appearance.metrics.dotGap;
        const int x =
            q->layoutDirection() == Qt::RightToLeft ? q->width() - cursor - mainExtent : cursor;
        geometry = QRect(x, y, mainExtent, crossExtent);
      }
      dot->setGeometry(geometry);
      dot->configure(active, verticalMode, autoplayProgress, showProgress, appearance);
      dot->setVisible(showDots);
      dot->raise();
      cursor += mainExtent;
    }
  }

  void stopAutoplayCountdown() {
    detail::cancelTimingTask(q, QStringLiteral("carousel-autoplay"));
    detail::clearFrameSubscription(q, QStringLiteral("carousel-autoplay-progress"));
  }

  bool canAutoplay() const {
    QWidget* focusWidget = QApplication::focusWidget();
    const bool focusWithin = focusWidget && (focusWidget == q || q->isAncestorOf(focusWidget));
    if (!autoplay || slides.size() < 2 || !q->isVisible() || !q->isEnabled() || running ||
        (pauseOnHover && hovered) || (pauseOnFocus && focusWithin)) {
      return false;
    }
    return infinite || canGoNext();
  }

  void restartAutoplayCountdown() {
    stopAutoplayCountdown();
    autoplayProgress = 0.0;
    relayout();
    if (!canAutoplay()) {
      return;
    }

    autoplayStartedAt = detail::timingNowMs();
    detail::scheduleTimingTask(q, QStringLiteral("carousel-autoplay"), autoplayInterval, [this]() {
      autoplayProgress = 1.0;
      relayout();
      q->next();
    });
    detail::setFrameSubscription(
        q, QStringLiteral("carousel-autoplay-progress"), autoplayProgressVisible,
        [this](qint64 now, qint64) {
          if (!canAutoplay()) {
            return;
          }
          autoplayProgress = std::clamp(static_cast<qreal>(now - autoplayStartedAt) /
                                            static_cast<qreal>(std::max(1, autoplayInterval)),
                                        0.0, 1.0);
          for (CarouselDotButton* dot : std::as_const(dots)) {
            if (dot && dot->slideIndex() == current) {
              dot->configure(true, isVertical(dotPlacement), autoplayProgress, true, appearance);
              break;
            }
          }
        });
  }

  void setRunning(bool value) {
    if (running == value) {
      return;
    }
    running = value;
    emit q->animationRunningChanged(value);
  }

  void settleRunningTransition() {
    if (!running) {
      return;
    }
    animation->stop();
    finishTransition();
  }

  void finishTransition() {
    if (!running) {
      return;
    }
    transitionLayer->hide();
    setRunning(false);
    updatePages();
    emit q->afterChange(current);
    if (adaptiveHeight) {
      q->updateGeometry();
    }
    relayout();
    restartAutoplayCountdown();
  }

  int boundedIndex(int index) const {
    if (slides.isEmpty()) {
      return -1;
    }
    return std::clamp(index, 0, static_cast<int>(slides.size()) - 1);
  }

  void requestIndex(int requested, bool withoutAnimation, int directionHint) {
    initialSlidePending = false;
    if (slides.isEmpty()) {
      return;
    }
    const int target = boundedIndex(requested);
    if (target < 0) {
      return;
    }
    if (running) {
      if (waitForAnimation) {
        return;
      }
      settleRunningTransition();
    }
    if (target == current) {
      restartAutoplayCountdown();
      return;
    }

    const int from = current;
    QWidget* fromWidget = from >= 0 ? slides.value(from) : nullptr;
    QWidget* toWidget = slides.value(target);
    if (!toWidget) {
      return;
    }

    stopAutoplayCountdown();
    emit q->beforeChange(from, target);
    current = target;
    emit q->currentIndexChanged(current);
    emit q->currentWidgetChanged(toWidget);
    updateAccessibleState();

    int physicalDirection = directionHint < 0 ? -1 : 1;
    if (!isVertical(dotPlacement) && q->layoutDirection() == Qt::RightToLeft) {
      physicalDirection *= -1;
    }
    const int duration = appearance.motionEnabled ? transitionDuration : 0;
    const bool animate = !withoutAnimation && duration > 0 && fromWidget && q->isVisible() &&
                         !viewport->size().isEmpty();
    if (!animate) {
      updatePages();
      emit q->afterChange(current);
      if (adaptiveHeight) {
        q->updateGeometry();
      }
      relayout();
      restartAutoplayCountdown();
      return;
    }

    fromWidget->setGeometry(viewport->rect());
    toWidget->setGeometry(viewport->rect());
    fromWidget->show();
    toWidget->show();
    const QPixmap fromPixmap = renderWidget(fromWidget, viewport->size());
    const QPixmap toPixmap = renderWidget(toWidget, viewport->size());
    fromWidget->hide();
    toWidget->hide();

    transitionFrom = from;
    transitionTo = target;
    transitionLayer->configure(fromPixmap, toPixmap, effect, isVertical(dotPlacement),
                               physicalDirection);
    transitionLayer->setGeometry(viewport->rect());
    transitionLayer->show();
    transitionLayer->raise();
    setRunning(true);
    relayout();
    animation->setDuration(duration);
    animation->setEasingCurve(easingCurve);
    animation->start();
  }

  void updateCurrentAfterRemoval(int removedIndex, QWidget* oldCurrentWidget) {
    const int oldIndex = current;
    if (slides.isEmpty()) {
      current = -1;
      initialSlidePending = true;
    } else if (removedIndex < current) {
      --current;
    } else if (removedIndex == current) {
      current = std::min(removedIndex, static_cast<int>(slides.size()) - 1);
    }

    QWidget* newCurrentWidget = current >= 0 ? slides.value(current) : nullptr;
    if (current != oldIndex) {
      emit q->currentIndexChanged(current);
    }
    if (newCurrentWidget != oldCurrentWidget) {
      emit q->currentWidgetChanged(newCurrentWidget);
    }
    updatePages();
    rebuildDots();
    emit q->countChanged(static_cast<int>(slides.size()));
    q->updateGeometry();
    restartAutoplayCountdown();
  }

  void handleDestroyedSlide(QWidget* destroyedSlide) {
    int index = -1;
    for (int candidate = 0; candidate < slides.size(); ++candidate) {
      if (slides.at(candidate).isNull() || slides.at(candidate).data() == destroyedSlide) {
        index = candidate;
        break;
      }
    }
    if (index < 0) {
      return;
    }
    if (running) {
      settleRunningTransition();
    }
    QWidget* oldCurrentWidget = current >= 0 ? slides.value(current) : nullptr;
    slides.removeAt(index);
    QObject::disconnect(slideDestroyedConnections.takeAt(index));
    updateCurrentAfterRemoval(index, oldCurrentWidget);
  }
};

AdCarousel::AdCarousel(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>(this)) {
  setObjectName(QStringLiteral("ad-carousel"));
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setAttribute(Qt::WA_Hover);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  d_->installArrow(true, nullptr);
  d_->installArrow(false, nullptr);
  d_->refreshAppearance();
  d_->updateAccessibleState();

  d_->themeChangedConnection =
      connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
              this, [this]() { d_->refreshAppearance(); });
  d_->applicationFocusChangedConnection =
      connect(qApp, &QApplication::focusChanged, this, [this](QWidget* previous, QWidget* current) {
        const auto isWithin = [this](QWidget* widget) {
          return widget && (widget == this || isAncestorOf(widget));
        };
        if (isWithin(previous) == isWithin(current)) return;
        if (d_->pauseOnFocus && isWithin(current)) {
          d_->stopAutoplayCountdown();
        } else {
          d_->restartAutoplayCountdown();
        }
      });
}

AdCarousel::~AdCarousel() {
  QObject::disconnect(d_->applicationFocusChangedConnection);
  QObject::disconnect(d_->themeChangedConnection);
  d_->stopAutoplayCountdown();
  d_->animation->stop();
  for (const QMetaObject::Connection& connection : std::as_const(d_->slideDestroyedConnections)) {
    QObject::disconnect(connection);
  }
  QObject::disconnect(d_->previousArrowClickConnection);
  QObject::disconnect(d_->nextArrowClickConnection);
  QObject::disconnect(d_->previousArrowDestroyedConnection);
  QObject::disconnect(d_->nextArrowDestroyedConnection);
}

int AdCarousel::count() const { return static_cast<int>(d_->slides.size()); }
int AdCarousel::initialSlide() const { return d_->initialSlide; }
void AdCarousel::setInitialSlide(int index) {
  index = std::max(0, index);
  if (d_->initialSlide == index) return;
  d_->initialSlide = index;
  if (count() > 0) {
    const int target = std::min(index, count() - 1);
    const int direction = d_->current < 0 || target >= d_->current ? 1 : -1;
    d_->requestIndex(target, false, direction);
  }
  d_->initialSlidePending = count() <= index;
  emit initialSlideChanged(index);
}
int AdCarousel::currentIndex() const { return d_->current; }
QWidget* AdCarousel::currentWidget() const { return widget(d_->current); }

int AdCarousel::addSlide(QWidget* slide) { return insertSlide(count(), slide); }

int AdCarousel::insertSlide(int index, QWidget* slide) {
  if (!slide || slide == this || slide->isAncestorOf(this) || isAncestorOf(slide) ||
      indexOf(slide) >= 0) {
    return -1;
  }
  if (d_->running) {
    d_->settleRunningTransition();
  }
  index = std::clamp(index, 0, count());
  const int oldIndex = d_->current;
  QWidget* oldCurrentWidget = currentWidget();
  slide->setParent(d_->viewport);
  slide->setGeometry(d_->viewport->rect());
  slide->hide();
  d_->installSlideEventFilters(slide);
  d_->slides.insert(index, QPointer<QWidget>(slide));
  d_->slideDestroyedConnections.insert(
      index, connect(slide, &QObject::destroyed, this,
                     [this, slide]() { d_->handleDestroyedSlide(slide); }));

  if (d_->initialSlidePending) {
    d_->current = std::min(d_->initialSlide, count() - 1);
    d_->initialSlidePending = count() <= d_->initialSlide;
  } else if (oldIndex < 0) {
    d_->current = index;
  } else if (index <= oldIndex) {
    ++d_->current;
  }
  if (d_->current != oldIndex) {
    emit currentIndexChanged(d_->current);
  }
  if (currentWidget() != oldCurrentWidget) {
    emit currentWidgetChanged(currentWidget());
  }
  d_->updatePages();
  d_->rebuildDots();
  emit countChanged(count());
  updateGeometry();
  d_->restartAutoplayCountdown();
  return index;
}

void AdCarousel::removeSlide(int index) {
  QWidget* removed = takeSlide(index);
  if (removed) {
    removed->deleteLater();
  }
}

QWidget* AdCarousel::takeSlide(int index) {
  if (index < 0 || index >= count()) {
    return nullptr;
  }
  if (d_->running) {
    d_->settleRunningTransition();
  }
  QWidget* oldCurrentWidget = currentWidget();
  QWidget* removed = d_->slides.at(index);
  QObject::disconnect(d_->slideDestroyedConnections.takeAt(index));
  d_->slides.removeAt(index);
  if (removed) {
    d_->removeSlideEventFilters(removed);
    removed->hide();
    removed->setParent(nullptr);
  }
  d_->updateCurrentAfterRemoval(index, oldCurrentWidget);
  return removed;
}

void AdCarousel::clear() {
  while (!d_->slides.isEmpty()) {
    removeSlide(static_cast<int>(d_->slides.size()) - 1);
  }
}

QWidget* AdCarousel::widget(int index) const {
  return index >= 0 && index < count() ? d_->slides.at(index).data() : nullptr;
}

int AdCarousel::indexOf(const QWidget* slide) const {
  for (int index = 0; index < count(); ++index) {
    if (d_->slides.at(index) == slide) {
      return index;
    }
  }
  return -1;
}

bool AdCarousel::arrowsVisible() const { return d_->arrowsVisible; }
void AdCarousel::setArrowsVisible(bool value) {
  if (d_->arrowsVisible == value) return;
  d_->arrowsVisible = value;
  d_->relayout();
  emit arrowsVisibleChanged(value);
}

bool AdCarousel::autoplay() const { return d_->autoplay; }
void AdCarousel::setAutoplay(bool value) {
  if (d_->autoplay == value) return;
  d_->autoplay = value;
  d_->restartAutoplayCountdown();
  emit autoplayChanged(value);
}

int AdCarousel::autoplayInterval() const { return d_->autoplayInterval; }
void AdCarousel::setAutoplayInterval(int milliseconds) {
  milliseconds = std::max(1, milliseconds);
  if (d_->autoplayInterval == milliseconds) return;
  d_->autoplayInterval = milliseconds;
  d_->restartAutoplayCountdown();
  emit autoplayIntervalChanged(milliseconds);
}

bool AdCarousel::autoplayProgressVisible() const { return d_->autoplayProgressVisible; }
void AdCarousel::setAutoplayProgressVisible(bool value) {
  if (d_->autoplayProgressVisible == value) return;
  d_->autoplayProgressVisible = value;
  d_->restartAutoplayCountdown();
  emit autoplayProgressVisibleChanged(value);
}

bool AdCarousel::adaptiveHeight() const { return d_->adaptiveHeight; }
void AdCarousel::setAdaptiveHeight(bool value) {
  if (d_->adaptiveHeight == value) return;
  d_->adaptiveHeight = value;
  updateGeometry();
  emit adaptiveHeightChanged(value);
}

AdCarousel::DotPlacement AdCarousel::dotPlacement() const { return d_->dotPlacement; }
void AdCarousel::setDotPlacement(DotPlacement value) {
  if (d_->dotPlacement == value) return;
  const bool wasVertical = vertical();
  d_->dotPlacement = value;
  d_->relayout();
  emit dotPlacementChanged(value);
  if (wasVertical != vertical()) emit verticalChanged(vertical());
}

bool AdCarousel::dotsVisible() const { return d_->dotsVisible; }
void AdCarousel::setDotsVisible(bool value) {
  if (d_->dotsVisible == value) return;
  d_->dotsVisible = value;
  d_->relayout();
  emit dotsVisibleChanged(value);
}

bool AdCarousel::draggable() const { return d_->draggable; }
void AdCarousel::setDraggable(bool value) {
  if (d_->draggable == value) return;
  d_->draggable = value;
  if (!value) d_->cancelDrag();
  emit draggableChanged(value);
}

AdCarousel::Effect AdCarousel::effect() const { return d_->effect; }
void AdCarousel::setEffect(Effect value) {
  if (d_->effect == value) return;
  d_->effect = value;
  d_->refreshAppearance();
  emit effectChanged(value);
}

bool AdCarousel::infinite() const { return d_->infinite; }
void AdCarousel::setInfinite(bool value) {
  if (d_->infinite == value) return;
  d_->infinite = value;
  d_->relayout();
  d_->restartAutoplayCountdown();
  emit infiniteChanged(value);
}

int AdCarousel::transitionDuration() const { return d_->transitionDuration; }
void AdCarousel::setTransitionDuration(int milliseconds) {
  milliseconds = std::max(0, milliseconds);
  if (d_->transitionDuration == milliseconds) return;
  d_->transitionDuration = milliseconds;
  emit transitionDurationChanged(milliseconds);
}

QEasingCurve AdCarousel::easingCurve() const { return d_->easingCurve; }
void AdCarousel::setEasingCurve(const QEasingCurve& value) {
  if (d_->easingCurve == value) return;
  d_->easingCurve = value;
  emit easingCurveChanged(value);
}

bool AdCarousel::waitForAnimation() const { return d_->waitForAnimation; }
void AdCarousel::setWaitForAnimation(bool value) {
  if (d_->waitForAnimation == value) return;
  d_->waitForAnimation = value;
  emit waitForAnimationChanged(value);
}

bool AdCarousel::pauseOnHover() const { return d_->pauseOnHover; }
void AdCarousel::setPauseOnHover(bool value) {
  if (d_->pauseOnHover == value) return;
  d_->pauseOnHover = value;
  d_->restartAutoplayCountdown();
  emit pauseOnHoverChanged(value);
}

bool AdCarousel::pauseOnFocus() const { return d_->pauseOnFocus; }
void AdCarousel::setPauseOnFocus(bool value) {
  if (d_->pauseOnFocus == value) return;
  d_->pauseOnFocus = value;
  d_->restartAutoplayCountdown();
  emit pauseOnFocusChanged(value);
}

bool AdCarousel::vertical() const { return isVertical(d_->dotPlacement); }
bool AdCarousel::animationRunning() const { return d_->running; }

QAbstractButton* AdCarousel::previousArrowButton() const { return d_->previousArrow; }
void AdCarousel::setPreviousArrowButton(QAbstractButton* button) {
  if (d_->installArrow(true, button)) {
    d_->emitArrowChanged(true);
  }
}
QAbstractButton* AdCarousel::takePreviousArrowButton() { return d_->takeArrow(true); }
QAbstractButton* AdCarousel::nextArrowButton() const { return d_->nextArrow; }
void AdCarousel::setNextArrowButton(QAbstractButton* button) {
  if (d_->installArrow(false, button)) {
    d_->emitArrowChanged(false);
  }
}
QAbstractButton* AdCarousel::takeNextArrowButton() { return d_->takeArrow(false); }

AdCarousel::ComponentTokens AdCarousel::componentTokens() const { return d_->componentTokens; }
void AdCarousel::setComponentTokens(const ComponentTokens& value) {
  d_->componentTokens = value;
  d_->refreshAppearance();
  emit componentTokensChanged();
}
void AdCarousel::resetComponentTokens() { setComponentTokens({}); }
void AdCarousel::setComponentTokenResolver(ComponentTokenResolver resolver) {
  d_->componentTokenResolver = std::move(resolver);
  d_->refreshAppearance();
  emit componentTokensChanged();
}
void AdCarousel::resetComponentTokenResolver() { setComponentTokenResolver({}); }

QSize AdCarousel::sizeHint() const {
  QSize result;
  if (d_->adaptiveHeight) {
    if (QWidget* slide = currentWidget(); slide && slide->sizeHint().isValid()) {
      result = slide->sizeHint();
    }
  } else {
    for (const QPointer<QWidget>& slide : std::as_const(d_->slides)) {
      if (slide && slide->sizeHint().isValid()) {
        result = result.expandedTo(slide->sizeHint());
      }
    }
  }
  return (result.isValid() ? result : QSize(320, 180)).expandedTo(QSize(120, 80));
}

QSize AdCarousel::minimumSizeHint() const {
  QSize result;
  if (d_->adaptiveHeight) {
    if (QWidget* slide = currentWidget(); slide && slide->minimumSizeHint().isValid()) {
      result = slide->minimumSizeHint();
    }
  } else {
    for (const QPointer<QWidget>& slide : std::as_const(d_->slides)) {
      if (slide && slide->minimumSizeHint().isValid()) {
        result = result.expandedTo(slide->minimumSizeHint());
      }
    }
  }
  return (result.isValid() ? result : QSize(80, 48)).expandedTo(QSize(80, 48));
}

bool AdCarousel::hasHeightForWidth() const {
  return d_->adaptiveHeight && currentWidget() && currentWidget()->hasHeightForWidth();
}

int AdCarousel::heightForWidth(int width) const {
  QWidget* slide = currentWidget();
  if (!d_->adaptiveHeight || !slide) return QWidget::heightForWidth(width);
  return slide->hasHeightForWidth() ? slide->heightForWidth(width) : slide->sizeHint().height();
}

void AdCarousel::setCurrentIndex(int index) { goTo(index); }
void AdCarousel::goTo(int index, bool withoutAnimation) {
  const int direction = d_->current < 0 || index >= d_->current ? 1 : -1;
  d_->requestIndex(index, withoutAnimation, direction);
}

void AdCarousel::next() {
  if (count() < 2) return;
  int target = d_->current + 1;
  if (target >= count()) {
    if (!d_->infinite) {
      d_->restartAutoplayCountdown();
      return;
    }
    target = 0;
  }
  d_->requestIndex(target, false, 1);
}

void AdCarousel::previous() {
  if (count() < 2) return;
  int target = d_->current - 1;
  if (target < 0) {
    if (!d_->infinite) return;
    target = count() - 1;
  }
  d_->requestIndex(target, false, -1);
}

bool AdCarousel::event(QEvent* event) {
  if (event->type() == QEvent::Enter) {
    d_->hovered = true;
    if (d_->pauseOnHover) d_->stopAutoplayCountdown();
  } else if (event->type() == QEvent::Leave) {
    d_->hovered = false;
    d_->restartAutoplayCountdown();
  } else if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate) {
    d_->cancelDrag();
  }
  return QWidget::event(event);
}

bool AdCarousel::eventFilter(QObject* watched, QEvent* event) {
  auto* watchedWidget = qobject_cast<QWidget*>(watched);
  if ((event->type() == QEvent::ChildAdded || event->type() == QEvent::ChildPolished) &&
      watchedWidget && isAncestorOf(watchedWidget)) {
    auto* childEvent = static_cast<QChildEvent*>(event);
    QPointer<QWidget> child = qobject_cast<QWidget*>(childEvent->child());
    if (child) {
      child->installEventFilter(this);
      const QString taskKey = QStringLiteral("carousel-install-slide-event-filter-%1")
                                  .arg(reinterpret_cast<quintptr>(child.data()), 0, 16);
      detail::deferTimingTask(this, taskKey, [this, child]() {
        if (child && isAncestorOf(child)) {
          d_->installSlideEventFilters(child);
        }
      });
    }
  }

  if (!d_->draggable || d_->running || !watchedWidget) {
    return QWidget::eventFilter(watched, event);
  }
  const bool interactiveTarget =
      watchedWidget->inherits("QAbstractButton") || watchedWidget->inherits("QAbstractSlider") ||
      watchedWidget->inherits("QAbstractSpinBox") || watchedWidget->inherits("QComboBox") ||
      watchedWidget->inherits("QLineEdit") || watchedWidget->inherits("QTextEdit") ||
      watchedWidget->inherits("QPlainTextEdit") || watchedWidget->inherits("QAbstractItemView");
  if (interactiveTarget && !d_->dragging) {
    return QWidget::eventFilter(watched, event);
  }
  auto mappedPosition = [this, watchedWidget](QMouseEvent* mouseEvent) {
    return QPointF(watchedWidget->mapTo(this, mouseEvent->position().toPoint()));
  };
  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (mouseEvent->button() == Qt::LeftButton) {
      d_->beginDrag(mappedPosition(mouseEvent));
    }
  } else if (event->type() == QEvent::MouseMove && d_->dragging) {
    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (d_->updateDrag(mappedPosition(mouseEvent))) {
      mouseEvent->accept();
      return true;
    }
  } else if (event->type() == QEvent::MouseButtonRelease && d_->dragging) {
    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (mouseEvent->button() == Qt::LeftButton && d_->finishDrag(mappedPosition(mouseEvent))) {
      mouseEvent->accept();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void AdCarousel::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::EnabledChange || event->type() == QEvent::LayoutDirectionChange ||
      event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
      event->type() == QEvent::LanguageChange) {
    d_->refreshAppearance();
    if (event->type() == QEvent::LanguageChange) d_->updateAccessibleState();
    d_->restartAutoplayCountdown();
  }
}

void AdCarousel::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (d_->running) d_->settleRunningTransition();
  d_->relayout();
}

void AdCarousel::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  d_->updatePages();
  d_->relayout();
  d_->restartAutoplayCountdown();
}

void AdCarousel::hideEvent(QHideEvent* event) {
  d_->cancelDrag();
  d_->stopAutoplayCountdown();
  QWidget::hideEvent(event);
}

void AdCarousel::keyPressEvent(QKeyEvent* event) {
  const bool verticalMode = vertical();
  const bool rtl = !verticalMode && layoutDirection() == Qt::RightToLeft;
  if (event->key() == Qt::Key_Home) {
    goTo(0);
  } else if (event->key() == Qt::Key_End) {
    goTo(count() - 1);
  } else if ((verticalMode && event->key() == Qt::Key_Up) ||
             (!verticalMode && event->key() == Qt::Key_Left)) {
    rtl ? next() : previous();
  } else if ((verticalMode && event->key() == Qt::Key_Down) ||
             (!verticalMode && event->key() == Qt::Key_Right)) {
    rtl ? previous() : next();
  } else {
    QWidget::keyPressEvent(event);
    return;
  }
  event->accept();
}

void AdCarousel::mousePressEvent(QMouseEvent* event) {
  if (d_->draggable && event->button() == Qt::LeftButton && !d_->running) {
    d_->beginDrag(event->position());
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void AdCarousel::mouseMoveEvent(QMouseEvent* event) {
  if (d_->dragging) {
    d_->updateDrag(event->position());
    event->accept();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void AdCarousel::mouseReleaseEvent(QMouseEvent* event) {
  if (!d_->dragging || event->button() != Qt::LeftButton) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  d_->finishDrag(event->position());
  event->accept();
}

}  // namespace adqt::widgets
