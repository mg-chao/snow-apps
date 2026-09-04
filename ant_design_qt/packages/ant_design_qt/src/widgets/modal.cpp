#include "modal.h"

#include "antd_icons.h"
#include "theme/theme.h"

#include <QApplication>
#include <QAbstractButton>
#include <QByteArray>
#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QToolButton>
#include <QVector>
#include <QVBoxLayout>
#include <QWindow>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 28301)
#endif
#include <dwmapi.h>
#include <qt_windows.h>
#include <windowsx.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Some Windows SDK headers define min/max macros unless NOMINMAX was set
// before their first inclusion.  Keep this translation unit safe even when a
// transitive include has already pulled those headers in.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include <algorithm>
#include <cmath>

namespace adqt::widgets {

namespace {

namespace filled_icons = adqt::icons::antd::filled;
namespace outlined_icons = adqt::icons::antd::outlined;

#if defined(Q_OS_WIN) || defined(_WIN32)
constexpr int kWindowModeDwmFrameMargin = 1;
#endif
constexpr int kWindowModeFallbackDragHeight = 48;

QWidget* deepestChildAt(QWidget* root, const QPoint& rootLocalPos) {
  if (!root) {
    return nullptr;
  }

  QWidget* current = root;
  QPoint currentLocalPos = rootLocalPos;
  while (QWidget* child = current->childAt(currentLocalPos)) {
    current = child;
    currentLocalPos = current->mapFrom(root, rootLocalPos);
  }
  return current;
}

bool blocksWindowDrag(QWidget* widget, const QWidget* boundary) {
  for (QWidget* current = widget; current && current != boundary;
       current = current->parentWidget()) {
    if (current->testAttribute(Qt::WA_TransparentForMouseEvents) || !current->isEnabled()) {
      continue;
    }
    if (qobject_cast<QAbstractButton*>(current)) {
      return true;
    }

    switch (current->focusPolicy()) {
      case Qt::ClickFocus:
      case Qt::StrongFocus:
      case Qt::WheelFocus:
        return true;
      default:
        break;
    }
  }
  return false;
}

bool isNonInteractiveDragArea(QWidget* root, const QPoint& rootLocalPos) {
  if (!root || !root->isVisible() || !root->rect().contains(rootLocalPos)) {
    return false;
  }

  return !blocksWindowDrag(deepestChildAt(root, rootLocalPos), root);
}

class ModalPanelWidget final : public QFrame {
 public:
  struct PaintStyle {
    QColor containerBg = QColor("#ffffff");
    QColor headerBg = QColor("#ffffff");
    QColor bodyBg = QColor("#ffffff");
    QColor footerBg = QColor("#ffffff");
    QColor borderColor = QColor("#f0f0f0");
    int borderRadius = 8;
    int borderWidth = 0;
    int footerBorderTopWidth = 0;
  };

  explicit ModalPanelWidget(QWidget* parent = nullptr) : QFrame(parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
  }

  void setSectionWidgets(QWidget* header, QWidget* body, QWidget* footer) {
    header_ = header;
    body_ = body;
    footer_ = footer;
    update();
  }

  void setPaintStyle(const PaintStyle& style) {
    paintStyle_ = style;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    const QRect widgetRect = rect();
    if (widgetRect.width() <= 0 || widgetRect.height() <= 0) {
      return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal borderWidth = std::max(0.0, static_cast<qreal>(paintStyle_.borderWidth));
    const qreal borderRadius = std::max(0.0, static_cast<qreal>(paintStyle_.borderRadius));
    QRectF fillRect(widgetRect);
    if (borderWidth > 0.0) {
      const qreal inset = borderWidth / 2.0;
      fillRect.adjust(inset, inset, -inset, -inset);
    }
    if (fillRect.width() <= 0.0 || fillRect.height() <= 0.0) {
      return;
    }

    const qreal maxRadius = std::min(fillRect.width(), fillRect.height()) / 2.0;
    const qreal effectiveRadius = std::min(borderRadius, maxRadius);

    QPainterPath panelPath;
    panelPath.addRoundedRect(fillRect, effectiveRadius, effectiveRadius);

    painter.fillPath(panelPath, paintStyle_.containerBg);

    painter.save();
    painter.setClipPath(panelPath);
    fillSection(painter, header_, paintStyle_.headerBg);
    fillSection(painter, body_, paintStyle_.bodyBg);
    fillSection(painter, footer_, paintStyle_.footerBg);

    if (footer_ && footer_->isVisible() && paintStyle_.footerBorderTopWidth > 0 &&
        paintStyle_.borderColor.alpha() > 0) {
      const QRect footerRect = footer_->geometry();
      const qreal separatorWidth = static_cast<qreal>(paintStyle_.footerBorderTopWidth);
      QPen separatorPen(paintStyle_.borderColor);
      separatorPen.setWidthF(separatorWidth);
      separatorPen.setCapStyle(Qt::FlatCap);
      painter.setPen(separatorPen);

      const qreal y = std::clamp(static_cast<qreal>(footerRect.top()) + separatorWidth / 2.0,
                                 fillRect.top(), fillRect.bottom());
      painter.drawLine(QPointF(fillRect.left(), y), QPointF(fillRect.right(), y));
    }
    painter.restore();

    if (borderWidth > 0.0 && paintStyle_.borderColor.alpha() > 0) {
      QPen borderPen(paintStyle_.borderColor);
      borderPen.setWidthF(borderWidth);
      borderPen.setJoinStyle(Qt::RoundJoin);
      painter.setPen(borderPen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPath(panelPath);
    }
  }

 private:
  static void fillSection(QPainter& painter, QWidget* section, const QColor& color) {
    if (!section || !section->isVisible() || color.alpha() <= 0) {
      return;
    }
    painter.fillRect(section->geometry(), color);
  }

  QWidget* header_ = nullptr;
  QWidget* body_ = nullptr;
  QWidget* footer_ = nullptr;
  PaintStyle paintStyle_;
};

class ModalOverlayWidget final : public QWidget {
 public:
  explicit ModalOverlayWidget(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags())
      : QWidget(parent, flags) {
    setAttribute(Qt::WA_StyledBackground, false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
  }

  void setFocusNavigator(std::function<bool(bool)> navigator) {
    focusNavigator_ = std::move(navigator);
  }

  void setWindowModeChromeEnabled(bool enabled) {
    if (windowModeChromeEnabled_ == enabled) {
      return;
    }
    windowModeChromeEnabled_ = enabled;
    if (enabled) {
      setAttribute(Qt::WA_TranslucentBackground, false);
    }
    applyWindowModeNativeChrome();
  }

  void setWindowModeDragWidgets(QWidget* header, QWidget* panel) {
    windowModeHeader_ = header;
    windowModePanel_ = panel;
    installWindowModeDragFilter(header);
    installWindowModeDragFilter(panel);
  }

  void setRootColor(const QColor& color) {
    if (rootColor_ == color) {
      return;
    }
    rootColor_ = color;
    update();
  }

  void setMaskEnabled(bool enabled) {
    if (maskEnabled_ == enabled) {
      return;
    }
    maskEnabled_ = enabled;
    update();
  }

  void setMaskColor(const QColor& color) {
    if (maskColor_ == color) {
      return;
    }
    maskColor_ = color;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* event) override {
    Q_UNUSED(event)

    QPainter painter(this);
    if (rootColor_.alpha() > 0) {
      painter.fillRect(rect(), rootColor_);
    }
    if (maskEnabled_ && maskColor_.alpha() > 0) {
      painter.fillRect(rect(), maskColor_);
    }
  }

  bool focusNextPrevChild(bool next) override {
    if (focusNavigator_ && focusNavigator_(next)) {
      return true;
    }
    return QWidget::focusNextPrevChild(next);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (handleWindowModeDragMousePress(watched, event)) {
      return true;
    }
    return QWidget::eventFilter(watched, event);
  }

  bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override {
    Q_UNUSED(eventType)
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (handleWindowModeNativeEvent(message, result)) {
      return true;
    }
#else
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
  }

 private:
#if defined(Q_OS_WIN) || defined(_WIN32)
  static HWND hwndForWidget(QWidget* widget) {
    if (!widget) {
      return nullptr;
    }
    // Qt transports the native HWND through its integer-valued WId type.
    return reinterpret_cast<HWND>(widget->winId());  // NOLINT(performance-no-int-to-ptr)
  }
#endif

  void applyWindowModeNativeChrome() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!windowModeChromeEnabled_) {
      return;
    }

    const HWND hwnd = hwndForWidget(this);
    if (hwnd == nullptr) {
      return;
    }

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    style &= ~(WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    exStyle &= ~WS_EX_LAYERED;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

    const DWMNCRENDERINGPOLICY renderingPolicy = DWMNCRP_ENABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &renderingPolicy,
                          sizeof(renderingPolicy));

    const MARGINS margins = {kWindowModeDwmFrameMargin, kWindowModeDwmFrameMargin,
                             kWindowModeDwmFrameMargin, kWindowModeDwmFrameMargin};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
  }

  void installWindowModeDragFilter(QWidget* root) {
    if (!root) {
      return;
    }

    root->installEventFilter(this);
    const QList<QWidget*> children = root->findChildren<QWidget*>();
    for (QWidget* child : children) {
      if (child) {
        child->installEventFilter(this);
      }
    }
  }

  bool isWindowModeDragAreaAt(const QPoint& globalPos) const {
    if (!windowModeChromeEnabled_) {
      return false;
    }

    QWidget* panel = windowModePanel_.data();
    if (!panel || !panel->isVisible()) {
      return false;
    }

    const QPoint panelLocalPos = panel->mapFromGlobal(globalPos);
    if (!panel->rect().contains(panelLocalPos)) {
      return false;
    }

    QWidget* header = windowModeHeader_.data();
    int dragHeight = std::min(kWindowModeFallbackDragHeight, panel->height());
    if (header && header->isVisible()) {
      const QRect headerRect(header->mapTo(panel, QPoint(0, 0)), header->size());
      dragHeight = std::clamp(headerRect.bottom() + 1, 0, panel->height());
    }

    if (panelLocalPos.y() >= dragHeight) {
      return false;
    }

    return isNonInteractiveDragArea(panel, panelLocalPos);
  }

  bool startWindowModeDrag() {
    if (QWindow* handle = windowHandle(); handle && handle->startSystemMove()) {
      return true;
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND hwnd = hwndForWidget(this);
    if (hwnd == nullptr) {
      return false;
    }
    POINT cursorPos{};
    GetCursorPos(&cursorPos);
    ReleaseCapture();
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursorPos.x, cursorPos.y));
    return true;
#else
    return false;
#endif
  }

  bool handleWindowModeDragMousePress(QObject* watched, QEvent* event) {
    if (!windowModeChromeEnabled_ || event == nullptr ||
        event->type() != QEvent::MouseButtonPress) {
      return false;
    }

    auto* watchedWidget = qobject_cast<QWidget*>(watched);
    if (!watchedWidget || watchedWidget->window() != this) {
      return false;
    }

    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (mouseEvent->button() != Qt::LeftButton) {
      return false;
    }

    const QPoint globalPos = mouseEvent->globalPosition().toPoint();
    if (!isWindowModeDragAreaAt(globalPos)) {
      return false;
    }

    mouseEvent->accept();
    return startWindowModeDrag();
  }

#if defined(Q_OS_WIN) || defined(_WIN32)
  bool handleWindowModeNativeEvent(void* message, qintptr* result) const {
    if (!windowModeChromeEnabled_ || message == nullptr || result == nullptr) {
      return false;
    }

    const auto* msg = static_cast<const MSG*>(message);
    switch (msg->message) {
      case WM_NCCALCSIZE:
        *result = 0;
        return true;

      case WM_NCHITTEST: {
        LRESULT dwmResult = 0;
        if (DwmDefWindowProc(msg->hwnd, msg->message, msg->wParam, msg->lParam, &dwmResult) != 0) {
          *result = dwmResult;
          return true;
        }

        *result = isWindowModeDragAreaAt(QCursor::pos()) ? HTCAPTION : HTCLIENT;
        return true;
      }

      default:
        break;
    }

    return false;
  }
#endif

