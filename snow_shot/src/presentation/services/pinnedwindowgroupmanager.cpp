#include "snow_shot/presentation/pinnedwindowgroupmanager.h"

#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/modal.h"
#include "widgets/button.h"

#include <QApplication>
#include <QCursor>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtGlobal>

#include <algorithm>

namespace snow_shot::presentation {
namespace {
constexpr auto kDefaultGroupId = "default";
constexpr auto kDefaultGroupName = "Default";
constexpr int kMaximumGroupNameLength = 16;
constexpr auto kGroupManagerMutationProperty = "snowPinnedWindowGroupManagerMutation";

storage::PinnedWindowGroup defaultGroup() {
    return {QString::fromLatin1(kDefaultGroupId), QString::fromLatin1(kDefaultGroupName), true};
}

adqt::widgets::AdModal* createDeletionModal(QWidget* owner, QObject* lifetimeOwner,
                                            const QString& objectName) {
    auto* modal =
        new adqt::widgets::AdModal(owner != nullptr ? static_cast<QObject*>(owner) : lifetimeOwner);
    modal->setObjectName(objectName);
    modal->setOwnerWindow(owner);
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    if (owner == nullptr) {
        modal->setWindowModeDetached(true);
        QScreen* screen = QApplication::screenAt(QCursor::pos());
        modal->setWindowScreen(screen != nullptr ? screen : QApplication::primaryScreen());
    }
    modal->setWindowModality(owner != nullptr ? Qt::WindowModal : Qt::ApplicationModal);
    modal->setCentered(true);
    modal->setPreset(adqt::widgets::AdModal::Preset::Confirm);
    modal->setAcceptAccentRole(adqt::widgets::AdButton::AccentRole::Danger);
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    QObject::connect(modal, &adqt::widgets::AdModal::finished, modal, &QObject::deleteLater);
    return modal;
}
} // namespace

PinnedWindowGroupManager::PinnedWindowGroupManager(storage::PinnedWindowRepository* repository,
                                                   QObject* parent)
    : QObject(parent), m_repository(repository) {
    if (m_repository == nullptr) {
        auto& storage = storage::ApplicationStorage::instance();
        if (storage.isInitialized()) {
            m_repository = &storage.pinnedWindows();
        }
    }
    m_groups =
        m_repository != nullptr ? m_repository->groups() : QVector<storage::PinnedWindowGroup>{};
    if (m_groups.isEmpty()) {
        m_groups.push_back(defaultGroup());
    }
    if (std::none_of(m_groups.cbegin(), m_groups.cend(),
                     [](const storage::PinnedWindowGroup& group) {
                         return group.id == QString::fromLatin1(kDefaultGroupId);
                     })) {
        m_groups.push_front(defaultGroup());
    }
    m_activeGroupId = m_repository != nullptr ? m_repository->activeGroupId()
                                              : QString::fromLatin1(kDefaultGroupId);
    if (!contains(m_activeGroupId)) {
        m_activeGroupId = QString::fromLatin1(kDefaultGroupId);
    }
    // Loading the in-memory default state must not rewrite the manifest. The
    // repository owns the first durable write after an actual mutation.
}

QVector<storage::PinnedWindowGroup> PinnedWindowGroupManager::groups() const {
    return m_groups;
}

QVector<storage::PinnedWindowGroup> PinnedWindowGroupManager::groupsSortedForDisplay() const {
    const QString defaultGroupId = QString::fromLatin1(kDefaultGroupId);
    QVector<storage::PinnedWindowGroup> sorted = m_groups;
    std::sort(sorted.begin(), sorted.end(),
              [this, &defaultGroupId](const auto& first, const auto& second) {
                  if (first.id == defaultGroupId) {
                      return second.id != defaultGroupId;
                  }
                  if (second.id == defaultGroupId) {
                      return false;
                  }
                  const int comparison = QString::localeAwareCompare(normalizedDisplayName(first),
                                                                     normalizedDisplayName(second));
                  return comparison == 0 ? first.id < second.id : comparison < 0;
              });
    return sorted;
}

QString PinnedWindowGroupManager::activeGroupId() const {
    return m_activeGroupId;
}

QString
PinnedWindowGroupManager::normalizedDisplayName(const storage::PinnedWindowGroup& group) const {
    return group.id == QString::fromLatin1(kDefaultGroupId) ? tr("Default") : group.name;
}

QString PinnedWindowGroupManager::displayName(const QString& groupId) const {
    const auto it = std::find_if(m_groups.cbegin(), m_groups.cend(),
                                 [&groupId](const auto& group) { return group.id == groupId; });
    return it == m_groups.cend() ? tr("Default") : normalizedDisplayName(*it);
}

bool PinnedWindowGroupManager::contains(const QString& groupId) const {
    return std::any_of(m_groups.cbegin(), m_groups.cend(),
                       [&groupId](const auto& group) { return group.id == groupId; });
}

GroupWindowCounts PinnedWindowGroupManager::windowCounts(const QString& groupId) const {
    QSet<QString> nonIgnoredPersistedIds;
    QSet<QString> allPersistedIds;
    if (m_repository != nullptr) {
        const quint64 repositoryRevision = m_repository->membershipRevision();
        if (repositoryRevision != m_countsRevision) {
            m_persistedCounts.clear();
            m_persistedTotalCounts.clear();
            m_persistedIdsByGroup.clear();
            m_allPersistedIdsByGroup.clear();
            const QVector<storage::PinnedWindowSummary> summaries = m_repository->summaries();
            for (const storage::PinnedWindowSummary& summary : summaries) {
                ++m_persistedTotalCounts[summary.groupId];
                m_allPersistedIdsByGroup[summary.groupId].insert(summary.id);
                if (!summary.ignored) {
                    ++m_persistedCounts[summary.groupId];
                    m_persistedIdsByGroup[summary.groupId].insert(summary.id);
                }
            }
            m_countsRevision = repositoryRevision;
        }
        nonIgnoredPersistedIds = m_persistedIdsByGroup.value(groupId);
        allPersistedIds = m_allPersistedIdsByGroup.value(groupId);
    }
    GroupWindowCounts counts{m_persistedCounts.value(groupId, 0),
                             m_persistedTotalCounts.value(groupId, 0)};
    for (auto it = m_windows.cbegin(); it != m_windows.cend(); ++it) {
        if (it.value() != nullptr && it.value()->groupId() == groupId) {
            if (!m_inactiveClosing.contains(it.key()) &&
                !nonIgnoredPersistedIds.contains(it.key())) {
                ++counts.nonIgnored;
            }
            if (!allPersistedIds.contains(it.key())) {
                ++counts.total;
            }
        }
    }
    for (auto it = m_pendingGroups.cbegin(); it != m_pendingGroups.cend(); ++it) {
        if (it.value() == groupId && m_windows.value(it.key()) == nullptr) {
            if (!nonIgnoredPersistedIds.contains(it.key())) {
                ++counts.nonIgnored;
            }
            if (!allPersistedIds.contains(it.key())) {
                ++counts.total;
            }
        }
    }
    return counts;
}

int PinnedWindowGroupManager::windowCount(const QString& groupId) const {
    return windowCounts(groupId).nonIgnored;
}

void PinnedWindowGroupManager::onPinnedRecordsChanged() {
    if (m_repository == nullptr)
        return;
    if (m_repository->membershipRevision() == m_countsRevision)
        return;
    const bool hadCounts = m_countsRevision != (std::numeric_limits<quint64>::max)();
    const auto previousCounts = m_persistedCounts;
    const auto previousTotals = m_persistedTotalCounts;
    static_cast<void>(windowCounts(m_activeGroupId));
    if (!hadCounts || previousCounts != m_persistedCounts ||
        previousTotals != m_persistedTotalCounts) {
        scheduleGroupsChanged();
    }
}

bool PinnedWindowGroupManager::hasWindow(const QString& persistenceId) const {
    const auto it = m_windows.constFind(persistenceId);
    return it != m_windows.cend() && it.value() != nullptr &&
           !m_inactiveClosing.contains(persistenceId);
}

bool PinnedWindowGroupManager::persist() {
    return m_repository == nullptr || m_repository->setGroups(m_groups, m_activeGroupId).success;
}

bool PinnedWindowGroupManager::setActiveGroup(const QString& groupId) {
    if (!contains(groupId) || m_activeGroupId == groupId) {
        return contains(groupId);
    }
    if (m_repository != nullptr && !m_repository->setActiveGroup(groupId).success) {
        return false;
    }
    m_activeGroupId = groupId;
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if (it.value() == nullptr) {
            it = m_windows.erase(it);
            continue;
        }
        ScreenshotPinnedWindow* window = it.value();
        if (window->groupId() != m_activeGroupId) {
            m_inactiveClosing.insert(it.key());
            QMetaObject::invokeMethod(window, "closeForInactiveGroup", Qt::DirectConnection);
        } else {
            m_inactiveClosing.remove(it.key());
            QMetaObject::invokeMethod(window, "cancelDeferredInactiveGroupClose",
                                      Qt::DirectConnection);
        }
        ++it;
    }
    emit activeGroupChanged(m_activeGroupId);
    restoreActiveGroupWindows();
    return true;
}

