#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/screenshottoolbarwindow.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QLineEdit>
#include <QObject>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/popover.h"
#include "widgets/select.h"
#include "widgets/tooltip.h"

#include <cstdlib>
#include <iostream>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
HWND toNativeHwnd(WId windowId) {
    // Qt transports the native HWND through its integer-valued WId type.
    return reinterpret_cast<HWND>(windowId); // NOLINT(performance-no-int-to-ptr)
}
#endif

class NoOpToolbarCommands final : public ScreenshotToolbarCommandSink {
  public:
    int selectToolRequests = 0;
    int arrowToolRequests = 0;
    int lineToolRequests = 0;

    void setMoveTool() override {}
    void setSelectTool() override {
        ++selectToolRequests;
    }
    void setShapeTool() override {}
    void setArrowTool() override {
        ++arrowToolRequests;
    }
    void setLineTool() override {
        ++lineToolRequests;
    }
    void setFreeDrawTool() override {}
    void setHighlightTool() override {}
    void setPenHighlightTool() override {}
    void setEraserTool() override {}
    void setFilterTool() override {}
    void setWatermarkTool() override {}
    void setWatermarkConfigFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void previewWatermarkFromToolbar(const SnowCanvasWatermarkConfig&) override {}
    void setFilterStyleFromToolbar(const SnowCanvasFilterStyle&, quint32) override {}
    void setTextTool() override {}
    void setSerialNumberTool() override {}
    void setOcrTool() override {}
    void startScrollingScreenshot() override {}
    void pinSelectionToScreen() override {}
    void cancelCapture() override {}
    void copySelectionToClipboard() override {}
    void startScreenRecording() override {}
    void setShapeStyleFromToolbar(const SnowCanvasShapeStyle&, quint32,
                                  SnowCanvasShapeKind) override {}
    void setTextStyleFromToolbar(const SnowCanvasTextStyle&) override {}
    void setSerialNumberStyleFromToolbar(const SnowCanvasSerialNumberStyle&) override {}
    void decrementSelectedSerialNumbers() override {}
    void incrementSelectedSerialNumbers() override {}
    void createTextForSelectedSerialNumber() override {}
    void repositionToolbarForContentChange() override {}
    void hideColorPickersForScreenshotUi() override {}
};

class MouseEventProbe final : public QObject {
  public:
    int eventCount = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched)
        if (event != nullptr &&
            (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter ||
             event->type() == QEvent::MouseMove || event->type() == QEvent::HoverMove)) {
            ++eventCount;
        }
        return false;
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void waitFor(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
    QCoreApplication::processEvents();
}

adqt::widgets::AdButton* toolbarButton(ScreenshotToolbarWindow& toolbar, const QString& tooltip) {
    for (QWidget* control : toolbar.findChildren<QWidget*>()) {
        if (control != nullptr &&
            (control->toolTip() == tooltip ||
             control->toolTip().startsWith(tooltip + QStringLiteral(" (")))) {
            return qobject_cast<adqt::widgets::AdButton*>(control);
        }
    }
    return nullptr;
}

adqt::widgets::AdButton* popoverButton(adqt::widgets::AdPopover* popover, const QString& tooltip) {
    if (popover == nullptr || popover->contentWidget() == nullptr) {
        return nullptr;
    }
    for (adqt::widgets::AdButton* button :
         popover->contentWidget()->findChildren<adqt::widgets::AdButton*>()) {
        if (button != nullptr &&
            (button->toolTip() == tooltip ||
             button->toolTip().startsWith(tooltip + QStringLiteral(" (")))) {
            return button;
        }
    }
    return nullptr;
}

adqt::widgets::AdColorPicker* fillColorPicker(ScreenshotToolbarWindow& toolbar) {
    for (adqt::widgets::AdColorPicker* picker :
         toolbar.findChildren<adqt::widgets::AdColorPicker*>()) {
        if (picker != nullptr && picker->accessibleName() == QStringLiteral("Fill color")) {
            return picker;
        }
    }
    return nullptr;
}

QWidget* visiblePopoverSurface() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget != nullptr && widget->isVisible() &&
            widget->objectName() == QStringLiteral("adpopover-surface")) {
            return widget;
        }
    }
    return nullptr;
}

QWidget* visibleTooltipSurface() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget != nullptr && widget->isVisible() &&
            widget->objectName() == QStringLiteral("adtooltip-surface")) {
            return widget;
        }
    }
    return nullptr;
}

