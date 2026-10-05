#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/mainwindowskinwidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "../src/image/snowimageqtcodec.h"
#include "../src/presentation/tools/screenshottoolpalettebuttons.h"

#include "widgets/button.h"
#include "widgets/control_scale.h"
#include "widgets/color_picker.h"
#include "widgets/radio.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/tooltip.h"
#include "theme/theme_manager.h"
#include "antd_icons.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QImage>
#include <QHideEvent>
#include <QLabel>
#include <QLayout>
#include <QBoxLayout>
#include <QLineEdit>
#include <QMargins>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QShowEvent>
#include <QVector>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

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

class ToolbarAppearanceEventCounter final : public QObject {
  public:
    int updateRequests = 0;
    int styleChanges = 0;
    int themeChanges = 0;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::UpdateRequest) {
            ++updateRequests;
        } else if (event->type() == QEvent::StyleChange) {
            ++styleChanges;
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

QImage renderToolbarWidget(QWidget& widget) {
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();
    return image;
}

bool imageContainsOpaqueColor(const QImage& image, const QColor& expected) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor actual = image.pixelColor(x, y);
            if (actual.alpha() >= 250 && actual.red() == expected.red() &&
                actual.green() == expected.green() && actual.blue() == expected.blue()) {
                return true;
            }
        }
    }
    return false;
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
        if (button != nullptr && (button->toolTip() == tooltip ||
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

void smallToolbarIconStaysVerticallyCentered() {
    ScreenshotToolbarMainPanel panel(ScreenshotToolbarMainPanel::Options{});
    adqt::widgets::AdControlScaleScope scaleScope(&panel);
    auto* button =
        panel.createToolButton(nullptr, adqt::icons::antd::outlined::Border().withColors(
                                            adqt::icons::IconColors::primary(Qt::magenta)));
    scaleScope.publishScale(
        adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.0, 1.0, 0.8));
    panel.setPhysicalScale(0.8);
    button->ensurePolished();
    const qreal dpr = button->devicePixelRatioF();
    QImage image(button->size() * dpr, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    button->render(&image);
    int top = image.height();
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 200 && color.blue() > 200 && color.green() < 100) {
                top = qMin(top, y);
                bottom = qMax(bottom, y);
            }
        }
    }
    require(bottom >= top, "the centering probe must render the toolbar icon");
    require(qAbs(top - (image.height() - bottom - 1)) <= 1,
            "Small toolbar icons must be centered at the rendering device pixel ratio");
}

void customToolbarWidgetsKeepCallerMetricsDuringScaling() {
    ScreenshotToolbarMainPanel panel(ScreenshotToolbarMainPanel::Options{});
    auto* button = panel.createActionButton("Copy", adqt::icons::antd::outlined::Copy());
    auto* readout = new QLabel(QStringLiteral("00:00:00"), &panel);
    readout->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    readout->setFixedSize(96, screenshot_action_toolbar::ControlSize);
    panel.contentLayout()->addWidget(button);
    panel.addSpacing(4);
    panel.contentLayout()->addWidget(readout);
    panel.show();
    panel.adjustSize();
    flushEvents();
    for (qreal scale : {1.0, 0.75, 1.25, 1.5, 2.0, 0.75}) {
        const QSize callerSize(qRound(96 * scale),
                               qRound(screenshot_action_toolbar::ControlSize * scale));
        // A parent participant commits before its row, as the recording palette does.
        readout->setFixedSize(callerSize);
        panel.setPhysicalScale(scale);
        panel.adjustSize();
        flushEvents();
        require(readout->size() == callerSize && readout->width() > 0 &&
                    button->width() == qRound(screenshot_action_toolbar::ControlSize * scale),
                "row scaling preserves caller metrics while scaling its own buttons");
        require(panel.rect().contains(readout->geometry()) &&
                    panel.rect().contains(button->geometry()),
                "custom widgets and row-owned controls remain unclipped after scaling");
        const int panelWidth = panel.width();
        readout->setFixedWidth(readout->width() + 30);
        panel.adjustSize();
        flushEvents();
        require(panel.width() >= panelWidth + 30 && panel.rect().contains(readout->geometry()),
                "caller content can grow without a stale reference-size cache clipping the row");
    }
}

void hiddenToolbarPeersKeepVisibleReferenceMetrics() {
    ScreenshotToolbarMainPanel panel(ScreenshotToolbarMainPanel::Options{});
    auto* first =
        panel.createActionButton("Start recording", adqt::icons::antd::outlined::PlayCircle());
    auto* alternate =
        panel.createActionButton("Stop recording", adqt::icons::antd::outlined::Stop());
    auto* copy = panel.createActionButton("Copy", adqt::icons::antd::outlined::Copy());
    panel.contentLayout()->addWidget(first);
    panel.contentLayout()->addWidget(alternate);
    panel.addSpacing(4);
    panel.contentLayout()->addWidget(copy);
    alternate->hide();
    panel.show();
    panel.adjustSize();
    flushEvents();
    const QSize referenceSize = panel.sizeHint();
    for (qreal scale : {0.75, 1.0, 1.5, 2.0}) {
        panel.setPhysicalScale(scale);
        panel.adjustSize();
        flushEvents();
        const int controlSize = qRound(screenshot_action_toolbar::ControlSize * scale);
        require(first->width() == controlSize && alternate->width() == controlSize &&
                    copy->width() == controlSize,
                "hidden phase peers retain source metrics without compressing visible controls");
        require(panel.sizeHint() == QSize(qRound(referenceSize.width() * scale),
                                          qRound(referenceSize.height() * scale)) &&
                    panel.rect().contains(first->geometry()) &&
                    panel.rect().contains(copy->geometry()),
                "hidden phase peers preserve the visible row's reference-size contract");
        first->hide();
        alternate->show();
        panel.adjustSize();
        flushEvents();
        require(alternate->width() == controlSize && panel.rect().contains(alternate->geometry()),
                "the hidden peer fits its logical position when the active phase changes");
        alternate->hide();
        first->show();
    }
}