std::optional<QString>
PinnedWindowGroupManager::createGroup(const QString& name,
                                      ::ScreenshotPinnedWindow* currentWindow) {
    const QString normalized = name.trimmed();
    if (normalized.isEmpty() || normalized.size() > kMaximumGroupNameLength ||
        std::any_of(m_groups.cbegin(), m_groups.cend(),
                    [&normalized](const auto& group) {
                        return group.name.trimmed().compare(normalized, Qt::CaseInsensitive) == 0;
                    }) ||
        (m_groups.size() >= storage::PinnedWindowRepository::maximumGroupCount())) {
        return std::nullopt;
    }
    storage::PinnedWindowGroup group;
    group.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    group.name = normalized;
    group.builtIn = false;
    m_groups.push_back(group);
    if (!persist()) {
        m_groups.removeLast();
        return std::nullopt;
    }
    scheduleGroupsChanged();
    if (currentWindow != nullptr) {
        if (!moveWindow(currentWindow, group.id)) {
            m_groups.removeLast();
            if (!persist()) {
                // The group was already persisted successfully. Keep the in-memory
                // state aligned with the durable state if the compensating flush fails.
                m_groups.push_back(group);
            }
            scheduleGroupsChanged();
            return std::nullopt;
        }
    }
    return group.id;
}

