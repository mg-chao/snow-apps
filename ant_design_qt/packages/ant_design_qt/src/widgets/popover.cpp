#include "popover.h"

#include "detail/overlay_accessibility.h"
#include "detail/overlay_popup_surface.h"
#include "popover_style.h"
#include "popup_placement.h"
#include "theme/theme.h"

#include <QEvent>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QPixmap>
#include <QSet>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace adqt::widgets::detail {

namespace {

void clearLayoutItems(QLayout* layout) {
  if (!layout) {
    return;
  }
  while (QLayoutItem* item = layout->takeAt(0)) {
    delete item;
  }
}

}  // namespace

class PopoverContentView final : public QWidget {
 public:
  explicit PopoverContentView(QWidget* parent = nullptr) : QWidget(parent) {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);
    rootLayout->setSizeConstraint(QLayout::SetMinimumSize);
    rootLayout_ = rootLayout;

    auto* titleHost = new QWidget(this);
    auto* titleLayout = new QVBoxLayout(titleHost);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);
    titleHost_ = titleHost;
    titleLayout_ = titleLayout;

    auto* titleLabel = new QLabel(titleHost);
    titleLabel->setWordWrap(true);
    titleLabel->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    titleLabel_ = titleLabel;

    auto* contentHost = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(contentHost);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentHost_ = contentHost;
    contentLayout_ = contentLayout;

    auto* textLabel = new QLabel(contentHost);
    textLabel->setWordWrap(true);
    textLabel->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    textLabel_ = textLabel;

    rootLayout_->addWidget(titleHost_);
    rootLayout_->addWidget(contentHost_);

    rebuildSections();
  }

  QSize sizeHint() const override { return layoutHint(false); }

  QSize minimumSizeHint() const override { return layoutHint(true); }

  void setTitle(const QString& value) {
    if (title_ == value) {
      return;
    }
    title_ = value;
    if (titleLabel_) {
      titleLabel_->setText(title_);
    }
    rebuildSections();
  }

  void setText(const QString& value) {
    if (text_ == value) {
      return;
    }
    text_ = value;
    if (textLabel_) {
      textLabel_->setText(text_);
    }
    rebuildSections();
  }

  void setTitleWidget(QWidget* widget) {
    if (titleWidget_ == widget) {
      rebuildSections();
      return;
    }
    if (titleWidget_ && titleWidget_ != widget) {
      titleWidget_->hide();
    }
    titleWidget_ = widget;
    rebuildSections();
  }

  void setContentWidget(QWidget* widget) {
    if (contentWidget_ == widget) {
      rebuildSections();
      return;
    }
    if (contentWidget_ && contentWidget_ != widget) {
      contentWidget_->hide();
    }
    contentWidget_ = widget;
    rebuildSections();
  }

  void setTitleFont(const QFont& value) {
    if (!titleLabel_) {
      return;
    }
    titleLabel_->setFont(value);
  }

  void setTextFont(const QFont& value) {
    if (!textLabel_) {
      return;
    }
    textLabel_->setFont(value);
  }

  void setTitleColor(const QColor& value) {
    if (!titleLabel_) {
      return;
    }
    QPalette palette = titleLabel_->palette();
    palette.setColor(QPalette::WindowText, value);
    titleLabel_->setPalette(palette);
  }

  void setTextColor(const QColor& value) {
    if (!textLabel_) {
      return;
    }
    QPalette palette = textLabel_->palette();
    palette.setColor(QPalette::WindowText, value);
    textLabel_->setPalette(palette);
  }

  void setContentMargins(const QMargins& value) {
    if (!rootLayout_) {
      return;
    }
    rootLayout_->setContentsMargins(value);
  }

  void setSectionSpacing(int value) {
    sectionSpacing_ = std::max(0, value);
    updateSpacing();
  }

  void setTitleMinimumWidth(int value) {
    titleMinimumWidth_ = std::max(0, value);
    if (titleHost_) {
      titleHost_->setMinimumWidth(titleMinimumWidth_);
    }
    setMinimumWidth(titleMinimumWidth_);
  }

  bool hasContent() const {
    const bool hasTitle = titleWidget_ || !title_.trimmed().isEmpty();
    const bool hasText = contentWidget_ || !text_.trimmed().isEmpty();
    return hasTitle || hasText;
  }

 private:
  QSize layoutHint(bool minimum) const {
    QSize hint;
    if (rootLayout_) {
      hint = minimum ? rootLayout_->minimumSize() : rootLayout_->sizeHint();
    }
    if (!hint.isValid()) {
      hint = minimum ? QWidget::minimumSizeHint() : QWidget::sizeHint();
    }
    if (titleMinimumWidth_ > 0) {
      hint.setWidth(std::max(hint.width(), titleMinimumWidth_));
    }
    return hint;
  }

  void rebuildSections() {
    const bool hasTitleWidget = !titleWidget_.isNull();
    const bool hasTitleText = !title_.trimmed().isEmpty();
    const bool hasTextWidget = !contentWidget_.isNull();
    const bool hasTextText = !text_.trimmed().isEmpty();

    if (titleLayout_) {
      clearLayoutItems(titleLayout_);
      if (hasTitleWidget) {
        if (titleWidget_->parentWidget() != titleHost_) {
          titleWidget_->setParent(titleHost_);
        }
        titleLayout_->addWidget(titleWidget_);
        titleWidget_->show();
      } else if (titleLabel_) {
        titleLabel_->setText(title_);
        titleLabel_->setVisible(hasTitleText);
        titleLayout_->addWidget(titleLabel_);
      }
    }

    if (contentLayout_) {
      clearLayoutItems(contentLayout_);
      if (hasTextWidget) {
        if (contentWidget_->parentWidget() != contentHost_) {
          contentWidget_->setParent(contentHost_);
        }
        contentLayout_->addWidget(contentWidget_);
        contentWidget_->show();
      } else if (textLabel_) {
        textLabel_->setText(text_);
        textLabel_->setVisible(hasTextText);
        contentLayout_->addWidget(textLabel_);
      }
    }

    if (titleHost_) {
      titleHost_->setVisible(hasTitleWidget || hasTitleText);
      titleHost_->setMinimumWidth(titleMinimumWidth_);
    }
    if (contentHost_) {
      contentHost_->setVisible(hasTextWidget || hasTextText);
    }

    setMinimumWidth(titleMinimumWidth_);
    if (titleLayout_) {
      titleLayout_->invalidate();
    }
    if (contentLayout_) {
      contentLayout_->invalidate();
    }
    if (rootLayout_) {
      rootLayout_->invalidate();
      rootLayout_->activate();
    }
    updateSpacing();
    updateGeometry();
  }

  void updateSpacing() {
    if (!rootLayout_) {
      return;
    }
    const bool showGap =
        titleHost_ && titleHost_->isVisible() && contentHost_ && contentHost_->isVisible();
    rootLayout_->setSpacing(showGap ? sectionSpacing_ : 0);
  }

  QString title_;
  QString text_;
  int sectionSpacing_ = 8;
  int titleMinimumWidth_ = 0;

  QPointer<QVBoxLayout> rootLayout_;
  QPointer<QWidget> titleHost_;
  QPointer<QVBoxLayout> titleLayout_;
  QPointer<QLabel> titleLabel_;
  QPointer<QWidget> contentHost_;
  QPointer<QVBoxLayout> contentLayout_;
  QPointer<QLabel> textLabel_;
  QPointer<QWidget> titleWidget_;
  QPointer<QWidget> contentWidget_;
};

}  // namespace adqt::widgets::detail