  QColor rootColor_ = QColor(0, 0, 0, 0);
  QColor maskColor_ = QColor(0, 0, 0, 115);
  bool maskEnabled_ = true;
  bool windowModeChromeEnabled_ = false;
  QPointer<QWidget> windowModeHeader_;
  QPointer<QWidget> windowModePanel_;
  std::function<bool(bool)> focusNavigator_;
};

class ModalIconButton final : public QToolButton {
 public:
  explicit ModalIconButton(QWidget* parent = nullptr) : QToolButton(parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setAutoRaise(false);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
  }

  void setVisualStyle(const QColor& normalBackground, const QColor& hoverBackground,
                      const QColor& pressedBackground, const QColor& disabledBackground,
                      const QColor& borderColor, qreal borderWidth, qreal radius) {
    normalBackground_ = normalBackground;
    hoverBackground_ = hoverBackground;
    pressedBackground_ = pressedBackground;
    disabledBackground_ = disabledBackground;
    borderColor_ = borderColor;
    borderWidth_ = std::max<qreal>(0.0, borderWidth);
    radius_ = std::max<qreal>(0.0, radius);
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

    QColor background = isEnabled() ? normalBackground_ : disabledBackground_;
    if (isEnabled() && isDown()) {
      background = pressedBackground_.isValid() ? pressedBackground_ : hoverBackground_;
    } else if (isEnabled() && hovered_) {
      background = hoverBackground_.isValid() ? hoverBackground_ : normalBackground_;
    }

    if ((background.isValid() && background.alpha() > 0) ||
        (borderColor_.isValid() && borderColor_.alpha() > 0 && borderWidth_ > 0.0)) {
      QRectF bounds = rect();
      const qreal inset = borderWidth_ > 0.0 ? borderWidth_ / 2.0 : 0.5;
      bounds.adjust(inset, inset, -inset, -inset);
      const qreal radius = std::min(radius_, std::min(bounds.width(), bounds.height()) * 0.5);
      painter.setPen((borderColor_.isValid() && borderColor_.alpha() > 0 && borderWidth_ > 0.0)
                         ? QPen(borderColor_, borderWidth_)
                         : Qt::NoPen);
      painter.setBrush((background.isValid() && background.alpha() > 0) ? QBrush(background)
                                                                        : QBrush(Qt::NoBrush));
      painter.drawRoundedRect(bounds, radius, radius);
    }

    const QIcon currentIcon = icon();
    if (!currentIcon.isNull()) {
      const QIcon::Mode mode =
          !isEnabled() ? QIcon::Disabled : ((hovered_ || isDown()) ? QIcon::Active : QIcon::Normal);
      const QSize logicalSize = iconSize().isValid() ? iconSize() : QSize(16, 16);
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
  QColor normalBackground_ = QColor(Qt::transparent);
  QColor hoverBackground_ = QColor(Qt::transparent);
  QColor pressedBackground_ = QColor(Qt::transparent);
  QColor disabledBackground_ = QColor(Qt::transparent);
  QColor borderColor_ = QColor(Qt::transparent);
  qreal borderWidth_ = 0.0;
  qreal radius_ = 0.0;
};

QVector<QPointer<AdModal>>& staticModals() {
  static QVector<QPointer<AdModal>> modals;
  return modals;
}

QVector<QPointer<AdModal>>& openModals() {
  static QVector<QPointer<AdModal>> modals;
  return modals;
}

quint64& nextOpenModalSequence() {
  static quint64 value = 1;
  return value;
}

QColor toColor(const QColor& value, const QColor& fallback) {
  return value.isValid() ? value : fallback;
}

QWidget* normalizeOwnerWindow(QWidget* widget) {
  if (!widget) {
    return nullptr;
  }
  if (QWidget* window = widget->window()) {
    return window;
  }
  return widget;
}

bool isFocusableWidget(QWidget* widget) {
  if (!widget || !widget->isVisible() || !widget->isEnabled()) {
    return false;
  }
  const Qt::FocusPolicy focusPolicy = widget->focusPolicy();
  return focusPolicy == Qt::TabFocus || focusPolicy == Qt::StrongFocus ||
         focusPolicy == Qt::WheelFocus;
}

}  // namespace

AdModal* AdModalService::showStaticRequest(const AdModalService::Request& request,
                                           AdModal::Preset defaultPreset, QWidget* ownerWindow) {
  QWidget* resolvedOwnerWindow = ownerWindow ? ownerWindow : QApplication::activeWindow();
  auto* modal = new AdModal(resolvedOwnerWindow);
  modal->staticServiceOwned_ = true;
  modal->setCentered(true);
  modal->setMaskVisible(true);
  modal->setCloseOnMaskClick(false);
  modal->setCloseOnEscape(true);
  modal->setCloseButtonVisible(false);
  modal->setFooterVisible(true);
  modal->setPreset(request.preset.value_or(defaultPreset));
  modal->setStandardButtons(modal->preset() == AdModal::Preset::Confirm
                                ? (AdModal::StandardButton::Ok | AdModal::StandardButton::Cancel)
                                : AdModal::StandardButton::Ok);
  modal->setPreferredWidth(416);
  modal->setOwnerWindow(resolvedOwnerWindow);

  switch (modal->preset()) {
    case AdModal::Preset::Info:
      modal->setWindowTitle(AdModal::tr("Information"));
      break;
    case AdModal::Preset::Success:
      modal->setWindowTitle(AdModal::tr("Success"));
      break;
    case AdModal::Preset::Error:
      modal->setWindowTitle(AdModal::tr("Error"));
      break;
    case AdModal::Preset::Warning:
      modal->setWindowTitle(AdModal::tr("Warning"));
      break;
    case AdModal::Preset::Confirm:
      modal->setWindowTitle(AdModal::tr("Confirm"));
      break;
    case AdModal::Preset::Plain:
      break;
  }

  if (request.title.has_value()) {
    modal->setWindowTitle(request.title.value());
  }
  if (request.mode.has_value()) {
    modal->setMode(request.mode.value());
  }
  if (request.text.has_value()) {
    modal->setText(request.text.value());
  }
  if (request.acceptText.has_value()) {
    modal->setAcceptText(request.acceptText.value());
  }
  if (request.rejectText.has_value()) {
    modal->setRejectText(request.rejectText.value());
  }
  if (request.standardButtons.has_value()) {
    modal->setStandardButtons(request.standardButtons.value());
  }
  if (request.centered.has_value()) {
    modal->setCentered(request.centered.value());
  }
  if (request.closeButtonVisible.has_value()) {
    modal->setCloseButtonVisible(request.closeButtonVisible.value());
  }
  if (request.maskVisible.has_value()) {
    modal->setMaskVisible(request.maskVisible.value());
  }
  if (request.closeOnMaskClick.has_value()) {
    modal->setCloseOnMaskClick(request.closeOnMaskClick.value());
  }
  if (request.closeOnEscape.has_value()) {
    modal->setCloseOnEscape(request.closeOnEscape.value());
  }
  if (request.footerVisible.has_value()) {
    modal->setFooterVisible(request.footerVisible.value());
  }
  if (request.acceptButtonBusy.has_value()) {
    modal->setAcceptButtonBusy(request.acceptButtonBusy.value());
  }
  if (request.contentLoading.has_value()) {
    modal->setContentLoading(request.contentLoading.value());
  }
  if (request.preferredWidth.has_value()) {
    modal->setPreferredWidth(request.preferredWidth.value());
  }
  if (request.topOffset.has_value()) {
    modal->setTopOffset(request.topOffset.value());
  }
  if (request.acceptAccentRole.has_value()) {
    modal->setAcceptAccentRole(request.acceptAccentRole.value());
  }
  if (request.acceptButtonStyle.has_value()) {
    modal->setAcceptButtonStyle(request.acceptButtonStyle.value());
  }

  if (request.onAccept || request.onReject) {
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    QObject::connect(modal, &AdModal::closeRequested, modal,
                     [modal, request](AdModal::CloseReason reason) {
                       if (!modal) {
                         return;
                       }
                       if (reason == AdModal::CloseReason::OkAction) {
                         if (request.onAccept) {
                           request.onAccept(modal);
                         } else {
                           modal->accept();
                         }
                         return;
                       }
                       if (request.onReject) {
                         request.onReject(modal);
                       } else {
                         modal->reject();
                       }
                     });
  }

  AdModal::registerStaticServiceModal(modal);
  QObject::connect(modal, &AdModal::finished, modal, &QObject::deleteLater);
  modal->open();
  return modal;
}

AdModal::AdModal(QObject* parent) : QObject(parent) {
  if (auto* parentWidget = qobject_cast<QWidget*>(parent)) {
    ownerWindow_ = normalizeOwnerWindow(parentWidget);
    attachOwnerWindowWatcher(ownerWindow_);
  }
  QObject::connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                   this, &AdModal::applyVisualStyle);
}

AdModal::~AdModal() {
  unregisterOpenModal(this);
  detachOwnerWindowWatcher();
  detachRenderContainerWatcher();
  releaseOverlay();
  unregisterStaticServiceModal(this);
  if (parkingWidget_) {
    delete parkingWidget_.data();
    parkingWidget_.clear();
  }
}

AdModal::ClosePolicy AdModal::closePolicy() const { return closePolicy_; }

void AdModal::setClosePolicy(ClosePolicy value) {
  if (closePolicy_ == value) {
    return;
  }
  closePolicy_ = value;
  emit closePolicyChanged(closePolicy_);
}

AdModal::Mode AdModal::mode() const { return mode_; }

void AdModal::setMode(Mode value) {
  if (mode_ == value) {
    return;
  }

  const bool reopen = open_;
  if (reopen) {
    unregisterOpenModal(this);
  }

  mode_ = value;
  emit modeChanged(mode_);

  if (overlay_) {
    releaseOverlay();
  }

  if (reopen) {
    ensureOverlay();
    registerOpenModal(this);
    setOpenInternal(true, false);
  }
}

Qt::WindowModality AdModal::windowModality() const { return windowModality_; }

void AdModal::setWindowModality(Qt::WindowModality value) {
  if (windowModality_ == value) {
    return;
  }

  windowModality_ = value;
  emit windowModalityChanged(windowModality_);
  if (overlay_ && usesWindowSurface()) {
    overlay_->setWindowModality(windowModality_);
  }
}

bool AdModal::windowModeDetached() const { return windowModeDetached_; }

void AdModal::setWindowModeDetached(bool value) {
  if (windowModeDetached_ == value) {
    return;
  }

  const bool reopen = open_;
  if (reopen) {
    unregisterOpenModal(this);
  }

  windowModeDetached_ = value;
  emit windowModeDetachedChanged(windowModeDetached_);

  if (overlay_ && usesWindowSurface()) {
    releaseOverlay();
  }

  if (reopen) {
    ensureOverlay();
    registerOpenModal(this);
    setOpenInternal(true, false);
  }
}

bool AdModal::isOpen() const { return open_; }

void AdModal::setOpen(bool value) {
  if (value) {
    if (open_) {
      ensureOverlay();
      refreshTexts();
      refreshVisibility();
      refreshLayout();
      applyVisualStyle();
      syncOverlayGeometry();
      if (overlay_) {
        overlay_->show();
        restackOpenModals(ownerWindow_ ? ownerWindow_.data() : resolveOwnerWindow());
        if (usesWindowSurface()) {
          syncOverlayGeometry();
          overlay_->raise();
          overlay_->activateWindow();
        }
      }
      return;
    }
    result_ = DialogCode::Rejected;
    pendingCloseReason_.reset();
    setOpenInternal(true, true);
    return;
  }

  if (!open_) {
    return;
  }
  finalizeClose(DialogCode::Rejected, effectiveProgrammaticReason(CloseReason::Programmatic));
}

void AdModal::open() { setOpen(true); }

QString AdModal::windowTitle() const { return windowTitle_; }

void AdModal::setWindowTitle(const QString& value) {
  if (windowTitle_ == value) {
    return;
  }
  windowTitle_ = value;
  emit windowTitleChanged(windowTitle_);
  refreshTexts();
  refreshVisibility();
  applyVisualStyle();
}

bool AdModal::centered() const { return centered_; }

void AdModal::setCentered(bool value) {
  if (centered_ == value) {
    return;
  }
  centered_ = value;
  emit centeredChanged(centered_);
  refreshLayout();
}

int AdModal::preferredWidth() const { return preferredWidth_; }

void AdModal::setPreferredWidth(int value) {
  const int clamped = std::max(240, value);
  if (preferredWidth_ == clamped) {
    return;
  }
  preferredWidth_ = clamped;
  emit preferredWidthChanged(preferredWidth_);
  applyVisualStyle();
}

int AdModal::topOffset() const { return topOffset_; }

void AdModal::setTopOffset(int value) {
  const int clamped = std::max(0, value);
  if (topOffset_ == clamped) {
    return;
  }
  topOffset_ = clamped;
  emit topOffsetChanged(topOffset_);
  refreshLayout();
}

bool AdModal::maskVisible() const { return maskVisible_; }

void AdModal::setMaskVisible(bool value) {
  if (maskVisible_ == value) {
    return;
  }
  maskVisible_ = value;
  emit maskVisibleChanged(maskVisible_);
  applyVisualStyle();
}

bool AdModal::closeOnMaskClick() const { return closeOnMaskClick_; }

void AdModal::setCloseOnMaskClick(bool value) {
  if (closeOnMaskClick_ == value) {
    return;
  }
  closeOnMaskClick_ = value;
  emit closeOnMaskClickChanged(closeOnMaskClick_);
}

bool AdModal::closeOnEscape() const { return closeOnEscape_; }

void AdModal::setCloseOnEscape(bool value) {
  if (closeOnEscape_ == value) {
    return;
  }
  closeOnEscape_ = value;
  emit closeOnEscapeChanged(closeOnEscape_);
}

bool AdModal::closeButtonVisible() const { return closeButtonVisible_; }

void AdModal::setCloseButtonVisible(bool value) {
  if (closeButtonVisible_ == value) {
    return;
  }
  closeButtonVisible_ = value;
  emit closeButtonVisibleChanged(closeButtonVisible_);
  refreshVisibility();
}

bool AdModal::footerVisible() const { return footerVisible_; }

void AdModal::setFooterVisible(bool value) {
  if (footerVisible_ == value) {
    return;
  }
  footerVisible_ = value;
  emit footerVisibleChanged(footerVisible_);
  refreshVisibility();
}

AdModal::StandardButtons AdModal::standardButtons() const { return standardButtons_; }

void AdModal::setStandardButtons(StandardButtons value) {
  if (standardButtons_ == value) {
    return;
  }
  standardButtons_ = value;
  emit standardButtonsChanged(standardButtons_);
  refreshVisibility();
}

bool AdModal::acceptButtonBusy() const { return acceptButtonBusy_; }

void AdModal::setAcceptButtonBusy(bool value) {
  if (acceptButtonBusy_ == value) {
    return;
  }
  acceptButtonBusy_ = value;
  emit acceptButtonBusyChanged(acceptButtonBusy_);
  refreshTexts();
}

bool AdModal::contentLoading() const { return contentLoading_; }

void AdModal::setContentLoading(bool value) {
  if (contentLoading_ == value) {
    return;
  }
  contentLoading_ = value;
  emit contentLoadingChanged(contentLoading_);
  refreshTexts();
  refreshVisibility();
}

QString AdModal::text() const { return text_; }

void AdModal::setText(const QString& value) {
  if (text_ == value) {
    return;
  }
  text_ = value;
  emit textChanged(text_);
  refreshTexts();
}

QString AdModal::acceptText() const { return acceptText_; }

void AdModal::setAcceptText(const QString& value) {
  if (acceptText_ == value) {
    return;
  }
  acceptText_ = value;
  acceptTextExplicit_ = true;
  emit acceptTextChanged(acceptText_);
  refreshTexts();
}

QString AdModal::rejectText() const { return rejectText_; }

void AdModal::setRejectText(const QString& value) {
  if (rejectText_ == value) {
    return;
  }
  rejectText_ = value;
  rejectTextExplicit_ = true;
  emit rejectTextChanged(rejectText_);
  refreshTexts();
}

AdButton::AccentRole AdModal::acceptAccentRole() const { return acceptAccentRole_; }

void AdModal::setAcceptAccentRole(AdButton::AccentRole value) {
  if (acceptAccentRole_ == value) {
    return;
  }
  acceptAccentRole_ = value;
  emit acceptAccentRoleChanged(acceptAccentRole_);
  refreshTexts();
}

AdButton::ButtonStyle AdModal::acceptButtonStyle() const { return acceptButtonStyle_; }

void AdModal::setAcceptButtonStyle(AdButton::ButtonStyle value) {
  if (acceptButtonStyle_ == value) {
    return;
  }
  acceptButtonStyle_ = value;
  emit acceptButtonStyleChanged(acceptButtonStyle_);
  refreshTexts();
}

AdModal::Preset AdModal::preset() const { return preset_; }

void AdModal::setPreset(Preset value) {
  if (preset_ == value) {
    return;
  }
  preset_ = value;
  emit presetChanged(preset_);
  refreshVisibility();
  applyVisualStyle();
}

QWidget* AdModal::ownerWindow() const { return ownerWindow_; }

void AdModal::setOwnerWindow(QWidget* value) {
  QWidget* normalizedOwnerWindow = normalizeOwnerWindow(value);
  if (ownerWindow_ == normalizedOwnerWindow) {
    return;
  }

  const bool reopen = open_;
  if (reopen) {
    unregisterOpenModal(this);
  }

  detachOwnerWindowWatcher();
  ownerWindow_ = normalizedOwnerWindow;
  attachOwnerWindowWatcher(ownerWindow_);
  emit ownerWindowChanged(ownerWindow_);

  if (reopen) {
    releaseOverlay();
    ensureOverlay();
    registerOpenModal(this);
    setOpenInternal(true, false);
  }
}

QWidget* AdModal::renderContainer() const { return renderContainer_; }

void AdModal::setRenderContainer(QWidget* value) {
  if (renderContainer_ == value) {
    return;
  }

  const bool reopen = open_;
  if (reopen) {
    unregisterOpenModal(this);
  }

  detachRenderContainerWatcher();
  releaseOverlay();
  renderContainer_ = value;
  attachRenderContainerWatcher(renderContainer_);
  emit renderContainerChanged(renderContainer_);

  if (reopen) {
    ensureOverlay();
    registerOpenModal(this);
    setOpenInternal(true, false);
  }
}

QWidget* AdModal::contentWidget() const { return contentWidget_; }

void AdModal::setContentWidget(QWidget* widget) {
  if (contentWidget_ == widget) {
    return;
  }

  clearContentWidget(true);
  attachContentWidget(widget);
  emit contentWidgetChanged(contentWidget_);
  refreshVisibility();
}

QWidget* AdModal::takeContentWidget() {
  QWidget* widget = contentWidget_.data();
  if (!widget) {
    return nullptr;
  }

  disconnect(contentWidgetDestroyedConnection_);
  contentWidgetDestroyedConnection_ = {};
  if (bodyLayout_) {
    bodyLayout_->removeWidget(widget);
  }
  if (widget->parentWidget()) {
    widget->setParent(nullptr);
  }
  widget->hide();
  contentWidget_.clear();
  emit contentWidgetChanged(nullptr);
  refreshVisibility();
  return widget;
}

QWidget* AdModal::footerWidget() const { return footerWidget_; }

void AdModal::setFooterWidget(QWidget* widget) {
  if (footerWidget_ == widget) {
    return;
  }

  clearFooterWidget(true);
  attachFooterWidget(widget);
  emit footerWidgetChanged(footerWidget_);
  refreshVisibility();
}

QWidget* AdModal::takeFooterWidget() {
  QWidget* widget = footerWidget_.data();
  if (!widget) {
    return nullptr;
  }

  disconnect(footerWidgetDestroyedConnection_);
  footerWidgetDestroyedConnection_ = {};
  if (footerLayout_) {
    footerLayout_->removeWidget(widget);
  }
  if (widget->parentWidget()) {
    widget->setParent(nullptr);
  }
  widget->hide();
  footerWidget_.clear();
  emit footerWidgetChanged(nullptr);
  refreshVisibility();
  return widget;
}

void AdModal::setInitialFocusWidget(QWidget* widget) { initialFocusWidget_ = widget; }

void AdModal::attachContentWidget(QWidget* widget) {
  contentWidget_ = widget;
  if (!contentWidget_) {
    return;
  }

  QWidget* parentWidget = body_ ? body_.data() : ensureParkingWidget();
  contentWidget_->setParent(parentWidget);
  if (bodyLayout_) {
    bodyLayout_->insertWidget(0, contentWidget_);
  } else {
    contentWidget_->hide();
  }

  QWidget* rawWidget = contentWidget_.data();
  contentWidgetDestroyedConnection_ =
      connect(contentWidget_, &QObject::destroyed, this, [this, rawWidget]() {
        if (contentWidget_.data() != rawWidget) {
          return;
        }
        contentWidget_.clear();
        contentWidgetDestroyedConnection_ = {};
        emit contentWidgetChanged(nullptr);
        refreshVisibility();
      });
}

void AdModal::attachFooterWidget(QWidget* widget) {
  footerWidget_ = widget;
  if (!footerWidget_) {
    return;
  }

  QWidget* parentWidget = footer_ ? footer_.data() : ensureParkingWidget();
  footerWidget_->setParent(parentWidget);
  if (footerLayout_) {
    footerLayout_->insertWidget(0, footerWidget_);
  } else {
    footerWidget_->hide();
  }

  QWidget* rawWidget = footerWidget_.data();
  footerWidgetDestroyedConnection_ =
      connect(footerWidget_, &QObject::destroyed, this, [this, rawWidget]() {
        if (footerWidget_.data() != rawWidget) {
          return;
        }
        footerWidget_.clear();
        footerWidgetDestroyedConnection_ = {};
        emit footerWidgetChanged(nullptr);
        refreshVisibility();
      });
}

void AdModal::clearContentWidget(bool deleteWidget) {
  QWidget* widget = contentWidget_.data();
  if (!widget) {
    return;
  }

  disconnect(contentWidgetDestroyedConnection_);
  contentWidgetDestroyedConnection_ = {};
  if (bodyLayout_) {
    bodyLayout_->removeWidget(widget);
  }
  if (widget->parentWidget()) {
    widget->setParent(nullptr);
  }
  widget->hide();
  contentWidget_.clear();
  if (deleteWidget) {
    widget->deleteLater();
  }
}

void AdModal::clearFooterWidget(bool deleteWidget) {
  QWidget* widget = footerWidget_.data();
  if (!widget) {
    return;
  }

  disconnect(footerWidgetDestroyedConnection_);
  footerWidgetDestroyedConnection_ = {};
  if (footerLayout_) {
    footerLayout_->removeWidget(widget);
  }
  if (widget->parentWidget()) {
    widget->setParent(nullptr);
  }
  widget->hide();
  footerWidget_.clear();
  if (deleteWidget) {
    widget->deleteLater();
  }
}

AdModal::ComponentTokens AdModal::componentTokens() const { return componentTokens_; }

void AdModal::setComponentTokens(const ComponentTokens& tokens) {
  componentTokens_ = tokens;
  emit componentTokensChanged();
  applyVisualStyle();
}

void AdModal::resetComponentTokens() {
  componentTokens_ = ComponentTokens{};
  emit componentTokensChanged();
  applyVisualStyle();
}

AdModal::SemanticStyles AdModal::semanticStyles() const { return semanticStyles_; }

void AdModal::setSemanticStyles(const SemanticStyles& styles) {
  semanticStyles_ = styles;
  emit semanticStylesChanged();
  applyVisualStyle();
}

void AdModal::setSemanticStyleResolver(SemanticStyleResolver resolver) {
  semanticStyleResolver_ = std::move(resolver);
  emit semanticStylesChanged();
  applyVisualStyle();
}

QPushButton* AdModal::acceptButton() const { return acceptButtonControl_; }

QPushButton* AdModal::rejectButton() const { return rejectButtonControl_; }

QToolButton* AdModal::closeButton() const { return closeButton_; }

int AdModal::result() const { return static_cast<int>(result_); }

void AdModal::done(DialogCode code) {
  result_ = code;
  if (!open_) {
    return;
  }
  finalizeClose(code, effectiveProgrammaticReason(CloseReason::Programmatic));
}

void AdModal::accept() {
  result_ = DialogCode::Accepted;
  if (!open_) {
    return;
  }
  finalizeClose(DialogCode::Accepted, effectiveProgrammaticReason(CloseReason::Programmatic));
}

void AdModal::reject() {
  result_ = DialogCode::Rejected;
  if (!open_) {
    return;
  }
  finalizeClose(DialogCode::Rejected, effectiveProgrammaticReason(CloseReason::Programmatic));
}

bool AdModal::close() {
  if (!open_) {
    return true;
  }
  finalizeClose(DialogCode::Rejected, effectiveProgrammaticReason(CloseReason::Programmatic));
  return !open_;
}

bool AdModal::eventFilter(QObject* watched, QEvent* event) {
  if (!watched || !event) {
    return QObject::eventFilter(watched, event);
  }

  if (event->type() == QEvent::LanguageChange &&
      (watched == overlay_ || watched == ownerWindow_ || watched == renderContainer_)) {
    refreshTexts();
    refreshVisibility();
    refreshLayout();
    syncOverlayGeometry();
    return QObject::eventFilter(watched, event);
  }

  if (usesWindowSurface() && watched == overlay_ && event->type() == QEvent::Close && open_) {
    event->ignore();
    requestReject(CloseReason::CloseButton);
    return true;
  }

  if (!usesWindowSurface() && watched == overlay_ && event->type() == QEvent::MouseButtonPress &&
      open_) {
    auto* mouseEvent = static_cast<QMouseEvent*>(event);
    if (mouseEvent->button() == Qt::LeftButton && panel_) {
      const QPoint localPos = mouseEvent->position().toPoint();
      if (!panel_->geometry().contains(localPos) && maskVisible_ && closeOnMaskClick_) {
        requestReject(CloseReason::Mask);
        return true;
      }
    }
  }

  if (ownerWindow_ && watched == ownerWindow_) {
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::Move:
      case QEvent::Show:
      case QEvent::WindowStateChange:
        syncOverlayGeometry();
        if (open_ && overlay_) {
          overlay_->raise();
        }
        break;
      case QEvent::LayoutRequest:
        if (!usesWindowSurface()) {
          syncOverlayGeometry();
          if (open_ && overlay_) {
            overlay_->raise();
          }
        }
        break;
      case QEvent::FontChange:
      case QEvent::ApplicationFontChange:
      case QEvent::PaletteChange:
      case QEvent::ApplicationPaletteChange:
      case QEvent::StyleChange:
        applyVisualStyle();
        break;
      case QEvent::Hide:
        if (open_) {
          requestReject(CloseReason::ScopeHidden, true);
        }
        break;
      default:
        break;
    }
  }