QString PinnedWindowGroupManager::uniqueGeneratedName() const {
    int index = std::max(1, static_cast<int>(m_groups.size()));
    for (;;) {
        const QString candidate = tr("Group %1").arg(index);
        const bool exists =
            std::any_of(m_groups.cbegin(), m_groups.cend(), [&candidate](const auto& group) {
                return group.name.trimmed().compare(candidate, Qt::CaseInsensitive) == 0;
            });
        if (!exists) {
            return candidate;
        }
        ++index;
    }
}

bool PinnedWindowGroupManager::deleteEmptyGroups() {
    const auto groups = m_groups;
    bool changed = false;
    for (const auto& group : groups) {
        if (group.id != QString::fromLatin1(kDefaultGroupId) && windowCount(group.id) == 0)
            changed = deleteSpecifiedGroup(group.id) || changed;
    }
    return changed;
}

bool PinnedWindowGroupManager::deleteSpecifiedGroup(const QString& groupId) {
    if (!contains(groupId)) {
        return false;
    }
    if (m_repository != nullptr && !m_repository->removeGroupAndRecords(groupId).success) {
        return false;
    }

    const bool removesGroup = groupId != QString::fromLatin1(kDefaultGroupId);
    const bool activeRemoved = removesGroup && m_activeGroupId == groupId;
    if (removesGroup) {
        m_groups.erase(
            std::remove_if(m_groups.begin(), m_groups.end(),
                           [&groupId](const auto& group) { return group.id == groupId; }),
            m_groups.end());
    }
    for (auto it = m_pendingGroups.begin(); it != m_pendingGroups.end();) {
        if (it.value() == groupId) {
            it = m_pendingGroups.erase(it);
        } else {
            ++it;
        }
    }
    if (activeRemoved) {
        m_activeGroupId = QString::fromLatin1(kDefaultGroupId);
    }

    emit groupDeletionRequested(groupId);
    scheduleGroupsChanged();
    if (activeRemoved) {
        emit activeGroupChanged(m_activeGroupId);
        restoreActiveGroupWindows();
    }
    return true;
}

