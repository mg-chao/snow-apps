#include "spin.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <utility>

#include "spin_style.h"
#include "theme/theme_manager.h"
#include "widgets/detail/timing_hub.h"

namespace adqt::widgets {

namespace {

constexpr char kActivationTaskKey[] = "AdSpin.Activation";
constexpr char kAnimationFrameKey[] = "AdSpin.Animation";
constexpr char kAutoProgressTaskKey[] = "AdSpin.AutoProgress";

struct SectionLayout {
  QRectF indicatorRect;
  QRectF descriptionRect;
};

QSize naturalIndicatorSize(QWidget* indicator, int fallback) {
  if (!indicator) {
    return QSize(fallback, fallback);
  }
  QSize size = indicator->sizeHint();
  if (!size.isValid() || size.isEmpty()) {
    size = indicator->size();
  }
  if (!size.isValid() || size.isEmpty()) {
    size = QSize(fallback, fallback);
  }
  return size.expandedTo(indicator->minimumSizeHint())
      .expandedTo(indicator->minimumSize())
      .boundedTo(indicator->maximumSize());
}

SectionLayout sectionLayoutFor(const QRect& bounds, QWidget* indicator, int dotSize,
                               const QString& description, const QFont& font, int gap) {
  const QSize indicatorSize = naturalIndicatorSize(indicator, dotSize);
  const QFontMetrics metrics(font);
  const int availableWidth = std::max(1, bounds.width());
  QRect textBounds;
  if (!description.isEmpty()) {
    textBounds = metrics.boundingRect(QRect(0, 0, availableWidth, 10000),
                                      Qt::AlignHCenter | Qt::TextWordWrap, description);
  }
  const int textHeight =
      description.isEmpty() ? 0 : std::max(metrics.height(), textBounds.height());
  const int totalHeight = indicatorSize.height() + (textHeight > 0 ? gap + textHeight : 0);
  // QRect's inclusive integer center shifts tight indicators up and left.
  // Use the same continuous bounds as the painter and indicator rectangles.
  const QPointF center = QRectF(bounds).center();
  const qreal top = center.y() - totalHeight / 2.0;
  SectionLayout layout;
  layout.indicatorRect = QRectF(center.x() - indicatorSize.width() / 2.0, top,
                                indicatorSize.width(), indicatorSize.height());
  if (textHeight > 0) {
    layout.descriptionRect =
        QRectF(bounds.left(), layout.indicatorRect.bottom() + gap, bounds.width(), textHeight);
  }
  return layout;
}

qreal triangleWave(qreal value) {
  const qreal wrapped = value - std::floor(value / 2.0) * 2.0;
  return wrapped <= 1.0 ? wrapped : 2.0 - wrapped;
}

bool isBlockedInputEvent(QEvent::Type type) {
  switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::ContextMenu:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
    case QEvent::NativeGesture:
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Shortcut:
    case QEvent::ShortcutOverride:
    case QEvent::InputMethod:
      return true;
    default:
      return false;
  }
}

class AdSpinAccessible final : public QAccessibleWidget, public QAccessibleValueInterface {
 public:
  explicit AdSpinAccessible(AdSpin* spin) : QAccessibleWidget(spin, QAccessible::ProgressBar) {}

  QString text(QAccessible::Text type) const override {
    const auto* spin = qobject_cast<AdSpin*>(object());
    if (!spin) {
      return QAccessibleWidget::text(type);
    }
    switch (type) {
      case QAccessible::Name:
        return spin->accessibleName().trimmed().isEmpty() ? AdSpin::tr("Loading")
                                                          : spin->accessibleName().trimmed();
      case QAccessible::Description:
        return spin->accessibleDescription().trimmed();
      case QAccessible::Value:
        return spin->progressMode() == AdSpin::ProgressMode::None
                   ? QString()
                   : AdSpin::tr("%1%").arg(qRound(spin->displayedPercent()));
      default:
        return QAccessibleWidget::text(type);
    }
  }

  QAccessible::State state() const override {
    QAccessible::State state = QAccessibleWidget::state();
    const auto* spin = qobject_cast<AdSpin*>(object());
    if (spin) {
      state.busy = spin->isActive();
      state.invisible = !spin->isActive() || !spin->isVisible();
      state.readOnly = true;
      state.focusable = false;
    }
    return state;
  }

  void* interface_cast(QAccessible::InterfaceType type) override {
    if (type == QAccessible::ValueInterface) {
      return static_cast<QAccessibleValueInterface*>(this);
    }
    return QAccessibleWidget::interface_cast(type);
  }

  QVariant currentValue() const override {
    const auto* spin = qobject_cast<AdSpin*>(object());
    if (!spin || spin->progressMode() == AdSpin::ProgressMode::None) {
      return QVariant();
    }
    return spin->displayedPercent();
  }

