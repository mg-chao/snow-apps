#include "snow_selected_text.h"
#include <assert.h>
#include <stddef.h>

#if defined(__cplusplus)
static_assert(sizeof(uintptr_t) == sizeof(void*), "handle width");
static_assert(offsetof(SnowSelectedTextOptions, abi_version) == 4, "ABI prefix");
#else
_Static_assert(sizeof(uintptr_t) == sizeof(void*), "handle width");
_Static_assert(offsetof(SnowSelectedTextOptions, abi_version) == 4, "ABI prefix");
#endif

int main(void) {
    SnowSelectedTextOptions options;
    SnowSelectedTextError error = {0};
    SnowSelectedTextService* service = NULL;
    SnowSelectedTextRequest* request = NULL;
    SnowSelectedTextResult* result = NULL;
    assert(snow_selected_text_options_init(&options) == 1);
    assert(options.struct_size == sizeof(options));
    assert(options.abi_version == 1 && options.timeout_ms == 2000 && options.copy_fallback == 1);
    assert(options.strategy == SNOW_SELECTED_TEXT_STRATEGY_AUTO);
    assert(snow_selected_text_service_create(&service, &error) == 0);
    /* Exercise real bundle symbols without reading another application or touching the clipboard.
     */
    options.copy_fallback = 42;
    assert(snow_selected_text_start(service, &options, &request, &error) ==
           SNOW_SELECTED_TEXT_ERROR_INVALID_CONFIGURATION);
    assert(request == NULL);
    assert(error.kind == SNOW_SELECTED_TEXT_ERROR_INVALID_CONFIGURATION);
    options.copy_fallback = 1;
    options.strategy = 42;
    assert(snow_selected_text_start(service, &options, &request, &error) ==
           SNOW_SELECTED_TEXT_ERROR_INVALID_CONFIGURATION);
    assert(request == NULL);
    assert(error.kind == SNOW_SELECTED_TEXT_ERROR_INVALID_CONFIGURATION);
    assert(snow_selected_text_request_poll(NULL) == SNOW_SELECTED_TEXT_INVALID_ARGUMENT);
    assert(snow_selected_text_request_result(NULL, &result) == SNOW_SELECTED_TEXT_INVALID_ARGUMENT);
    assert(result == NULL);
    assert(snow_selected_text_result_status(NULL) == SNOW_SELECTED_TEXT_INVALID_ARGUMENT);
    snow_selected_text_request_cancel(NULL);
    snow_selected_text_request_destroy(NULL);
    snow_selected_text_result_destroy(NULL);
    snow_selected_text_service_destroy(service);
    return 0;
}