  if (renderContainer_ && watched == renderContainer_) {
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::Show:
      case QEvent::LayoutRequest:
        syncOverlayGeometry();
        if (open_ && overlay_) {
          overlay_->raise();
        }
        break;
      case QEvent::Hide:
        if (open_) {
          requestReject(CloseReason::ScopeHidden, true);
        }
        break;
      default:
        break;
    }
  }

  return QObject::eventFilter(watched, event);
}

void AdModal::registerOpenModal(AdModal* modal) {
  if (!modal) {
    return;
  }
  auto& modals = openModals();
  for (qsizetype i = modals.size() - 1; i >= 0; --i) {
    if (!modals.at(i) || modals.at(i).data() == modal) {
      modals.removeAt(i);
    }
  }
  modal->openSequence_ = nextOpenModalSequence()++;
  modals.append(modal);
  restackOpenModals(modal->ownerWindow_ ? modal->ownerWindow_.data() : modal->resolveOwnerWindow());
}

void AdModal::unregisterOpenModal(AdModal* modal) {
  if (!modal) {
    return;
  }
  QWidget* ownerWindow =
      modal->ownerWindow_ ? modal->ownerWindow_.data() : modal->resolveOwnerWindow();
  auto& modals = openModals();
  for (qsizetype i = modals.size() - 1; i >= 0; --i) {
    if (!modals.at(i) || modals.at(i).data() == modal) {
      modals.removeAt(i);
    }
  }
  if (ownerWindow) {
    restackOpenModals(ownerWindow);
  }
}