  void setCurrentValue(const QVariant&) override {}
  QVariant maximumValue() const override { return 100.0; }
  QVariant minimumValue() const override { return 0.0; }
  QVariant minimumStepSize() const override { return QVariant(); }
};

QAccessibleInterface* spinAccessibleFactory(const QString& className, QObject* object) {
  Q_UNUSED(className)
  if (auto* spin = qobject_cast<AdSpin*>(object)) {
    return new AdSpinAccessible(spin);
  }
  return nullptr;
}

void ensureSpinAccessibleFactoryInstalled() {
  static const bool installed = []() {
    QAccessible::installFactory(spinAccessibleFactory);
    return true;
  }();
  Q_UNUSED(installed)
}

}  // namespace

namespace detail {

class SpinSurface final : public QWidget {
 public:
  explicit SpinSurface(AdSpin* owner, QWidget* parent, bool fullscreenMode = false)
      : QWidget(parent), owner_(owner), fullscreenMode_(fullscreenMode) {
    setObjectName(fullscreenMode ? QStringLiteral("spinFullscreenSurface")
                                 : QStringLiteral("spinSurface"));
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
  }

  bool fullscreenMode() const { return fullscreenMode_; }
  void setFullscreenMode(bool value) { fullscreenMode_ = value; }

 protected:
  void paintEvent(QPaintEvent*) override {
    if (!owner_) {
      return;
    }
    QPainter painter(this);
    owner_->paintSurface(this, &painter);
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    if (owner_) {
      owner_->layoutIndicator(this);
    }
  }

  bool event(QEvent* event) override {
    switch (event->type()) {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonRelease:
      case QEvent::MouseButtonDblClick:
      case QEvent::MouseMove:
      case QEvent::Wheel:
      case QEvent::ContextMenu:
      case QEvent::TouchBegin:
      case QEvent::TouchUpdate:
      case QEvent::TouchEnd:
        event->accept();
        return true;
      default:
        break;
    }
    return QWidget::event(event);
  }

 private:
  QPointer<AdSpin> owner_;
  bool fullscreenMode_ = false;
};

}  // namespace detail

struct AdSpin::Private {
  explicit Private(AdSpin* owner) : q(owner) {}

  AdSpin* q = nullptr;
  QPointer<QWidget> content;
  QPointer<QWidget> indicatorWidget;
  detail::SpinSurface* localSurface = nullptr;
  QPointer<detail::SpinSurface> fullscreenSurface;
  QPointer<QWidget> fullscreenHost;

  bool spinning = true;
  bool active = true;
  bool fullscreen = false;
  bool animationSubscribed = false;
  bool autoProgressScheduled = false;
  bool interactionFilterInstalled = false;
  int delayMs = 0;
  SizeClass sizeClass = SizeClass::Medium;
  QString description;
  ProgressMode progressMode = ProgressMode::None;
  qreal percent = 0.0;
  qreal autoPercent = 0.0;
  qint64 paintTimeMs = 0;

  ComponentTokens componentTokens;
  SemanticStyles semanticStyles;
  detail::SpinVisualStyle appearance;
};

AdSpin::AdSpin(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>(this)) {
  ensureSpinAccessibleFactoryInstalled();
  setObjectName(QStringLiteral("spin"));
  setAccessibleName(tr("Loading"));
  setProperty("busy", true);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  d_->localSurface = new detail::SpinSurface(this, this);
  connect(&theme::ThemeManager::instance(), &theme::ThemeManager::themeChanged, this,
          [this]() { refreshAppearance(); });
  refreshAppearance();
  refreshSurfaces();
}

AdSpin::AdSpin(QWidget* contentWidget, QWidget* parent) : AdSpin(parent) {
  setContentWidget(contentWidget);
}

AdSpin::~AdSpin() {
  if (d_->interactionFilterInstalled && qApp) {
    qApp->removeEventFilter(this);
    d_->interactionFilterInstalled = false;
  }
  detail::cancelTimingTask(this, QString::fromLatin1(kActivationTaskKey));
  cancelAutoProgressStep();
  detail::clearFrameSubscription(this, QString::fromLatin1(kAnimationFrameKey));
  d_->animationSubscribed = false;
  releaseFullscreenSurface();
  if (d_->content) {
    disconnect(d_->content, &QObject::destroyed, this, nullptr);
  }
  if (d_->indicatorWidget) {
    disconnect(d_->indicatorWidget, &QObject::destroyed, this, nullptr);
  }
}

bool AdSpin::spinning() const { return d_->spinning; }

void AdSpin::setSpinning(bool value) {
  if (d_->spinning == value) {
    return;
  }
  d_->spinning = value;
  emit spinningChanged(value);
  if (value) {
    scheduleActivation();
  } else {
    detail::cancelTimingTask(this, QString::fromLatin1(kActivationTaskKey));
    setActive(false);
  }
}

