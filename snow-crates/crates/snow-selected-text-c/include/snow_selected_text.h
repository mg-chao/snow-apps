#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowSelectedTextService SnowSelectedTextService;
typedef struct SnowSelectedTextRequest SnowSelectedTextRequest;
typedef struct SnowSelectedTextResult SnowSelectedTextResult;

/* All status/option values use fixed-width integers, not compiler-sized enums. */
enum {
    SNOW_SELECTED_TEXT_PENDING = 0,
    SNOW_SELECTED_TEXT_SELECTED = 1,
    SNOW_SELECTED_TEXT_NO_SELECTION = 2,
    SNOW_SELECTED_TEXT_UNSUPPORTED = 3,
    SNOW_SELECTED_TEXT_FAILED = 4,
    SNOW_SELECTED_TEXT_INVALID_ARGUMENT = 5
};
enum {
    SNOW_SELECTED_TEXT_UIA = 1,
    SNOW_SELECTED_TEXT_NATIVE_EDIT = 2,
    SNOW_SELECTED_TEXT_CLIPBOARD = 3
};
enum {
    SNOW_SELECTED_TEXT_STRATEGY_AUTO = 0,
    SNOW_SELECTED_TEXT_STRATEGY_UIA = 1,
    SNOW_SELECTED_TEXT_STRATEGY_NATIVE_EDIT = 2,
    SNOW_SELECTED_TEXT_STRATEGY_CLIPBOARD = 3
};
enum {
    SNOW_SELECTED_TEXT_CLIPBOARD_UNCHANGED = 0,
    SNOW_SELECTED_TEXT_CLIPBOARD_RESTORED = 1,
    SNOW_SELECTED_TEXT_CLIPBOARD_PRESERVATION_INCOMPLETE = 2,
    SNOW_SELECTED_TEXT_CLIPBOARD_SUPERSEDED = 3,
    SNOW_SELECTED_TEXT_CLIPBOARD_RESTORATION_FAILED = 4,
    SNOW_SELECTED_TEXT_CLIPBOARD_UNKNOWN = 5
};
enum {
    SNOW_SELECTED_TEXT_ERROR_INVALID_CONFIGURATION = 1,
    SNOW_SELECTED_TEXT_ERROR_BUSY = 2,
    SNOW_SELECTED_TEXT_ERROR_WORKER_UNAVAILABLE = 3,
    SNOW_SELECTED_TEXT_ERROR_TIMED_OUT = 4,
    SNOW_SELECTED_TEXT_ERROR_CANCELLED = 5,
    SNOW_SELECTED_TEXT_ERROR_NO_FOREGROUND_WINDOW = 6,
    SNOW_SELECTED_TEXT_ERROR_TARGET_CHANGED = 7,
    SNOW_SELECTED_TEXT_ERROR_PROTECTED_CONTENT = 8,
    SNOW_SELECTED_TEXT_ERROR_ACCESS_DENIED = 9,
    SNOW_SELECTED_TEXT_ERROR_CLIPBOARD_BUSY = 10,
    SNOW_SELECTED_TEXT_ERROR_CLIPBOARD_AMBIGUOUS = 11,
    SNOW_SELECTED_TEXT_ERROR_INPUT_INJECTION_FAILED = 12,
    SNOW_SELECTED_TEXT_ERROR_MALFORMED_DATA = 13,
    SNOW_SELECTED_TEXT_ERROR_LIMIT_EXCEEDED = 14,
    SNOW_SELECTED_TEXT_ERROR_UNSUPPORTED_PLATFORM = 15,
    SNOW_SELECTED_TEXT_ERROR_NATIVE_API = 16,
    SNOW_SELECTED_TEXT_ERROR_COPY_BLOCKED = 17,
    SNOW_SELECTED_TEXT_ERROR_TARGET_UNAVAILABLE = 18
};

/* UTF-8 bytes, never implicitly NUL-terminated. A zero length permits a null pointer. */
typedef struct SnowSelectedTextBytes {
    const uint8_t* data;
    size_t length;
} SnowSelectedTextBytes;

