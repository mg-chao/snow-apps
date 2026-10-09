#include "snow_shot/runtime/memoryoptimizationcontroller.h"
#include <QCoreApplication>
#include <QEvent>
#include <algorithm>

namespace snow_shot::runtime {
using namespace std::chrono_literals;

MemoryOptimizationController::MemoryOptimizationController(RuntimeActivityTracker& activity,
                                                           Options options, QObject* parent)
    : QObject(parent), m_activity(activity), m_options(std::move(options)),
      m_started(m_options.clock()), m_policyChanged(m_started) {
    m_timer.setInterval(5000);
    m_timer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &MemoryOptimizationController::evaluate);
    if (auto* application = QCoreApplication::instance()) {
        application->installEventFilter(this);
        connect(application, &QCoreApplication::aboutToQuit, this,
                &MemoryOptimizationController::stop);
    }
    synchronizePolicy();
}

void MemoryOptimizationController::synchronizePolicy() {
    if (m_stopped)
        return;
    const bool enabled = m_options.enabled && m_options.enabled();
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    m_policyChanged = m_options.clock();
    m_activity.markActivity();
    m_blocked = true;
    if (enabled)
        m_timer.start();
    else
        m_timer.stop();
}

void MemoryOptimizationController::stop() {
    m_stopped = true;
    m_enabled = false;
    m_timer.stop();
}

void MemoryOptimizationController::evaluate() {
    synchronizePolicy();
    if (!m_enabled || m_stopped)
        return;
    const bool safe = m_options.safe && m_options.safe();
    if (!safe) {
        m_blocked = true;
        return;
    }
    if (m_blocked) {
        m_blocked = false;
        m_activity.markActivity();
    }
    const auto now = m_options.clock();
    if (now - m_started < 60s || (m_lastAttempt && now - *m_lastAttempt < 5min))
        return;
    const auto snapshot = m_activity.snapshot();
    if (snapshot.activeCount != 0 || m_attemptedIdlePeriod == snapshot.idlePeriod)
        return;
    const bool pressure = m_options.lowMemory && m_options.lowMemory().value_or(false);
    const auto quiet = pressure ? 15s : 60s;
    if (now - std::max(snapshot.lastActivity, m_policyChanged) < quiet)
        return;
    const auto before = m_options.sample ? m_options.sample() : std::nullopt;
    constexpr std::uint64_t mib = 1024 * 1024;
    if (!before || before->privateWorkingSetBytes < (pressure ? 12 : 18) * mib)
        return;
    // Sampling can race worker admission. Recheck UI/policy before the tracker
    // checks its generation under the admission lock.
    if (!m_options.enabled || !m_options.enabled() || !m_options.safe || !m_options.safe() ||
        !m_options.trim)
        return;
    MemoryTrimReport report;
    report.lowMemory = pressure;
    report.before = *before;
    auto attemptedAt = now;
    const auto admitted = m_activity.tryRunWhenIdle(snapshot.generation, [&] {
        const auto started = m_options.clock();
        attemptedAt = started;
        report.result = m_options.trim();
        report.duration = m_options.clock() - started;
    });
    if (!admitted)
        return;
    m_lastAttempt = attemptedAt;
    m_attemptedIdlePeriod = snapshot.idlePeriod;
    report.after = m_options.sample ? m_options.sample() : std::nullopt;
    if (m_options.report)
        m_options.report(report);
}

bool MemoryOptimizationController::eventFilter(QObject* watched, QEvent* event) {
    if (m_enabled && !m_stopped) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::WindowActivate:
        case QEvent::ContextMenu:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
            m_activity.markActivity();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}
} // namespace snow_shot::runtime
