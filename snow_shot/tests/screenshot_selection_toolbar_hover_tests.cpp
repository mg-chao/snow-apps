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
#include <QTranslator>
#include <QWidget>
#include <QWheelEvent>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
class NoOpSelectionToolbarCommands final : public ScreenshotSelectionToolbarCommandSink {
  public:
    void toggleSelectionAspectRatioLockFromToolbar() override {
        ++interactionCount;
    }
    void openSelectionResizeModalFromToolbar() override {
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
    void setSelectionToolbarHovered(bool) override {
        ++interactionCount;
    }

    std::vector<int> lastAdjustment;
    int interactionCount = 0;
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

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    panelBoundaryExclusivelyOwnsToolbarHoverState();
    valueLabelPaintsFromQtHoverState();
    selectionToolbarInputSurfaceMatchesInteractivePanel();
    selectionToolbarLabelsFollowApplicationFontFamily();
    selectionToolbarUsesCanvasUnitsForEditingAndSmartSelection();
    smartSelectionToolbarIsClickThroughAcrossCaptureLifecycles();
    smartSelectionToolbarShedsNativeWindowForcedByNativeSiblingEmbed();
    return 0;
}
