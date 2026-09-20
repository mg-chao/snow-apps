#include "snow_shot/presentation/windowshortcutmanager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QKeyEvent>
#include <QInputMethodQueryEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QTextEdit>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace snow_shot::presentation {
namespace {

QWindow* surfaceForScope(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object))
        return widget->windowHandle();
    return qobject_cast<QWindow*>(object);
}
bool scopeVisible(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object))
        return widget->isVisible();
    if (auto* window = qobject_cast<QWindow*>(object))
        return window->isVisible();
    return false;
}
bool scopeActive(QObject* object) {
    if (auto* widget = qobject_cast<QWidget*>(object))
        return widget->isActiveWindow();
    if (auto* window = qobject_cast<QWindow*>(object))
        return window->isActive();
    return false;
}

// Qt keeps its Windows pressed-key bookkeeping for the application lifetime.
// Preserve lost-release evidence for the same lifetime, including when the
// last pin is closed and a later pin creates a new shortcut manager.
struct UnreleasedKeyState final : QObject {
    explicit UnreleasedKeyState(QObject* parent) : QObject(parent) {}
    QSet<quint64> keys;
    QHash<quint64, quint64> revisions;
    QHash<quint64, QPointer<WindowShortcutManager>> releaseOwners;
};

UnreleasedKeyState& applicationKeyState() {
    static QPointer<UnreleasedKeyState> state;
    if (state == nullptr) {
        state = new UnreleasedKeyState(QCoreApplication::instance());
    }
    return *state;
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

shortcuts::ShortcutBindingList normalizedBindings(const shortcuts::ShortcutBindingList& bindings) {
    shortcuts::ShortcutBindingList result;
    for (const shortcuts::ShortcutBinding& candidate : bindings) {
        const bool modifierOnlyShift =
            candidate.portableText.compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0;
        const shortcuts::ShortcutBinding binding =
            shortcuts::canonicalBinding(candidate, modifierOnlyShift);
        if (binding.portableText.isEmpty()) {
            continue;
        }
        const bool duplicate = std::any_of(
            result.cbegin(), result.cend(), [&binding](const shortcuts::ShortcutBinding& existing) {
                return shortcuts::bindingsConflict(existing, binding);
            });
        if (!duplicate) {
            result.push_back(binding);
        }
    }
    return result;
}

} // namespace

struct WindowShortcutManager::Impl {
    struct PendingActivation {
        BindingHandle handle = 0;
        QPointer<QObject> scope;
    };

    struct RegisteredBinding {
        BindingHandle handle = 0;
        quint64 order = 0;
        QPointer<QObject> owner;
        Binding binding;
        shortcuts::ShortcutBindingList activeReleaseBindings;
    };

    struct Candidate {
        BindingHandle handle = 0;
        int priority = 0;
        quint64 order = 0;
        shortcuts::ShortcutBinding binding;
    };

    explicit Impl(WindowShortcutManager& manager)
        : q(manager), m_unreleasedKeys(applicationKeyState().keys),
          m_keyStateRevisions(applicationKeyState().revisions) {}

    // Physical keys whose press was observed through this filter without a
    // matching release, and keys that were still held when keyboard input last
    // became unreachable for the scope windows. When the capture UI closes
    // while a completion key is still held, the key release is delivered to
    // whichever window regains the foreground and never reaches this process;
    // Qt's Windows key mapper then keeps the key recorded as pressed and labels
    // the NEXT physical press of it as an auto-repeat. Tracking the observed
    // press state lets the manager recognize such mislabeled presses and
    // dispatch them as the fresh presses they physically are.
    [[nodiscard]] bool isStaleAutoRepeat(const QKeyEvent& event) const {
        const quint64 token = shortcuts::eventKeyToken(event);
        return event.isAutoRepeat() && event.key() != Qt::Key_unknown &&
               !m_heldKeys.contains(token) && m_unreleasedKeys.contains(token);
    }

    void noteKeyPress(const QKeyEvent& event) {
        if (event.key() == Qt::Key_unknown) {
            return;
        }
        const quint64 token = shortcuts::eventKeyToken(event);
        ++m_keyStateRevisions[token];
        m_heldKeys.insert(token);
        m_unreleasedKeys.remove(token);
    }

