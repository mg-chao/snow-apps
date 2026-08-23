#include "scroll_area.h"

#include "detail/themed_scrollbar.h"
#include "theme/theme_manager.h"

#include <QColor>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QHideEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>

#include <algorithm>

namespace adqt::widgets {

namespace {

constexpr float kTrackOpacity = 0.18F;
constexpr float kHandleOpacity = 0.45F;
constexpr float kHandleHoverOpacity = 0.60F;
constexpr float kHandlePressedOpacity = 0.75F;

QMargins normalizedMargins(const QMargins& margins) {
  return QMargins(std::max(0, margins.left()), std::max(0, margins.top()),
                  std::max(0, margins.right()), std::max(0, margins.bottom()));
}

}  // namespace

AdScrollBar::AdScrollBar(Qt::Orientation orientation, QWidget* parent)
    : QScrollBar(orientation, parent) {
  setObjectName(QStringLiteral("adscrollbar"));
  setFocusPolicy(Qt::NoFocus);
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);

  connect(this, &QScrollBar::sliderPressed, this, [this]() { setExpanded(true); });
  connect(this, &QScrollBar::sliderReleased, this, [this]() { setExpanded(underMouse()); });
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { applyScrollBarStyle(); });

  applyScrollBarStyle();
}

int AdScrollBar::scrollBarThickness() const { return scrollBarThickness_; }

void AdScrollBar::setScrollBarThickness(int value) {
  const int normalized = std::max(6, value);
  if (scrollBarThickness_ == normalized) {
    return;
  }
  scrollBarThickness_ = normalized;
  applyScrollBarStyle();
  emit scrollBarThicknessChanged(scrollBarThickness_);
}

int AdScrollBar::scrollBarRadius() const { return scrollBarRadius_; }

void AdScrollBar::setScrollBarRadius(int value) {
  const int normalized = std::max(0, value);
  if (scrollBarRadius_ == normalized) {
    return;
  }
  scrollBarRadius_ = normalized;
  applyScrollBarStyle();
  emit scrollBarRadiusChanged(scrollBarRadius_);
}

int AdScrollBar::collapsedVisualThickness() const { return collapsedVisualThickness_; }

void AdScrollBar::setCollapsedVisualThickness(int value) {
  const int normalized = std::max(1, value);
  if (collapsedVisualThickness_ == normalized) {
    return;
  }
  collapsedVisualThickness_ = normalized;
  applyScrollBarStyle();
  emit collapsedVisualThicknessChanged(collapsedVisualThickness_);
}

QMargins AdScrollBar::overlayMargins() const { return overlayMargins_; }

void AdScrollBar::setOverlayMargins(const QMargins& margins) {
  const QMargins normalized = normalizedMargins(margins);
  if (overlayMargins_ == normalized) {
    return;
  }
  overlayMargins_ = normalized;
  updateOverlayGeometry();
  emit overlayMarginsChanged(overlayMargins_);
}

QRect AdScrollBar::overlayBounds() const { return overlayBounds_; }

void AdScrollBar::setOverlayBounds(const QRect& bounds) {
  if (overlayBounds_ == bounds) {
    return;
  }
  overlayBounds_ = bounds;
  updateOverlayGeometry();
  emit overlayBoundsChanged(overlayBounds_);
}

bool AdScrollBar::isExpanded() const { return expanded_; }

bool AdScrollBar::hoverExpansionEnabled() const { return hoverExpansionEnabled_; }

void AdScrollBar::setHoverExpansionEnabled(bool enabled) {
  if (hoverExpansionEnabled_ == enabled) {
    return;
  }
  hoverExpansionEnabled_ = enabled;
  if (!hoverExpansionEnabled_) {
    setExpanded(false);
  } else {
    applyScrollBarStyle();
  }
  emit hoverExpansionEnabledChanged(hoverExpansionEnabled_);
}

bool AdScrollBar::overlayColorsEnabled() const { return overlayColorsEnabled_; }

void AdScrollBar::setOverlayColorsEnabled(bool enabled) {
  if (overlayColorsEnabled_ == enabled) {
    return;
  }
  overlayColorsEnabled_ = enabled;
  applyScrollBarStyle();
  emit overlayColorsEnabledChanged(overlayColorsEnabled_);
}

bool AdScrollBar::isEmbedded() const { return embedded_; }

