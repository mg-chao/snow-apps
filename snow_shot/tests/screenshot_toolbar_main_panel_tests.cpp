#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "../src/presentation/tools/screenshottoolpalettebuttons.h"

#include "widgets/button.h"
#include "widgets/radio.h"
#include "widgets/tooltip.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMargins>
#include <QPainter>
#include <QPixmap>
#include <QVector>

#include <cstdlib>
#include <iostream>

namespace {
class LayoutRequestCounter final : public QObject {
  public:
    int count = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (event != nullptr && event->type() == QEvent::LayoutRequest) {
            ++count;
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

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

ScreenshotToolPalette::Options screenshotOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.enableStyleToolbar = false;
    return options;
}

ScreenshotToolPalette::Options recordingOptions() {
    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showRecordingControls = true;
    options.enableStyleToolbar = false;
    return options;
}

void prepare(ScreenshotToolPalette& palette) {
    palette.setShadowMargins(ScreenshotToolbarMainPanel::shadowMargins());
    palette.prepareForDisplay();
    flushEvents();
}

QColor renderedCenterColor(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    return image.pixelColor(widget.rect().center());
}

bool imageContainsColor(const QImage& image, const QColor& expected) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor actual = image.pixelColor(x, y);
            if (actual.alpha() > 0 && actual.red() == expected.red() &&
                actual.green() == expected.green() && actual.blue() == expected.blue()) {
                return true;
            }
        }
    }
    return false;
}

adqt::widgets::AdButton* buttonWithTooltip(ScreenshotToolPalette& palette, const QString& tooltip) {
    for (adqt::widgets::AdButton* button : palette.findChildren<adqt::widgets::AdButton*>()) {
        if (button != nullptr &&
            (button->toolTip() == tooltip ||
             (button->toolTip().startsWith(tooltip + QStringLiteral(" (")) &&
              button->toolTip().endsWith(')')))) {
            return button;
        }
    }
    return nullptr;
}

bool buttonIconContainsColor(const adqt::widgets::AdButton* button, const QColor& expected) {
    if (button == nullptr) {
        return false;
    }
    adqt::icons::IconRenderRequest request;
    request.logicalSize = button->iconSize();
    request.devicePixelRatio = 1.0;
    return imageContainsColor(adqt::icons::renderIconPixmap(button->iconRef(), request).toImage(),
                              expected);
}

void historyButtonsFollowCanvasAvailability() {
    ScreenshotToolPalette::Options options;
    options.showHistoryActions = true;
    options.showMoveTool = false;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showWatermarkTool = true;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette toolbar(options);
    prepare(toolbar);

    auto* undoButton =
        toolbar.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotUndoButton"));
    auto* redoButton =
        toolbar.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRedoButton"));
    auto* watermarkButton =
        toolbar.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotWatermarkButton"));
    require(watermarkButton != nullptr, "toolbar should expose its watermark button");
    require(undoButton != nullptr, "toolbar should expose an undo button");
    require(redoButton != nullptr, "toolbar should expose a redo button");
    require(!undoButton->isEnabled() && !redoButton->isEnabled(),
            "history buttons should start disabled");
    QLayout* mainLayout = toolbar.mainPanel()->layout();
    require(mainLayout != nullptr, "toolbar should expose its main layout");
    const int watermarkIndex = mainLayout->indexOf(watermarkButton);
    const int undoIndex = mainLayout->indexOf(undoButton);
    const int redoIndex = mainLayout->indexOf(redoButton);
    require(watermarkIndex >= 0 && watermarkIndex < undoIndex && undoIndex < redoIndex,
            "undo and redo should appear directly to the right of watermark");

    int undoRequests = 0;
    int redoRequests = 0;
    QObject::connect(&toolbar, &ScreenshotToolPalette::undoRequested,
                     [&undoRequests]() { ++undoRequests; });
    QObject::connect(&toolbar, &ScreenshotToolPalette::redoRequested,
                     [&redoRequests]() { ++redoRequests; });

    SnowCanvasHistoryState state;
    state.canUndo = true;
    toolbar.setHistoryState(state);
    require(undoButton->isEnabled() && !redoButton->isEnabled(),
            "history state should enable only undo when redo is unavailable");
    undoButton->click();
    redoButton->click();
    require(undoRequests == 1 && redoRequests == 0,
            "only enabled history buttons should emit commands");

    state.canUndo = false;
    state.canRedo = true;
    toolbar.setHistoryState(state);
    undoButton->click();
    redoButton->click();
    require(undoRequests == 1 && redoRequests == 1, "redo should emit once it becomes available");
}

