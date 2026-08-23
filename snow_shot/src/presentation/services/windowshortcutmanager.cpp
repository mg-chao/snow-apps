#include "snow_shot/presentation/windowshortcutmanager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
namespace {

Qt::KeyboardModifier modifierForKey(const Qt::Key key) {
    switch (key) {
    case Qt::Key_Shift:
        return Qt::ShiftModifier;
    case Qt::Key_Control:
        return Qt::ControlModifier;
    case Qt::Key_Alt:
        return Qt::AltModifier;
    case Qt::Key_Meta:
        return Qt::MetaModifier;
    case Qt::Key_AltGr:
        return Qt::GroupSwitchModifier;
    default:
        return Qt::NoModifier;
    }
}

bool keyCombinationMatches(const QKeyCombination combination, const QKeyEvent& event,
                           Qt::KeyboardModifiers allowedAdditionalModifiers) {
    if (combination.key() != Qt::Key(event.key())) {
        return false;
    }

    const Qt::KeyboardModifiers required = combination.keyboardModifiers();
    // Qt may deliver a modifier-key press before it includes that key in
    // QKeyEvent::modifiers(). Treat the physical key as its own modifier,
    // while continuing to require every other modifier in the combination.
    Qt::KeyboardModifiers actual = event.modifiers();
    actual |= modifierForKey(Qt::Key(event.key()));
    const Qt::KeyboardModifiers unexpected = actual & ~(required | allowedAdditionalModifiers);
    return (actual & required) == required && unexpected == Qt::NoModifier;
}

bool releasedKeyEndsCombination(const QKeyCombination combination, const QKeyEvent& event) {
    // The trigger key identifies the held action. Modifier flags may already
    // be cleared, or modifiers may have been released in a different order.
    return combination.key() == Qt::Key(event.key());
}

QList<QKeyCombination> normalizedCombinations(const QList<QKeyCombination>& combinations) {
    QList<QKeyCombination> result;
    QSet<int> seen;
    result.reserve(combinations.size());
    for (const QKeyCombination combination : combinations) {
        if (combination.key() == Qt::Key_unknown) {
            continue;
        }
        const int combined = combination.toCombined();
        if (!seen.contains(combined)) {
            seen.insert(combined);
            result.push_back(combination);
        }
    }
    return result;
}

} // namespace

struct WindowShortcutManager::Impl {
    struct RegisteredBinding {
        BindingHandle handle = 0;
        quint64 order = 0;
        QPointer<QObject> owner;
        Binding binding;
        QList<QKeyCombination> activeReleaseCombinations;
    };

    struct Candidate {
        BindingHandle handle = 0;
        int priority = 0;
        quint64 order = 0;
        QKeyCombination combination;
    };

    explicit Impl(WindowShortcutManager& manager) : q(manager) {}

    [[nodiscard]] bool containsReceiver(QObject* receiver) {
        m_scopeWindows.erase(
            std::remove_if(m_scopeWindows.begin(), m_scopeWindows.end(),
                           [](const QPointer<QWidget>& window) { return window.isNull(); }),
            m_scopeWindows.end());

        auto* widget = qobject_cast<QWidget*>(receiver);
        if (widget == nullptr) {
            return false;
        }
        QWidget* receiverWindow = widget->window();
        return std::any_of(m_scopeWindows.cbegin(), m_scopeWindows.cend(),
                           [receiverWindow](const QPointer<QWidget>& scopeWindow) {
                               return scopeWindow != nullptr &&
                                      scopeWindow->window() == receiverWindow;
                           });
    }

    [[nodiscard]] RegisteredBinding* findBinding(BindingHandle handle) {
        const auto binding = std::find_if(
            m_bindings.begin(), m_bindings.end(),
            [handle](const RegisteredBinding& item) { return item.handle == handle; });
        return binding != m_bindings.end() ? &*binding : nullptr;
    }

    [[nodiscard]] QVector<Candidate> candidates(const QKeyEvent& event) {
        QVector<Candidate> result;
        for (RegisteredBinding& registered : m_bindings) {
            if (registered.owner == nullptr ||
                (event.isAutoRepeat() && !registered.binding.autoRepeat)) {
                continue;
            }
            const auto match = std::find_if(
                registered.binding.keyCombinations.cbegin(),
                registered.binding.keyCombinations.cend(),
                [&event, &registered](QKeyCombination combination) {
                    return keyCombinationMatches(combination, event,
                                                 registered.binding.allowedAdditionalModifiers);
                });
            if (match != registered.binding.keyCombinations.cend()) {
                result.push_back(Candidate{registered.handle, registered.binding.priority,
                                           registered.order, *match});
            }
        }
        sortCandidates(&result);
        return result;
    }

