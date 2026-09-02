#include "screenshotoverlayframepresenter.h"

#include "../capture/screenshotcaptureperfinstrumentation.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QWidget>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
constexpr auto kRevealStrategyEnvironment = "SNOW_SHOT_CAPTURE_REVEAL_STRATEGY";

class ShowPaintSuppression final : public QObject {
  public:
    ShowPaintSuppression(QWidget& window, bool enabled) {
        if (!enabled) {
            return;
        }

        {
            SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.update_filter_install");
            SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.update_filter_install_requests", 1);
            window.installEventFilter(this);
        }
        m_filteredWindow = &window;

#if defined(Q_OS_WIN) || defined(_WIN32)
        if (QGuiApplication::platformName() != QStringLiteral("windows")) {
            return;
        }

        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        if (hwnd == nullptr) {
            return;
        }

        {
            SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.redraw_suppress");
            SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.redraw_suppress_requests", 1);
            static_cast<void>(SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0));
        }
        m_hwnd = hwnd;
#else
        Q_UNUSED(window);
#endif
    }

    ~ShowPaintSuppression() {
        restore();
    }

    ShowPaintSuppression(const ShowPaintSuppression&) = delete;
    ShowPaintSuppression& operator=(const ShowPaintSuppression&) = delete;

    void restoreNativeRedraw() {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (m_hwnd != nullptr) {
            const HWND hwnd = m_hwnd;
            m_hwnd = nullptr;
            SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.redraw_restore");
            SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.redraw_restore_requests", 1);
            static_cast<void>(SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0));
        }
#endif
    }

    void discardDeferredUpdateRequests() {
        if (m_filteredWindow == nullptr) {
            return;
        }

        SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.update_filter_drain");
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.update_filter_drain_requests", 1);
        QCoreApplication::sendPostedEvents(m_filteredWindow->window(), QEvent::UpdateRequest);
    }

    void restore() {
        restoreNativeRedraw();

        if (m_filteredWindow != nullptr) {
            QWidget* window = m_filteredWindow;
            m_filteredWindow = nullptr;
            {
                SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.update_filter_remove");
                SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.update_filter_remove_requests",
                                               1);
                window->removeEventFilter(this);
            }
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_filteredWindow && event != nullptr &&
            event->type() == QEvent::UpdateRequest) {
            SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.update_requests_suppressed", 1);
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QWidget* m_filteredWindow = nullptr;
#if defined(Q_OS_WIN) || defined(_WIN32)
    HWND m_hwnd = nullptr;
#endif
};

ScreenshotOverlayRevealStrategy configuredRevealStrategy() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    constexpr auto defaultStrategy = ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed;
#else
    constexpr auto defaultStrategy = ScreenshotOverlayRevealStrategy::SingleRepaint;
#endif
#if defined(SNOW_SHOT_CAPTURE_PERF_INSTRUMENTATION) || defined(SNOW_SHOT_BENCH_INTERNALS)
    return ScreenshotOverlayFramePresenter::strategyForName(qgetenv(kRevealStrategyEnvironment),
                                                            defaultStrategy);
#else
    return defaultStrategy;
#endif
}

bool shouldConcealFirstPaint(const QWidget& window) {
    return !window.isVisible() && window.isWindow() &&
           QGuiApplication::platformName() == QStringLiteral("windows");
}

void recordStrategy(ScreenshotOverlayRevealStrategy strategy) {
    switch (strategy) {
    case ScreenshotOverlayRevealStrategy::SingleRepaint:
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.strategy.single_repaint", 1);
        break;
    case ScreenshotOverlayRevealStrategy::PostedUpdate:
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.strategy.posted_update", 1);
        break;
    case ScreenshotOverlayRevealStrategy::NativeUpdate:
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.strategy.native_update", 1);
        break;
    case ScreenshotOverlayRevealStrategy::NativeInvalidate:
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.strategy.native_invalidate", 1);
        break;
    case ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed:
        SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.strategy.native_invalidate_suppressed",
                                       1);
        break;
    }
}
} // namespace

ScreenshotOverlayFramePresenter::ScreenshotOverlayFramePresenter(QWidget& window)
    : m_window(window), m_strategy(configuredRevealStrategy()) {}