void toolbarSurfacesFollowThemeBackground() {
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);

    QWidget host;
    host.resize(160, 48);
    ScreenshotToolbarMainPanel panel(ScreenshotToolbarMainPanel::Options{}, &host);
    panel.resize(160, 48);
    panel.setGraphicsEffect(nullptr);
    panel.move(0, 0);
    host.show();
    panel.show();
    flushEvents();

    const QMargins panelMargins = panel.layout()->contentsMargins();
    require(panelMargins.left() == panelMargins.right(),
            "toolbar main panel should use equal horizontal content margins");

    const QColor lightBackground = themeManager.themeColorScheme().map.colorBgContainer;
    require(renderedCenterColor(panel) == lightBackground,
            "toolbar main panel should use the light theme container background");

    panel.addSeparator();
    QFrame* separator = panel.findChild<QFrame*>();
    require(separator != nullptr, "toolbar main panel should expose its separator");
    const QColor lightBorder = themeManager.themeColorScheme().map.colorBorder;
    require(separator->styleSheet().contains(lightBorder.name(QColor::HexRgb)),
            "toolbar separator should use the light theme border color");

    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Dark);
    flushEvents();

    const QColor darkBackground = themeManager.themeColorScheme().map.colorBgContainer;
    const QColor darkBorder = themeManager.themeColorScheme().map.colorBorder;
    QWidget darkHost;
    darkHost.resize(160, 48);
    ScreenshotToolbarMainPanel darkPanel(ScreenshotToolbarMainPanel::Options{}, &darkHost);
    darkPanel.resize(160, 48);
    darkPanel.setGraphicsEffect(nullptr);
    darkPanel.move(0, 0);
    darkHost.show();
    darkPanel.show();
    flushEvents();
    require(darkBackground != lightBackground && renderedCenterColor(darkPanel) == darkBackground,
            "toolbar main panel should refresh its background when the theme changes");
    require(separator->styleSheet().contains(darkBorder.name(QColor::HexRgb)),
            "toolbar separator should refresh with the dark theme border color");

    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);
    flushEvents();
    QWidget restoredHost;
    restoredHost.resize(160, 48);
    ScreenshotToolbarMainPanel restoredPanel(ScreenshotToolbarMainPanel::Options{}, &restoredHost);
    restoredPanel.resize(160, 48);
    restoredPanel.setGraphicsEffect(nullptr);
    restoredPanel.move(0, 0);
    restoredHost.show();
    restoredPanel.show();
    flushEvents();
    require(renderedCenterColor(restoredPanel) == lightBackground,
            "toolbar main panel should use the light theme after a theme switch");
}

void toolbarSeparatorsKeepMinimumWidthAtCompactScale() {
    QWidget host;
    ScreenshotToolbarMainPanel panel(ScreenshotToolbarMainPanel::Options{}, &host);
    panel.addSeparator();
    panel.setPhysicalScale(0.25);
    panel.resize(panel.sizeHint());
    panel.show();
    flushEvents();

    QFrame* separator = panel.findChild<QFrame*>();
    require(separator != nullptr && separator->width() >= 1,
            "toolbar separator should retain at least one pixel at compact scale");
}

