#include "screenshotselectiontoolbarwidgets.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "theme/theme_manager.h"
#include "widgets/select.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QScreen>
#include <QTimer>
#include <QTranslator>
#include <QWidget>
#include <QWheelEvent>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <vector>

namespace {
class NoOpSelectionToolbarCommands final : public ScreenshotSelectionToolbarCommandSink {
  public:
    void toggleSelectionAspectRatioLockFromToolbar() override {
        ++lockToggleCount;
        ++interactionCount;
    }
    void
    setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset preset) override {
        selectedPresets.push_back(preset);
        ++interactionCount;
        if (presetSelectionObserver) {
            presetSelectionObserver(preset);
        }
    }
    void openSelectionResizeModalFromToolbar() override {
        ++resizeModalCount;
        ++interactionCount;
    }
    void hideColorPickersForScreenshotUi() override {
        ++interactionCount;
    }
    void adjustSelectionFromToolbar(int minDx, int minDy, int maxDx, int maxDy) override {
        lastAdjustment = {minDx, minDy, maxDx, maxDy};
        ++interactionCount;
    }
    void setSelectionCornerRadiusFromToolbar(int) override {
        ++interactionCount;
    }
    void setSelectionShadowWidthFromToolbar(int) override {
        ++interactionCount;
    }
    void setSelectionToolbarHovered(bool hovered) override {
        toolbarHovered = hovered;
        ++interactionCount;
    }
    void setSelectionToolbarPopupVisible(bool visible) override {
        popupVisibility.push_back(visible);
        if (popupVisibilityObserver) {
            popupVisibilityObserver(visible);
        }
    }

    std::vector<int> lastAdjustment;
    int interactionCount = 0;
    int lockToggleCount = 0;
    int resizeModalCount = 0;
    std::vector<ScreenshotSelectionAspectRatioPreset> selectedPresets;
    std::function<void(ScreenshotSelectionAspectRatioPreset)> presetSelectionObserver;
    std::vector<bool> popupVisibility;
    std::function<void(bool)> popupVisibilityObserver;
    bool toolbarHovered = false;
};

class PixelUnitTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }

    QString translate(const char*, const char* sourceText, const char*, int) const override {
        const QByteArray source(sourceText);
        if (source == "Pixels" || source == "Logical pixels") {
            return QStringLiteral("translated-unit");
        }
        return {};
    }
};

class AspectRatioTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) != "ScreenshotSelectionToolbarWidget") {
            return {};
        }
        if (QByteArray(source) == "Free") {
            return QStringLiteral("Unconstrained selection");
        }
        if (QByteArray(source) == "Selection aspect ratio") {
            return QStringLiteral("Translated aspect ratio");
        }
        return {};
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void sendEnter(QWidget* widget) {
    require(widget != nullptr, "enter-event target should exist");
    const QPointF localPosition(widget->rect().center());
    const QPointF globalPosition(widget->mapToGlobal(localPosition.toPoint()));
    QEnterEvent event(localPosition, localPosition, globalPosition);
    QCoreApplication::sendEvent(widget, &event);
}

void sendLeave(QWidget* widget) {
    require(widget != nullptr, "leave-event target should exist");
    QEvent event(QEvent::Leave);
    QCoreApplication::sendEvent(widget, &event);
}

void sendHide(QWidget* widget) {
    require(widget != nullptr, "hide-event target should exist");
    QHideEvent event;
    QCoreApplication::sendEvent(widget, &event);
}

QImage renderWidget(QWidget* widget, QWidget::RenderFlags flags = QWidget::DrawWindowBackground |
                                                                  QWidget::DrawChildren) {
    require(widget != nullptr, "render target should exist");
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget->render(&painter, QPoint(), QRegion(), flags);
    return image;
}

QImage textInk(const QImage& image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    require(!bounds.isEmpty(), "ratio labels must render visible text");
    return image.copy(bounds);
}

void requireUnclippedRatioText(QLineEdit* input) {
    QLineEdit reference;
    reference.setFrame(false);
    reference.setReadOnly(true);
    reference.setFont(input->font());
    reference.setPalette(input->palette());
    reference.setText(input->text());
    reference.resize(input->width() + 100, input->height());
    require(
        textInk(renderWidget(input, QWidget::DrawChildren)) ==
            textInk(renderWidget(&reference, QWidget::DrawChildren)),
        "compact ratio labels must render every glyph without horizontal scrolling or clipping");
}

void panelBoundaryExclusivelyOwnsToolbarHoverState() {
    SelectionToolbarPanel panel;
    panel.resize(180, screenshot_selection_toolbar::PanelHeight);
    QLabel child(&panel);
    child.setGeometry(20, 2, 60, panel.height() - 4);

    std::vector<bool> hoverTransitions;
    QObject::connect(&panel, &SelectionToolbarPanel::hoverChanged, &panel,
                     [&hoverTransitions](bool hovered) { hoverTransitions.push_back(hovered); });

    require(!panel.hasMouseTracking() && !panel.testAttribute(Qt::WA_Hover),
            "panel boundary tracking should rely on QWidget enter/leave events");

    sendEnter(&child);
    sendLeave(&child);
    require(hoverTransitions.empty(),
            "descendant hover events must not control panel boundary state");

    sendEnter(&panel);
    require(hoverTransitions == std::vector<bool>({true}),
            "entering the panel should begin one hover session");

    sendEnter(&panel);
    require(hoverTransitions == std::vector<bool>({true}),
            "repeated panel enter events must not duplicate the hover transition");

    sendEnter(&child);
    sendLeave(&child);
    require(hoverTransitions == std::vector<bool>({true}),
            "moving across panel descendants must preserve the hover session");

    sendLeave(&panel);
    require(hoverTransitions == std::vector<bool>({true, false}),
            "leaving the panel should end the hover session");

    panel.setPointerInteractionEnabled(false);
    sendEnter(&panel);
    require(hoverTransitions == std::vector<bool>({true, false}),
            "a transparent panel must ignore a stale or synthetic enter event");

    panel.setPointerInteractionEnabled(true);
    sendEnter(&panel);
    require(hoverTransitions == std::vector<bool>({true, false, true}),
            "re-enabling the panel must restore its next hover session");

    sendHide(&panel);
    require(hoverTransitions == std::vector<bool>({true, false, true, false}),
            "hiding the panel must clear its hover state");

    sendEnter(&panel);
    require(hoverTransitions == std::vector<bool>({true, false, true, false, true}),
            "showing the panel must allow a fresh hover session");

    sendLeave(&panel);
    panel.setPointerInteractionEnabled(false);
    require(hoverTransitions == std::vector<bool>({true, false, true, false, true, false}),
            "disabling a hovered panel must synchronously clear its hover state");
    panel.setPointerInteractionEnabled(true);
    sendEnter(&panel);
    require(panel.pointerHovered(), "the panel must expose its authoritative hover state");
    panel.setEnabled(false);
    require(!panel.pointerHovered(), "QWidget disabling must synchronously end panel hover");
    sendEnter(&panel);
    require(!panel.pointerHovered(), "disabled panels must reject stale enter events");
}