bool AdSpin::isActive() const { return d_->active; }

int AdSpin::delayMs() const { return d_->delayMs; }

void AdSpin::setDelayMs(int value) {
  value = std::max(0, value);
  if (d_->delayMs == value) {
    return;
  }
  d_->delayMs = value;
  if (d_->spinning && (!isVisible() || !d_->active)) {
    if (!isVisible()) {
      setActive(false);
    }
    scheduleActivation();
  }
  emit delayMsChanged(value);
}

AdSpin::SizeClass AdSpin::sizeClass() const { return d_->sizeClass; }

void AdSpin::setSizeClass(SizeClass value) {
  if (d_->sizeClass == value) {
    return;
  }
  d_->sizeClass = value;
  refreshAppearance();
  emit sizeClassChanged(value);
}

QString AdSpin::description() const { return d_->description; }

void AdSpin::setDescription(const QString& value) {
  if (d_->description == value) {
    return;
  }
  d_->description = value;
  setAccessibleDescription(value);
  QAccessibleEvent accessibilityEvent(this, QAccessible::DescriptionChanged);
  QAccessible::updateAccessibility(&accessibilityEvent);
  refreshAppearance();
  emit descriptionChanged(value);
}

bool AdSpin::fullscreen() const { return d_->fullscreen; }

void AdSpin::setFullscreen(bool value) {
  if (d_->fullscreen == value) {
    return;
  }
  d_->fullscreen = value;
  if (!value) {
    releaseFullscreenSurface();
  }
  refreshSurfaces();
  updateGeometry();
  emit fullscreenChanged(value);
}

AdSpin::ProgressMode AdSpin::progressMode() const { return d_->progressMode; }

void AdSpin::setProgressMode(ProgressMode mode) {
  if (d_->progressMode == mode) {
    return;
  }
  const qreal previousDisplayedPercent = displayedPercent();
  d_->progressMode = mode;
  if (mode == ProgressMode::Automatic) {
    d_->autoPercent = 0.0;
  }
  if (d_->indicatorWidget) {
    if (mode == ProgressMode::None) {
      d_->indicatorWidget->setProperty("percent", QVariant());
    } else {
      d_->indicatorWidget->setProperty("percent", displayedPercent());
    }
  }
  if (mode == ProgressMode::Automatic && d_->active && hasVisibleSurface()) {
    scheduleAutoProgressStep();
  } else {
    cancelAutoProgressStep();
  }
  refreshAnimationSubscription();
  if (d_->localSurface) {
    d_->localSurface->update();
  }
  if (d_->fullscreenSurface) {
    d_->fullscreenSurface->update();
  }
  emit progressModeChanged(mode);
  if (!qFuzzyCompare(previousDisplayedPercent + 1.0, displayedPercent() + 1.0)) {
    emit displayedPercentChanged(displayedPercent());
  }
  notifyAccessibleValueChange();
}

qreal AdSpin::percent() const { return d_->percent; }

void AdSpin::setPercent(qreal value) {
  if (!std::isfinite(value)) {
    value = 0.0;
  }
  value = std::clamp(value, 0.0, 100.0);
  if (qFuzzyCompare(d_->percent + 1.0, value + 1.0) &&
      d_->progressMode == ProgressMode::Determinate) {
    return;
  }
  const bool modeChanged = d_->progressMode != ProgressMode::Determinate;
  const qreal previousDisplayedPercent = displayedPercent();
  d_->percent = value;
  d_->progressMode = ProgressMode::Determinate;
  cancelAutoProgressStep();
  if (d_->indicatorWidget) {
    d_->indicatorWidget->setProperty("percent", value);
  }
  if (modeChanged) {
    emit progressModeChanged(d_->progressMode);
  }
  emit percentChanged(value);
  if (!qFuzzyCompare(previousDisplayedPercent + 1.0, displayedPercent() + 1.0)) {
    emit displayedPercentChanged(displayedPercent());
  }
  notifyAccessibleValueChange();
  if (d_->localSurface) {
    d_->localSurface->update();
  }
  if (d_->fullscreenSurface) {
    d_->fullscreenSurface->update();
  }
}

qreal AdSpin::displayedPercent() const {
  if (d_->progressMode == ProgressMode::Automatic) {
    return d_->autoPercent;
  }
  return d_->progressMode == ProgressMode::Determinate ? d_->percent : 0.0;
}

void AdSpin::setAutoProgress(bool enabled) {
  setProgressMode(enabled ? ProgressMode::Automatic : ProgressMode::None);
}

void AdSpin::clearProgress() { setProgressMode(ProgressMode::None); }

QWidget* AdSpin::contentWidget() const { return d_->content.data(); }