void cachedToolbarIconsFollowThemeColors() {
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);
    flushEvents();

    ScreenshotToolPalette::Options options;
    options.showDragHandle = true;
    options.showWatermarkTool = true;
    options.actions = ScreenshotToolPalette::CopyAction;
    ScreenshotToolPalette toolbar(options);
    require(toolbar.ensureActionFamily(ScreenshotToolPalette::ActionFamily::Selection) &&
                toolbar.ensureStyleFamily(ScreenshotToolPalette::Tool::Watermark),
            "theme icon test should materialize the inspected toolbar families");
    prepare(toolbar);

    auto* selectionOpacity =
        toolbar.findChild<QLabel*>(QStringLiteral("screenshotSelectionOpacityIcon"));
    auto* watermarkOpacity =
        toolbar.findChild<QLabel*>(QStringLiteral("screenshotWatermarkOpacityIcon"));
    auto* dragHandle = qobject_cast<QLabel*>(toolbar.dragHandle());
    auto* copyButton = buttonWithTooltip(toolbar, QStringLiteral("Copy to clipboard"));
    require(selectionOpacity != nullptr && watermarkOpacity != nullptr && dragHandle != nullptr &&
                copyButton != nullptr,
            "theme icon test should expose screenshot toolbar icon controls");

    ScreenshotToolPalette recordingToolbar(recordingOptions());
    prepare(recordingToolbar);
    auto* recordStartButton =
        buttonWithTooltip(recordingToolbar, QStringLiteral("Start recording"));
    auto* microphoneButton =
        buttonWithTooltip(recordingToolbar, QStringLiteral("Record microphone"));
    auto* systemAudioButton =
        buttonWithTooltip(recordingToolbar, QStringLiteral("Record speakers"));
    auto* pauseButton = buttonWithTooltip(recordingToolbar, QStringLiteral("Pause recording"));
    auto* copyGifButton =
        buttonWithTooltip(recordingToolbar, QStringLiteral("Copy animated image"));
    require(recordStartButton != nullptr && microphoneButton != nullptr &&
                systemAudioButton != nullptr && pauseButton != nullptr && copyGifButton != nullptr,
            "theme icon test should expose recording toolbar icon controls");
    recordingToolbar.setRecordingMicrophoneEnabled(true);
    recordingToolbar.setRecordingSystemAudioEnabled(false);
    recordingToolbar.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);

    const auto lightScheme = themeManager.themeColorScheme();
    require(imageContainsColor(selectionOpacity->pixmap().toImage(),
                               lightScheme.map.colorTextQuaternary),
            "selection opacity icon should use the light disabled text color");
    require(imageContainsColor(watermarkOpacity->pixmap().toImage(), lightScheme.map.colorText),
            "watermark opacity icon should use the light text color");
    require(imageContainsColor(dragHandle->pixmap().toImage(), lightScheme.map.colorTextQuaternary),
            "drag handle should use the light weak text color");
    require(buttonIconContainsColor(copyButton, lightScheme.map.colorPrimary),
            "copy icon should use the light primary color");
    require(buttonIconContainsColor(recordStartButton, lightScheme.map.colorPrimary),
            "record start icon should use the light primary color");
    require(buttonIconContainsColor(microphoneButton, lightScheme.map.colorSuccess),
            "enabled microphone icon should use the light success color");
    require(buttonIconContainsColor(systemAudioButton, lightScheme.map.colorTextQuaternary),
            "disabled system audio icon should use the light weak text color");
    require(buttonIconContainsColor(pauseButton, lightScheme.map.colorWarning),
            "active pause icon should use the light warning color");
    require(buttonIconContainsColor(copyGifButton, lightScheme.map.colorPrimary),
            "enabled GIF copy icon should use the light primary color");

    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Dark);
    flushEvents();
    const auto darkScheme = themeManager.themeColorScheme();
    require(
        imageContainsColor(selectionOpacity->pixmap().toImage(),
                           darkScheme.map.colorTextQuaternary) &&
            imageContainsColor(watermarkOpacity->pixmap().toImage(), darkScheme.map.colorText) &&
            imageContainsColor(dragHandle->pixmap().toImage(),
                               darkScheme.map.colorTextQuaternary) &&
            buttonIconContainsColor(copyButton, darkScheme.map.colorPrimary) &&
            buttonIconContainsColor(recordStartButton, darkScheme.map.colorPrimary) &&
            buttonIconContainsColor(microphoneButton, darkScheme.map.colorSuccess) &&
            buttonIconContainsColor(systemAudioButton, darkScheme.map.colorTextQuaternary) &&
            buttonIconContainsColor(pauseButton, darkScheme.map.colorWarning) &&
            buttonIconContainsColor(copyGifButton, darkScheme.map.colorPrimary),
        "cached toolbar icons should refresh to the dark theme colors");

    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);
    flushEvents();
}