void valueLabelPaintsFromQtHoverState() {
    SelectionToolbarValueLabel label;
    label.setText(QStringLiteral("640"));
    label.setFixedSize(label.sizeHint());
    require(label.testAttribute(Qt::WA_Hover) && !label.hasMouseTracking(),
            "value labels should use Qt hover state without mouse move tracking");

    const QImage idleImage = renderWidget(&label);
    sendEnter(&label);
    const QImage hoveredImage = renderWidget(&label);
    label.setPointerInteractionEnabled(false);
    sendEnter(&label);
    const QImage transparentImage = renderWidget(&label);
    label.setPointerInteractionEnabled(true);
    const QImage restoredImage = renderWidget(&label);

    require(hoveredImage != idleImage,
            "value-label enter events should enable the hover visual without cursor polling");
    require(transparentImage == idleImage && restoredImage == idleImage,
            "disabled value labels should clear hover and ignore stale enter events");

    sendEnter(&label);
    sendHide(&label);
    require(renderWidget(&label) == idleImage,
            "hiding a value label should clear its hover visual");

    sendEnter(&label);
    sendLeave(&label);
    require(renderWidget(&label) == idleImage,
            "value-label leave events should restore the idle visual");

    sendEnter(&label);
    label.setEnabled(false);
    SelectionToolbarValueLabel disabledReference;
    disabledReference.setText(label.text());
    disabledReference.setFixedSize(label.size());
    disabledReference.setEnabled(false);
    require(renderWidget(&label) == renderWidget(&disabledReference),
            "disabling a hovered value label must clear its hover visual");
    label.setEnabled(true);
    require(renderWidget(&label) == idleImage,
            "re-enabling a value label must wait for a fresh hover event");
}

void valueLabelCentersIconsAtEveryRenderScale() {
    const QColor iconColor(255, 0, 255);
    const int contentHeight = screenshot_selection_toolbar::PanelHeight -
                              screenshot_selection_toolbar::PanelVerticalPadding * 2;
    for (const qreal dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        QPixmap icon(QSize(qRound(screenshot_selection_toolbar::IconSize * dpr),
                           qRound(screenshot_selection_toolbar::IconSize * dpr)));
        icon.setDevicePixelRatio(dpr);
        icon.fill(iconColor);
        for (const bool iconOnly : {false, true}) {
            SelectionToolbarValueLabel label;
            label.setAttribute(Qt::WA_TranslucentBackground);
            label.setText(QStringLiteral("123"));
            if (iconOnly) {
                label.setIconOnlyPixmap(icon);
            } else {
                label.setLeadingIcon(icon);
            }
            for (const int height : {contentHeight, contentHeight + 1}) {
                label.setFixedSize(label.sizeHint().width(), height);
                QImage image(QSize(qRound(label.width() * dpr), qRound(height * dpr)),
                             QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(dpr);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                label.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
                painter.end();
                QRect iconBounds;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor pixel = image.pixelColor(x, y);
                        if (pixel.alpha() > 0 && pixel.red() == 255 && pixel.green() == 0 &&
                            pixel.blue() == 255) {
                            iconBounds = iconBounds.united(QRect(x, y, 1, 1));
                        }
                    }
                }
                require(!iconBounds.isEmpty(), "toolbar labels must paint the supplied icon");
                const qreal iconCenter = (iconBounds.top() + iconBounds.bottom() + 1) / 2.0;
                const qreal labelCenter = height * dpr / 2.0;
                if (std::abs(iconCenter - labelCenter) > 0.5) {
                    std::cerr << "iconOnly=" << iconOnly << ", height=" << height << ", dpr=" << dpr
                              << ", iconCenter=" << iconCenter << ", labelCenter=" << labelCenter
                              << '\n';
                }
                require(std::abs(iconCenter - labelCenter) <= 0.5,
                        "toolbar icons must be vertically centered within half a physical pixel");
            }
        }
    }
}

void selectionToolbarInputSurfaceMatchesInteractivePanel() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(640, 360);

    QWidget canvas(&host);
    canvas.setGeometry(host.rect());
    canvas.setCursor(Qt::CrossCursor);

    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(80, 120, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::Full);
    toolbar.move(120, 60);
    host.show();
    toolbar.show();
    toolbar.raise();
    QCoreApplication::processEvents();

    QWidget* panel = toolbar.findChild<QWidget*>(QStringLiteral("screenshotSelectionToolbarPanel"));
    require(panel != nullptr, "selection toolbar panel should be findable");
    const QRect panelRect(panel->mapTo(&host, QPoint(0, 0)), panel->size());

    QWidget* panelHit = host.childAt(panelRect.center());
    require(panelHit != nullptr && toolbar.isAncestorOf(panelHit) && panelHit != &toolbar,
            "points over the panel should hit the interactive toolbar content");
    require(commands.interactionCount == 0,
            "hit testing alone must not trigger selection toolbar commands");

    const QPoint belowPanel(panelRect.center().x(), toolbar.y() + toolbar.height() - 2);
    const QPoint leftOfPanel(toolbar.x() + 2, panelRect.center().y());
    const QPoint abovePanel(panelRect.center().x(), toolbar.y() + 2);
    const QPoint cornerMargin(toolbar.x() + 2, toolbar.y() + toolbar.height() - 2);
    for (const QPoint& marginPoint : {belowPanel, leftOfPanel, abovePanel, cornerMargin}) {
        QWidget* hit = host.childAt(marginPoint);
        require(hit == &canvas,
                "idle toolbar margin points must fall through to the underlying canvas");
        require(hit->cursor().shape() == Qt::CrossCursor,
                "toolbar margin hit testing must preserve the canvas cursor");
    }

    sendEnter(panel);
    QWidget* glowHit = host.childAt(QPoint(panelRect.center().x(), panelRect.bottom() + 2));
    require(glowHit == &toolbar,
            "hovering should route the visible glow halo through the toolbar surface");
    QWidget* beyondGlowHit = host.childAt(QPoint(panelRect.center().x(), panelRect.bottom() + 7));
    require(beyondGlowHit == &canvas,
            "margin pixels beyond the glow outset must keep falling through while hovered");
    sendLeave(panel);
    require(host.childAt(QPoint(panelRect.center().x(), panelRect.bottom() + 2)) == &canvas,
            "leaving the toolbar must shrink the input surface back to the panel");
}

void selectionDragCannotActivateToolbarPreview() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(1000, 400);
    QWidget canvas(&host);
    canvas.setGeometry(host.rect());
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(40, 0, 100, 20), false, 0, 0);
    toolbar.moveContentTo(QPoint(144, 0));
    host.show();
    toolbar.show();
    QCoreApplication::processEvents();
    auto* panel = toolbar.findChild<SelectionToolbarPanel*>();
    require(panel != nullptr, "selection toolbar panel must exist");
    sendEnter(panel);
    require(commands.toolbarHovered, "idle toolbar hover must activate the result preview");
    toolbar.setPointerInteractionEnabled(false);
    require(!commands.toolbarHovered && !panel->pointerHovered(),
            "starting a selection drag must clear the border-hiding toolbar preview");
    const int before = commands.interactionCount;
    sendEnter(panel);
    toolbar.setSelectionState(QRect(40, 0, 200, 60), false, 0, 0);
    toolbar.moveContentTo(QPoint(244, 0));
    QCoreApplication::processEvents();
    require(!commands.toolbarHovered && commands.interactionCount == before,
            "a moving toolbar must not activate hover preview during a selection drag");
    require(host.childAt(panel->mapTo(&host, panel->rect().center())) == &canvas,
            "toolbar content must pass through pointer input while drawing a top-edge selection");
    toolbar.setPointerInteractionEnabled(true);
    sendEnter(panel);
    require(commands.toolbarHovered, "ending a selection drag must restore toolbar hover");
    toolbar.setPointerInteractionEnabled(false);
    toolbar.hide();
    toolbar.resetForNewCapture();
    require(!toolbar.testAttribute(Qt::WA_TransparentForMouseEvents),
            "capture reset must restore the toolbar interaction policy");
}

