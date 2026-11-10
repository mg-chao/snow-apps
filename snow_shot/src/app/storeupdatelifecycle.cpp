#include "snow_shot/app/storeupdatelifecycle.h"
#include "snow_shot/app/distribution.h"

#include <QCoreApplication>
#include <QDebug>
#include <algorithm>
#include <utility>
#include <chrono>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::app {

StoreUpdateLifecycle::StoreUpdateLifecycle(Hooks hooks) : m_hooks(std::move(hooks)) {}

StoreUpdateLifecycle::~StoreUpdateLifecycle() {
    if (m_installed && QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
    if (m_pending && !m_ended && m_hooks.suspend) {
        try {
            m_hooks.suspend(false);
        } catch (...) {
            qWarning("Could not restore availability after Store update preparation");
        }
    }
}

void StoreUpdateLifecycle::install() {
    if (m_installed || !distribution::isStoreManaged() || !QCoreApplication::instance())
        return;
#ifdef Q_OS_WIN
    const HRESULT result = RegisterApplicationRestart(
        L"--autostart", RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);
    if (FAILED(result))
        qWarning() << "Could not register Store update restart:" << Qt::hex << result;
#endif
    QCoreApplication::instance()->installNativeEventFilter(this);
    m_installed = true;
}

bool StoreUpdateLifecycle::queryEndSession() {
    if (m_ended || m_pending)
        return true;
    bool suspensionRequested = false;
    const auto restoreAvailability = [this] {
        if (m_ended)
            return;
        try {
            if (m_hooks.suspend)
                m_hooks.suspend(false);
        } catch (...) {
            qWarning("Could not restore availability after Store update preparation");
        }
    };
    try {
        const bool busy = m_hooks.busy && m_hooks.busy();
        if (m_ended)
            return true;
        if (busy)
            return false;
        // Suspension can partially complete before an exception. Always restore it when a
        // checkpoint cannot be accepted, including when the suspend hook itself throws.
        suspensionRequested = true;
        if (m_hooks.suspend)
            m_hooks.suspend(true);
        if (m_ended)
            return true;
        const bool checkpointed = !m_hooks.checkpoint || m_hooks.checkpoint();
        if (m_ended)
            return true;
        const bool admitted =
            checkpointed && (!m_hooks.validateAfterCheckpoint || m_hooks.validateAfterCheckpoint());
        // A terminal notification owns shutdown even if the cooperative callback declines or
        // throws. Its resources must remain closed, and no later preparation callback may run.
        if (m_ended)
            return true;
        if (!admitted) {
            restoreAvailability();
            return m_ended;
        }
        m_pending = true;
        return true;
    } catch (...) {
        if (m_ended)
            return true;
        if (suspensionRequested)
            restoreAvailability();
        if (!m_ended)
            qWarning("Store update shutdown preparation failed");
        return m_ended;
    }
}

void StoreUpdateLifecycle::endSession(bool ending) {
    if (m_ended)
        return;
    if (!ending) {
        if (std::exchange(m_pending, false) && m_hooks.suspend) {
            try {
                m_hooks.suspend(false);
            } catch (...) {
                qWarning("Could not restore availability after cancelled Store update");
            }
        }
        return;
    }
    m_ended = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    if (!m_pending && m_hooks.suspendForTermination) {
        try {
            m_hooks.suspendForTermination();
        } catch (...) {
            qWarning("Store update terminal input suspension failed");
        }
    }
    m_pending = true;
    std::shared_future<void> checkpoint;
    try {
        if (m_hooks.terminalCheckpoint)
            checkpoint = m_hooks.terminalCheckpoint();
    } catch (...) {
        qWarning("Store update terminal checkpoint could not start");
    }
    try {
        if (m_hooks.preserveRecording)
            m_hooks.preserveRecording(
                static_cast<int>(std::max(std::chrono::milliseconds(0),
                                          std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - std::chrono::steady_clock::now()))
                                     .count()));
    } catch (...) {
        qWarning("Store update recording preservation failed");
    }
    if (checkpoint.valid())
        static_cast<void>(checkpoint.wait_until(deadline));
}

bool StoreUpdateLifecycle::pending() const {
    return m_pending;
}

bool StoreUpdateLifecycle::nativeEventFilter(const QByteArray& eventType, void* message,
                                             qintptr* result) {
    Q_UNUSED(eventType);
#ifdef Q_OS_WIN
    const auto* native = static_cast<const MSG*>(message);
    if (!native || !result)
        return false;
    if (native->message == WM_QUERYENDSESSION && (native->lParam & ENDSESSION_CLOSEAPP) != 0) {
        try {
            *result = queryEndSession() ? TRUE : FALSE;
        } catch (...) {
            qWarning("Store update shutdown preparation failed");
            *result = FALSE;
        }
        return true;
    }
    if (native->message == WM_ENDSESSION &&
        (m_pending || (native->lParam & ENDSESSION_CLOSEAPP) != 0)) {
        try {
            endSession(native->wParam != FALSE);
        } catch (...) {
            qWarning("Store update terminal preservation failed");
        }
        if (native->wParam != FALSE) {
            // All terminal saves have a shared deadline. Qt's ordinary aboutToQuit and
            // static destructors can join unrelated workers or cancel preserved media.
            std::_Exit(EXIT_SUCCESS);
        }
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

} // namespace snow_shot::app
