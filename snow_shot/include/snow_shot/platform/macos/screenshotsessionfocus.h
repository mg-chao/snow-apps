// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/platform/macos/screenshotbackgroundwindowguard.h"
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <utility>

class QWidget;

namespace snow_shot::platform::macos {

// Native identities remain owned by the target, never by an asynchronous export.
class ScreenshotFocusTarget {
  public:
    virtual ~ScreenshotFocusTarget() = default;
    [[nodiscard]] virtual bool canRestore() const = 0;
    virtual void restore() = 0;
    [[nodiscard]] virtual bool isExternalApplication() const {
        return false;
    }
};

class ScreenshotSessionFocus final : public QObject {
  public:
    using Capture = std::function<std::unique_ptr<ScreenshotFocusTarget>()>;

    explicit ScreenshotSessionFocus(
        Capture capture, std::unique_ptr<ScreenshotBackgroundWindowGuard> background = {})
        : m_capture(std::move(capture)), m_background(std::move(background)) {}

    // Prepare before dismissing input blockers; a rejected start must abandon this candidate.
    void prepare() {
        if (!m_active) {
            resetFocus();
            m_prepared = true;
            m_target = m_capture();
            if (m_background)
                m_background->prepare(m_target && m_target->isExternalApplication());
        }
    }
    void abandonPrepared() {
        if (m_prepared) {
            resetFocus();
            if (m_background)
                m_background->abandonPrepared();
        }
    }
    void begin(bool visible) {
        if (!visible) {
            resetFocus();
            if (m_background)
                m_background->abandonPrepared();
            return;
        }
        if (m_active)
            return; // A restart/recapture still belongs to the original invocation.
        if (!m_prepared)
            prepare();
        m_prepared = false;
        m_active = true;
        if (m_background)
            m_background->begin();
    }
    void beforeHide(bool restorePrevious) {
        if (!m_active || m_ending)
            return;
        m_ending = true;
        m_restorePrevious = restorePrevious;
        if (!restorePrevious || (m_target && !m_target->canRestore()))
            m_target.reset();
    }
    void afterHide() {
        if (!m_active || !m_ending)
            return;
        m_active = false;
        m_ending = false;
        if (m_background)
            m_background->presentationHidden(m_restorePrevious);
        const auto generation = m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != m_generation)
                return;
            const QPointer<ScreenshotSessionFocus> lifetime(this);
            auto target = std::move(m_target);
            if (target && target->canRestore())
                target->restore();
            if (lifetime && generation == m_generation && m_background)
                m_background->afterHide(m_restorePrevious);
        });
    }
    void discard() {
        resetFocus();
        if (m_background)
            m_background->discard();
    }
    void releaseBackgroundWindow() {
        // An explicit main-window request supersedes queued focus restoration too.
        ++m_generation;
        m_target.reset();
        if (m_background)
            m_background->discard();
    }

  private:
    void resetFocus() {
        ++m_generation;
        m_target.reset();
        m_prepared = false;
        m_active = false;
        m_ending = false;
    }

    Capture m_capture;
    std::unique_ptr<ScreenshotFocusTarget> m_target;
    std::unique_ptr<ScreenshotBackgroundWindowGuard> m_background;
    quint64 m_generation = 0;
    bool m_prepared = false;
    bool m_active = false;
    bool m_ending = false;
    bool m_restorePrevious = false;
};

// The permission query is injectable; it must never request permission or show UI.
[[nodiscard]] std::unique_ptr<ScreenshotSessionFocus>
createScreenshotSessionFocus(std::function<bool()> accessibilityTrusted = {},
                             std::function<QWidget*()> backgroundWindow = {});
} // namespace snow_shot::platform::macos