void smartSelectionToolbarIsClickThroughAcrossCaptureLifecycles() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(640, 360);

    QWidget canvas(&host);
    canvas.setGeometry(host.rect());
    canvas.setCursor(Qt::CrossCursor);

    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(80, 70, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::Full);
    toolbar.move(120, 120);
    host.show();
    toolbar.show();
    toolbar.raise();
    QCoreApplication::processEvents();

    require(!toolbar.testAttribute(Qt::WA_TransparentForMouseEvents),
            "full selection toolbar should remain interactive");

    toolbar.setSelectionState(QRect(80, 70, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly);
    QCoreApplication::processEvents();

    require(toolbar.testAttribute(Qt::WA_TransparentForMouseEvents),
            "smart-selection toolbar root must be transparent for mouse events");
    for (QWidget* child : toolbar.findChildren<QWidget*>()) {
        require(child->testAttribute(Qt::WA_TransparentForMouseEvents),
                "smart-selection toolbar descendants must be transparent for mouse events");
    }

    const QPoint toolbarCenter = toolbar.pos() + QPoint(toolbar.width() / 2, toolbar.height() / 2);
    QWidget* hitWidget = host.childAt(toolbarCenter);
    require(hitWidget == &canvas,
            "smart-selection toolbar must leave the underlying canvas as the hit target");
    require(hitWidget->cursor().shape() == Qt::CrossCursor,
            "smart-selection hit testing must preserve the canvas crosshair cursor");
    require(commands.interactionCount == 0,
            "smart-selection toolbar must not trigger commands while click-through");

    toolbar.hide();
    toolbar.resetForNewCapture();
    require(!toolbar.testAttribute(Qt::WA_TransparentForMouseEvents),
            "resetting a pooled toolbar must restore its canonical interactive state");

    toolbar.setSelectionState(QRect(80, 70, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly);
    toolbar.show();
    toolbar.raise();
    QCoreApplication::processEvents();
    require(toolbar.testAttribute(Qt::WA_TransparentForMouseEvents),
            "a subsequent smart-selection capture must reapply click-through state");
    require(host.childAt(toolbarCenter) == &canvas,
            "a subsequent smart-selection capture must not retain toolbar hit testing");
    require(commands.interactionCount == 0,
            "a subsequent smart-selection capture must not trigger stale toolbar commands");
}

void smartSelectionToolbarShedsNativeWindowForcedByNativeSiblingEmbed() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(640, 360);

    QWidget canvas(&host);
    canvas.setGeometry(host.rect());
    canvas.setCursor(Qt::CrossCursor);

    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(80, 70, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::Full);
    toolbar.move(120, 120);
    host.show();
    toolbar.show();
    toolbar.raise();
    QCoreApplication::processEvents();

    // Replicate ScreenshotFloatingToolPaletteWindow::setOwnerWindow(): a native,
    // window-type child is reparented into the overlay. Qt's native-sibling rule
    // (enforceNativeChildren) then force-nativizes every alien sibling of the
    // overlay, including the selection toolbar.
    QWidget palette(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    static_cast<void>(palette.winId());
    palette.setParent(&host, Qt::Tool | Qt::FramelessWindowHint);
    palette.move(10, 300);
    palette.resize(120, 32);
    palette.show();
    QCoreApplication::processEvents();

    require(toolbar.testAttribute(Qt::WA_NativeWindow) || toolbar.internalWinId() != 0,
            "embedding a native window-type sibling should nativize the selection toolbar "
            "(the causal chain this test guards against)");

    // Even after that external nativization, entering the smart-selection
    // click-through phase must release the native window: a native child HWND
    // intercepts OS-level hit testing, which WA_TransparentForMouseEvents cannot
    // redirect, leaving an arrow cursor and freezing the overlay color picker.
    toolbar.setSelectionState(QRect(80, 70, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly);
    QCoreApplication::processEvents();

    require(toolbar.testAttribute(Qt::WA_TransparentForMouseEvents),
            "smart-selection toolbar must apply click-through after a native embed");
    require(!toolbar.testAttribute(Qt::WA_NativeWindow),
            "smart-selection toolbar must not stay flagged native while click-through");
    require(toolbar.internalWinId() == 0,
            "smart-selection toolbar must release its native window handle so OS hit "
            "testing falls through to the overlay canvas");
    require(toolbar.isVisible(), "shedding the native surface must keep the toolbar visible");

    const QPoint toolbarCenter = toolbar.pos() + QPoint(toolbar.width() / 2, toolbar.height() / 2);
    require(host.childAt(toolbarCenter) == &canvas,
            "post-embed smart-selection toolbar must leave the canvas as the hit target");
    require(commands.interactionCount == 0,
            "post-embed smart-selection toolbar must not trigger commands");

    // The pooled widget must keep shedding the native surface on later cycles:
    // the palette remains embedded and re-asserts nativization on every attach.
    toolbar.hide();
    toolbar.resetForNewCapture();
    toolbar.setParent(nullptr);
    palette.setParent(nullptr);
    QCoreApplication::processEvents();

    toolbar.setParent(&host, Qt::Widget);
    toolbar.move(120, 120);
    QWidget paletteAgain(&host, Qt::Tool | Qt::FramelessWindowHint);
    static_cast<void>(paletteAgain.winId());
    paletteAgain.show();
    QCoreApplication::processEvents();
    require(toolbar.testAttribute(Qt::WA_NativeWindow) || toolbar.internalWinId() != 0,
            "a pooled re-attach under a native sibling should nativize the toolbar again");

    toolbar.setSelectionState(QRect(80, 70, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly);
    toolbar.show();
    toolbar.raise();
    QCoreApplication::processEvents();
    require(toolbar.internalWinId() == 0,
            "a subsequent capture must shed the native surface again before smart selection");
    require(host.childAt(toolbarCenter) == &canvas,
            "a subsequent capture must keep the canvas as the hit target after shedding");
    require(toolbar.isVisible(),
            "shedding the native surface on a later cycle must keep the toolbar visible");
}

void selectionToolbarLabelsFollowApplicationFontFamily() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(640, 360);

    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(80, 120, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::Full);
    QCoreApplication::processEvents();

    const QList<QLabel*> labels = toolbar.findChildren<QLabel*>();
    require(!labels.isEmpty(), "selection toolbar should expose its labels for font checks");
    for (const QLabel* label : labels) {
        require(label->font().family() == QApplication::font().family(),
                "selection toolbar labels must follow the application font family");
    }
}

void selectionToolbarUsesCanvasUnitsForEditingAndSmartSelection() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    using Mode = ScreenshotSelectionToolbarWidget::DisplayMode;
    const QRect selection(80, 70, 317, 181);
    toolbar.setSelectionState(selection, false, 10, 5, Mode::SizeOnly, true);

    const auto labels = toolbar.findChildren<QLabel*>();
    const auto field = [&](const char* name) -> QLabel* {
        for (QLabel* label : labels) {
            if (label->accessibleName() == QString::fromLatin1(name)) {
                return label;
            }
        }
        require(false, "selection toolbar field missing");
        return nullptr;
    };
    QLabel* width = field("Width");
    QLabel* height = field("Height");
    const auto checkUnits = [&](int logicalPixels, int pixels) {
        int logicalPixelLabels = 0;
        int pixelLabels = 0;
        for (QLabel* label : labels) {
            if (label->text() == QStringLiteral("dp")) {
                ++logicalPixelLabels;
                require(label->toolTip() == QStringLiteral("Logical pixels") &&
                            label->accessibleName() == QStringLiteral("Logical pixels"),
                        "logical pixel units need descriptive accessibility text");
            } else if (label->text() == QStringLiteral("px")) {
                ++pixelLabels;
                require(label->toolTip() == QStringLiteral("Pixels") &&
                            label->accessibleName() == QStringLiteral("Pixels"),
                        "pixel units need descriptive accessibility text");
            }
        }
        require(logicalPixelLabels == logicalPixels && pixelLabels == pixels,
                "incorrect toolbar unit system");
    };
    require(width->text() == QStringLiteral("317") && height->text() == QStringLiteral("181"),
            "initial smart selection must show canvas dimensions");
    checkUnits(4, 0);
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, true);
    require(width->text() == QStringLiteral("317") && height->text() == QStringLiteral("181") &&
                field("X coordinate")->text() == QStringLiteral("80") &&
                field("Corner radius")->text() == QStringLiteral("10") &&
                field("Shadow width")->text() == QStringLiteral("5"),
            "editable values must match the canvas and resize dialog units");
    checkUnits(4, 0);

    const QPointF local(width->rect().center());
    QWheelEvent wheel(local, width->mapToGlobal(local.toPoint()), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(width, &wheel);
    require(commands.lastAdjustment == std::vector<int>({0, 0, 1, 0}),
            "width wheel must request one displayed canvas unit");
    toolbar.setSelectionState(QRect(80, 70, 318, 181), false, 10, 5, Mode::Full, true);
    require(width->text() == QStringLiteral("318"),
            "one canvas-unit edit must advance the editable readout by one");

    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, true);
    toolbar.setSelectionState(selection, false, 10, 5, Mode::SizeOnly, true);
    require(width->text() == QStringLiteral("317") && height->text() == QStringLiteral("181"),
            "smart selection must show the same dimensions as manual selection");
    checkUnits(4, 0);
    commands.lastAdjustment.clear();
    QApplication::sendEvent(width, &wheel);
    require(commands.lastAdjustment.empty(), "smart selection must not dispatch canvas edits");
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, true);
    require(width->text() == QStringLiteral("317"), "editing must restore canvas dimensions");
    checkUnits(4, 0);

    PixelUnitTranslator translator;
    require(QApplication::installTranslator(&translator), "pixel-unit translator unavailable");
    QCoreApplication::processEvents();
    for (QLabel* label : labels) {
        if (label->text() == QStringLiteral("dp")) {
            require(label->toolTip() == QStringLiteral("translated-unit") &&
                        label->accessibleName() == QStringLiteral("translated-unit"),
                    "unit descriptions must follow language changes");
        }
    }
    toolbar.setSelectionState(selection, false, 10, 5, Mode::SizeOnly, true);
    for (QLabel* label : labels) {
        if (label->text() == QStringLiteral("dp")) {
            require(label->accessibleName() == QStringLiteral("translated-unit"),
                    "smart-selection unit descriptions must follow language changes");
        }
    }
    QApplication::removeTranslator(&translator);
    QCoreApplication::processEvents();

    // Converted labels must never change geometry-edit commands or effect units.
    const ScreenshotSelectionDisplayValues logicalValues{
        QPointF(64, 56), QSizeF(253.6, 144.8), ScreenshotSelectionDisplayUnit::LogicalPixels,
        false};
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, false, logicalValues);
    require(width->text() == QStringLiteral("254") && height->text() == QStringLiteral("145") &&
                field("X coordinate")->text() == QStringLiteral("64") &&
                field("Corner radius")->text() == QStringLiteral("10") &&
                field("Shadow width")->text() == QStringLiteral("5"),
            "unit toggles must update position and size while retaining effect values");
    const QSize stableSize = toolbar.contentSizeHint();
    auto fractionalChange = logicalValues;
    fractionalChange.position = QPointF(64.2, 56.3);
    fractionalChange.size = QSizeF(254.2, 145.1);
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, false, fractionalChange);
    require(width->text() == QStringLiteral("254") && height->text() == QStringLiteral("145") &&
                field("X coordinate")->text() == QStringLiteral("64") &&
                toolbar.contentSizeHint() == stableSize,
            "fractional changes within the same rounded pixel must keep labels and layout stable");
    checkUnits(2, 2);
    int dpLabels = 0;
    for (auto* label : labels)
        dpLabels += label->text() == QStringLiteral("dp") ? 1 : 0;
    require(dpLabels == 2, "logical Windows mode must label only coordinates and dimensions as dp");
    commands.lastAdjustment.clear();
    QApplication::sendEvent(width, &wheel);
    require(commands.lastAdjustment == std::vector<int>({0, 0, 1, 0}),
            "logical unit display must retain one native geometry unit per wheel step");
    const ScreenshotSelectionDisplayValues physicalValues{
        QPointF(160, 140), QSizeF(634, 362), ScreenshotSelectionDisplayUnit::PhysicalPixels, true};
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, true, physicalValues);
    require(width->text() == QStringLiteral("634") &&
                field("Corner radius")->text() == QStringLiteral("10"),
            "physical macOS readout must not scale editable effect values");
    checkUnits(2, 2);

    // A point-backed 1x display still uses points, even when values coincide.
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full, true);
    checkUnits(4, 0);
    toolbar.setSelectionState(selection, false, 10, 5, Mode::SizeOnly, false);
    checkUnits(0, 4);
    toolbar.setSelectionState(selection, false, 10, 5, Mode::Full);
    require(width->text() == QStringLiteral("317"), "pixel canvases must retain their dimensions");
    checkUnits(0, 4);
    toolbar.resetForNewCapture();
    checkUnits(0, 4);
}