void PinnedWindowGroupManager::openDeleteEmptyGroupsConfirmation(QWidget* owner) {
    const bool hasEmptyGroup =
        std::any_of(m_groups.cbegin(), m_groups.cend(), [this](const auto& group) {
            return !group.builtIn && windowCount(group.id) == 0;
        });
    if (!hasEmptyGroup) {
        return;
    }
    auto* modal =
        createDeletionModal(owner, this, QStringLiteral("pinnedWindowGroupDeleteEmptyModal"));
    const auto updateText = [modal]() {
        modal->setWindowTitle(tr("Delete empty groups"));
        modal->setText(
            tr("Delete every group with no pinned windows other than closed ones? Closed pinned "
               "windows saved in those groups will also be permanently deleted. This "
               "action cannot be undone."));
        modal->setAcceptText(tr("Delete groups"));
        modal->setRejectText(tr("Cancel"));
    };
    updateText();
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged, modal,
            [manager = QPointer<PinnedWindowGroupManager>(this), updateText](const QString&,
                                                                             const QLocale&) {
                if (manager != nullptr) {
                    updateText();
                }
            });
    connect(modal, &adqt::widgets::AdModal::accepted, this,
            [this]() { static_cast<void>(deleteEmptyGroups()); });
    modal->open();
}

void PinnedWindowGroupManager::openDeleteSpecifiedGroupConfirmation(const QString& groupId,
                                                                    QWidget* owner) {
    if (!contains(groupId)) {
        return;
    }
    const bool isDefault = groupId == QString::fromLatin1(kDefaultGroupId);
    auto* modal =
        createDeletionModal(owner, this, QStringLiteral("pinnedWindowGroupDeleteSpecifiedModal"));
    const auto updateText = [this, modal, groupId, isDefault]() {
        modal->setWindowTitle(isDefault ? tr("Clear Default group") : tr("Delete group"));
        modal->setText(
            isDefault
                ? tr("Delete all pinned windows in \"%1\", including closed windows? The Default "
                     "group will remain. This action cannot be undone.")
                      .arg(displayName(groupId))
                : tr("Delete \"%1\" and all its pinned windows, including closed windows? This "
                     "action cannot be undone.")
                      .arg(displayName(groupId)));
        modal->setAcceptText(isDefault ? tr("Clear group") : tr("Delete group"));
        modal->setRejectText(tr("Cancel"));
    };
    updateText();
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged, modal,
            [manager = QPointer<PinnedWindowGroupManager>(this), updateText](const QString&,
                                                                             const QLocale&) {
                if (manager != nullptr) {
                    updateText();
                }
            });
    connect(modal, &adqt::widgets::AdModal::accepted, this,
            [this, groupId]() { static_cast<void>(deleteSpecifiedGroup(groupId)); });
    modal->open();
}

bool PinnedWindowGroupManager::showWindow(const QString& id) {
    const auto window = m_windows.value(id);
    if (!hasWindow(id) || window == nullptr)
        return false;
    QMetaObject::invokeMethod(window, "cancelDeferredInactiveGroupClose", Qt::DirectConnection);
    QMetaObject::invokeMethod(window, "showFromManagement", Qt::DirectConnection);
    return true;
}

void PinnedWindowGroupManager::markWindowClosing(ScreenshotPinnedWindow* window) {
    const auto key = windowKey(window);
    m_inactiveClosing.insert(key);
    m_pendingGroups.remove(key);
    scheduleGroupsChanged();
}

void PinnedWindowGroupManager::destroyWindow(const QString& id) {
    if (const auto window = m_windows.value(id))
        QMetaObject::invokeMethod(window, "requestDestroy", Qt::DirectConnection);
    m_pendingGroups.remove(id);
    scheduleGroupsChanged();
}

void PinnedWindowGroupManager::restoreActiveGroupWindows() {
    emit restoreActiveGroupWindowsRequested();
}