namespace adqt::widgets {

namespace {

void syncGeometryChain(QWidget* widget, QWidget* stopInclusive = nullptr) {
  for (QWidget* current = widget; current; current = current->parentWidget()) {
    if (QLayout* layout = current->layout()) {
      layout->invalidate();
      layout->activate();
    }
    current->updateGeometry();
    current->adjustSize();
    if (current == stopInclusive) {
      break;
    }
  }
}

bool isVisualRefreshEvent(QEvent::Type type) {
  return type == QEvent::FontChange || type == QEvent::PaletteChange ||
         type == QEvent::StyleChange || type == QEvent::ApplicationFontChange ||
         type == QEvent::ApplicationPaletteChange;
}

QString firstNonEmpty(std::initializer_list<QString> values) {
  for (const QString& value : values) {
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty()) {
      return trimmed;
    }
  }
  return QString();
}

detail::PopoverStyleOverrides makePopoverStyleOverrides(
    const std::optional<QFont>& titleFontOverride, const std::optional<QFont>& textFontOverride,
    const std::optional<QColor>& backgroundColorOverride,
    const std::optional<QColor>& borderColorOverride,
    const std::optional<QColor>& titleColorOverride, const std::optional<QColor>& textColorOverride,
    const std::optional<QMargins>& contentMarginsOverride,
    const std::optional<int>& titleMinimumWidthOverride,
    const std::optional<int>& maximumWidthOverride, const std::optional<int>& zIndexOverride,
    const std::optional<int>& cornerRadiusOverride, const std::optional<int>& borderWidthOverride,
    const std::optional<int>& arrowSizeOverride, const std::optional<int>& popupOffsetOverride) {
  detail::PopoverStyleOverrides overrides;
  overrides.titleFont = titleFontOverride;
  overrides.textFont = textFontOverride;
  overrides.backgroundColor = backgroundColorOverride;
  overrides.borderColor = borderColorOverride;
  overrides.titleColor = titleColorOverride;
  overrides.textColor = textColorOverride;
  overrides.contentMargins = contentMarginsOverride;
  overrides.titleMinimumWidth = titleMinimumWidthOverride;
  overrides.maximumWidth = maximumWidthOverride;
  overrides.zIndex = zIndexOverride;
  overrides.cornerRadius = cornerRadiusOverride;
  overrides.borderWidth = borderWidthOverride;
  overrides.arrowSize = arrowSizeOverride;
  overrides.popupOffset = popupOffsetOverride;
  return overrides;
}

detail::PopoverVisualStyle resolvePopoverStyleSnapshot(
    AdPopover::Placement placement, bool visible, bool disabled, bool arrowVisible,
    const QFont& baseFont, const detail::PopoverStyleOverrides& overrides,
    const QWidget* themeOwner) {
  detail::PopoverStyleInput input;
  input.placement = placement;
  input.visible = visible;
  input.disabled = disabled;
  input.arrowVisible = arrowVisible;
  input.baseFont = baseFont;
  input.overrides = overrides;

  if (!themeOwner) {
    return detail::resolvePopoverVisualStyle(input);
  }

  const adqt::theme::ResolvedTheme resolvedTheme =
      adqt::theme::ThemeManager::instance().resolve(themeOwner, themeOwner);
  return detail::resolvePopoverVisualStyle(input, resolvedTheme);
}

}  // namespace

AdPopover::AdPopover(QObject* parent) : QObject(parent) {
  controller_ = new detail::OverlayPopupController(this, this);
  connect(controller_, &detail::OverlayPopupController::popupVisibleChanged, this,
          &AdPopover::handleControllerPopupVisibleChanged);
  connect(controller_, &detail::OverlayPopupController::popupVisibilityRequested, this,
          &AdPopover::visibilityRequested);
  syncControllerConfiguration();
}

AdPopover::~AdPopover() {
  detail::clearDerivedAccessibleDescription(sourceWidget_);
  clearObservedWidgets();
  releasePopupSurface();
  deleteOwnedWidget(titleWidget_, titleWidgetDestroyedConnection_);
  deleteOwnedWidget(contentWidget_, contentWidgetDestroyedConnection_);
  if (ownedWidgetParkingRoot_) {
    ownedWidgetParkingRoot_->hide();
    ownedWidgetParkingRoot_->deleteLater();
    ownedWidgetParkingRoot_.clear();
  }
}

void AdPopover::resetSyncPopupGeometryCountersForTesting() {
  detail::OverlayPopupController::resetSyncPopupGeometryCountersForTesting();
}

qint64 AdPopover::syncPopupGeometryCallCountForTesting() {
  return detail::OverlayPopupController::syncPopupGeometryCallCountForTesting();
}

qint64 AdPopover::syncPopupGeometryShortCircuitCountForTesting() {
  return detail::OverlayPopupController::syncPopupGeometryShortCircuitCountForTesting();
}

void AdPopover::setPlacement(Placement value) {
  if (placement_ == value) {
    return;
  }
  placement_ = value;
  emit placementChanged(placement_);
  updateStyle();
  refreshVisiblePopup();
}