void smartSelectionDefersHiddenFieldsUntilEditing() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    using Mode = ScreenshotSelectionToolbarWidget::DisplayMode;
    using Preset = ScreenshotSelectionAspectRatioPreset;
    const QRect initial(80, 70, 317, 181);
    toolbar.setSelectionState(initial, false, 4, 2);
    const auto field = [&](const char* name) -> QLabel* {
        for (QLabel* label : toolbar.findChildren<QLabel*>()) {
            if (label->accessibleName() == QString::fromLatin1(name)) {
                return label;
            }
        }
        require(false, "selection toolbar field missing");
        return nullptr;
    };
    const auto* x = field("X coordinate");
    const auto* y = field("Y coordinate");
    const auto* radius = field("Corner radius");
    const auto* shadow = field("Shadow width");
    toolbar.setSelectionState(initial, false, 4, 2, Mode::SizeOnly);
    const QSize sizeOnlySize = toolbar.contentSizeHint();
    const auto* select = toolbar.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotSelectionAspectRatioSelect"));
    require(select != nullptr, "selection toolbar must expose its ratio selector");
    for (int index = 0; index < 100; ++index) {
        toolbar.setSelectionState(QRect(10000 + index, -10000 - index, 317, 181), true, 12, 8,
                                  Mode::SizeOnly, false, std::nullopt, Preset::Square);
        require(toolbar.contentSizeHint() == sizeOnlySize,
                "smart-selection positions and hidden effects must not change the size readout");
    }
    require(x->text() == QStringLiteral("80") && y->text() == QStringLiteral("70") &&
                radius->text() == QStringLiteral("4") && shadow->text() == QStringLiteral("2") &&
                select->currentValue() == QStringLiteral("free"),
            "smart-selection frames must defer hidden field and ratio presentation");
    const QRect latest(10099, -10099, 1024, 576);
    toolbar.setSelectionState(latest, true, 12, 8, Mode::SizeOnly, false, std::nullopt,
                              Preset::Square);
    require(field("Width")->text() == QStringLiteral("1024") &&
                field("Height")->text() == QStringLiteral("576"),
            "smart-selection dimensions must continue to update immediately");
    toolbar.setSelectionState(latest, true, 12, 8, Mode::Full, false, std::nullopt, Preset::Square);
    require(x->text() == QStringLiteral("10099") && y->text() == QStringLiteral("-10099") &&
                radius->text() == QStringLiteral("12") && shadow->text() == QStringLiteral("8") &&
                select->currentValue() == QStringLiteral("1:1"),
            "entering editing must present the latest deferred values and ratio");
    require(commands.selectedPresets.empty(),
            "revealing a deferred ratio must not dispatch a selection edit");
}

