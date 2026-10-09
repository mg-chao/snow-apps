#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/presentation/systemnotificationcontroller.h"
#include "systemtraymenuposition.h"
#include "snowimageqtcodec.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/presentation/shortcutdisplaytext.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/storage/applicationstorage.h"

#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingscatalog.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"

#ifdef Q_OS_MACOS
#include "snow_shot/platform/macos/systemtraymenu.h"
#include "systemnotificationservice.h"
#endif

#include "antd_icons.h"
#include "widgets/context_menu.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QIcon>
#include <QImageReader>
#include <QEvent>
#include <QPointer>
#include <QPixmap>
#include <QPainter>
#include <QSet>
#include <QSystemTrayIcon>
#include <QDir>
#include <QVariant>

#include <algorithm>

namespace snow_shot::presentation {
namespace {
constexpr auto DEFAULT_TRAY_ICON = "default";
constexpr auto DEFAULT_LEFT_CLICK_ACTION = "screenshot";
constexpr auto DEFAULT_MIDDLE_CLICK_ACTION = "screenshot_fixed";

namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;
namespace outlined_icons = adqt::icons::antd::outlined;

const QHash<QString, QString>& bundledIconResources() {
    static const QHash<QString, QString> resources{
        {QStringLiteral("default"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-default.png")},
        {QStringLiteral("light"), QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png")},
        {QStringLiteral("dark"), QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-dark.png")},
        {QStringLiteral("snow-default"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-snow-default.png")},
        {QStringLiteral("snow-light"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-snow-light.png")},
        {QStringLiteral("snow-dark"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-snow-dark.png")},
    };
    return resources;
}

QString normalizedIconSelection(const QString& selection) {
    return bundledIconResources().contains(selection) ? selection
                                                      : QString::fromLatin1(DEFAULT_TRAY_ICON);
}

QString bundledIconResource(const QString& selection) {
    return bundledIconResources().value(normalizedIconSelection(selection));
}

QIcon withShortcutsDisabledBadge(const QIcon& base) {
    QIcon result;
    // Supply native tray sizes and their high-DPI counterparts to keep the badge crisp.
    for (const int size : {16, 20, 22, 24, 32, 40, 44, 48, 64, 128, 256}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        base.paint(&painter, QRect(0, 0, size, size));
        painter.scale(size / 16.0, size / 16.0);
        // A white edge separates the centered red badge from both light and dark artwork.
        const QRectF badge(4.25, 4.25, 7.5, 7.5);
        painter.setPen(QPen(Qt::white, 0.75));
        painter.setBrush(QColor(QStringLiteral("#e53935")));
        painter.drawEllipse(badge);
        painter.setPen(QPen(Qt::white, 1.15, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(6.45, 9.55), QPointF(9.55, 6.45));
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

QString normalizedClickAction(const QString& action, const char* defaultAction) {
    return action == QStringLiteral("screenshot") || action == QStringLiteral("show_main_window") ||
                   action == QStringLiteral("screenshot_copy") ||
                   action == QStringLiteral("screenshot_fixed") ||
                   action == QStringLiteral("open_function_settings")
               ? action
               : QString::fromLatin1(defaultAction);
}

// QSystemTrayIcon only reports that some balloon was clicked. macOS uses a native
// notification payload instead, because multiple delivered notifications coexist.
enum class BalloonKind { None, Capture, Warning, Update, RecordingExport };

class TrayImageCache final {
  public:
    QIcon load(const QString& path) {
        if (path.trimmed().isEmpty()) {
            clearEntry();
            return {};
        }

        const QFileInfo info(path);
        const qint64 size = info.exists() ? info.size() : -1;
        const QDateTime modified = info.exists() ? info.lastModified() : QDateTime();
        if (hasEntry_ && path == source_ && size == sourceFileSize_ &&
            modified == sourceModified_) {
            ++hitCount_;
            return icon_;
        }

        // Drop the old QIcon before decoding a replacement so a large custom image cannot remain
        // reachable through the one-entry cache after the source changes.
        clearEntry();
        ++missCount_;
        const QString suffix = info.suffix().toLower();
        if (suffix != QStringLiteral("png") && suffix != QStringLiteral("ico")) {
            remember(path, size, modified, {}, {}, {});
            return {};
        }

        if (suffix == QStringLiteral("ico")) {
            ++decodeCount_;
            QImage image = image_codec::decodeIconFile(path, 256);
            const QSize sourceSize = image.size();
            if (image.width() > 256 || image.height() > 256) {
                image =
                    image.scaled(QSize(256, 256), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            QIcon icon;
            if (!image.isNull()) {
                icon = QIcon(QPixmap::fromImage(image));
            }
            remember(path, size, modified, icon, sourceSize, image.size());
            return icon;
        }

        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (!reader.canRead()) {
            remember(path, size, modified, {}, {}, {});
            return {};
        }

        const QSize sourceSize = reader.effectiveSize();
        if (!sourceSize.isValid() || sourceSize.width() <= 0 || sourceSize.height() <= 0 ||
            sourceSize.width() > 16384 || sourceSize.height() > 16384 ||
            static_cast<qint64>(sourceSize.width()) * sourceSize.height() > 64LL * 1024 * 1024) {
            remember(path, size, modified, {}, sourceSize, {});
            return {};
        }

        // The decoded size accounts for orientation; Qt applies setScaledSize before that
        // transformation, so request scaling in the encoded image's coordinate system.
        const QSize bounded = reader.size().scaled(QSize(256, 256), Qt::KeepAspectRatio);
        if (sourceSize.width() > 256 || sourceSize.height() > 256) {
            reader.setScaledSize(bounded);
        }
        ++decodeCount_;
        QImage image = reader.read();
        if (image.isNull()) {
            remember(path, size, modified, {}, sourceSize, {});
            return {};
        }
        if (image.width() > 256 || image.height() > 256) {
            image = image.scaled(QSize(256, 256), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QIcon icon(QPixmap::fromImage(image));
        remember(path, size, modified, icon, sourceSize, image.size());
        return icon;
    }

    quint64 hitCount() const {
        return hitCount_;
    }
    quint64 missCount() const {
        return missCount_;
    }
    quint64 decodeCount() const {
        return decodeCount_;
    }
    QSize sourcePixelSize() const {
        return sourcePixelSize_;
    }
    QSize decodedPixelSize() const {
        return decodedPixelSize_;
    }

  private:
    void clearEntry() {
        source_.clear();
        sourceFileSize_ = -2;
        sourceModified_ = {};
        sourcePixelSize_ = {};
        decodedPixelSize_ = {};
        icon_ = QIcon();
        hasEntry_ = false;
    }

    void remember(const QString& source, qint64 size, const QDateTime& modified, const QIcon& icon,
                  const QSize& sourcePixelSize, const QSize& decodedPixelSize) {
        source_ = source;
        sourceFileSize_ = size;
        sourceModified_ = modified;
        sourcePixelSize_ = sourcePixelSize;
        decodedPixelSize_ = decodedPixelSize;
        icon_ = icon;
        hasEntry_ = true;
    }

    QString source_;
    qint64 sourceFileSize_ = -2;
    QDateTime sourceModified_;
    QSize sourcePixelSize_;
    QSize decodedPixelSize_;
    QIcon icon_;
    quint64 hitCount_ = 0;
    quint64 missCount_ = 0;
    quint64 decodeCount_ = 0;
    bool hasEntry_ = false;
};

// Each popup has its own viewport; decoding and preparation are shared by the
// application-owned skin service. Native tray menus never attach a skin view.
class TrayMenuSkinBinding final : public QObject {
  public:
    explicit TrayMenuSkinBinding(adqt::widgets::AdContextMenu* menu) : QObject(menu), menu_(menu) {
        menu_->installEventFilter(this);
        // Qt deletes children after the menu's derived state has been released.
        // End this binding while its event filter and settings callbacks can still use menu APIs.
        QObject::connect(menu_, &adqt::widgets::AdContextMenu::aboutToDestroy, this,
                         [this] { delete this; });
        connectConfiguration();
    }

    ~TrayMenuSkinBinding() override {
        QObject::disconnect(configurationConnection_);
        menu_->removeEventFilter(this);
        releaseFrame();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != menu_) {
            return QObject::eventFilter(watched, event);
        }
        const auto type = event->type();
        if (type != QEvent::Show && type != QEvent::Hide &&
            ((type != QEvent::Resize && type != QEvent::DevicePixelRatioChange) || !controller_ ||
             !attached_)) {
            return QObject::eventFilter(watched, event);
        }
        const QPointer<TrayMenuSkinBinding> lifetime(this);
        const QPointer<QObject> watchedLifetime(watched);
        switch (event->type()) {
        case QEvent::Show:
            syncVisibility(true);
            break;
        case QEvent::Hide:
            releaseFrame();
            break;
        case QEvent::Resize:
        case QEvent::DevicePixelRatioChange:
            if (controller_ && attached_) {
                controller_->setViewport(menu_, menu_->size(), menu_->devicePixelRatioF(),
                                         event->type() == QEvent::DevicePixelRatioChange);
            }
            break;
        default:
            break;
        }
        return !lifetime || !watchedLifetime ? true : QObject::eventFilter(watched, event);
    }

  private:
    void connectConfiguration() {
        auto& appStorage = storage::ApplicationStorage::instance();
        if (!appStorage.isInitialized()) {
            return;
        }
        auto* configuration = &appStorage.configuration();
        if (configuration_ == configuration) {
            return;
        }
        QObject::disconnect(configurationConnection_);
        configuration_ = configuration;
        configurationConnection_ =
            QObject::connect(configuration, &storage::ConfigurationStore::valueChanged, this,
                             [this](const QString& key) {
                                 if (key == QStringLiteral("interface/tray_menu_skin_path") ||
                                     key == QStringLiteral("interface/skin_opacity")) {
                                     syncVisibility();
                                 }
                             });
    }

    void releaseFrame() {
        const auto controller = controller_;
        auto* menu = menu_;
        const bool attached = attached_;
        if (controller)
            QObject::disconnect(controller, nullptr, this, nullptr);
        attached_ = false;
        controller_.clear();
        menu->resetBackgroundFrame();
        if (controller && attached)
            controller->detach(menu);
    }

    void syncVisibility(bool showing = false) {
        connectConfiguration();
        if (menu_->nativeMenuEnabled() || (!showing && !menu_->isVisible()) ||
            !storage::ApplicationStorage::instance().isInitialized() || !configuration_ ||
            configuration_->value(QStringLiteral("interface/skin_opacity")).toInt(100) <= 0 ||
            configuration_->value(QStringLiteral("interface/tray_menu_skin_path"))
                .toString()
                .isEmpty()) {
            releaseFrame();
            return;
        }
        if (!controller_) {
            attached_ = false;
            controller_ = &MainWindowSkinController::instance();
            QObject::connect(controller_, &MainWindowSkinController::viewFrameChanged, this,
                             [this](QObject* view) {
                                 if (view == menu_ && attached_) {
                                     syncFrame();
                                 }
                             });
            QObject::connect(controller_, &MainWindowSkinController::appearanceChanged, this,
                             [this]() {
                                 if (attached_) {
                                     syncFrame();
                                 }
                             });
        }
        if (!attached_) {
            attached_ = true;
            const QPointer<TrayMenuSkinBinding> lifetime(this);
            controller_->attach(menu_, SkinSurface::TrayMenu, menu_->size(),
                                menu_->devicePixelRatioF());
            if (!lifetime) {
                return;
            }
        }
        syncFrame();
    }

    void syncFrame() {
        if (!controller_) {
            attached_ = false;
            menu_->resetBackgroundFrame();
            return;
        }
        const auto frame = controller_->frame(menu_);
        menu_->setBackgroundFrame({controller_->pixmap(menu_), frame.normalizedPlacement,
                                   controller_->opacity(), controller_->maskOpacity()});
    }

    adqt::widgets::AdContextMenu* menu_;
    QPointer<MainWindowSkinController> controller_;
    QPointer<storage::ConfigurationStore> configuration_;
    QMetaObject::Connection configurationConnection_;
    bool attached_ = false;
};

} // namespace

class SystemTrayController::Impl {
  public:
    Impl(SystemTrayController& owner, const settings::TrayCommandManifest& sourceManifest,
         PinnedWindowGroupManager* groupManager)
        : q(owner), trayIcon(new QSystemTrayIcon(&owner)), manifest(sourceManifest),
          groups(manifest.groups), groupManager(groupManager) {
        if (this->groupManager == nullptr) {
            ownedGroupManager = std::make_unique<PinnedWindowGroupManager>();
            this->groupManager = ownedGroupManager.get();
        }
        q.setObjectName(QStringLiteral("systemTrayController"));
        trayIcon->setObjectName(QStringLiteral("snowShotSystemTrayIcon"));
        trayIcon->setToolTip(app::edition::isMini ? app::edition::productName()
                                                  : QStringLiteral("SnowShot"));
#ifdef Q_OS_MACOS
        notifications = std::make_unique<platform::macos::SystemNotificationService>(&q);
        using NotificationService = platform::macos::SystemNotificationService;
        QObject::connect(notifications.get(), &NotificationService::activated, &q,
                         [this](NotificationService::Action action, const QString& path) {
                             if (action == NotificationService::Action::OpenAbout)
                                 activateMessage(BalloonKind::Update, {});
                             else if (action == NotificationService::Action::OpenRecording)
                                 activateMessage(BalloonKind::RecordingExport, path);
                         });
        platform::SystemNotificationDelivery delivery =
            [native = notifications.get()](const platform::SystemNotificationRequest& request,
                                           platform::SystemNotificationCompletion completed) {
                native->show(request, std::move(completed));
            };
#else
        const QPointer<QSystemTrayIcon> trayGuard(trayIcon);
        platform::SystemNotificationDelivery delivery =
            [trayGuard](const platform::SystemNotificationRequest& request,
                        platform::SystemNotificationCompletion completed) {
                if (!trayGuard || !trayGuard->isVisible() ||
                    !QSystemTrayIcon::isSystemTrayAvailable() ||
                    !QSystemTrayIcon::supportsMessages()) {
                    completed({platform::SystemNotificationResult::Status::Unavailable,
                               QStringLiteral("System tray notifications are unavailable.")});
                    return;
                }
                const auto icon =
                    request.severity == platform::SystemNotificationSeverity::Error
                        ? QSystemTrayIcon::Critical
                    : request.severity == platform::SystemNotificationSeverity::Warning
                        ? QSystemTrayIcon::Warning
                        : QSystemTrayIcon::Information;
                trayGuard->showMessage(request.title, request.body, icon);
                completed({});
            };
#endif
        notificationDelivery =
            std::make_unique<SystemNotificationController>(std::move(delivery), &q);
        QObject::connect(notificationDelivery.get(),
                         &SystemNotificationController::deliveryFinished, &q,
                         &SystemTrayController::notificationDeliveryFinished);
        updateIcon();

        setMenuOptions({});

        // Cocoa opens an attached menu on left-click too. Handle Context explicitly there.
        QObject::connect(trayIcon, &QSystemTrayIcon::activated, &q,
                         [this](QSystemTrayIcon::ActivationReason reason) {
#ifdef Q_OS_MACOS
                             // AppKit/Qt emits another activation when our attached menu starts
                             // tracking. It is presentation, not another user click.
                             if (trayIcon->contextMenu()) {
                                 return;
                             }
#endif
                             if (reason == QSystemTrayIcon::Trigger) {
                                 dispatchClickAction(leftClickAction);
                             } else if (reason == QSystemTrayIcon::MiddleClick) {
                                 dispatchClickAction(middleClickAction);
                             } else if (reason == QSystemTrayIcon::Context) {
                                 if (menu && menu->isPopupVisible())
                                     return;
                                 auto* popup = createMenu();
#ifdef Q_OS_MACOS
                                 platform::macos::showSystemTrayMenu(trayIcon, popup);
#else
                                 popup->popupAt(
                                     systemTrayMenuPosition(trayIcon->geometry(), QCursor::pos()));
#endif
                             }
                         });
#ifndef Q_OS_MACOS
        QObject::connect(trayIcon, &QSystemTrayIcon::messageClicked, &q,
                         [this]() { activateMessage(lastBalloonKind, lastBalloonFilePath); });
#endif
        QObject::connect(&LanguageManager::instance(), &LanguageManager::languageChanged, &q,
                         [this](const QString&, const QLocale&) { retranslateUi(); });
        QObject::connect(&shortcuts::ShortcutDisplayService::instance(),
                         &shortcuts::ShortcutDisplayService::displayChanged, &q,
                         [this]() { retranslateUi(); });
        connectGroupManagerSignals();
    }

    ~Impl() {
        trayIcon->hide();
        trayIcon->setContextMenu(nullptr);
        for (const auto& popup : menus)
            delete popup.data();
    }

    adqt::widgets::AdContextMenu* createMenu() {
        if (menu)
            menu->dismissPopup();
        menu = new adqt::widgets::AdContextMenu;
        menus.removeIf([](const auto& popup) { return popup.isNull(); });
        menus.append(menu);
        menu->setDeleteOnHide();
        menu->setObjectName(QStringLiteral("systemTrayMenu"));
        menu->setMinimumWidth(300);
        new TrayMenuSkinBinding(menu);
        actions.clear();
        checkableQuickActions.clear();
        separatorsBeforeGroup.clear();
        buildMenu();
        retranslateUi();
        setMenuOptions(menuOptions);
        return menu;
    }

    void dispatchClickAction(const QString& action) {
        if (action == QStringLiteral("show_main_window")) {
            emit q.showMainWindowRequested();
        } else if (action == QStringLiteral("screenshot_copy")) {
            emit q.quickActionRequested(GlobalShortcutAction::ScreenshotCopy);
        } else if (action == QStringLiteral("screenshot_fixed")) {
            emit q.quickActionRequested(GlobalShortcutAction::ScreenshotFixed);
        } else if (action == QStringLiteral("open_function_settings")) {
            emit q.openFunctionSettingsRequested();
        } else {
            emit q.screenshotRequested();
        }
    }

    void activateMessage(BalloonKind kind, const QString& filePath) {
#ifndef Q_OS_MACOS
        if (!enabled)
            return;
#endif
        if (kind == BalloonKind::Update) {
            emit q.openAboutRequested();
        } else if (kind == BalloonKind::RecordingExport) {
            const QString path = filePath;
            emit q.openRecordingFileRequested(path);
        }
    }

    void showBalloon(const QString& title, const QString& message,
                     QSystemTrayIcon::MessageIcon icon, BalloonKind kind,
                     const QString& filePath = {}) {
        // Tray visibility governs only the icon/menu. macOS delivery and the actions
        // of already delivered notifications have their own lifetime and payloads.
#ifndef Q_OS_MACOS
        if (enabled) {
#endif
            lastBalloonKind = kind;
            lastBalloonFilePath = filePath;
            trayIcon->setProperty("lastBalloonTitle", title);
            trayIcon->setProperty("lastBalloonMessage", message);
            trayIcon->setProperty("lastBalloonIcon", static_cast<int>(icon));
#ifndef Q_OS_MACOS
        }
#endif
        using Action = platform::SystemNotificationAction;
        const Action action = kind == BalloonKind::Update            ? Action::OpenAbout
                              : kind == BalloonKind::RecordingExport ? Action::OpenRecording
                                                                     : Action::None;
        const auto severity =
            icon == QSystemTrayIcon::Critical  ? platform::SystemNotificationSeverity::Error
            : icon == QSystemTrayIcon::Warning ? platform::SystemNotificationSeverity::Warning
                                               : platform::SystemNotificationSeverity::Information;
        notificationDelivery->show({title, message, severity, action, filePath});
    }

    void buildMenu() {
        separatorsBeforeGroup.resize(groups.size());
        QString windowGroupingOptionId;
        for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            const settings::SettingsTrayMenuGroupDefinition& group = groups.at(groupIndex);
            if (groupIndex > 0) {
                QAction* separator = menu->addSeparator();
                separator->setObjectName(QStringLiteral("trayMenuSeparator-%1").arg(group.id));
                separatorsBeforeGroup[groupIndex] = separator;
            }
            for (const settings::SettingsTrayMenuOptionDefinition& option : group.options) {
                if (option.kind == settings::SettingsTrayMenuOptionKind::WindowGrouping) {
                    // The window group submenu created below stands in for this
                    // option, so it must not spawn a second menu action.
                    windowGroupingOptionId = option.id;
                    continue;
                }
                const adqt::icons::IconRef icon =
                    option.iconFactory ? option.iconFactory() : adqt::icons::IconRef{};
                QAction* action = menu->addItem(QString(), icon);
                action->setObjectName(
                    settings::generatedObjectName(QStringLiteral("tray-menu-action"), option.id));
                action->setData(option.id);
                actions.insert(option.id, action);
                switch (option.kind) {
                case settings::SettingsTrayMenuOptionKind::QuickAction:
                    QObject::connect(action, &QAction::triggered, &q,
                                     [this, shortcutAction = option.shortcutAction]() {
                                         emit q.quickActionRequested(shortcutAction);
                                     });
                    if (option.checkable) {
                        // The checkmark is a view of owner state; nothing
                        // listens to toggled() here.
                        action->setCheckable(true);
                        action->setChecked(checkedQuickActions.value(option.shortcutAction));
                        checkableQuickActions.insert(option.shortcutAction, action);
                    }
                    break;
                case settings::SettingsTrayMenuOptionKind::ShowMainWindow:
                    QObject::connect(action, &QAction::triggered, &q,
                                     &SystemTrayController::showMainWindowRequested);
                    break;
                case settings::SettingsTrayMenuOptionKind::RestartApp:
                    QObject::connect(action, &QAction::triggered, &q,
                                     &SystemTrayController::restartRequested);
                    break;
                case settings::SettingsTrayMenuOptionKind::Exit:
                    menu->setActionDanger(action);
                    QObject::connect(action, &QAction::triggered, &q,
                                     &SystemTrayController::exitRequested);
                    break;
                case settings::SettingsTrayMenuOptionKind::WindowGrouping:
                    break;
                }
            }
        }
        QAction* showMainWindow = actions.value(QStringLiteral("tray.show-main-window"));
        groupMenuAction = menu->addLazySubMenu(
            QString(),
            [this](auto* popup) {
                groupMenu = popup;
                popup->setObjectName(QStringLiteral("systemTrayWindowGroupMenu"));
                popup->setMinimumWidth(300);
                new TrayMenuSkinBinding(popup);
                rebuildGroupMenu();
            },
            custom_outlined_icons::Group());
        groupMenuAction->setObjectName(QStringLiteral("systemTrayWindowGroupAction"));
        if (!windowGroupingOptionId.isEmpty()) {
            groupMenuAction->setData(windowGroupingOptionId);
            actions.insert(windowGroupingOptionId, groupMenuAction);
        }
        if (showMainWindow != nullptr)
            menu->insertAction(showMainWindow, groupMenuAction);
        updateGroupMenuTitle();
    }

    void updateGroupMenuTitle() {
        if (groupMenuAction)
            groupMenuAction->setText(
                QCoreApplication::translate("SystemTrayController", "Window Group: %1")
                    .arg(groupManager->displayName(groupManager->activeGroupId())));
    }

    void requestGroupMenuRefresh() {
        updateGroupMenuTitle();
        if (groupMenu && groupMenu->isPopupVisible() && !groupMenu->isRetiring())
            rebuildGroupMenu();
    }

    void rebuildGroupMenu() {
        if (groupMenu == nullptr || groupManager == nullptr) {
            return;
        }
        groupMenu->clear();
        updateGroupMenuTitle();
        const auto currentGroups = groupManager->displaySnapshot();
        bool hasDeletableEmptyGroups = false;
        for (const auto& group : currentGroups) {
            const auto counts = group.counts;
            hasDeletableEmptyGroups =
                hasDeletableEmptyGroups ||
                (group.id != QStringLiteral("default") && counts.nonIgnored == 0);
            QAction* action = groupMenu->addItem(group.name);
            groupMenu->setActionBadge(
                action, QStringLiteral("%1/%2").arg(counts.nonIgnored).arg(counts.total));
            action->setObjectName(QStringLiteral("systemTrayGroupAction-%1").arg(group.id));
            action->setData(group.id);
            action->setCheckable(true);
            action->setChecked(group.id == groupManager->activeGroupId());
            QObject::connect(action, &QAction::triggered, &q,
                             [this, id = group.id]() { groupManager->setActiveGroup(id); });
        }
        groupMenu->addSeparator();
        QAction* newGroup =
            groupMenu->addItem(QCoreApplication::translate("SystemTrayController", "New Group"),
                               outlined_icons::FolderAdd());
        newGroup->setObjectName(QStringLiteral("systemTrayNewGroupAction"));
        QObject::connect(newGroup, &QAction::triggered, &q,
                         [this]() { groupManager->openCreateGroupModal(nullptr); });
        QAction* deleteEmpty = groupMenu->addItem(
            QCoreApplication::translate("SystemTrayController", "Delete Empty Groups"),
            outlined_icons::Clear());
        deleteEmpty->setObjectName(QStringLiteral("systemTrayDeleteEmptyGroupsAction"));
        deleteEmpty->setEnabled(hasDeletableEmptyGroups);
        QObject::connect(deleteEmpty, &QAction::triggered, &q,
                         [this]() { groupManager->openDeleteEmptyGroupsConfirmation(nullptr); });

        auto* deleteSpecified = groupMenu->addLazySubMenu(
            QCoreApplication::translate("SystemTrayController", "Delete Specified Group"),
            [this](auto* popup) {
                popup->setObjectName(QStringLiteral("systemTrayDeleteSpecifiedGroupMenu"));
                popup->setMinimumWidth(300);
                new TrayMenuSkinBinding(popup);
                for (const auto& group : groupManager->displaySnapshot()) {
                    const auto counts = group.counts;
                    QAction* action = popup->addItem(group.name);
                    popup->setActionBadge(
                        action, QStringLiteral("%1/%2").arg(counts.nonIgnored).arg(counts.total));
                    action->setObjectName(
                        QStringLiteral("systemTrayDeleteSpecifiedGroupAction-%1").arg(group.id));
                    action->setData(group.id);
                    QObject::connect(action, &QAction::triggered, &q, [this, id = group.id]() {
                        groupManager->openDeleteSpecifiedGroupConfirmation(id, nullptr);
                    });
                }
            },
            custom_outlined_icons::Delete());
        deleteSpecified->setObjectName(QStringLiteral("systemTrayDeleteSpecifiedGroupAction"));
    }

    void connectGroupManagerSignals() {
        if (groupManager == nullptr) {
            return;
        }
        QObject::disconnect(groupManager, nullptr, &q, nullptr);
        QObject::connect(groupManager, &PinnedWindowGroupManager::groupsChanged, &q,
                         [this]() { requestGroupMenuRefresh(); });
        QObject::connect(groupManager, &PinnedWindowGroupManager::activeGroupChanged, &q,
                         [this](const QString&) { requestGroupMenuRefresh(); });
    }

    void retranslateUi() {
        if (app::edition::isMini)
            trayIcon->setToolTip(app::edition::productName());
        for (const settings::SettingsTrayMenuGroupDefinition& group : groups) {
            for (const settings::SettingsTrayMenuOptionDefinition& option : group.options) {
                if (QAction* action = actions.value(option.id)) {
                    const QString label =
                        option.kind == settings::SettingsTrayMenuOptionKind::QuickAction
                            ? manifest.shortcutActionTitle(option.shortcutAction,
                                                           screenshotDelaySeconds)
                            : option.label.translated();
                    Q_ASSERT(!label.isEmpty());
                    const QString shortcut =
                        option.kind == settings::SettingsTrayMenuOptionKind::QuickAction
                            ? shortcuts::formatShortcutListDisplayText(
                                  shortcutBindings.value(option.shortcutAction))
                            : QString();
                    action->setText(shortcut.isEmpty() ? label
                                                       : label + QLatin1Char('\t') + shortcut);
                }
            }
        }
        requestGroupMenuRefresh();
    }

    void setMenuOptions(const QStringList& options) {
        const QSet<QString> requested(options.cbegin(), options.cend());
        QStringList normalized;
        QVector<bool> visibleGroups(groups.size(), false);
        bool toggleVisible = false;
        for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            for (const settings::SettingsTrayMenuOptionDefinition& option :
                 groups.at(groupIndex).options) {
                QAction* action = actions.value(option.id).data();
                const bool visible =
                    requested.contains(option.id)
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
                    && (option.shortcutAction != GlobalShortcutAction::TranslateSelectedText ||
                        snow_shot::storage::ExtendedFeaturesSettings().translationPageEnabled())
#endif
                    ;
                if (option.shortcutAction == GlobalShortcutAction::ToggleGlobalHotkeys)
                    toggleVisible = visible;
                if (action != nullptr) {
                    action->setVisible(visible);
                }
                if (visible) {
                    normalized.push_back(option.id);
                    visibleGroups[groupIndex] = true;
                }
            }
        }
        menuOptions = normalized;

        bool priorGroupVisible = visibleGroups.value(0, false);
        for (int groupIndex = 1; groupIndex < groups.size(); ++groupIndex) {
            if (QAction* separator = separatorsBeforeGroup.value(groupIndex)) {
                separator->setVisible(priorGroupVisible && visibleGroups.at(groupIndex));
            }
            priorGroupVisible = priorGroupVisible || visibleGroups.at(groupIndex);
        }

        // Session latches must not leave the user without a way back: hiding a
        // checked ToggleGlobalHotkeys re-enables through the same quick action.
        // Persisted checkables such as fullscreen suppression must not be
        // flipped by menu visibility.
        if (checkedQuickActions.value(GlobalShortcutAction::ToggleGlobalHotkeys) &&
            !toggleVisible) {
            checkedQuickActions.insert(GlobalShortcutAction::ToggleGlobalHotkeys, false);
            if (auto* action =
                    checkableQuickActions.value(GlobalShortcutAction::ToggleGlobalHotkeys).data())
                action->setChecked(false);
            emit q.quickActionRequested(GlobalShortcutAction::ToggleGlobalHotkeys);
        }
    }

    void updateIcon() {
        QIcon icon = iconCache.load(customIconPath);
        QString resolvedSource = customIconPath;
        if (icon.isNull()) {
            resolvedSource = bundledIconResource(iconSelection);
            icon = QIcon(resolvedSource);
        }
        if (icon.isNull()) {
            resolvedSource = QStringLiteral("application-window-icon");
            icon = QApplication::windowIcon();
        }
        if (icon.isNull()) {
            resolvedSource = QCoreApplication::applicationFilePath();
            icon = QIcon(QCoreApplication::applicationFilePath());
        }
        if (globalShortcutsDisabled) {
            icon = withShortcutsDisabledBadge(icon);
        }
#ifdef Q_OS_MACOS
        // Template rendering discards RGB colors, including the selected bundled artwork.
        icon.setIsMask(false);
#endif
        trayIcon->setIcon(icon);
        trayIcon->setProperty("resolvedIconSource", resolvedSource);
        trayIcon->setProperty("customIconCacheHits",
                              QVariant::fromValue<qulonglong>(iconCache.hitCount()));
        trayIcon->setProperty("customIconCacheMisses",
                              QVariant::fromValue<qulonglong>(iconCache.missCount()));
        trayIcon->setProperty("customIconDecodeCount",
                              QVariant::fromValue<qulonglong>(iconCache.decodeCount()));
        trayIcon->setProperty("customIconSourcePixelSize", iconCache.sourcePixelSize());
        trayIcon->setProperty("customIconDecodedPixelSize", iconCache.decodedPixelSize());
    }

    SystemTrayController& q;
    QPointer<adqt::widgets::AdContextMenu> menu;
    QList<QPointer<adqt::widgets::AdContextMenu>> menus;
    QSystemTrayIcon* trayIcon = nullptr;
#ifdef Q_OS_MACOS
    std::unique_ptr<platform::macos::SystemNotificationService> notifications;
#endif
    std::unique_ptr<SystemNotificationController> notificationDelivery;
    settings::TrayCommandManifest manifest;
    QVector<settings::SettingsTrayMenuGroupDefinition> groups;
    std::unique_ptr<PinnedWindowGroupManager> ownedGroupManager;
    PinnedWindowGroupManager* groupManager = nullptr;
    QPointer<adqt::widgets::AdContextMenu> groupMenu;
    QPointer<QAction> groupMenuAction;
    QHash<QString, QPointer<QAction>> actions;
    QHash<GlobalShortcutAction, shortcuts::ShortcutBindingList> shortcutBindings;
    QHash<GlobalShortcutAction, QPointer<QAction>> checkableQuickActions;
    QHash<GlobalShortcutAction, bool> checkedQuickActions;
    QVector<QPointer<QAction>> separatorsBeforeGroup;
    TrayImageCache iconCache;
    QStringList menuOptions;
    QString iconSelection = QString::fromLatin1(DEFAULT_TRAY_ICON);
    QString customIconPath;
    QString leftClickAction = QString::fromLatin1(DEFAULT_LEFT_CLICK_ACTION);
    QString middleClickAction = QString::fromLatin1(DEFAULT_MIDDLE_CLICK_ACTION);
    int screenshotDelaySeconds = 3;
    BalloonKind lastBalloonKind = BalloonKind::None;
    QString lastBalloonFilePath;
    bool enabled = true;
    bool globalShortcutsDisabled = false;
};

SystemTrayController::SystemTrayController(QObject* parent)
    : SystemTrayController(settings::builtInTrayCommandManifest(), nullptr, parent) {}

SystemTrayController::SystemTrayController(const settings::TrayCommandManifest& manifest,
                                           QObject* parent)
    : SystemTrayController(manifest, nullptr, parent) {}

SystemTrayController::SystemTrayController(const settings::TrayCommandManifest& manifest,
                                           std::nullptr_t)
    : SystemTrayController(manifest, static_cast<QObject*>(nullptr)) {}

SystemTrayController::SystemTrayController(const settings::TrayCommandManifest& manifest,
                                           PinnedWindowGroupManager* groupManager, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, manifest, groupManager)) {}

SystemTrayController::~SystemTrayController() = default;

adqt::widgets::AdContextMenu* SystemTrayController::createContextMenu() {
    if (m_impl->menu && m_impl->menu->isPopupVisible())
        return m_impl->menu;
    return m_impl->createMenu();
}

void SystemTrayController::setGroupManager(PinnedWindowGroupManager* groupManager) {
    if (groupManager == nullptr) {
        return;
    }
    if (m_impl->groupManager == groupManager) {
        return;
    }
    if (m_impl->groupManager != nullptr) {
        QObject::disconnect(m_impl->groupManager, nullptr, this, nullptr);
    }
    m_impl->ownedGroupManager.reset();
    m_impl->groupManager = groupManager;
    m_impl->connectGroupManagerSignals();
    m_impl->requestGroupMenuRefresh();
}

void SystemTrayController::show() {
    if (!m_impl->enabled) {
        m_impl->trayIcon->hide();
        return;
    }
    m_impl->updateIcon();
    m_impl->trayIcon->show();
}

void SystemTrayController::hide() {
    if (m_impl->menu)
        m_impl->menu->dismissPopup();
    m_impl->trayIcon->hide();
}

void SystemTrayController::showCaptureMessage(const QString& message, bool warning) {
    m_impl->showBalloon(tr("Capture"), message,
                        warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Critical,
                        BalloonKind::Capture);
}

void SystemTrayController::showWarningMessage(const QString& title, const QString& message) {
    m_impl->showBalloon(title, message, QSystemTrayIcon::Warning, BalloonKind::Warning);
}

void SystemTrayController::showUpdateMessage(const QString& message) {
    m_impl->showBalloon(tr("Update"), message, QSystemTrayIcon::Information, BalloonKind::Update);
}

void SystemTrayController::showRecordingExportMessage(const QString& path) {
    if (path.isEmpty())
        return;
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    m_impl->showBalloon(tr("Video export completed"), QDir::toNativeSeparators(absolutePath),
                        QSystemTrayIcon::Information, BalloonKind::RecordingExport, absolutePath);
}

void SystemTrayController::setEnabled(bool enabled) {
    if (m_impl->enabled == enabled) {
        return;
    }
    m_impl->enabled = enabled;
    if (enabled) {
        show();
    } else {
        hide();
    }
}

bool SystemTrayController::isEnabled() const {
    return m_impl->enabled;
}

void SystemTrayController::setIconSelection(const QString& selection) {
    const QString normalized = normalizedIconSelection(selection);
    if (m_impl->iconSelection == normalized) {
        return;
    }
    m_impl->iconSelection = normalized;
    m_impl->updateIcon();
}

QString SystemTrayController::iconSelection() const {
    return m_impl->iconSelection;
}

void SystemTrayController::setCustomIconPath(const QString& path) {
    if (m_impl->customIconPath == path) {
        return;
    }
    m_impl->customIconPath = path;
    m_impl->updateIcon();
}

QString SystemTrayController::customIconPath() const {
    return m_impl->customIconPath;
}

void SystemTrayController::setLeftClickAction(const QString& action) {
    m_impl->leftClickAction = normalizedClickAction(action, DEFAULT_LEFT_CLICK_ACTION);
}

QString SystemTrayController::leftClickAction() const {
    return m_impl->leftClickAction;
}

void SystemTrayController::setMiddleClickAction(const QString& action) {
    m_impl->middleClickAction = normalizedClickAction(action, DEFAULT_MIDDLE_CLICK_ACTION);
}

QString SystemTrayController::middleClickAction() const {
    return m_impl->middleClickAction;
}

void SystemTrayController::setScreenshotDelaySeconds(int seconds) {
    const int normalized = std::clamp(seconds, 1, 10);
    if (m_impl->screenshotDelaySeconds == normalized) {
        return;
    }
    m_impl->screenshotDelaySeconds = normalized;
    m_impl->retranslateUi();
}

int SystemTrayController::screenshotDelaySeconds() const {
    return m_impl->screenshotDelaySeconds;
}

void SystemTrayController::setGlobalShortcuts(GlobalShortcutAction action,
                                              const shortcuts::ShortcutBindingList& shortcuts) {
    if (m_impl->shortcutBindings.value(action) == shortcuts) {
        return;
    }
    if (shortcuts.isEmpty()) {
        m_impl->shortcutBindings.remove(action);
    } else {
        m_impl->shortcutBindings.insert(action, shortcuts);
    }
    m_impl->retranslateUi();
}

void SystemTrayController::setMenuOptions(const QStringList& options) {
    m_impl->setMenuOptions(options);
}

QStringList SystemTrayController::menuOptions() const {
    return m_impl->menuOptions;
}

void SystemTrayController::setQuickActionChecked(GlobalShortcutAction action, bool checked) {
    if (action == GlobalShortcutAction::ToggleGlobalHotkeys &&
        m_impl->globalShortcutsDisabled != checked) {
        m_impl->globalShortcutsDisabled = checked;
        m_impl->updateIcon();
    }
    m_impl->checkedQuickActions.insert(action, checked);
    // Pure view update: owners announce changes; the checkmark only mirrors
    // them, so this must not dispatch anything.
    if (QAction* trayAction = m_impl->checkableQuickActions.value(action)) {
        trayAction->setChecked(checked);
    }
}
} // namespace snow_shot::presentation