void toolbarControlsStayVerticallyCentered() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    adqt::widgets::AdControlScaleScope scaleScope(&toolbar);
    prepare(toolbar);
    toolbar.show();
    bool centered = true;
    for (const qreal scale : {1.0, 0.8, 0.64, 0.8 / 1.5, 0.4, 1.25, 0.8, 1.0}) {
        scaleScope.publishScale(
            adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.0, 1.0, scale));
        toolbar.setPhysicalScale(scale);
        for (const auto tool :
             {ScreenshotToolPalette::Tool::Shape, ScreenshotToolPalette::Tool::Arrow,
              ScreenshotToolPalette::Tool::Spotlight, ScreenshotToolPalette::Tool::Filter,
              ScreenshotToolPalette::Tool::Text, ScreenshotToolPalette::Tool::Select}) {
            toolbar.setActiveTool(tool);
            flushEvents();
            for (QWidget* panel :
                 {toolbar.mainPanel(), toolbar.stylePanel(), toolbar.actionPanel()}) {
                if (panel == nullptr || !panel->isVisible()) {
                    continue;
                }
                for (QWidget* child : panel->findChildren<QWidget*>()) {
                    if (!child->isVisible() || child->isWindow() ||
                        (qobject_cast<adqt::widgets::AdButton*>(child) == nullptr &&
                         qobject_cast<adqt::widgets::AdRadio*>(child) == nullptr &&
                         qobject_cast<adqt::widgets::AdSlider*>(child) == nullptr &&
                         qobject_cast<adqt::widgets::AdSelect*>(child) == nullptr &&
                         qobject_cast<adqt::widgets::AdColorPicker*>(child) == nullptr)) {
                        continue;
                    }
                    const int top = child->mapTo(panel, QPoint()).y();
                    const int bottom = panel->height() - top - child->height();
                    if (qAbs(top - bottom) > 1) {
                        centered = false;
                        std::cerr << "scale=" << scale
                                  << " panel=" << panel->objectName().toStdString()
                                  << " child=" << child->objectName().toStdString()
                                  << " top=" << top << " bottom=" << bottom << '\n';
                    }
                }
            }
        }
    }
    require(centered, "toolbar controls must remain vertically centered at every size");
}

void drawingSelectsInheritScaleWhenMaterialized() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    adqt::widgets::AdControlScaleScope scope(&toolbar);
    prepare(toolbar);
    toolbar.show();
    bool matched = true;
    for (const qreal scale : {0.8, 1.0, 0.64, 0.8}) {
        scope.publishScale(
            adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1, 1, scale));
        toolbar.setPhysicalScale(scale);
        for (const auto tool :
             {ScreenshotToolPalette::Tool::Text, ScreenshotToolPalette::Tool::RectangleFilter,
              ScreenshotToolPalette::Tool::SerialNumber, ScreenshotToolPalette::Tool::PenFilter,
              ScreenshotToolPalette::Tool::Text}) {
            toolbar.setActiveTool(tool);
            flushEvents();
            const auto selects = toolbar.stylePanel()->findChildren<adqt::widgets::AdSelect*>();
            require(!selects.isEmpty(), "drawing tool must materialize its select editor");
            for (auto* select : selects) {
                if (!select->isVisible()) {
                    continue;
                }
                select->setStatus(adqt::widgets::AdSelect::Status::Warning);
                select->setStatus(adqt::widgets::AdSelect::Status::None);
                flushEvents();
                if (select->height() != qRound(28 * scale) ||
                    select->lineEdit()->font().pixelSize() != qRound(14 * scale)) {
                    matched = false;
                    std::cerr << "tool=" << static_cast<int>(tool) << " scale=" << scale
                              << " select=" << select->objectName().toStdString()
                              << " height=" << select->height()
                              << " font=" << select->lineEdit()->font().pixelSize() << '\n';
                }
            }
        }
    }
    require(matched, "drawing selects must inherit scale on creation, reuse, and style refresh");
}

void toolbarSelectHeightSurvivesStyleRefresh() {
    for (const int buttonSize : {28, 32}) {
        QWidget host;
        ScreenshotToolPaletteSelectEditorConfig config;
        auto editor = createScreenshotToolPaletteSelectEditor(
            &host, config, ScreenshotToolPaletteButtonMetrics{buttonSize, 18, 1.0});
        adqt::widgets::AdControlScaleScope scope(&host);
        for (const qreal scale : {0.8, 1.0, 0.64}) {
            scope.publishScale(
                adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1, 1, scale));
            configureScreenshotToolPaletteSelectEditor(
                editor, ScreenshotToolPaletteButtonMetrics{buttonSize, 18, scale});
            editor.select->setStatus(adqt::widgets::AdSelect::Status::Warning);
            editor.select->setStatus(adqt::widgets::AdSelect::Status::None);
            require(editor.select->height() == qRound(buttonSize * scale),
                    "toolbar selects must retain the toolbar height after style refreshes");
        }
    }
}

void secondaryToolbarControlsFollowScale() {
    ScreenshotToolPalette::Options options;
    options.showOcrTool = true;
    options.showTextTranslationTool = true;
    options.showTableTool = true;
    options.showScrollingScreenshotTool = true;
    ScreenshotToolPalette toolbar(options);
    adqt::widgets::AdControlScaleScope scaleScope(&toolbar);
    prepare(toolbar);
    toolbar.show();
    bool matched = true;
    for (const qreal scale : {0.8, 1.0, 0.64, 0.8 / 1.5, 1.25, 0.8, 1.0}) {
        scaleScope.publishScale(
            adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.0, 1.0, scale));
        toolbar.setPhysicalScale(scale);
        for (const auto tool :
             {ScreenshotToolPalette::Tool::Select, ScreenshotToolPalette::Tool::Ocr,
              ScreenshotToolPalette::Tool::TextTranslation, ScreenshotToolPalette::Tool::Table,
              ScreenshotToolPalette::Tool::ScrollingScreenshot}) {
            toolbar.setActiveTool(tool);
            flushEvents();
            QWidget* panel = toolbar.actionPanel();
            require(panel != nullptr, "action tools must materialize their secondary panel");
            for (QWidget* child : panel->findChildren<QWidget*>()) {
                if (!child->isVisible() || child->isWindow() ||
                    (qobject_cast<adqt::widgets::AdButton*>(child) == nullptr &&
                     qobject_cast<adqt::widgets::AdSelect*>(child) == nullptr &&
                     qobject_cast<adqt::widgets::AdSlider*>(child) == nullptr)) {
                    continue;
                }
                if (child->height() != qRound(32 * scale)) {
                    matched = false;
                    std::cerr << "scale=" << scale << " child=" << child->objectName().toStdString()
                              << " height=" << child->height() << " expected=" << qRound(32 * scale)
                              << '\n';
                }
            }
        }
    }
    require(matched, "all secondary action controls must follow the toolbar scale");
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

