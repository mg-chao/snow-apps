#include "snow_shot/presentation/fullscreencanvaswindow.h"

#include "fullscreencanvaslaser.h"
#include "../pinned/pinnedwindowplatform.h"
#include "../tools/screenshottoolpalettestylecomponents.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/fullscreencanvasstylepanel.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "antd_icons.h"
#include "widgets/button.h"

#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {
namespace {
namespace custom_icons = snow_shot::presentation::icons::custom::outlined;
namespace antd = adqt::icons::antd::outlined;
constexpr int kMargin = 12;

class TransparentCanvas final : public SnowCanvasWidget {
  public:
    TransparentCanvas(SnowCanvasRuntime& runtime, FullscreenCanvasWindow& owner)
        : SnowCanvasWidget(runtime, &owner), m_owner(owner) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setClearBackgroundEnabled(false);
        setWheelZoomEnabled(false);
        setMouseTracking(true);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        // SourceOver with transparent paint cannot erase expired trails or moved shapes.
        {
            QPainter painter(this);
            painter.setClipRegion(event->region());
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(rect(), m_owner.inputSurfaceColor());
        }
        SnowCanvasWidget::paintEvent(event);
    }

  private:
    FullscreenCanvasWindow& m_owner;
};

struct ToolEntry {
    SnowCanvasTool tool;
    const char* id;
    const char* title;
    adqt::icons::IconRef icon;
};

