#include "snow_shot/presentation/pinnedwindowselectioncontroller.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/settingsadapters.h"
#include "antd_icons.h"
#include "widgets/context_menu.h"
#include "widgets/message.h"
#include "widgets/modal.h"
#include "widgets/button.h"

#include <QApplication>
#include <QAction>
#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
namespace outlined = adqt::icons::antd::outlined;
namespace customIcons = snow_shot::presentation::icons::custom::outlined;
namespace {
[[maybe_unused]] constexpr const char* translationSources[] = {
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Close Other Windows"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Align Position"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Group"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Lock"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Close"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Destroy"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Align left"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Center horizontally"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Align right"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Distribute horizontally"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Align top"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Center vertically"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "Align bottom"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Distribute vertically"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController", "New Group"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Delete Empty Groups"),
    QT_TRANSLATE_NOOP("snow_shot::presentation::PinnedWindowSelectionController",
                      "Delete Specified Group"),
};
QString text(const char* source) {
    return PinnedWindowSelectionController::tr(source);
}
void translateAction(QAction* action, const char* source) {
    action->setProperty("pinnedSelectionTranslationSource", QString::fromUtf8(source));
}
} // namespace

PinnedWindowSelectionController::PinnedWindowSelectionController(QObject* parent,
                                                                 DestroyRecords destroyRecords)
    : QObject(parent), m_destroyRecords(std::move(destroyRecords)) {
    if (qApp)
        qApp->installEventFilter(this);
}

PinnedWindowSelectionController::~PinnedWindowSelectionController() {
    if (qApp)
        qApp->removeEventFilter(this);
    cancelGeometry();
    for (const auto& window : std::as_const(m_selected))
        if (window)
            window->setWindowSelected(false);
    if (m_contextMenu)
        delete m_contextMenu;
    if (m_destroyConfirmation)
        delete m_destroyConfirmation;
}

void PinnedWindowSelectionController::registerWindow(ScreenshotPinnedWindow* window) {
    if (window && !m_windowIndex.contains(window)) {
        m_windows.append(window);
        m_windowIndex.insert(window);
        connect(window, &QObject::destroyed, this, [this, window] {
            m_windowIndex.remove(window);
            m_selectedIndex.remove(window);
            m_windows.removeAll(nullptr);
            if (m_selected.removeAll(nullptr) > 0) {
                cancelGeometry();
                notifySelectionChanged();
            }
        });
    }
}

void PinnedWindowSelectionController::unregisterWindow(ScreenshotPinnedWindow* window) {
    if (window)
        disconnect(window, nullptr, this, nullptr);
    deselectWindow(window);
    m_windows.removeAll(window);
    m_windowIndex.remove(window);
}

bool PinnedWindowSelectionController::isSelectable(const ScreenshotPinnedWindow* window) const {
    return window && m_windowIndex.contains(window) && window->m_presented &&
           window->m_firstContentFramePublished && !window->m_closing && window->isVisible() &&
           !window->isMinimized() && !window->m_thumbnailMode && !window->hideToTopActive() &&
           !window->m_clickThroughActive &&
           (!window->m_groupManager ||
            window->groupId() == window->m_groupManager->activeGroupId());
}

bool PinnedWindowSelectionController::isSelected(const ScreenshotPinnedWindow* window) const {
    return window && m_selectedIndex.contains(window);
}

int PinnedWindowSelectionController::selectedCount() const {
    return static_cast<int>(m_selected.size());
}

QList<QPointer<ScreenshotPinnedWindow>> PinnedWindowSelectionController::selectedWindows() const {
    return m_selected;
}

void PinnedWindowSelectionController::windowStateChanged(ScreenshotPinnedWindow* window) {
    if (!isSelectable(window))
        deselectWindow(window);
}

void PinnedWindowSelectionController::notifySelectionChanged() {
    ++m_selectionRevision;
    if (m_contextMenu)
        m_contextMenu->hide();
    emit selectionChanged();
}

void PinnedWindowSelectionController::toggleSelection(ScreenshotPinnedWindow* window) {
    if (isSelected(window)) {
        deselectWindow(window);
    } else if (isSelectable(window)) {
        cancelGeometry();
        m_selected.append(window);
        m_selectedIndex.insert(window);
        window->setWindowSelected(true);
        notifySelectionChanged();
    }
}