void AdModal::restackOpenModals(QWidget* ownerWindow) {
  if (!ownerWindow) {
    return;
  }

  QVector<AdModal*> candidates;
  auto& modals = openModals();
  for (qsizetype i = modals.size() - 1; i >= 0; --i) {
    AdModal* modal = modals.at(i).data();
    if (!modal || !modal->open_ || !modal->overlay_) {
      if (!modal) {
        modals.removeAt(i);
      }
      continue;
    }
    if (modal->ownerWindow_ == ownerWindow) {
      candidates.append(modal);
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](AdModal* lhs, AdModal* rhs) {
    if (!lhs || !rhs) {
      return lhs < rhs;
    }
    if (lhs->resolvedZIndex_ != rhs->resolvedZIndex_) {
      return lhs->resolvedZIndex_ < rhs->resolvedZIndex_;
    }
    return lhs->openSequence_ < rhs->openSequence_;
  });

  for (AdModal* modal : candidates) {
    if (modal && modal->overlay_) {
      modal->overlay_->raise();
    }
  }
}

void AdModal::registerStaticServiceModal(AdModal* modal) {
  if (!modal) {
    return;
  }
  auto& modals = staticModals();
  for (qsizetype i = modals.size() - 1; i >= 0; --i) {
    if (!modals.at(i) || modals.at(i).data() == modal) {
      modals.removeAt(i);
    }
  }
  modals.append(modal);
}

void AdModal::unregisterStaticServiceModal(AdModal* modal) {
  if (!modal) {
    return;
  }
  auto& modals = staticModals();
  for (qsizetype i = modals.size() - 1; i >= 0; --i) {
    if (!modals.at(i) || modals.at(i).data() == modal) {
      modals.removeAt(i);
    }
  }
}

QWidget* AdModal::ensureParkingWidget() {
  if (!parkingWidget_) {
    auto* parking = new QWidget();
    parking->setObjectName(QStringLiteral("ad-modal-parking"));
    parking->setAttribute(Qt::WA_DontShowOnScreen, true);
    parking->setAttribute(Qt::WA_StyledBackground, false);
    parking->hide();
    parkingWidget_ = parking;
  }
  return parkingWidget_;
}

void AdModal::attachOwnerWindowWatcher(QWidget* ownerWindow) {
  if (!ownerWindow) {
    return;
  }
  ownerWindow->removeEventFilter(this);
  ownerWindow->installEventFilter(this);
  disconnect(ownerWindowDestroyedConnection_);
  ownerWindowDestroyedConnection_ =
      connect(ownerWindow, &QObject::destroyed, this, [this]() { ownerWindow_.clear(); });
}

void AdModal::detachOwnerWindowWatcher() {
  disconnect(ownerWindowDestroyedConnection_);
  ownerWindowDestroyedConnection_ = {};
  if (!ownerWindow_) {
    return;
  }
  ownerWindow_->removeEventFilter(this);
}

void AdModal::attachRenderContainerWatcher(QWidget* renderContainer) {
  if (!renderContainer) {
    return;
  }
  renderContainer->installEventFilter(this);
  renderContainerDestroyedConnection_ =
      connect(renderContainer, &QObject::destroyed, this, [this]() {
        renderContainer_.clear();
        if (open_) {
          finalizeClose(DialogCode::Rejected, CloseReason::ScopeHidden);
        }
      });
}

void AdModal::detachRenderContainerWatcher() {
  disconnect(renderContainerDestroyedConnection_);
  renderContainerDestroyedConnection_ = {};
  if (renderContainer_) {
    renderContainer_->removeEventFilter(this);
  }
}

bool AdModal::usesWindowSurface() const { return mode_ == Mode::Window && !renderContainer_; }

QWidget* AdModal::resolveOwnerWindow() const {
  if (ownerWindow_) {
    return ownerWindow_;
  }
  if (auto* parentWidget = qobject_cast<QWidget*>(parent())) {
    return normalizeOwnerWindow(parentWidget);
  }
  if (QWidget* active = QApplication::activeWindow()) {
    return normalizeOwnerWindow(active);
  }
  const QWidgetList topLevels = QApplication::topLevelWidgets();
  for (QWidget* widget : topLevels) {
    if (widget && widget->isVisible()) {
      return normalizeOwnerWindow(widget);
    }
  }
  return nullptr;
}

const QWidget* AdModal::themeLogicalOwner() const {
  if (ownerWindow_) {
    return ownerWindow_;
  }
  if (QWidget* resolvedOwnerWindow = resolveOwnerWindow()) {
    return resolvedOwnerWindow;
  }
  return themeSourceWidget();
}

const QWidget* AdModal::themeSourceWidget() const {
  if (panel_) {
    return panel_;
  }
  if (overlay_) {
    return overlay_;
  }
  if (ownerWindow_) {
    return ownerWindow_;
  }
  if (auto* parentWidget = qobject_cast<QWidget*>(parent())) {
    return parentWidget;
  }
  return QApplication::activeWindow();
}

QRect AdModal::windowModeAvailableGeometry() const {
  QScreen* screen = nullptr;
  if (ownerWindow_) {
    screen = ownerWindow_->screen();
  }
  if (!screen && overlay_) {
    screen = overlay_->screen();
  }
  if (!screen) {
    screen = QApplication::primaryScreen();
  }
  return screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
}

QRect AdModal::windowModeAnchorGeometry() const {
  if (!ownerWindow_) {
    return windowModeAvailableGeometry();
  }

  const QRect frameGeometry = ownerWindow_->frameGeometry();
  if (frameGeometry.isValid() && !frameGeometry.isEmpty()) {
    return frameGeometry;
  }

  return QRect(ownerWindow_->mapToGlobal(QPoint(0, 0)), ownerWindow_->size());
}