void toolbarRowsShareShadowMetrics() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Shape);
    for (qreal scale : {0.5, 1.0, 1.25, 1.5, 2.0}) {
        toolbar.setPhysicalScale(scale);
        auto* main =
            qobject_cast<QGraphicsDropShadowEffect*>(toolbar.mainPanel()->graphicsEffect());
        auto* style =
            qobject_cast<QGraphicsDropShadowEffect*>(toolbar.stylePanel()->graphicsEffect());
        require(main != nullptr && style != nullptr && main->blurRadius() == style->blurRadius() &&
                    main->offset() == style->offset() && main->color() == style->color(),
                "main and secondary rows must use identical shadow metrics at every scale");
        require(qFuzzyCompare(main->blurRadius(), 18.0 * scale) &&
                    main->offset() == QPointF(0.0, 3.0 * scale),
                "shared toolbar shadows must retain fractional DPI precision");
    }
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
    auto* copyRecordingButton =
        buttonWithTooltip(recordingToolbar, QStringLiteral("Copy recording"));
    require(recordStartButton != nullptr && microphoneButton != nullptr &&
                systemAudioButton != nullptr && pauseButton != nullptr &&
                copyRecordingButton != nullptr,
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
    require(buttonIconContainsColor(copyRecordingButton, lightScheme.map.colorPrimary),
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
            buttonIconContainsColor(copyRecordingButton, darkScheme.map.colorPrimary),
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
            require(item->geometry().width() ==
                        qRound(referenceWidths.at(index + 1) * exactMetricScale),
                    "main toolbar spacing must scale from its reference width");
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

    // Keep every separator interval nonzero while inspecting exact cumulative
    // edges. Compact scales intentionally clamp zero-width separators to one
    // pixel, as toolbarSeparatorsKeepMinimumWidthAtCompactScale verifies.
    constexpr qreal fractionalScale = 1.25;
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
        const int expectedEdge =
            qRound(static_cast<qreal>(referenceEdge) * targetWidth / referencePanelWidth);
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

void visibleToolbarRowsDrivePaletteGeometry() {
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

    require(toolbar.findChild<QWidget*>(QStringLiteral("screenshotStyleToolbarReserve")) == nullptr,
            "the toolbar should not create a placeholder reserve control");
    const QMargins shadowMargins = ScreenshotToolbarMainPanel::shadowMargins();
    const QSize mainPanelSize = toolbar.mainPanel()->size();
    int visibleContentChangeCount = 0;
    QObject::connect(&toolbar, &ScreenshotToolPalette::visibleContentChanged,
                     [&visibleContentChangeCount]() { ++visibleContentChangeCount; });
    for (const ScreenshotToolPalette::Tool tool : tools) {
        const int previousChangeCount = visibleContentChangeCount;
        toolbar.setActiveTool(tool);
        flushEvents();
        const ScreenshotToolbarPlacementSnapshot snapshot = toolbar.placementSnapshot();
        const QSize expectedPaletteSize =
            snapshot.visibleContentSize + QSize(shadowMargins.left() + shadowMargins.right(),
                                                shadowMargins.top() + shadowMargins.bottom());
        require(toolbar.size() == expectedPaletteSize &&
                    toolbar.contentSizeHint() == snapshot.visibleContentSize &&
                    toolbar.mainPanel()->size() == mainPanelSize &&
                    toolbar.mainToolbarContentRect() == snapshot.top.mainToolbarContentRect,
                "visible toolbar rows should determine the palette geometry");
        const QWidget* secondaryPanel = toolbar.actionToolbarVisible()  ? toolbar.actionPanel()
                                        : toolbar.styleToolbarVisible() ? toolbar.stylePanel()
                                                                        : nullptr;
        require(secondaryPanel != nullptr,
                "each inspected tool should expose its active secondary toolbar");
        const QRect secondaryRect = secondaryPanel->geometry().translated(-toolbar.contentOffset());
        require(secondaryRect == snapshot.top.secondaryToolbarContentRect,
                "the active secondary toolbar should match the placement snapshot");
        require(visibleContentChangeCount == previousChangeCount + 1,
                "switching secondary toolbars should refresh the window only once");
    }

    constexpr qreal scales[] = {0.8, 1.25};
    for (const qreal scale : scales) {
        toolbar.setPhysicalScale(scale);
        for (const ScreenshotToolPalette::Tool tool : tools) {
            toolbar.setActiveTool(tool);
            flushEvents();
            const ScreenshotToolbarPlacementSnapshot snapshot = toolbar.placementSnapshot();
            const QSize currentShadowExtent = toolbar.size() - toolbar.contentSizeHint();
            const QSize expectedPaletteSize = snapshot.visibleContentSize + currentShadowExtent;
            require(toolbar.size() == expectedPaletteSize,
                    "scaled visible rows should determine the palette extent without a reserve");
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

void spotlightConfigSurvivesStyleRowEviction() {
    ScreenshotToolPalette toolbar(ScreenshotToolPalette::Options{});
    prepare(toolbar);
    for (int iteration = 0; iteration < 8; ++iteration) {
        toolbar.setActiveTool(ScreenshotToolPalette::Tool::Spotlight);
        flushEvents();
        QPointer<adqt::widgets::AdSlider> oldSlider = toolbar.findChild<adqt::widgets::AdSlider*>(
            QStringLiteral("screenshotSpotlightOpacitySlider"));
        require(oldSlider != nullptr, "Spotlight must materialize its opacity slider");
        // Leave value/geometry tooltip work queued when the style row is evicted.
        // Queued tooltip work must not outlive the style row that owns the slider.
        oldSlider->setTooltipEnabled(true);
        oldSlider->setValue(17 + iteration);
        oldSlider->resize(oldSlider->width() + 1, oldSlider->height());
        toolbar.setActiveTool(ScreenshotToolPalette::Tool::Arrow);
        require(oldSlider == nullptr, "changing tool must evict the old Spotlight slider");
        SnowCanvasSpotlightConfig config;
        config.opacity = 0.37;
        toolbar.setSpotlightConfig(config);
        flushEvents();
        toolbar.setActiveTool(ScreenshotToolPalette::Tool::Spotlight);
        auto* slider = toolbar.findChild<adqt::widgets::AdSlider*>(
            QStringLiteral("screenshotSpotlightOpacitySlider"));
        require(slider != nullptr && qRound(slider->value()) == 37,
                "the recreated Spotlight slider must reflect settings synchronized while absent");
    }

    // Spotlight and Watermark reuse the opacity editor. A live widget that now
    // belongs to Watermark must no longer receive Spotlight settings either.
    toolbar.setActiveTool(ScreenshotToolPalette::Tool::Watermark);
    auto* watermarkSlider = toolbar.findChild<adqt::widgets::AdSlider*>(
        QStringLiteral("screenshotWatermarkOpacitySlider"));
    require(watermarkSlider != nullptr, "Watermark must materialize its opacity slider");
    const double watermarkOpacity = watermarkSlider->value();
    SnowCanvasSpotlightConfig config;
    config.opacity = 0.81;
    toolbar.setSpotlightConfig(config);
    require(watermarkSlider->value() == watermarkOpacity,
            "Spotlight updates must not modify an opacity editor reused by Watermark");
}

void toolbarWithoutSkinLeavesAppearanceIdle(snow_shot::storage::ConfigurationStore& configuration,
                                            const QString& skinPath) {
    ScreenshotToolbarPanel row;
    row.setGraphicsEffect(nullptr);
    row.resize(160, 48);
    adqt::widgets::AdButton child(&row);
    child.setGeometry(8, 8, 64, 32);
    row.show();
    flushEvents();

    auto& themes = adqt::theme::ThemeManager::instance();
    adqt::theme::ThemeOverride external;
    external.primary = QColor(Qt::blue);
    external.backgroundOpacity = 0.65;
    themes.setScopeOverride(&row, external);
    flushEvents();
    ToolbarAppearanceEventCounter counter;
    row.installEventFilter(&counter);
    child.installEventFilter(&counter);
    QObject::connect(&themes, &adqt::theme::ThemeManager::themeChanged, &counter,
                     [&counter] { ++counter.themeChanges; });
    const auto requireIdleLifecycle = [&] {
        // Deliver the lifecycle notifications without remapping a native window:
        // repainting for a real show is Qt's responsibility, not skin appearance work.
        for (int index = 0; index < 8; ++index) {
            QHideEvent hide;
            QCoreApplication::sendEvent(&row, &hide);
            QShowEvent show;
            QCoreApplication::sendEvent(&row, &show);
        }
        flushEvents();
        require(counter.updateRequests == 0 && counter.styleChanges == 0 &&
                    counter.themeChanges == 0,
                "unskinned toolbar lifecycle must not schedule paint or theme changes");
        require(themes.scopeOverride(&row) == external,
                "an inactive skin must preserve externally supplied scope fields");
        require(snow_shot::presentation::MainWindowSkinController::existingInstance() == nullptr,
                "an inactive toolbar must leave the skin service unallocated");
    };
    requireIdleLifecycle();
    require(configuration.setValues({{QStringLiteral("interface/toolbar_skin_path"), skinPath},
                                     {QStringLiteral("interface/skin_opacity"), 0}}),
            "configure a fully transparent skin without enabling its renderer");
    requireIdleLifecycle();
    require(configuration.setValues({{QStringLiteral("interface/toolbar_skin_path"), QString()},
                                     {QStringLiteral("interface/skin_opacity"), 100}}),
            "restore the empty toolbar configuration");
    requireIdleLifecycle();
}

void toolbarSkinProfilesAndLifecycle() {
    namespace presentation = snow_shot::presentation;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    QTemporaryDir temporary;
    require(temporary.isValid() &&
                storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated toolbar skin configuration");
    const auto waitUntil = [](const auto& condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 15000) {
            flushEvents();
            QThread::msleep(1);
        }
        require(condition(), "toolbar skin operation must complete");
    };
    const auto writeFixture = [&temporary](const QString& name, const QColor& color) {
        QImage source(96, 64, QImage::Format_ARGB32_Premultiplied);
        source.fill(color);
        const QString path = temporary.filePath(name);
        QFile file(path);
        const QByteArray encoded = snow_shot::image_codec::encodePng(source);
        require(file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size(),
                "write toolbar skin fixture");
        return path;
    };
    const QString toolbarPath = writeFixture(QStringLiteral("toolbar.png"), Qt::magenta);
    const QString mainPath = writeFixture(QStringLiteral("main.png"), Qt::green);
    auto& configuration = storage.configuration();
    require(configuration.setValues({{QStringLiteral("interface/skin_mask_opacity"), 0},
                                     {QStringLiteral("interface/skin_blur_level"), 0}}),
            "prepare unmasked deterministic skin rendering");
    toolbarWithoutSkinLeavesAppearanceIdle(configuration, toolbarPath);

    ScreenshotToolbarPanel first;
    ScreenshotToolbarPanel second;
    first.setGraphicsEffect(nullptr);
    second.setGraphicsEffect(nullptr);
    first.resize(160, 48);
    second.resize(220, 36);
    first.show();
    second.show();
    flushEvents();
    require(presentation::MainWindowSkinController::existingInstance() == nullptr,
            "unskinned toolbar rows must not allocate a skin controller");
    require(configuration.setValues({{QStringLiteral("interface/toolbar_skin_path"), toolbarPath},
                                     {QStringLiteral("interface/skin_path"), mainPath}}),
            "configure independent main-window and toolbar images");
    auto& controller = presentation::MainWindowSkinController::instance();
    presentation::MainWindowSkinWidget main;
    main.resize(80, 64);
    main.show();
    waitUntil([&] {
        return !controller.diagnostics().busy && controller.skinActive(&first) &&
               controller.skinActive(&second) && controller.skinActive(&main);
    });
    require(renderedCenterColor(first) == QColor(Qt::magenta) &&
                renderedCenterColor(second) == QColor(Qt::magenta) &&
                renderedCenterColor(main) == QColor(Qt::green),
            "unequal toolbar viewports and the main interface must retain independent skins");
    flushEvents();
    {
        ToolbarAppearanceEventCounter counter;
        first.installEventFilter(&counter);
        for (int index = 0; index < 8; ++index) {
            emit controller.appearanceChanged();
            emit controller.viewFrameChanged(&first);
        }
        flushEvents();
        require(counter.updateRequests == 0,
                "unchanged skin appearance and frame notifications must not repaint a toolbar");
        const auto preparationJobs = controller.diagnostics().preparationJobs;
        require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 50),
                "change only the opacity of a visible toolbar skin");
        waitUntil([&] { return controller.opacity() == 0.5; });
        flushEvents();
        require(counter.updateRequests > 0 && renderedCenterColor(first) != QColor(Qt::magenta) &&
                    controller.diagnostics().preparationJobs == preparationJobs,
                "skin opacity changes must repaint without rebuilding the toolbar image");
        require(configuration.setValue(QStringLiteral("interface/skin_opacity"), 100),
                "restore the fully visible toolbar fixture");
        waitUntil([&] { return controller.opacity() == 1.0; });
        require(renderedCenterColor(first) == QColor(Qt::magenta),
                "restoring skin opacity must restore the cached toolbar image");
    }
    QImage clipped(first.size(), QImage::Format_ARGB32_Premultiplied);
    clipped.fill(Qt::transparent);
    QPainter painter(&clipped);
    first.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();
    require(clipped.pixelColor(0, 0).alpha() == 0 &&
                clipped.pixelColor(first.rect().center()) == QColor(Qt::magenta),
            "toolbar skins must respect transparent rounded corners");

    const auto beforeEffects = controller.diagnostics();
    require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), 100),
            "make the shared theme mask opaque");
    waitUntil([&] { return controller.maskOpacity() == 1.0; });
    require(renderedCenterColor(first) == presentation::styles::ThemeManager::instance()
                                              .themeColorScheme()
                                              .map.colorBgContainer &&
                controller.diagnostics().preparationJobs == beforeEffects.preparationJobs,
            "mask changes must restore the themed surface without preparing images");
    require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), 0),
            "restore the skin fixture mask");
    waitUntil([&] { return controller.maskOpacity() == 0.0; });

    ScreenshotToolbarPanel probes;
    probes.setGraphicsEffect(nullptr);
    probes.resize(304, 48);
    adqt::widgets::AdButton button(&probes);
    button.setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Solid);
    button.setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
    button.setIconRef(
        adqt::icons::antd::filled::Heart().withColors(adqt::icons::IconColors::primary(Qt::green)));
    button.setIconSize(QSize(16, 16));
    button.setGeometry(8, 8, 40, 32);
    adqt::widgets::AdRadio radio(&probes);
    radio.setVariant(adqt::widgets::AdRadio::Variant::Button);
    radio.setButtonStyle(adqt::widgets::AdRadio::ButtonStyle::Solid);
    radio.setChecked(true);
    radio.setGeometry(56, 8, 40, 32);
    adqt::widgets::AdSelect select(&probes);
    select.setPlaceholder(QString());
    select.setGeometry(104, 8, 96, 32);
    adqt::widgets::AdColorPicker picker(&probes);
    picker.setCssText(QStringLiteral("#d52f61"));
    picker.setGeometry(208, 8, 32, 32);
    ColorSwatchButton swatch(&probes);
    const QColor semanticColor(QStringLiteral("#d52f61"));
    swatch.setSwatchColor(semanticColor);
    swatch.setGeometry(248, 8, 32, 32);
    QWidget detachedPopup(&probes, Qt::Tool);
    for (QWidget* control : {static_cast<QWidget*>(&button), static_cast<QWidget*>(&radio),
                             static_cast<QWidget*>(&select), static_cast<QWidget*>(&picker),
                             static_cast<QWidget*>(&swatch)}) {
        control->setFocusPolicy(Qt::NoFocus);
    }
    probes.show();
    waitUntil([&] { return !controller.diagnostics().busy && controller.skinActive(&probes); });

    auto& componentThemes = adqt::theme::ThemeManager::instance();
    const auto requireRowOpacity = [&componentThemes, &controller](QWidget* row, qreal expected) {
        require(row != nullptr && controller.skinActive(row),
                "visible toolbar rows must have an active skin before checking child surfaces");
        require(qAbs(componentThemes.backgroundOpacity(row) - expected) < 0.0001,
                "toolbar rows must publish their active skin mask to component backgrounds");
        for (QWidget* child : row->findChildren<QWidget*>()) {
            if (!child->isVisibleTo(row) || child->window() != row->window()) {
                continue;
            }
            require(qAbs(componentThemes.backgroundOpacity(child) - expected) < 0.0001,
                    "visible toolbar child surfaces must inherit the skin mask opacity");
        }
    };

    std::vector<std::unique_ptr<ScreenshotToolPalette>> workflows;
    for (int index = 0; index < 4; ++index) {
        ScreenshotToolPalette::Options options;
        options.showDragHandle = true;
        options.showHistoryActions = true;
        options.showMoveTool = index < 2;
        options.enableMoveOptionsToolbar = index == 0;
        options.showLineTool = true;
        options.showFreeDrawTool = true;
        options.showHighlightTool = true;
        options.showPenHighlightTool = index == 3;
        options.showSpotlightTool = true;
        options.showEraserTool = true;
        options.showFilterTool = true;
        options.showWatermarkTool = true;
        options.showTextTool = true;
        options.showSerialNumberTool = true;
        options.showOcrTool = index < 2;
        options.showTextTranslationTool = index < 2;
        options.showTableTool = index < 2;
        options.showQrTool = index < 2;
        options.showImageConversionTools = index < 2;
        options.showSaveButton = index < 2;
        options.showScrollingScreenshotTool = index == 0;
        options.showScreenRecordButton = index == 0;
        options.showGlobalCanvasActions = index == 2;
        options.enableStyleToolbar = true;
        options.showRecordingControls = index == 3;
        options.recordingDrawingMode = index == 3;
        if (index == 0) {
            options.separatorBeforeShape = true;
            options.actions = ScreenshotToolPalette::PinAction |
                              ScreenshotToolPalette::CancelAction |
                              ScreenshotToolPalette::CopyAction;
        } else if (index == 1) {
            options.moveToolPresentation =
                ScreenshotToolPalette::MoveToolPresentation::ResizeWindow;
            options.copyButtonWithNeutralIcon = true;
            options.saveButtonWithResultActions = true;
            options.separatorBeforeConfirm = true;
            options.actions =
                ScreenshotToolPalette::CopyAction | ScreenshotToolPalette::ConfirmAction;
        }
        auto palette = std::make_unique<ScreenshotToolPalette>(options);
        prepare(*palette);
        palette->show();
        palette->setActiveTool(ScreenshotToolPalette::Tool::Shape);
        flushEvents();
        workflows.push_back(std::move(palette));
    }
    waitUntil([&] { return !controller.diagnostics().busy; });
    for (const auto& palette : workflows) {
        for (QWidget* row : {palette->mainPanel(), palette->stylePanel()}) {
            require(row != nullptr && controller.skinActive(row),
                    "every workflow must skin its main and secondary drawing-toolbar rows");
            require(controller.frame(row).image.pixelColor(0, 0) == QColor(Qt::magenta),
                    "every workflow must use the shared toolbar image");
        }
    }
    workflows.front()->setActiveTool(ScreenshotToolPalette::Tool::Move);
    QWidget* action = workflows.front()->actionPanel();
    QWidget* recordingExport = workflows.back()->recordingExportSettingsPanel();
    require(action != nullptr && recordingExport != nullptr,
            "action and recording export rows must exist");
    action->show();
    recordingExport->show();
    waitUntil([&] {
        return !controller.diagnostics().busy && controller.skinActive(action) &&
               controller.skinActive(recordingExport);
    });

    const auto cached = controller.diagnostics();
    second.hide();
    require(!controller.skinActive(&second), "hidden toolbar rows must release their active frame");
    require(componentThemes.backgroundOpacity(&second) == 1.0,
            "hidden toolbar rows must restore opaque component backgrounds");
    second.show();
    waitUntil([&] { return !controller.diagnostics().busy && controller.skinActive(&second); });
    require(controller.diagnostics().decodeJobs == cached.decodeJobs &&
                controller.diagnostics().preparationJobs == cached.preparationJobs,
            "reopening a cached toolbar must reuse its source and prepared frame");

    using Tool = ScreenshotToolPalette::Tool;
    const std::vector<std::pair<Tool, const char*>> tools{
        {Tool::Move, "move"},
        {Tool::Select, "selection"},
        {Tool::Shape, "shape"},
        {Tool::Arrow, "arrow"},
        {Tool::Line, "line"},
        {Tool::FreeDraw, "free-draw"},
        {Tool::RectangleHighlight, "rectangle-highlight"},
        {Tool::PenHighlight, "pen-highlight"},
        {Tool::Eraser, "eraser"},
        {Tool::RectangleFilter, "rectangle-filter"},
        {Tool::Watermark, "watermark"},
        {Tool::Text, "text"},
        {Tool::SerialNumber, "serial-number"},
        {Tool::Ocr, "ocr"},
        {Tool::TextTranslation, "text-translation"},
        {Tool::Table, "table"},
        {Tool::Qr, "qr"},
        {Tool::ScrollingScreenshot, "scrolling-screenshot"},
        {Tool::PenFilter, "pen-filter"},
        {Tool::Spotlight, "spotlight"},
        {Tool::Markdown, "markdown"},
        {Tool::Html, "html"},
        {Tool::AutoFilter, "auto-filter"},
        {Tool::Latex, "latex"},
    };
    const QString snapshotDirectory = qEnvironmentVariable("SNOW_TOOLBAR_SKIN_SNAPSHOTS");
    auto& applicationThemes = presentation::styles::ThemeManager::instance();
    const auto originalAppearance = applicationThemes.themeColorScheme().appearance;
    for (const auto appearance : {presentation::styles::ThemeAppearance::Light,
                                  presentation::styles::ThemeAppearance::Dark}) {
        applicationThemes.setThemeAppearance(appearance);
        for (const int maskPercent : {0, 45, 100}) {
            require(
                configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), maskPercent),
                "configure toolbar component mask opacity");
            const qreal expectedOpacity = maskPercent / 100.0;
            waitUntil([&] { return controller.maskOpacity() == expectedOpacity; });
            workflows.front()->setActiveTool(Tool::Move);
            flushEvents();
            waitUntil([&] { return !controller.diagnostics().busy; });
            requireRowOpacity(&probes, expectedOpacity);
            require(componentThemes.backgroundOpacity(&detachedPopup) == 1.0,
                    "owned popup windows must retain opaque surfaces outside the toolbar backdrop");
            // The solid button paints an offset shadow beneath its fill. Sample
            // the top interior above that shadow, away from its centered icon.
            for (const auto& sample :
                 {std::make_pair(static_cast<QWidget*>(&button), QPoint(20, 1)),
                  std::make_pair(static_cast<QWidget*>(&radio), QPoint(20, 16)),
                  std::make_pair(static_cast<QWidget*>(&select), QPoint(48, 16))}) {
                const QImage rendered = renderToolbarWidget(*sample.first);
                const QColor actual = rendered.pixelColor(sample.second);
                const int expectedAlpha = qRound(255 * expectedOpacity);
                if (qAbs(actual.alpha() - expectedAlpha) > 1) {
                    std::cerr << sample.first->metaObject()->className() << " mask=" << maskPercent
                              << " appearance=" << int(appearance)
                              << " rgba=" << actual.name(QColor::HexArgb).toStdString()
                              << " expected-alpha=" << expectedAlpha << '\n';
                    if (!snapshotDirectory.isEmpty()) {
                        QDir().mkpath(snapshotDirectory);
                        rendered.save(QDir(snapshotDirectory)
                                          .filePath(QStringLiteral("failed-component-probe.png")));
                    }
                }
                require(
                    qAbs(actual.alpha() - expectedAlpha) <= 1,
                    "button, checked radio and select fills must render at the skin mask alpha");
            }
            require(imageContainsOpaqueColor(renderToolbarWidget(button), QColor(Qt::green)),
                    "skin masks must preserve opaque foreground icons");
            require(imageContainsOpaqueColor(renderToolbarWidget(swatch), semanticColor) &&
                        imageContainsOpaqueColor(renderToolbarWidget(picker), semanticColor),
                    "skin masks must preserve the actual colors of drawing swatches and pickers");

            QList<QPair<QString, QImage>> snapshots;
            const auto capture = [&snapshots, &snapshotDirectory](const QString& label,
                                                                  QWidget* row) {
                if (!snapshotDirectory.isEmpty()) {
                    snapshots.append({label, renderToolbarWidget(*row)});
                }
            };
            capture(QStringLiteral("component surfaces and semantic color probes"), &probes);
            for (const auto& palette : workflows) {
                for (QWidget* row :
                     {palette->mainPanel(), palette->stylePanel(), palette->actionPanel(),
                      palette->recordingExportSettingsPanel()}) {
                    if (row != nullptr && row->isVisible()) {
                        requireRowOpacity(row, expectedOpacity);
                    }
                }
            }
            const QStringList workflowNames{QStringLiteral("screenshot"), QStringLiteral("pinned"),
                                            QStringLiteral("global canvas"),
                                            QStringLiteral("recording")};
            for (std::size_t index = 0; index < workflows.size(); ++index) {
                capture(workflowNames.at(static_cast<qsizetype>(index)) +
                            QStringLiteral(" main toolbar"),
                        workflows.at(index)->mainPanel());
            }
            for (const auto& tool : tools) {
                workflows.front()->setActiveTool(tool.first);
                flushEvents();
                waitUntil([&] { return !controller.diagnostics().busy; });
                for (QWidget* row :
                     {workflows.front()->stylePanel(), workflows.front()->actionPanel()}) {
                    if (row != nullptr && row->isVisible()) {
                        requireRowOpacity(row, expectedOpacity);
                        capture(QString::fromLatin1(tool.second), row);
                    }
                }
            }
            for (const auto state : {ScreenshotToolPalette::RecordingState::Idle,
                                     ScreenshotToolPalette::RecordingState::Recording,
                                     ScreenshotToolPalette::RecordingState::Paused}) {
                workflows.back()->setRecordingState(state);
                flushEvents();
                requireRowOpacity(workflows.back()->mainPanel(), expectedOpacity);
                capture(QStringLiteral("recording main toolbar, state %1").arg(int(state)),
                        workflows.back()->mainPanel());
            }
            workflows.back()->setRecordingState(ScreenshotToolPalette::RecordingState::Idle);
            recordingExport->show();
            waitUntil([&] {
                return !controller.diagnostics().busy && controller.skinActive(recordingExport);
            });
            requireRowOpacity(recordingExport, expectedOpacity);
            capture(QStringLiteral("recording drawing toolbar"), workflows.back()->stylePanel());
            capture(QStringLiteral("recording export toolbar"), recordingExport);
            if (!snapshotDirectory.isEmpty()) {
                require(QDir().mkpath(snapshotDirectory), "create toolbar visual review directory");
                int width = 400;
                int height = 16;
                for (const auto& snapshot : snapshots) {
                    width = qMax(width, snapshot.second.width() + 32);
                    height += snapshot.second.height() + 32;
                }
                QImage montage(width, height, QImage::Format_ARGB32_Premultiplied);
                montage.fill(appearance == presentation::styles::ThemeAppearance::Dark
                                 ? QColor(QStringLiteral("#202020"))
                                 : QColor(QStringLiteral("#eeeeee")));
                QPainter montagePainter(&montage);
                montagePainter.setPen(appearance == presentation::styles::ThemeAppearance::Dark
                                          ? Qt::white
                                          : Qt::black);
                int y = 16;
                for (const auto& snapshot : snapshots) {
                    montagePainter.drawText(QPoint(16, y + 12), snapshot.first);
                    montagePainter.drawImage(QPoint(16, y + 20), snapshot.second);
                    y += snapshot.second.height() + 32;
                }
                montagePainter.end();
                const QString filename =
                    QStringLiteral("toolbar-%1-mask-%2.png")
                        .arg(appearance == presentation::styles::ThemeAppearance::Dark
                                 ? QStringLiteral("dark")
                                 : QStringLiteral("light"))
                        .arg(maskPercent);
                require(montage.save(QDir(snapshotDirectory).filePath(filename)),
                        "save toolbar visual review montage");
            }
        }
    }
    applicationThemes.setThemeAppearance(originalAppearance);
    require(configuration.setValue(QStringLiteral("interface/skin_mask_opacity"), 0),
            "restore the skin mask for lifecycle checks");
    waitUntil([&] { return controller.maskOpacity() == 0.0; });

    probes.hide();
    require(componentThemes.backgroundOpacity(&button) == 1.0,
            "hiding a toolbar must restore its child component backgrounds");
    probes.show();
    waitUntil([&] { return !controller.diagnostics().busy && controller.skinActive(&probes); });
    requireRowOpacity(&probes, 0.0);

    const QString invalidPath = temporary.filePath(QStringLiteral("invalid.png"));
    QFile invalid(invalidPath);
    require(invalid.open(QIODevice::WriteOnly) && invalid.write("invalid image") > 0,
            "write a failed toolbar skin fixture");
    invalid.close();
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), invalidPath),
            "replace toolbar image with an invalid source");
    require(
        componentThemes.backgroundOpacity(&button) ==
            (controller.skinActive(&probes) ? controller.maskOpacity() : 1.0),
        "loading a replacement skin must keep child backgrounds consistent with its visible frame");
    waitUntil([&] {
        return controller.hasError(presentation::SkinSurface::Toolbar) &&
               !controller.diagnostics().busy;
    });
    require(controller.hasError(presentation::SkinSurface::Toolbar) &&
                !controller.skinActive(&probes) &&
                componentThemes.backgroundOpacity(&button) == 1.0,
            "failed toolbar skins must leave normal component backgrounds restored");
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), toolbarPath),
            "recover the valid toolbar skin");
    waitUntil([&] { return !controller.diagnostics().busy && controller.skinActive(&probes); });
    requireRowOpacity(&probes, 0.0);
    require(configuration.setValue(QStringLiteral("interface/toolbar_skin_path"), QString()),
            "remove only the toolbar image");
    waitUntil([&] { return !controller.skinActive(&first); });
    require(controller.skinActive(&main) &&
                renderedCenterColor(first) == presentation::styles::ThemeManager::instance()
                                                  .themeColorScheme()
                                                  .map.colorBgContainer,
            "clearing toolbar skin must preserve the main skin and restore the normal toolbar");
    require(componentThemes.backgroundOpacity(&button) == 1.0 &&
                renderToolbarWidget(select).pixelColor(48, 16).alpha() == 255,
            "removing a toolbar skin must restore opaque descendant surfaces");
    require(configuration.setValue(QStringLiteral("interface/skin_path"), QString()),
            "clear isolated main-window fixture");
    flushEvents();
    require(storage.flushNow().success,
            "flush isolated skin settings before removing their directory");
}

} // namespace