    void noteKeyRelease(const QKeyEvent& event) {
        // Auto-repeat sequences include synthetic repeat releases that must not
        // end the held state; only a real release clears the records.
        if (!event.isAutoRepeat() && event.key() != Qt::Key_unknown) {
            const quint64 token = shortcuts::eventKeyToken(event);
            ++m_keyStateRevisions[token];
            m_heldKeys.remove(token);
            m_unreleasedKeys.remove(token);
        }
    }

    // While no scope window can receive keyboard input, releases of keys the
    // user is still holding are routed to other applications and never reach
    // this process. From that point on the release state of every held key is
    // unknown: move it to the unreleased set so a later auto-repeat-labeled
    // press of the same key is recognized as a fresh press.
    void noteScopeInputUnreachable(QObject* object, QEvent::Type type) {
        auto* widget = qobject_cast<QWidget*>(object);
        auto* nativeWindow = qobject_cast<QWindow*>(object);
        if ((!widget || !widget->isWindow()) && !nativeWindow)
            return;
        QObject* eventWindow = widget ? static_cast<QObject*>(widget->window()) : nativeWindow;
        const bool isScopeWindow = std::any_of(m_scopeWindows.cbegin(), m_scopeWindows.cend(),
                                               [eventWindow](const QPointer<QObject>& scopeWindow) {
                                                   return scopeWindow == eventWindow;
                                               });
        // Every manager observes application-wide key presses, including those
        // handled by another window. That window can lose the release when it
        // closes, so invalidate inactive scopes even when the event is outside
        // this manager. Hiding an ordinary child must not end a held key.
        if (type == QEvent::Hide && isScopeWindow) {
            for (const QPointer<QObject>& scopeWindow : m_scopeWindows) {
                if (scopeWindow != nullptr && scopeVisible(scopeWindow)) {
                    return;
                }
            }
        } else {
            for (const QPointer<QObject>& scopeWindow : m_scopeWindows) {
                if (scopeWindow != nullptr && scopeActive(scopeWindow)) {
                    return;
                }
            }
        }
        invalidateHeldKeys();
        cancelHeldBindings(false);
        // WindowDeactivate precedes activation of the next toolbar/tool window.
        // Resolve the destination after Qt has completed that focus transition.
        QMetaObject::invokeMethod(
            &q,
            [this] {
                if (scopeForReceiver(QGuiApplication::focusWindow()) == nullptr &&
                    scopeForReceiver(QApplication::activeWindow()) == nullptr) {
                    cancelReleaseActivations();
                }
            },
            Qt::QueuedConnection);
    }

    [[nodiscard]] QObject* scopeForReceiver(QObject* receiver) {
        if (!receiver)
            return nullptr;
        for (const auto& scope : m_scopeWindows) {
            if (auto* surface = surfaceForScope(scope);
                surface && surface->focusObject() == receiver)
                return scope;
        }
        auto* widget = qobject_cast<QWidget*>(receiver);
        if (!widget)
            widget = qobject_cast<QWidget*>(receiver->parent());
        QWidget* receiverWidget = widget ? widget->window() : nullptr;
        QWindow* candidate =
            receiverWidget ? receiverWidget->windowHandle() : qobject_cast<QWindow*>(receiver);
        for (const auto& scope : m_scopeWindows) {
            if (scope && (scope == receiver || scope == receiverWidget))
                return scope;
        }
        QSet<QWindow*> visited;
        while (candidate && !visited.contains(candidate)) {
            visited.insert(candidate);
            for (const auto& scope : m_scopeWindows)
                if (scope && surfaceForScope(scope) == candidate)
                    return scope;
            candidate = candidate->transientParent();
        }
        for (QWidget* parent = receiverWidget ? receiverWidget->parentWidget() : nullptr; parent;
             parent = parent->parentWidget()) {
            for (const auto& scope : m_scopeWindows)
                if (scope == parent->window())
                    return scope;
        }
        return nullptr;
    }