void AdSpin::setContentWidget(QWidget* widget) {
  if (d_->content == widget || !canAdoptWidget(widget) || widget == d_->indicatorWidget) {
    return;
  }
  if (d_->content) {
    QWidget* previous = d_->content.data();
    d_->content = nullptr;
    disconnect(previous, &QObject::destroyed, this, nullptr);
    delete previous;
  }
  d_->content = widget;
  if (widget) {
    widget->setParent(this);
    widget->show();
    connect(widget, &QObject::destroyed, this, [this, widget]() {
      if (!d_->content || d_->content.data() == widget) {
        d_->content = nullptr;
        refreshSurfaces();
        updateGeometry();
        emit contentWidgetChanged(nullptr);
      }
    });
  }
  layoutChildren();
  refreshSurfaces();
  updateGeometry();
  emit contentWidgetChanged(widget);
}

QWidget* AdSpin::takeContentWidget() {
  QWidget* result = d_->content.data();
  if (!result) {
    return nullptr;
  }
  d_->content = nullptr;
  disconnect(result, &QObject::destroyed, this, nullptr);
  result->hide();
  result->setParent(nullptr);
  refreshSurfaces();
  updateGeometry();
  emit contentWidgetChanged(nullptr);
  return result;
}

QWidget* AdSpin::indicatorWidget() const { return d_->indicatorWidget.data(); }

void AdSpin::setIndicatorWidget(QWidget* widget) {
  if (d_->indicatorWidget == widget || !canAdoptWidget(widget) || widget == d_->content) {
    return;
  }
  if (d_->indicatorWidget) {
    QWidget* previous = d_->indicatorWidget.data();
    d_->indicatorWidget = nullptr;
    disconnect(previous, &QObject::destroyed, this, nullptr);
    delete previous;
  }
  d_->indicatorWidget = widget;
  if (widget) {
    QWidget* surface = d_->fullscreenSurface && d_->fullscreenSurface->isVisible()
                           ? static_cast<QWidget*>(d_->fullscreenSurface.data())
                           : static_cast<QWidget*>(d_->localSurface);
    widget->setParent(surface);
    widget->setProperty("percent", displayedPercent());
    widget->show();
    connect(widget, &QObject::destroyed, this, [this, widget]() {
      if (!d_->indicatorWidget || d_->indicatorWidget.data() == widget) {
        d_->indicatorWidget = nullptr;
        refreshAnimationSubscription();
        updateGeometry();
        emit indicatorWidgetChanged(nullptr);
      }
    });
  }
  layoutIndicator(d_->localSurface);
  refreshAnimationSubscription();
  updateGeometry();
  emit indicatorWidgetChanged(widget);
}

QWidget* AdSpin::takeIndicatorWidget() {
  QWidget* result = d_->indicatorWidget.data();
  if (!result) {
    return nullptr;
  }
  d_->indicatorWidget = nullptr;
  disconnect(result, &QObject::destroyed, this, nullptr);
  result->hide();
  result->setParent(nullptr);
  refreshAnimationSubscription();
  updateGeometry();
  emit indicatorWidgetChanged(nullptr);
  return result;
}

AdSpin::ComponentTokens AdSpin::componentTokens() const { return d_->componentTokens; }

void AdSpin::setComponentTokens(const ComponentTokens& tokens) {
  d_->componentTokens = tokens;
  refreshAppearance();
  emit componentTokensChanged();
}

void AdSpin::resetComponentTokens() {
  d_->componentTokens = {};
  refreshAppearance();
  emit componentTokensChanged();
}

AdSpin::SemanticStyles AdSpin::semanticStyles() const { return d_->semanticStyles; }

void AdSpin::setSemanticStyles(const SemanticStyles& styles) {
  d_->semanticStyles = styles;
  refreshAppearance();
  emit semanticStylesChanged();
}

void AdSpin::resetSemanticStyles() {
  d_->semanticStyles = {};
  refreshAppearance();
  emit semanticStylesChanged();
}

QSize AdSpin::sizeHint() const {
  if (d_->content) {
    return d_->content->sizeHint();
  }
  if (d_->fullscreen) {
    return QSize(0, 0);
  }
  const QSize indicatorSize = naturalIndicatorSize(d_->indicatorWidget, d_->appearance.dotSize);
  const QFontMetrics metrics(d_->appearance.font);
  const int textWidth = d_->description.isEmpty() ? 0 : metrics.horizontalAdvance(d_->description);
  const int width = std::max(indicatorSize.width(), textWidth);
  const int height =
      indicatorSize.height() +
      (d_->description.isEmpty() ? 0 : d_->appearance.descriptionGap + metrics.height());
  return QSize(std::max(1, width), std::max(1, height));
}

QSize AdSpin::minimumSizeHint() const {
  return d_->content ? d_->content->minimumSizeHint() : sizeHint();
}