adqt::widgets::AdSelect* ratioSelect(ScreenshotSelectionToolbarWidget& toolbar) {
    auto* select = toolbar.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotSelectionAspectRatioSelect"));
    require(select != nullptr, "selection toolbar must expose the aspect-ratio Select");
    return select;
}

void aspectRatioSelectSynchronizesWithoutDispatchingCommands() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    host.show();
    toolbar.show();
    QCoreApplication::processEvents();
    auto* select = ratioSelect(toolbar);
    using Preset = ScreenshotSelectionAspectRatioPreset;
    const QStringList expectedLabels = {QStringLiteral("Free"), QStringLiteral("1:1"),
                                        QStringLiteral("3:2"),  QStringLiteral("4:3"),
                                        QStringLiteral("16:9"), QStringLiteral("2:3"),
                                        QStringLiteral("3:4"),  QStringLiteral("9:16")};
    const auto options = select->options();
    require(options.size() == expectedLabels.size(), "aspect-ratio options must be complete");
    for (qsizetype index = 0; index < options.size(); ++index) {
        require(options[index].label == expectedLabels[index], "aspect-ratio option order changed");
    }
    require(select->currentValue() == QStringLiteral("free") && select->currentText() == "Free",
            "new captures must default to Free");
    require(!select->searchEnabled() && !select->allowClear() &&
                select->mode() == adqt::widgets::AdSelect::Mode::Single &&
                select->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool,
            "aspect-ratio control must use a compact single-selection popup");
    require(select->accessibleName() == "Selection aspect ratio" &&
                select->toolTip() == select->accessibleName(),
            "aspect-ratio Select must identify its purpose to accessibility and tooltips");

    const QRect selection(80, 70, 320, 180);
    toolbar.setSelectionState(selection, false, 4, 2);
    require(select->currentValue() == QStringLiteral("free"),
            "a naturally matching selection must retain Free until the user chooses a preset");
    int styleUpdates = 0;
    int optionUpdates = 0;
    QObject::connect(select, &adqt::widgets::AdSelect::semanticStylesChanged, &toolbar,
                     [&styleUpdates]() { ++styleUpdates; });
    QObject::connect(select, &adqt::widgets::AdSelect::optionsChanged, &toolbar,
                     [&optionUpdates]() { ++optionUpdates; });
    for (int index = 0; index < 100; ++index) {
        toolbar.setSelectionResizable(true);
        toolbar.setPointerInteractionEnabled(true);
        toolbar.setSelectionState(QRect(80, 70, 320 + index % 2, 180), false, 4, 2);
    }
    toolbar.setSelectionState(selection, false, 4, 2);
    require(styleUpdates == 0 && optionUpdates == 0,
            "selection frames must not rebuild ratio options or reapply identical Select styles");
    const QSize initialToolbarSize = toolbar.contentSizeHint();
    const int initialSelectWidth = select->width();
    const int initialTextWidth = select->lineEdit()->fontMetrics().horizontalAdvance("Free");
    requireUnclippedRatioText(select->lineEdit());
    QLabel* widthLabel = nullptr;
    QLabel* sizeUnitLabel = nullptr;
    QLabel* lockLabel = nullptr;
    for (QLabel* label : toolbar.findChildren<QLabel*>()) {
        if (label->accessibleName() == "Width") {
            widthLabel = label;
        }
        if (label->accessibleName() == "Lock selection aspect ratio") {
            lockLabel = label;
        }
        if (label->text() == "px" && label->geometry().right() < select->geometry().left() &&
            (sizeUnitLabel == nullptr ||
             label->geometry().right() > sizeUnitLabel->geometry().right())) {
            sizeUnitLabel = label;
        }
    }
    require(widthLabel != nullptr && sizeUnitLabel != nullptr && lockLabel != nullptr,
            "dimension and lock controls must remain present");
    require(select->geometry().left() > sizeUnitLabel->geometry().right() &&
                select->height() == screenshot_selection_toolbar::PanelHeight - 4,
            "aspect-ratio Select must follow the dimensions and fit the panel");
    const QImage unlockedIcon = renderWidget(lockLabel);
    for (qsizetype index = 1; index < options.size(); ++index) {
        const Preset preset =
            screenshotSelectionAspectRatioPresetFromId(options[index].value.toString());
        toolbar.setSelectionState(selection, true, 4, 2,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt, preset);
        const int textWidth =
            select->lineEdit()->fontMetrics().horizontalAdvance(options[index].label);
        const int widthDelta = textWidth - initialTextWidth;
        require(select->currentValue() == options[index].value &&
                    select->width() == initialSelectWidth + widthDelta &&
                    toolbar.contentSizeHint() == initialToolbarSize + QSize(widthDelta, 0),
                "preset synchronization must fit the selected text and resize the toolbar");
        require(select->lineEdit()->width() >= textWidth + 4,
                "compact ratio labels must fit without clipping the text or cursor inset");
        requireUnclippedRatioText(select->lineEdit());
    }
    require(renderWidget(lockLabel) != unlockedIcon,
            "a locked preset must highlight the existing lock icon");
    require(commands.selectedPresets.empty(),
            "state synchronization must not dispatch preset commands");
    toolbar.resetForNewCapture();
    require(select->currentValue() == QStringLiteral("free") &&
                select->width() == initialSelectWidth && renderWidget(lockLabel) == unlockedIcon,
            "capture reset must restore Free and the unhighlighted lock");

    AspectRatioTranslator translator;
    require(QApplication::installTranslator(&translator), "aspect-ratio translator unavailable");
    QCoreApplication::processEvents();
    require(select->currentText() == "Unconstrained selection" &&
                select->accessibleName() == "Translated aspect ratio" &&
                select->width() > initialSelectWidth,
            "language changes must retranslate Free, accessibility, and measured trigger width");
    require(select->options()[4].label == "16:9" && select->options()[0].value == "free",
            "language changes must preserve ratio labels and stable option identifiers");
    toolbar.setSelectionState(selection, true, 4, 2,
                              ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                              std::nullopt, Preset::Square);
    require(select->width() == initialSelectWidth +
                                   select->lineEdit()->fontMetrics().horizontalAdvance("1:1") -
                                   initialTextWidth,
            "a short ratio must shrink even when another translated option is much longer");
    toolbar.setSelectionState(selection, false, 4, 2);
    QApplication::removeTranslator(&translator);
    QCoreApplication::processEvents();
    require(select->width() == initialSelectWidth && select->currentText() == "Free",
            "restoring language must restore the compact trigger width");
}