void secondaryToolbarUsesEqualHorizontalMargins() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    prepare(toolbar);

    QWidget* panel = toolbar.findChild<QWidget*>(QStringLiteral("screenshotSelectActionPanel"));
    require(panel != nullptr && panel->layout() != nullptr,
            "toolbar should expose its selection action panel layout");
    const QMargins margins = panel->layout()->contentsMargins();
    require(margins.left() == margins.right(),
            "selection action toolbar should use equal horizontal content margins");
}

void recordingToolbarUsesTheScreenshotMainPanelContract() {
    ScreenshotToolPalette screenshotToolbar(screenshotOptions());
    ScreenshotToolPalette recordingToolbar(recordingOptions());
    prepare(screenshotToolbar);
    prepare(recordingToolbar);

    require(recordingToolbar.mainPanel() != nullptr, "recording toolbar should have a main panel");
    require(recordingToolbar.stylePanel() == nullptr,
            "recording toolbar should not create a style row");
    require(recordingToolbar.dragHandle() != nullptr,
            "recording toolbar should keep its left drag handle");
    require(recordingToolbar.trailingDragHandle() == nullptr,
            "recording toolbar should not create a right drag handle");

    const QList<adqt::widgets::AdButton*> screenshotButtons =
        screenshotToolbar.mainPanel()->findChildren<adqt::widgets::AdButton*>();
    const QList<adqt::widgets::AdButton*> recordingButtons =
        recordingToolbar.mainPanel()->findChildren<adqt::widgets::AdButton*>();
    require(!screenshotButtons.isEmpty(), "screenshot toolbar should expose controls");
    require(!recordingButtons.isEmpty(), "recording toolbar should expose controls");
    require(screenshotButtons.constFirst()->size() == recordingButtons.constFirst()->size(),
            "recording and screenshot toolbar buttons should share dimensions");

    QLabel* duration =
        recordingToolbar.findChild<QLabel*>(QStringLiteral("screenRecordingDuration"));
    require(duration != nullptr, "recording toolbar should expose its duration label");
    require(duration->height() == recordingButtons.constFirst()->height(),
            "recording duration should align with the shared button height");

    const QMargins margins = ScreenshotToolbarMainPanel::shadowMargins();
    const QSize expectedSize =
        recordingToolbar.contentSizeHint() +
        QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    require(recordingToolbar.size() == expectedSize,
            "recording toolbar should reserve the shared shadow margins without a style row");

    const auto* shadow =
        qobject_cast<QGraphicsDropShadowEffect*>(recordingToolbar.mainPanel()->graphicsEffect());
    require(shadow != nullptr, "recording shadow should use the reference drop-shadow effect");
    require(qFuzzyCompare(shadow->blurRadius() + 1.0, 19.0) &&
                shadow->offset() == QPointF(0.0, 3.0) && shadow->color() == QColor(0, 0, 0, 90),
            "recording shadow should preserve the reference visual parameters");
}

