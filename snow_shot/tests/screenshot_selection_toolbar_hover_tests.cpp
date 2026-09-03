#include "screenshotselectiontoolbarwidgets.h"
#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QHideEvent>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPointF>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
class NoOpSelectionToolbarCommands final : public ScreenshotSelectionToolbarCommandSink {
  public:
    void toggleSelectionAspectRatioLockFromToolbar() override { ++interactionCount; }
    void openSelectionResizeModalFromToolbar() override { ++interactionCount; }
    void hideColorPickersForScreenshotUi() override { ++interactionCount; }
    void adjustSelectionFromToolbar(int, int, int, int) override { ++interactionCount; }
    void setSelectionCornerRadiusFromToolbar(int) override { ++interactionCount; }
    void setSelectionShadowWidthFromToolbar(int) override { ++interactionCount; }
    void setSelectionToolbarHovered(bool) override { ++interactionCount; }

    int interactionCount = 0;
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

QImage renderWidget(QWidget* widget) {
    require(widget != nullptr, "render target should exist");
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget->render(&painter);
    return image;
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
}

void valueLabelPaintsFromItsOwnEnterLeaveState() {
    SelectionToolbarValueLabel label;
    label.setText(QStringLiteral("640"));
    label.setFixedSize(label.sizeHint());

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
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    panelBoundaryExclusivelyOwnsToolbarHoverState();
    valueLabelPaintsFromItsOwnEnterLeaveState();
    selectionToolbarInputSurfaceMatchesInteractivePanel();
    smartSelectionToolbarIsClickThroughAcrossCaptureLifecycles();
    return 0;
}