void ScreenshotOverlayFramePresenter::presentPreparedFrame() {
    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.sync_reveal");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.windows_shown", 1);
    recordStrategy(m_strategy);

    const ScreenshotOverlayRevealPlan plan = planFor(m_strategy);
    const bool concealFirstPaint = shouldConcealFirstPaint(m_window);
    const qreal previousOpacity = m_window.windowOpacity();
    if (concealFirstPaint) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.opacity_conceal");
        m_window.setWindowOpacity(0.0);
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.concealed");

    ShowPaintSuppression paintSuppression(m_window, concealFirstPaint && plan.suppressShowPaint);
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.redraw_suppressed");
    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.show");
        m_window.show();
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.show_returned");
    paintSuppression.restoreNativeRedraw();
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.redraw_restored");

    {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.surface_commit");
        commitPreparedSurface(plan);
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.surface_commit_done");
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.redraw_done");
    paintSuppression.discardDeferredUpdateRequests();
    paintSuppression.restore();
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.update_filter_removed");

    if (concealFirstPaint) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.opacity_restore");
        m_window.setWindowOpacity(previousOpacity);
    }
    SNOW_SHOT_CAPTURE_PERF_MILESTONE("presentation.window.opacity_restored");
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
void ScreenshotOverlayFramePresenter::setStrategyForTesting(
    ScreenshotOverlayRevealStrategy strategy) {
    m_strategy = strategy;
}
#endif

ScreenshotOverlayRevealStrategy
ScreenshotOverlayFramePresenter::strategyForName(const QByteArray& name,
                                                 ScreenshotOverlayRevealStrategy fallback) {
    const QByteArray normalized = name.trimmed().toLower();
    if (normalized == QByteArrayLiteral("single-repaint") ||
        normalized == QByteArrayLiteral("single_repaint") ||
        normalized == QByteArrayLiteral("repaint")) {
        return ScreenshotOverlayRevealStrategy::SingleRepaint;
    }
    if (normalized == QByteArrayLiteral("posted-update") ||
        normalized == QByteArrayLiteral("posted_update")) {
        return ScreenshotOverlayRevealStrategy::PostedUpdate;
    }
    if (normalized == QByteArrayLiteral("native-update") ||
        normalized == QByteArrayLiteral("native_update")) {
        return ScreenshotOverlayRevealStrategy::NativeUpdate;
    }
    if (normalized == QByteArrayLiteral("native-invalidate") ||
        normalized == QByteArrayLiteral("native_invalidate")) {
        return ScreenshotOverlayRevealStrategy::NativeInvalidate;
    }
    if (normalized == QByteArrayLiteral("native-invalidate-suppressed") ||
        normalized == QByteArrayLiteral("native_invalidate_suppressed")) {
        return ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed;
    }
    return fallback;
}

ScreenshotOverlayRevealPlan
ScreenshotOverlayFramePresenter::planFor(ScreenshotOverlayRevealStrategy strategy) {
    ScreenshotOverlayRevealPlan plan;
    switch (strategy) {
    case ScreenshotOverlayRevealStrategy::SingleRepaint:
        plan.repaint = true;
        break;
    case ScreenshotOverlayRevealStrategy::PostedUpdate:
        plan.sendPostedUpdate = true;
        break;
    case ScreenshotOverlayRevealStrategy::NativeUpdate:
        plan.nativeUpdate = true;
        break;
    case ScreenshotOverlayRevealStrategy::NativeInvalidate:
        plan.nativeInvalidate = true;
        break;
    case ScreenshotOverlayRevealStrategy::NativeInvalidateSuppressed:
        plan.suppressShowPaint = true;
        plan.nativeInvalidate = true;
        break;
    }
    return plan;
}

void ScreenshotOverlayFramePresenter::commitPreparedSurface(
    const ScreenshotOverlayRevealPlan& plan) {
    if (plan.repaint) {
        repaintSurface();
    }
    if (plan.sendPostedUpdate) {
        sendPostedUpdate();
    }
    if (plan.nativeUpdate) {
        updateNativeSurface(false);
    }
    if (plan.nativeInvalidate) {
        updateNativeSurface(true);
    }
}

void ScreenshotOverlayFramePresenter::repaintSurface() {
    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.commit.repaint");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.surface_commit_requests", 1);
    m_window.repaint();
}

void ScreenshotOverlayFramePresenter::sendPostedUpdate() {
    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.commit.posted_update");
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.surface_commit_requests", 1);
    QCoreApplication::sendPostedEvents(m_window.window(), QEvent::UpdateRequest);
}

void ScreenshotOverlayFramePresenter::updateNativeSurface(bool invalidate) {
    SNOW_SHOT_CAPTURE_PERF_COUNTER("presentation.window.surface_commit_requests", 1);
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return;
    }

    const HWND hwnd = reinterpret_cast<HWND>(m_window.winId());
    if (hwnd == nullptr) {
        return;
    }

    if (invalidate) {
        SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.commit.native_invalidate");
        static_cast<void>(
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN));
        return;
    }

    SNOW_SHOT_CAPTURE_PERF_SCOPE("presentation.window.commit.native_update");
    static_cast<void>(RedrawWindow(hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN));
#else
    Q_UNUSED(invalidate);
#endif
}