std::array<ToolEntry, 12> toolEntries() {
    return {
        {{SnowCanvasTool::Select, "select", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Select"),
          custom_icons::Select()},
         {SnowCanvasTool::Shape, "shape", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Shape"),
          custom_icons::ToolRectangle()},
         {SnowCanvasTool::Arrow, "arrow", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Arrow"),
          custom_icons::ToolArrow()},
         {SnowCanvasTool::Line, "line", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Line"),
          custom_icons::ToolLine()},
         {SnowCanvasTool::FreeDraw, "pen", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen"),
          custom_icons::ToolFreeDraw()},
         {SnowCanvasTool::RectangleHighlight, "highlight",
          QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Rectangle highlight"),
          custom_icons::ToolHighlight()},
         {SnowCanvasTool::PenHighlight, "pen-highlight",
          QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Pen highlight"),
          custom_icons::ToolHighlight()},
         {SnowCanvasTool::Text, "text", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Text"),
          custom_icons::ToolText()},
         {SnowCanvasTool::SerialNumber, "number",
          QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Serial number"),
          custom_icons::ToolSerialNumber()},
         {SnowCanvasTool::Eraser, "eraser", QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Eraser"),
          custom_icons::ToolEraser()},
         {SnowCanvasTool::Watermark, "watermark",
          QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Watermark"), custom_icons::ToolWatermark()},
         {SnowCanvasTool::Spotlight, "spotlight",
          QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Spotlight"), custom_icons::ToolSpotlight()}}};
}
} // namespace

struct FullscreenCanvasWindow::Impl {
    FullscreenCanvasWindow& q;
    QPointer<QScreen> screen;
    std::unique_ptr<SnowCanvasRuntime> runtime;
    TransparentCanvas* canvas = nullptr;
    ScreenshotToolbarMainPanel* toolbar = nullptr;
    QScrollArea* toolsScroll = nullptr;
    QWidget* toolsBody = nullptr;
    FullscreenCanvasStylePanel* styles = nullptr;
    std::unique_ptr<FullscreenCanvasLaser> laser;
    std::unique_ptr<PinnedWindowPlatform> platform;
    std::unique_ptr<adqt::widgets::AdButton> recovery;
    std::unique_ptr<WindowShortcutManager> shortcuts;
    QMetaObject::Connection safeAreaConnection;
    QMap<QString, WindowShortcutManager::BindingHandle> drawingBindings;
    QMap<SnowCanvasTool, adqt::widgets::AdButton*> toolButtons;
    adqt::widgets::AdButton* laserButton = nullptr;
    adqt::widgets::AdButton* undoButton = nullptr;
    adqt::widgets::AdButton* redoButton = nullptr;
    bool transparent = false;
    bool laserSelected = false;
    bool closing = false;
    bool layingOut = false;
    bool transitioning = false;
    bool environmentUpdatePending = false;

    Impl(FullscreenCanvasWindow& owner, QScreen* target, const PlatformFactory& factory)
        : q(owner), screen(target) {
        const auto defaults = screenshotCanvasToolStyleDefaults();
        runtime = std::make_unique<SnowCanvasRuntime>(SnowCanvasRuntimeConfig{defaults});
        canvas = new TransparentCanvas(*runtime, q);
        canvas->setObjectName(QStringLiteral("fullscreenCanvas"));
        applyScreenshotCanvasToolStyles(*canvas, defaults);
        static_cast<void>(canvas->setCanvasTool(SnowCanvasTool::Select));
        static_cast<void>(
            runtime->setQuickSelectionDisabledTools(screenshotQuickSelectionDisabledTools(
                storage::DrawingSettings().quickSelectionDisabledTools())));
        styles = new FullscreenCanvasStylePanel(*canvas, defaults, &q);
        styles->setObjectName(QStringLiteral("fullscreenCanvasStylePanel"));
        laser = std::make_unique<FullscreenCanvasLaser>(*canvas);
        canvas->setCustomRenderer(laser.get());
        canvas->installEventFilter(&q);
        makeToolbar();
        platform = factory ? factory(&q)
                           : createPinnedWindowPlatform(&q, PinnedWindowPlatform::Role::Canvas);
        platform->environmentChanged = [this](bool) { scheduleEnvironmentUpdate(); };
        QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, &q, [this](QScreen* removed) {
            if (screen == removed) {
                screen = nullptr;
                scheduleEnvironmentUpdate();
            }
        });
        QObject::connect(styles, &FullscreenCanvasStylePanel::toolVariantRequested, &q,
                         &FullscreenCanvasWindow::activateTool);
        QObject::connect(
            styles, &FullscreenCanvasStylePanel::laserStyleChanged, &q,
            [this](const QColor& color, qreal width, int duration) {
                laser->setStyle(color, width, duration);
                auto& storage = storage::ApplicationStorage::instance();
                if (storage.isInitialized()) {
                    auto& config = storage.configuration();
                    static_cast<void>(
                        config.setValue(QStringLiteral("fullscreen_canvas/laser_color"),
                                        color.name(QColor::HexRgb)));
                    static_cast<void>(config.setValue(
                        QStringLiteral("fullscreen_canvas/laser_width"), qRound(width)));
                    static_cast<void>(config.setValue(
                        QStringLiteral("fullscreen_canvas/laser_duration_ms"), duration));
                }
            });
        QObject::connect(canvas, &SnowCanvasWidget::historyStateChanged, &q,
                         [this] { syncHistory(); });
        QObject::connect(canvas, &SnowCanvasWidget::activeToolChanged, &q, [this] { syncTools(); });
        auto& storage = storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            auto& config = storage.configuration();
            QColor color(config.value(QStringLiteral("fullscreen_canvas/laser_color")).toString());
            if (!color.isValid())
                color = Qt::red;
            const qreal width =
                config.value(QStringLiteral("fullscreen_canvas/laser_width")).toDouble(4);
            const int duration =
                config.value(QStringLiteral("fullscreen_canvas/laser_duration_ms")).toInt(1000);
            laser->setStyle(color, width, duration);
            styles->setLaserStyle(color, width, duration);
            QObject::connect(
                &config, &storage::ConfigurationStore::valueChanged, &q,
                [this](const QString& key, const QJsonValue&) {
                    if (key.startsWith(QStringLiteral("drawing_shortcuts/")))
                        reloadShortcuts();
                    if (key == QStringLiteral("drawing/quick_selection_disabled_tools"))
                        static_cast<void>(runtime->setQuickSelectionDisabledTools(
                            screenshotQuickSelectionDisabledTools(
                                storage::DrawingSettings().quickSelectionDisabledTools())));
                });
        }
        makeShortcuts();
        syncTools();
        syncHistory();
    }

    ~Impl() {
        platform->environmentChanged = {};
        if (q.internalWinId())
            static_cast<void>(platform->setInputTransparent(false));
        if (recovery && recovery->windowHandle())
            recovery->windowHandle()->setTransientParent(nullptr);
        recovery.reset();
        canvas->setCustomRenderer(nullptr);
        laser.reset();
        shortcuts.reset();
        // Canvas viewports must die before their borrowed runtime.
        delete styles;
        delete canvas;
    }

    void makeToolbar() {
        toolbar = new ScreenshotToolbarMainPanel({}, &q);
        toolbar->setObjectName(QStringLiteral("fullscreenCanvasToolbar"));
        toolbar->contentLayout()->setSizeConstraint(QLayout::SetNoConstraint);
        toolsScroll = new QScrollArea(toolbar);
        toolsScroll->setObjectName(QStringLiteral("fullscreenCanvasToolStrip"));
        toolsScroll->setFrameShape(QFrame::NoFrame);
        toolsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        toolsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        toolsScroll->setWidgetResizable(false);
        toolsScroll->setStyleSheet(
            QStringLiteral("QScrollArea { background: transparent; border: 0; }"));
        toolsScroll->viewport()->setAutoFillBackground(false);
        toolsBody = new QWidget;
        auto* layout = new QHBoxLayout(toolsBody);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        for (const auto& entry : toolEntries()) {
            auto* button = toolbar->createToolButton(entry.title, entry.icon);
            button->setObjectName(QStringLiteral("fullscreenCanvasTool-") +
                                  QString::fromLatin1(entry.id));
            layout->addWidget(button);
            toolButtons.insert(entry.tool, button);
            QObject::connect(button, &adqt::widgets::AdButton::clicked, &q,
                             [this, tool = entry.tool] { q.activateTool(tool); });
        }
        laserButton =
            toolbar->createToolButton(QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Laser pointer"),
                                      custom_icons::LaserPointer());
        laserButton->setObjectName(QStringLiteral("fullscreenCanvasTool-laser"));
        layout->addWidget(laserButton);
        QObject::connect(laserButton, &adqt::widgets::AdButton::clicked, &q,
                         &FullscreenCanvasWindow::activateLaser);
        toolsBody->setFixedSize(layout->sizeHint());
        toolsScroll->setWidget(toolsBody);
        toolbar->contentLayout()->addWidget(toolsScroll, 1);
        toolbar->addSeparator();
        const auto action = [this](const char* label, const adqt::icons::IconRef& icon,
                                   const char* id, auto callback) {
            auto* button = toolbar->createActionButton(label, icon);
            button->setObjectName(QStringLiteral("fullscreenCanvasAction-") +
                                  QString::fromLatin1(id));
            toolbar->contentLayout()->addWidget(button);
            QObject::connect(button, &adqt::widgets::AdButton::clicked, &q, callback);
            return button;
        };
        undoButton = action(QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Undo"), antd::Undo(),
                            "undo", [this] { static_cast<void>(canvas->undo()); });
        redoButton = action(QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Redo"), antd::Redo(),
                            "redo", [this] { static_cast<void>(canvas->redo()); });
        action(QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Clear canvas"), custom_icons::Delete(),
               "clear", [this] { q.clearCanvas(); });
        action(QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Close"), custom_icons::Exit(), "close",
               [this] { q.close(); });
        action(QT_TRANSLATE_NOOP("ScreenshotToolPalette", "Click-through"), custom_icons::Mouse(),
               "click-through", [this] { static_cast<void>(q.setClickThrough(true)); });
    }

    void syncHistory() {
        const auto history = canvas->canvasHistoryState();
        undoButton->setEnabled(history.canUndo);
        redoButton->setEnabled(history.canRedo);
    }

    void syncTools() {
        for (auto it = toolButtons.cbegin(); it != toolButtons.cend(); ++it)
            setScreenshotToolPaletteStyleButtonActive(
                it.value(), !laserSelected && canvas->canvasTool() == it.key());
        setScreenshotToolPaletteStyleButtonActive(laserButton, laserSelected);
        styles->setActiveTool(canvas->canvasTool());
        styles->setLaserActive(laserSelected);
    }

    void makeShortcuts() {
        shortcuts = std::make_unique<WindowShortcutManager>();
        shortcuts->addScopeWindow(&q);
        const auto historyShortcut = [this](const QString& id, QKeySequence::StandardKey key,
                                            const std::function<bool()>& action) {
            WindowShortcutManager::Binding binding;
            binding.id = id;
            for (const auto& sequence : QKeySequence::keyBindings(key)) {
                if (sequence.count() == 1)
                    binding.keyCombinations.push_back(sequence[0]);
            }
            binding.priority = WindowShortcutManager::StandardPriority::WindowCommand;
            binding.canActivate = [this](const auto& context) {
                return !transparent && !closing && !canvas->hasActiveTextEditing() &&
                       !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget);
            };
            binding.activate = [action](const auto&) { return action(); };
            static_cast<void>(shortcuts->addBinding(&q, std::move(binding)));
        };
        historyShortcut(QStringLiteral("fullscreen.undo"), QKeySequence::Undo,
                        [this] { return canvas->undo(); });
        historyShortcut(QStringLiteral("fullscreen.redo"), QKeySequence::Redo,
                        [this] { return canvas->redo(); });
        const QMap<QString, SnowCanvasTool> tools{
            {QStringLiteral("select"), SnowCanvasTool::Select},
            {QStringLiteral("shape"), SnowCanvasTool::Shape},
            {QStringLiteral("arrow"), SnowCanvasTool::Arrow},
            {QStringLiteral("brush"), SnowCanvasTool::FreeDraw},
            {QStringLiteral("highlight"), SnowCanvasTool::RectangleHighlight},
            {QStringLiteral("text"), SnowCanvasTool::Text},
            {QStringLiteral("serial_number"), SnowCanvasTool::SerialNumber},
            {QStringLiteral("eraser"), SnowCanvasTool::Eraser},
            {QStringLiteral("watermark"), SnowCanvasTool::Watermark}};
        for (auto it = tools.cbegin(); it != tools.cend(); ++it) {
            WindowShortcutManager::Binding binding;
            binding.id = QStringLiteral("fullscreen.drawing.") + it.key();
            binding.shortcutBindings = storage::DrawingShortcutSettings().shortcuts(it.key());
            binding.priority = WindowShortcutManager::StandardPriority::DrawingShortcut;
            binding.canActivate = [this](const auto& context) {
                return !transparent && !closing && !canvas->hasActiveTextEditing() &&
                       !WindowShortcutManager::focusAcceptsTextInput(context.focusWidget);
            };
            binding.activate = [this, tool = it.value()](const auto&) {
                q.activateTool(tool);
                return true;
            };
            drawingBindings.insert(it.key(), shortcuts->addBinding(&q, std::move(binding)));
        }
    }

    void reloadShortcuts() {
        for (auto it = drawingBindings.cbegin(); it != drawingBindings.cend(); ++it)
            static_cast<void>(shortcuts->setShortcuts(
                it.value(), storage::DrawingShortcutSettings().shortcuts(it.key())));
    }

    void layout() {
        if (layingOut)
            return;
        layingOut = true;
        canvas->setGeometry(q.rect());
        static_cast<void>(canvas->setViewportCamera(q.width() / 2.0, q.height() / 2.0, 1.0));
        QRect usable = q.rect();
        if (screen) {
            const QRect screenUsable = pinnedDisplayGeometry(*screen).usableBounds.toRect();
            const QRect local(q.mapFromGlobal(screenUsable.topLeft()), screenUsable.size());
            usable = usable.intersected(local);
        }
        if (usable.isEmpty())
            usable = q.rect();
        const int availableWidth = std::max(1, usable.width() - 2 * kMargin);
        const int actionsWidth = 5 * toolbar->buttonSize() + 49;
        const int width = std::min(availableWidth, toolsBody->width() + actionsWidth);
        const bool scrolling = width < toolsBody->width() + actionsWidth;
        const int height =
            toolbar->buttonSize() + 8 +
            (scrolling ? toolsScroll->horizontalScrollBar()->sizeHint().height() : 0);
        toolsScroll->setMinimumWidth(0);
        toolsScroll->setFixedHeight(height - 8);
        toolbar->setGeometry(usable.center().x() - width / 2, usable.top() + kMargin, width,
                             height);
        const int styleTop = toolbar->geometry().bottom() + kMargin + 1;
        styles->setGeometry(usable.left() + kMargin, styleTop, std::min(260, availableWidth),
                            std::max(1, usable.bottom() - styleTop - kMargin + 1));
        toolbar->raise();
        styles->raise();
        if (recovery)
            placeRecovery();
        layingOut = false;
    }

    void scheduleEnvironmentUpdate() {
        if (environmentUpdatePending || closing)
            return;
        environmentUpdatePending = true;
        QTimer::singleShot(0, &q, [this] {
            environmentUpdatePending = false;
            if (closing)
                return;
            if (!screen || !QGuiApplication::screens().contains(screen))
                screen = QGuiApplication::primaryScreen();
            if (!screen) {
                q.close();
                return;
            }
            q.setScreen(screen);
            q.setGeometry(screen->geometry());
            layout();
            if (transparent && !ensureRecovery())
                static_cast<void>(q.setClickThrough(false));
            canvas->update();
        });
    }

    void retranslate() {
        q.setWindowTitle(FullscreenCanvasWindow::tr("Full-screen canvas"));
        if (recovery) {
            recovery->setText(FullscreenCanvasWindow::tr("Resume drawing"));
            recovery->setAccessibleName(FullscreenCanvasWindow::tr("Resume drawing"));
            recovery->setToolTip(
                FullscreenCanvasWindow::tr("Turn off click-through and resume drawing"));
        }
    }

    bool ensureRecovery() {
        if (!recovery) {
            recovery = std::make_unique<adqt::widgets::AdButton>();
            recovery->setObjectName(QStringLiteral("fullscreenCanvasRecoveryButton"));
            recovery->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                                     Qt::WindowDoesNotAcceptFocus);
            recovery->setAttribute(Qt::WA_ShowWithoutActivating);
            recovery->setAttribute(Qt::WA_TranslucentBackground);
            recovery->setFocusPolicy(Qt::NoFocus);
            recovery->setCursor(Qt::PointingHandCursor);
            recovery->installEventFilter(&q);
            configurePinnedAuxiliary(recovery.get());
            QObject::connect(recovery.get(), &adqt::widgets::AdButton::clicked, &q,
                             [this] { static_cast<void>(q.setClickThrough(false)); });
            retranslate();
        }
        recovery->winId();
        if (!recovery->windowHandle() || !q.windowHandle() || !screen)
            return false;
        if (!configurePinnedAuxiliary(recovery.get())->attach())
            return false;
        recovery->windowHandle()->setTransientParent(q.windowHandle());
        placeRecovery();
        recovery->setWindowOpacity(0.65);
        recovery->show();
        recovery->raise();
        return recovery->isVisible();
    }

    void placeRecovery() {
        if (!screen || !recovery)
            return;
        const QRect bounds = pinnedDisplayGeometry(*screen).usableBounds.toRect();
        recovery->setScreen(screen);
        if (recovery->windowHandle() && q.windowHandle())
            recovery->windowHandle()->setTransientParent(q.windowHandle());
        const int width = std::min(256, bounds.width());
        recovery->setGeometry(bounds.center().x() - width / 2, bounds.top(), width, 32);
    }

    void applyInputState() {
        q.setAttribute(Qt::WA_TransparentForMouseEvents, transparent);
        q.setAttribute(Qt::WA_ShowWithoutActivating, transparent);
        toolbar->setVisible(!transparent);
        styles->setVisible(!transparent);
        canvas->setInteractionEnabled(!transparent && !laserSelected);
        laser->setActive(!transparent && laserSelected);
        if (laserSelected && !transparent)
            canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::CrossCursor));
        else
            canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
        canvas->update();
    }
};