void PinnedWindowSelectionController::deselectWindow(ScreenshotPinnedWindow* window) {
    if (!isSelected(window))
        return;
    cancelGeometry();
    m_selected.removeAll(window);
    m_selectedIndex.remove(window);
    if (window)
        window->setWindowSelected(false);
    notifySelectionChanged();
}

void PinnedWindowSelectionController::clearSelection() {
    if (m_selected.isEmpty())
        return;
    cancelGeometry();
    const auto selected = std::exchange(m_selected, {});
    m_selectedIndex.clear();
    for (const auto& window : selected)
        if (window)
            window->setWindowSelected(false);
    notifySelectionChanged();
}

void PinnedWindowSelectionController::showFailure(QWidget* owner, const QString& message) {
    adqt::widgets::AdMessage::Request request;
    request.key = QStringLiteral("pinned-selection-operation");
    request.content = message;
    adqt::widgets::AdMessageService::error(std::move(request), owner);
}

bool PinnedWindowSelectionController::showContextMenu(ScreenshotPinnedWindow* window,
                                                      const QPoint& globalPosition) {
    if (!window || !m_windowIndex.contains(window))
        return false;
    if (!isSelected(window)) {
        if (m_selected.isEmpty())
            return false;
        clearSelection();
        window->showContextMenu(globalPosition);
        return true;
    }
    if (selectedCount() < 2)
        return false;
    cancelGeometry();
    buildContextMenu(window);
    m_contextMenu->popupAt(globalPosition);
    return true;
}

