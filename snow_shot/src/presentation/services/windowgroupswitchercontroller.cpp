#include "snow_shot/presentation/windowgroupswitchercontroller.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/windowgroupswitcherpopup.h"
#include <QApplication>
#include <QCursor>
#include <QScreen>
#include <algorithm>

namespace snow_shot::presentation {
WindowGroupSwitcherController::WindowGroupSwitcherController(GlobalShortcutManager& shortcuts,
                                                             PinnedWindowGroupManager& groups,
                                                             QObject* parent)
    : QObject(parent), m_shortcuts(shortcuts), m_groups(groups) {
    m_timer.setInterval(8);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &WindowGroupSwitcherController::pollInput);
    connect(&groups, &PinnedWindowGroupManager::groupsChanged, this,
            &WindowGroupSwitcherController::refreshGroups);
    connect(&groups, &PinnedWindowGroupManager::activeGroupChanged, this, [this] { cancel(); });
    connect(&shortcuts, &GlobalShortcutManager::registrationsSuspended, this,
            &WindowGroupSwitcherController::cancel);
    connect(&shortcuts, &GlobalShortcutManager::globalHotkeysEnabledChanged, this,
            [this](bool enabled) {
                if (!enabled)
                    cancel();
            });
    connect(&shortcuts, &GlobalShortcutManager::stateChanged, this,
            [this](GlobalShortcutAction action) {
                if (action == GlobalShortcutAction::SwitchWindowGroup && m_timer.isActive())
                    pollInput();
            });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
        if (screen == m_screen)
            cancel();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        finish();
        m_timer.stop();
        m_participating.clear();
        m_draining = false;
    });
}
WindowGroupSwitcherController::~WindowGroupSwitcherController() = default;
bool WindowGroupSwitcherController::isVisible() const {
    return m_popup && m_popup->isVisible();
}
QString WindowGroupSwitcherController::selectedGroupId() const {
    return m_selectedId;
}
bool WindowGroupSwitcherController::isTrackingInput() const {
    return m_timer.isActive();
}
WindowGroupSwitcherPopup* WindowGroupSwitcherController::popup() const {
    return m_popup.get();
}
void WindowGroupSwitcherController::begin(bool shortcutMode) {
    m_entries = m_groups.displaySnapshot();
    if (m_entries.isEmpty())
        return;
    m_screen = QGuiApplication::screenAt(QCursor::pos());
    if (!m_screen)
        m_screen = QGuiApplication::primaryScreen();
    if (!m_screen)
        return;
    if (!m_popup) {
        m_popup = std::make_unique<WindowGroupSwitcherPopup>();
        connect(m_popup.get(), &WindowGroupSwitcherPopup::groupClicked, this,
                &WindowGroupSwitcherController::selectGroup);
        connect(m_popup.get(), &WindowGroupSwitcherPopup::languageChanged, this,
                &WindowGroupSwitcherController::refreshGroups);
    }
    m_popup->setGroups(m_entries, m_groups.activeGroupId());
    m_popup->setShortcutMode(shortcutMode);
    m_selectedId = m_groups.activeGroupId();
    advance();
    m_escapeDown = m_shortcuts.inputState(0).value_or(GlobalShortcutInputState{}).escapeDown;
    m_popup->showOnScreen(m_screen);
    m_popup->setSelectedGroup(m_selectedId);
    m_timer.start();
}
void WindowGroupSwitcherController::openPicker() {
    if (m_draining)
        pollInput();
    if (m_draining || isVisible())
        return;
    begin(false);
}
void WindowGroupSwitcherController::activateShortcut(int registrationId) {
    if (m_draining) {
        // A new activation must not turn a click/Escape dismissal into a second switch.
        pollInput();
        if (m_draining) {
            m_participating.insert(registrationId);
            return;
        }
    }
    if (!m_shortcuts.inputState(registrationId)) {
        cancel();
        return;
    }
    m_participating.insert(registrationId);
    if (isVisible()) {
        m_popup->setShortcutMode(true);
        advance();
    } else {
        begin(true);
        if (!isVisible())
            m_participating.clear();
    }
}
void WindowGroupSwitcherController::advance() {
    if (m_entries.isEmpty())
        return;
    const auto current =
        std::find_if(m_entries.cbegin(), m_entries.cend(),
                     [this](const auto& group) { return group.id == m_selectedId; });
    const auto next = current == m_entries.cend() || std::next(current) == m_entries.cend()
                          ? m_entries.cbegin()
                          : std::next(current);
    m_selectedId = next->id;
    m_popup->setSelectedGroup(m_selectedId);
}
void WindowGroupSwitcherController::refreshGroups() {
    if (!isVisible())
        return;
    if (!m_groups.contains(m_selectedId)) {
        cancel();
        return;
    }
    m_entries = m_groups.displaySnapshot();
    m_popup->setGroups(m_entries, m_groups.activeGroupId());
    m_popup->setSelectedGroup(m_selectedId);
}
void WindowGroupSwitcherController::pollInput() {
    if (!isVisible() && !m_draining) {
        m_timer.stop();
        return;
    }
    if (isVisible() && !m_groups.contains(m_selectedId)) {
        cancel();
        return;
    }
    const auto escape = m_shortcuts.inputState(0);
    bool unavailable = !escape.has_value();
    bool anyDown = false;
    bool escapeBelongsToShortcut = false;
    for (int id : std::as_const(m_participating)) {
        const auto state = m_shortcuts.inputState(id);
        if (!state) {
            unavailable = true;
            break;
        }
        anyDown |= state->anyShortcutKeyDown;
        escapeBelongsToShortcut |= state->escapeIsShortcutKey;
    }
    if (unavailable) {
        m_participating.clear();
        finish();
        m_draining = false;
        m_timer.stop();
        return;
    }
    if (m_draining) {
        if (!anyDown) {
            m_draining = false;
            m_participating.clear();
            m_timer.stop();
        }
        return;
    }
    const bool cancelPressed = escape->escapeDown && !m_escapeDown && !escapeBelongsToShortcut;
    m_escapeDown = escape->escapeDown;
    if (cancelPressed) {
        cancel();
        return;
    }
    if (!m_participating.isEmpty() && !anyDown) {
        m_participating.clear();
        finish(m_selectedId);
    }
}
void WindowGroupSwitcherController::cancel() {
    if (isVisible())
        finish();
}
void WindowGroupSwitcherController::selectGroup(const QString& id) {
    if (!isVisible())
        return;
    if (!m_groups.contains(id)) {
        cancel();
        return;
    }
    finish(id);
}
void WindowGroupSwitcherController::finish(const QString& commitId) {
    // Copy before clearing selection: release confirmation passes m_selectedId itself.
    const QString target = commitId;
    m_timer.stop();
    if (m_popup)
        m_popup->hide();
    m_selectedId.clear();
    m_screen.clear();
    m_draining = !m_participating.isEmpty();
    if (m_draining)
        m_timer.start();
    if (!target.isEmpty() && !m_groups.setActiveGroup(target))
        emit errorOccurred(tr("Could not switch window group. Please try again."));
}
} // namespace snow_shot::presentation
