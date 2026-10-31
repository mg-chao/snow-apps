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

void smartSelectionUsesConfiguredElementApi() {
#ifdef Q_OS_MACOS
    for (const auto& backend : {QByteArray(), QByteArrayLiteral("unknown"),
                                QByteArrayLiteral("uia"), QByteArrayLiteral("msaa")})
        require(
            screenshotSelectorLookupPolicy(snow_shot::SmartSelectionMode::WindowSubElement, backend)
                    .backend == SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY,
            "macOS must ignore all Windows backend preferences");
#else
    for (const auto& backend : {QByteArrayLiteral("msaa"), QByteArrayLiteral("uia")}) {
        const auto policy = screenshotSelectorLookupPolicy(
            snow_shot::SmartSelectionMode::WindowSubElement, backend);
        require(policy.backend == (backend == "uia" ? SNOW_UI_SELECTOR_BACKEND_UIA
                                                    : SNOW_UI_SELECTOR_BACKEND_MSAA) &&
                    policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
                "Smart selection must use the configured API for child-element lookup");
    }
    for (const auto& backend : {QByteArray(), QByteArrayLiteral("unknown")}) {
        require(
            screenshotSelectorLookupPolicy(snow_shot::SmartSelectionMode::WindowSubElement, backend)
                    .backend == SNOW_UI_SELECTOR_BACKEND_UIA,
            "missing or invalid window element APIs must default to UIA");
    }
#endif
}

void windowSelectionUsesWindowLookup() {
    const auto policy = screenshotSelectorLookupPolicy(snow_shot::SmartSelectionMode::Window,
                                                       QByteArrayLiteral("uia"));
    require(policy.backend == screenshotSelectorLookupPolicy(
                                  snow_shot::SmartSelectionMode::WindowSubElement, {})
                                  .backend &&
                policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "Window selection must use window-only lookup");
}

void invalidBackendFallsBackToUiaWindowLookup() {
    const auto policy = screenshotSelectorLookupPolicy(snow_shot::SmartSelectionMode::Window,
                                                       QByteArrayLiteral("unknown"));
    require(policy.backend == screenshotSelectorLookupPolicy(
                                  snow_shot::SmartSelectionMode::WindowSubElement, {})
                                  .backend &&
                policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "unknown selector backends must retain the UIA window fallback");
}

void windowTargetUsesWindowOnlyLookup() {
    require(screenshotSelectorHitTestMode(snow_shot::SmartSelectionMode::WindowSubElement,
                                          ScreenshotSelectorHitTestMode::Window) ==
                SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "window target must not traverse window sub-elements");
}

void windowSubElementTargetUsesElementLookup() {
    require(screenshotSelectorHitTestMode(snow_shot::SmartSelectionMode::WindowSubElement,
                                          ScreenshotSelectorHitTestMode::WindowSubElement) ==
                SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
            "window sub-element target must traverse the element hierarchy");
}

void windowSelectionOverridesSubElementRequests() {
    const auto policy = screenshotSelectorLookupPolicy(snow_shot::SmartSelectionMode::Window,
                                                       QByteArrayLiteral("uia"));
    require(policy.mode == SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW &&
                screenshotSelectorHitTestMode(snow_shot::SmartSelectionMode::Window,
                                              ScreenshotSelectorHitTestMode::WindowSubElement) ==
                    SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW,
            "Window selection must constrain stale window sub-element requests");
}

void mcpSmartSelectionOverridesRemainCompatible() {
    using snow_shot::SmartSelectionMode;
    for (const auto mode : {SmartSelectionMode::WindowSubElement, SmartSelectionMode::Window,
                            SmartSelectionMode::Disabled}) {
        require(snow_shot::smartSelectionModeFromId(snow_shot::smartSelectionModeId(mode)) == mode,
                "mode IDs must round-trip");
        require(snow_shot::smartSelectionModeWithOverride(mode, std::nullopt) == mode &&
                    snow_shot::smartSelectionModeWithOverride(mode, false) ==
                        SmartSelectionMode::Window &&
                    snow_shot::smartSelectionModeWithOverride(mode, true) ==
                        (mode == SmartSelectionMode::Disabled ? SmartSelectionMode::WindowSubElement
                                                              : mode),
                "MCP booleans must preserve window-only behavior and enable disabled lookup");
    }
}

} // namespace

int main() {
    mcpSmartSelectionOverridesRemainCompatible();
    smartSelectionUsesConfiguredElementApi();
    windowSelectionUsesWindowLookup();
    invalidBackendFallsBackToUiaWindowLookup();
    windowTargetUsesWindowOnlyLookup();
    windowSubElementTargetUsesElementLookup();
    windowSelectionOverridesSubElementRequests();

    return 0;
}