void AdPopover::setTriggers(Triggers value) {
  if (triggers_ == value) {
    return;
  }
  triggers_ = value;
  if (controller_) {
    controller_->setTriggerModes(toOverlayTriggers(triggers_));
  }
  emit triggersChanged(triggers_);
}

bool AdPopover::isVisible() const { return controller_ && controller_->popupVisible(); }

void AdPopover::setVisible(bool value) {
  if (!controller_) {
    return;
  }
  explicitVisibleSet_ = true;
  controller_->setPopupVisible(value);
}

void AdPopover::show() { setVisible(true); }

void AdPopover::hide() { setVisible(false); }

void AdPopover::toggle() { setVisible(!isVisible()); }

void AdPopover::preparePopup() {
  if (!popupHasContent()) {
    return;
  }
  popupPrepareToShow();
  if (!popupSurface_ || popupSurface_->isVisible()) {
    return;
  }

  // Prewarming must not enter the visible popup lifecycle. Even a zero-opacity
  // top-level show can briefly expose a native compositor surface on Windows.
  popupSurface_->ensurePolished();
  if (!popupSurface_->size().isEmpty()) {
    QPixmap warmupFrame(popupSurface_->size());
    warmupFrame.fill(Qt::transparent);
    popupSurface_->render(&warmupFrame);
  }
}

void AdPopover::setVisibilityPolicy(VisibilityPolicy value) {
  if (visibilityPolicy_ == value) {
    return;
  }
  visibilityPolicy_ = value;
  if (controller_) {
    controller_->setVisibilityMode(visibilityPolicy_ == VisibilityPolicy::Manual
                                       ? detail::OverlayPopupController::VisibilityMode::External
                                       : detail::OverlayPopupController::VisibilityMode::Automatic);
  }
  emit visibilityPolicyChanged(visibilityPolicy_);
  applyDefaultVisibleIfNeeded();
}

void AdPopover::setPopupLifetime(PopupLifetime value) {
  if (popupLifetime_ == value) {
    return;
  }
  popupLifetime_ = value;
  emit popupLifetimeChanged(popupLifetime_);
  emit destroyOnHiddenChanged(destroyOnHidden());
  if (!isVisible() && popupLifetime_ == PopupLifetime::RecreateOnOpen) {
    releasePopupSurface();
  }
}

void AdPopover::setPopupLayerMode(PopupLayerMode value) {
  if (popupLayerMode_ == value) {
    return;
  }

  const bool wasVisible = isVisible();
  popupLayerMode_ = value;
  if (popupSurface_) {
    releasePopupSurface();
  } else if (controller_) {
    controller_->popupSurfaceChanged();
    controller_->invalidatePopupGeometry();
  }
  emit popupLayerModeChanged(popupLayerMode_);
  if (wasVisible) {
    refreshVisiblePopup();
  }
}

void AdPopover::setDestroyOnHidden(bool value) {
  setPopupLifetime(value ? PopupLifetime::RecreateOnOpen : PopupLifetime::Retained);
}

void AdPopover::setDefaultVisible(bool value) {
  if (defaultVisible_ == value) {
    return;
  }
  defaultVisible_ = value;
  if (defaultVisible_) {
    defaultVisibleApplied_ = false;
  }
  emit defaultVisibleChanged(defaultVisible_);
  applyDefaultVisibleIfNeeded();
}

void AdPopover::setAutoAdjustOverflow(bool value) {
  if (autoAdjustOverflow_ == value) {
    return;
  }
  autoAdjustOverflow_ = value;
  emit autoAdjustOverflowChanged(autoAdjustOverflow_);
  refreshVisiblePopup();
}

void AdPopover::setArrowVisible(bool value) {
  if (arrowVisible_ == value) {
    return;
  }
  arrowVisible_ = value;
  emit arrowVisibleChanged(arrowVisible_);
  updateStyle();
  refreshVisiblePopup();
}

void AdPopover::setArrowPointAtCenter(bool value) {
  if (arrowPointAtCenter_ == value) {
    return;
  }
  arrowPointAtCenter_ = value;
  emit arrowPointAtCenterChanged(arrowPointAtCenter_);
  refreshVisiblePopup();
}

void AdPopover::setHoverOpenDelayMs(int value) {
  const int normalized = std::max(0, value);
  if (hoverOpenDelayMs_ == normalized) {
    return;
  }
  hoverOpenDelayMs_ = normalized;
  if (controller_) {
    controller_->setMouseEnterDelayMs(hoverOpenDelayMs_);
  }
  emit hoverOpenDelayMsChanged(hoverOpenDelayMs_);
}

void AdPopover::setHoverCloseDelayMs(int value) {
  const int normalized = std::max(0, value);
  if (hoverCloseDelayMs_ == normalized) {
    return;
  }
  hoverCloseDelayMs_ = normalized;
  if (controller_) {
    controller_->setMouseLeaveDelayMs(hoverCloseDelayMs_);
  }
  emit hoverCloseDelayMsChanged(hoverCloseDelayMs_);
}

void AdPopover::setEnabled(bool value) {
  if (enabled_ == value) {
    return;
  }
  enabled_ = value;
  emit enabledChanged(enabled_);
  syncControllerConfiguration();
  syncAccessibleState();
  updateStyle();
  refreshVisiblePopup();
}

void AdPopover::setTitle(const QString& value) {
  if (title_ == value) {
    return;
  }
  title_ = value;
  emit titleChanged(title_);
  syncPopupContent();
  syncAccessibleState();
  if (controller_) {
    controller_->popupContentChanged(true);
  }
  applyDefaultVisibleIfNeeded();
  refreshVisiblePopup();
}

void AdPopover::setText(const QString& value) {
  if (text_ == value) {
    return;
  }
  text_ = value;
  emit textChanged(text_);
  syncPopupContent();
  syncAccessibleState();
  if (controller_) {
    controller_->popupContentChanged(true);
  }
  applyDefaultVisibleIfNeeded();
  refreshVisiblePopup();
}