void AdModal::ensureOverlay() {
  if (overlay_) {
    syncOverlayGeometry();
    return;
  }

  QWidget* resolvedOwnerWindow = normalizeOwnerWindow(resolveOwnerWindow());
  // A window-mode surface is a top-level dialog: the owner window only
  // anchors it (centering, stacking, transient parenting) and is optional.
  // Callers such as tray-menu actions open dialogs while the application has
  // no active or visible window, so requiring an owner here would silently
  // drop the dialog. Overlay-mode surfaces still require a host: either an
  // owner window or an explicit render container to cover.
  if (!resolvedOwnerWindow && !renderContainer_ && !usesWindowSurface()) {
    return;
  }

  if (resolvedOwnerWindow && ownerWindow_ != resolvedOwnerWindow) {
    detachOwnerWindowWatcher();
    ownerWindow_ = resolvedOwnerWindow;
    attachOwnerWindowWatcher(ownerWindow_);
    emit ownerWindowChanged(ownerWindow_);
  }

  const bool windowMode = usesWindowSurface();
  Qt::WindowFlags overlayFlags = Qt::WindowFlags();
  if (windowMode) {
    overlayFlags = (windowModeDetached_ ? Qt::Tool : Qt::Dialog) | Qt::FramelessWindowHint;
    if (ownerWindow_ && ownerWindow_->windowFlags().testFlag(Qt::WindowStaysOnTopHint)) {
      overlayFlags |= Qt::WindowStaysOnTopHint;
    }
  }

  QWidget* nativeParent = renderContainer_
                              ? renderContainer_.data()
                              : (windowMode && windowModeDetached_ ? nullptr : ownerWindow_.data());
  auto* overlay = new ModalOverlayWidget(nativeParent, overlayFlags);
  overlay->setObjectName(QStringLiteral("ad-modal-overlay"));
  overlay->setProperty("adqt.interaction.surface", true);
  if (windowMode) {
    overlay->setWindowTitle(windowTitle_.trimmed().isEmpty() ? tr("Modal")
                                                             : windowTitle_.trimmed());
    overlay->setWindowModality(windowModality_);
    overlay->setAttribute(Qt::WA_DeleteOnClose, false);
  } else {
    overlay->setWindowModality(Qt::NonModal);
    QWidget* geometryHost = renderContainer_ ? renderContainer_.data() : ownerWindow_.data();
    if (geometryHost != nullptr) {
      overlay->setGeometry(geometryHost->rect());
    }
  }
  overlay->hide();
  overlay->installEventFilter(this);
  overlay->setFocusNavigator([this](bool next) { return focusNextPrevChildInModal(next); });

  auto* overlayLayout = new QVBoxLayout(overlay);
  overlayLayout->setContentsMargins(16, topOffset_, 16, 16);
  overlayLayout->setSpacing(0);

  auto* panel = new ModalPanelWidget(overlay);
  panel->setObjectName(QStringLiteral("ad-modal-panel"));
  panel->setFrameShape(QFrame::NoFrame);
  panel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Maximum);

  auto* panelLayout = new QVBoxLayout(panel);
  panelLayout->setContentsMargins(24, 20, 24, 20);
  panelLayout->setSpacing(0);

  auto* header = new QWidget(panel);
  header->setObjectName(QStringLiteral("ad-modal-header"));
  header->setAttribute(Qt::WA_StyledBackground, false);
  header->setAutoFillBackground(false);
  auto* headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(0, 0, 0, 8);
  headerLayout->setSpacing(0);

  auto* titleLabel = new QLabel(header);
  titleLabel->setObjectName(QStringLiteral("ad-modal-title"));
  titleLabel->setWordWrap(true);
  titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

  auto* closeButton = new ModalIconButton(header);
  closeButton->setObjectName(QStringLiteral("ad-modal-close"));
  closeButton->setAutoRaise(true);
  closeButton->setCursor(Qt::PointingHandCursor);
  closeButton->setFocusPolicy(Qt::StrongFocus);
  connect(closeButton, &QToolButton::clicked, this,
          [this]() { requestReject(CloseReason::CloseButton); });

  headerLayout->addWidget(titleLabel, 1, Qt::AlignVCenter);
  headerLayout->addWidget(closeButton, 0, Qt::AlignTop);

  auto* body = new QWidget(panel);
  body->setObjectName(QStringLiteral("ad-modal-body"));
  body->setAttribute(Qt::WA_StyledBackground, false);
  body->setAutoFillBackground(false);
  auto* bodyLayout = new QVBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);

  auto* confirmBodyHost = new QWidget(body);
  confirmBodyHost->setObjectName(QStringLiteral("ad-modal-confirm-body"));
  confirmBodyHost->setAttribute(Qt::WA_StyledBackground, false);
  confirmBodyHost->setAutoFillBackground(false);
  auto* confirmBodyLayout = new QHBoxLayout(confirmBodyHost);
  confirmBodyLayout->setContentsMargins(0, 0, 0, 0);
  confirmBodyLayout->setSpacing(0);

  auto* titleIconLabel = new QLabel(confirmBodyHost);
  titleIconLabel->setObjectName(QStringLiteral("ad-modal-title-icon"));
  titleIconLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  titleIconLabel->hide();

  auto* confirmParagraph = new QWidget(confirmBodyHost);
  confirmParagraph->setObjectName(QStringLiteral("ad-modal-confirm-paragraph"));
  auto* confirmParagraphLayout = new QVBoxLayout(confirmParagraph);
  confirmParagraphLayout->setContentsMargins(0, 0, 0, 0);
  confirmParagraphLayout->setSpacing(0);

  auto* confirmTitleLabel = new QLabel(confirmParagraph);
  confirmTitleLabel->setObjectName(QStringLiteral("ad-modal-confirm-title"));
  confirmTitleLabel->setWordWrap(true);
  confirmTitleLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

  auto* confirmContentLabel = new QLabel(confirmParagraph);
  confirmContentLabel->setObjectName(QStringLiteral("ad-modal-confirm-content"));
  confirmContentLabel->setWordWrap(true);
  confirmContentLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);

  confirmParagraphLayout->addWidget(confirmTitleLabel);
  confirmParagraphLayout->addWidget(confirmContentLabel);

  confirmBodyLayout->addWidget(titleIconLabel, 0, Qt::AlignTop);
  confirmBodyLayout->addWidget(confirmParagraph, 1, Qt::AlignTop);

  auto* contentLabel = new QLabel(body);
  contentLabel->setObjectName(QStringLiteral("ad-modal-content"));
  contentLabel->setWordWrap(true);
  contentLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  bodyLayout->addWidget(confirmBodyHost);
  bodyLayout->addWidget(contentLabel);

  auto* footer = new QWidget(panel);
  footer->setObjectName(QStringLiteral("ad-modal-footer"));
  footer->setAttribute(Qt::WA_StyledBackground, false);
  footer->setAutoFillBackground(false);
  auto* footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(0, 12, 0, 0);
  footerLayout->setSpacing(0);

  auto* footerButtonsHost = new QWidget(footer);
  auto* footerButtonsLayout = new QHBoxLayout(footerButtonsHost);
  footerButtonsLayout->setContentsMargins(0, 0, 0, 0);
  footerButtonsLayout->setSpacing(8);

  auto* rejectButton = new AdButton(tr("Cancel"), footerButtonsHost);
  rejectButton->setButtonStyle(AdButton::ButtonStyle::Outline);
  rejectButton->setAccentRole(AdButton::AccentRole::Neutral);
  rejectButton->setAutoDefault(false);
  rejectButton->setDefault(false);

  auto* acceptButton = new AdButton(tr("OK"), footerButtonsHost);
  acceptButton->setButtonStyle(acceptButtonStyle_);
  acceptButton->setAccentRole(acceptAccentRole_);
  acceptButton->setAutoDefault(true);
  acceptButton->setDefault(true);

  connect(rejectButton, &QAbstractButton::clicked, this,
          [this]() { requestReject(CloseReason::CancelAction); });
  connect(acceptButton, &QAbstractButton::clicked, this, [this]() { requestAccept(); });

  footerButtonsLayout->addStretch();
  footerButtonsLayout->addWidget(rejectButton);
  footerButtonsLayout->addWidget(acceptButton);
  footerLayout->addWidget(footerButtonsHost);

  panelLayout->addWidget(header);
  panelLayout->addWidget(body);
  panelLayout->addWidget(footer);

  overlayLayout->addWidget(panel, 0, Qt::AlignTop | Qt::AlignHCenter);
  if (windowMode) {
    overlay->setWindowModeDragWidgets(header, panel);
  }

  auto* escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), overlay);
  escShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(escShortcut, &QShortcut::activated, this, [this]() {
    if (open_ && closeOnEscape_) {
      requestReject(CloseReason::Keyboard);
    }
  });

  overlay_ = overlay;
  overlayLayout_ = overlayLayout;
  panel_ = panel;
  panelLayout_ = panelLayout;
  header_ = header;
  headerLayout_ = headerLayout;
  titleIconLabel_ = titleIconLabel;
  titleLabel_ = titleLabel;
  closeButton_ = closeButton;
  body_ = body;
  bodyLayout_ = bodyLayout;
  confirmBodyHost_ = confirmBodyHost;
  confirmBodyLayout_ = confirmBodyLayout;
  confirmParagraphLayout_ = confirmParagraphLayout;
  contentLabel_ = contentLabel;
  confirmTitleLabel_ = confirmTitleLabel;
  confirmContentLabel_ = confirmContentLabel;
  footer_ = footer;
  footerLayout_ = footerLayout;
  footerButtonsHost_ = footerButtonsHost;
  footerButtonsLayout_ = footerButtonsLayout;
  rejectButtonControl_ = rejectButton;
  acceptButtonControl_ = acceptButton;
  escShortcut_ = escShortcut;

  if (contentWidget_) {
    contentWidget_->setParent(body_);
    bodyLayout_->insertWidget(0, contentWidget_);
  }
  if (footerWidget_) {
    footerWidget_->setParent(footer_);
    footerLayout_->insertWidget(0, footerWidget_);
  }

  refreshTexts();
  refreshVisibility();
  refreshLayout();
  applyVisualStyle();
  updateAccessibility();
  syncOverlayGeometry();
  if (windowMode) {
    overlay->setWindowModeChromeEnabled(true);
  }
}

void AdModal::releaseOverlay() {
  if (!overlay_) {
    return;
  }

  QWidget* parking = ensureParkingWidget();
  if (contentWidget_ && contentWidget_->parentWidget() == body_) {
    if (bodyLayout_) {
      bodyLayout_->removeWidget(contentWidget_);
    }
    contentWidget_->setParent(parking);
    contentWidget_->hide();
  }

  if (footerWidget_ && footerWidget_->parentWidget() == footer_) {
    if (footerLayout_) {
      footerLayout_->removeWidget(footerWidget_);
    }
    footerWidget_->setParent(parking);
    footerWidget_->hide();
  }

  overlay_->removeEventFilter(this);
  overlay_->hide();
  if (usesWindowSurface()) {
    overlay_->setWindowModality(Qt::NonModal);
  }
  overlay_->deleteLater();

  overlay_.clear();
  overlayLayout_.clear();
  panel_.clear();
  panelLayout_.clear();
  header_.clear();
  headerLayout_.clear();
  titleIconLabel_.clear();
  titleLabel_.clear();
  closeButton_.clear();
  body_.clear();
  bodyLayout_.clear();
  confirmBodyHost_.clear();
  confirmBodyLayout_.clear();
  confirmParagraphLayout_.clear();
  contentLabel_.clear();
  confirmTitleLabel_.clear();
  confirmContentLabel_.clear();
  footer_.clear();
  footerLayout_.clear();
  footerButtonsHost_.clear();
  footerButtonsLayout_.clear();
  rejectButtonControl_.clear();
  acceptButtonControl_.clear();
  escShortcut_.clear();
}

void AdModal::syncOverlayGeometry() {
  if (!overlay_) {
    return;
  }

  if (usesWindowSurface()) {
    syncWindowModeGeometry();
    return;
  }

  QWidget* geometryHost = renderContainer_ ? renderContainer_.data() : ownerWindow_.data();
  if (!geometryHost) {
    return;
  }
  const QRect rect = geometryHost->rect();
  if (overlay_->geometry() != rect) {
    overlay_->setGeometry(rect);
  }
}