void requireTooltipWindowHierarchy(ScreenshotToolbarWindow& toolbar, QWidget& tooltipSurface) {
    require(tooltipSurface.windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "screenshot toolbar tooltip should inherit the always-on-top window flag");
    require(tooltipSurface.windowHandle() != nullptr && toolbar.windowHandle() != nullptr,
            "tooltip and toolbar should have native windows");
    require(tooltipSurface.windowHandle()->transientParent() == toolbar.windowHandle(),
            "tooltip should be transient for the screenshot toolbar");

#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND toolbarHwnd = toNativeHwnd(toolbar.winId());
    const HWND tooltipHwnd = toNativeHwnd(tooltipSurface.winId());
    require(GetWindow(tooltipHwnd, GW_OWNER) == toolbarHwnd,
            "tooltip HWND should be owned by the toolbar HWND");
    require((GetWindowLongPtr(tooltipHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
            "tooltip HWND should be in the topmost band");
#endif
}

void requireProductionWindowHierarchy(QWidget& overlay, ScreenshotToolbarWindow& toolbar,
                                      QWidget& popupSurface) {
    require(overlay.windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "screenshot overlay should be configured as always-on-top");
    require(toolbar.windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "screenshot toolbar should be configured as always-on-top");
    require(toolbar.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus),
            "screenshot toolbar should not accept focus");
    require(popupSurface.windowHandle() != nullptr && toolbar.windowHandle() != nullptr,
            "popup and toolbar should have native windows");
    require(popupSurface.windowHandle()->transientParent() == toolbar.windowHandle(),
            "color picker popup should be transient for the screenshot toolbar");

#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND overlayHwnd = toNativeHwnd(overlay.winId());
    const HWND toolbarHwnd = toNativeHwnd(toolbar.winId());
    const HWND popupHwnd = toNativeHwnd(popupSurface.winId());
    require(GetWindow(toolbarHwnd, GW_OWNER) == overlayHwnd,
            "screenshot toolbar HWND should be owned by the overlay HWND");
    require(GetWindow(popupHwnd, GW_OWNER) == toolbarHwnd,
            "color picker popup HWND should be owned by the toolbar HWND");
    require((GetWindowLongPtr(overlayHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
            "screenshot overlay HWND should be in the topmost band");
    require((GetWindowLongPtr(toolbarHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
            "screenshot toolbar HWND should be in the topmost band");
    require((GetWindowLongPtr(popupHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
            "color picker popup HWND should be in the topmost band");

    RECT popupRect{};
    require(GetWindowRect(popupHwnd, &popupRect) != FALSE,
            "color picker popup should have a native window rectangle");
    const POINT nativePopupCenter{popupRect.left + (popupRect.right - popupRect.left) / 2,
                                  popupRect.top + (popupRect.bottom - popupRect.top) / 2};
    const HWND windowAtPopupCenter = WindowFromPoint(nativePopupCenter);
    require(windowAtPopupCenter != nullptr &&
                GetAncestor(windowAtPopupCenter, GA_ROOT) == popupHwnd,
            "color picker popup should be the top-level window visible at its center");
#endif
}

void moveSystemMouseTo(const QPoint& globalPosition) {
    QCursor::setPos(globalPosition);
    for (int attempt = 0; attempt < 5 && QCursor::pos() != globalPosition; ++attempt) {
        waitFor(50);
        QCursor::setPos(globalPosition);
    }
    require(QCursor::pos() == globalPosition, "system mouse should move to the requested position");
}

void clickSystemMouseAt(const QPoint& globalPosition) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    moveSystemMouseTo(globalPosition);
    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "the native mouse click should be delivered");
    waitFor(100);
#else
    Q_UNUSED(globalPosition)
#endif
}

void typeSystemText(const QString& text) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    for (const QChar character : text) {
        INPUT input[2]{};
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wScan = character.unicode();
        input[0].ki.dwFlags = KEYEVENTF_UNICODE;
        input[1] = input[0];
        input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        require(SendInput(2, input, sizeof(INPUT)) == 2, "native text input should be delivered");
    }
    waitFor(100);
#else
    Q_UNUSED(text)
#endif
}

void pressSystemEnter() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_RETURN;
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "the native Enter key should be delivered");
    waitFor(100);
#endif
}

void pressSystemEscape() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_ESCAPE;
    input[1] = input[0];
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, input, sizeof(INPUT)) == 2, "the native Escape key should be delivered");
    waitFor(100);