void mainToolbarSpacingUsesReferenceItemMetrics() {
    ScreenshotToolPalette::Options options;
    options.enableStyleToolbar = false;
    ScreenshotToolPalette toolbar(options);
    prepare(toolbar);

    QLayout* layout = toolbar.mainPanel()->layout();
    require(layout != nullptr, "scaled toolbar should expose its main layout");
    layout->activate();

    const int referencePanelWidth = toolbar.mainPanel()->sizeHint().width();
    const QMargins referenceMargins = layout->contentsMargins();
    QVector<int> referenceWidths{referenceMargins.left()};
    referenceWidths.reserve(layout->count() + 2);
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        require(item != nullptr, "reference toolbar layout contains an empty item");
        referenceWidths.append(item->geometry().width());
    }
    referenceWidths.append(referenceMargins.right());

    int referenceTotalWidth = 0;
    for (int width : referenceWidths) {
        referenceTotalWidth += width;
    }
    require(referenceTotalWidth == referencePanelWidth,
            "reference toolbar metrics do not span the panel width");

    constexpr qreal exactMetricScale = 1.5;
    toolbar.setPhysicalScale(exactMetricScale);
    flushEvents();
    layout->activate();

    int spacerCount = 0;
    int buttonCount = 0;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item == nullptr) {
            continue;
        }
        if (item->spacerItem() != nullptr) {
            ++spacerCount;
            require(item->geometry().width() == qRound(8.0 * exactMetricScale),
                    "main toolbar item spacing should remain 8 reference pixels at 1.5x");
            continue;
        }
        if (qobject_cast<adqt::widgets::AdButton*>(item->widget()) != nullptr) {
            ++buttonCount;
            require(item->geometry().width() == qRound(32.0 * exactMetricScale),
                    "main toolbar buttons should retain their 32 reference pixel width at 1.5x");
        }
    }
    require(spacerCount > 0 && buttonCount > 0,
            "scaled main toolbar should contain buttons and explicit item spacers");

    constexpr qreal fractionalScale = 0.8;
    toolbar.setPhysicalScale(fractionalScale);
    flushEvents();
    layout->activate();

    const int targetWidth = qRound(referencePanelWidth * fractionalScale);
    require(toolbar.mainPanel()->sizeHint().width() == targetWidth,
            "fractionally scaled toolbar did not preserve the rounded total width");
    int referenceEdge = referenceWidths.constFirst();
    bool observedCumulativeRedistribution = false;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        referenceEdge += referenceWidths.at(index + 1);
        const int expectedEdge = qRound(static_cast<qreal>(referenceEdge) * targetWidth /
                                        referencePanelWidth);
        require(item->geometry().x() + item->geometry().width() == expectedEdge,
                "main toolbar item edge was not allocated from cumulative reference widths");
        observedCumulativeRedistribution =
            observedCumulativeRedistribution ||
            item->geometry().width() != qRound(referenceWidths.at(index + 1) * fractionalScale);
    }
    require(observedCumulativeRedistribution,
            "fractional toolbar scale did not exercise cumulative rounding redistribution");
}

void toolbarTooltipsUseApplicationBridge() {
    ScreenshotToolPalette toolbar(screenshotOptions());
    prepare(toolbar);

    const QList<QWidget*> controls = toolbar.mainPanel()->findChildren<QWidget*>();
    int tooltipTriggerCount = 0;
    for (QWidget* control : controls) {
        if (control == nullptr || control->toolTip().isEmpty()) {
            continue;
        }

        ++tooltipTriggerCount;
        const QList<adqt::widgets::AdTooltip*> tooltips =
            control->findChildren<adqt::widgets::AdTooltip*>(QString(), Qt::FindDirectChildrenOnly);
        require(tooltips.isEmpty(),
                "toolbar tooltips should be rendered by the application QtTooltipBridge");
    }
    require(tooltipTriggerCount > 0, "screenshot toolbar should expose tooltip triggers");
}

void cornerRadiusTextKeepsItsPhysicalSizeAcrossDpiChanges() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    prepare(toolbar);

    auto* editor = dynamic_cast<CornerRadiusEditorButton*>(
        toolbar.findChild<QWidget*>(QStringLiteral("screenshotSelectionCornerRadiusButton")));
    require(editor != nullptr, "style toolbar should expose a corner-radius editor");

    const QFont referenceFont = editor->font();
    const qreal referenceLogicalSize = referenceFont.pointSizeF() > 0.0
                                           ? referenceFont.pointSizeF()
                                           : static_cast<qreal>(referenceFont.pixelSize());
    require(referenceLogicalSize > 0.0, "corner-radius editor should have a valid font size");

    constexpr qreal referenceDpr = 1.25;
    const qreal referencePhysicalSize = referenceLogicalSize * referenceDpr;
    constexpr qreal targetDprs[] = {1.0, 1.5, 2.0};
    for (const qreal targetDpr : targetDprs) {
        toolbar.setPhysicalScale(referenceDpr / targetDpr);

        const QFont scaledFont = editor->font();
        const qreal scaledLogicalSize = scaledFont.pointSizeF() > 0.0
                                            ? scaledFont.pointSizeF()
                                            : static_cast<qreal>(scaledFont.pixelSize());
        require(std::abs(scaledLogicalSize * targetDpr - referencePhysicalSize) <= 0.01,
                "corner-radius text should counter-scale for the destination monitor");
    }
}

