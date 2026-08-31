#include "snow_shot/presentation/systemtraycontroller.h"

#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingscatalog.h"

#include "widgets/context_menu.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QIcon>
#include <QImageReader>
#include <QKeySequence>
#include <QPixmap>
#include <QSet>
#include <QSystemTrayIcon>
#include <QVariant>

#include <algorithm>
#include <limits>

namespace snow_shot::presentation {
namespace {
constexpr auto DEFAULT_TRAY_ICON = "default";
constexpr auto DEFAULT_LEFT_CLICK_ACTION = "screenshot";

const QHash<QString, QString>& bundledIconResources() {
    static const QHash<QString, QString> resources{
        {QStringLiteral("default"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-default.png")},
        {QStringLiteral("light"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-light.png")},
        {QStringLiteral("dark"),
         QStringLiteral(":/snow-shot/app-icons/snow-shot-tray-dark.png")},
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

QString normalizedLeftClickAction(const QString& action) {
    return action == QStringLiteral("show_main_window")
               ? action
               : QString::fromLatin1(DEFAULT_LEFT_CLICK_ACTION);
}

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

        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (!reader.canRead()) {
            remember(path, size, modified, {}, {}, {});
            return {};
        }

        int selectedFrame = -1;
        if (suffix == QStringLiteral("ico")) {
            QImageReader probe(path);
            probe.setAutoTransform(true);
            const int frameCount = probe.imageCount();
            qint64 bestScore = std::numeric_limits<qint64>::max();
            for (int frame = 0; frame < frameCount; ++frame) {
                if (!probe.jumpToImage(frame)) {
                    continue;
                }
                const QSize candidate = probe.size();
                if (!candidate.isValid() || candidate.width() <= 0 || candidate.height() <= 0) {
                    continue;
                }
                const qint64 score = qAbs(static_cast<qint64>(candidate.width()) - 256) +
                                     qAbs(static_cast<qint64>(candidate.height()) - 256);
                if (score < bestScore) {
                    bestScore = score;
                    selectedFrame = frame;
                }
            }
            if (selectedFrame >= 0 && !reader.jumpToImage(selectedFrame)) {
                remember(path, size, modified, {}, {}, {});
                return {};
            }
        }

        const QSize sourceSize = reader.size();
        if (!sourceSize.isValid() || sourceSize.width() <= 0 || sourceSize.height() <= 0 ||
            sourceSize.width() > 16384 || sourceSize.height() > 16384 ||
            static_cast<qint64>(sourceSize.width()) * sourceSize.height() > 64LL * 1024 * 1024) {
            remember(path, size, modified, {}, sourceSize, {});
            return {};
        }

        const QSize bounded = sourceSize.scaled(QSize(256, 256), Qt::KeepAspectRatio);
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
            image = image.scaled(QSize(256, 256), Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
        QIcon icon(QPixmap::fromImage(image));
        remember(path, size, modified, icon, sourceSize, image.size());
        return icon;
    }

    quint64 hitCount() const { return hitCount_; }
    quint64 missCount() const { return missCount_; }
    quint64 decodeCount() const { return decodeCount_; }
    QSize sourcePixelSize() const { return sourcePixelSize_; }
    QSize decodedPixelSize() const { return decodedPixelSize_; }

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

    void remember(const QString& source, qint64 size, const QDateTime& modified,
                  const QIcon& icon, const QSize& sourcePixelSize,
                  const QSize& decodedPixelSize) {
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

QString nativeShortcutText(const QStringList& shortcuts) {
    QStringList nativeShortcuts;
    for (const QString& shortcut : shortcuts) {
        QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
        if (sequence.isEmpty()) {
            sequence = QKeySequence::fromString(shortcut, QKeySequence::NativeText);
        }
        const QString nativeShortcut = sequence.toString(QKeySequence::NativeText).trimmed();
        if (!nativeShortcut.isEmpty() && !nativeShortcuts.contains(nativeShortcut)) {
            nativeShortcuts.push_back(nativeShortcut);
        }
    }
    return nativeShortcuts.join(QStringLiteral(" / "));
}
} // namespace

class SystemTrayController::Impl {
  public:
    Impl(SystemTrayController& owner, const settings::TrayCommandManifest& sourceManifest)
        : q(owner), menu(std::make_unique<adqt::widgets::AdContextMenu>()),
          trayIcon(new QSystemTrayIcon(&owner)), manifest(sourceManifest),
          groups(manifest.groups) {
        q.setObjectName(QStringLiteral("systemTrayController"));
        menu->setObjectName(QStringLiteral("systemTrayMenu"));
        menu->setMinimumWidth(300);
        trayIcon->setObjectName(QStringLiteral("snowShotSystemTrayIcon"));
        trayIcon->setToolTip(QStringLiteral("SnowShot"));
        updateIcon();

        buildMenu();
        retranslateUi();
        setMenuOptions({});

        trayIcon->setContextMenu(menu.get());
        QObject::connect(trayIcon, &QSystemTrayIcon::activated, &q,
                         [this](QSystemTrayIcon::ActivationReason reason) {
                             if (reason == QSystemTrayIcon::Trigger) {
                                 if (leftClickAction == QStringLiteral("show_main_window")) {
                                     emit q.showMainWindowRequested();
                                 } else {
                                     emit q.screenshotRequested();
                                 }
                             }
                         });
        QObject::connect(&LanguageManager::instance(), &LanguageManager::languageChanged, &q,
                         [this](const QString&, const QLocale&) { retranslateUi(); });
    }

    ~Impl() {
        trayIcon->hide();
        trayIcon->setContextMenu(nullptr);
    }

    void buildMenu() {
        separatorsBeforeGroup.resize(groups.size());
        for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            const settings::SettingsTrayMenuGroupDefinition& group = groups.at(groupIndex);
            if (groupIndex > 0) {
                QAction* separator = menu->addSeparator();
                separator->setObjectName(
                    QStringLiteral("trayMenuSeparator-%1").arg(group.id));
                separatorsBeforeGroup[groupIndex] = separator;
            }
            for (const settings::SettingsTrayMenuOptionDefinition& option : group.options) {
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
                    break;
                case settings::SettingsTrayMenuOptionKind::DisableShortcutFunctions:
                    disableShortcutFunctionsAction = action;
                    action->setCheckable(true);
                    QObject::connect(action, &QAction::toggled, &q,
                                     [this](bool checked) {
                                         emit q.shortcutFunctionsDisabledChanged(checked);
                                     });
                    break;
                case settings::SettingsTrayMenuOptionKind::ShowMainWindow:
                    QObject::connect(action, &QAction::triggered, &q,
                                     &SystemTrayController::showMainWindowRequested);
                    break;
                case settings::SettingsTrayMenuOptionKind::Exit:
                    menu->setActionDanger(action);
                    QObject::connect(action, &QAction::triggered, &q,
                                     &SystemTrayController::exitRequested);
                    break;
                }
            }
        }
    }

    void retranslateUi() {
        for (const settings::SettingsTrayMenuGroupDefinition& group : groups) {
            for (const settings::SettingsTrayMenuOptionDefinition& option : group.options) {
                if (QAction* action = actions.value(option.id)) {
                    const QString label = option.kind == settings::SettingsTrayMenuOptionKind::QuickAction
                                              ? manifest.shortcutActionTitle(option.shortcutAction,
                                                                             screenshotDelaySeconds)
                                              : option.label.translated();
                    Q_ASSERT(!label.isEmpty());
                    const QString shortcut =
                        option.kind == settings::SettingsTrayMenuOptionKind::QuickAction
                            ? shortcutText.value(option.shortcutAction)
                            : QString();
                    action->setText(shortcut.isEmpty()
                                        ? label
                                        : label + QLatin1Char('\t') + shortcut);
                }
            }
        }
    }

    void setMenuOptions(const QStringList& options) {
        const QSet<QString> requested(options.cbegin(), options.cend());
        QStringList normalized;
        QVector<bool> visibleGroups(groups.size(), false);
        for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
            for (const settings::SettingsTrayMenuOptionDefinition& option :
                 groups.at(groupIndex).options) {
                QAction* action = actions.value(option.id);
                const bool visible = action != nullptr && requested.contains(option.id);
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

        if (disableShortcutFunctionsAction != nullptr &&
            !disableShortcutFunctionsAction->isVisible() &&
            disableShortcutFunctionsAction->isChecked()) {
            disableShortcutFunctionsAction->setChecked(false);
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
    std::unique_ptr<adqt::widgets::AdContextMenu> menu;
    QSystemTrayIcon* trayIcon = nullptr;
    settings::TrayCommandManifest manifest;
    QVector<settings::SettingsTrayMenuGroupDefinition> groups;
    QHash<QString, QAction*> actions;
    QHash<GlobalShortcutAction, QString> shortcutText;
    QVector<QAction*> separatorsBeforeGroup;
    TrayImageCache iconCache;
    QAction* disableShortcutFunctionsAction = nullptr;
    QStringList menuOptions;
    QString iconSelection = QString::fromLatin1(DEFAULT_TRAY_ICON);
    QString customIconPath;
    QString leftClickAction = QString::fromLatin1(DEFAULT_LEFT_CLICK_ACTION);
    int screenshotDelaySeconds = 3;
    bool enabled = true;
};

SystemTrayController::SystemTrayController(QObject* parent)
    : SystemTrayController(settings::builtInTrayCommandManifest(), parent) {}

SystemTrayController::SystemTrayController(const settings::TrayCommandManifest& manifest,
                                           QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, manifest)) {}

SystemTrayController::~SystemTrayController() = default;

void SystemTrayController::show() {
    if (!m_impl->enabled) {
        m_impl->trayIcon->hide();
        return;
    }
    m_impl->updateIcon();
    m_impl->trayIcon->show();
}

void SystemTrayController::hide() {
    m_impl->trayIcon->hide();
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
    m_impl->leftClickAction = normalizedLeftClickAction(action);
}

QString SystemTrayController::leftClickAction() const {
    return m_impl->leftClickAction;
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
                                               const QStringList& shortcuts) {
    const QString shortcutText = nativeShortcutText(shortcuts);
    if (m_impl->shortcutText.value(action) == shortcutText) {
        return;
    }
    if (shortcutText.isEmpty()) {
        m_impl->shortcutText.remove(action);
    } else {
        m_impl->shortcutText.insert(action, shortcutText);
    }
    m_impl->retranslateUi();
}

void SystemTrayController::setMenuOptions(const QStringList& options) {
    m_impl->setMenuOptions(options);
}

QStringList SystemTrayController::menuOptions() const {
    return m_impl->menuOptions;
}

bool SystemTrayController::shortcutFunctionsDisabled() const {
    return m_impl->disableShortcutFunctionsAction != nullptr &&
           m_impl->disableShortcutFunctionsAction->isChecked();
}
} // namespace snow_shot::presentation
