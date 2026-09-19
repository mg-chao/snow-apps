#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QMargins>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>

#include "detail/overlay_popup_controller.h"
#include "popup_types.h"

class QEvent;

namespace adqt::widgets::detail {
class PopoverContentView;
}

namespace adqt::widgets {

class AdPopover final : public QObject, private detail::OverlayPopupControllerDelegate {
  Q_OBJECT

  Q_PROPERTY(adqt::widgets::AdPopupPlacement placement READ placement WRITE setPlacement NOTIFY
                 placementChanged)
  Q_PROPERTY(adqt::widgets::AdPopupTriggers triggers READ triggers WRITE setTriggers NOTIFY
                 triggersChanged)
  Q_PROPERTY(bool visible READ isVisible WRITE setVisible NOTIFY visibleChanged)
  Q_PROPERTY(adqt::widgets::AdPopupActivationMode visibilityPolicy READ visibilityPolicy WRITE
                 setVisibilityPolicy NOTIFY visibilityPolicyChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLifetime popupLifetime READ popupLifetime WRITE setPopupLifetime
                 NOTIFY popupLifetimeChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLayerMode popupLayerMode READ popupLayerMode WRITE
                 setPopupLayerMode NOTIFY popupLayerModeChanged)
  Q_PROPERTY(bool destroyOnHidden READ destroyOnHidden WRITE setDestroyOnHidden NOTIFY
                 destroyOnHiddenChanged)
  Q_PROPERTY(
      bool defaultVisible READ defaultVisible WRITE setDefaultVisible NOTIFY defaultVisibleChanged)
  Q_PROPERTY(bool autoAdjustOverflow READ autoAdjustOverflow WRITE setAutoAdjustOverflow NOTIFY
                 autoAdjustOverflowChanged)
  Q_PROPERTY(bool arrowVisible READ arrowVisible WRITE setArrowVisible NOTIFY arrowVisibleChanged)
  Q_PROPERTY(bool arrowPointAtCenter READ arrowPointAtCenter WRITE setArrowPointAtCenter NOTIFY
                 arrowPointAtCenterChanged)
  Q_PROPERTY(int hoverOpenDelayMs READ hoverOpenDelayMs WRITE setHoverOpenDelayMs NOTIFY
                 hoverOpenDelayMsChanged)
  Q_PROPERTY(int hoverCloseDelayMs READ hoverCloseDelayMs WRITE setHoverCloseDelayMs NOTIFY
                 hoverCloseDelayMsChanged)
  Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)
  Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
  Q_PROPERTY(
      QWidget* sourceWidget READ sourceWidget WRITE setSourceWidget NOTIFY sourceWidgetChanged)
  Q_PROPERTY(
      QWidget* anchorWidget READ anchorWidget WRITE setAnchorWidget NOTIFY anchorWidgetChanged)
  Q_PROPERTY(QFont titleFont READ titleFont WRITE setTitleFont NOTIFY titleFontChanged)
  Q_PROPERTY(QFont textFont READ textFont WRITE setTextFont NOTIFY textFontChanged)
  Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY
                 backgroundColorChanged)
  Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor NOTIFY borderColorChanged)
  Q_PROPERTY(QColor titleColor READ titleColor WRITE setTitleColor NOTIFY titleColorChanged)
  Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor NOTIFY textColorChanged)
  Q_PROPERTY(QMargins contentMargins READ contentMargins WRITE setContentMargins NOTIFY
                 contentMarginsChanged)
  Q_PROPERTY(int titleMinimumWidth READ titleMinimumWidth WRITE setTitleMinimumWidth NOTIFY
                 titleMinimumWidthChanged)
  Q_PROPERTY(int maximumWidth READ maximumWidth WRITE setMaximumWidth NOTIFY maximumWidthChanged)
  Q_PROPERTY(int zIndex READ zIndex WRITE setZIndex NOTIFY zIndexChanged)
  Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)
  Q_PROPERTY(int borderWidth READ borderWidth WRITE setBorderWidth NOTIFY borderWidthChanged)
  Q_PROPERTY(int arrowSize READ arrowSize WRITE setArrowSize NOTIFY arrowSizeChanged)
  Q_PROPERTY(int popupOffset READ popupOffset WRITE setPopupOffset NOTIFY popupOffsetChanged)

 public:
  using Placement = AdPopupPlacement;
  using Trigger = AdPopupTrigger;
  using Triggers = AdPopupTriggers;
  using VisibilityPolicy = AdPopupActivationMode;
  using PopupLifetime = AdPopupLifetime;
  using PopupLayerMode = AdPopupLayerMode;
  using ContentFactory = std::function<QWidget*()>;

  enum class FactoryContentLifetime {
    Retained,
    RecreateOnOpen,
  };
  Q_ENUM(FactoryContentLifetime)

  explicit AdPopover(QObject* parent = nullptr);
  ~AdPopover() override;

  static void resetSyncPopupGeometryCountersForTesting();
  static qint64 syncPopupGeometryCallCountForTesting();
  static qint64 syncPopupGeometryShortCircuitCountForTesting();

  Placement placement() const { return placement_; }
  void setPlacement(Placement value);

  Triggers triggers() const { return triggers_; }
  void setTriggers(Triggers value);

  bool isVisible() const;
  void setVisible(bool value);
  void show();
  void hide();
  void toggle();
  void preparePopup();

  VisibilityPolicy visibilityPolicy() const { return visibilityPolicy_; }
  void setVisibilityPolicy(VisibilityPolicy value);

  PopupLifetime popupLifetime() const { return popupLifetime_; }
  void setPopupLifetime(PopupLifetime value);

  PopupLayerMode popupLayerMode() const override { return popupLayerMode_; }
  void setPopupLayerMode(PopupLayerMode value);

  bool destroyOnHidden() const { return popupLifetime_ == PopupLifetime::RecreateOnOpen; }
  void setDestroyOnHidden(bool value);

  bool defaultVisible() const { return defaultVisible_; }
  void setDefaultVisible(bool value);

  bool autoAdjustOverflow() const { return autoAdjustOverflow_; }
  void setAutoAdjustOverflow(bool value);

  bool arrowVisible() const { return arrowVisible_; }
  void setArrowVisible(bool value);

  bool arrowPointAtCenter() const { return arrowPointAtCenter_; }
  void setArrowPointAtCenter(bool value);

  int hoverOpenDelayMs() const { return hoverOpenDelayMs_; }
  void setHoverOpenDelayMs(int value);

  int hoverCloseDelayMs() const { return hoverCloseDelayMs_; }
  void setHoverCloseDelayMs(int value);

  bool isEnabled() const { return enabled_; }
  void setEnabled(bool value);

  QString title() const { return title_; }
  void setTitle(const QString& value);

  QString text() const { return text_; }
  void setText(const QString& value);

  QWidget* sourceWidget() const { return sourceWidget_; }
  void setSourceWidget(QWidget* widget);

  QWidget* anchorWidget() const;
  void setAnchorWidget(QWidget* widget);

  QWidget* titleWidget() const { return titleWidget_; }
  void setTitleWidget(QWidget* widget);
  QWidget* takeTitleWidget();

  QWidget* contentWidget() const { return contentWidget_; }
  void setContentWidget(QWidget* widget);
  QWidget* takeContentWidget();

  ContentFactory contentFactory() const { return contentFactory_; }
  void setContentFactory(ContentFactory factory,
                         FactoryContentLifetime lifetime = FactoryContentLifetime::Retained);
  FactoryContentLifetime factoryContentLifetime() const { return factoryContentLifetime_; }

  QFont titleFont() const;
  void setTitleFont(const QFont& value);

  QFont textFont() const;
  void setTextFont(const QFont& value);

  QColor backgroundColor() const;
  void setBackgroundColor(const QColor& value);

  QColor borderColor() const;
  void setBorderColor(const QColor& value);

  QColor titleColor() const;
  void setTitleColor(const QColor& value);

  QColor textColor() const;
  void setTextColor(const QColor& value);

  QMargins contentMargins() const;
  void setContentMargins(const QMargins& value);

  int titleMinimumWidth() const;
  void setTitleMinimumWidth(int value);

  int maximumWidth() const;
  void setMaximumWidth(int value);

  int zIndex() const;
  void setZIndex(int value);

  int cornerRadius() const;
  void setCornerRadius(int value);

  int borderWidth() const;
  void setBorderWidth(int value);

  int arrowSize() const;
  void setArrowSize(int value);

  int popupOffset() const override;
  void setPopupOffset(int value);

  void refreshPopupLayout();

 signals:
  void placementChanged(Placement value);
  void triggersChanged(Triggers value);
  void visibleChanged(bool value);
  void visibilityRequested(bool value);
  void visibilityPolicyChanged(VisibilityPolicy value);
  void popupLifetimeChanged(PopupLifetime value);
  void popupLayerModeChanged(PopupLayerMode value);
  void destroyOnHiddenChanged(bool value);
  void defaultVisibleChanged(bool value);
  void autoAdjustOverflowChanged(bool value);
  void arrowVisibleChanged(bool value);
  void arrowPointAtCenterChanged(bool value);
  void hoverOpenDelayMsChanged(int value);
  void hoverCloseDelayMsChanged(int value);
  void enabledChanged(bool value);
  void titleChanged(const QString& value);
  void textChanged(const QString& value);
  void sourceWidgetChanged(QWidget* value);
  void anchorWidgetChanged(QWidget* value);
  void titleWidgetChanged(QWidget* value);
  void contentWidgetChanged(QWidget* value);
  void titleFontChanged(const QFont& value);
  void textFontChanged(const QFont& value);
  void backgroundColorChanged(const QColor& value);
  void borderColorChanged(const QColor& value);
  void titleColorChanged(const QColor& value);
  void textColorChanged(const QColor& value);
  void contentMarginsChanged(const QMargins& value);
  void titleMinimumWidthChanged(int value);
  void maximumWidthChanged(int value);
  void zIndexChanged(int value);
  void cornerRadiusChanged(int value);
  void borderWidthChanged(int value);
  void arrowSizeChanged(int value);
  void popupOffsetChanged(int value);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  static detail::OverlayPopupPlacement toOverlayPlacement(Placement value);
  static detail::OverlayPopupController::Triggers toOverlayTriggers(Triggers value);
  static int arrowOffsetHorizontalForRadius(int borderRadius);
  static int arrowOffsetVerticalForRadius(int borderRadius);

  QWidget* effectiveAnchorWidget() const;
  QWidget* effectiveScopeWidget() const;
  bool effectiveDisabled() const;
  QFont effectiveBaseFont() const;

  QWidget* ensureOwnedWidgetParkingRoot();
  void deleteOwnedWidget(QPointer<QWidget>& widget, QMetaObject::Connection& destroyedConnection);
  QWidget* takeOwnedWidget(QPointer<QWidget>& widget, QMetaObject::Connection& destroyedConnection);
  void bindOwnedWidgetDestroyed(QPointer<QWidget>& widget,
                                QMetaObject::Connection& destroyedConnection,
                                const std::function<void()>& onDestroyed);
  void ensurePopupSurface();
  void releasePopupSurface();
  void syncPopupContent();
  void syncAccessibleState();
  void updateStyle();
  void refreshVisiblePopup();
  void syncControllerConfiguration();
  void refreshObservedWidgets();
  void clearObservedWidgets();
  void handleObservedWidgetDestroyed(QObject* object);
  void handleControllerPopupVisibleChanged(bool value);
  void ensureFactoryContent();
  void releaseFactoryContent();
  void applyDefaultVisibleIfNeeded();
  void emitVisibleSignals(bool value);

  QObject* popupOwnerObject() const override;
  QWidget* popupTriggerWidget() const override;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  QWidget* popupSurfaceWidget() const override;
  QWidget* popupEnsureSurface() override;
  void popupPrepareToShow() override;
  bool popupHasContent() const override;
  detail::OverlayPopupPlacement popupPlacement() const override;
  bool popupAutoAdjustOverflow() const override;
  bool popupArrowVisible() const override;
  bool popupArrowPointAtCenter() const override;
  int popupArrowOffsetHorizontal() const override;
  int popupArrowOffsetVertical() const override;
  void popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                   qreal arrowCenterCoord) override;
  bool popupReleaseOnHide() const override;
  void popupReleaseSurface() override;

  Placement placement_ = Placement::Top;
  Triggers triggers_ = Trigger::Hover;
  VisibilityPolicy visibilityPolicy_ = VisibilityPolicy::Automatic;
  PopupLifetime popupLifetime_ = PopupLifetime::Retained;
  PopupLayerMode popupLayerMode_ = PopupLayerMode::InWindow;
  bool defaultVisible_ = false;
  bool defaultVisibleApplied_ = false;
  bool explicitVisibleSet_ = false;
  bool autoAdjustOverflow_ = true;
  bool arrowVisible_ = true;
  bool arrowPointAtCenter_ = false;
  int hoverOpenDelayMs_ = 100;
  int hoverCloseDelayMs_ = 100;
  bool enabled_ = true;
  QString title_;
  QString text_;

  std::optional<QFont> titleFontOverride_;
  std::optional<QFont> textFontOverride_;
  std::optional<QColor> backgroundColorOverride_;
  std::optional<QColor> borderColorOverride_;
  std::optional<QColor> titleColorOverride_;
  std::optional<QColor> textColorOverride_;
  std::optional<QMargins> contentMarginsOverride_;
  std::optional<int> titleMinimumWidthOverride_;
  std::optional<int> maximumWidthOverride_;
  std::optional<int> zIndexOverride_;
  std::optional<int> cornerRadiusOverride_;
  std::optional<int> borderWidthOverride_;
  std::optional<int> arrowSizeOverride_;
  std::optional<int> popupOffsetOverride_;

  QPointer<detail::OverlayPopupController> controller_;
  QPointer<QWidget> sourceWidget_;
  QPointer<QWidget> anchorWidget_;
  QHash<QObject*, QMetaObject::Connection> observedWidgetDestroyedConnections_;

  QPointer<QWidget> ownedWidgetParkingRoot_;
  QPointer<QWidget> titleWidget_;
  QPointer<QWidget> contentWidget_;
  QMetaObject::Connection titleWidgetDestroyedConnection_;
  QMetaObject::Connection contentWidgetDestroyedConnection_;
  ContentFactory contentFactory_;
  FactoryContentLifetime factoryContentLifetime_ = FactoryContentLifetime::Retained;
  bool updatingFactoryContent_ = false;
  bool contentCreatedByFactory_ = false;

  QPointer<QWidget> popupSurface_;
  QPointer<QWidget> popupBodyHost_;
  QPointer<detail::PopoverContentView> popupContentView_;

  int cachedPopupOffset_ = 0;
  int cachedBorderRadius_ = 0;
};

}  // namespace adqt::widgets