void styleToolbarPresetFitsEveryToolWithoutResizing() {
    ScreenshotToolPalette::Options options;
    options.showTextTool = true;
    ScreenshotToolPalette toolbar(options);
    constexpr ScreenshotToolPalette::Tool tools[] = {
        ScreenshotToolPalette::Tool::Select,
        ScreenshotToolPalette::Tool::Shape,
        ScreenshotToolPalette::Tool::Arrow,
        ScreenshotToolPalette::Tool::Text,
    };
    require(toolbar.ensureActionFamily(ScreenshotToolPalette::ActionFamily::Selection),
            "preset test should materialize the inspected selection actions");
    for (const ScreenshotToolPalette::Tool tool : tools) {
        if (tool != ScreenshotToolPalette::Tool::Select) {
            require(toolbar.ensureStyleFamily(tool),
                    "preset test should materialize every inspected style family");
        }
    }
    toolbar.setStyleToolbarAboveMain(true);
    prepare(toolbar);

    const QSize presetSize = toolbar.size();
    const QSize presetContentSize = toolbar.contentSizeHint();
    const QRect presetMainToolbarRect = toolbar.mainToolbarContentRect();
    int visibleContentChangeCount = 0;
    QObject::connect(&toolbar, &ScreenshotToolPalette::visibleContentChanged,
                     [&visibleContentChangeCount]() { ++visibleContentChangeCount; });
    for (const ScreenshotToolPalette::Tool tool : tools) {
        const int previousChangeCount = visibleContentChangeCount;
        toolbar.setActiveTool(tool);
        flushEvents();
        require(toolbar.size() == presetSize && toolbar.contentSizeHint() == presetContentSize &&
                    toolbar.mainToolbarContentRect() == presetMainToolbarRect,
                "activating a style tool must not resize the toolbar preset");
        require(visibleContentChangeCount == previousChangeCount + 1,
                "switching secondary toolbars should refresh the window only once");
    }

    constexpr qreal scales[] = {0.8, 1.25};
    for (const qreal scale : scales) {
        toolbar.setPhysicalScale(scale);
        const QSize scaledPresetSize = toolbar.size();
        const QSize scaledPresetContentSize = toolbar.contentSizeHint();
        for (const ScreenshotToolPalette::Tool tool : tools) {
            toolbar.setActiveTool(tool);
            flushEvents();
            require(toolbar.size() == scaledPresetSize &&
                        toolbar.contentSizeHint() == scaledPresetContentSize,
                    "the precomputed toolbar size must stay fixed at every scale");
        }
    }
}

void styleToolbarButtonGroupLayoutRequestQuiesces() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    require(toolbar.ensureStyleFamily(ScreenshotToolPalette::Tool::Shape),
            "layout test should materialize the inspected shape family");
    prepare(toolbar);

    QWidget* shapeGroup = toolbar.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    require(shapeGroup != nullptr, "style toolbar should expose its shape button group");
    require(shapeGroup->layout() != nullptr, "shape button group should have a managed layout");

    // Clear construction-time requests before observing a deliberate
    // invalidation of the managed button-group layout.
    QCoreApplication::sendPostedEvents(shapeGroup, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(shapeGroup, QEvent::LayoutRequest);

    LayoutRequestCounter counter;
    shapeGroup->installEventFilter(&counter);
    shapeGroup->layout()->invalidate();
    QCoreApplication::sendPostedEvents(shapeGroup, QEvent::LayoutRequest);
    const int settledCount = counter.count;
    require(settledCount == 1, "managed layout invalidation should post one layout request");

    QCoreApplication::sendPostedEvents(shapeGroup, QEvent::LayoutRequest);
    require(counter.count == settledCount,
            "handling a button-group layout request must not post another request");
    shapeGroup->removeEventFilter(&counter);
}

