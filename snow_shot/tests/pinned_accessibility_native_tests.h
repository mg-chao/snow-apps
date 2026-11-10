// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef Q_OS_WIN
#include "snow_ui_selector.h"
#endif
#ifdef Q_OS_MACOS
void checkMacPinnedAccessibility(QWidget& window, const QRect& content);
#endif

namespace pinned_accessibility_tests {
#ifdef Q_OS_WIN
void checkNativePath(ScreenshotPinnedWindow& window) {
    struct State {
        QObject receiver;
        bool refreshed = false;
        bool delivered = false;
        bool ok = false;
        QVector<QRect> rects;
    } state;
    const auto callback = [](const SnowUiSelectorEvent* event, void* userdata) {
        auto* state = static_cast<State*>(userdata);
        QVector<QRect> rects;
        for (size_t i = 0; i < event->count; ++i) {
            const auto& r = event->rects[i];
            rects.push_back(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top));
        }
        QMetaObject::invokeMethod(
            &state->receiver,
            [state, rects = std::move(rects), phase = event->phase, ok = event->ok]() {
                state->rects = rects;
                state->ok = ok != 0;
                if (phase != SNOW_UI_SELECTOR_REFINEMENT)
                    state->delivered = true;
            },
            Qt::QueuedConnection);
    };
    const auto refreshed = [](uint64_t, uint8_t ok, void* userdata) {
        auto* state = static_cast<State*>(userdata);
        QMetaObject::invokeMethod(
            &state->receiver,
            [state, ok] {
                state->ok = ok != 0;
                state->refreshed = true;
            },
            Qt::QueuedConnection);
    };
    const QRect expected = Access::contentGeometry(window);
    const QPoint point = expected.center();
    for (const auto backend : {SNOW_UI_SELECTOR_BACKEND_UIA, SNOW_UI_SELECTOR_BACKEND_MSAA}) {
        std::unique_ptr<SnowUiSelectorService, decltype(&snow_ui_selector_service_destroy)> service(
            snow_ui_selector_service_create(callback, refreshed, &state),
            snow_ui_selector_service_destroy);
        require(service != nullptr, "create native selector for pinned accessibility");
        state.refreshed = false;
        require(snow_ui_selector_service_refresh(service.get(), 1, backend, nullptr, 0),
                "refresh native pinned selector");
        live_pinned_tests::wait([&] { return state.refreshed; },
                                "native selector refresh completes");
        require(state.ok, "native selector refresh succeeds");
        SnowUiSelectorQuery query{
            1, 1, 1, point.x(), point.y(), SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW, 0, 0, 0};
        state.delivered = false;
        require(snow_ui_selector_service_query(service.get(), &query),
                "query pinned window bounds");
        live_pinned_tests::wait([&] { return state.delivered; }, "window bounds query completes");
        require(state.ok && state.rects.size() == 1 &&
                    state.rects.front() == window.currentNativeGeometry(),
                "window mode retains the complete native pin including padding");
        const QRect outer = state.rects.front();
        query.request_id++;
        query.mode = SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT;
        state.delivered = false;
        require(snow_ui_selector_service_query(service.get(), &query),
                "query pinned child elements");
        live_pinned_tests::wait([&] { return state.delivered; }, "child element query completes");
        if (backend == SNOW_UI_SELECTOR_BACKEND_MSAA) {
            // Qt 6 publishes UIA providers. The legacy MSAA backend retains its
            // window fallback; this feature must not change that window geometry.
            require(state.ok && !state.rects.isEmpty() && state.rects.back() == outer,
                    "legacy MSAA retains the pinned window fallback");
            continue;
        }
        if (backend == SNOW_UI_SELECTOR_BACKEND_UIA && state.rects.size() < 2) {
            state.delivered = false;
            require(snow_ui_selector_service_refine(service.get(), &query),
                    "refine pinned child elements");
            live_pinned_tests::wait([&] { return state.delivered; },
                                    "child element refinement completes");
        }
        if (!state.ok || state.rects.size() < 2) {
            std::cerr << "pinned accessibility backend=" << int(backend)
                      << " frames=" << state.rects.size() << '\n';
        }
        require(state.ok && state.rects.size() >= 2 && state.rects.back() == outer,
                "UIA publishes a content child followed by the full pin");
        const QRect actual = state.rects.front();
        const int tolerance = qCeil(window.devicePixelRatioF()) * 2;
        require((actual.topLeft() - expected.topLeft()).manhattanLength() <= tolerance &&
                    std::abs(actual.width() - expected.width()) <= tolerance &&
                    std::abs(actual.height() - expected.height()) <= tolerance,
                "native accessibility child bounds match pinned content pixels");
    }
}
#elif defined(Q_OS_MACOS)
void checkNativePath(ScreenshotPinnedWindow& window) {
    checkMacPinnedAccessibility(window,
                                content(QAccessible::queryAccessibleInterface(&window))->rect());
}
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void runNative() {
    {
        ScreenshotPinnedWindow window;
        auto config = pinned_crop_tests::config();
        require(window.present(config), "present native accessibility fixture");
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        QCoreApplication::processEvents();
        checkNativePath(window);
        window.hide();
    }
    {
        auto source = std::make_shared<live_pinned_tests::FakeSource>();
        source->nativePresentation = true;
        source->region = QRect(220, 200, 240, 180);
        ScreenshotPinnedWindow window;
        live_pinned_tests::present(window, live_pinned_tests::config(source));
        require(!window.findChild<SnowCanvasWidget*>()->isVisible(),
                "native live fixture hides its canvas");
        checkNativePath(window);
        window.hide();
    }
}
#endif
} // namespace pinned_accessibility_tests