bool AdSpin::event(QEvent* event) {
  const bool result = QWidget::event(event);
  switch (event->type()) {
    case QEvent::Show:
      layoutChildren();
      refreshSurfaces();
      break;
    case QEvent::Hide:
    case QEvent::ParentChange:
    case QEvent::WindowStateChange:
      refreshSurfaces();
      break;
    default:
      break;
  }
  return result;
}

bool AdSpin::eventFilter(QObject* watched, QEvent* event) {
  if (watched == d_->fullscreenHost) {
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::LayoutRequest:
      case QEvent::WindowStateChange:
      case QEvent::ChildAdded:
        if (d_->fullscreenSurface && d_->fullscreenHost) {
          d_->fullscreenSurface->setGeometry(d_->fullscreenHost->rect());
          d_->fullscreenSurface->raise();
        }
        break;
      case QEvent::Hide:
        if (d_->fullscreenSurface) {
          d_->fullscreenSurface->hide();
        }
        cancelAutoProgressStep();
        refreshAnimationSubscription();
        syncInteractionFilter();
        break;
      default:
        break;
    }
    if (event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange) {
      refreshSurfaces();
    }
  }
  if (d_->interactionFilterInstalled && isInteractionBlockedTarget(watched)) {
    if (event->type() == QEvent::FocusIn) {
      if (auto* widget = qobject_cast<QWidget*>(watched)) {
        widget->clearFocus();
      }
      return true;
    }
    if (isBlockedInputEvent(event->type())) {
      event->accept();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void AdSpin::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  layoutChildren();
}

void AdSpin::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
      refreshAppearance();
      break;
    case QEvent::LanguageChange:
      setAccessibleName(tr("Loading"));
      {
        QAccessibleEvent accessibilityEvent(this, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&accessibilityEvent);
      }
      break;
    default:
      break;
  }
}

void AdSpin::setActive(bool value) {
  if (d_->active == value) {
    return;
  }
  d_->active = value;
  const qreal previousDisplayedPercent = displayedPercent();
  if (value && d_->progressMode == ProgressMode::Automatic) {
    d_->autoPercent = 0.0;
    if (hasVisibleSurface()) {
      scheduleAutoProgressStep();
    }
  } else if (!value) {
    cancelAutoProgressStep();
  }
  setProperty("busy", value);
  refreshSurfaces();
  emit activeChanged(value);
  QAccessible::State changedState;
  changedState.busy = true;
  changedState.invisible = true;
  QAccessibleStateChangeEvent accessibilityEvent(this, changedState);
  QAccessible::updateAccessibility(&accessibilityEvent);
  if (!qFuzzyCompare(previousDisplayedPercent + 1.0, displayedPercent() + 1.0)) {
    emit displayedPercentChanged(displayedPercent());
    notifyAccessibleValueChange();
  }
}

void AdSpin::scheduleActivation() {
  detail::cancelTimingTask(this, QString::fromLatin1(kActivationTaskKey));
  if (d_->delayMs <= 0) {
    setActive(true);
    return;
  }
  detail::scheduleTimingTask(this, QString::fromLatin1(kActivationTaskKey), d_->delayMs, [this]() {
    if (d_->spinning) {
      setActive(true);
    }
  });
}

void AdSpin::refreshAppearance() {
  detail::SpinStyleInput input;
  input.sizeClass = d_->sizeClass;
  input.baseFont = font();
  input.componentTokens = d_->componentTokens;
  input.semanticStyles = d_->semanticStyles;
  d_->appearance =
      detail::resolveSpinVisualStyle(input, theme::ThemeManager::instance().resolve(this));
  if (d_->progressMode == ProgressMode::Automatic && d_->active && hasVisibleSurface()) {
    cancelAutoProgressStep();
    scheduleAutoProgressStep();
  }
  d_->paintTimeMs = detail::timingNowMs();
  layoutChildren();
  updateGeometry();
  if (d_->localSurface) {
    d_->localSurface->update();
  }
  if (d_->fullscreenSurface) {
    d_->fullscreenSurface->update();
  }
  refreshAnimationSubscription();
}