    [[nodiscard]] QVector<Candidate> releaseCandidates(const QKeyEvent& event) {
        QVector<Candidate> result;
        if (event.isAutoRepeat()) {
            return result;
        }
        for (RegisteredBinding& registered : m_bindings) {
            if (registered.owner == nullptr || registered.activeReleaseCombinations.isEmpty()) {
                continue;
            }
            const auto match = std::find_if(
                registered.activeReleaseCombinations.cbegin(),
                registered.activeReleaseCombinations.cend(),
                [&event](QKeyCombination combination) {
                    return releasedKeyEndsCombination(combination, event);
                });
            if (match != registered.activeReleaseCombinations.cend()) {
                result.push_back(Candidate{registered.handle, registered.binding.priority,
                                           registered.order, *match});
            }
        }
        sortCandidates(&result);
        return result;
    }

    static void sortCandidates(QVector<Candidate>* candidates) {
        if (candidates == nullptr) {
            return;
        }
        std::stable_sort(candidates->begin(), candidates->end(), [](const Candidate& left,
                                                                    const Candidate& right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            return left.order < right.order;
        });
    }

    WindowShortcutManager& q;
    QList<QPointer<QWidget>> m_scopeWindows;
    QVector<RegisteredBinding> m_bindings;
    BindingHandle m_nextHandle = 1;
    quint64 m_nextOrder = 1;
};