FullscreenCanvasWindow::FullscreenCanvasWindow(QScreen* screen, PlatformFactory platformFactory)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("fullscreenCanvasWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    m_impl = std::make_unique<Impl>(*this, screen, platformFactory);
    if (screen) {
        setScreen(screen);
        setGeometry(screen->geometry());
    }
    m_impl->retranslate();
    m_impl->layout();
}

FullscreenCanvasWindow::~FullscreenCanvasWindow() = default;

bool FullscreenCanvasWindow::present() {
    if (!m_impl->runtime->isValid()) {
        emit operationFailed(tr("Could not initialize the full-screen canvas."), true);
        return false;
    }
    winId();
    QObject::disconnect(m_impl->safeAreaConnection);
    m_impl->safeAreaConnection = connect(windowHandle(), &QWindow::safeAreaMarginsChanged, this,
                                         [this] { m_impl->scheduleEnvironmentUpdate(); });
    if (!m_impl->platform->attach()) {
        emit operationFailed(tr("Could not initialize the canvas window."), true);
        return false;
    }
    show();
    m_impl->layout();
    raise();
    static_cast<void>(m_impl->platform->activate());
    m_impl->canvas->setFocus(Qt::OtherFocusReason);
    return true;
}

bool FullscreenCanvasWindow::clickThrough() const {
    return m_impl->transparent;
}
bool FullscreenCanvasWindow::laserActive() const {
    return m_impl->laserSelected;
}
SnowCanvasWidget* FullscreenCanvasWindow::canvas() const {
    return m_impl->canvas;
}
ScreenshotToolbarMainPanel* FullscreenCanvasWindow::toolbar() const {
    return m_impl->toolbar;
}
FullscreenCanvasStylePanel* FullscreenCanvasWindow::stylePanel() const {
    return m_impl->styles;
}
QWidget* FullscreenCanvasWindow::recoveryButton() const {
    return m_impl->recovery.get();
}
QColor FullscreenCanvasWindow::inputSurfaceColor() const {
    return m_impl && m_impl->transparent ? QColor(Qt::transparent) : QColor(0, 0, 0, 2);
}

