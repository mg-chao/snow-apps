// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <utility>

namespace snow_shot::platform::macos {

// A lease owns one native identity. Destruction never changes window visibility.
class ScreenshotBackgroundWindow {
  public:
    virtual ~ScreenshotBackgroundWindow() = default;
    virtual void suppress() = 0;
    virtual void restoreBehind() = 0;
    [[nodiscard]] virtual bool isValid() const {
        return true;
    }
};

class ScreenshotBackgroundWindowGuard final : public QObject {
  public:
    struct Ports {
        std::function<std::unique_ptr<ScreenshotBackgroundWindow>()> capture;
        std::function<bool()> externalApplicationActive;
        std::function<void(QObject*, std::function<void()>)> deferRestore =
            [](QObject* context, std::function<void()> callback) {
                QTimer::singleShot(1000, context, std::move(callback));
            };
    };

    explicit ScreenshotBackgroundWindowGuard(Ports ports) : m_ports(std::move(ports)) {}

    void prepare(bool externalApplication) {
        invalidateRestoration();
        if (m_hidden && !m_hidden->isValid())
            m_hidden.reset();
        // A new invocation inherits a window still hidden by the previous one.
        // Sampling native visibility again would lose that window permanently.
        m_prepared = externalApplication && !m_hidden ? m_ports.capture() : nullptr;
    }
    void begin() {
        invalidateRestoration();
        m_active = true;
        if (!m_hidden && m_prepared) {
            m_hidden = std::move(m_prepared);
            m_hidden->suppress();
        }
        m_prepared.reset();
    }
    void abandonPrepared() {
        m_prepared.reset();
        if (!m_active && m_hidden)
            afterHide(m_waitForExternalApplication);
    }
    void presentationHidden(bool waitForExternalApplication) {
        m_active = false;
        m_waitForExternalApplication = waitForExternalApplication;
    }
    void afterHide(bool waitForExternalApplication) {
        invalidateRestoration();
        presentationHidden(waitForExternalApplication);
        m_prepared.reset();
        if (!m_hidden)
            return;
        if (!waitForExternalApplication || m_ports.externalApplicationActive()) {
            restore();
            return;
        }
        m_restorePending = true;
        const auto generation = m_generation;
        const QPointer<ScreenshotBackgroundWindowGuard> lifetime(this);
        m_ports.deferRestore(this, [lifetime, generation] {
            if (lifetime && generation == lifetime->m_generation && lifetime->m_restorePending)
                lifetime->restore();
        });
    }
    void applicationActivated() {
        if (m_restorePending && m_ports.externalApplicationActive())
            restore();
    }
    // Explicit reopening and shutdown abandon our ownership of visibility.
    void discard() {
        invalidateRestoration();
        m_active = false;
        m_prepared.reset();
        m_hidden.reset();
    }

  private:
    void invalidateRestoration() {
        ++m_generation;
        m_restorePending = false;
    }
    void restore() {
        invalidateRestoration();
        auto window = std::move(m_hidden);
        if (window)
            window->restoreBehind();
    }

    Ports m_ports;
    std::unique_ptr<ScreenshotBackgroundWindow> m_prepared;
    std::unique_ptr<ScreenshotBackgroundWindow> m_hidden;
    quint64 m_generation = 0;
    bool m_active = false;
    bool m_restorePending = false;
    bool m_waitForExternalApplication = true;
};
} // namespace snow_shot::platform::macos