void PinnedWindowSelectionController::buildContextMenu(ScreenshotPinnedWindow* owner) {
    if (m_contextMenu)
        delete m_contextMenu;
    auto* menu = new adqt::widgets::AdContextMenu(owner);
    m_contextMenu = menu;
    menu->setObjectName(QStringLiteral("screenshotPinnedMultiSelectionMenu"));
    menu->setFixedWidth(300);
    const auto targets = selectedWindows();
    const quint64 revision = m_selectionRevision;
    const auto valid = [this, revision, targets] {
        return m_selectionRevision == revision &&
               std::all_of(targets.cbegin(), targets.cend(), [this](const auto& window) {
                   return isSelected(window) && isSelectable(window);
               });
    };
    auto add = [menu](const char* source, const QString& name, const auto& icon) {
        auto* action = menu->addItem(text(source), icon);
        action->setObjectName(name);
        translateAction(action, source);
        return action;
    };
    auto* count =
        menu->addItem(tr("%n selected window(s)", nullptr, static_cast<int>(targets.size())));
    count->setObjectName(QStringLiteral("screenshotPinnedSelectionCount"));
    count->setEnabled(false);
    count->setProperty("pinnedSelectionCount", targets.size());
    menu->addSeparator();
    auto* closeOthers =
        add("Close Other Windows", QStringLiteral("screenshotPinnedSelectionCloseOthers"),
            outlined::DeleteRow());
    bool hasOther = false;
    for (const auto& window : std::as_const(m_windows))
        hasOther |= window && !targets.contains(window) && window->m_presented &&
                    !window->m_closing &&
                    (!window->m_groupManager ||
                     window->groupId() == window->m_groupManager->activeGroupId());
    closeOthers->setEnabled(hasOther);
    connect(closeOthers, &QAction::triggered, this, [this, targets, valid] {
        if (!valid())
            return;
        const auto windows = m_windows;
        for (const auto& window : windows)
            if (window && !targets.contains(window) && window->m_presented && !window->m_closing &&
                (!window->m_groupManager ||
                 window->groupId() == window->m_groupManager->activeGroupId()))
                window->requestUserClose();
    });

    auto* alignment = menu->addSubMenu(text("Align Position"), customIcons::AlignLeft());
    translateAction(alignment->menuAction(), "Align Position");
    alignment->setObjectName(QStringLiteral("screenshotPinnedSelectionAlignmentMenu"));
    const bool locked = std::any_of(targets.cbegin(), targets.cend(),
                                    [](const auto& w) { return !w || w->m_lockedMode; });
    alignment->menuAction()->setEnabled(!locked);
    auto align = [this, alignment, valid, locked, count = targets.size()](
                     const char* source, SnowCanvasSelectionAlignment operation, const auto& icon,
                     bool distributes = false) {
        auto* action = alignment->addItem(text(source), icon);
        translateAction(action, source);
        action->setObjectName(
            QStringLiteral("screenshotPinnedSelectionAlign-%1").arg(int(operation)));
        action->setEnabled(!locked && (!distributes || count >= 3));
        connect(action, &QAction::triggered, this, [this, valid, operation] {
            if (valid())
                static_cast<void>(alignSelection(operation));
        });
    };
    using A = SnowCanvasSelectionAlignment;
    align("Align left", A::AlignLeft, customIcons::AlignLeft());
    align("Center horizontally", A::AlignCenterHorizontally, customIcons::AlignCenterHorizontal());
    align("Align right", A::AlignRight, customIcons::AlignRight());
    align("Distribute horizontally", A::DistributeHorizontally, customIcons::DistributeHorizontal(),
          true);
    alignment->addSeparator();
    align("Align top", A::AlignTop, customIcons::AlignTop());
    align("Center vertically", A::AlignCenterVertically, customIcons::AlignCenterVertical());
    align("Align bottom", A::AlignBottom, customIcons::AlignBottom());
    align("Distribute vertically", A::DistributeVertically, customIcons::DistributeVertical(),
          true);

    auto* group = menu->addSubMenu(text("Group"), customIcons::Group());
    translateAction(group->menuAction(), "Group");
    group->setObjectName(QStringLiteral("screenshotPinnedSelectionGroupMenu"));
    const QPointer<PinnedWindowGroupManager> manager = owner->m_groupManager;
    group->menuAction()->setEnabled(manager != nullptr);
    if (manager) {
        const auto entries = manager->displaySnapshot();
        for (const auto& entry : entries) {
            auto* action = group->addItem(entry.name);
            group->setActionBadge(
                action,
                QStringLiteral("%1/%2").arg(entry.counts.nonIgnored).arg(entry.counts.total));
            action->setObjectName(
                QStringLiteral("screenshotPinnedSelectionGroup-%1").arg(entry.id));
            action->setCheckable(true);
            action->setChecked(
                std::all_of(targets.cbegin(), targets.cend(),
                            [&entry](const auto& w) { return w && w->groupId() == entry.id; }));
            connect(action, &QAction::triggered, this,
                    [this, manager, targets, valid, id = entry.id, owner = QPointer(owner)] {
                        if (valid() && manager && !manager->moveWindows(targets, id))
                            showFailure(
                                owner, tr("The selected windows could not be moved to the group."));
                    });
        }
        group->addSeparator();
        auto* create = group->addItem(text("New Group"), outlined::FolderAdd());
        translateAction(create, "New Group");
        create->setObjectName(QStringLiteral("screenshotPinnedSelectionNewGroup"));
        connect(create, &QAction::triggered, this,
                [manager, targets, valid, owner = QPointer(owner)] {
                    if (valid() && manager && owner)
                        manager->openCreateGroupModal(owner, targets);
                });
        auto* empty = group->addItem(text("Delete Empty Groups"), outlined::Clear());
        translateAction(empty, "Delete Empty Groups");
        empty->setEnabled(std::any_of(entries.cbegin(), entries.cend(), [](const auto& entry) {
            return entry.id != QStringLiteral("default") && entry.counts.nonIgnored == 0;
        }));
        connect(empty, &QAction::triggered, this, [manager, owner = QPointer(owner), valid] {
            if (valid() && manager && owner)
                manager->openDeleteEmptyGroupsConfirmation(owner);
        });
        auto* remove = group->addSubMenu(text("Delete Specified Group"), customIcons::Delete());
        translateAction(remove->menuAction(), "Delete Specified Group");
        for (const auto& entry : entries) {
            auto* action = remove->addItem(entry.name);
            connect(action, &QAction::triggered, this,
                    [manager, owner = QPointer(owner), id = entry.id, valid] {
                        if (valid() && manager && owner)
                            manager->openDeleteSpecifiedGroupConfirmation(id, owner);
                    });
        }
    }

    auto* lock = add("Lock", QStringLiteral("screenshotPinnedSelectionLock"), outlined::Lock());
    const bool allLocked = std::all_of(targets.cbegin(), targets.cend(),
                                       [](const auto& w) { return w && w->m_lockedMode; });
    lock->setCheckable(true);
    lock->setChecked(allLocked);
    connect(lock, &QAction::triggered, this, [targets, valid] {
        if (!valid())
            return;
        const bool unlock = std::all_of(targets.cbegin(), targets.cend(),
                                        [](const auto& w) { return w && w->m_lockedMode; });
        for (const auto& window : targets)
            if (window)
                window->setLockedMode(!unlock);
    });
    menu->addSeparator();
    auto* close = add("Close", QStringLiteral("screenshotPinnedSelectionClose"), outlined::Close());
    connect(close, &QAction::triggered, this, [targets, valid] {
        if (valid())
            for (const auto& window : targets)
                if (window)
                    window->requestUserClose();
    });
    auto* destroy = add("Destroy", QStringLiteral("screenshotPinnedSelectionDestroy"),
                        customIcons::DestroyPinnedWindow());
    menu->setActionDanger(destroy);
    connect(destroy, &QAction::triggered, this, [this, targets, valid, owner = QPointer(owner)] {
        if (valid() && owner)
            confirmDestroy(owner, targets);
    });
}

