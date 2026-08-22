#include "screenshotselectiontoolbarwidgets.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPointF>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
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

    sendEnter(&child);
    sendLeave(&child);
    require(hoverTransitions == std::vector<bool>({true}),
            "moving across panel descendants must preserve the hover session");

    sendLeave(&panel);
    require(hoverTransitions == std::vector<bool>({true, false}),
            "leaving the panel should end the hover session");
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
    sendLeave(&label);
    require(renderWidget(&label) == idleImage,
            "value-label leave events should restore the idle visual");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    panelBoundaryExclusivelyOwnsToolbarHoverState();
    valueLabelPaintsFromItsOwnEnterLeaveState();
    return 0;
}