bool FullscreenCanvasWindow::setClickThrough(bool enabled) {
    auto& d = *m_impl;
    if (d.closing || d.transitioning || !isVisible())
        return false;
    if (enabled == d.transparent)
        return true;
    d.transitioning = true;
    const auto fail = [this, &d] {
        d.transitioning = false;
        emit operationFailed(tr("Could not change click-through mode."), true);
        return false;
    };
    if (enabled) {
        d.styles->dismissPopups();
        static_cast<void>(d.canvas->resetEditingStatePreservingTool());
        d.laser->clear();
        if (QWidget* grabber = QWidget::mouseGrabber();
            grabber && (grabber == this || isAncestorOf(grabber)))
            grabber->releaseMouse();
        if (!d.ensureRecovery())
            return fail();
        if (!d.platform->setInputTransparent(true)) {
            // A native failure can occur after partially changing the window. Never
            // remove the only interactive recovery surface unless rollback succeeds.
            d.transparent = !d.platform->setInputTransparent(false);
            d.applyInputState();
            if (!d.transparent)
                d.recovery->hide();
            return fail();
        }
        d.transparent = true;
        d.applyInputState();
        d.canvas->clearFocus();
    } else {
        if (!d.platform->setInputTransparent(false))
            return fail();
        d.transparent = false;
        d.applyInputState();
        d.recovery->hide();
        raise();
        static_cast<void>(d.platform->activate());
        d.canvas->setFocus(Qt::OtherFocusReason);
    }
    d.transitioning = false;
    return true;
}