bool PinnedWindowGroupManager::moveWindow(::ScreenshotPinnedWindow* window,
                                          const QString& groupId) {
    if (window == nullptr || !contains(groupId)) {
        return false;
    }
    const QString previousGroupId = window->groupId();
    window->setProperty(kGroupManagerMutationProperty, true);
    const bool groupChanged = QMetaObject::invokeMethod(window, "setGroupId", Qt::DirectConnection,
                                                        Q_ARG(QString, groupId));
    window->setProperty(kGroupManagerMutationProperty, false);
    if (!groupChanged) {
        return false;
    }
    if (m_repository != nullptr && !window->persistenceId().isEmpty()) {
        const QString persistenceId = window->persistenceId();
        const QVector<storage::PinnedWindowSummary> summaries = m_repository->summaries();
        const bool hasPersistedRecord = std::any_of(
            summaries.cbegin(), summaries.cend(),
            [&persistenceId](const auto& summary) { return summary.id == persistenceId; });
        if (hasPersistedRecord && !m_repository->setRecordGroup(persistenceId, groupId).success) {
            window->setProperty(kGroupManagerMutationProperty, true);
            static_cast<void>(QMetaObject::invokeMethod(window, "setGroupId", Qt::DirectConnection,
                                                        Q_ARG(QString, previousGroupId)));
            window->setProperty(kGroupManagerMutationProperty, false);
            return false;
        }
    }
    registerWindow(window, groupId);
    scheduleGroupsChanged();
    if (groupId != m_activeGroupId) {
        m_inactiveClosing.insert(windowKey(window));
        QMetaObject::invokeMethod(window, "closeForInactiveGroup", Qt::DirectConnection);
    } else {
        m_inactiveClosing.remove(windowKey(window));
        QMetaObject::invokeMethod(window, "cancelDeferredInactiveGroupClose", Qt::DirectConnection);
    }
    return true;
}

void PinnedWindowGroupManager::registerWindow(::ScreenshotPinnedWindow* window,
                                              const QString& groupId) {
    if (window == nullptr || !contains(groupId)) {
        return;
    }
    const QString key = windowKey(window);
    if (m_windows.value(key) == window) {
        return;
    }
    m_windows.insert(key, QPointer<::ScreenshotPinnedWindow>(window));
    m_inactiveClosing.remove(key);
    const ScreenshotPinnedWindow* identity = window;
    QObject::connect(window, &QObject::destroyed, this, [this, key, identity]() {
        const auto it = m_windows.find(key);
        if (it != m_windows.end() && (it.value().isNull() || it.value().data() == identity)) {
            m_windows.erase(it);
            m_inactiveClosing.remove(key);
        }
        scheduleGroupsChanged();
    });
    scheduleGroupsChanged();
}

void PinnedWindowGroupManager::unregisterWindow(::ScreenshotPinnedWindow* window) {
    if (window == nullptr) {
        return;
    }
    const QString key = windowKey(window);
    m_windows.remove(key);
    m_inactiveClosing.remove(key);
    scheduleGroupsChanged();
}

void PinnedWindowGroupManager::registerPendingPin(const QString& persistenceId,
                                                  const QString& groupId) {
    if (persistenceId.isEmpty() || !contains(groupId) ||
        m_pendingGroups.value(persistenceId) == groupId) {
        return;
    }
    m_pendingGroups.insert(persistenceId, groupId);
    scheduleGroupsChanged();
}

void PinnedWindowGroupManager::completePendingPin(const QString& persistenceId) {
    const bool existed = m_pendingGroups.contains(persistenceId);
    m_pendingGroups.remove(persistenceId);
    if (existed) {
        scheduleGroupsChanged();
    }
}

void PinnedWindowGroupManager::scheduleGroupsChanged() {
    if (m_groupsChangedScheduled) {
        return;
    }
    m_groupsChangedScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_groupsChangedScheduled = false;
        emit groupsChanged();
    });
}

QString PinnedWindowGroupManager::windowKey(::ScreenshotPinnedWindow* window) const {
    if (window == nullptr) {
        return {};
    }
    const QString persistenceId = window->persistenceId();
    if (!persistenceId.isEmpty()) {
        return persistenceId;
    }
    return QStringLiteral("runtime:%1")
        .arg(QString::number(reinterpret_cast<quintptr>(window), 16));
}