void styleRadioIconsMatchTheirCurrentDevicePixelRatio() {
    auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);
    flushEvents();

    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    require(toolbar.ensureStyleFamily(ScreenshotToolPalette::Tool::Shape),
            "DPI test should materialize the inspected shape family");
    prepare(toolbar);

    QWidget* shapeGroup = toolbar.findChild<QWidget*>(QStringLiteral("screenshotShapeButtonGroup"));
    require(shapeGroup != nullptr, "style toolbar should expose shape radios");
    const QList<adqt::widgets::AdRadio*> radios =
        shapeGroup->findChildren<adqt::widgets::AdRadio*>();
    require(!radios.isEmpty(), "shape button group should contain radios");

    const auto requireSharpPixmap = [](adqt::widgets::AdRadio* radio) {
        const QPixmap icon = radio->icon().pixmap(radio->iconSize());
        const qreal devicePixelRatio = radio->devicePixelRatioF();
        require(!icon.isNull(), "style radio should render an icon pixmap");
        require(qFuzzyCompare(icon.devicePixelRatioF() + 1.0, devicePixelRatio + 1.0),
                "style radio icon should use the radio's current device-pixel ratio");
        require(icon.size() == QSize(qRound(radio->iconSize().width() * devicePixelRatio),
                                     qRound(radio->iconSize().height() * devicePixelRatio)),
                "style radio icon should be rasterized at its requested physical size");
    };

    for (adqt::widgets::AdRadio* radio : radios) {
        requireSharpPixmap(radio);
    }

    toolbar.setPhysicalScale(1.5);
    for (adqt::widgets::AdRadio* radio : radios) {
        requireSharpPixmap(radio);
    }

    const auto iconContainsColor = [](adqt::widgets::AdRadio* radio, const QColor& expected) {
        const QIcon::State state = radio->isChecked() ? QIcon::On : QIcon::Off;
        const QImage image =
            radio->icon().pixmap(radio->iconSize(), QIcon::Normal, state).toImage();
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor actual = image.pixelColor(x, y);
                if (actual.alpha() > 0 && actual.red() == expected.red() &&
                    actual.green() == expected.green() && actual.blue() == expected.blue()) {
                    return true;
                }
            }
        }
        return false;
    };

    adqt::widgets::AdRadio* themeRadio = radios.at(1);
    require(!themeRadio->isChecked(), "theme test radio should start unchecked");
    const QColor lightText = themeManager.themeColorScheme().map.colorText;
    require(iconContainsColor(themeRadio, lightText),
            "unchecked style radio icon should use the light theme text color");

    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Dark);
    flushEvents();
    const auto darkScheme = themeManager.themeColorScheme();
    require(darkScheme.map.colorText != lightText &&
                iconContainsColor(themeRadio, darkScheme.map.colorText),
            "unchecked style radio icon should follow the dark theme text color");

    themeRadio->click();
    flushEvents();
    require(iconContainsColor(themeRadio, darkScheme.map.colorPrimary),
            "checked style radio icon should follow its themed text color");

    themeManager.setThemeAppearance(snow_shot::presentation::styles::ThemeAppearance::Light);
    flushEvents();
    require(iconContainsColor(themeRadio, themeManager.themeColorScheme().map.colorPrimary),
            "checked style radio icon should follow the restored light theme text color");
}