void AdSpin::refreshSurfaces() {
  const bool shouldDisplay = d_->active && isVisible();
  if (d_->fullscreen && shouldDisplay) {
    ensureFullscreenSurface();
  }
  const bool proxyFullscreen = d_->fullscreen && d_->fullscreenSurface;
  if (d_->localSurface) {
    d_->localSurface->setFullscreenMode(d_->fullscreen && !proxyFullscreen);
    d_->localSurface->setVisible(shouldDisplay && !proxyFullscreen);
    if (d_->localSurface->isVisible()) {
      d_->localSurface->raise();
    }
  }
  if (d_->fullscreenSurface) {
    d_->fullscreenSurface->setVisible(shouldDisplay && d_->fullscreen);
    if (d_->fullscreenSurface->isVisible()) {
      d_->fullscreenSurface->raise();
    }
  }
  QWidget* indicatorParent = d_->fullscreenSurface && d_->fullscreenSurface->isVisible()
                                 ? static_cast<QWidget*>(d_->fullscreenSurface.data())
                                 : static_cast<QWidget*>(d_->localSurface);
  if (d_->indicatorWidget && indicatorParent &&
      d_->indicatorWidget->parentWidget() != indicatorParent) {
    d_->indicatorWidget->setParent(indicatorParent);
    d_->indicatorWidget->show();
  }
  if (auto* surface = dynamic_cast<detail::SpinSurface*>(indicatorParent)) {
    layoutIndicator(surface);
  }
  if (d_->progressMode == ProgressMode::Automatic && d_->active && hasVisibleSurface()) {
    scheduleAutoProgressStep();
  } else {
    cancelAutoProgressStep();
  }
  syncInteractionFilter();
  refreshAnimationSubscription();
}

void AdSpin::refreshAnimationSubscription() {
  const bool surfaceVisible = (d_->localSurface && d_->localSurface->isVisible()) ||
                              (d_->fullscreenSurface && d_->fullscreenSurface->isVisible());
  const bool requiresFrames = d_->active && surfaceVisible && !d_->indicatorWidget &&
                              d_->progressMode == ProgressMode::None;
  const bool shouldSubscribe =
      requiresFrames && d_->appearance.animationCycleMs > 0 && detail::spinnerCycleDurationMs() > 0;
  if (shouldSubscribe && !d_->animationSubscribed) {
    detail::setFrameSubscription(this, QString::fromLatin1(kAnimationFrameKey), true,
                                 [this](qint64 nowMs, qint64) { animationFrame(nowMs); });
    d_->animationSubscribed = true;
  } else if (!shouldSubscribe && d_->animationSubscribed) {
    detail::clearFrameSubscription(this, QString::fromLatin1(kAnimationFrameKey));
    d_->animationSubscribed = false;
  }
}

void AdSpin::scheduleAutoProgressStep() {
  if (d_->autoProgressScheduled || !d_->active || d_->progressMode != ProgressMode::Automatic ||
      !hasVisibleSurface()) {
    return;
  }
  d_->autoProgressScheduled = true;
  detail::scheduleTimingTask(
      this, QString::fromLatin1(kAutoProgressTaskKey), d_->appearance.autoProgressIntervalMs,
      [this]() {
        d_->autoProgressScheduled = false;
        if (!d_->active || d_->progressMode != ProgressMode::Automatic || !hasVisibleSurface()) {
          return;
        }
        const qreal previousPercent = d_->autoPercent;
        const qreal rest = 100.0 - d_->autoPercent;
        if (d_->autoPercent <= 30.0) {
          d_->autoPercent += rest * 0.05;
        } else if (d_->autoPercent <= 70.0) {
          d_->autoPercent += rest * 0.03;
        } else if (d_->autoPercent <= 96.0) {
          d_->autoPercent += rest * 0.01;
        } else {
          d_->autoPercent = 99.0;
        }
        if (!qFuzzyCompare(previousPercent + 1.0, d_->autoPercent + 1.0)) {
          if (d_->indicatorWidget) {
            d_->indicatorWidget->setProperty("percent", d_->autoPercent);
          }
          emit displayedPercentChanged(d_->autoPercent);
          notifyAccessibleValueChange();
          if (d_->localSurface) {
            d_->localSurface->update();
          }
          if (d_->fullscreenSurface) {
            d_->fullscreenSurface->update();
          }
        }
        if (d_->autoPercent < 99.0) {
          scheduleAutoProgressStep();
        }
      });
}

void AdSpin::cancelAutoProgressStep() {
  detail::cancelTimingTask(this, QString::fromLatin1(kAutoProgressTaskKey));
  d_->autoProgressScheduled = false;
}

bool AdSpin::hasVisibleSurface() const {
  return (d_->localSurface && d_->localSurface->isVisible()) ||
         (d_->fullscreenSurface && d_->fullscreenSurface->isVisible());
}

bool AdSpin::canAdoptWidget(const QWidget* widget) const {
  if (!widget) {
    return true;
  }
  return widget != this && widget != d_->localSurface && widget != d_->fullscreenSurface &&
         !widget->isAncestorOf(this);
}

bool AdSpin::isInteractionBlockedTarget(const QObject* object) const {
  const QObject* candidate = object;
  const QWidget* widget = nullptr;
  while (candidate && !widget) {
    widget = qobject_cast<const QWidget*>(candidate);
    candidate = candidate->parent();
  }
  if (!widget || !d_->active || !hasVisibleSurface()) {
    return false;
  }
  if (d_->fullscreen && d_->fullscreenSurface && d_->fullscreenSurface->isVisible() &&
      d_->fullscreenHost) {
    return widget == d_->fullscreenHost || d_->fullscreenHost->isAncestorOf(widget);
  }
  return d_->content && d_->localSurface && d_->localSurface->isVisible() &&
         (widget == d_->content || d_->content->isAncestorOf(widget));
}