void AdModal::syncWindowModeGeometry() {
  if (!overlay_) {
    return;
  }
  if (syncingWindowModeGeometry_) {
    return;
  }
  QScopedValueRollback<bool> syncingGuard(syncingWindowModeGeometry_, true);

  overlay_->setMinimumSize(QSize(0, 0));
  overlay_->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));

  if (overlayLayout_) {
    overlayLayout_->activate();
  }
  if (panelLayout_) {
    panelLayout_->activate();
  }
  if (panel_) {
    panel_->adjustSize();
  }

  const QRect available = windowModeAvailableGeometry();
  const int horizontalPadding = 16;
  const int verticalPadding = 16;
  const int maxWidth = std::max(160, available.width() - horizontalPadding * 2);
  const int maxHeight = std::max(120, available.height() - verticalPadding * 2);

  QSize panelSize = panel_ ? panel_->sizeHint() : QSize(preferredWidth_, 1);
  if (!panelSize.isValid() || panelSize.isEmpty()) {
    panelSize = QSize(preferredWidth_, 1);
  }

  int panelWidth = std::clamp(std::max(1, panelSize.width()), 160, maxWidth);
  if (panel_ && panel_->minimumWidth() > 0 && panel_->minimumWidth() == panel_->maximumWidth()) {
    panelWidth = std::clamp(panel_->minimumWidth(), 160, maxWidth);
  }

  int panelHeight = panelSize.height();
  if (panel_) {
    const int heightForWidth = panel_->heightForWidth(panelWidth);
    if (heightForWidth > 0) {
      panelHeight = heightForWidth;
    } else if (panelLayout_) {
      const int layoutHeightForWidth = panelLayout_->heightForWidth(panelWidth);
      if (layoutHeightForWidth > 0) {
        panelHeight = layoutHeightForWidth;
      }
    }
  }

  QMargins margins;
  if (overlayLayout_) {
    margins = overlayLayout_->contentsMargins();
  }

  QSize targetSize(panelWidth + margins.left() + margins.right(),
                   std::max(1, panelHeight) + margins.top() + margins.bottom());
  targetSize.setWidth(std::clamp(targetSize.width(), 160, maxWidth));
  targetSize.setHeight(std::clamp(targetSize.height(), 1, maxHeight));

  if (overlay_->minimumSize() != targetSize || overlay_->maximumSize() != targetSize) {
    overlay_->setFixedSize(targetSize);
  }

  const QRect anchor = windowModeAnchorGeometry();
  const QPoint anchorCenter = anchor.center();
  QPoint topLeft;
  if (centered_) {
    topLeft = QPoint(anchorCenter.x() - targetSize.width() / 2,
                     anchorCenter.y() - targetSize.height() / 2);
  } else {
    topLeft = QPoint(anchor.left() + (anchor.width() - targetSize.width()) / 2,
                     anchor.top() + std::max(0, topOffset_));
  }

  const int minX = available.left() + horizontalPadding;
  const int maxX = available.right() - horizontalPadding - targetSize.width() + 1;
  const int minY = available.top() + verticalPadding;
  const int maxY = available.bottom() - verticalPadding - targetSize.height() + 1;
  topLeft.setX(std::clamp(topLeft.x(), std::min(minX, maxX), std::max(minX, maxX)));
  topLeft.setY(std::clamp(topLeft.y(), std::min(minY, maxY), std::max(minY, maxY)));

  const QRect geometry(topLeft, targetSize);
  if (overlay_->geometry() != geometry) {
    overlay_->setGeometry(geometry);
  }
}

void AdModal::refreshLayout() {
  if (!overlayLayout_ || !panel_) {
    return;
  }

  if (usesWindowSurface() || renderContainer_) {
    overlayLayout_->setContentsMargins(0, 0, 0, 0);
    overlayLayout_->setAlignment(panel_, Qt::AlignCenter);
    syncOverlayGeometry();
    return;
  }

  if (centered_) {
    overlayLayout_->setContentsMargins(16, 16, 16, 16);
    overlayLayout_->setAlignment(panel_, Qt::AlignCenter);
  } else {
    overlayLayout_->setContentsMargins(16, std::max(0, topOffset_), 16, 16);
    overlayLayout_->setAlignment(panel_, Qt::AlignTop | Qt::AlignHCenter);
  }

  syncOverlayGeometry();
}

void AdModal::refreshTexts() {
  const QString resolvedContent = contentLoading_ ? tr("Loading...") : text_;
  const QString resolvedTitle = windowTitle_;

  if (overlay_) {
    overlay_->setWindowTitle(resolvedTitle.trimmed().isEmpty() ? tr("Modal")
                                                               : resolvedTitle.trimmed());
  }

  if (titleLabel_) {
    titleLabel_->setText(resolvedTitle);
  }
  if (confirmTitleLabel_) {
    confirmTitleLabel_->setText(resolvedTitle);
  }

  if (contentLabel_) {
    contentLabel_->setText(resolvedContent);
  }
  if (confirmContentLabel_) {
    confirmContentLabel_->setText(resolvedContent);
  }

  if (acceptButtonControl_) {
    acceptButtonControl_->setText(acceptTextExplicit_ ? acceptText_ : tr("OK"));
    acceptButtonControl_->setButtonStyle(acceptButtonStyle_);
    acceptButtonControl_->setAccentRole(acceptAccentRole_);
    acceptButtonControl_->setBusy(acceptButtonBusy_);
  }

  if (rejectButtonControl_) {
    rejectButtonControl_->setText(rejectTextExplicit_ ? rejectText_ : tr("Cancel"));
  }

  updateAccessibility();
}

void AdModal::refreshVisibility() {
  const bool hasTitle = !windowTitle_.trimmed().isEmpty();
  const bool confirmMode = preset_ != Preset::Plain;
  const bool showPanelCloseButton =
      closeButtonVisible_ && !(usesWindowSurface() && preset_ == Preset::Confirm);
  const bool showTextContent = contentLoading_ || !contentWidget_;
  const bool showAcceptButton = standardButtons_.testFlag(StandardButton::Ok);
  const bool showRejectButton = standardButtons_.testFlag(StandardButton::Cancel);
  const bool showButtonFooter = showAcceptButton || showRejectButton;

  if (header_) {
    const bool showHeader = confirmMode ? showPanelCloseButton : (hasTitle || showPanelCloseButton);
    header_->setVisible(showHeader);
  }

  if (titleLabel_) {
    titleLabel_->setVisible(!confirmMode && hasTitle);
  }

  if (closeButton_) {
    closeButton_->setVisible(showPanelCloseButton);
  }

  if (confirmBodyHost_) {
    confirmBodyHost_->setVisible(confirmMode && showTextContent);
  }
  if (confirmTitleLabel_) {
    confirmTitleLabel_->setVisible(confirmMode && hasTitle && showTextContent);
  }
  if (titleIconLabel_) {
    titleIconLabel_->setVisible(confirmMode && showTextContent);
  }

  if (contentWidget_) {
    contentWidget_->setVisible(!contentLoading_);
  }

  if (contentLabel_) {
    contentLabel_->setVisible(!confirmMode && showTextContent);
  }
  if (confirmContentLabel_) {
    confirmContentLabel_->setVisible(confirmMode && showTextContent);
  }

  if (footer_) {
    footer_->setVisible(footerVisible_ && (footerWidget_ || showButtonFooter));
  }

  if (footerButtonsHost_) {
    footerButtonsHost_->setVisible(!footerWidget_ && showButtonFooter);
  }

  if (footerWidget_) {
    footerWidget_->setVisible(true);
  }

  if (acceptButtonControl_) {
    acceptButtonControl_->setVisible(showAcceptButton);
  }

  if (rejectButtonControl_) {
    rejectButtonControl_->setVisible(showRejectButton);
  }

  refreshTitleIcon();
  updateAccessibility();
  refreshLayout();
}

void AdModal::refreshTitleIcon() {
  if (!titleIconLabel_) {
    return;
  }

  if (preset_ == Preset::Plain) {
    titleIconLabel_->clear();
    titleIconLabel_->hide();
    return;
  }

  const VisualStyle style = resolveVisualStyle();
  const auto iconColors = adqt::icons::IconColors::primary(style.iconColor);

  adqt::icons::IconRef token;
  switch (preset_) {
    case Preset::Info:
      token = filled_icons::InfoCircle(iconColors);
      break;
    case Preset::Success:
      token = filled_icons::CheckCircle(iconColors);
      break;
    case Preset::Error:
      token = filled_icons::CloseCircle(iconColors);
      break;
    case Preset::Warning:
    case Preset::Confirm:
      token = filled_icons::ExclamationCircle(iconColors);
      break;
    case Preset::Plain:
      return;
  }

  const int iconSize = std::max(12, style.iconSize);
  const bool hasTitle = !windowTitle_.trimmed().isEmpty();
  const int anchorHeight = hasTitle ? style.titleLineHeight : style.textLineHeight;
  const int iconOffsetTop =
      std::max(0, static_cast<int>(std::round((anchorHeight - iconSize) / 2.0)));
  titleIconLabel_->setContentsMargins(0, iconOffsetTop, 0, 0);
  titleIconLabel_->setFixedSize(iconSize, iconSize + iconOffsetTop);
  const qreal ratio = panel_ ? std::max(1.0, panel_->devicePixelRatioF()) : 1.0;
  titleIconLabel_->setPixmap(
      adqt::icons::renderIconPixmap(token, {QSize(iconSize, iconSize), ratio}));
  titleIconLabel_->show();
}

void AdModal::updateAccessibility() {
  if (!overlay_) {
    return;
  }

  const QString resolvedTitle =
      windowTitle_.trimmed().isEmpty() ? tr("Modal") : windowTitle_.trimmed();
  const QString resolvedContent = (contentLoading_ ? tr("Loading...") : text_).trimmed();

  overlay_->setAccessibleName(usesWindowSurface() ? tr("Modal window") : tr("Modal overlay"));
  overlay_->setAccessibleDescription(resolvedTitle);

  if (panel_) {
    panel_->setAccessibleName(resolvedTitle);
    panel_->setAccessibleDescription(resolvedContent);
  }

  if (titleLabel_) {
    titleLabel_->setAccessibleName(resolvedTitle);
  }
  if (confirmTitleLabel_) {
    confirmTitleLabel_->setAccessibleName(resolvedTitle);
  }
  if (contentLabel_) {
    contentLabel_->setAccessibleName(tr("Modal content"));
    contentLabel_->setAccessibleDescription(resolvedContent);
  }
  if (confirmContentLabel_) {
    confirmContentLabel_->setAccessibleName(tr("Modal content"));
    confirmContentLabel_->setAccessibleDescription(resolvedContent);
  }
  if (closeButton_) {
    closeButton_->setAccessibleName(tr("Close"));
    closeButton_->setAccessibleDescription(tr("Close modal"));
  }
  if (acceptButtonControl_) {
    acceptButtonControl_->setAccessibleName(acceptButtonControl_->text().trimmed().isEmpty()
                                                ? tr("OK")
                                                : acceptButtonControl_->text().trimmed());
  }
  if (rejectButtonControl_) {
    rejectButtonControl_->setAccessibleName(rejectButtonControl_->text().trimmed().isEmpty()
                                                ? tr("Cancel")
                                                : rejectButtonControl_->text().trimmed());
  }
}

void AdModal::saveFocusBeforeOpen() {
  QWidget* focusWidget = QApplication::focusWidget();
  if (focusWidget && overlay_ && overlay_->isAncestorOf(focusWidget)) {
    return;
  }
  focusBeforeOpen_ = focusWidget;
}

void AdModal::restoreFocusAfterClose() {
  QWidget* target = focusBeforeOpen_.data();
  focusBeforeOpen_.clear();
  if (!target || !target->isVisible() || !target->isEnabled()) {
    return;
  }
  target->setFocus(Qt::OtherFocusReason);
}

QWidget* AdModal::nextFocusableFrom(QWidget* start, bool next) const {
  if (!overlay_ || !start) {
    return nullptr;
  }

  QWidget* cursor = start;
  QSet<QWidget*> visited;
  while (cursor && !visited.contains(cursor)) {
    visited.insert(cursor);
    if (cursor != overlay_.data() && overlay_->isAncestorOf(cursor) && isFocusableWidget(cursor)) {
      return cursor;
    }
    cursor = next ? cursor->nextInFocusChain() : cursor->previousInFocusChain();
  }
  return nullptr;
}