void AdScrollBar::setEmbedded(bool embedded) {
  if (embedded_ == embedded) {
    return;
  }
  embedded_ = embedded;
  const bool wasExpanded = expanded_;
  if (embedded_) {
    expanded_ = false;
  }
  applyScrollBarStyle();
  if (wasExpanded != expanded_) {
    emit expandedChanged(expanded_);
  }
  emit embeddedChanged(embedded_);
}

void AdScrollBar::changeEvent(QEvent* event) {
  QScrollBar::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
      applyScrollBarStyle();
      break;
    default:
      break;
  }
}

void AdScrollBar::enterEvent(QEnterEvent* event) {
  QScrollBar::enterEvent(event);
  if (!embedded_ && hoverExpansionEnabled_) {
    setExpanded(true);
  }
}

void AdScrollBar::leaveEvent(QEvent* event) {
  QScrollBar::leaveEvent(event);
  if (!isSliderDown()) {
    setExpanded(false);
  }
}

void AdScrollBar::hideEvent(QHideEvent* event) {
  QScrollBar::hideEvent(event);
  setExpanded(false);
}

void AdScrollBar::setExpanded(bool expanded) {
  expanded = !embedded_ && hoverExpansionEnabled_ && expanded;
  if (expanded_ == expanded) {
    return;
  }
  expanded_ = expanded;
  applyScrollBarStyle();
  emit expandedChanged(expanded_);
}

void AdScrollBar::applyScrollBarStyle() {
  if (applyingStyle_) {
    return;
  }
  const QScopedValueRollback<bool> guard(applyingStyle_, true);

  const int thickness = std::max(6, scrollBarThickness_);
  const bool expanded = !embedded_ && hoverExpansionEnabled_ && expanded_;
  const int extent = expanded ? thickness + std::max(1, thickness / 2) : thickness;
  const int inset = embedded_ || expanded ? 0 : std::max(0, thickness - collapsedVisualThickness_);
  const int visualThickness = std::max(1, extent - inset);
  const int maximumRadius = std::max(1, (visualThickness + 1) / 2);
  const int radius =
      scrollBarRadius_ > 0 ? std::min(scrollBarRadius_, maximumRadius) : maximumRadius;

  detail::applyThemedScrollBar(this, extent, radius, inset);
  if (!embedded_ && overlayColorsEnabled_) {
    setProperty("_adqt_track_color", QColor::fromRgbF(0.0F, 0.0F, 0.0F, kTrackOpacity));
    setProperty("_adqt_handle_color", QColor::fromRgbF(0.0F, 0.0F, 0.0F, kHandleOpacity));
    setProperty("_adqt_handle_hover_color",
                QColor::fromRgbF(0.0F, 0.0F, 0.0F, kHandleHoverOpacity));
    setProperty("_adqt_handle_pressed_color",
                QColor::fromRgbF(0.0F, 0.0F, 0.0F, kHandlePressedOpacity));
  }
  update();
  updateOverlayGeometry();
}

void AdScrollBar::updateOverlayGeometry() {
  if (!overlayBounds_.isValid()) {
    return;
  }

  const QRect available = overlayBounds_.marginsRemoved(overlayMargins_);
  QRect nextGeometry;
  if (!available.isValid()) {
    nextGeometry = QRect();
  } else if (orientation() == Qt::Vertical) {
    nextGeometry =
        QRect(available.right() - width() + 1, available.top(), width(), available.height());
  } else {
    nextGeometry =
        QRect(available.left(), available.bottom() - height() + 1, available.width(), height());
  }

  const QRect previousGeometry = geometry();
  if (previousGeometry != nextGeometry) {
    setGeometry(nextGeometry);
    if (QWidget* host = parentWidget()) {
      host->update(previousGeometry.united(nextGeometry));
    }
  }
  if (nextGeometry.isValid()) {
    raise();
  }
}

void AdScrollArea::applyThemedScrollBar(QScrollBar* bar, int extent, int radius, int inset,
                                        int marginStart, int marginEnd) {
  detail::applyThemedScrollBar(bar, extent, radius, inset, marginStart, marginEnd);
}