void AdPopover::setSourceWidget(QWidget* widget) {
  if (sourceWidget_ == widget) {
    return;
  }

  const QWidget* previousEffectiveAnchor = effectiveAnchorWidget();
  detail::clearDerivedAccessibleDescription(sourceWidget_);
  sourceWidget_ = widget;
  refreshObservedWidgets();
  syncControllerConfiguration();
  emit sourceWidgetChanged(sourceWidget_);
  if (!anchorWidget_ && previousEffectiveAnchor != effectiveAnchorWidget()) {
    emit anchorWidgetChanged(anchorWidget());
  }
  syncAccessibleState();
  updateStyle();
  applyDefaultVisibleIfNeeded();
  refreshVisiblePopup();
}

QWidget* AdPopover::anchorWidget() const { return effectiveAnchorWidget(); }

void AdPopover::setAnchorWidget(QWidget* widget) {
  if (anchorWidget_ == widget) {
    return;
  }
  anchorWidget_ = widget;
  refreshObservedWidgets();
  syncControllerConfiguration();
  emit anchorWidgetChanged(anchorWidget());
  updateStyle();
  applyDefaultVisibleIfNeeded();
  refreshVisiblePopup();
}

void AdPopover::setTitleWidget(QWidget* widget) {
  if (titleWidget_ == widget) {
    return;
  }

  const bool movedFromContent = widget && widget == contentWidget_;
  if (movedFromContent) {
    QWidget* moved = takeOwnedWidget(contentWidget_, contentWidgetDestroyedConnection_);
    Q_UNUSED(moved)
  }

  deleteOwnedWidget(titleWidget_, titleWidgetDestroyedConnection_);
  titleWidget_ = widget;
  if (titleWidget_) {
    titleWidget_->hide();
    titleWidget_->setParent(ensureOwnedWidgetParkingRoot());
    bindOwnedWidgetDestroyed(titleWidget_, titleWidgetDestroyedConnection_, [this]() {
      emit titleWidgetChanged(nullptr);
      syncPopupContent();
      syncAccessibleState();
      if (controller_) {
        controller_->popupContentChanged(true);
      }
      refreshVisiblePopup();
    });
  }

  if (movedFromContent) {
    emit contentWidgetChanged(nullptr);
  }
  emit titleWidgetChanged(titleWidget_);
  syncPopupContent();
  syncAccessibleState();
  if (controller_) {
    controller_->popupContentChanged(true);
  }
  applyDefaultVisibleIfNeeded();
  refreshVisiblePopup();
}

QWidget* AdPopover::takeTitleWidget() {
  if (!titleWidget_) {
    return nullptr;
  }
  QWidget* widget = takeOwnedWidget(titleWidget_, titleWidgetDestroyedConnection_);
  emit titleWidgetChanged(nullptr);
  syncPopupContent();
  syncAccessibleState();
  if (controller_) {
    controller_->popupContentChanged(true);
  }
  refreshVisiblePopup();
  return widget;
}

void AdPopover::setContentWidget(QWidget* widget) {
  if (contentWidget_ == widget) {
    return;
  }

  const bool movedFromTitle = widget && widget == titleWidget_;
  if (movedFromTitle) {
    QWidget* moved = takeOwnedWidget(titleWidget_, titleWidgetDestroyedConnection_);
    Q_UNUSED(moved)
  }

  deleteOwnedWidget(contentWidget_, contentWidgetDestroyedConnection_);
  contentWidget_ = widget;
  if (contentWidget_) {
    contentWidget_->hide();
    contentWidget_->setParent(ensureOwnedWidgetParkingRoot());
    bindOwnedWidgetDestroyed(contentWidget_, contentWidgetDestroyedConnection_, [this]() {
      emit contentWidgetChanged(nullptr);
      syncPopupContent();
      syncAccessibleState();
      if (controller_) {
        controller_->popupContentChanged(true);
      }
      refreshVisiblePopup();
    });
  }

  if (movedFromTitle) {
    emit titleWidgetChanged(nullptr);
  }
  emit contentWidgetChanged(contentWidget_);
  syncPopupContent();
  syncAccessibleState();
  if (controller_) {
    controller_->popupContentChanged(true);
  }
  applyDefaultVisibleIfNeeded();
  refreshVisiblePopup();
}

QWidget* AdPopover::takeContentWidget() {
  if (!contentWidget_) {
    return nullptr;
  }
  QWidget* widget = takeOwnedWidget(contentWidget_, contentWidgetDestroyedConnection_);
  emit contentWidgetChanged(nullptr);
  syncPopupContent();
  syncAccessibleState();
  if (controller_) {
    controller_->popupContentChanged(true);
  }
  refreshVisiblePopup();
  return widget;
}