void AdSpin::syncInteractionFilter() {
  const bool shouldInstall = qApp && d_->active && hasVisibleSurface() &&
                             ((d_->fullscreen && d_->fullscreenHost) || d_->content);
  if (shouldInstall && !d_->interactionFilterInstalled) {
    qApp->installEventFilter(this);
    d_->interactionFilterInstalled = true;
  } else if (!shouldInstall && d_->interactionFilterInstalled) {
    qApp->removeEventFilter(this);
    d_->interactionFilterInstalled = false;
  }
  if (d_->interactionFilterInstalled) {
    if (QWidget* focused = QApplication::focusWidget(); isInteractionBlockedTarget(focused)) {
      focused->clearFocus();
    }
  }
}

void AdSpin::notifyAccessibleValueChange() {
  const QVariant value =
      d_->progressMode == ProgressMode::None ? QVariant() : QVariant(displayedPercent());
  QAccessibleValueChangeEvent event(this, value);
  QAccessible::updateAccessibility(&event);
}

void AdSpin::ensureFullscreenSurface() {
  QWidget* host = window();
  if (!host || host == this) {
    releaseFullscreenSurface();
    return;
  }
  if (d_->fullscreenSurface && d_->fullscreenHost == host) {
    d_->fullscreenSurface->setGeometry(host->rect());
    return;
  }
  releaseFullscreenSurface();
  d_->fullscreenHost = host;
  host->installEventFilter(this);
  d_->fullscreenSurface = new detail::SpinSurface(this, host, true);
  d_->fullscreenSurface->setGeometry(host->rect());
  d_->fullscreenSurface->show();
  d_->fullscreenSurface->raise();
}

void AdSpin::releaseFullscreenSurface() {
  if (d_->fullscreenHost) {
    d_->fullscreenHost->removeEventFilter(this);
  }
  if (d_->indicatorWidget && d_->fullscreenSurface &&
      d_->indicatorWidget->parentWidget() == d_->fullscreenSurface) {
    d_->indicatorWidget->setParent(d_->localSurface);
    d_->indicatorWidget->show();
  }
  if (d_->fullscreenSurface) {
    delete d_->fullscreenSurface.data();
  }
  d_->fullscreenSurface = nullptr;
  d_->fullscreenHost = nullptr;
}

void AdSpin::layoutChildren() {
  if (d_->content) {
    d_->content->setGeometry(rect());
    d_->content->lower();
  }
  if (d_->localSurface) {
    d_->localSurface->setGeometry(rect());
    d_->localSurface->raise();
    layoutIndicator(d_->localSurface);
  }
  if (d_->fullscreenSurface && d_->fullscreenHost) {
    d_->fullscreenSurface->setGeometry(d_->fullscreenHost->rect());
    layoutIndicator(d_->fullscreenSurface);
  }
}

void AdSpin::layoutIndicator(detail::SpinSurface* surface) {
  if (!surface || !d_->indicatorWidget || d_->indicatorWidget->parentWidget() != surface) {
    return;
  }
  const SectionLayout layout =
      sectionLayoutFor(surface->rect(), d_->indicatorWidget, d_->appearance.dotSize,
                       d_->description, d_->appearance.font, d_->appearance.descriptionGap);
  d_->indicatorWidget->setGeometry(layout.indicatorRect.toAlignedRect());
  d_->indicatorWidget->raise();
}