void PinnedWindowGroupManager::openCreateGroupModal(QWidget* owner,
                                                    ::ScreenshotPinnedWindow* currentWindow) {
    auto* form = new adqt::widgets::AdForm();
    form->setObjectName(QStringLiteral("pinnedWindowGroupCreateForm"));
    form->setFixedWidth(352);
    form->setFormLayout(adqt::widgets::AdForm::FormLayout::Vertical);
    form->setLabelAlign(adqt::widgets::AdForm::LabelAlign::Left);
    form->setRequiredMark(adqt::widgets::AdForm::RequiredMark::Visible);
    form->setControlSize(adqt::widgets::AdForm::ControlSize::Medium);
    form->setVariant(adqt::widgets::AdForm::Variant::Outlined);
    form->setColon(false);
    form->setScrollToFirstError(true);

    auto* input = new adqt::widgets::AdLineEdit(form);
    input->setObjectName(QStringLiteral("pinnedWindowGroupNameInput"));
    input->setMaxLength(kMaximumGroupNameLength);
    input->setAllowClear(true);
    input->setText(uniqueGeneratedName());
    auto* item = form->addField(tr("Group name"), input, QStringLiteral("groupName"));
    item->setItemLayout(adqt::widgets::AdFormItem::ItemLayout::Vertical);
    item->setRequired(true);
    item->setRequiredMessage(tr("Please enter a group name"));
    item->setFormValidator([this](const QVariant& value, adqt::widgets::AdFormItem*) {
        adqt::widgets::AdFormItem::ValidationResult result;
        const QString name = value.toString().trimmed();
        if (name.isEmpty()) {
            result.status = adqt::widgets::AdFormItem::ValidateStatus::Error;
            result.errors.push_back(tr("Please enter a group name"));
        } else if (name.size() > kMaximumGroupNameLength ||
                   std::any_of(m_groups.cbegin(), m_groups.cend(), [&name](const auto& group) {
                       return group.name.trimmed().compare(name, Qt::CaseInsensitive) == 0;
                   })) {
            result.status = adqt::widgets::AdFormItem::ValidateStatus::Error;
            result.errors.push_back(tr("This group name is already in use"));
        }
        return result;
    });

    auto* modal = new adqt::widgets::AdModal(owner);
    modal->setObjectName(QStringLiteral("pinnedWindowGroupCreateModal"));
    modal->setOwnerWindow(owner);
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    if (owner == nullptr) {
        // Tray creation belongs to the cursor's screen, independent of visible pinned windows.
        modal->setWindowModeDetached(true);
        QScreen* screen = QApplication::screenAt(QCursor::pos());
        modal->setWindowScreen(screen != nullptr ? screen : QApplication::primaryScreen());
    }
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setWindowTitle(tr("New Group"));
    modal->setCentered(true);
    modal->setPreferredWidth(400);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setAcceptText(tr("Add"));
    modal->setRejectText(tr("Cancel"));
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    modal->setContentWidget(form);
    modal->setInitialFocusWidget(input);
    const QPointer<adqt::widgets::AdForm> formGuard(form);
    const QPointer<adqt::widgets::AdLineEdit> inputGuard(input);
    const QPointer<::ScreenshotPinnedWindow> currentWindowGuard(currentWindow);
    connect(modal, &adqt::widgets::AdModal::closeRequested, modal,
            [this, modal, formGuard, inputGuard,
             currentWindowGuard](adqt::widgets::AdModal::CloseReason reason) {
                if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                    modal->reject();
                    return;
                }
                if (formGuard == nullptr || inputGuard == nullptr || !formGuard->submit()) {
                    return;
                }
                if (!createGroup(inputGuard->text(), currentWindowGuard.data())) {
                    return;
                }
                modal->accept();
            });
    connect(modal, &adqt::widgets::AdModal::finished, modal, &QObject::deleteLater);
    modal->open();
    input->focusEditor(adqt::widgets::AdLineEdit::FocusSelection::SelectAll);
}
} // namespace snow_shot::presentation
