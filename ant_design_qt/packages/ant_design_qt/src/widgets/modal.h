#pragma once

#include <QColor>
#include <QFont>
#include <QFlags>
#include <QMetaObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QObject>

#include <functional>
#include <optional>

#include "button.h"

class QEvent;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QShortcut;
class QScreen;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace adqt::widgets {

class AdModalService;

class AdModal final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AdModal)
  friend class AdModalService;

  Q_PROPERTY(
      ClosePolicy closePolicy READ closePolicy WRITE setClosePolicy NOTIFY closePolicyChanged)
  Q_PROPERTY(Mode mode READ mode WRITE setMode NOTIFY modeChanged)
  Q_PROPERTY(Qt::WindowModality windowModality READ windowModality WRITE setWindowModality NOTIFY
                 windowModalityChanged)
  Q_PROPERTY(bool windowModeDetached READ windowModeDetached WRITE setWindowModeDetached NOTIFY
                 windowModeDetachedChanged)
  Q_PROPERTY(bool open READ isOpen WRITE setOpen NOTIFY openChanged)
  Q_PROPERTY(QString windowTitle READ windowTitle WRITE setWindowTitle NOTIFY windowTitleChanged)
  Q_PROPERTY(bool centered READ centered WRITE setCentered NOTIFY centeredChanged)
  Q_PROPERTY(
      int preferredWidth READ preferredWidth WRITE setPreferredWidth NOTIFY preferredWidthChanged)
  Q_PROPERTY(int topOffset READ topOffset WRITE setTopOffset NOTIFY topOffsetChanged)
  Q_PROPERTY(bool maskVisible READ maskVisible WRITE setMaskVisible NOTIFY maskVisibleChanged)
  Q_PROPERTY(bool closeOnMaskClick READ closeOnMaskClick WRITE setCloseOnMaskClick NOTIFY
                 closeOnMaskClickChanged)
  Q_PROPERTY(
      bool closeOnEscape READ closeOnEscape WRITE setCloseOnEscape NOTIFY closeOnEscapeChanged)
  Q_PROPERTY(bool closeButtonVisible READ closeButtonVisible WRITE setCloseButtonVisible NOTIFY
                 closeButtonVisibleChanged)
  Q_PROPERTY(
      bool footerVisible READ footerVisible WRITE setFooterVisible NOTIFY footerVisibleChanged)
  Q_PROPERTY(StandardButtons standardButtons READ standardButtons WRITE setStandardButtons NOTIFY
                 standardButtonsChanged)
  Q_PROPERTY(bool acceptButtonBusy READ acceptButtonBusy WRITE setAcceptButtonBusy NOTIFY
                 acceptButtonBusyChanged)
  Q_PROPERTY(
      bool contentLoading READ contentLoading WRITE setContentLoading NOTIFY contentLoadingChanged)
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
  Q_PROPERTY(QString acceptText READ acceptText WRITE setAcceptText NOTIFY acceptTextChanged)
  Q_PROPERTY(QString rejectText READ rejectText WRITE setRejectText NOTIFY rejectTextChanged)
  Q_PROPERTY(adqt::widgets::AdButton::AccentRole acceptAccentRole READ acceptAccentRole WRITE
                 setAcceptAccentRole NOTIFY acceptAccentRoleChanged)
  Q_PROPERTY(adqt::widgets::AdButton::ButtonStyle acceptButtonStyle READ acceptButtonStyle WRITE
                 setAcceptButtonStyle NOTIFY acceptButtonStyleChanged)
  Q_PROPERTY(Preset preset READ preset WRITE setPreset NOTIFY presetChanged)
  Q_PROPERTY(QWidget* ownerWindow READ ownerWindow WRITE setOwnerWindow NOTIFY ownerWindowChanged)
  Q_PROPERTY(QWidget* renderContainer READ renderContainer WRITE setRenderContainer NOTIFY
                 renderContainerChanged)
  Q_PROPERTY(
      QWidget* contentWidget READ contentWidget WRITE setContentWidget NOTIFY contentWidgetChanged)
  Q_PROPERTY(
      QWidget* footerWidget READ footerWidget WRITE setFooterWidget NOTIFY footerWidgetChanged)

 public:
  enum class ClosePolicy {
    Automatic,
    Manual,
  };
  Q_ENUM(ClosePolicy)

  enum class Mode {
    Overlay,
    Window,
  };
  Q_ENUM(Mode)

  enum class Preset {
    Plain,
    Info,
    Success,
    Error,
    Warning,
    Confirm,
  };
  Q_ENUM(Preset)

  enum class DialogCode {
    Rejected = 0,
    Accepted = 1,
  };
  Q_ENUM(DialogCode)

  enum class StandardButton {
    NoButton = 0x0,
    Ok = 0x1,
    Cancel = 0x2,
  };
  Q_ENUM(StandardButton)
  Q_DECLARE_FLAGS(StandardButtons, StandardButton)
  Q_FLAG(StandardButtons)

  enum class CloseReason {
    OkAction,
    CancelAction,
    CloseButton,
    Mask,
    Keyboard,
    ScopeHidden,
    Programmatic,
  };
  Q_ENUM(CloseReason)

  struct ComponentTokens {
    std::optional<int> width;
    std::optional<int> zIndexPopup;
    std::optional<int> borderRadius;
    std::optional<int> borderWidth;
    std::optional<int> headerPaddingHorizontal;
    std::optional<int> headerPaddingVertical;
    std::optional<int> bodyPaddingHorizontal;
    std::optional<int> bodyPaddingVertical;
    std::optional<int> footerPaddingHorizontal;
    std::optional<int> footerPaddingVertical;
    std::optional<int> footerButtonGap;
    std::optional<int> iconSize;
    std::optional<QColor> maskBg;
    std::optional<QColor> contentBg;
    std::optional<QColor> headerBg;
    std::optional<QColor> bodyBg;
    std::optional<QColor> footerBg;
    std::optional<QColor> borderColor;
    std::optional<QColor> titleColor;
    std::optional<QColor> bodyColor;
    std::optional<QColor> iconColor;
    std::optional<QColor> closeIconColor;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle mask;
    SemanticSlotStyle container;
    SemanticSlotStyle header;
    SemanticSlotStyle title;
    SemanticSlotStyle body;
    SemanticSlotStyle footer;
    SemanticSlotStyle icon;
    SemanticSlotStyle close;
  };

  struct StyleContext {
    bool open = false;
    Mode mode = Mode::Overlay;
    bool centered = false;
    bool contentLoading = false;
    bool acceptButtonBusy = false;
    bool maskVisible = true;
    bool closeButtonVisible = true;
    StandardButtons standardButtons = StandardButtons(StandardButton::Ok) | StandardButton::Cancel;
    Preset preset = Preset::Plain;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdModal(QObject* parent = nullptr);
  ~AdModal() override;

  ClosePolicy closePolicy() const;
  void setClosePolicy(ClosePolicy value);

  Mode mode() const;
  void setMode(Mode value);

  Qt::WindowModality windowModality() const;
  void setWindowModality(Qt::WindowModality value);

  bool windowModeDetached() const;
  void setWindowModeDetached(bool value);

  // Optional window-surface geometry. Empty sizes retain content-driven sizing.
  QScreen* windowScreen() const;
  void setWindowScreen(QScreen* screen);
  QSize windowPreferredSize() const;
  void setWindowPreferredSize(const QSize& size);
  QSize windowMinimumSize() const;
  void setWindowMinimumSize(const QSize& size);
  bool windowResizable() const;
  void setWindowResizable(bool value);
  // Open, or restore and activate an already open surface without resetting geometry.
  void present();

  bool isOpen() const;
  void setOpen(bool value);
  void open();

  QString windowTitle() const;
  void setWindowTitle(const QString& value);

  bool centered() const;
  void setCentered(bool value);

  int preferredWidth() const;
  void setPreferredWidth(int value);

  int topOffset() const;
  void setTopOffset(int value);

  bool maskVisible() const;
  void setMaskVisible(bool value);

  bool closeOnMaskClick() const;
  void setCloseOnMaskClick(bool value);

  bool closeOnEscape() const;
  void setCloseOnEscape(bool value);

  bool closeButtonVisible() const;
  void setCloseButtonVisible(bool value);

  bool footerVisible() const;
  void setFooterVisible(bool value);

  StandardButtons standardButtons() const;
  void setStandardButtons(StandardButtons value);

  bool acceptButtonBusy() const;
  void setAcceptButtonBusy(bool value);

  bool contentLoading() const;
  void setContentLoading(bool value);

  QString text() const;
  void setText(const QString& value);

  QString acceptText() const;
  void setAcceptText(const QString& value);

  QString rejectText() const;
  void setRejectText(const QString& value);

  AdButton::AccentRole acceptAccentRole() const;
  void setAcceptAccentRole(AdButton::AccentRole value);

  AdButton::ButtonStyle acceptButtonStyle() const;
  void setAcceptButtonStyle(AdButton::ButtonStyle value);

  Preset preset() const;
  void setPreset(Preset value);

  QWidget* ownerWindow() const;
  void setOwnerWindow(QWidget* value);

  QWidget* renderContainer() const;
  void setRenderContainer(QWidget* value);

  QWidget* contentWidget() const;
  void setContentWidget(QWidget* widget);
  QWidget* takeContentWidget();

  QWidget* footerWidget() const;
  void setFooterWidget(QWidget* widget);
  QWidget* takeFooterWidget();

  void setInitialFocusWidget(QWidget* widget);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  QPushButton* acceptButton() const;
  QPushButton* rejectButton() const;
  QToolButton* closeButton() const;

  int result() const;
  void done(DialogCode code);
  void accept();
  void reject();
  bool close();

 signals:
  void closePolicyChanged(ClosePolicy value);
  void modeChanged(Mode value);
  void windowModalityChanged(Qt::WindowModality value);
  void windowModeDetachedChanged(bool value);
  void openChanged(bool value);
  void windowTitleChanged(const QString& value);
  void finished(DialogCode code);
  void accepted();
  void rejected();
  void closed(CloseReason reason);
  void centeredChanged(bool value);
  void preferredWidthChanged(int value);
  void topOffsetChanged(int value);
  void maskVisibleChanged(bool value);
  void closeOnMaskClickChanged(bool value);
  void closeOnEscapeChanged(bool value);
  void closeButtonVisibleChanged(bool value);
  void footerVisibleChanged(bool value);
  void standardButtonsChanged(StandardButtons value);
  void acceptButtonBusyChanged(bool value);
  void contentLoadingChanged(bool value);
  void textChanged(const QString& value);
  void acceptTextChanged(const QString& value);
  void rejectTextChanged(const QString& value);
  void acceptAccentRoleChanged(adqt::widgets::AdButton::AccentRole value);
  void acceptButtonStyleChanged(adqt::widgets::AdButton::ButtonStyle value);
  void presetChanged(Preset value);
  void ownerWindowChanged(QWidget* value);
  void renderContainerChanged(QWidget* value);
  void contentWidgetChanged(QWidget* value);
  void footerWidgetChanged(QWidget* value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void closeRequested(CloseReason reason);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct VisualStyle {
    QColor rootBg = QColor(0, 0, 0, 0);
    QColor maskBg = QColor(0, 0, 0, 115);
    QColor containerBg = QColor("#ffffff");
    QColor headerBg = QColor("#ffffff");
    QColor bodyBg = QColor("#ffffff");
    QColor footerBg = QColor("#ffffff");
    QColor borderColor = QColor("#f0f0f0");
    QColor titleColor = QColor("#141414");
    QColor bodyColor = QColor("#141414");
    QColor iconColor = QColor("#1677ff");
    QColor closeIconColor = QColor("#8c8c8c");
    QColor closeButtonBackground = QColor(Qt::transparent);
    QColor closeButtonHoverBackground = QColor(0, 0, 0, 20);
    QColor closeButtonPressedBackground = QColor(0, 0, 0, 20);
    QColor closeButtonDisabledBackground = QColor(Qt::transparent);
    QColor closeButtonBorderColor = QColor(Qt::transparent);
    int zIndex = 1000;
    int width = 520;
    int borderRadius = 8;
    int borderWidth = 0;
    int contentPaddingHorizontal = 24;
    int contentPaddingVertical = 20;
    int headerPaddingHorizontal = 0;
    int headerPaddingVertical = 0;
    int headerMarginBottom = 8;
    int bodyPaddingHorizontal = 0;
    int bodyPaddingVertical = 0;
    int footerPaddingHorizontal = 0;
    int footerPaddingVertical = 0;
    int footerMarginTop = 12;
    int footerBorderTopWidth = 0;
    int footerButtonGap = 8;
    int confirmIconGap = 12;
    int confirmParagraphGap = 8;
    int textLineHeight = 22;
    int titleLineHeight = 24;
    int iconSize = 16;
    int closeButtonSize = 32;
    int closeIconSize = 16;
    int closeButtonRadius = 4;
    qreal closeButtonBorderWidth = 0.0;
    QFont titleFont;
    QFont bodyFont;
  };

  static void registerOpenModal(AdModal* modal);
  static void unregisterOpenModal(AdModal* modal);
  static void restackOpenModals(QWidget* ownerWindow);
  static void registerStaticServiceModal(AdModal* modal);
  static void unregisterStaticServiceModal(AdModal* modal);

  QWidget* resolveOwnerWindow() const;
  const QWidget* themeLogicalOwner() const;
  const QWidget* themeSourceWidget() const;
  QRect windowModeAvailableGeometry() const;
  QRect windowModeAnchorGeometry() const;

  QWidget* ensureParkingWidget();
  void attachOwnerWindowWatcher(QWidget* ownerWindow);
  void detachOwnerWindowWatcher();
  void attachRenderContainerWatcher(QWidget* renderContainer);
  void detachRenderContainerWatcher();
  bool usesWindowSurface() const;

  void ensureOverlay();
  void releaseOverlay();
  void syncOverlayGeometry();
  void syncWindowModeGeometry();
  void refreshLayout();
  void refreshTexts();
  void refreshVisibility();
  void refreshTitleIcon();
  void applyVisualStyle();
  void updateAccessibility();
  void saveFocusBeforeOpen();
  void restoreFocusAfterClose();
  bool focusNextPrevChildInModal(bool next);
  QWidget* firstFocusableWidget(bool reverse = false) const;
  QWidget* nextFocusableFrom(QWidget* start, bool next) const;
  QWidget* resolveInitialFocusTarget() const;
  VisualStyle resolveVisualStyle() const;
  void attachContentWidget(QWidget* widget);
  void attachFooterWidget(QWidget* widget);
  void clearContentWidget(bool deleteWidget);
  void clearFooterWidget(bool deleteWidget);
  void setOpenInternal(bool value, bool emitSignal);
  void requestAccept();
  void requestReject(CloseReason reason, bool ignoreBusy = false);
  bool emitCloseRequestedSafely(CloseReason reason);
  CloseReason effectiveProgrammaticReason(CloseReason fallback) const;
  void finalizeClose(DialogCode code, CloseReason reason);

  static QColor parseColorToken(const std::optional<QColor>& token, const QColor& fallback);
  static void applySemanticSlot(const SemanticSlotStyle& slot, QColor* textColor,
                                QColor* backgroundColor, QColor* borderColor);

  ClosePolicy closePolicy_ = ClosePolicy::Automatic;
  Mode mode_ = Mode::Overlay;
  Qt::WindowModality windowModality_ = Qt::WindowModal;
  bool windowModeDetached_ = false;
  bool open_ = false;
  QString windowTitle_;
  bool centered_ = false;
  int preferredWidth_ = 520;
  int topOffset_ = 100;
  bool maskVisible_ = true;
  bool closeOnMaskClick_ = true;
  bool closeOnEscape_ = true;
  bool closeButtonVisible_ = true;
  bool footerVisible_ = true;
  StandardButtons standardButtons_ = StandardButtons(StandardButton::Ok) | StandardButton::Cancel;
  bool acceptButtonBusy_ = false;
  bool contentLoading_ = false;
  DialogCode result_ = DialogCode::Rejected;
  QString text_;
  QString acceptText_ = QStringLiteral("OK");
  QString rejectText_ = QStringLiteral("Cancel");
  bool acceptTextExplicit_ = false;
  bool rejectTextExplicit_ = false;
  AdButton::AccentRole acceptAccentRole_ = AdButton::AccentRole::Primary;
  AdButton::ButtonStyle acceptButtonStyle_ = AdButton::ButtonStyle::Solid;
  Preset preset_ = Preset::Plain;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;

  QPointer<QWidget> ownerWindow_;
  QPointer<QWidget> renderContainer_;
  QPointer<QWidget> parkingWidget_;
  QPointer<QWidget> overlay_;
  QPointer<QVBoxLayout> overlayLayout_;
  QPointer<QFrame> panel_;
  QPointer<QVBoxLayout> panelLayout_;
  QPointer<QWidget> header_;
  QPointer<QHBoxLayout> headerLayout_;
  QPointer<QLabel> titleIconLabel_;
  QPointer<QLabel> titleLabel_;
  QPointer<QToolButton> closeButton_;
  QPointer<QWidget> body_;
  QPointer<QVBoxLayout> bodyLayout_;
  QPointer<QWidget> confirmBodyHost_;
  QPointer<QHBoxLayout> confirmBodyLayout_;
  QPointer<QVBoxLayout> confirmParagraphLayout_;
  QPointer<QLabel> contentLabel_;
  QPointer<QLabel> confirmTitleLabel_;
  QPointer<QLabel> confirmContentLabel_;
  QPointer<QWidget> contentWidget_;
  QPointer<QWidget> footer_;
  QPointer<QHBoxLayout> footerLayout_;
  QPointer<QWidget> footerButtonsHost_;
  QPointer<QHBoxLayout> footerButtonsLayout_;
  QPointer<AdButton> rejectButtonControl_;
  QPointer<AdButton> acceptButtonControl_;
  QPointer<QWidget> footerWidget_;
  QPointer<QShortcut> escShortcut_;
  QPointer<QWidget> initialFocusWidget_;
  QPointer<QWidget> focusBeforeOpen_;
  QMetaObject::Connection contentWidgetDestroyedConnection_;
  QMetaObject::Connection footerWidgetDestroyedConnection_;
  QMetaObject::Connection ownerWindowDestroyedConnection_;
  QMetaObject::Connection renderContainerDestroyedConnection_;

  bool emittingCloseRequested_ = false;
  bool staticServiceOwned_ = false;
  bool deletionScheduled_ = false;
  bool syncingWindowModeGeometry_ = false;
  QPointer<QScreen> windowScreen_;
  QSize windowPreferredSize_;
  QSize windowMinimumSize_;
  bool windowResizable_ = false;
  bool windowGeometryInitialized_ = false;
  int resolvedZIndex_ = 1000;
  quint64 openSequence_ = 0;
  std::optional<CloseReason> pendingCloseReason_;
};

class AdModalService final {
 public:
  using ActionHandler = std::function<void(AdModal*)>;

  struct Request {
    std::optional<AdModal::Preset> preset;
    std::optional<AdModal::Mode> mode;
    std::optional<QString> title;
    std::optional<QString> text;
    std::optional<QString> acceptText;
    std::optional<QString> rejectText;
    std::optional<AdModal::StandardButtons> standardButtons;
    std::optional<bool> centered;
    std::optional<bool> closeButtonVisible;
    std::optional<bool> maskVisible;
    std::optional<bool> closeOnMaskClick;
    std::optional<bool> closeOnEscape;
    std::optional<bool> footerVisible;
    std::optional<bool> acceptButtonBusy;
    std::optional<bool> contentLoading;
    std::optional<int> preferredWidth;
    std::optional<int> topOffset;
    std::optional<AdButton::AccentRole> acceptAccentRole;
    std::optional<AdButton::ButtonStyle> acceptButtonStyle;
    ActionHandler onAccept;
    ActionHandler onReject;
  };

  static AdModal* showInfo(const Request& request = {}, QWidget* ownerWindow = nullptr);
  static AdModal* showSuccess(const Request& request = {}, QWidget* ownerWindow = nullptr);
  static AdModal* showError(const Request& request = {}, QWidget* ownerWindow = nullptr);
  static AdModal* showWarning(const Request& request = {}, QWidget* ownerWindow = nullptr);
  static AdModal* showConfirm(const Request& request = {}, QWidget* ownerWindow = nullptr);
  static void closeAll();

 private:
  static AdModal* showStaticRequest(const Request& request, AdModal::Preset defaultPreset,
                                    QWidget* ownerWindow);
};

}  // namespace adqt::widgets

Q_DECLARE_OPERATORS_FOR_FLAGS(adqt::widgets::AdModal::StandardButtons)
Q_DECLARE_METATYPE(adqt::widgets::AdModal::ClosePolicy)
Q_DECLARE_METATYPE(adqt::widgets::AdModal::Mode)
Q_DECLARE_METATYPE(adqt::widgets::AdModal::Preset)
Q_DECLARE_METATYPE(adqt::widgets::AdModal::DialogCode)
Q_DECLARE_METATYPE(adqt::widgets::AdModal::StandardButtons)
Q_DECLARE_METATYPE(adqt::widgets::AdModal::CloseReason)