QWidget* AdModal::firstFocusableWidget(bool reverse) const {
  QWidget* seed = panel_ ? static_cast<QWidget*>(panel_.data()) : overlay_.data();
  return nextFocusableFrom(seed, !reverse);
}

QWidget* AdModal::resolveInitialFocusTarget() const {
  if (initialFocusWidget_ && overlay_ && overlay_->isAncestorOf(initialFocusWidget_) &&
      isFocusableWidget(initialFocusWidget_)) {
    return initialFocusWidget_;
  }
  if (acceptButtonControl_ && acceptButtonControl_->isVisible() &&
      acceptButtonControl_->isEnabled()) {
    return acceptButtonControl_;
  }
  if (rejectButtonControl_ && rejectButtonControl_->isVisible() &&
      rejectButtonControl_->isEnabled()) {
    return rejectButtonControl_;
  }
  if (closeButton_ && closeButton_->isVisible() && closeButton_->isEnabled()) {
    return closeButton_;
  }
  if (QWidget* first = firstFocusableWidget(false)) {
    return first;
  }
  return overlay_.data();
}

bool AdModal::focusNextPrevChildInModal(bool next) {
  if (!overlay_) {
    return false;
  }

  QWidget* current = QApplication::focusWidget();
  if (!current || !overlay_->isAncestorOf(current)) {
    QWidget* target = firstFocusableWidget(!next);
    if (!target) {
      overlay_->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
      return true;
    }
    target->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
  }

  QWidget* cursor = current;
  QSet<QWidget*> visited;
  while (cursor && !visited.contains(cursor)) {
    visited.insert(cursor);
    cursor = next ? cursor->nextInFocusChain() : cursor->previousInFocusChain();
    if (!cursor) {
      break;
    }
    if (overlay_->isAncestorOf(cursor) && isFocusableWidget(cursor)) {
      cursor->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
      return true;
    }
  }

  QWidget* wrap = firstFocusableWidget(!next);
  if (wrap) {
    wrap->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
  }

  overlay_->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
  return true;
}

void AdModal::applyVisualStyle() {
  if (!overlay_) {
    return;
  }

  const VisualStyle style = resolveVisualStyle();
  resolvedZIndex_ = style.zIndex;

  auto* overlayWidget = static_cast<ModalOverlayWidget*>(overlay_.data());
  if (overlayWidget) {
    overlayWidget->setRootColor(usesWindowSurface() ? style.containerBg : style.rootBg);
    overlayWidget->setMaskEnabled(!usesWindowSurface() && maskVisible_);
    overlayWidget->setMaskColor(style.maskBg);
  }

  if (panel_) {
    int width = std::max(240, style.width);
    if (usesWindowSurface()) {
      const QRect availableGeometry = windowModeAvailableGeometry();
      const int available = availableGeometry.width() - 32;
      if (available > 0) {
        width = std::min(width, available);
      }
    } else if (overlay_ && overlayLayout_) {
      const QMargins margins = overlayLayout_->contentsMargins();
      const int available = overlay_->width() - margins.left() - margins.right();
      if (available > 0) {
        width = std::min(width, available);
      }
    }
    panel_->setFixedWidth(std::max(160, width));

    auto* panelWidget = static_cast<ModalPanelWidget*>(panel_.data());
    if (panelWidget) {
      ModalPanelWidget::PaintStyle paintStyle;
      paintStyle.containerBg = style.containerBg;
      paintStyle.headerBg = style.headerBg;
      paintStyle.bodyBg = style.bodyBg;
      paintStyle.footerBg = style.footerBg;
      paintStyle.borderColor = style.borderColor;
      paintStyle.borderRadius = style.borderRadius;
      paintStyle.borderWidth = style.borderWidth;
      paintStyle.footerBorderTopWidth = style.footerBorderTopWidth;
      panelWidget->setSectionWidgets(header_, body_, footer_);
      panelWidget->setPaintStyle(paintStyle);
    }
  }

  if (panelLayout_) {
    panelLayout_->setContentsMargins(style.contentPaddingHorizontal, style.contentPaddingVertical,
                                     style.contentPaddingHorizontal, style.contentPaddingVertical);
  }
  if (headerLayout_) {
    headerLayout_->setContentsMargins(style.headerPaddingHorizontal, style.headerPaddingVertical,
                                      style.headerPaddingHorizontal,
                                      style.headerPaddingVertical + style.headerMarginBottom);
  }
  if (bodyLayout_) {
    bodyLayout_->setContentsMargins(style.bodyPaddingHorizontal, style.bodyPaddingVertical,
                                    style.bodyPaddingHorizontal, style.bodyPaddingVertical);
  }
  if (confirmBodyLayout_) {
    confirmBodyLayout_->setSpacing(style.confirmIconGap);
  }
  if (confirmParagraphLayout_) {
    confirmParagraphLayout_->setSpacing(style.confirmParagraphGap);
  }
  if (footerLayout_) {
    footerLayout_->setContentsMargins(style.footerPaddingHorizontal,
                                      style.footerPaddingVertical + style.footerMarginTop,
                                      style.footerPaddingHorizontal, style.footerPaddingVertical);
  }
  if (footerButtonsLayout_) {
    footerButtonsLayout_->setSpacing(style.footerButtonGap);
  }

  if (titleLabel_) {
    titleLabel_->setFont(style.titleFont);
    QPalette palette = titleLabel_->palette();
    palette.setColor(QPalette::WindowText, style.titleColor);
    titleLabel_->setPalette(palette);
  }
  if (contentLabel_) {
    contentLabel_->setFont(style.bodyFont);
    QPalette palette = contentLabel_->palette();
    palette.setColor(QPalette::WindowText, style.bodyColor);
    contentLabel_->setPalette(palette);
  }
  if (confirmTitleLabel_) {
    confirmTitleLabel_->setFont(style.titleFont);
    QPalette palette = confirmTitleLabel_->palette();
    palette.setColor(QPalette::WindowText, style.titleColor);
    confirmTitleLabel_->setPalette(palette);
  }
  if (confirmContentLabel_) {
    confirmContentLabel_->setFont(style.bodyFont);
    QPalette palette = confirmContentLabel_->palette();
    palette.setColor(QPalette::WindowText, style.bodyColor);
    confirmContentLabel_->setPalette(palette);
  }

  if (closeButton_) {
    const auto iconColors = adqt::icons::IconColors::primary(style.closeIconColor);
    const int closeIconSize = std::max(10, style.closeIconSize);
    closeButton_->setFixedSize(std::max(closeIconSize, style.closeButtonSize),
                               std::max(closeIconSize, style.closeButtonSize));
    const qreal ratio = panel_ ? std::max(1.0, panel_->devicePixelRatioF()) : 1.0;
    closeButton_->setIcon(QIcon(adqt::icons::renderIconPixmap(
        outlined_icons::Close(iconColors), {QSize(closeIconSize, closeIconSize), ratio})));
    closeButton_->setIconSize(QSize(closeIconSize, closeIconSize));
    closeButton_->setEnabled(!acceptButtonBusy_);
    static_cast<ModalIconButton*>(closeButton_.data())
        ->setVisualStyle(style.closeButtonBackground, style.closeButtonHoverBackground,
                         style.closeButtonPressedBackground, style.closeButtonDisabledBackground,
                         style.closeButtonBorderColor, style.closeButtonBorderWidth,
                         style.closeButtonRadius);
  }

  if (rejectButtonControl_) {
    rejectButtonControl_->setEnabled(!acceptButtonBusy_);
  }
  if (acceptButtonControl_) {
    acceptButtonControl_->setEnabled(true);
  }

  refreshTitleIcon();
  updateAccessibility();
  refreshLayout();
  if (open_) {
    restackOpenModals(ownerWindow_ ? ownerWindow_.data() : resolveOwnerWindow());
  }
}