WindowShortcutManager::WindowShortcutManager(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {
    if (QCoreApplication* application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
}

WindowShortcutManager::~WindowShortcutManager() {
    if (QCoreApplication* application = QCoreApplication::instance()) {
        application->removeEventFilter(this);
    }
}

void WindowShortcutManager::addScopeWindow(QWidget* window) {
    if (window == nullptr) {
        return;
    }
    QWidget* root = window->window();
    const bool alreadyRegistered =
        std::any_of(m_impl->m_scopeWindows.cbegin(), m_impl->m_scopeWindows.cend(),
                    [root](const QPointer<QWidget>& existing) { return existing == root; });
    if (alreadyRegistered) {
        return;
    }
    m_impl->m_scopeWindows.push_back(root);
    connect(root, &QObject::destroyed, this, [this]() {
        m_impl->m_scopeWindows.erase(
            std::remove_if(m_impl->m_scopeWindows.begin(), m_impl->m_scopeWindows.end(),
                           [](const QPointer<QWidget>& item) { return item.isNull(); }),
            m_impl->m_scopeWindows.end());
    });
}

void WindowShortcutManager::removeScopeWindow(QWidget* window) {
    QWidget* root = window != nullptr ? window->window() : nullptr;
    m_impl->m_scopeWindows.erase(
        std::remove_if(m_impl->m_scopeWindows.begin(), m_impl->m_scopeWindows.end(),
                       [root](const QPointer<QWidget>& item) {
                           return item.isNull() || item == root;
                       }),
        m_impl->m_scopeWindows.end());
}

WindowShortcutManager::BindingHandle WindowShortcutManager::addBinding(QObject* owner,
                                                                        Binding binding) {
    if (owner == nullptr || !binding.activate) {
        return 0;
    }
    binding.keyCombinations = normalizedCombinations(binding.keyCombinations);

    const BindingHandle handle = m_impl->m_nextHandle++;
    m_impl->m_bindings.push_back(
        Impl::RegisteredBinding{handle, m_impl->m_nextOrder++, owner, std::move(binding), {}});
    connect(owner, &QObject::destroyed, this,
            [this, handle]() { static_cast<void>(removeBinding(handle)); });
    return handle;
}

bool WindowShortcutManager::setKeyCombinations(
    BindingHandle handle, const QList<QKeyCombination>& keyCombinations) {
    const auto binding = std::find_if(
        m_impl->m_bindings.begin(), m_impl->m_bindings.end(),
        [handle](const Impl::RegisteredBinding& item) { return item.handle == handle; });
    if (binding == m_impl->m_bindings.end()) {
        return false;
    }
    binding->binding.keyCombinations = normalizedCombinations(keyCombinations);
    return true;
}

bool WindowShortcutManager::removeBinding(BindingHandle handle) {
    const auto previousSize = m_impl->m_bindings.size();
    m_impl->m_bindings.erase(
        std::remove_if(m_impl->m_bindings.begin(), m_impl->m_bindings.end(),
                       [handle](const Impl::RegisteredBinding& item) {
                           return item.handle == handle;
                       }),
        m_impl->m_bindings.end());
    return m_impl->m_bindings.size() != previousSize;
}

QList<QKeyCombination>
WindowShortcutManager::keyCombinationsFromPortableText(const QStringList& shortcuts) {
    QList<QKeyCombination> combinations;
    combinations.reserve(shortcuts.size());
    for (const QString& shortcut : shortcuts) {
        if (shortcut.trimmed().compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0) {
            combinations.push_back(QKeyCombination(Qt::ShiftModifier, Qt::Key_Shift));
            continue;
        }
        QKeySequence sequence =
            QKeySequence::fromString(shortcut.trimmed(), QKeySequence::PortableText);
        if (sequence.isEmpty()) {
            sequence = QKeySequence::fromString(shortcut.trimmed(), QKeySequence::NativeText);
        }
        if (sequence.count() == 1) {
            combinations.push_back(sequence[0]);
        }
    }
    return normalizedCombinations(combinations);
}

bool WindowShortcutManager::eventFilter(QObject* watched, QEvent* event) {
    if (event == nullptr ||
        (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress &&
         event->type() != QEvent::KeyRelease)) {
        return QObject::eventFilter(watched, event);
    }

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    const bool keyRelease = event->type() == QEvent::KeyRelease;
    const QVector<Impl::Candidate> candidates =
        keyRelease ? m_impl->releaseCandidates(*keyEvent) : m_impl->candidates(*keyEvent);
    const bool receiverInScope = m_impl->containsReceiver(watched);
    const ActivationContext context{watched, QApplication::focusWidget(), keyEvent};
    const auto candidateAllowedForReceiver = [this, receiverInScope,
                                              &context](BindingHandle handle) {
        if (receiverInScope) {
            return true;
        }
        Impl::RegisteredBinding* registered = m_impl->findBinding(handle);
        if (registered == nullptr || registered->owner == nullptr) {
            return false;
        }
        const auto canActivateOutsideScope = registered->binding.canActivateOutsideScope;
        return canActivateOutsideScope && canActivateOutsideScope(context) &&
               m_impl->findBinding(handle) != nullptr;
    };
    if (event->type() == QEvent::ShortcutOverride) {
        for (const Impl::Candidate& candidate : candidates) {
            if (!candidateAllowedForReceiver(candidate.handle)) {
                continue;
            }
            Impl::RegisteredBinding* registered = m_impl->findBinding(candidate.handle);
            if (registered == nullptr || registered->owner == nullptr) {
                continue;
            }
            const auto canActivate = registered->binding.canActivate;
            if ((!canActivate || canActivate(context)) &&
                m_impl->findBinding(candidate.handle) != nullptr) {
                event->accept();
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

    if (keyRelease) {
        // Once a held binding is armed, accept its physical release even if
        // focus moved to another top-level widget in the same process.
        bool handled = false;
        for (const Impl::Candidate& candidate : candidates) {
            Impl::RegisteredBinding* registered = m_impl->findBinding(candidate.handle);
            if (registered == nullptr || registered->owner == nullptr) {
                continue;
            }

            const auto previousSize = registered->activeReleaseCombinations.size();
            registered->activeReleaseCombinations.erase(
                std::remove_if(registered->activeReleaseCombinations.begin(),
                               registered->activeReleaseCombinations.end(),
                               [keyEvent](QKeyCombination combination) {
                                   return releasedKeyEndsCombination(combination, *keyEvent);
                               }),
                registered->activeReleaseCombinations.end());
            if (registered->activeReleaseCombinations.size() == previousSize) {
                continue;
            }
            if (!registered->activeReleaseCombinations.isEmpty()) {
                handled = true;
                continue;
            }

            const auto release = registered->binding.release;
            if (release && release(context)) {
                handled = true;
            }
        }
        if (handled) {
            event->accept();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

    for (const Impl::Candidate& candidate : candidates) {
        if (!candidateAllowedForReceiver(candidate.handle)) {
            continue;
        }
        Impl::RegisteredBinding* registered = m_impl->findBinding(candidate.handle);
        if (registered == nullptr || registered->owner == nullptr) {
            continue;
        }
        const auto canActivate = registered->binding.canActivate;
        if (canActivate && !canActivate(context)) {
            continue;
        }
        registered = m_impl->findBinding(candidate.handle);
        if (registered == nullptr || registered->owner == nullptr) {
            continue;
        }
        const auto activate = registered->binding.activate;
        if (activate && activate(context)) {
            registered = m_impl->findBinding(candidate.handle);
            if (registered != nullptr && registered->owner != nullptr &&
                registered->binding.release &&
                !registered->activeReleaseCombinations.contains(candidate.combination)) {
                registered->activeReleaseCombinations.push_back(candidate.combination);
            }
            event->accept();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace snow_shot::presentation