#endif
}

bool capturedControlHasVisualDetail(const QImage& image, const QRect& logicalControlRect,
                                    const QSize& logicalWindowSize) {
    if (image.isNull() || logicalControlRect.isEmpty() || logicalWindowSize.isEmpty()) {
        return false;
    }

    const qreal scaleX = static_cast<qreal>(image.width()) / logicalWindowSize.width();
    const qreal scaleY = static_cast<qreal>(image.height()) / logicalWindowSize.height();
    const QRect pixelRect(
        qFloor(logicalControlRect.left() * scaleX), qFloor(logicalControlRect.top() * scaleY),
        qCeil(logicalControlRect.width() * scaleX), qCeil(logicalControlRect.height() * scaleY));
    const QRect bounded = pixelRect.intersected(image.rect());
    if (bounded.isEmpty()) {
        return false;
    }

    int minimumLuminance = 255;
    int maximumLuminance = 0;
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const int luminance = qGray(color.rgb());
            minimumLuminance = qMin(minimumLuminance, luminance);
            maximumLuminance = qMax(maximumLuminance, luminance);
        }
    }
    return maximumLuminance - minimumLuminance >= 32;
}

void requirePopupShadowPassesMouseThroughToTrigger(QWidget& popupSurface, QWidget& trigger) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND popupHwnd = toNativeHwnd(popupSurface.winId());
    RECT popupRect{};
    require(GetWindowRect(popupHwnd, &popupRect) != FALSE,
            "color picker popup should expose its native hit-test rectangle");

    const QList<QPoint> localSamples{
        QPoint(trigger.width() / 4, trigger.height() / 2),
        QPoint(trigger.width() / 2, trigger.height() / 2),
        QPoint(trigger.width() * 3 / 4, trigger.height() / 2),
        QPoint(trigger.width() / 4, trigger.height() - 3),
        QPoint(trigger.width() / 2, trigger.height() - 3),
        QPoint(trigger.width() * 3 / 4, trigger.height() - 3),
    };
    MouseEventProbe mouseEventProbe;
    trigger.installEventFilter(&mouseEventProbe);
    int overlappedSampleCount = 0;
    for (const QPoint& localSample : localSamples) {
        const int previousEventCount = mouseEventProbe.eventCount;
        moveSystemMouseTo(trigger.mapToGlobal(localSample));
        waitFor(20);
        POINT nativeCursorPosition{};
        require(GetCursorPos(&nativeCursorPosition) != FALSE,
                "the native cursor position should be available");
        if (PtInRect(&popupRect, nativeCursorPosition) == FALSE) {
            continue;
        }

        ++overlappedSampleCount;
        const LPARAM hitTestPosition = MAKELPARAM(nativeCursorPosition.x, nativeCursorPosition.y);
        require(SendMessageW(popupHwnd, WM_NCHITTEST, 0, hitTestPosition) == HTTRANSPARENT,
                "the transparent popup shadow should pass mouse events through");
        require(mouseEventProbe.eventCount > previousEventCount,
                "the popup shadow must not block any overlapped trigger area");
    }
    require(overlappedSampleCount > 0,
            "the popup shadow should overlap a sampled part of the trigger");
#else
    Q_UNUSED(popupSurface)
    Q_UNUSED(trigger)
#endif
}