typedef struct SnowSelectedTextOptions {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t timeout_ms;    /* 1..60000; default 2000 */
    uint32_t copy_fallback; /* 0 or 1; default 1; only applies to AUTO */
    uint32_t strategy;      /* STRATEGY_*; default AUTO; explicit strategies never fall back */
    size_t max_text_bytes;  /* 1..64 MiB; default 1 MiB */
    const uintptr_t* excluded_windows;
    size_t excluded_window_count;
    const SnowSelectedTextBytes* excluded_executables; /* UTF-8 basenames */
    size_t excluded_executable_count;                  /* Both lists are limited to 1024 entries. */
} SnowSelectedTextOptions;

typedef struct SnowSelectedTextError {
    uint32_t kind;
    int32_t native_code;
    uint32_t has_native_code;
    uint32_t clipboard_status;
    SnowSelectedTextBytes operation; /* Static lifetime; contains no captured text. */
} SnowSelectedTextError;

typedef struct SnowSelectedTextMetadata {
    uintptr_t window;
    uint32_t process_id;
    uintptr_t focused_control;
    uint32_t method;
    uint32_t clipboard_status;
    SnowSelectedTextBytes executable;
    size_t range_count;
} SnowSelectedTextMetadata;

typedef struct SnowSelectedTextRect {
    double left;
    double top;
    double width;
    double height;
} SnowSelectedTextRect;

/* Non-null pointers must be live and aligned; destruction must not race another call.
   No API activates a window. Submit BEFORE activating the host's translation popup.
   One process-wide request may run at a time. A timed-out provider can keep it busy.
   Handles can be used on different threads; cancellation and polling are thread-safe.
   Request/result handles remain valid after service destruction. */
uint8_t snow_selected_text_options_init(SnowSelectedTextOptions* output);
/* Creation/submission return 0 on success, an ERROR_* kind otherwise. Error is optional.
   Output handles are cleared on failure. Input options/arrays are copied by start. */
uint32_t snow_selected_text_service_create(SnowSelectedTextService** output,
                                           SnowSelectedTextError* error);
void snow_selected_text_service_destroy(SnowSelectedTextService* service);
uint32_t snow_selected_text_start(const SnowSelectedTextService* service,
                                  const SnowSelectedTextOptions* options,
                                  SnowSelectedTextRequest** output, SnowSelectedTextError* error);
uint32_t snow_selected_text_request_poll(const SnowSelectedTextRequest* request);
/* Returns a status. Each terminal call creates an independent result; destroy each one.
   Pending or invalid arguments produce a null result. Prefer poll until terminal. */
uint32_t snow_selected_text_request_result(const SnowSelectedTextRequest* request,
                                           SnowSelectedTextResult** output);
void snow_selected_text_request_cancel(const SnowSelectedTextRequest* request);
void snow_selected_text_request_destroy(SnowSelectedTextRequest* request);
void snow_selected_text_result_destroy(SnowSelectedTextResult* result);
uint32_t snow_selected_text_result_status(const SnowSelectedTextResult* result);
/* Accessors return 1 on success, 0 for null/wrong outcome/out-of-range arguments.
   Outputs are cleared on failure. Text/executable views last until result destruction. */
uint8_t snow_selected_text_result_error(const SnowSelectedTextResult* result,
                                        SnowSelectedTextError* output);
uint8_t snow_selected_text_result_text(const SnowSelectedTextResult* result,
                                       SnowSelectedTextBytes* output);
uint8_t snow_selected_text_result_metadata(const SnowSelectedTextResult* result,
                                           SnowSelectedTextMetadata* output);
uint8_t snow_selected_text_result_range(const SnowSelectedTextResult* result, size_t index,
                                        SnowSelectedTextBytes* output, size_t* rectangle_count);
uint8_t snow_selected_text_result_rect(const SnowSelectedTextResult* result, size_t range,
                                       size_t index, SnowSelectedTextRect* output);

#ifdef __cplusplus
}
#endif
