#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowUiSelectorServiceImpl SnowUiSelectorService;
typedef enum SnowUiSelectorBackend {
    SNOW_UI_SELECTOR_BACKEND_UIA,
    SNOW_UI_SELECTOR_BACKEND_MSAA,
    SNOW_UI_SELECTOR_BACKEND_ACCESSIBILITY
} SnowUiSelectorBackend;
typedef enum SnowUiSelectorHitTestMode {
    SNOW_UI_SELECTOR_HIT_TEST_MODE_UI_ELEMENT,
    SNOW_UI_SELECTOR_HIT_TEST_MODE_WINDOW
} SnowUiSelectorHitTestMode;
typedef enum SnowUiSelectorPhase {
    SNOW_UI_SELECTOR_INITIAL,
    SNOW_UI_SELECTOR_REFINEMENT,
    SNOW_UI_SELECTOR_FINISHED
} SnowUiSelectorPhase;
typedef enum SnowUiSelectorStopReason {
    SNOW_UI_SELECTOR_COMPLETE,
    SNOW_UI_SELECTOR_BUDGET_EXHAUSTED,
    SNOW_UI_SELECTOR_DECODING_PENDING,
    SNOW_UI_SELECTOR_PROVIDER_TIMEOUT,
    SNOW_UI_SELECTOR_PROVIDER_FAILURE,
    SNOW_UI_SELECTOR_CANCELLED,
    SNOW_UI_SELECTOR_TRAVERSAL_LIMIT,
    SNOW_UI_SELECTOR_PERMISSION_REQUIRED,
    SNOW_UI_SELECTOR_ACCESSIBILITY_PENDING
} SnowUiSelectorStopReason;
typedef struct SnowUiSelectorRect {
    int32_t left, top, right, bottom;
} SnowUiSelectorRect;
typedef struct SnowUiSelectorQuery {
    uint64_t epoch, request_id, generation;
    int32_t x, y;
    SnowUiSelectorHitTestMode mode;
    /* CGDirectDisplayID on macOS; zero selects the first matching display. Ignored on Windows. */
    uint32_t display_id;
} SnowUiSelectorQuery;
typedef struct SnowUiSelectorEvent {
    SnowUiSelectorQuery query;
    SnowUiSelectorPhase phase;
    SnowUiSelectorStopReason reason;
    uint8_t ok;
    uint64_t elapsed_us;
    const SnowUiSelectorRect* rects;
    size_t count;
} SnowUiSelectorEvent;
/* Events and rectangles are borrowed for the callback duration. Callbacks may run on
   either worker or the submitting thread, are serialized, must copy/queue only, and must not
   re-enter the service. Destroy closes delivery synchronously but joins provider workers in the
   background. */
typedef void (*SnowUiSelectorEventCallback)(const SnowUiSelectorEvent*, void*);
typedef void (*SnowUiSelectorRefreshCallback)(uint64_t epoch, uint8_t ok, void*);
SnowUiSelectorService* snow_ui_selector_service_create(SnowUiSelectorEventCallback,
                                                       SnowUiSelectorRefreshCallback, void*);
void snow_ui_selector_service_destroy(SnowUiSelectorService*);
uint8_t snow_ui_selector_service_release_cache(SnowUiSelectorService*);
uint8_t snow_ui_selector_service_refresh(SnowUiSelectorService*, uint64_t epoch,
                                         SnowUiSelectorBackend, const uintptr_t*, size_t);
uint8_t snow_ui_selector_service_query(SnowUiSelectorService*, const SnowUiSelectorQuery*);
uint8_t snow_ui_selector_service_refine(SnowUiSelectorService*, const SnowUiSelectorQuery*);
/* Pass prompt=1 only for an explicit user action. This never prompts when prompt=0. */
uint8_t snow_ui_selector_accessibility_permission(uint8_t prompt);
void snow_ui_selector_service_invalidate_refinement(SnowUiSelectorService*);
#ifdef __cplusplus
}
#endif