namespace {
void printToolbarRoutingAndLayoutMigration() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    using Kind = snow_shot::storage::ScreenshotToolbarLayoutKind;
    const QString print = QStringLiteral("print");
    const QString quick = QStringLiteral("quick-save");
    const QString save = QStringLiteral("save-as-file");
    for (const auto kind : {Kind::ActionTools, Kind::PinnedActionTools}) {
        auto defaults = layout::defaultPositions(kind);
        require(defaults.contains(QStringList{print, quick, save}),
                "Print must join the default Save as File stack");
        snow_shot::storage::ScreenshotToolbarLayout old;
        old.positions = {{save, QStringLiteral("text-recognition")}};
        const auto upgraded = layout::normalizedLayout(old, kind);
        require(upgraded.positions[0] ==
                    QStringList{print, quick, save, QStringLiteral("text-recognition")},
                "migration must preserve previous item order and placement");
        require(layout::normalizedLayout(upgraded, kind) == upgraded,
                "migration must be idempotent");
        old.positions = {{print}, {save}};
        require(layout::normalizedLayout(old, kind).positions[0] == QStringList{print},
                "explicitly positioned Print must remain in its position");
        old.positions = {{save}};
        old.hidden = {print};
        const auto hiddenPrint = layout::normalizedLayout(old, kind);
        require(hiddenPrint.hidden.contains(print) && !hiddenPrint.positions[0].contains(print),
                "explicitly hidden Print must remain hidden");
        old.positions.clear();
        old.hidden = {save};
        require(layout::normalizedLayout(old, kind).hidden.contains(print),
                "migrated Print must inherit Save as File's hidden state");
        const auto stack = layout::stackPresentation(QStringList{print, quick, save},
                                                     [](const QString&) { return true; });
        require(stack.entryItemId() == save, "Save as File must remain the initial trigger");
    }
    for (int variant = 0; variant < 3; ++variant) {
        auto options = screenshotOptions();
        options.showSaveButton = true;
        options.saveButtonWithResultActions = variant != 2;
        options.actionToolsLayoutKind = variant == 2 ? Kind::PinnedActionTools : Kind::ActionTools;
        if (variant != 0)
            options.actionToolsLayout = layout::normalizedLayout({}, options.actionToolsLayoutKind);
        ScreenshotToolPalette palette(options);
        prepare(palette);
        int prints = 0;
        int saves = 0;
        QObject::connect(&palette, &ScreenshotToolPalette::printRequested, [&] { ++prints; });
        QObject::connect(&palette, &ScreenshotToolPalette::saveRequested, [&] { ++saves; });
        require(palette.activateScreenshotShortcut(print) && prints == 1 && saves == 0,
                "fixed and configurable print actions must emit only printRequested");
        auto* button =
            palette.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotPrintButton"));
        require(button != nullptr, "each result toolbar must provide a Print source button");
        button->click();
        require(prints == 2 && saves == 0, "Print source button must route once");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--print-only"))) {
        printToolbarRoutingAndLayoutMigration();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--skin-only"))) {
        toolbarSkinProfilesAndLifecycle();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--icon-centering-only"))) {
        smallToolbarIconStaysVerticallyCentered();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--spotlight-eviction-only"))) {
        spotlightConfigSurvivesStyleRowEviction();
        return 0;
    }
    smallToolbarIconStaysVerticallyCentered();
    customToolbarWidgetsKeepCallerMetricsDuringScaling();
    hiddenToolbarPeersKeepVisibleReferenceMetrics();
    drawingSelectsInheritScaleWhenMaterialized();
    toolbarSelectHeightSurvivesStyleRefresh();
    secondaryToolbarControlsFollowScale();
    toolbarControlsStayVerticallyCentered();
    historyButtonsFollowCanvasAvailability();
    toolbarSurfacesFollowThemeBackground();
    toolbarRowsShareShadowMetrics();
    toolbarSeparatorsKeepMinimumWidthAtCompactScale();
    secondaryToolbarUsesEqualHorizontalMargins();
    cachedToolbarIconsFollowThemeColors();
    recordingToolbarUsesTheScreenshotMainPanelContract();
    mainToolbarSpacingUsesReferenceItemMetrics();
    screenshotToolbarRendersShadowOutsideItsPanel();
    toolbarTooltipsUseApplicationBridge();
    cornerRadiusTextKeepsItsPhysicalSizeAcrossDpiChanges();
    visibleToolbarRowsDrivePaletteGeometry();
    styleToolbarButtonGroupLayoutRequestQuiesces();
    styleRadioIconsMatchTheirCurrentDevicePixelRatio();
    duplicateStyleStateDoesNotInvalidateToolbarGeometry();
    filterValuesDoNotRelayoutTheStableControlSet();
    spotlightConfigSurvivesStyleRowEviction();
    return 0;
}
