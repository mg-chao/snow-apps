#include "snow_shot/presentation/floatingtoolbarcontroller.h"

#include "snow_shot/presentation/floatingtoolbarplacement.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/platform/focusedfullscreenwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/floatingtoolbarsettings.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "widgets/popover.h"
#include "../tools/screenshottoolpalettebuttons.h"
#include "widgets/control_scale.h"
#include "antd_icons.h"

#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include "snow_shot/presentation/screenshotcontentdrop.h"
#include <QGraphicsDropShadowEffect>
#include <QHideEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_MACOS
#include "snow_shot/platform/screenshotnative.h"
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#endif

namespace snow_shot::presentation {
namespace {
QString text(const char* source) {
    return QCoreApplication::translate("FloatingToolbar", source);
}
constexpr auto kKind = storage::ScreenshotToolbarLayoutKind::FloatingTools;

class ToolbarShadow final : public QGraphicsDropShadowEffect {
  public:
    explicit ToolbarShadow(const QGraphicsDropShadowEffect& source, QObject* parent)
        : QGraphicsDropShadowEffect(parent) {
        setBlurRadius(source.blurRadius());
        setOffset(source.offset());
        setColor(source.color());
    }
    void setVisibleMargins(QMargins margins) {
        if (margins == m_visibleMargins)
            return;
        m_visibleMargins = margins;
        updateBoundingRect();
    }
    QRectF boundingRectFor(const QRectF& rect) const override {
        // Keep Qt's native dirty region inside the allocated shadow surface.
        return QGraphicsDropShadowEffect::boundingRectFor(rect).intersected(
            rect.marginsAdded(QMarginsF(m_visibleMargins)));
    }