void disabledAspectRatioSelectPreservesItsTextColor() {
    auto& theme = adqt::theme::ThemeManager::instance();
    const auto originalConfig = theme.config();
    for (const auto scheme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        theme.setColorScheme(scheme);
        NoOpSelectionToolbarCommands commands;
        ScreenshotSelectionToolbarWidget toolbar(commands);
        toolbar.setSelectionState(QRect(80, 70, 320, 180), true, 4, 2,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt,
                                  ScreenshotSelectionAspectRatioPreset::Landscape16x9);
        toolbar.show();
        QCoreApplication::processEvents();
        auto* select = ratioSelect(toolbar);
        auto* input = select->lineEdit();
        input->clearFocus();
        input->deselect();
        const QColor textColor = input->palette().color(QPalette::Active, QPalette::Text);
        const QImage textImage = renderWidget(input, QWidget::DrawChildren);
        const QSize toolbarSize = toolbar.contentSizeHint();
        for (const bool disablePointer : {true, false}) {
            if (disablePointer) {
                toolbar.setPointerInteractionEnabled(false);
            } else {
                toolbar.setSelectionResizable(false);
            }
            require(select->disabled() && !input->isEnabled(),
                    "selection modification must disable ratio input");
            require(input->palette().color(QPalette::Disabled, QPalette::Text) == textColor,
                    "temporarily disabled ratio text must keep its normal palette color");
            const QImage disabledTextImage = renderWidget(input, QWidget::DrawChildren);
            require(disabledTextImage == textImage,
                    "temporarily disabled ratio text must keep its rendered appearance");
            require(toolbar.contentSizeHint() == toolbarSize,
                    "disabling the ratio selector must preserve its compact geometry");
            toolbar.setPointerInteractionEnabled(true);
            toolbar.setSelectionResizable(true);
            require(!select->disabled() && renderWidget(input, QWidget::DrawChildren) == textImage,
                    "reenabling ratio input must preserve its text appearance");
        }
    }
    theme.setConfig(originalConfig);
}

void aspectRatioPopupOwnsInputAndEndsWithToolbarLifecycle() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(800, 400);
    host.setFocusPolicy(Qt::StrongFocus);
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(50, 50, 320, 180), false, 0, 0);
    toolbar.move(100, 40);
    host.show();
    toolbar.show();
    host.activateWindow();
    host.setFocus();
    QCoreApplication::processEvents();
    auto* select = ratioSelect(toolbar);
    auto* panel = toolbar.findChild<SelectionToolbarPanel*>();
    require(panel != nullptr, "aspect-ratio test needs the toolbar panel");

    const QPointF position(select->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress, position, position,
                      select->mapToGlobal(position.toPoint()), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(select, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, position,
                        select->mapToGlobal(position.toPoint()), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(select, &release);
    QCoreApplication::processEvents();
    if (!select->popupVisible() || commands.resizeModalCount != 0 ||
        commands.lockToggleCount != 0) {
        std::cerr << "ratio popup visible=" << select->popupVisible()
                  << " resize modal=" << commands.resizeModalCount
                  << " lock toggles=" << commands.lockToggleCount << '\n';
    }
    require(select->popupVisible() && commands.resizeModalCount == 0 &&
                commands.lockToggleCount == 0,
            "clicking Select must open its popup without dispatching other toolbar actions");
    require(select->lineEdit()->palette().color(QPalette::Text) ==
                screenshot_selection_toolbar::panelTextColor(),
            "an open aspect-ratio trigger must retain white text against its blue highlight");
    sendLeave(panel);
    require(commands.toolbarHovered,
            "the dropdown must preserve the toolbar preview while its menu owns the pointer");
    auto* view = select->view();
    require(view != nullptr && view->window()->isVisible(), "ratio popup list must be visible");
    require(toolbar.containsInteractiveGlobalPoint(view->mapToGlobal(view->rect().center())),
            "ratio popup must count as screenshot UI for magnifier suppression");
    require(!toolbar.containsInteractiveGlobalPoint(host.mapToGlobal(QPoint(799, 399))),
            "popup hit testing must not capture unrelated canvas points");

    view->setCurrentIndex(view->model()->index(4, 0));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(view, &enter);
    QCoreApplication::processEvents();
    require(commands.selectedPresets ==
                    std::vector<ScreenshotSelectionAspectRatioPreset>{
                        ScreenshotSelectionAspectRatioPreset::Landscape16x9} &&
                !select->popupVisible(),
            "Enter must apply one selected preset and dismiss the dropdown");
    require(QApplication::focusWidget() == &host,
            "closing a ratio popup must restore the previous screenshot keyboard owner");
    require(select->lineEdit()->palette().color(QPalette::Text) ==
                screenshot_selection_toolbar::panelTextColor(),
            "the closed aspect-ratio trigger must retain the same white text");

    select->showPopup();
    QCoreApplication::processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(select->view(), &escape);
    require(!select->popupVisible() && commands.selectedPresets.size() == 1,
            "Escape must dismiss the dropdown without changing its selection");
    select->showPopup();
    toolbar.setPointerInteractionEnabled(false);
    require(!select->popupVisible() && select->disabled() && !commands.toolbarHovered,
            "selection drags must dismiss and disable the dropdown synchronously");
    toolbar.setPointerInteractionEnabled(true);
    select->showPopup();
    toolbar.setSelectionResizable(false);
    require(!select->popupVisible() && select->disabled(),
            "a nonresizable selection must not retain an interactive ratio popup");
    toolbar.setSelectionResizable(true);
    select->showPopup();
    toolbar.setSelectionState(QRect(50, 50, 320, 180), false, 0, 0,
                              ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly);
    require(!select->popupVisible() && select->isHidden() && select->disabled(),
            "SizeOnly must hide the ratio control and dismiss its popup");
    toolbar.setSelectionState(QRect(50, 50, 320, 180), false, 0, 0);
    select->showPopup();
    toolbar.hide();
    require(!select->popupVisible() && !commands.toolbarHovered,
            "hiding the pooled toolbar must end dropdown hover and visibility");
    toolbar.show();
    select->showPopup();
    toolbar.resetForNewCapture();
    require(!select->popupVisible() && select->currentValue() == "free",
            "capture reset must dismiss any old ratio popup");
}

void aspectRatioPopupSuspendsScreenshotShortcutsThroughDismissal() {
    using snow_shot::presentation::WindowShortcutManager;
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(800, 400);
    host.setFocusPolicy(Qt::StrongFocus);
    WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&host);
    WindowShortcutManager::InputSuspensionHandle suspension = 0;
    commands.popupVisibilityObserver = [&shortcutManager, &suspension](bool visible) {
        if (visible) {
            require(suspension == 0, "popup visibility must not double-suspend screenshot input");
            suspension = shortcutManager.suspendInput();
        } else {
            const auto previousSuspension = suspension;
            suspension = 0;
            QTimer::singleShot(0, &shortcutManager, [&shortcutManager, previousSuspension]() {
                shortcutManager.resumeInput(previousSuspension);
            });
        }
    };
    int cancellationCount = 0;
    WindowShortcutManager::Binding cancel;
    cancel.id = QStringLiteral("cancel_screenshot");
    cancel.keyCombinations = {QKeyCombination(Qt::NoModifier, Qt::Key_Escape)};
    cancel.priority = WindowShortcutManager::StandardPriority::WindowCommand;
    cancel.activationTrigger = WindowShortcutManager::Binding::ActivationTrigger::Release;
    cancel.activate = [&cancellationCount](const auto&) {
        ++cancellationCount;
        return true;
    };
    const auto cancelHandle = shortcutManager.addBinding(&host, std::move(cancel));
    require(cancelHandle != 0, "screenshot cancellation shortcut must register");

    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(50, 50, 320, 180), false, 0, 0);
    toolbar.move(100, 40);
    host.show();
    toolbar.show();
    host.activateWindow();
    host.setFocus();
    QCoreApplication::processEvents();
    auto* select = ratioSelect(toolbar);
    select->showPopup();
    QCoreApplication::processEvents();
    require(suspension != 0, "opening the ratio menu must suspend screenshot shortcuts");
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(select->view(), &press);
    require(!select->popupVisible() && cancellationCount == 0,
            "the opening Escape press must dismiss only the dropdown");
    QCoreApplication::processEvents();
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&host, &release);
    require(cancellationCount == 0 && commands.popupVisibility == std::vector<bool>({true, false}),
            "the dropdown dismissal release must not cancel the screenshot");
    QKeyEvent nextPress(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&host, &nextPress);
    QKeyEvent nextRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&host, &nextRelease);
    require(cancellationCount == 1,
            "after dropdown dismissal the next screenshot shortcut must work normally");
}