    [[nodiscard]] bool inputSuspended() const {
        return !m_inputSuspensions.isEmpty();
    }

    void invalidateHeldKeys() {
        for (const quint64 key : m_heldKeys) {
            ++m_keyStateRevisions[key];
        }
        m_unreleasedKeys.unite(m_heldKeys);
        m_heldKeys.clear();
    }

    void cancelHeldBindings(bool cancelRelease = true) {
        if (cancelRelease) {
            cancelReleaseActivations();
        }
        struct PendingCancellation {
            QPointer<QObject> owner;
            std::function<void()> cancel;
        };
        QVector<PendingCancellation> pending;
        for (RegisteredBinding& registered : m_bindings) {
            if (!registered.activeReleaseBindings.isEmpty()) {
                for (const auto& binding : registered.activeReleaseBindings) {
                    applicationKeyState().releaseOwners.remove(
                        shortcuts::shortcutKeyToken(binding));
                }
                pending.push_back({registered.owner, registered.binding.cancel});
                registered.activeReleaseBindings.clear();
            }
        }
        // Detach every hold before calling clients: cancellation can suspend
        // again or unregister bindings. Each surviving client must still reset.
        for (const auto& item : pending) {
            if (item.owner != nullptr && item.cancel) {
                item.cancel();
            }
        }
    }

    void cancelReleaseActivations(BindingHandle handle = 0, QObject* scope = nullptr) {
        for (auto& pending : m_releaseActivations) {
            if ((handle == 0 || pending.handle == handle) &&
                (scope == nullptr || pending.scope == scope)) {
                // Still drain this sequence if its release reaches us. A fresh
                // physical press after interruption may replace the reservation.
                pending.handle = 0;
            }
        }
    }

    [[nodiscard]] RegisteredBinding* findBinding(BindingHandle handle) {
        const auto binding =
            std::find_if(m_bindings.begin(), m_bindings.end(),
                         [handle](const RegisteredBinding& item) { return item.handle == handle; });
        return binding != m_bindings.end() ? &*binding : nullptr;
    }