QFont AdPopover::titleFont() const {
  if (titleFontOverride_.has_value()) {
    return titleFontOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.titleFont;
}

void AdPopover::setTitleFont(const QFont& value) {
  const QFont previous = titleFont();
  titleFontOverride_ = value;
  const QFont current = titleFont();
  if (previous == current) {
    return;
  }
  emit titleFontChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

QFont AdPopover::textFont() const {
  if (textFontOverride_.has_value()) {
    return textFontOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.textFont;
}

void AdPopover::setTextFont(const QFont& value) {
  const QFont previous = textFont();
  textFontOverride_ = value;
  const QFont current = textFont();
  if (previous == current) {
    return;
  }
  emit textFontChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

QColor AdPopover::backgroundColor() const {
  if (backgroundColorOverride_.has_value() && backgroundColorOverride_.value().isValid()) {
    return backgroundColorOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.backgroundColor;
}

void AdPopover::setBackgroundColor(const QColor& value) {
  const QColor previous = backgroundColor();
  if (value.isValid()) {
    backgroundColorOverride_ = value;
  } else {
    backgroundColorOverride_.reset();
  }
  const QColor current = backgroundColor();
  if (previous == current) {
    return;
  }
  emit backgroundColorChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

QColor AdPopover::borderColor() const {
  if (borderColorOverride_.has_value() && borderColorOverride_.value().isValid()) {
    return borderColorOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.borderColor;
}

void AdPopover::setBorderColor(const QColor& value) {
  const QColor previous = borderColor();
  if (value.isValid()) {
    borderColorOverride_ = value;
  } else {
    borderColorOverride_.reset();
  }
  const QColor current = borderColor();
  if (previous == current) {
    return;
  }
  emit borderColorChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

QColor AdPopover::titleColor() const {
  if (titleColorOverride_.has_value() && titleColorOverride_.value().isValid()) {
    return titleColorOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.titleColor;
}

void AdPopover::setTitleColor(const QColor& value) {
  const QColor previous = titleColor();
  if (value.isValid()) {
    titleColorOverride_ = value;
  } else {
    titleColorOverride_.reset();
  }
  const QColor current = titleColor();
  if (previous == current) {
    return;
  }
  emit titleColorChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

QColor AdPopover::textColor() const {
  if (textColorOverride_.has_value() && textColorOverride_.value().isValid()) {
    return textColorOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.textColor;
}

void AdPopover::setTextColor(const QColor& value) {
  const QColor previous = textColor();
  if (value.isValid()) {
    textColorOverride_ = value;
  } else {
    textColorOverride_.reset();
  }
  const QColor current = textColor();
  if (previous == current) {
    return;
  }
  emit textColorChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

QMargins AdPopover::contentMargins() const {
  if (contentMarginsOverride_.has_value()) {
    return contentMarginsOverride_.value();
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.contentMargins;
}

void AdPopover::setContentMargins(const QMargins& value) {
  const QMargins previous = contentMargins();
  contentMarginsOverride_ = value;
  const QMargins current = contentMargins();
  if (previous == current) {
    return;
  }
  emit contentMarginsChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::titleMinimumWidth() const {
  if (titleMinimumWidthOverride_.has_value()) {
    return std::max(0, titleMinimumWidthOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.titleMinimumWidth;
}

void AdPopover::setTitleMinimumWidth(int value) {
  const int normalized = std::max(0, value);
  const int previous = titleMinimumWidth();
  titleMinimumWidthOverride_ = normalized;
  const int current = titleMinimumWidth();
  if (previous == current) {
    return;
  }
  emit titleMinimumWidthChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::maximumWidth() const {
  if (maximumWidthOverride_.has_value()) {
    return std::max(1, maximumWidthOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.maximumWidth;
}

void AdPopover::setMaximumWidth(int value) {
  const int normalized = std::max(1, value);
  const int previous = maximumWidth();
  maximumWidthOverride_ = normalized;
  const int current = maximumWidth();
  if (previous == current) {
    return;
  }
  emit maximumWidthChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::zIndex() const {
  if (zIndexOverride_.has_value()) {
    return std::max(0, zIndexOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.zIndex;
}

void AdPopover::setZIndex(int value) {
  const int normalized = std::max(0, value);
  const int previous = zIndex();
  zIndexOverride_ = normalized;
  const int current = zIndex();
  if (previous == current) {
    return;
  }
  emit zIndexChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::cornerRadius() const {
  if (cornerRadiusOverride_.has_value()) {
    return std::max(0, cornerRadiusOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.cornerRadius;
}

void AdPopover::setCornerRadius(int value) {
  const int normalized = std::max(0, value);
  const int previous = cornerRadius();
  cornerRadiusOverride_ = normalized;
  const int current = cornerRadius();
  if (previous == current) {
    return;
  }
  emit cornerRadiusChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::borderWidth() const {
  if (borderWidthOverride_.has_value()) {
    return std::max(0, borderWidthOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.borderWidth;
}

void AdPopover::setBorderWidth(int value) {
  const int normalized = std::max(0, value);
  const int previous = borderWidth();
  borderWidthOverride_ = normalized;
  const int current = borderWidth();
  if (previous == current) {
    return;
  }
  emit borderWidthChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::arrowSize() const {
  if (arrowSizeOverride_.has_value()) {
    return std::max(0, arrowSizeOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.arrowSize;
}

void AdPopover::setArrowSize(int value) {
  const int normalized = std::max(0, value);
  const int previous = arrowSize();
  arrowSizeOverride_ = normalized;
  const int current = arrowSize();
  if (previous == current) {
    return;
  }
  emit arrowSizeChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

int AdPopover::popupOffset() const {
  if (popupOffsetOverride_.has_value()) {
    return std::max(0, popupOffsetOverride_.value());
  }
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  return style.metrics.popupOffset;
}

void AdPopover::setPopupOffset(int value) {
  const int normalized = std::max(0, value);
  const int previous = popupOffset();
  popupOffsetOverride_ = normalized;
  const int current = popupOffset();
  if (previous == current) {
    return;
  }
  emit popupOffsetChanged(current);
  updateStyle();
  refreshVisiblePopup();
}

void AdPopover::refreshPopupLayout() { refreshVisiblePopup(); }

bool AdPopover::eventFilter(QObject* watched, QEvent* event) {
  if (event && (watched == sourceWidget_ || watched == anchorWidget_)) {
    if (event->type() == QEvent::EnabledChange) {
      syncControllerConfiguration();
      syncAccessibleState();
      updateStyle();
      refreshVisiblePopup();
    } else if (isVisualRefreshEvent(event->type())) {
      updateStyle();
      refreshVisiblePopup();
    }
  }
  return QObject::eventFilter(watched, event);
}

detail::OverlayPopupPlacement AdPopover::toOverlayPlacement(Placement value) {
  switch (value) {
    case Placement::Top:
      return detail::OverlayPopupPlacement::Top;
    case Placement::TopLeft:
      return detail::OverlayPopupPlacement::TopLeft;
    case Placement::TopRight:
      return detail::OverlayPopupPlacement::TopRight;
    case Placement::Bottom:
      return detail::OverlayPopupPlacement::Bottom;
    case Placement::BottomLeft:
      return detail::OverlayPopupPlacement::BottomLeft;
    case Placement::BottomRight:
      return detail::OverlayPopupPlacement::BottomRight;
    case Placement::Left:
      return detail::OverlayPopupPlacement::Left;
    case Placement::LeftTop:
      return detail::OverlayPopupPlacement::LeftTop;
    case Placement::LeftBottom:
      return detail::OverlayPopupPlacement::LeftBottom;
    case Placement::Right:
      return detail::OverlayPopupPlacement::Right;
    case Placement::RightTop:
      return detail::OverlayPopupPlacement::RightTop;
    case Placement::RightBottom:
      return detail::OverlayPopupPlacement::RightBottom;
  }
  return detail::OverlayPopupPlacement::Top;
}

detail::OverlayPopupController::Triggers AdPopover::toOverlayTriggers(Triggers value) {
  detail::OverlayPopupController::Triggers mapped;
  if (value.testFlag(Trigger::Hover)) {
    mapped |= detail::OverlayPopupController::Trigger::Hover;
  }
  if (value.testFlag(Trigger::Focus)) {
    mapped |= detail::OverlayPopupController::Trigger::Focus;
  }
  if (value.testFlag(Trigger::Click)) {
    mapped |= detail::OverlayPopupController::Trigger::Click;
  }
  if (value.testFlag(Trigger::ContextMenu)) {
    mapped |= detail::OverlayPopupController::Trigger::ContextMenu;
  }
  return mapped;
}

int AdPopover::arrowOffsetHorizontalForRadius(int borderRadius) {
  if (borderRadius > 12) {
    return borderRadius + 2;
  }
  return 12;
}

int AdPopover::arrowOffsetVerticalForRadius(int borderRadius) {
  return std::min(8, arrowOffsetHorizontalForRadius(borderRadius));
}

QWidget* AdPopover::effectiveAnchorWidget() const {
  if (anchorWidget_) {
    return anchorWidget_;
  }
  if (sourceWidget_) {
    return sourceWidget_;
  }
  return qobject_cast<QWidget*>(parent());
}

QWidget* AdPopover::effectiveScopeWidget() const {
  if (QWidget* anchor = effectiveAnchorWidget()) {
    return anchor;
  }
  return qobject_cast<QWidget*>(parent());
}

bool AdPopover::effectiveDisabled() const {
  if (!enabled_) {
    return true;
  }
  if (sourceWidget_ && !sourceWidget_->isEnabled()) {
    return true;
  }
  if (anchorWidget_ && !anchorWidget_->isEnabled()) {
    return true;
  }
  if (!sourceWidget_ && !anchorWidget_) {
    if (QWidget* parentWidget = qobject_cast<QWidget*>(parent())) {
      return !parentWidget->isEnabled();
    }
  }
  return false;
}

QFont AdPopover::effectiveBaseFont() const {
  if (sourceWidget_) {
    return sourceWidget_->font();
  }
  if (anchorWidget_) {
    return anchorWidget_->font();
  }
  if (QWidget* parentWidget = qobject_cast<QWidget*>(parent())) {
    return parentWidget->font();
  }
  return QFont();
}

QWidget* AdPopover::ensureOwnedWidgetParkingRoot() {
  if (ownedWidgetParkingRoot_) {
    return ownedWidgetParkingRoot_;
  }

  auto* parkingRoot = new QWidget();
  parkingRoot->setObjectName(QStringLiteral("adpopover-owned-widget-root"));
  parkingRoot->setAttribute(Qt::WA_DontShowOnScreen, true);
  parkingRoot->hide();
  ownedWidgetParkingRoot_ = parkingRoot;
  return ownedWidgetParkingRoot_;
}

void AdPopover::deleteOwnedWidget(QPointer<QWidget>& widget,
                                  QMetaObject::Connection& destroyedConnection) {
  QObject::disconnect(destroyedConnection);
  destroyedConnection = QMetaObject::Connection();
  if (!widget) {
    return;
  }
  QPointer<QWidget> owned = widget;
  widget.clear();
  owned->hide();
  owned->deleteLater();
}

QWidget* AdPopover::takeOwnedWidget(QPointer<QWidget>& widget,
                                    QMetaObject::Connection& destroyedConnection) {
  QObject::disconnect(destroyedConnection);
  destroyedConnection = QMetaObject::Connection();
  if (!widget) {
    return nullptr;
  }

  QWidget* owned = widget.data();
  widget.clear();
  owned->hide();
  owned->setParent(nullptr);
  return owned;
}

void AdPopover::bindOwnedWidgetDestroyed(QPointer<QWidget>& widget,
                                         QMetaObject::Connection& destroyedConnection,
                                         const std::function<void()>& onDestroyed) {
  QObject::disconnect(destroyedConnection);
  destroyedConnection = QMetaObject::Connection();
  if (!widget) {
    return;
  }

  destroyedConnection = connect(widget, &QObject::destroyed, this,
                                [this, &widget, &destroyedConnection, onDestroyed](QObject*) {
                                  QObject::disconnect(destroyedConnection);
                                  destroyedConnection = QMetaObject::Connection();
                                  widget.clear();
                                  if (onDestroyed) {
                                    onDestroyed();
                                  }
                                });
}

void AdPopover::ensurePopupSurface() {
  if (popupSurface_ && popupBodyHost_ && popupContentView_) {
    return;
  }
  if (popupSurface_ || popupBodyHost_ || popupContentView_) {
    releasePopupSurface();
  }

  QWidget* scopeWindow = popupScopeWindow();
  auto* surface = new detail::OverlayPopupSurface(
      popupLayerMode_ == PopupLayerMode::QtTool ? nullptr : scopeWindow);
  if (popupLayerMode_ == PopupLayerMode::QtTool) {
    surface->setWindowFlags(adQtToolWindowFlags());
    surface->setAttribute(Qt::WA_ShowWithoutActivating, true);
    // Qt otherwise suppresses hover tooltip events in this parentless, inactive window.
    surface->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    surface->setAttribute(Qt::WA_TranslucentBackground, true);
    surface->setAttribute(Qt::WA_QuitOnClose, false);
  }
  surface->setObjectName(QStringLiteral("adpopover-surface"));
  surface->setProperty("adqt.interaction.surface", true);
  surface->setAttribute(Qt::WA_DeleteOnClose, false);
  surface->setAttribute(Qt::WA_Hover, true);
  surface->setMouseTracking(true);
  surface->hide();
  popupSurface_ = surface;

  popupBodyHost_ = surface->bodyWidget();
  if (popupBodyHost_) {
    popupBodyHost_->setObjectName(QStringLiteral("adpopover-popup"));
    popupBodyHost_->setProperty("adqt.interaction.surface", true);
    popupBodyHost_->setAttribute(Qt::WA_Hover, true);
    popupBodyHost_->setMouseTracking(true);
    popupBodyHost_->setAutoFillBackground(false);

    auto* bodyLayout = new QVBoxLayout(popupBodyHost_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto* contentView = new detail::PopoverContentView(popupBodyHost_);
    popupContentView_ = contentView;
    bodyLayout->addWidget(contentView);
  }

  if (controller_) {
    controller_->popupSurfaceChanged();
  }
  syncPopupContent();
  syncAccessibleState();
  updateStyle();
}

void AdPopover::releasePopupSurface() {
  QPointer<QWidget> surfaceToRelease = popupSurface_;
  const auto reparentIfOwned = [this, surfaceToRelease](QPointer<QWidget>& widget) {
    if (!widget) {
      return;
    }
    const bool insideSurface = surfaceToRelease && (widget.data() == surfaceToRelease ||
                                                    surfaceToRelease->isAncestorOf(widget));
    if (!insideSurface) {
      return;
    }
    widget->hide();
    widget->setParent(ensureOwnedWidgetParkingRoot());
  };

  reparentIfOwned(titleWidget_);
  reparentIfOwned(contentWidget_);

  popupSurface_.clear();
  popupBodyHost_.clear();
  popupContentView_.clear();

  if (surfaceToRelease) {
    surfaceToRelease->hide();
    surfaceToRelease->deleteLater();
  }

  if (controller_) {
    controller_->popupSurfaceChanged();
    controller_->invalidatePopupGeometry();
  }
}

void AdPopover::syncPopupContent() {
  if (popupContentView_) {
    popupContentView_->setTitle(title_);
    popupContentView_->setText(text_);
    popupContentView_->setTitleWidget(titleWidget_);
    popupContentView_->setContentWidget(contentWidget_);
  }
  syncAccessibleState();
}

void AdPopover::syncAccessibleState() {
  const QString accessibleName =
      firstNonEmpty({title_, titleWidget_ ? titleWidget_->accessibleName() : QString(),
                     contentWidget_ ? contentWidget_->accessibleName() : QString(), text_,
                     titleWidget_ ? titleWidget_->accessibleDescription() : QString(),
                     contentWidget_ ? contentWidget_->accessibleDescription() : QString()});
  const QString accessibleDescription =
      firstNonEmpty({text_, contentWidget_ ? contentWidget_->accessibleDescription() : QString(),
                     contentWidget_ ? contentWidget_->accessibleName() : QString(),
                     titleWidget_ ? titleWidget_->accessibleDescription() : QString(),
                     titleWidget_ ? titleWidget_->accessibleName() : QString(), title_});

  if (popupContentView_) {
    popupContentView_->setAccessibleName(accessibleName);
    popupContentView_->setAccessibleDescription(accessibleDescription);
  }
  if (popupBodyHost_) {
    popupBodyHost_->setAccessibleName(accessibleName);
    popupBodyHost_->setAccessibleDescription(accessibleDescription);
  }
  if (popupSurface_) {
    popupSurface_->setAccessibleName(accessibleName);
    popupSurface_->setAccessibleDescription(accessibleDescription);
  }

  detail::syncDerivedAccessibleDescription(sourceWidget_, accessibleDescription);
}

void AdPopover::updateStyle() {
  const detail::PopoverVisualStyle style = resolvePopoverStyleSnapshot(
      placement_, isVisible(), effectiveDisabled(), arrowVisible_, effectiveBaseFont(),
      makePopoverStyleOverrides(titleFontOverride_, textFontOverride_, backgroundColorOverride_,
                                borderColorOverride_, titleColorOverride_, textColorOverride_,
                                contentMarginsOverride_, titleMinimumWidthOverride_,
                                maximumWidthOverride_, zIndexOverride_, cornerRadiusOverride_,
                                borderWidthOverride_, arrowSizeOverride_, popupOffsetOverride_),
      effectiveAnchorWidget() ? effectiveAnchorWidget() : qobject_cast<QWidget*>(parent()));
  cachedPopupOffset_ = std::max(0, style.metrics.popupOffset);
  cachedBorderRadius_ = std::max(0, style.metrics.cornerRadius);

  if (!popupSurface_) {
    return;
  }

  auto* surface = static_cast<detail::OverlayPopupSurface*>(popupSurface_.data());
  detail::OverlayPopupSurfaceStyle surfaceStyle;
  surfaceStyle.background = style.backgroundColor;
  surfaceStyle.borderColor = style.borderColor;
  surfaceStyle.metrics.borderRadius = std::max(0, style.metrics.cornerRadius);
  surfaceStyle.metrics.borderWidth = std::max(0, style.metrics.borderWidth);
  surfaceStyle.metrics.arrowSize = std::max(0, style.metrics.arrowSize);
  surface->setSurfaceStyle(surfaceStyle);
  surface->setArrowVisible(arrowVisible_);
  surface->setPlacement(toOverlayPlacement(placement_));
  popupSurface_->setProperty("adqt.zIndex", style.metrics.zIndex);

  if (popupBodyHost_) {
    popupBodyHost_->setProperty("adqt.zIndex", style.metrics.zIndex);
    popupBodyHost_->setMaximumWidth(std::max(1, style.metrics.maximumWidth));
  }
  if (popupContentView_) {
    popupContentView_->setMaximumWidth(std::max(1, style.metrics.maximumWidth));
    popupContentView_->setTitleFont(style.metrics.titleFont);
    popupContentView_->setTextFont(style.metrics.textFont);
    popupContentView_->setTitleColor(style.titleColor);
    popupContentView_->setTextColor(style.textColor);
    popupContentView_->setContentMargins(style.metrics.contentMargins);
    popupContentView_->setSectionSpacing(style.metrics.sectionSpacing);
    popupContentView_->setTitleMinimumWidth(style.metrics.titleMinimumWidth);
  }

  if (controller_) {
    controller_->invalidatePopupGeometry();
  }
}

void AdPopover::refreshVisiblePopup() {
  if (controller_ && isVisible()) {
    controller_->refreshVisiblePopup();
  }
}

void AdPopover::syncControllerConfiguration() {
  if (!controller_) {
    return;
  }
  controller_->setTriggerModes(toOverlayTriggers(triggers_));
  controller_->setVisibilityMode(visibilityPolicy_ == VisibilityPolicy::Manual
                                     ? detail::OverlayPopupController::VisibilityMode::External
                                     : detail::OverlayPopupController::VisibilityMode::Automatic);
  controller_->setDisabled(effectiveDisabled());
  controller_->setMouseEnterDelayMs(hoverOpenDelayMs_);
  controller_->setMouseLeaveDelayMs(hoverCloseDelayMs_);
  controller_->anchorWidgetChanged();
}

void AdPopover::refreshObservedWidgets() {
  QSet<QObject*> nextWidgets;
  if (sourceWidget_) {
    nextWidgets.insert(sourceWidget_);
  }
  if (anchorWidget_) {
    nextWidgets.insert(anchorWidget_);
  }

  for (auto it = observedWidgetDestroyedConnections_.begin();
       it != observedWidgetDestroyedConnections_.end();) {
    QObject* object = it.key();
    if (!object || !nextWidgets.contains(object)) {
      if (object) {
        object->removeEventFilter(this);
      }
      disconnect(it.value());
      it = observedWidgetDestroyedConnections_.erase(it);
    } else {
      ++it;
    }
  }

  for (QObject* object : nextWidgets) {
    if (!object || observedWidgetDestroyedConnections_.contains(object)) {
      continue;
    }
    object->installEventFilter(this);
    observedWidgetDestroyedConnections_.insert(
        object,
        connect(object, &QObject::destroyed, this, &AdPopover::handleObservedWidgetDestroyed));
  }
}

void AdPopover::clearObservedWidgets() {
  for (auto it = observedWidgetDestroyedConnections_.begin();
       it != observedWidgetDestroyedConnections_.end(); ++it) {
    if (QObject* object = it.key()) {
      object->removeEventFilter(this);
    }
    disconnect(it.value());
  }
  observedWidgetDestroyedConnections_.clear();
}

void AdPopover::handleObservedWidgetDestroyed(QObject* object) {
  const QWidget* previousEffectiveAnchor = effectiveAnchorWidget();
  bool anchorSignalNeeded = false;

  if (object == sourceWidget_) {
    sourceWidget_.clear();
    emit sourceWidgetChanged(nullptr);
    anchorSignalNeeded = !anchorWidget_;
  }
  if (object == anchorWidget_) {
    anchorWidget_.clear();
    anchorSignalNeeded = true;
  }

  auto it = observedWidgetDestroyedConnections_.find(object);
  if (it != observedWidgetDestroyedConnections_.end()) {
    disconnect(it.value());
    observedWidgetDestroyedConnections_.erase(it);
  }

  if (anchorSignalNeeded || previousEffectiveAnchor != effectiveAnchorWidget()) {
    emit anchorWidgetChanged(anchorWidget());
  }

  syncControllerConfiguration();
  syncAccessibleState();
  updateStyle();
  refreshVisiblePopup();
}

void AdPopover::applyDefaultVisibleIfNeeded() {
  if (!controller_ || defaultVisibleApplied_ || explicitVisibleSet_ || !defaultVisible_ ||
      visibilityPolicy_ == VisibilityPolicy::Manual || !popupHasContent() ||
      !effectiveAnchorWidget()) {
    return;
  }
  defaultVisibleApplied_ = true;
  controller_->setPopupVisible(true);
}

void AdPopover::emitVisibleSignals(bool value) { emit visibleChanged(value); }

void AdPopover::handleControllerPopupVisibleChanged(bool value) {
  if (popupSurface_) {
    detail::notifyAccessibilityEvent(popupSurface_,
                                     value ? QAccessible::ObjectShow : QAccessible::ObjectHide);
  }
  emitVisibleSignals(value);
}

QObject* AdPopover::popupOwnerObject() const { return const_cast<AdPopover*>(this); }

QWidget* AdPopover::popupTriggerWidget() const {
  if (sourceWidget_) {
    return sourceWidget_;
  }
  return effectiveAnchorWidget();
}

QWidget* AdPopover::popupAnchorWidget() const { return effectiveAnchorWidget(); }

QWidget* AdPopover::popupScopeWindow() const {
  return detail::resolvePopupScopeWindow(effectiveScopeWidget());
}

QWidget* AdPopover::popupSurfaceWidget() const { return popupSurface_; }

QWidget* AdPopover::popupEnsureSurface() {
  ensurePopupSurface();
  return popupSurface_;
}

void AdPopover::popupPrepareToShow() {
  ensurePopupSurface();
  syncPopupContent();
  updateStyle();

  syncGeometryChain(titleWidget_, popupSurface_);
  syncGeometryChain(contentWidget_, popupSurface_);
  if (popupContentView_) {
    if (QLayout* layout = popupContentView_->layout()) {
      layout->invalidate();
      layout->activate();
    }
    popupContentView_->updateGeometry();
    popupContentView_->adjustSize();
  }
  if (popupBodyHost_) {
    if (QLayout* layout = popupBodyHost_->layout()) {
      layout->invalidate();
      layout->activate();
    }
    popupBodyHost_->updateGeometry();
    popupBodyHost_->adjustSize();
  }
  if (popupSurface_) {
    popupSurface_->updateGeometry();
    popupSurface_->adjustSize();
  }
}

bool AdPopover::popupHasContent() const {
  return !title_.trimmed().isEmpty() || !text_.trimmed().isEmpty() || !titleWidget_.isNull() ||
         !contentWidget_.isNull();
}

detail::OverlayPopupPlacement AdPopover::popupPlacement() const {
  return toOverlayPlacement(placement_);
}

bool AdPopover::popupAutoAdjustOverflow() const { return autoAdjustOverflow_; }

bool AdPopover::popupArrowVisible() const { return arrowVisible_; }

bool AdPopover::popupArrowPointAtCenter() const { return arrowPointAtCenter_; }

int AdPopover::popupArrowOffsetHorizontal() const {
  return arrowOffsetHorizontalForRadius(std::max(0, cachedBorderRadius_));
}

int AdPopover::popupArrowOffsetVertical() const {
  return arrowOffsetVerticalForRadius(std::max(0, cachedBorderRadius_));
}

void AdPopover::popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                            qreal arrowCenterCoord) {
  auto* surface = static_cast<detail::OverlayPopupSurface*>(popupSurface_.data());
  if (!surface) {
    return;
  }
  surface->setPlacement(placement);
  surface->setArrowCenter(arrowCenterCoord);
}

bool AdPopover::popupReleaseOnHide() const {
  return popupLifetime_ == PopupLifetime::RecreateOnOpen;
}

void AdPopover::popupReleaseSurface() { releasePopupSurface(); }

}  // namespace adqt::widgets
