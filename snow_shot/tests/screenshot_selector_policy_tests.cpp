#include "screenshotselectorpolicy.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void smartSelectionForcesMsaaElementLookup() {
    const auto policy = screenshotSelectorLookupPolicy(true, QByteArrayLiteral("uia"));
    require(policy.backend == SNOW_UI_SELECTOR_BACKEND_MSAA &&
                policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
            "Smart Selection must force MSAA child-element lookup");
}

void disabledSelectionUsesWindowLookup() {
    const auto policy = screenshotSelectorLookupPolicy(false, QByteArrayLiteral("uia"));
    require(policy.backend == SNOW_UI_SELECTOR_BACKEND_UIA &&
                policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "disabled Smart Selection must use window-only lookup");
}

void invalidBackendFallsBackToMsaaWindowLookup() {
    const auto policy = screenshotSelectorLookupPolicy(false, QByteArrayLiteral("unknown"));
    require(policy.backend == SNOW_UI_SELECTOR_BACKEND_MSAA &&
                policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "unknown selector backends must retain the MSAA window fallback");
}

void windowTargetUsesWindowOnlyLookup() {
    require(screenshotSelectorHitTestMode(true, ScreenshotSelectorHitTestMode::Window) ==
                SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "window target must not traverse window sub-elements");
}

void windowSubElementTargetUsesElementLookup() {
    require(screenshotSelectorHitTestMode(true,
                                          ScreenshotSelectorHitTestMode::WindowSubElement) ==
                SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
            "window sub-element target must traverse the element hierarchy");
}

void disabledSmartSelectionOverridesSubElementRequests() {
    const auto policy = screenshotSelectorLookupPolicy(false, QByteArrayLiteral("uia"));
    require(policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW &&
                screenshotSelectorHitTestMode(
                    false, ScreenshotSelectorHitTestMode::WindowSubElement) ==
                    SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "disabled Smart Selection must reject stale window sub-element requests");
}
} // namespace

int main() {
    smartSelectionForcesMsaaElementLookup();
    disabledSelectionUsesWindowLookup();
    invalidBackendFallsBackToMsaaWindowLookup();
    windowTargetUsesWindowOnlyLookup();
    windowSubElementTargetUsesElementLookup();
    disabledSmartSelectionOverridesSubElementRequests();
    return 0;
}
