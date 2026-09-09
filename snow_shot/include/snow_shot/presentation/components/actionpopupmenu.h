#pragma once

#include "theme/theme_manager.h"
#include "widgets/context_menu.h"

#include <QAbstractButton>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <utility>

namespace snow_shot::presentation {

// Shared hover/click menu behavior for compact action triggers.
class ActionPopupMenu final : public QObject {
  public:
    enum class Placement { BottomLeft, TopRight };
    ActionPopupMenu(QAbstractButton* trigger,
                    std::function<adqt::widgets::AdContextMenu*()> createMenu,
                    Placement placement = Placement::BottomLeft)
        : QObject(trigger), m_trigger(trigger), m_createMenu(std::move(createMenu)),
          m_placement(placement) {
        m_closeTimer.setSingleShot(true);
        m_closeTimer.setInterval(150);
        connect(&m_closeTimer, &QTimer::timeout, this, [this] {
            if (m_menu && !contains(QCursor::pos()))
                m_menu->hide();
        });
        trigger->installEventFilter(this);
        connect(trigger, &QAbstractButton::clicked, this, [this] { open(); });
    }

    ~ActionPopupMenu() override {
        if (m_menu)
            m_menu->hide();
    }

    void open(bool keyboard = false) {
        m_closeTimer.stop();
        m_keyboard = keyboard;
        if (!m_trigger->isVisible() || !m_trigger->isEnabled())
            return;
        if (!m_menu || !m_menu->isVisible()) {
            m_menu = m_createMenu();
            if (!m_menu)
                return;
            m_menu->setTriggerWidget(m_trigger);
            m_menu->installEventFilter(this);
            connect(m_menu, &QMenu::aboutToHide, &m_closeTimer, &QTimer::stop,
                    Qt::UniqueConnection);
            const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(m_trigger);
            auto tokens = m_menu->componentTokens();
            tokens.minimumWidth = 0;
            tokens.text = theme.colorPrimary;
            tokens.hoverText = theme.colorPrimary;
            m_menu->setComponentTokens(tokens);
            QPoint position(0, m_trigger->height());
            if (m_placement == Placement::TopRight) {
                const QSize size = m_menu->sizeHint();
                position = QPoint(m_trigger->width() - size.width(), -size.height());
            }
            m_menu->popupAt(m_trigger->mapToGlobal(position));
        }
        if (keyboard) {
            for (auto* action : m_menu->actions()) {
                if (action->isEnabled() && action->isVisible() && !action->isSeparator()) {
                    m_menu->setActiveAction(action);
                    break;
                }
            }
            m_menu->setFocus(Qt::PopupFocusReason);
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_trigger && event->type() == QEvent::Enter) {
            open();
        }
        if (watched == m_trigger && event->type() == QEvent::Hide && m_menu)
            m_menu->hide();
        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (watched == m_trigger &&
                (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
                 key->key() == Qt::Key_Space || key->key() == Qt::Key_Up ||
                 key->key() == Qt::Key_Down)) {
                open(true);
                return true;
            }
            if (watched == m_menu) {
                m_keyboard = true;
                m_closeTimer.stop();
            }
            if (watched == m_menu && key->key() == Qt::Key_Escape) {
                m_menu->hide();
                m_trigger->setFocus(Qt::PopupFocusReason);
                return true;
            }
        }
        if (m_menu && m_menu->isVisible()) {
            if (event->type() == QEvent::Enter) {
                m_closeTimer.stop();
            } else if (event->type() == QEvent::Leave && !m_keyboard) {
                m_closeTimer.start();
            } else if (event->type() == QEvent::MouseMove) {
                m_keyboard = false;
                const auto* mouse = static_cast<QMouseEvent*>(event);
                // Native menus also grab mouse moves outside their popup window.
                if (contains(mouse->globalPosition().toPoint()))
                    m_closeTimer.stop();
                else if (!m_closeTimer.isActive())
                    m_closeTimer.start();
            }
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    bool contains(const QPoint& position) const {
        return m_menu && (m_menu->rect().contains(m_menu->mapFromGlobal(position)) ||
                          m_trigger->rect().contains(m_trigger->mapFromGlobal(position)));
    }
    QAbstractButton* m_trigger;
    QPointer<adqt::widgets::AdContextMenu> m_menu;
    std::function<adqt::widgets::AdContextMenu*()> m_createMenu;
    Placement m_placement;
    QTimer m_closeTimer;
    bool m_keyboard = false;
};
} // namespace snow_shot::presentation