void duplicateStyleStateDoesNotInvalidateToolbarGeometry() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::DefaultRectangle;
    toolbar.setStyleToolbarState(state);
    int visibleContentChanges = 0;
    QObject::connect(&toolbar, &ScreenshotToolPalette::visibleContentChanged,
                     [&visibleContentChanges]() { ++visibleContentChanges; });

    for (int index = 0; index < 32; ++index) {
        state.shapeStyle.strokeWidth = index + 1.0;
        toolbar.setStyleToolbarState(state);
    }
    require(visibleContentChanges == 0,
            "style-value synchronization must not relayout an unchanged control set");

    state.source = SnowCanvasStyleToolbarSource::DefaultArrow;
    toolbar.setStyleToolbarState(state);
    require(visibleContentChanges == 1,
            "changing the active control set should relayout exactly once");
    toolbar.setStyleToolbarState(state);
    require(visibleContentChanges == 1,
            "duplicate style state must not repeat the control-set relayout");
}

void screenshotToolbarRendersShadowOutsideItsPanel() {
    ScreenshotToolPalette toolbar(screenshotOptions());
    prepare(toolbar);
    toolbar.show();
    flushEvents();

    QImage image(toolbar.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    toolbar.render(&painter);
    painter.end();

    const QRect panel = toolbar.mainPanel()->geometry();
    const QPoint topSample(panel.center().x(), panel.top() - 1);
    const QPoint bottomSample(panel.center().x(), panel.bottom() + 1);
    const QPoint leftSample(panel.left() - 1, panel.center().y());
    const QPoint rightSample(panel.right() + 1, panel.center().y());
    require(image.pixelColor(topSample).alpha() > 0 && image.pixelColor(bottomSample).alpha() > 0 &&
                image.pixelColor(leftSample).alpha() > 0 &&
                image.pixelColor(rightSample).alpha() > 0,
            "screenshot toolbar shadow should remain visible outside the panel");
    require(image.pixelColor(topSample).alpha() < 255 &&
                image.pixelColor(bottomSample).alpha() < 255 &&
                image.pixelColor(leftSample).alpha() < 255 &&
                image.pixelColor(rightSample).alpha() < 255,
            "screenshot toolbar shadow should remain translucent outside the panel");
}

void filterValuesDoNotRelayoutTheStableControlSet() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Filter);

    SnowCanvasStyleToolbarState state;
    state.source = SnowCanvasStyleToolbarSource::DefaultFilter;
    state.filterStyle.type = SnowCanvasFilterType::Grayscale;
    toolbar.setStyleToolbarState(state);

    int visibleContentChanges = 0;
    QObject::connect(&toolbar, &ScreenshotToolPalette::visibleContentChanged,
                     [&visibleContentChanges]() { ++visibleContentChanges; });

    state.filterStyle.opacity = 0.45;
    toolbar.setStyleToolbarState(state);
    require(visibleContentChanges == 0,
            "filter value synchronization must not relayout unchanged controls");

    state.filterStyle.type = SnowCanvasFilterType::Mosaic;
    toolbar.setStyleToolbarState(state);
    require(visibleContentChanges == 0,
            "filter type changes must not relayout the stable control set");

    state.filterStyle.strength = 0.82;
    toolbar.setStyleToolbarState(state);
    toolbar.setStyleToolbarState(state);
    require(visibleContentChanges == 0, "filter value updates and duplicates must not relayout");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    historyButtonsFollowCanvasAvailability();
    toolbarSurfacesFollowThemeBackground();
    toolbarSeparatorsKeepMinimumWidthAtCompactScale();
    secondaryToolbarUsesEqualHorizontalMargins();
    cachedToolbarIconsFollowThemeColors();
    recordingToolbarUsesTheScreenshotMainPanelContract();
    mainToolbarSpacingUsesReferenceItemMetrics();
    screenshotToolbarRendersShadowOutsideItsPanel();
    toolbarTooltipsUseApplicationBridge();
    cornerRadiusTextKeepsItsPhysicalSizeAcrossDpiChanges();
    styleToolbarPresetFitsEveryToolWithoutResizing();
    styleToolbarButtonGroupLayoutRequestQuiesces();
    styleRadioIconsMatchTheirCurrentDevicePixelRatio();
    duplicateStyleStateDoesNotInvalidateToolbarGeometry();
    filterValuesDoNotRelayoutTheStableControlSet();
    return 0;
}