void hoveringFillColorTriggerShowsPicker() {
    require(QGuiApplication::platformName() == QStringLiteral("windows"),
            "this test requires the real Windows Qt platform plugin");

    const QPoint originalCursorPosition = QCursor::pos();
    NoOpToolbarCommands commands;
    QWidget overlay(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    overlay.setObjectName(QStringLiteral("productionOverlaySurrogate"));
    overlay.setAttribute(Qt::WA_TranslucentBackground, true);

    ScreenshotToolbarWindow toolbar(commands);
    toolbar.setOwnerWindow(&overlay);
    require(toolbar.paletteHost() != nullptr,
            "screenshot toolbar should have its production palette host");
    toolbar.paletteHost()->setStyleToolbarVisible(true);
    toolbar.prepareForDisplay();

    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a real display should be available");
    const QRect availableGeometry = screen->availableGeometry();
    overlay.setGeometry(availableGeometry);
    overlay.show();
    overlay.raise();
    overlay.activateWindow();
    waitFor(100);

    toolbar.setPlacementContext(screen, availableGeometry, availableGeometry);
    toolbar.moveContentTo(
        availableGeometry.center() -
        QPoint(toolbar.contentSizeHint().width() / 2, toolbar.contentSizeHint().height() / 2));
    QPoint outsidePosition = availableGeometry.topLeft() + QPoint(8, 8);
    moveSystemMouseTo(outsidePosition);
    toolbar.show();
    toolbar.raise();
    waitFor(150);
    require(toolbar.isVisible(), "screenshot toolbar should be visible");

    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    waitFor(50);
    auto* watermarkText =
        toolbar.findChild<QLineEdit*>(QStringLiteral("screenshotWatermarkTextEdit"));
    require(watermarkText != nullptr && watermarkText->isVisible(),
            "watermark text editor should be visible");
    adqt::widgets::AdButton* rectangleButton = toolbarButton(toolbar, QStringLiteral("Shape"));
    require(rectangleButton != nullptr, "shape tool should be present");
    const QRect rectangleButtonRect(rectangleButton->mapTo(&toolbar, QPoint()),
                                    rectangleButton->size());
    const QRect toolbarScreenRect = toolbar.frameGeometry();
    const QImage toolbarBeforeEditing =
        screen
            ->grabWindow(0, toolbarScreenRect.x(), toolbarScreenRect.y(), toolbarScreenRect.width(),
                         toolbarScreenRect.height())
            .toImage();
    require(
        capturedControlHasVisualDetail(toolbarBeforeEditing, rectangleButtonRect, toolbar.size()),
        "the native toolbar capture should contain the shape control before editing");
#if defined(Q_OS_WIN) || defined(_WIN32)
    const HWND overlayHwnd = toNativeHwnd(overlay.winId());
    const HWND toolbarHwnd = toNativeHwnd(toolbar.winId());
    RECT toolbarRectBeforeEditing{};
    require(GetWindowRect(toolbarHwnd, &toolbarRectBeforeEditing) != FALSE,
            "the toolbar should expose its native bounds before editing");
    require((GetWindowLongPtr(toolbarHwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
            "the toolbar should start in its non-activating state");
#endif

    watermarkText->clear();
    clickSystemMouseAt(watermarkText->mapToGlobal(watermarkText->rect().center()));
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(GetForegroundWindow() == toolbarHwnd && watermarkText->hasFocus(),
            "clicking the watermark editor should activate it for keyboard input");
    require((GetWindowLongPtr(toolbarHwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0,
            "watermark editing should temporarily remove WS_EX_NOACTIVATE");
    require(toNativeHwnd(toolbar.winId()) == toolbarHwnd,
            "watermark editing must preserve the existing toolbar HWND");
    RECT toolbarRectWhileEditing{};
    require(GetWindowRect(toolbarHwnd, &toolbarRectWhileEditing) != FALSE &&
                EqualRect(&toolbarRectBeforeEditing, &toolbarRectWhileEditing) != FALSE,
            "watermark editing must preserve the complete toolbar surface bounds");
    require(IsWindowVisible(toolbarHwnd) != FALSE && toolbar.paletteHost() != nullptr &&
                toolbar.paletteHost()->isVisible() && toolbar.palette()->mainPanel()->isVisible() &&
                toolbar.palette()->stylePanel()->isVisible(),
            "activating the watermark editor must keep every toolbar panel visible");
#endif
    const QImage toolbarWhileEditing =
        screen
            ->grabWindow(0, toolbarScreenRect.x(), toolbarScreenRect.y(), toolbarScreenRect.width(),
                         toolbarScreenRect.height())
            .toImage();
    require(
        capturedControlHasVisualDetail(toolbarWhileEditing, rectangleButtonRect, toolbar.size()),
        "activating the watermark editor must keep the full native toolbar painted");
    typeSystemText(QStringLiteral("typed watermark"));
    require(watermarkText->text() == QStringLiteral("typed watermark"),
            "native keyboard input should update the watermark text");
    pressSystemEnter();
#if defined(Q_OS_WIN) || defined(_WIN32)
    require((GetWindowLongPtr(toolbarHwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
            "finishing watermark editing should restore WS_EX_NOACTIVATE");
    require(GetForegroundWindow() == overlayHwnd,
            "finishing watermark editing should return focus to the overlay");
#endif

    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Text);
    waitFor(50);
    adqt::widgets::AdSelect* fontSelect = nullptr;
    for (adqt::widgets::AdSelect* select : toolbar.findChildren<adqt::widgets::AdSelect*>()) {
        if (select != nullptr && select->accessibleName() == QStringLiteral("Text font family")) {
            fontSelect = select;
            break;
        }
    }
    require(fontSelect != nullptr && fontSelect->isVisible() && fontSelect->searchEnabled() &&
                fontSelect->lineEdit() != nullptr,
            "the text style toolbar should expose a searchable font selector");
    clickSystemMouseAt(
        fontSelect->lineEdit()->mapToGlobal(fontSelect->lineEdit()->rect().center()));
    require(fontSelect->popupVisible() && fontSelect->lineEdit()->hasFocus(),
            "opening the font selector should focus its search editor");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(GetForegroundWindow() == toolbarHwnd &&
                (GetWindowLongPtr(toolbarHwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0,
            "opening a searchable selector should activate the toolbar for keyboard input");
#endif
    typeSystemText(QStringLiteral("a"));
    require(fontSelect->searchText() == QStringLiteral("a"),
            "native keyboard input should filter the font selector");
    pressSystemEscape();
    require(!fontSelect->popupVisible(), "Escape should close the searchable font selector");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require((GetWindowLongPtr(toolbarHwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0 &&
                GetForegroundWindow() == overlayHwnd,
            "closing the searchable selector should restore the non-activating toolbar");
#endif
    adqt::widgets::AdTooltip::showText(rectangleButton, rectangleButton->toolTip(), 5000);
    waitFor(100);
    QWidget* tooltipSurface = visibleTooltipSurface();
    require(tooltipSurface != nullptr,
            "showing a screenshot toolbar tooltip should create a visible surface");
    requireTooltipWindowHierarchy(toolbar, *tooltipSurface);
    adqt::widgets::AdTooltip::showText(rectangleButton, QString());
    waitFor(350);

    rectangleButton->click();
    waitFor(50);

    adqt::widgets::AdColorPicker* fillPicker = fillColorPicker(toolbar);
    require(fillPicker != nullptr && fillPicker->isVisible(),
            "fill color picker should be visible for the shape tool");
    require(fillPicker->popupLayerMode() == adqt::widgets::AdColorPicker::PopupLayerMode::QtTool,
            "production fill color picker should use the top-level popup layer");

    auto* popover = fillPicker->findChild<adqt::widgets::AdPopover*>();
    require(popover != nullptr, "fill color picker should own a popup");
    QWidget* trigger = popover->sourceWidget();
    require(trigger != nullptr && trigger->isVisible(),
            "fill color picker trigger should be visible");
    require(!fillPicker->popupVisible(), "fill color picker should start hidden");

    if (toolbar.frameGeometry().contains(outsidePosition)) {
        outsidePosition = availableGeometry.bottomRight() - QPoint(8, 8);
    }
    moveSystemMouseTo(outsidePosition);
    waitFor(popover->hoverCloseDelayMs() + 50);

    const QPoint triggerPosition = trigger->mapToGlobal(trigger->rect().center());
    moveSystemMouseTo(triggerPosition);
    QWidget* widgetUnderMouse = nullptr;
    for (int attempt = 0; attempt < 10; ++attempt) {
        waitFor(50);
        widgetUnderMouse = QApplication::widgetAt(QCursor::pos());
        if (widgetUnderMouse == trigger || trigger->isAncestorOf(widgetUnderMouse)) {
            break;
        }
        moveSystemMouseTo(triggerPosition);
    }
    require(widgetUnderMouse == trigger || trigger->isAncestorOf(widgetUnderMouse),
            "the real system mouse should be inside the fill color trigger");
    for (int attempt = 0; attempt < 10 && !fillPicker->popupVisible(); ++attempt) {
        waitFor(50);
    }

    require(fillPicker->popupVisible() && popover->isVisible(),
            "hovering the fill color trigger should display the color picker");
    QWidget* popupSurface = visiblePopoverSurface();
    require(popupSurface != nullptr,
            "hovering the fill color trigger should show a top-level popup surface");
    requireProductionWindowHierarchy(overlay, toolbar, *popupSurface);
    requirePopupShadowPassesMouseThroughToTrigger(*popupSurface, *trigger);

    moveSystemMouseTo(outsidePosition);
    for (int attempt = 0; attempt < 20 && fillPicker->popupVisible(); ++attempt) {
        waitFor(25);
    }
    require(!fillPicker->popupVisible() && !popover->isVisible() && !popupSurface->isVisible(),
            "moving the real cursor outside must automatically close the QtTool popover");

    for (int cycle = 0; cycle < 5; ++cycle) {
        moveSystemMouseTo(triggerPosition);
        for (int attempt = 0; attempt < 20 && !fillPicker->popupVisible(); ++attempt) {
            waitFor(25);
        }
        require(fillPicker->popupVisible(),
                "the hover popover should reopen after an automatic close");

        moveSystemMouseTo(outsidePosition);
        for (int attempt = 0; attempt < 20 && fillPicker->popupVisible(); ++attempt) {
            waitFor(25);
        }
        require(!fillPicker->popupVisible(),
                "repeated cursor exits must never leave the hover popover stuck open");
    }

    auto* arrowLineButton =
        toolbar.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotArrowLineButton"));
    auto* arrowLinePopover = arrowLineButton == nullptr
                                 ? nullptr
                                 : arrowLineButton->findChild<adqt::widgets::AdPopover*>();
    require(arrowLineButton != nullptr && arrowLinePopover != nullptr &&
                arrowLinePopover->sourceWidget() == arrowLineButton,
            "Arrow and Line should share the live toolbar's hover popover shell");

    moveSystemMouseTo(outsidePosition);
    waitFor(arrowLinePopover->hoverCloseDelayMs() + 50);
    const auto arrowLinePosition = [arrowLineButton]() {
        return arrowLineButton->mapToGlobal(arrowLineButton->rect().center());
    };
    moveSystemMouseTo(arrowLinePosition());
    for (int attempt = 0; attempt < 10 && !arrowLinePopover->isVisible(); ++attempt) {
        waitFor(50);
        moveSystemMouseTo(arrowLinePosition());
    }
    require(arrowLinePopover->isVisible(),
            "hovering the Arrow and Line trigger should open its horizontal popover");
    auto* lineOption = popoverButton(arrowLinePopover, QStringLiteral("Line"));
    auto* arrowOption = popoverButton(arrowLinePopover, QStringLiteral("Arrow"));
    require(lineOption != nullptr && arrowOption != nullptr,
            "hovering the Arrow and Line trigger should materialize both options");

    const int lineToolRequestsBeforeSelection = commands.lineToolRequests;
    clickSystemMouseAt(lineOption->mapToGlobal(lineOption->rect().center()));
    waitFor(50);
    require(commands.lineToolRequests == lineToolRequestsBeforeSelection + 1 &&
                arrowLineButton->accessibleName() == QStringLiteral("Line") &&
                arrowLineButton->property("screenshotToolbarItemId").toString() ==
                    QStringLiteral("line"),
            "selecting Line should activate it and replace the shared toolbar trigger");

    moveSystemMouseTo(outsidePosition);
    waitFor(arrowLinePopover->hoverCloseDelayMs() + 50);
    const int lineToolRequestsBeforeTrigger = commands.lineToolRequests;
    const int selectToolRequestsBeforeTrigger = commands.selectToolRequests;
    clickSystemMouseAt(arrowLinePosition());
    waitFor(50);
    require(commands.lineToolRequests == lineToolRequestsBeforeTrigger &&
                commands.selectToolRequests == selectToolRequestsBeforeTrigger + 1,
            "clicking the active replaced trigger should return to selection mode");

    moveSystemMouseTo(outsidePosition);
    waitFor(arrowLinePopover->hoverCloseDelayMs() + 50);
    moveSystemMouseTo(arrowLinePosition());
    for (int attempt = 0; attempt < 10 && !arrowLinePopover->isVisible(); ++attempt) {
        waitFor(50);
        moveSystemMouseTo(arrowLinePosition());
    }
    require(arrowLinePopover->isVisible(),
            "the drawing-tool popover should reopen after selecting an entry");
    const int arrowToolRequestsBeforeSelection = commands.arrowToolRequests;
    clickSystemMouseAt(arrowOption->mapToGlobal(arrowOption->rect().center()));
    waitFor(50);
    require(commands.arrowToolRequests == arrowToolRequestsBeforeSelection + 1 &&
                arrowLineButton->accessibleName() == QStringLiteral("Arrow"),
            "selecting Arrow should restore it as the shared toolbar trigger");
    moveSystemMouseTo(outsidePosition);

    toolbar.hide();
    overlay.hide();
    QCursor::setPos(originalCursorPosition);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    hoveringFillColorTriggerShowsPicker();
    return 0;
}