void PinnedWindowSelectionController::confirmDestroy(
    ScreenshotPinnedWindow* owner, const QList<QPointer<ScreenshotPinnedWindow>>& targets) {
    if (m_destroyConfirmation) {
        m_destroyConfirmation->setOpen(true);
        return;
    }
    QVector<QString> ids;
    for (const auto& window : targets)
        if (window && !ids.contains(window->persistenceId()))
            ids.append(window->persistenceId());
    const auto destroy = [this, ids, targets, owner = QPointer(owner)] {
        if (m_destroyRecords) {
            const auto result = m_destroyRecords(ids);
            if (!result.success) {
                showFailure(owner, tr("The selected windows could not be destroyed."));
                return;
            }
        }
        for (const auto& window : targets)
            if (window && !window->m_closing && ids.contains(window->persistenceId()))
                window->requestDestroy();
    };
    if (!storage::PinToScreenSettings().confirmBeforeDestroyingWindow()) {
        destroy();
        return;
    }
    auto* modal = new adqt::widgets::AdModal(owner);
    m_destroyConfirmation = modal;
    modal->setObjectName(QStringLiteral("screenshotPinnedSelectionDestroyConfirmation"));
    modal->setOwnerWindow(owner);
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setPreset(adqt::widgets::AdModal::Preset::Confirm);
    modal->setProperty("pinnedSelectionCount", ids.size());
    modal->setAcceptAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    retranslateUi();
    connect(modal, &adqt::widgets::AdModal::accepted, this, destroy);
    connect(modal, &adqt::widgets::AdModal::finished, this, [this, modal](auto) {
        if (m_destroyConfirmation == modal)
            m_destroyConfirmation = nullptr;
        modal->deleteLater();
    });
    modal->open();
}

void PinnedWindowSelectionController::retranslateUi() {
    if (m_contextMenu) {
        for (auto* action : m_contextMenu->findChildren<QAction*>()) {
            const QByteArray source =
                action->property("pinnedSelectionTranslationSource").toString().toUtf8();
            if (!source.isEmpty())
                action->setText(text(source.constData()));
            if (action->property("pinnedSelectionCount").isValid())
                action->setText(tr("%n selected window(s)", nullptr,
                                   action->property("pinnedSelectionCount").toInt()));
        }
    }
    if (m_destroyConfirmation) {
        m_destroyConfirmation->setWindowTitle(tr("Destroy selected windows"));
        m_destroyConfirmation->setText(
            tr("Destroy %n selected window(s)? This action cannot be undone.", nullptr,
               m_destroyConfirmation->property("pinnedSelectionCount").toInt()));
        m_destroyConfirmation->setAcceptText(tr("Destroy"));
        m_destroyConfirmation->setRejectText(tr("Cancel"));
    }
}
} // namespace snow_shot::presentation