void choosingFreeDispatchesExactlyOneExplicitCommand() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(800, 400);
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    const QRect selection(50, 50, 317, 181);
    toolbar.move(100, 40);
    host.show();
    toolbar.show();
    QCoreApplication::processEvents();
    auto* select = ratioSelect(toolbar);
    const auto activateFreeWithMouse = [select]() {
        select->showPopup();
        QCoreApplication::processEvents();
        auto* view = select->view();
        view->setCurrentIndex(view->model()->index(0, 0));
        const QPointF position(view->visualRect(view->currentIndex()).center());
        QMouseEvent press(QEvent::MouseButtonPress, position, position,
                          view->viewport()->mapToGlobal(position.toPoint()), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, position,
                            view->viewport()->mapToGlobal(position.toPoint()), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &release);
    };
    const auto activateFreeWithKeyboard = [select]() {
        select->showPopup();
        QCoreApplication::processEvents();
        auto* view = select->view();
        view->setCurrentIndex(view->model()->index(0, 0));
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(view, &enter);
    };
    for (const bool locked : {false, true}) {
        toolbar.setSelectionState(selection, locked, 0, 0);
        commands.selectedPresets.clear();
        activateFreeWithMouse();
        require(
            !select->popupVisible() && commands.selectedPresets ==
                                           std::vector<ScreenshotSelectionAspectRatioPreset>{
                                               ScreenshotSelectionAspectRatioPreset::Free},
            "clicking already-selected Free must dispatch one explicit command even when unlocked");
        activateFreeWithKeyboard();
        require(
            !select->popupVisible() && commands.selectedPresets.size() == 2 &&
                commands.selectedPresets.back() == ScreenshotSelectionAspectRatioPreset::Free,
            "Enter on already-selected Free must dispatch one explicit command even when unlocked");
    }
    commands.presetSelectionObserver = [&toolbar,
                                        selection](ScreenshotSelectionAspectRatioPreset preset) {
        toolbar.setSelectionState(selection, preset != ScreenshotSelectionAspectRatioPreset::Free,
                                  0, 0, ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt, preset);
    };
    for (const bool keyboard : {false, true}) {
        toolbar.setSelectionState(
            selection, true, 0, 0, ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
            std::nullopt, ScreenshotSelectionAspectRatioPreset::Landscape16x9);
        commands.selectedPresets.clear();
        if (keyboard) {
            activateFreeWithKeyboard();
        } else {
            activateFreeWithMouse();
        }
        require(
            !select->popupVisible() && commands.selectedPresets ==
                                           std::vector<ScreenshotSelectionAspectRatioPreset>{
                                               ScreenshotSelectionAspectRatioPreset::Free},
            "changing another preset to Free must not duplicate the explicit selection command");
        require(select->currentValue() == QStringLiteral("free"),
                "successful Free activation must retain the authoritative synchronized value");
    }
}

void deletingOpenRatioToolbarBalancesPopupVisibility() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(800, 400);
    auto* toolbar = new ScreenshotSelectionToolbarWidget(commands, &host);
    toolbar->setSelectionState(QRect(50, 50, 320, 180), false, 0, 0);
    host.show();
    toolbar->show();
    QCoreApplication::processEvents();
    auto* select = ratioSelect(*toolbar);
    select->showPopup();
    QCoreApplication::processEvents();
    const QPointer<QWidget> popup = select->view()->window();
    require(commands.popupVisibility == std::vector<bool>({true}),
            "opening the dropdown must begin one popup visibility session");
    delete toolbar;
    require(commands.popupVisibility == std::vector<bool>({true, false}) &&
                !commands.toolbarHovered,
            "deleting an open toolbar must balance popup visibility and clear preview hover");
    require(popup == nullptr || !popup->isVisible(),
            "deleting a toolbar must synchronously hide its top-level ratio popup");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void closingRatioPopupPreservesNewKeyboardFocus() {
    NoOpSelectionToolbarCommands commands;
    QWidget host;
    host.resize(800, 400);
    host.setFocusPolicy(Qt::StrongFocus);
    QLineEdit externalEditor(&host);
    externalEditor.setGeometry(20, 320, 160, 24);
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    toolbar.setSelectionState(QRect(50, 50, 320, 180), false, 0, 0);
    toolbar.move(100, 40);
    host.show();
    toolbar.show();
    host.activateWindow();
    host.setFocus();
    QCoreApplication::processEvents();
    auto* select = ratioSelect(toolbar);
    select->showPopup();
    QCoreApplication::processEvents();
    select->hidePopup();
    externalEditor.setFocus();
    QCoreApplication::processEvents();
    require(QApplication::focusWidget() == &externalEditor,
            "queued dropdown dismissal must preserve a newly focused external editor");

    // An in-window surface gives this fixture deterministic keyboard focus. Native tool windows
    // can remain nonactive, so their list's requested focus is not an established precondition.
    select->setPopupLayerMode(adqt::widgets::AdSelect::PopupLayerMode::InWindow);
    host.setFocus();
    select->showPopup();
    QCoreApplication::processEvents();
    require(QApplication::focusWidget() == select->view(),
            "reopening fixture must establish list focus before exercising queued restoration");
    select->hidePopup();
    select->showPopup();
    require(select->popupVisible() && QApplication::focusWidget() == select->view(),
            "the reopened dropdown must own keyboard focus before the old callback runs");
    QCoreApplication::processEvents();
    require(select->popupVisible() && QApplication::focusWidget() == select->view(),
            "an old dismissal callback must not steal keyboard focus from a reopened dropdown");
    select->hidePopup();
    QCoreApplication::processEvents();
}

