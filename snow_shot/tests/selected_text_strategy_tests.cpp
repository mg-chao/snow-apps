#include "snow_shot/presentation/selectedtexttranslationcontroller.h"
#include "snow_selected_text.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

using snow_shot::presentation::SelectedTextTranslationController;

namespace {
int submissions = 0;
int completions = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

// Substitute the C boundary so the production native backend can be exercised without
// accessing another application, injecting input, or modifying the clipboard.
struct SnowSelectedTextService {};

extern "C" {
uint8_t snow_selected_text_options_init(SnowSelectedTextOptions* output) {
    *output = {};
    output->struct_size = sizeof(*output);
    output->abi_version = 1;
    output->timeout_ms = 2000;
    output->copy_fallback = 1;
    output->strategy = SNOW_SELECTED_TEXT_STRATEGY_AUTO;
    output->max_text_bytes = 1024 * 1024;
    return 1;
}

uint32_t snow_selected_text_service_create(SnowSelectedTextService** output,
                                           SnowSelectedTextError*) {
    *output = new SnowSelectedTextService;
    return 0;
}

void snow_selected_text_service_destroy(SnowSelectedTextService* service) {
    delete service;
}

uint32_t snow_selected_text_start(const SnowSelectedTextService*,
                                  const SnowSelectedTextOptions* options,
                                  SnowSelectedTextRequest** output, SnowSelectedTextError*) {
    ++submissions;
#if defined(Q_OS_MACOS)
    require(options != nullptr && options->strategy == SNOW_SELECTED_TEXT_STRATEGY_AUTO &&
                options->timeout_ms == 2000,
            "macOS must retain Accessibility-first defaults");
#else
    require(options != nullptr && options->strategy == SNOW_SELECTED_TEXT_STRATEGY_CLIPBOARD &&
                options->timeout_ms == 800,
            "non-macOS platforms must retain clipboard-only capture and its short timeout");
#endif
    *output = nullptr;
    return SNOW_SELECTED_TEXT_ERROR_COPY_BLOCKED;
}

uint32_t snow_selected_text_request_poll(const SnowSelectedTextRequest*) {
    require(false, "failed submission must not be polled");
    return SNOW_SELECTED_TEXT_FAILED;
}

uint32_t snow_selected_text_request_result(const SnowSelectedTextRequest*,
                                           SnowSelectedTextResult**) {
    require(false, "failed submission must not retrieve a result");
    return SNOW_SELECTED_TEXT_FAILED;
}

void snow_selected_text_request_destroy(SnowSelectedTextRequest*) {}
void snow_selected_text_request_cancel(const SnowSelectedTextRequest*) {}
void snow_selected_text_result_destroy(SnowSelectedTextResult*) {}
uint8_t snow_selected_text_result_text(const SnowSelectedTextResult*, SnowSelectedTextBytes*) {
    return 0;
}
uint8_t snow_selected_text_result_error(const SnowSelectedTextResult*, SnowSelectedTextError*) {
    return 0;
}
} // extern "C"

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    SelectedTextTranslationController controller;
    QObject::connect(&controller, &SelectedTextTranslationController::textReady, &controller,
                     [&](const QString& text) {
                         require(text.isEmpty(), "failed capture must not deliver a payload");
                         ++completions;
                     });
    controller.capture();
    require(submissions == 1 && completions == 1,
            "failed capture must complete without submitting another strategy");
    controller.capture();
    require(submissions == 2 && completions == 2,
            "an explicit retry must preserve the platform strategy");
    return 0;
}