    [[nodiscard]] QVector<Candidate> candidates(const QKeyEvent& event) {
        QVector<Candidate> result;
        const bool staleAutoRepeat = isStaleAutoRepeat(event);
        for (RegisteredBinding& registered : m_bindings) {
            if (registered.owner == nullptr ||
                (event.isAutoRepeat() && !staleAutoRepeat && !registered.binding.autoRepeat)) {
                continue;
            }
            const auto match =
                std::find_if(registered.binding.shortcutBindings.cbegin(),
                             registered.binding.shortcutBindings.cend(),
                             [&event, &registered](const auto& binding) {
                                 return shortcuts::shortcutMatchesEvent(
                                     binding, event, registered.binding.allowedAdditionalModifiers);
                             });
            if (match != registered.binding.shortcutBindings.cend()) {
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
            if (registered.owner == nullptr || registered.activeReleaseBindings.isEmpty()) {
                continue;
            }
            const auto match = std::find_if(
                registered.activeReleaseBindings.cbegin(), registered.activeReleaseBindings.cend(),
                [&event](const auto& binding) {
                    return shortcuts::shortcutReleaseMatchesEvent(binding, event);
                });
            if (match != registered.activeReleaseBindings.cend()) {
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
        std::stable_sort(candidates->begin(), candidates->end(),
                         [](const Candidate& left, const Candidate& right) {
                             if (left.priority != right.priority) {
                                 return left.priority > right.priority;
                             }
                             return left.order < right.order;
                         });
    }

    WindowShortcutManager& q;
    QList<QPointer<QObject>> m_scopeWindows;
    QVector<RegisteredBinding> m_bindings;
    QHash<quint64, PendingActivation> m_releaseActivations;
    QSet<quint64> m_heldKeys;
    QSet<quint64>& m_unreleasedKeys;
    QHash<quint64, quint64>& m_keyStateRevisions;
    BindingHandle m_nextHandle = 1;
    quint64 m_nextOrder = 1;
    InputSuspensionHandle m_nextSuspensionHandle = 1;
    QSet<InputSuspensionHandle> m_inputSuspensions;
};

WindowShortcutManager::WindowShortcutManager(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {
    if (QCoreApplication* application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
}

WindowShortcutManager::~WindowShortcutManager() {
    auto& owners = applicationKeyState().releaseOwners;
    for (auto it = owners.begin(); it != owners.end();) {
        it = it.value() == this ? owners.erase(it) : std::next(it);
    }
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
                    [root](const QPointer<QObject>& existing) { return existing == root; });
    if (alreadyRegistered) {
        return;
    }
    m_impl->m_scopeWindows.push_back(root);
    connect(root, &QObject::destroyed, this, [this]() {
        for (auto& pending : m_impl->m_releaseActivations) {
            if (!pending.scope) {
                pending.handle = 0;
            }
        }
        m_impl->m_scopeWindows.erase(
            std::remove_if(m_impl->m_scopeWindows.begin(), m_impl->m_scopeWindows.end(),
                           [](const QPointer<QObject>& item) { return item.isNull(); }),
            m_impl->m_scopeWindows.end());
    });
}

void WindowShortcutManager::removeScopeWindow(QWidget* window) {
    QWidget* root = window != nullptr ? window->window() : nullptr;
    m_impl->cancelReleaseActivations(0, root);
    m_impl->m_scopeWindows.erase(std::remove_if(m_impl->m_scopeWindows.begin(),
                                                m_impl->m_scopeWindows.end(),
                                                [root](const QPointer<QObject>& item) {
                                                    return item.isNull() || item == root;
                                                }),
                                 m_impl->m_scopeWindows.end());
}

void WindowShortcutManager::addScopeWindow(QWindow* window) {
    if (!window || m_impl->m_scopeWindows.contains(window))
        return;
    m_impl->m_scopeWindows.push_back(window);
    connect(window, &QObject::destroyed, this, [this] {
        m_impl->cancelReleaseActivations();
        m_impl->m_scopeWindows.removeIf([](const auto& scope) { return scope.isNull(); });
    });
}
void WindowShortcutManager::removeScopeWindow(QWindow* window) {
    m_impl->cancelReleaseActivations(0, window);
    m_impl->m_scopeWindows.removeIf(
        [window](const auto& scope) { return !scope || scope == window; });
}

WindowShortcutManager::InputSuspensionHandle WindowShortcutManager::suspendInput() {
    const InputSuspensionHandle handle = m_impl->m_nextSuspensionHandle++;
    m_impl->m_inputSuspensions.insert(handle);
    m_impl->invalidateHeldKeys();
    m_impl->cancelHeldBindings();
    // Modal interactions (for example the native save dialog) move keyboard
    // input outside the manager's visibility, so the release state of every
    // held key can no longer be tracked reliably.
    return handle;
}

void WindowShortcutManager::resumeInput(InputSuspensionHandle handle) {
    if (m_impl->m_inputSuspensions.remove(handle) && !m_impl->inputSuspended()) {
        // Presses observed inside modal interactions may lose their releases too.
        m_impl->invalidateHeldKeys();
    }
}

WindowShortcutManager::BindingHandle WindowShortcutManager::addBinding(QObject* owner,
                                                                       Binding binding) {
    if (owner == nullptr || !binding.activate ||
        (binding.activationTrigger == Binding::ActivationTrigger::Release &&
         (binding.release || binding.cancel || binding.autoRepeat)) ||
        static_cast<bool>(binding.release) != static_cast<bool>(binding.cancel)) {
        return 0;
    }
    if (binding.shortcutBindings.isEmpty()) {
        binding.shortcutBindings = shortcutBindingsFromKeyCombinations(binding.keyCombinations);
    } else {
        binding.shortcutBindings = normalizedBindings(binding.shortcutBindings);
    }
    binding.keyCombinations.clear();

    const BindingHandle handle = m_impl->m_nextHandle++;
    m_impl->m_bindings.push_back(
        Impl::RegisteredBinding{handle, m_impl->m_nextOrder++, owner, std::move(binding), {}});
    connect(owner, &QObject::destroyed, this,
            [this, handle]() { static_cast<void>(removeBinding(handle)); });
    return handle;
}

bool WindowShortcutManager::setShortcuts(BindingHandle handle,
                                         const shortcuts::ShortcutBindingList& shortcuts) {
    const auto binding = std::find_if(
        m_impl->m_bindings.begin(), m_impl->m_bindings.end(),
        [handle](const Impl::RegisteredBinding& item) { return item.handle == handle; });
    if (binding == m_impl->m_bindings.end()) {
        return false;
    }
    binding->binding.shortcutBindings = normalizedBindings(shortcuts);
    m_impl->cancelReleaseActivations(handle);
    return true;
}

bool WindowShortcutManager::setKeyCombinations(BindingHandle handle,
                                               const QList<QKeyCombination>& keyCombinations) {
    return setShortcuts(handle, shortcutBindingsFromKeyCombinations(keyCombinations));
}

bool WindowShortcutManager::removeBinding(BindingHandle handle) {
    m_impl->cancelReleaseActivations(handle);
    if (const auto* registered = m_impl->findBinding(handle)) {
        for (const auto& binding : registered->activeReleaseBindings) {
            applicationKeyState().releaseOwners.remove(shortcuts::shortcutKeyToken(binding));
        }
    }
    const auto previousSize = m_impl->m_bindings.size();
    m_impl->m_bindings.erase(std::remove_if(m_impl->m_bindings.begin(), m_impl->m_bindings.end(),
                                            [handle](const Impl::RegisteredBinding& item) {
                                                return item.handle == handle;
                                            }),
                             m_impl->m_bindings.end());
    return m_impl->m_bindings.size() != previousSize;
}

shortcuts::ShortcutBindingList WindowShortcutManager::shortcutBindingsFromKeyCombinations(
    const QList<QKeyCombination>& keyCombinations) {
    shortcuts::ShortcutBindingList bindings;
    for (const QKeyCombination combination : normalizedCombinations(keyCombinations)) {
        if (combination.key() == Qt::Key_Shift &&
            combination.keyboardModifiers() == Qt::ShiftModifier) {
            bindings.push_back(shortcuts::ShortcutBinding{QStringLiteral("Shift")});
            continue;
        }
        const QString portable = QKeySequence(combination).toString(QKeySequence::PortableText);
        const shortcuts::ShortcutBinding binding = shortcuts::bindingFromPortableText(portable);
        if (!binding.portableText.isEmpty()) {
            bindings.push_back(binding);
        }
    }
    return normalizedBindings(bindings);
}

QList<QKeyCombination> WindowShortcutManager::keyCombinationsFromBindings(
    const shortcuts::ShortcutBindingList& shortcuts) {
    QList<QKeyCombination> combinations;
    combinations.reserve(shortcuts.size());
    for (const shortcuts::ShortcutBinding& binding : shortcuts) {
        if (binding.portableText.compare(QStringLiteral("Shift"), Qt::CaseInsensitive) == 0) {
            combinations.push_back(QKeyCombination(Qt::ShiftModifier, Qt::Key_Shift));
            continue;
        }
        const QKeySequence sequence =
            QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
        if (sequence.count() == 1) {
            combinations.push_back(sequence[0]);
        }
    }
    return normalizedCombinations(combinations);
}

bool WindowShortcutManager::focusAcceptsTextInput(QWidget* focusWidget) {
    for (QWidget* current = focusWidget; current != nullptr; current = current->parentWidget()) {
        if (auto* spinBox = qobject_cast<QAbstractSpinBox*>(current)) {
            return !spinBox->isReadOnly();
        }
        if (auto* lineEdit = qobject_cast<QLineEdit*>(current)) {
            return !lineEdit->isReadOnly();
        }
        if (auto* textEdit = qobject_cast<QTextEdit*>(current)) {
            return !textEdit->isReadOnly();
        }
        if (auto* plainTextEdit = qobject_cast<QPlainTextEdit*>(current)) {
            return !plainTextEdit->isReadOnly();
        }
    }
    return false;
}

bool WindowShortcutManager::focusObjectAcceptsTextInput(QObject* focusObject) {
    if (auto* widget = qobject_cast<QWidget*>(focusObject))
        return focusAcceptsTextInput(widget);
    if (!focusObject)
        return false;
    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(focusObject, &query);
    return query.value(Qt::ImEnabled).toBool();
}

bool WindowShortcutManager::eventFilter(QObject* watched, QEvent* event) {
    if (event == nullptr) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) {
        if (event->type() == QEvent::Hide) {
            if (auto* widget = qobject_cast<QWidget*>(watched); widget && widget->isWindow()) {
                m_impl->cancelReleaseActivations(0, widget);
            }
            if (auto* window = qobject_cast<QWindow*>(watched))
                m_impl->cancelReleaseActivations(0, window);
        }
        m_impl->noteScopeInputUnreachable(watched, event->type());
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::ApplicationDeactivate) {
        m_impl->invalidateHeldKeys();
        m_impl->cancelHeldBindings();
        return false;
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress &&
        event->type() != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }
    // QWidgetWindow forwards native input to the focused QWidget. Consuming
    // that first delivery would bypass the scope and Qt's widget bookkeeping.
    if (qobject_cast<QWindow*>(watched) &&
        !std::any_of(m_impl->m_scopeWindows.cbegin(), m_impl->m_scopeWindows.cend(),
                     [watched](const auto& scope) { return scope == watched; })) {
        return false;
    }

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    const quint64 keyToken = shortcuts::eventKeyToken(*keyEvent);
    const bool keyRelease = event->type() == QEvent::KeyRelease;
    auto& releaseOwners = applicationKeyState().releaseOwners;
    if (const auto owner = releaseOwners.value(keyToken); owner && owner != this) {
        const auto pending = owner->m_impl->m_releaseActivations.constFind(keyToken);
        if (event->type() == QEvent::KeyPress && !keyEvent->isAutoRepeat() &&
            pending != owner->m_impl->m_releaseActivations.cend() && pending->handle == 0) {
            owner->m_impl->m_releaseActivations.remove(keyToken);
            releaseOwners.remove(keyToken);
        } else {
            return false;
        }
    }
    auto pending = m_impl->m_releaseActivations.find(keyToken);
    if (pending != m_impl->m_releaseActivations.end()) {
        if (pending->handle == 0 && event->type() == QEvent::KeyPress &&
            !keyEvent->isAutoRepeat()) {
            m_impl->m_releaseActivations.erase(pending);
            releaseOwners.remove(keyToken);
        } else {
            event->accept();
            if (!keyRelease || keyEvent->isAutoRepeat()) {
                return true;
            }
            const auto activation = *pending;
            m_impl->m_releaseActivations.erase(pending);
            releaseOwners.remove(keyToken);
            m_impl->noteKeyRelease(*keyEvent);
            auto* registered = m_impl->findBinding(activation.handle);
            if (registered == nullptr || registered->owner == nullptr || !activation.scope ||
                !scopeVisible(activation.scope) || m_impl->inputSuspended() ||
                m_impl->scopeForReceiver(watched) == nullptr) {
                return true;
            }
            const QPointer<WindowShortcutManager> guard(this);
            const QPointer<QObject> owner = registered->owner;
            const auto eligible = registered->binding.canActivate;
            const auto activate = registered->binding.activate;
            const ActivationContext context{
                watched,          QApplication::focusWidget(),
                keyEvent,         qobject_cast<QWidget*>(activation.scope.data()),
                activation.scope, QGuiApplication::focusObject()};
            if ((!eligible || eligible(context)) && guard && owner &&
                m_impl->findBinding(activation.handle)) {
                static_cast<void>(activate(context));
            }
            // Activation may have destroyed this manager and the event receiver.
            return true;
        }
    }
    if (keyRelease) {
        m_impl->noteKeyRelease(*keyEvent);
    } else if (event->type() == QEvent::KeyPress && !keyEvent->isAutoRepeat()) {
        m_impl->noteKeyPress(*keyEvent);
    }
    const QVector<Impl::Candidate> candidates =
        keyRelease ? m_impl->releaseCandidates(*keyEvent) : m_impl->candidates(*keyEvent);
    if (!keyRelease || keyEvent->isAutoRepeat()) {
        for (const auto& registered : m_impl->m_bindings) {
            if (std::any_of(registered.activeReleaseBindings.cbegin(),
                            registered.activeReleaseBindings.cend(),
                            [keyEvent](const auto& binding) {
                                return shortcuts::shortcutReleaseMatchesEvent(binding, *keyEvent);
                            })) {
                event->accept();
                return true;
            }
        }
    }
    if (m_impl->inputSuspended()) {
        return QObject::eventFilter(watched, event);
    }

    QObject* scopeWindow = m_impl->scopeForReceiver(watched);
    const bool receiverInScope = scopeWindow != nullptr;
    const ActivationContext context{watched,     QApplication::focusWidget(),
                                    keyEvent,    qobject_cast<QWidget*>(scopeWindow),
                                    scopeWindow, QGuiApplication::focusObject()};
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
        if (!keyEvent->isAutoRepeat()) {
            releaseOwners.remove(keyToken);
        }
        // Once a held binding is armed, accept its physical release even if
        // focus moved to another top-level widget in the same process.
        bool handled = false;
        for (const Impl::Candidate& candidate : candidates) {
            Impl::RegisteredBinding* registered = m_impl->findBinding(candidate.handle);
            if (registered == nullptr || registered->owner == nullptr) {
                continue;
            }

            const auto previousSize = registered->activeReleaseBindings.size();
            registered->activeReleaseBindings.erase(
                std::remove_if(registered->activeReleaseBindings.begin(),
                               registered->activeReleaseBindings.end(),
                               [keyEvent](const auto& binding) {
                                   return shortcuts::shortcutReleaseMatchesEvent(binding,
                                                                                 *keyEvent);
                               }),
                registered->activeReleaseBindings.end());
            if (registered->activeReleaseBindings.size() == previousSize) {
                continue;
            }
            if (!registered->activeReleaseBindings.isEmpty()) {
                handled = true;
                continue;
            }

            const auto release = registered->binding.release;
            const QPointer<WindowShortcutManager> guard(this);
            if (release && release(context)) {
                handled = true;
            }
            if (!guard) {
                event->accept();
                return true;
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
        const bool recovered = m_impl->isStaleAutoRepeat(*keyEvent);
        if (activate && keyEvent->isAutoRepeat()) {
            m_impl->noteKeyPress(*keyEvent);
        }
        if (registered->binding.activationTrigger == Binding::ActivationTrigger::Release) {
            m_impl->m_releaseActivations.insert(keyToken, {candidate.handle, scopeWindow});
            releaseOwners.insert(keyToken, this);
            event->accept();
            return true;
        }
        const quint64 pressRevision = m_impl->m_keyStateRevisions.value(keyToken);
        // Arm before entering client code so a synchronous modal dialog,
        // deactivation, or physical release can end this hold immediately.
        const bool armed = registered->binding.release &&
                           !registered->activeReleaseBindings.contains(candidate.binding);
        if (armed) {
            registered->activeReleaseBindings.push_back(candidate.binding);
            releaseOwners.insert(keyToken, this);
        }
        const QPointer<WindowShortcutManager> guard(this);
        const bool activated = activate && activate(context);
        if (!guard) {
            event->accept();
            return true;
        }
        if (!activated) {
            registered = m_impl->findBinding(candidate.handle);
            if (armed && registered != nullptr) {
                registered->activeReleaseBindings.removeAll(candidate.binding);
                releaseOwners.remove(keyToken);
            }
            // A declining handler must not consume another manager's recovery
            // evidence. Do not roll back any newer input/lifecycle transition
            // that happened reentrantly inside the callback.
            if (recovered && m_impl->m_keyStateRevisions.value(keyToken) == pressRevision) {
                m_impl->m_heldKeys.remove(keyToken);
                m_impl->m_unreleasedKeys.insert(keyToken);
                ++m_impl->m_keyStateRevisions[keyToken];
            }
        }
        if (activated) {
            event->accept();
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace snow_shot::presentation