void AdSpin::paintSurface(detail::SpinSurface* surface, QPainter* painter) {
  if (!surface || !painter || !d_->active) {
    return;
  }
  painter->setRenderHint(QPainter::Antialiasing, true);
  const bool fullscreenMode = surface->fullscreenMode();
  if (fullscreenMode) {
    painter->fillRect(surface->rect(), d_->appearance.fullscreenMask);
  } else if (d_->content) {
    painter->fillRect(surface->rect(), d_->appearance.containerOverlay);
  } else if (d_->appearance.hasRootBackground) {
    painter->fillRect(surface->rect(), d_->appearance.rootBackground);
  }
  const SectionLayout layout =
      sectionLayoutFor(surface->rect(), d_->indicatorWidget, d_->appearance.dotSize,
                       d_->description, d_->appearance.font, d_->appearance.descriptionGap);
  if (d_->appearance.hasSectionBackground) {
    QRectF sectionRect = layout.indicatorRect;
    if (layout.descriptionRect.isValid()) {
      sectionRect = sectionRect.united(layout.descriptionRect);
    }
    painter->fillRect(sectionRect, d_->appearance.sectionBackground);
  }
  if (d_->appearance.hasIndicatorBackground) {
    painter->fillRect(layout.indicatorRect, d_->appearance.indicatorBackground);
  }
  if (d_->appearance.hasDescriptionBackground && layout.descriptionRect.isValid()) {
    painter->fillRect(layout.descriptionRect, d_->appearance.descriptionBackground);
  }
  const QColor indicatorColor =
      fullscreenMode ? d_->appearance.fullscreenIndicator : d_->appearance.indicator;
  const QColor descriptionColor =
      fullscreenMode ? d_->appearance.fullscreenDescription : d_->appearance.description;

  if (!d_->indicatorWidget) {
    const qreal rawPercent = displayedPercent();
    const bool showProgress = d_->progressMode != ProgressMode::None;
    if (showProgress) {
      const qreal safePercent = std::clamp(rawPercent, 0.0, 100.0);
      const qreal side = std::min(layout.indicatorRect.width(), layout.indicatorRect.height());
      const qreal strokeWidth = std::max<qreal>(1.0, side / 5.0);
      const QRectF circleRect(layout.indicatorRect.center().x() - side / 2.0 + strokeWidth / 2.0,
                              layout.indicatorRect.center().y() - side / 2.0 + strokeWidth / 2.0,
                              side - strokeWidth, side - strokeWidth);
      painter->setBrush(Qt::NoBrush);
      painter->setPen(QPen(d_->appearance.progressTrack, strokeWidth, Qt::SolidLine, Qt::RoundCap));
      painter->drawArc(circleRect, 90 * 16, -360 * 16);
      painter->setPen(QPen(indicatorColor, strokeWidth, Qt::SolidLine, Qt::RoundCap));
      painter->drawArc(circleRect, 90 * 16, qRound(-360.0 * safePercent / 100.0 * 16.0));
    } else {
      const qreal side = std::min(layout.indicatorRect.width(), layout.indicatorRect.height());
      const qreal spacing = std::max<qreal>(1.0, side * 0.025);
      const qreal itemSide = ((side - spacing * 2.0) / 2.0) * 0.75;
      // At 45 degrees the dot centers reach their maximum axis distance.
      // Keep the entire circular orbit, including each dot's radius, inside
      // the advertised indicator bounds at every animation angle.
      const qreal centerOffset = std::min(side / 4.0, (side - itemSide) / (2.0 * std::sqrt(2.0)));
      const qreal inset = side / 2.0 - centerOffset - itemSide / 2.0;
      const QPointF center = layout.indicatorRect.center();
      const qreal rotationPeriod = std::max(1, d_->appearance.animationCycleMs);
      const bool motionEnabled =
          d_->appearance.animationCycleMs > 0 && detail::spinnerCycleDurationMs() > 0;
      const qreal angle =
          motionEnabled ? 45.0 + std::fmod(static_cast<qreal>(d_->paintTimeMs), rotationPeriod) /
                                     rotationPeriod * 360.0
                        : 45.0;
      painter->save();
      painter->translate(center);
      painter->rotate(angle);
      painter->translate(-center);
      const QPointF origins[4] = {
          QPointF(layout.indicatorRect.left() + inset, layout.indicatorRect.top() + inset),
          QPointF(layout.indicatorRect.right() - inset - itemSide,
                  layout.indicatorRect.top() + inset),
          QPointF(layout.indicatorRect.right() - inset - itemSide,
                  layout.indicatorRect.bottom() - inset - itemSide),
          QPointF(layout.indicatorRect.left() + inset,
                  layout.indicatorRect.bottom() - inset - itemSide),
      };
      for (int i = 0; i < 4; ++i) {
        const qreal pulse =
            motionEnabled ? triangleWave((static_cast<qreal>(d_->paintTimeMs) - i * 400.0) / 1000.0)
                          : static_cast<qreal>(i) / 3.0;
        QColor dotColor = indicatorColor;
        dotColor.setAlphaF(static_cast<float>(dotColor.alphaF() * (0.3 + pulse * 0.7)));
        painter->setPen(Qt::NoPen);
        painter->setBrush(dotColor);
        painter->drawEllipse(QRectF(origins[i], QSizeF(itemSide, itemSide)));
      }
      painter->restore();
    }
  }

  if (!d_->description.isEmpty() && layout.descriptionRect.isValid()) {
    painter->setFont(d_->appearance.font);
    painter->setPen(descriptionColor);
    painter->drawText(layout.descriptionRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                      d_->description);
  }
}

void AdSpin::animationFrame(qint64 nowMs) {
  d_->paintTimeMs = nowMs;
  if (d_->localSurface && d_->localSurface->isVisible()) {
    d_->localSurface->update();
  }
  if (d_->fullscreenSurface && d_->fullscreenSurface->isVisible()) {
    d_->fullscreenSurface->update();
  }
}

}  // namespace adqt::widgets
