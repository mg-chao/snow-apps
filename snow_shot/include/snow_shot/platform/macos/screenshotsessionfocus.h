// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <utility>

namespace snow_shot::platform::macos {

// Native identities remain owned by the target, never by an asynchronous export.
class ScreenshotFocusTarget {
  public:
    virtual ~ScreenshotFocusTarget() = default;
    [[nodiscard]] virtual bool canRestore() const = 0;
    virtual void restore() = 0;
};

class ScreenshotSessionFocus final : public QObject {
  public:
    using Capture = std::function<std::unique_ptr<ScreenshotFocusTarget>()>;

    explicit ScreenshotSessionFocus(Capture capture) : m_capture(std::move(capture)) {}

    // Prepare before dismissing input blockers; a rejected start must abandon this candidate.
    void prepare() {
        if (!m_active) {
            discard();
            m_prepared = true;
            m_target = m_capture();
        }
    }
    void abandonPrepared() {
        if (m_prepared)
            discard();
    }
    void begin(bool visible) {
        if (!visible) {
            discard();
            return;
        }
        if (m_active)
            return; // A restart/recapture still belongs to the original invocation.
        if (!m_prepared)
            prepare();
        m_prepared = false;
        m_active = true;
    }
    void beforeHide(bool restorePrevious) {
        if (!m_active || m_ending)
            return;
        m_ending = true;
        if (!restorePrevious || (m_target && !m_target->canRestore()))
            m_target.reset();
    }
    void afterHide() {
        if (!m_active || !m_ending)
            return;
        m_active = false;
        m_ending = false;
        const auto generation = m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != m_generation)
                return;
            auto target = std::move(m_target);
            if (target && target->canRestore())
                target->restore();
        });
    }
    void discard() {
        ++m_generation;
        m_target.reset();
        m_prepared = false;
        m_active = false;
        m_ending = false;
    }

  private:
    Capture m_capture;
    std::unique_ptr<ScreenshotFocusTarget> m_target;
    quint64 m_generation = 0;
    bool m_prepared = false;
    bool m_active = false;
    bool m_ending = false;
};

// The permission query is injectable; it must never request permission or show UI.
[[nodiscard]] std::unique_ptr<ScreenshotSessionFocus>
createScreenshotSessionFocus(std::function<bool()> accessibilityTrusted = {});
} // namespace snow_shot::platform::macos