void FullscreenCanvasWindow::activateTool(SnowCanvasTool tool) {
    auto& d = *m_impl;
    if (d.transparent || d.closing || !d.toolButtons.contains(tool))
        return;
    d.styles->dismissPopups();
    d.laserSelected = false;
    d.laser->setActive(false);
    d.canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
    d.canvas->setInteractionEnabled(true);
    static_cast<void>(d.canvas->setCanvasTool(tool));
    d.syncTools();
    d.canvas->setFocus(Qt::OtherFocusReason);
}

void FullscreenCanvasWindow::activateLaser() {
    auto& d = *m_impl;
    if (d.transparent || d.closing)
        return;
    d.styles->dismissPopups();
    static_cast<void>(d.canvas->resetEditingStatePreservingTool());
    d.laserSelected = true;
    d.applyInputState();
    d.syncTools();
    d.canvas->setFocus(Qt::OtherFocusReason);
}

void FullscreenCanvasWindow::clearCanvas() {
    m_impl->laser->clear();
    static_cast<void>(m_impl->canvas->deleteAllElements());
}

bool FullscreenCanvasWindow::eventFilter(QObject* watched, QEvent* event) {
    if (!m_impl)
        return QWidget::eventFilter(watched, event);
    if (watched == m_impl->recovery.get()) {
        if (event->type() == QEvent::Enter)
            m_impl->recovery->setWindowOpacity(1.0);
        else if (event->type() == QEvent::Leave)
            m_impl->recovery->setWindowOpacity(0.65);
    }
    if (watched == m_impl->canvas && event->type() == QEvent::Wheel)
        return true;
    if (watched == m_impl->canvas && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && !m_impl->canvas->hasActiveTextEditing()) {
            m_impl->laser->clear();
            static_cast<void>(m_impl->canvas->resetEditingStatePreservingTool());
            return true;
        }
        if (key->key() == Qt::Key_Space && !m_impl->canvas->hasActiveTextEditing())
            return true;
    }
    if (watched == m_impl->canvas &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease) &&
        static_cast<QMouseEvent*>(event)->button() == Qt::MiddleButton)
        return true;
    return QWidget::eventFilter(watched, event);
}

bool FullscreenCanvasWindow::nativeEvent(const QByteArray& eventType, void* message,
                                         qintptr* result) {
#ifdef Q_OS_WIN
    if (m_impl && m_impl->transparent) {
        const auto* native = static_cast<MSG*>(message);
        if (native->message == WM_NCHITTEST) {
            *result = HTTRANSPARENT;
            return true;
        }
        if (native->message == WM_MOUSEACTIVATE) {
            *result = MA_NOACTIVATE;
            return true;
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void FullscreenCanvasWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_impl)
        m_impl->layout();
}

void FullscreenCanvasWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (m_impl && event->type() == QEvent::LanguageChange) {
        m_impl->retranslate();
        m_impl->layout();
    }
}

void FullscreenCanvasWindow::closeEvent(QCloseEvent* event) {
    auto& d = *m_impl;
    d.closing = true;
    d.styles->dismissPopups();
    d.laser->setActive(false);
    static_cast<void>(d.platform->setInputTransparent(false));
    if (d.recovery) {
        d.recovery->hide();
        if (d.recovery->windowHandle())
            d.recovery->windowHandle()->setTransientParent(nullptr);
    }
    event->accept();
    emit closed();
}
} // namespace snow_shot::presentation