void saveAspectRatioDiagnostics(const QString& directory) {
    require(QDir().mkpath(directory), "unable to create toolbar diagnostics directory");
    auto& theme = adqt::theme::ThemeManager::instance();
    const auto originalConfig = theme.config();
    const QRect screenBounds = QApplication::primaryScreen()->availableGeometry();
    for (const auto scheme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        theme.setColorScheme(scheme);
        const QString schemeName = scheme == adqt::theme::ThemeScheme::Light
                                       ? QStringLiteral("light")
                                       : QStringLiteral("dark");
        NoOpSelectionToolbarCommands commands;
        QWidget host;
        host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        host.resize(std::min(800, screenBounds.width()), std::min(360, screenBounds.height()));
        host.setAutoFillBackground(true);
        QPalette palette = host.palette();
        palette.setColor(QPalette::Window, scheme == adqt::theme::ThemeScheme::Light
                                               ? QColor(244, 246, 249)
                                               : QColor(24, 29, 39));
        host.setPalette(palette);
        ScreenshotSelectionToolbarWidget toolbar(commands, &host);
        toolbar.setSelectionState(QRect(127, 94, 320, 180), true, 12, 8,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt,
                                  ScreenshotSelectionAspectRatioPreset::Landscape16x9);
        auto* select = ratioSelect(toolbar);
        host.show();
        toolbar.show();
        const auto saveCapture = [&](QWidget* popup, const QString& state) {
            const QRect hostRect(host.mapToGlobal(QPoint()), host.size());
            const QRect popupRect =
                popup != nullptr ? QRect(popup->mapToGlobal(QPoint()), popup->size()) : QRect();
            const QRect captureRect = popup != nullptr ? hostRect.united(popupRect) : hostRect;
            const qreal dpr = host.devicePixelRatioF();
            QImage capture(
                QSize(qRound(captureRect.width() * dpr), qRound(captureRect.height() * dpr)),
                QImage::Format_ARGB32_Premultiplied);
            capture.setDevicePixelRatio(dpr);
            capture.fill(palette.color(QPalette::Window));
            QPainter painter(&capture);
            host.render(&painter, hostRect.topLeft() - captureRect.topLeft());
            if (popup != nullptr) {
                popup->render(&painter, popupRect.topLeft() - captureRect.topLeft());
            }
            painter.end();
            const QString filename = QStringLiteral("ratio-toolbar-%1-%2-%3.png")
                                         .arg(schemeName)
                                         .arg(qRound(dpr * 100))
                                         .arg(state);
            require(capture.save(QDir(directory).filePath(filename)),
                    "unable to save toolbar diagnostic");
        };
        host.move(screenBounds.center() - QPoint(host.width() / 2, host.height() / 2));
        toolbar.moveContentTo(QPoint(24, host.height() / 2));
        toolbar.setSelectionState(QRect(127, 94, 320, 180), false, 12, 8);
        QCoreApplication::processEvents();
        saveCapture(nullptr, QStringLiteral("free-closed"));
        toolbar.setSelectionState(QRect(127, 94, 320, 180), true, 12, 8,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt,
                                  ScreenshotSelectionAspectRatioPreset::Landscape16x9);
        QCoreApplication::processEvents();
        saveCapture(nullptr, QStringLiteral("locked-closed"));
        for (const bool bottomEdge : {false, true}) {
            host.move(screenBounds.left() + 8,
                      bottomEdge ? screenBounds.bottom() - host.height() + 1 : screenBounds.top());
            toolbar.moveContentTo(QPoint(24, bottomEdge ? host.height() - 30 : 4));
            select->setPlacement(bottomEdge ? adqt::widgets::AdSelect::Placement::BottomLeft
                                            : adqt::widgets::AdSelect::Placement::TopLeft);
            QCoreApplication::processEvents();
            select->showPopup();
            QCoreApplication::processEvents();
            QWidget* popup = select->view()->window();
            const QRect triggerRect(select->mapToGlobal(QPoint()), select->size());
            const QRect listRect(select->view()->mapToGlobal(QPoint()), select->view()->size());
            require(!triggerRect.intersects(listRect),
                    "edge-flipped ratio popup must not cover its trigger");
            saveCapture(popup,
                        bottomEdge ? QStringLiteral("bottom-edge") : QStringLiteral("top-edge"));
            select->hidePopup();
        }
    }
    theme.setConfig(originalConfig);
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    const qsizetype diagnosticIndex = arguments.indexOf(QStringLiteral("--render-diagnostics"));
    if (diagnosticIndex >= 0) {
        require(diagnosticIndex + 1 < arguments.size(),
                "--render-diagnostics requires an output directory");
        saveAspectRatioDiagnostics(arguments[diagnosticIndex + 1]);
        return 0;
    }
    if (arguments.contains(QStringLiteral("--compact-ratio-tests"))) {
        aspectRatioSelectSynchronizesWithoutDispatchingCommands();
        disabledAspectRatioSelectPreservesItsTextColor();
        return 0;
    }
    if (arguments.contains(QStringLiteral("--icon-centering-tests"))) {
        valueLabelCentersIconsAtEveryRenderScale();
        return 0;
    }
    selectionDragCannotActivateToolbarPreview();
    panelBoundaryExclusivelyOwnsToolbarHoverState();
    valueLabelPaintsFromQtHoverState();
    valueLabelCentersIconsAtEveryRenderScale();
    selectionToolbarInputSurfaceMatchesInteractivePanel();
    selectionToolbarLabelsFollowApplicationFontFamily();
    selectionToolbarUsesCanvasUnitsForEditingAndSmartSelection();
    smartSelectionDefersHiddenFieldsUntilEditing();
    smartSelectionToolbarIsClickThroughAcrossCaptureLifecycles();
    smartSelectionToolbarShedsNativeWindowForcedByNativeSiblingEmbed();
    aspectRatioSelectSynchronizesWithoutDispatchingCommands();
    disabledAspectRatioSelectPreservesItsTextColor();
    aspectRatioPopupOwnsInputAndEndsWithToolbarLifecycle();
    aspectRatioPopupSuspendsScreenshotShortcutsThroughDismissal();
    choosingFreeDispatchesExactlyOneExplicitCommand();
    deletingOpenRatioToolbarBalancesPopupVisibility();
    closingRatioPopupPreservesNewKeyboardFocus();
    return 0;
}