AdScrollArea::AdScrollArea(QWidget* parent) : QScrollArea(parent) {
  setObjectName(QStringLiteral("adscrollarea"));
  setFrameShape(QFrame::NoFrame);
  setAlignment(Qt::AlignTop | Qt::AlignLeft);
  setWidgetResizable(false);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  if (viewport()) {
    viewport()->installEventFilter(this);
    overlayVerticalScrollBar_ = new AdScrollBar(Qt::Vertical, viewport());
    overlayVerticalScrollBar_->setObjectName(QStringLiteral("adscrollarea-overlay-vbar"));
    overlayVerticalScrollBar_->hide();
    overlayHorizontalScrollBar_ = new AdScrollBar(Qt::Horizontal, viewport());
    overlayHorizontalScrollBar_->setObjectName(QStringLiteral("adscrollarea-overlay-hbar"));
    overlayHorizontalScrollBar_->hide();
    overlayVerticalScrollBar_->setOverlayBounds(viewport()->rect());
    overlayHorizontalScrollBar_->setOverlayBounds(viewport()->rect());
  }

  if (verticalScrollBar()) {
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { syncOverlayScrollBars(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { syncOverlayScrollBars(); });
  }
  if (horizontalScrollBar()) {
    connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { syncOverlayScrollBars(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { syncOverlayScrollBars(); });
  }
  if (overlayVerticalScrollBar_) {
    connect(overlayVerticalScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
      QScrollBar* source = verticalScrollBar();
      if (!source || source->value() == value) {
        return;
      }
      source->setValue(value);
    });
  }
  if (overlayHorizontalScrollBar_) {
    connect(overlayHorizontalScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
      QScrollBar* source = horizontalScrollBar();
      if (!source || source->value() == value) {
        return;
      }
      source->setValue(value);
    });
  }

  applyScrollBarStyle();
  syncOverlayScrollBars();
}

void AdScrollArea::setContentWidget(QWidget* widget) {
  if (contentWidget_ == widget) {
    return;
  }

  if (contentWidget_) {
    contentWidget_->removeEventFilter(this);
  }

  QWidget* previous = takeWidget();
  if (previous && previous != widget) {
    previous->removeEventFilter(this);
  }

  contentWidget_ = widget;
  if (contentWidget_) {
    contentWidget_->installEventFilter(this);
    setWidget(contentWidget_);
    syncContentSize();
    return;
  }
  syncOverlayScrollBars();
}

QWidget* AdScrollArea::contentWidget() const { return contentWidget_; }

AdScrollBar* AdScrollArea::overlayVerticalScrollBar() const { return overlayVerticalScrollBar_; }

AdScrollBar* AdScrollArea::overlayHorizontalScrollBar() const {
  return overlayHorizontalScrollBar_;
}

bool AdScrollArea::fitToWidth() const { return fitToWidth_; }

void AdScrollArea::setFitToWidth(bool value) {
  if (fitToWidth_ == value) {
    return;
  }
  fitToWidth_ = value;
  syncContentSize();
  emit fitToWidthChanged(fitToWidth_);
}

int AdScrollArea::scrollBarThickness() const { return scrollBarThickness_; }

void AdScrollArea::setScrollBarThickness(int value) {
  const int normalized = std::max(6, value);
  if (scrollBarThickness_ == normalized) {
    return;
  }
  scrollBarThickness_ = normalized;
  applyScrollBarStyle();
  updateOverlayGeometry();
  emit scrollBarThicknessChanged(scrollBarThickness_);
}

int AdScrollArea::scrollBarRadius() const { return scrollBarRadius_; }

void AdScrollArea::setScrollBarRadius(int value) {
  const int normalized = std::max(0, value);
  if (scrollBarRadius_ == normalized) {
    return;
  }
  scrollBarRadius_ = normalized;
  applyScrollBarStyle();
  emit scrollBarRadiusChanged(scrollBarRadius_);
}

bool AdScrollArea::eventFilter(QObject* watched, QEvent* event) {
  if (event && (watched == contentWidget_ || watched == viewport())) {
    switch (event->type()) {
      case QEvent::LayoutRequest:
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::StyleChange:
      case QEvent::FontChange:
        syncContentSize();
        if (watched == viewport()) {
          updateOverlayGeometry();
        }
        break;
      default:
        break;
    }
  }
  return QScrollArea::eventFilter(watched, event);
}