  private:
    QMargins m_visibleMargins = ScreenshotToolbarMainPanel::shadowMargins();
};

class SnowflakeButton final : public QAbstractButton {
  public:
    explicit SnowflakeButton(QWidget* parent) : QAbstractButton(parent) {
        setObjectName(QStringLiteral("floatingToolbarSnowflake"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
    }
    void setDropReady(bool ready) {
        if (m_dropReady == ready)
            return;
        m_dropReady = ready;
        update();
    }
    void hideEvent(QHideEvent* event) override {
        setDropReady(false);
        QAbstractButton::hideEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        const auto& colors = styles::ThemeManager::instance().themeColorScheme().map;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(width() / 48.0, height() / 48.0);
        p.setPen(QPen(hasFocus() ? colors.colorPrimary : colors.colorBorderSecondary, 1));
        p.setBrush(isDown() ? colors.colorFillSecondary : colors.colorBgContainer);
        p.drawRoundedRect(QRectF(0.5, 0.5, 47, 47), 12, 12);
        if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(colors.colorFillQuaternary);
            p.drawRoundedRect(QRectF(1, 1, 46, 46), 12, 12);
        }
        if (m_dropReady) {
            QColor glow = colors.colorSuccess;
            glow.setAlphaF(0.14F);
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            p.drawRoundedRect(QRectF(1, 1, 46, 46), 12, 12);
            QColor ring = colors.colorSuccess;
            ring.setAlphaF(0.7F);
            p.setPen(QPen(ring, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(2.5, 2.5, 43, 43), 10, 10);
        }
        p.translate(24, 24);
        if (m_dropReady)
            p.scale(1.08, 1.08);
        p.setPen(QPen(m_dropReady ? colors.colorSuccess : colors.colorPrimary, 1.8, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        for (int arm = 0; arm < 6; ++arm) {
            p.drawLine(QPointF(0, 0), QPointF(0, -14));
            p.drawLine(QPointF(0, -8.5), QPointF(-4, -11));
            p.drawLine(QPointF(0, -8.5), QPointF(4, -11));
            p.rotate(60);
        }
    }

  private:
    bool m_dropReady = false;
};

class DropSurface final : public QWidget {
  public:
    std::function<void(ScreenshotClipboardContentSnapshot, QStringList)> dropped;
    std::function<void()> hoverChanged;
    std::function<void()> dropReadyChanged;
    explicit DropSurface()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_AlwaysShowToolTips);
#ifdef Q_OS_MACOS
        setAttribute(Qt::WA_MacAlwaysShowToolWindow);
        platform::configureControlledWindowDragging(this);
#endif
        setAcceptDrops(true);
    }
    bool isHovered() const {
        return m_hovered;
    }
    bool isDropReady() const {
        return m_dropReady;
    }
    void enterEvent(QEnterEvent* event) override {
        setHovered(true);
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override {
        setHovered(false);
        QWidget::leaveEvent(event);
    }
    void showEvent(QShowEvent* event) override {
        setHovered(geometry().contains(QCursor::pos()));
        QWidget::showEvent(event);
    }
    void hideEvent(QHideEvent* event) override {
        setDropReady(false);
        setHovered(false);
        QWidget::hideEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent* event) override {
        updateDropReady(event);
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        updateDropReady(event);
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override {
        setDropReady(false);
        event->accept();
    }
    void dropEvent(QDropEvent* event) override {
        setDropReady(false);
        if (!event->possibleActions().testFlag(Qt::CopyAction) ||
            !acceptsScreenshotDrop(event->mimeData()) || !dropped)
            return;
        const auto paths = screenshotDropFilePaths(event->mimeData());
        if (!paths.isEmpty()) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            dropped({}, paths);
            return;
        }
        auto snapshot = ScreenshotClipboardContentReader::snapshotMimeData(
            event->mimeData(), devicePixelRatioF(), palette().color(QPalette::Base));
        if (snapshot && snapshot->isValid()) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            dropped(std::move(*snapshot), {});
        }
    }

  private:
    void updateDropReady(QDropEvent* event) {
        const bool ready = event->possibleActions().testFlag(Qt::CopyAction) && dropped &&
                           acceptsScreenshotDrop(event->mimeData());
        setDropReady(ready);
        if (ready) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
    }
    void setDropReady(bool ready) {
        if (m_dropReady == ready)
            return;
        m_dropReady = ready;
        if (dropReadyChanged)
            dropReadyChanged();
    }
    void setHovered(bool hovered) {
        m_hovered = hovered;
        if (hoverChanged)
            hoverChanged();
    }
    bool m_hovered = false;
    bool m_dropReady = false;
};
} // namespace

class FloatingToolbarController::Impl {
  public:
    Impl(FloatingToolbarController& owner, FullscreenDetector source)
        : q(owner),
          detector(source ? std::move(source)
                          : FullscreenDetector(platform::focusedFullscreenWindowOnScreen)) {
        reveal.setParent(&q);
        retreat.setParent(&q);
        fullscreen.setParent(&q);
        reveal.setObjectName(QStringLiteral("floatingToolbarRevealTimer"));
        retreat.setObjectName(QStringLiteral("floatingToolbarRetreatTimer"));
        fullscreen.setObjectName(QStringLiteral("floatingToolbarFullscreenTimer"));
        reveal.setSingleShot(true);
        reveal.setInterval(100);
        retreat.setSingleShot(true);
        retreat.setInterval(200);
        fullscreen.setInterval(2000);
        fullscreen.setTimerType(Qt::PreciseTimer);
        QObject::connect(&reveal, &QTimer::timeout, &q, [this] { showToolbar(); });
        QObject::connect(&retreat, &QTimer::timeout, &q, [this] {
            if (menuDepth || dragging || dropReady() || containsCursor())
                return;
            expanded = false;
            place();
        });
        QObject::connect(&fullscreen, &QTimer::timeout, &q, [this] {
            if (menuDepth || dragging)
                return;
            const bool hidden = screen && detector(screen);
            if (hidden != fullscreenHidden) {
                fullscreenHidden = hidden;
                syncVisibility();
            }
        });
        QObject::connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, &q,
                         [this] { queueRefresh(false); });
        auto& app = storage::ApplicationStorage::instance();
        QObject::connect(&app, &storage::ApplicationStorage::storageStatusChanged, &q,
                         [this] { bindConfiguration(); });
        QObject::connect(qApp, &QGuiApplication::screenAdded, &q, [this](QScreen* added) {
            observeScreen(added);
            restorePlacement();
        });
        QObject::connect(qApp, &QGuiApplication::screenRemoved, &q, [this] { restorePlacement(); });
        for (auto* display : QGuiApplication::screens())
            observeScreen(display);
        bindConfiguration();
    }
    void observeScreen(QScreen* display) {
        QObject::connect(display, &QScreen::availableGeometryChanged, &q,
                         [this] { restorePlacement(); });
        QObject::connect(display, &QScreen::geometryChanged, &q, [this] { restorePlacement(); });
        QObject::connect(display, &QScreen::logicalDotsPerInchChanged, &q,
                         [this] { queueRefresh(); });
    }
    void bindConfiguration() {
        auto& app = storage::ApplicationStorage::instance();
        if (!app.isInitialized())
            return;
        auto* next = &app.configuration();
        if (configuration == next)
            return;
        if (configuration)
            QObject::disconnect(configuration, nullptr, &q, nullptr);
        configuration = next;
        QObject::connect(next, &storage::ConfigurationStore::valueChanged, &q,
                         [this](const QString& key) {
                             if (key == QStringLiteral("floating_toolbar/placement")) {
                                 if (!saving)
                                     restorePlacement();
                             } else if (key.startsWith(QLatin1String("floating_toolbar/")) ||
                                        key == QStringLiteral("screenshot/delay_seconds") ||
                                        key == QStringLiteral("screenshot_ui/toolbar_size"))
                                 queueRefresh(key == QStringLiteral("floating_toolbar/layout") ||
                                              key == QStringLiteral("screenshot/delay_seconds") ||
                                              key == QStringLiteral("screenshot_ui/toolbar_size"));
                         });
        restorePlacement();
        queueRefresh();
    }
    void queueRefresh(bool updateContent = true) {
        contentDirty |= updateContent;
        if (refreshQueued || stopped)
            return;
        refreshQueued = true;
        QTimer::singleShot(0, &q, [this] {
            refreshQueued = false;
            refresh();
        });
    }
    int iconSize() const {
        return qRound(48 * scale);
    }
    QSize iconExtent() const {
        return QSize(iconSize(), iconSize());
    }
    QSize anchorExtent(bool mode) const {
        const int size = mode ? qRound(32 * scale) : iconSize();
        return QSize(size, size);
    }
    QRect bounds() const {
        return screen ? screen->availableGeometry() : QRect();
    }
    bool right() const {
        return dragging ? dragRight
                        : floating_toolbar::rightSide(anchor, anchorExtent(toolbarMode), bounds());
    }
    bool visibleAllowed() const {
        return !stopped && enabled && screen && !fullscreenHidden &&
               !(hideCapture && !activities.isEmpty());
    }
    static QString screenIdentity(const QScreen* display) {
        // Friendly monitor names are not unique; EDID serials distinguish
        // otherwise identical displays without persisting native handles.
        return display->serialNumber().isEmpty()
                   ? display->name()
                   : display->name() + QLatin1Char('|') + display->serialNumber();
    }
    void restorePlacement() {
        if (!configuration)
            return;
        const auto saved = storage::FloatingToolbarSettings().placement();
        screen = nullptr;
        for (auto* display : QGuiApplication::screens())
            if (screenIdentity(display) == saved.value(QStringLiteral("screen")).toString())
                screen = display;
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (!screen)
            return;
        anchor = floating_toolbar::restore(
            {saved.value(QStringLiteral("x")).toDouble(1),
             saved.value(QStringLiteral("y")).toDouble(0.97)},
            anchorExtent(storage::FloatingToolbarSettings().toolbarMode()), bounds());
        if (!storage::FloatingToolbarSettings().toolbarMode())
            anchor = floating_toolbar::dock(anchor, iconExtent(), bounds());
        if (fullscreen.isActive())
            fullscreenHidden = detector(screen);
        place();
    }
    void savePlacement() {
        if (!screen || !configuration)
            return;
        const auto fraction =
            floating_toolbar::remember(anchor, anchorExtent(toolbarMode), bounds());
        saving = true;
        storage::FloatingToolbarSettings().setPlacement(
            {{QStringLiteral("screen"), screenIdentity(screen)},
             {QStringLiteral("x"), fraction.x()},
             {QStringLiteral("y"), fraction.y()}});
        saving = false;
    }
    std::unique_ptr<DropSurface> surface(const QString& name) {
        auto result = std::make_unique<DropSurface>();
        result->setObjectName(name);
        result->setWindowOpacity(idleOpacity);
        result->hoverChanged = [this] { syncOpacity(); };
        result->dropReadyChanged = [this] {
            if (icon)
                icon->setDropReady(!toolbarMode && dropReady());
            if (toolbarIcon)
                toolbarIcon->setDropReady(toolbarMode && dropReady());
            syncOpacity();
        };
        result->installEventFilter(&q);
        result->dropped = [this](auto snapshot, auto paths) {
            emit q.contentDropped(std::move(snapshot), std::move(paths));
        };
        return result;
    }
    SnowflakeButton* snowflake(QWidget* parent) {
        auto* button = new SnowflakeButton(parent);
        button->setFixedSize(iconExtent());
        button->installEventFilter(&q);
        return button;
    }
    void ensureIcon() {
        if (iconWindow)
            return;
        iconWindow = surface(QStringLiteral("floatingToolbarIconWindow"));
        icon = snowflake(iconWindow.get());
#ifdef Q_OS_MACOS
        QObject::connect(icon, &QAbstractButton::clicked, &q, [this] { showToolbar(); });
#else
        QObject::connect(icon, &QAbstractButton::clicked, &q,
                         [this] { invoke(QStringLiteral("screenshot")); });
#endif
        icon->move(8, 8);
        auto* shadow = new QGraphicsDropShadowEffect(icon);
        shadow->setBlurRadius(14);
        shadow->setOffset(0, 2);
        shadow->setColor(QColor(0, 0, 0, 65));
        icon->setGraphicsEffect(shadow);
    }
    void ensureToolbar() {
        if (toolbarWindow)
            return;
        toolbarWindow = surface(QStringLiteral("floatingToolbarWindow"));
        auto* root = new QVBoxLayout(toolbarWindow.get());
        root->setContentsMargins(ScreenshotToolbarMainPanel::shadowMargins());
        panel = new ScreenshotToolbarMainPanel({false}, toolbarWindow.get());
        if (auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(panel->graphicsEffect()))
            panel->setGraphicsEffect(new ToolbarShadow(*shadow, panel));
        root->addWidget(panel);
        toolbarIcon = snowflake(panel);
        QObject::connect(toolbarIcon, &QAbstractButton::clicked, &q,
                         [this] { invoke(QStringLiteral("screenshot")); });
        scaleScope = new adqt::widgets::AdControlScaleScope(panel, panel);
        rebuild();
    }
    void showToolbar() {
        if (toolbarMode || dragging || pressed || !visibleAllowed())
            return;
        reveal.stop();
        retreat.stop();
        expanded = true;
        ensureToolbar();
        place();
    }
    QString label(const QString& id) const {
        for (const auto& item : toolbar_layout::editorDescriptors(kKind))
            if (id == QLatin1String(item.id))
                return QCoreApplication::translate(item.translationContext, item.label)
                    .replace(QStringLiteral("%1"),
                             QString::number(storage::ScreenshotSettings().delaySeconds()));
        return {};
    }
    adqt::icons::IconRef toolIcon(const QString& id) const {
        for (const auto& item : toolbar_layout::editorDescriptors(kKind))
            if (id == QLatin1String(item.id))
                return toolbar_layout::icon(item.icon);
        return {};
    }
    void invoke(const QString& id) {
        expanded = false;
        reveal.stop();
        retreat.stop();
        closeMenus();
        place();
        emit q.actionRequested(id);
    }
    adqt::widgets::AdContextMenu* makeMenu(QWidget* parent) {
        auto* menu = new adqt::widgets::AdContextMenu(parent);
        menu->setDeleteOnHide();
        menus.removeIf([](const auto& entry) { return entry.isNull(); });
        menus.push_back(menu);
        QObject::connect(menu, &QMenu::aboutToShow, &q, [this] {
            ++menuDepth;
            syncOpacity();
            retreat.stop();
            place();
        });
        QObject::connect(menu, &QMenu::aboutToHide, &q, [this] {
            menuDepth = std::max(0, menuDepth - 1);
            syncOpacity();
            retreat.start();
        });
        return menu;
    }
    void closeMenus() {
        for (const auto& group : groups)
            if (group.popover)
                group.popover->hide();
        for (const auto& menu : menus)
            if (menu)
                menu->dismissPopup();
    }
    struct Group {
        QStringList items;
        QString current;
        QWidget* widget = nullptr;
        adqt::widgets::AdButton* main = nullptr;
        adqt::widgets::AdPopover* popover = nullptr;
    };
    void updateGroup(Group& group) {
        setScreenshotToolPaletteToolButtonIcon(group.main, toolIcon(group.current));
        group.main->setToolTip(label(group.current));
        group.main->setAccessibleName(label(group.current));
        group.main->setProperty("floatingToolbarAction", group.current);
    }
    void addMenuTools(adqt::widgets::AdContextMenu* menu, int index) {
        const auto items = groups[index].items;
        for (const auto& id : items) {
            auto* action = menu->addAction(label(id));
            action->setObjectName(id);
            QObject::connect(action, &QAction::triggered, &q, [this, index, id] {
                auto& group = groups[index];
                group.current = id;
                choices.insert(group.items.join(QLatin1Char('|')), id);
                updateGroup(group);
                invoke(id);
            });
        }
    }
    void materializeGroup(int index) {
        auto& group = groups[index];
        ScreenshotToolPaletteOptionPopoverEditorConfig config;
        config.contentObjectName = QStringLiteral("floatingToolbarGroupContent");
        config.optionSpacing = 8;
        for (int item = 0; item < group.items.size(); ++item)
            config.options.push_back({item, QString(), toolIcon(group.items[item])});
        const auto editor = materializeScreenshotToolPaletteOptionPopoverEditor(
            group.popover, &q, config,
            [this, index](int item) {
                auto& selected = groups[index];
                selected.current = selected.items[item];
                choices.insert(selected.items.join(QLatin1Char('|')), selected.current);
                updateGroup(selected);
                invoke(selected.current);
            },
            {32, 24, scale, nullptr});
        for (int item = 0; item < editor.buttons.size(); ++item) {
            auto* option = editor.buttons[item];
            const auto& id = group.items[item];
            option->setObjectName(id);
            option->setToolTip(label(id));
            option->setAccessibleName(label(id));
        }
        updateScreenshotToolPaletteOptionPopoverEditor(
            editor.buttons, editor.values, static_cast<int>(group.items.indexOf(group.current)));
    }
    adqt::widgets::AdButton* button(const adqt::icons::IconRef& iconRef) {
        return createScreenshotToolPaletteToolButton(panel, "", iconRef, {32, 24, scale, panel});
    }
    void rebuild() {
        if (!panel)
            return;
        contentDirty = false;
        arrangementKey.clear();
        closeMenus();
        for (auto& group : groups)
            delete group.widget;
        groups.clear();
        if (overflow)
            delete overflow;
        overflow = nullptr;
        menus.removeIf([](const auto& menu) { return menu.isNull(); });
        panel->resetContentLayout();
        const auto layout = toolbar_layout::normalizedLayout(
            storage::ScreenshotToolbarSettings().layout(kKind), kKind);
        for (const auto& position : layout.positions) {
            auto stack =
                toolbar_layout::stackPresentation(position, [](const QString&) { return true; });
            if (stack.itemIds.isEmpty())
                continue;
            Group group;
            group.items = stack.popoverItemIds;
            group.current = choices.value(group.items.join(QLatin1Char('|')), stack.entryItemId());
            if (!group.items.contains(group.current))
                group.current = stack.entryItemId();
            group.main = button(toolIcon(group.current));
            group.widget = group.main;
            const int index = static_cast<int>(groups.size());
            groups.push_back(group);
            updateGroup(groups[index]);
            QObject::connect(group.main, &adqt::widgets::AdButton::clicked, &q,
                             [this, index] { invoke(groups[index].current); });
            if (group.items.size() > 1) {
                auto* popover = createScreenshotToolPaletteOptionPopoverShell(
                    group.main, &q, [this, index] { materializeGroup(index); }, {});
                groups[index].popover = popover;
                QObject::connect(popover, &adqt::widgets::AdPopover::visibleChanged, &q,
                                 [this](bool visible) {
                                     menuDepth = std::max(0, menuDepth + (visible ? 1 : -1));
                                     syncOpacity();
                                     if (visible)
                                         retreat.stop();
                                     else
                                         retreat.start();
                                 });
            }
        }
        overflow = button(adqt::icons::antd::outlined::Ellipsis());
        overflow->setAccessibleName(text(QT_TRANSLATE_NOOP("FloatingToolbar", "More tools")));
        overflow->setToolTip(overflow->accessibleName());
        QObject::connect(overflow, &adqt::widgets::AdButton::clicked, &q, [this] {
            if (overflowMenu && overflowMenu->isPopupVisible())
                return;
            overflowMenu = makeMenu(overflow);
            for (int index = visibleGroupCount; index < groups.size(); ++index)
                addMenuTools(overflowMenu, index);
            overflowMenu->popupAt(overflow->mapToGlobal(QPoint(0, overflow->height())));
        });
        applyScale();
    }
    void applyScale() {
        if (icon)
            icon->setFixedSize(iconExtent());
        if (panel) {
            scaleScope->publishScale(
                adqt::widgets::AdControlScaleContext::fromDprsAndContentScale(1.0, 1.0, scale));
            toolbarIcon->setFixedSize(panel->buttonSize(), panel->buttonSize());
        }
    }
    void refresh() {
        if (stopped || !configuration)
            return;
        const storage::FloatingToolbarSettings settings;
        const auto fraction =
            floating_toolbar::remember(anchor, anchorExtent(toolbarMode), bounds());
        const qreal previousScale = scale;
        const bool previousMode = toolbarMode;
        enabled = settings.enabled();
        toolbarMode = settings.toolbarMode();
        hideCapture = settings.hideDuringCapture();
        idleOpacity = settings.opacity() / 100.0;
        scale =
            storage::ScreenshotUiSettings().toolbarSize() == QStringLiteral("small") ? 0.8 : 1.0;
        if (screen && scale != previousScale)
            anchor = floating_toolbar::restore(fraction, anchorExtent(toolbarMode), bounds());
        if (screen && !toolbarMode && (previousMode || scale != previousScale))
            anchor = floating_toolbar::dock(anchor, iconExtent(), bounds());
        if (!enabled) {
            fullscreen.stop();
            reveal.stop();
            retreat.stop();
            closeMenus();
            groups.clear();
            iconWindow.reset();
            toolbarWindow.reset();
            icon = nullptr;
            toolbarIcon = nullptr;
            panel = nullptr;
            overflow = nullptr;
            context = nullptr;
            overflowMenu = nullptr;
            scaleScope = nullptr;
            menus.clear();
            menuDepth = 0;
            expanded = false;
            dragging = false;
            pressed = false;
            contentDirty = true;
            return;
        }
        ensureIcon();
        if (toolbarMode)
            ensureToolbar();
        syncOpacity();
        if (contentDirty)
            rebuild();
        applyScale();
#ifdef Q_OS_MACOS
        icon->setAccessibleName(text(QT_TRANSLATE_NOOP("FloatingToolbar", "Show toolbar")));
#else
        icon->setAccessibleName(text(QT_TRANSLATE_NOOP("FloatingToolbar", "Screenshot")));
#endif
        icon->setToolTip(icon->accessibleName());
        if (toolbarIcon) {
            toolbarIcon->setAccessibleName(
                text(QT_TRANSLATE_NOOP("FloatingToolbar", "Screenshot")));
            toolbarIcon->setToolTip(toolbarIcon->accessibleName());
        }
        if (settings.hideInFullscreen()) {
            if (!fullscreen.isActive()) {
                fullscreenHidden = screen && detector(screen);
                fullscreen.start();
            }
        } else {
            fullscreen.stop();
            fullscreenHidden = false;
        }
        place();
        if (icon)
            icon->update();
        if (toolbarIcon)
            toolbarIcon->update();
    }
    void arrangeToolbar() {
        if (!panel || !screen)
            return;
        toolbarWindow->ensurePolished();
        const int sideSpace =
            right() ? anchor.x() - bounds().left() : bounds().right() + 1 - anchor.x() - iconSize();
        const int spacing = qRound(8 * scale);
        int remaining = (toolbarMode ? bounds().width() - toolbarIcon->width() : sideSpace) - 64;
        int visibleCount = 0;
        for (const auto& group : groups) {
            const int width = group.widget->sizeHint().width() + spacing;
            if (width > remaining)
                break;
            remaining -= width;
            ++visibleCount;
        }
        const QString nextKey = QStringLiteral("%1/%2/%3/%4")
                                    .arg(toolbarMode)
                                    .arg(right())
                                    .arg(visibleCount)
                                    .arg(scale);
        if (nextKey == arrangementKey)
            return;
        arrangementKey = nextKey;
        panel->resetContentLayout();
        auto* row = panel->contentLayout();
        toolbarIcon->setVisible(toolbarMode);
        if (toolbarMode && !right())
            row->addWidget(toolbarIcon);
        visibleGroupCount = visibleCount;
        if (overflowMenu)
            overflowMenu->dismissPopup();
        for (int index = 0; index < groups.size(); ++index) {
            auto& group = groups[index];
            group.widget->setVisible(index < visibleCount);
            if (index < visibleCount) {
                if (row->count())
                    panel->addSpacing(8);
                row->addWidget(group.widget);
            }
        }
        const bool hasOverflow = visibleCount < groups.size();
        overflow->setVisible(hasOverflow);
        if (hasOverflow) {
            if (row->count())
                panel->addSpacing(8);
            row->addWidget(overflow);
        }
        if (toolbarMode && right()) {
            if (row->count())
                panel->addSpacing(8);
            row->addWidget(toolbarIcon);
        }
        row->invalidate();
    }
    void place() {
        if (!screen || !iconWindow)
            return;
        anchor = floating_toolbar::constrain(anchor, anchorExtent(toolbarMode), bounds());
        const QPoint shown = (!toolbarMode && !dragging)
                                 ? floating_toolbar::tucked(anchor, iconExtent(), bounds())
                                 : anchor;
        // Keep the native surface inside this work area. Clipping the child,
        // rather than placing a masked native window across the seam, prevents
        // the concealed half ever appearing on an adjacent monitor.
        const QRect iconFrame(shown - QPoint(8, 8), iconExtent() + QSize(16, 16));
        const QRect visibleFrame = iconFrame.intersected(bounds());
        iconWindow->setGeometry(visibleFrame);
        icon->move(shown - visibleFrame.topLeft());
        if (toolbarWindow && (toolbarMode || expanded)) {
            // Publish the shadow and native frame changes as one paint update.
            toolbarWindow->setUpdatesEnabled(false);
            arrangeToolbar();
            toolbarWindow->layout()->setContentsMargins(
                ScreenshotToolbarMainPanel::shadowMargins());
            // Activate from the inside out so hidden content changes update the
            // panel's cached size hint before the outer layout measures it.
            panel->contentLayout()->activate();
            toolbarWindow->layout()->activate();
            toolbarWindow->adjustSize();
            // Hidden widgets defer resize events until show. Commit both layouts
            // to the final size before reading the panel or toolbar icon geometry.
            toolbarWindow->layout()->setGeometry(toolbarWindow->rect());
            panel->contentLayout()->setGeometry(panel->rect());
            const QPoint desired =
                toolbarMode ? anchor - toolbarIcon->mapTo(toolbarWindow.get(), QPoint())
                            : QPoint(right() ? shown.x() - panel->x() - panel->width() - 4
                                             : shown.x() + iconSize() + 4 - panel->x(),
                                     shown.y() + (iconSize() - panel->height()) / 2 - panel->y());
            if (toolbarMode) {
                // Constrain the visible panel, then clip only its shadow at the monitor edge.
                const QPoint panelPosition =
                    floating_toolbar::constrain(desired + panel->pos(), panel->size(), bounds());
                const QRect frame(panelPosition - panel->pos(), toolbarWindow->size());
                const QRect visible = frame.intersected(bounds());
                const QRect panelFrame(panelPosition, panel->size());
                toolbarWindow->layout()->setContentsMargins(
                    panelFrame.left() - visible.left(), panelFrame.top() - visible.top(),
                    visible.right() - panelFrame.right(), visible.bottom() - panelFrame.bottom());
                // Update the minimum window size before shrinking the shadow allocation.
                toolbarWindow->layout()->activate();
                toolbarWindow->setGeometry(visible);
                toolbarWindow->layout()->setGeometry(toolbarWindow->rect());
            } else {
                toolbarWindow->move(
                    floating_toolbar::constrain(desired, toolbarWindow->size(), bounds()));
            }
            if (auto* shadow = dynamic_cast<ToolbarShadow*>(panel->graphicsEffect()))
                shadow->setVisibleMargins(toolbarWindow->layout()->contentsMargins());
            toolbarWindow->setUpdatesEnabled(true);
        }
        syncVisibility();
    }
    bool dropReady() const {
        return (iconWindow && iconWindow->isDropReady()) ||
               (toolbarWindow && toolbarWindow->isDropReady());
    }
    void syncOpacity() {
        // The icon, expanded toolbar, and owned popups form one interaction surface.
        const bool hovered =
            menuDepth > 0 || (iconWindow && iconWindow->isVisible() && iconWindow->isHovered()) ||
            (toolbarWindow && toolbarWindow->isVisible() && toolbarWindow->isHovered());
        const qreal opacity = hovered || dropReady() ? 1.0 : idleOpacity;
        if (iconWindow)
            iconWindow->setWindowOpacity(opacity);
        if (toolbarWindow)
            toolbarWindow->setWindowOpacity(opacity);
    }
    void syncVisibility() {
        const bool allowed = visibleAllowed();
        if (!allowed) {
            reveal.stop();
            retreat.stop();
            closeMenus();
            expanded = false;
            cancelDrag();
            if (iconWindow)
                iconWindow->hide();
            if (toolbarWindow)
                toolbarWindow->hide();
            return;
        }
        if (iconWindow)
            iconWindow->setVisible(!toolbarMode);
        if (toolbarWindow)
            toolbarWindow->setVisible(toolbarMode || (expanded && !dragging));
#ifdef Q_OS_MACOS
        if (iconWindow && iconWindow->isVisible())
            platform::configureFloatingToolbarWindow(iconWindow.get(), !activities.isEmpty());
        if (toolbarWindow && toolbarWindow->isVisible())
            platform::configureFloatingToolbarWindow(toolbarWindow.get(), !activities.isEmpty());
#endif
    }
    void cancelDrag() {
        pressed = false;
        dragging = false;
        for (auto* handle : {icon, toolbarIcon}) {
            if (handle) {
                handle->setDown(false);
                handle->setCursor(Qt::PointingHandCursor);
            }
        }
    }
    bool containsCursor() const {
        const QPoint position = QCursor::pos();
        return (iconWindow && iconWindow->isVisible() &&
                iconWindow->geometry().contains(position)) ||
               (toolbarWindow && toolbarWindow->isVisible() &&
                toolbarWindow->geometry().contains(position));
    }
    void showContext(QPoint position) {
        if (!visibleAllowed() || dragging)
            return;
        reveal.stop();
        if (context && context->isPopupVisible())
            return;
        context = makeMenu(iconWindow.get());
        context->setObjectName(QStringLiteral("floatingToolbarContextMenu"));
        auto* mode = context->addAction(
            toolbarMode ? text(QT_TRANSLATE_NOOP("FloatingToolbar", "Toolbar mode"))
                        : text(QT_TRANSLATE_NOOP("FloatingToolbar", "Icon mode")));
        QObject::connect(mode, &QAction::triggered, &q, [this] {
            savePlacement();
            storage::FloatingToolbarSettings().setToolbarMode(!toolbarMode);
        });
        auto* customize =
            context->addAction(text(QT_TRANSLATE_NOOP("FloatingToolbar", "Customize toolbar")));
        QObject::connect(customize, &QAction::triggered, &q,
                         [this] { emit q.customizeRequested(); });
        context->addSeparator();
        auto* full =
            context->addAction(text(QT_TRANSLATE_NOOP("FloatingToolbar", "Hide in fullscreen")));
        full->setCheckable(true);
        full->setChecked(storage::FloatingToolbarSettings().hideInFullscreen());
        QObject::connect(full, &QAction::triggered, &q, [](bool value) {
            storage::FloatingToolbarSettings().setHideInFullscreen(value);
        });
        auto* capture = context->addAction(
            text(QT_TRANSLATE_NOOP("FloatingToolbar", "Hide during screenshots")));
        capture->setCheckable(true);
        capture->setChecked(hideCapture);
        QObject::connect(capture, &QAction::triggered, &q, [](bool value) {
            storage::FloatingToolbarSettings().setHideDuringCapture(value);
        });
        auto* close = context->addAction(text(QT_TRANSLATE_NOOP("FloatingToolbar", "Close")));
        QObject::connect(close, &QAction::triggered, &q,
                         [] { storage::FloatingToolbarSettings().setEnabled(false); });
        context->popupAt(position);
    }
    bool event(QObject* watched, QEvent* event) {
        if (event->type() == QEvent::LanguageChange) {
            closeMenus();
            queueRefresh();
            return false;
        }
        const bool handle = watched == icon || watched == toolbarIcon;
        if (handle && event->type() == QEvent::UngrabMouse) {
            cancelDrag();
        } else if (event->type() == QEvent::Enter) {
            retreat.stop();
            if (!toolbarMode && !dragging && !pressed && !expanded)
                reveal.start();
        } else if (event->type() == QEvent::Leave) {
            reveal.stop();
            if (!dragging)
                retreat.start();
        } else if (event->type() == QEvent::ContextMenu) {
            showContext(static_cast<QContextMenuEvent*>(event)->globalPos());
            return true;
        } else if (handle && event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                pressed = true;
                dragging = false;
                static_cast<QAbstractButton*>(watched)->setDown(true);
                pressPosition = mouse->globalPosition().toPoint();
                pressAnchor = toolbarMode ? toolbarIcon->mapToGlobal(QPoint()) : anchor;
                reveal.stop();
                return true;
            }
        } else if (handle && event->type() == QEvent::MouseMove && pressed) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const auto delta = mouse->globalPosition().toPoint() - pressPosition;
            if (!dragging && delta.manhattanLength() >= QApplication::startDragDistance()) {
                dragRight = right();
                dragging = true;
                expanded = false;
                closeMenus();
                static_cast<QAbstractButton*>(watched)->setDown(false);
                static_cast<QWidget*>(watched)->setCursor(Qt::ClosedHandCursor);
            }
            if (dragging) {
                if (auto* next = QGuiApplication::screenAt(mouse->globalPosition().toPoint()))
                    screen = next;
                anchor = pressAnchor + delta;
                place();
            }
            return true;
        } else if (handle && event->type() == QEvent::MouseButtonRelease && pressed) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton)
                return false;
            pressed = false;
            static_cast<QAbstractButton*>(watched)->setDown(false);
            static_cast<QWidget*>(watched)->setCursor(Qt::PointingHandCursor);
            if (dragging) {
                dragging = false;
                if (!toolbarMode)
                    anchor = floating_toolbar::dock(anchor, iconExtent(), bounds());
                savePlacement();
                place();
            } else {
                static_cast<QAbstractButton*>(watched)->click();
            }
            return true;
        }
        return false;
    }

    FloatingToolbarController& q;
    FullscreenDetector detector;
    QPointer<storage::ConfigurationStore> configuration;
    QPointer<QScreen> screen;
    std::unique_ptr<DropSurface> iconWindow, toolbarWindow;
    SnowflakeButton* icon = nullptr;
    SnowflakeButton* toolbarIcon = nullptr;
    ScreenshotToolbarMainPanel* panel = nullptr;
    adqt::widgets::AdControlScaleScope* scaleScope = nullptr;
    adqt::widgets::AdButton* overflow = nullptr;
    int visibleGroupCount = 0;
    QPointer<adqt::widgets::AdContextMenu> overflowMenu, context;
    QVector<QPointer<adqt::widgets::AdContextMenu>> menus;
    QVector<Group> groups;
    QHash<QString, QString> choices;
    QSet<QString> activities;
    QTimer reveal, retreat, fullscreen;
    QString arrangementKey;
    QPoint anchor, pressPosition, pressAnchor;
    bool dragRight = false;
    qreal scale = 1;
    qreal idleOpacity = 0.5;
    int menuDepth = 0;
    bool enabled = false, toolbarMode = false, hideCapture = true, fullscreenHidden = false;
    bool expanded = false, dragging = false, pressed = false, refreshQueued = false;
    bool saving = false, stopped = false, contentDirty = true;
};