AdModal::VisualStyle AdModal::resolveVisualStyle() const {
  const adqt::theme::ThemeManager& themeManager = adqt::theme::ThemeManager::instance();
  const adqt::theme::ResolvedTheme resolved =
      themeManager.resolve(themeSourceWidget(), themeLogicalOwner());
  const adqt::theme::ThemeMapToken& map = resolved.values;
  const adqt::theme::ThemeSeedToken& seed = resolved.config;

  const QWidget* referenceWidget = themeSourceWidget();
  const QFont baseFont = referenceWidget ? referenceWidget->font() : QApplication::font();

  VisualStyle style;
  style.rootBg = QColor(0, 0, 0, 0);
  style.maskBg = toColor(map.colorBgMask, QColor(0, 0, 0, 115));
  style.containerBg = toColor(map.colorBgElevated, QColor("#ffffff"));
  const bool wireframe = seed.wireframe;
  style.headerBg = wireframe ? style.containerBg : QColor(0, 0, 0, 0);
  style.bodyBg = wireframe ? style.containerBg : QColor(0, 0, 0, 0);
  style.footerBg = wireframe ? style.containerBg : QColor(0, 0, 0, 0);
  style.borderColor = toColor(map.colorBorderSecondary, QColor("#f0f0f0"));
  style.titleColor = toColor(map.colorText, QColor("#141414"));
  style.bodyColor = toColor(map.colorText, QColor("#141414"));
  style.closeIconColor = toColor(map.colorTextTertiary, QColor("#8c8c8c"));
  style.closeButtonBackground = QColor(Qt::transparent);
  style.closeButtonHoverBackground = toColor(map.colorFillTertiary, QColor(0, 0, 0, 20));
  style.closeButtonPressedBackground =
      toColor(map.colorFillSecondary, style.closeButtonHoverBackground);
  style.closeButtonDisabledBackground = QColor(Qt::transparent);
  style.closeButtonBorderColor = QColor(Qt::transparent);
  style.width = preferredWidth_;
  style.zIndex = static_cast<int>(std::round(seed.zIndexPopupBase));
  style.borderRadius = std::max(0, qRound(map.borderRadiusLG));
  style.borderWidth = 0;
  style.contentPaddingHorizontal = wireframe ? 0 : std::max(0, qRound(map.sizeLG));
  style.contentPaddingVertical = wireframe ? 0 : std::max(0, qRound(map.sizeMD));
  style.headerPaddingHorizontal = wireframe ? std::max(0, qRound(map.sizeLG)) : 0;
  style.headerPaddingVertical = wireframe ? std::max(0, qRound(map.size)) : 0;
  style.headerMarginBottom = wireframe ? 0 : std::max(0, qRound(map.sizeXS));
  style.bodyPaddingHorizontal = wireframe ? std::max(0, qRound(map.sizeLG)) : 0;
  style.bodyPaddingVertical = wireframe ? std::max(0, qRound(map.sizeLG)) : 0;
  style.footerPaddingHorizontal = wireframe ? std::max(0, qRound(map.size)) : 0;
  style.footerPaddingVertical = wireframe ? std::max(0, qRound(map.sizeXS)) : 0;
  style.footerMarginTop = wireframe ? 0 : std::max(0, qRound(map.sizeSM));
  style.footerBorderTopWidth = wireframe ? std::max(0, qRound(map.lineWidth)) : 0;
  style.footerButtonGap = std::max(4, qRound(map.sizeXS));
  style.confirmIconGap =
      wireframe ? std::max(0, qRound(map.size)) : std::max(0, qRound(map.sizeSM));
  style.confirmParagraphGap = std::max(0, qRound(map.sizeXS));
  style.textLineHeight = std::max(0, qRound(map.fontHeight));
  style.titleLineHeight = std::max(0, qRound(map.fontSizeHeading5 * map.lineHeightHeading5));
  style.iconSize = std::max(12, qRound(map.fontHeight));
  style.closeButtonSize = std::max(24, qRound(map.controlHeight));
  style.closeIconSize = std::max(12, qRound(map.fontSizeLG));
  style.closeButtonRadius = std::max(0, qRound(map.borderRadiusSM));
  style.closeButtonBorderWidth = 0.0;
  style.titleFont = baseFont;
  style.titleFont.setPixelSize(std::max(12, qRound(map.fontSizeHeading5)));
  style.titleFont.setWeight(QFont::DemiBold);
  style.bodyFont = baseFont;
  style.bodyFont.setPixelSize(std::max(12, qRound(map.fontSize)));
  style.bodyFont.setWeight(QFont::Normal);

  switch (preset_) {
    case Preset::Info:
      style.iconColor = toColor(map.colorInfo, QColor("#1677ff"));
      break;
    case Preset::Success:
      style.iconColor = toColor(map.colorSuccess, QColor("#52c41a"));
      break;
    case Preset::Error:
      style.iconColor = toColor(map.colorError, QColor("#ff4d4f"));
      break;
    case Preset::Warning:
    case Preset::Confirm:
      style.iconColor = toColor(map.colorWarning, QColor("#faad14"));
      break;
    case Preset::Plain:
      style.iconColor = toColor(map.colorInfo, QColor("#1677ff"));
      break;
  }

  if (componentTokens_.width.has_value()) {
    style.width = std::max(240, componentTokens_.width.value());
  }
  if (componentTokens_.zIndexPopup.has_value()) {
    style.zIndex = std::max(0, componentTokens_.zIndexPopup.value());
  }
  if (componentTokens_.borderRadius.has_value()) {
    style.borderRadius = std::max(0, componentTokens_.borderRadius.value());
  }
  if (componentTokens_.borderWidth.has_value()) {
    style.borderWidth = std::max(0, componentTokens_.borderWidth.value());
  }
  if (componentTokens_.headerPaddingHorizontal.has_value()) {
    style.headerPaddingHorizontal = std::max(0, componentTokens_.headerPaddingHorizontal.value());
  }
  if (componentTokens_.headerPaddingVertical.has_value()) {
    style.headerPaddingVertical = std::max(0, componentTokens_.headerPaddingVertical.value());
  }
  if (componentTokens_.bodyPaddingHorizontal.has_value()) {
    style.bodyPaddingHorizontal = std::max(0, componentTokens_.bodyPaddingHorizontal.value());
  }
  if (componentTokens_.bodyPaddingVertical.has_value()) {
    style.bodyPaddingVertical = std::max(0, componentTokens_.bodyPaddingVertical.value());
  }
  if (componentTokens_.footerPaddingHorizontal.has_value()) {
    style.footerPaddingHorizontal = std::max(0, componentTokens_.footerPaddingHorizontal.value());
  }
  if (componentTokens_.footerPaddingVertical.has_value()) {
    style.footerPaddingVertical = std::max(0, componentTokens_.footerPaddingVertical.value());
  }
  if (componentTokens_.footerButtonGap.has_value()) {
    style.footerButtonGap = std::max(0, componentTokens_.footerButtonGap.value());
  }
  if (componentTokens_.iconSize.has_value()) {
    style.iconSize = std::max(10, componentTokens_.iconSize.value());
  }

  style.maskBg = parseColorToken(componentTokens_.maskBg, style.maskBg);
  style.containerBg = parseColorToken(componentTokens_.contentBg, style.containerBg);
  style.headerBg = parseColorToken(componentTokens_.headerBg, style.headerBg);
  style.bodyBg = parseColorToken(componentTokens_.bodyBg, style.bodyBg);
  style.footerBg = parseColorToken(componentTokens_.footerBg, style.footerBg);
  style.borderColor = parseColorToken(componentTokens_.borderColor, style.borderColor);
  style.titleColor = parseColorToken(componentTokens_.titleColor, style.titleColor);
  style.bodyColor = parseColorToken(componentTokens_.bodyColor, style.bodyColor);
  style.iconColor = parseColorToken(componentTokens_.iconColor, style.iconColor);
  style.closeIconColor = parseColorToken(componentTokens_.closeIconColor, style.closeIconColor);

  StyleContext context;
  context.open = open_;
  context.mode = mode_;
  context.centered = centered_;
  context.contentLoading = contentLoading_;
  context.acceptButtonBusy = acceptButtonBusy_;
  context.maskVisible = !usesWindowSurface() && maskVisible_;
  context.closeButtonVisible = closeButtonVisible_;
  context.standardButtons = standardButtons_;
  context.preset = preset_;

  const SemanticStyles effectiveStyles =
      semanticStyleResolver_ ? semanticStyleResolver_(context) : semanticStyles_;
  applySemanticSlot(effectiveStyles.root, nullptr, &style.rootBg, &style.borderColor);
  applySemanticSlot(effectiveStyles.mask, nullptr, &style.maskBg, nullptr);
  applySemanticSlot(effectiveStyles.container, nullptr, &style.containerBg, &style.borderColor);
  applySemanticSlot(effectiveStyles.header, nullptr, &style.headerBg, &style.borderColor);
  applySemanticSlot(effectiveStyles.title, &style.titleColor, nullptr, nullptr);
  applySemanticSlot(effectiveStyles.body, &style.bodyColor, &style.bodyBg, nullptr);
  applySemanticSlot(effectiveStyles.footer, nullptr, &style.footerBg, &style.borderColor);
  applySemanticSlot(effectiveStyles.icon, &style.iconColor, nullptr, nullptr);
  if (effectiveStyles.close.textColor.has_value()) {
    style.closeIconColor = effectiveStyles.close.textColor.value();
  }
  if (effectiveStyles.close.backgroundColor.has_value()) {
    style.closeButtonBackground = effectiveStyles.close.backgroundColor.value();
    style.closeButtonHoverBackground = effectiveStyles.close.backgroundColor.value();
    style.closeButtonPressedBackground = effectiveStyles.close.backgroundColor.value();
  }
  if (effectiveStyles.close.borderColor.has_value()) {
    style.closeButtonBorderColor = effectiveStyles.close.borderColor.value();
    style.closeButtonBorderWidth =
        style.closeButtonBorderColor.alpha() > 0 ? std::max<qreal>(1.0, map.lineWidth) : 0.0;
  }

  return style;
}

void AdModal::setOpenInternal(bool value, bool emitSignal) {
  if (open_ == value) {
    if (open_) {
      ensureOverlay();
      refreshTexts();
      refreshVisibility();
      refreshLayout();
      applyVisualStyle();
      syncOverlayGeometry();
      if (overlay_) {
        overlay_->show();
        restackOpenModals(ownerWindow_ ? ownerWindow_.data() : resolveOwnerWindow());
        if (usesWindowSurface()) {
          syncOverlayGeometry();
          overlay_->raise();
          overlay_->activateWindow();
        }
        QWidget* initialFocus = resolveInitialFocusTarget();
        if (initialFocus) {
          initialFocus->setFocus(Qt::PopupFocusReason);
        } else {
          overlay_->setFocus(Qt::PopupFocusReason);
        }
      }
    }
    return;
  }

  open_ = value;
  if (open_) {
    saveFocusBeforeOpen();
    ensureOverlay();
    refreshTexts();
    refreshVisibility();
    refreshLayout();
    applyVisualStyle();
    syncOverlayGeometry();
    if (overlay_) {
      if (usesWindowSurface()) {
        overlay_->setWindowModality(windowModality_);
      }
      overlay_->show();
      registerOpenModal(this);
      if (usesWindowSurface()) {
        syncOverlayGeometry();
        overlay_->raise();
        overlay_->activateWindow();
      }
      QWidget* initialFocus = resolveInitialFocusTarget();
      if (initialFocus) {
        initialFocus->setFocus(Qt::PopupFocusReason);
      } else {
        overlay_->setFocus(Qt::PopupFocusReason);
      }
    }
  } else {
    unregisterOpenModal(this);
    if (overlay_) {
      overlay_->hide();
      if (usesWindowSurface()) {
        overlay_->setWindowModality(Qt::NonModal);
      }
    }
    restoreFocusAfterClose();
  }

  if (emitSignal) {
    emit openChanged(open_);
  }
}

bool AdModal::emitCloseRequestedSafely(CloseReason reason) {
  if (emittingCloseRequested_) {
    return true;
  }

  QPointer<AdModal> guard(this);
  pendingCloseReason_ = reason;
  QScopedValueRollback<bool> emittingGuard(emittingCloseRequested_, true);
  emit closeRequested(reason);
  return !guard.isNull();
}

AdModal::CloseReason AdModal::effectiveProgrammaticReason(CloseReason fallback) const {
  return pendingCloseReason_.value_or(fallback);
}

void AdModal::requestAccept() {
  if (acceptButtonBusy_) {
    return;
  }

  if (closePolicy_ == ClosePolicy::Manual) {
    emitCloseRequestedSafely(CloseReason::OkAction);
    return;
  }

  finalizeClose(DialogCode::Accepted, CloseReason::OkAction);
}

void AdModal::requestReject(CloseReason reason, bool ignoreBusy) {
  if (!ignoreBusy && acceptButtonBusy_) {
    return;
  }

  const bool forceClose = reason == CloseReason::ScopeHidden;
  if (closePolicy_ == ClosePolicy::Manual && !forceClose) {
    emitCloseRequestedSafely(reason);
    return;
  }

  finalizeClose(DialogCode::Rejected, reason);
}

void AdModal::finalizeClose(DialogCode code, CloseReason reason) {
  result_ = code;
  if (!open_) {
    pendingCloseReason_.reset();
    return;
  }

  pendingCloseReason_.reset();
  setOpenInternal(false, true);

  QPointer<AdModal> guard(this);
  emit closed(reason);
  if (!guard) {
    return;
  }

  if (code == DialogCode::Accepted) {
    emit accepted();
  } else {
    emit rejected();
  }
  if (!guard) {
    return;
  }

  emit finished(code);
  if (!guard) {
    return;
  }

  if (staticServiceOwned_ && !deletionScheduled_) {
    deletionScheduled_ = true;
    unregisterStaticServiceModal(this);
    deleteLater();
  }
}

QColor AdModal::parseColorToken(const std::optional<QColor>& token, const QColor& fallback) {
  if (!token.has_value()) {
    return fallback;
  }
  return toColor(token.value(), fallback);
}

void AdModal::applySemanticSlot(const SemanticSlotStyle& slot, QColor* textColor,
                                QColor* backgroundColor, QColor* borderColor) {
  if (textColor && slot.textColor.has_value()) {
    *textColor = slot.textColor.value();
  }
  if (backgroundColor && slot.backgroundColor.has_value()) {
    *backgroundColor = slot.backgroundColor.value();
  }
  if (borderColor && slot.borderColor.has_value()) {
    *borderColor = slot.borderColor.value();
  }
}

AdModal* AdModalService::showInfo(const Request& request, QWidget* ownerWindow) {
  return showStaticRequest(request, AdModal::Preset::Info, ownerWindow);
}

AdModal* AdModalService::showSuccess(const Request& request, QWidget* ownerWindow) {
  return showStaticRequest(request, AdModal::Preset::Success, ownerWindow);
}

AdModal* AdModalService::showError(const Request& request, QWidget* ownerWindow) {
  return showStaticRequest(request, AdModal::Preset::Error, ownerWindow);
}

AdModal* AdModalService::showWarning(const Request& request, QWidget* ownerWindow) {
  return showStaticRequest(request, AdModal::Preset::Warning, ownerWindow);
}

AdModal* AdModalService::showConfirm(const Request& request, QWidget* ownerWindow) {
  return showStaticRequest(request, AdModal::Preset::Confirm, ownerWindow);
}

void AdModalService::closeAll() {
  const QVector<QPointer<AdModal>> modals = staticModals();
  for (const QPointer<AdModal>& modal : modals) {
    if (modal) {
      modal->close();
    }
  }
}

}  // namespace adqt::widgets