void AdScrollArea::changeEvent(QEvent* event) {
  QScrollArea::changeEvent(event);
  if (!event) {
    return;
  }

  switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
      applyScrollBarStyle();
      syncContentSize();
      break;
    default:
      break;
  }
}

void AdScrollArea::resizeEvent(QResizeEvent* event) {
  QScrollArea::resizeEvent(event);
  syncContentSize();
  syncOverlayScrollBars();
}

void AdScrollArea::applyScrollBarStyle() {
  const int thickness = std::max(6, scrollBarThickness_);

  if (overlayVerticalScrollBar_) {
    overlayVerticalScrollBar_->setScrollBarThickness(thickness);
    overlayVerticalScrollBar_->setScrollBarRadius(scrollBarRadius_);
  }

  if (overlayHorizontalScrollBar_) {
    overlayHorizontalScrollBar_->setScrollBarThickness(thickness);
    overlayHorizontalScrollBar_->setScrollBarRadius(scrollBarRadius_);
  }

  if (QScrollBar* hBar = horizontalScrollBar()) {
    const int maximumRadius = std::max(1, (thickness + 1) / 2);
    const int radius =
        scrollBarRadius_ > 0 ? std::min(scrollBarRadius_, maximumRadius) : maximumRadius;
    applyThemedScrollBar(hBar, thickness, radius, 0);
  }

  updateOverlayGeometry();
  if (viewport()) {
    viewport()->update();
  }
}

void AdScrollArea::syncContentSize() {
  if (!contentWidget_ || syncingContentSize_) {
    return;
  }

  syncingContentSize_ = true;

  QSize hint = contentWidget_->sizeHint();
  if (!hint.isValid()) {
    hint = contentWidget_->minimumSizeHint();
  }
  if (!hint.isValid()) {
    hint = contentWidget_->size();
  }

  int targetWidth = hint.width();
  if (fitToWidth_) {
    targetWidth = viewport()->width();
  } else {
    targetWidth = std::max(targetWidth, contentWidget_->minimumSizeHint().width());
  }

  int targetHeight = hint.height();
  if (fitToWidth_ && contentWidget_->hasHeightForWidth()) {
    const int fittedHeight = contentWidget_->heightForWidth(targetWidth);
    if (fittedHeight >= 0) {
      targetHeight = fittedHeight;
    }
  } else if (!fitToWidth_) {
    targetHeight = std::max(targetHeight, contentWidget_->minimumSizeHint().height());
  }
  // Paired size-hint heights may describe a different width. When fitting the content,
  // heightForWidth() is the authoritative natural height at the viewport width.
  targetHeight = std::max(targetHeight, contentWidget_->minimumHeight());
  targetWidth = std::max(0, targetWidth);
  targetHeight = std::max(0, targetHeight);

  const QSize nextSize(targetWidth, targetHeight);
  if (contentWidget_->size() != nextSize) {
    contentWidget_->resize(nextSize);
  }

  syncingContentSize_ = false;
  syncOverlayScrollBars();
}

void AdScrollArea::syncOverlayScrollBars() {
  if (!overlayVerticalScrollBar_ && !overlayHorizontalScrollBar_) {
    return;
  }

  auto syncBar = [](AdScrollBar* overlay, QScrollBar* source) {
    if (!overlay) {
      return;
    }
    if (!source) {
      overlay->hide();
      return;
    }
    {
      QSignalBlocker blocker(overlay);
      overlay->setRange(source->minimum(), source->maximum());
      overlay->setPageStep(source->pageStep());
      overlay->setSingleStep(source->singleStep());
      overlay->setValue(source->value());
    }
    const bool visible = source->maximum() > source->minimum();
    overlay->setVisible(visible);
    if (visible) {
      overlay->raise();
    }
  };

  syncBar(overlayVerticalScrollBar_, verticalScrollBar());
  syncBar(overlayHorizontalScrollBar_, horizontalScrollBar());
  updateOverlayGeometry();
}

void AdScrollArea::updateOverlayGeometry() {
  if (!viewport()) {
    return;
  }

  const QRect bounds = viewport()->rect();
  if (overlayVerticalScrollBar_) {
    overlayVerticalScrollBar_->setOverlayBounds(bounds);
  }
  if (overlayHorizontalScrollBar_) {
    overlayHorizontalScrollBar_->setOverlayBounds(bounds);
  }
}

}  // namespace adqt::widgets