FloatingToolbarController::FloatingToolbarController(QObject* parent, FullscreenDetector detector)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(detector))) {}
FloatingToolbarController::~FloatingToolbarController() {
    shutdown();
}
void FloatingToolbarController::refreshConfiguration() {
    m_impl->queueRefresh();
}
void FloatingToolbarController::setCaptureActive(const QString& source, bool active) {
    if (active) {
        m_impl->activities.insert(source);
        const bool wasVisible = (m_impl->iconWindow && m_impl->iconWindow->isVisible()) ||
                                (m_impl->toolbarWindow && m_impl->toolbarWindow->isVisible());
        m_impl->syncVisibility();
#ifdef Q_OS_WIN
        if (wasVisible && m_impl->hideCapture)
            DwmFlush();
#else
        Q_UNUSED(wasVisible);
#endif
    } else {
        if (!m_impl->activities.remove(source))
            return;
        QTimer::singleShot(0, this, [this] { m_impl->place(); });
    }
}
void FloatingToolbarController::shutdown() {
    m_impl->stopped = true;
    m_impl->fullscreen.stop();
    m_impl->reveal.stop();
    m_impl->retreat.stop();
    m_impl->syncVisibility();
}
bool FloatingToolbarController::ownsIdleSurface(const QWidget* widget) const {
    return widget == m_impl->iconWindow.get() || widget == m_impl->toolbarWindow.get();
}
bool FloatingToolbarController::blocksMemoryTrimming() const {
    if (m_impl->pressed || m_impl->dragging || m_impl->menuDepth || m_impl->dropReady() ||
        m_impl->containsCursor())
        return true;
    for (const auto& group : m_impl->groups) {
        if (group.popover && group.popover->isVisible())
            return true;
    }
    return false;
}
bool FloatingToolbarController::eventFilter(QObject* watched, QEvent* event) {
    return m_impl && m_impl->event(watched, event);
}
} // namespace snow_shot::presentation
