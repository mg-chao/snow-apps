#include "translation_test_support.h"
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"

#include <QTimer>

using namespace translation_tests;
using namespace snow_shot::presentation;

namespace {
struct CaptureState {
    SelectedTextCaptureResult initial;
    SelectedTextCaptureResult result;
    int starts = 0;
    int polls = 0;
    int cancellations = 0;
};

class FakeCaptureBackend final : public SelectedTextCaptureBackend {
  public:
    explicit FakeCaptureBackend(std::shared_ptr<CaptureState> state) : m_state(std::move(state)) {}
    SelectedTextCaptureResult start() override {
        ++m_state->starts;
        return m_state->initial;
    }
    SelectedTextCaptureResult poll() override {
        ++m_state->polls;
        return m_state->result;
    }
    void cancel() override {
        ++m_state->cancellations;
    }

  private:
    std::shared_ptr<CaptureState> m_state;
};

void captureOnlyHandsOffCompletedText() {
    auto state = std::make_shared<CaptureState>();
    SelectedTextTranslationController controller(std::make_unique<FakeCaptureBackend>(state));
    QString delivered;
    int successes = 0;
    QObject::connect(&controller, &SelectedTextTranslationController::textReady, &controller,
                     [&](const QString& text) {
                         delivered = text;
                         ++successes;
                     });
    controller.capture();
    controller.capture();
    require(state->starts == 1 && successes == 0,
            "submit immediately without activating a page or replacing a pending capture");
    waitUntil([&]() { return state->polls > 1; }, "poll pending capture without blocking Qt");
    require(successes == 0, "pending capture must not navigate or notify");
    const QString text =
        QString::fromUtf8("  Selected \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x8c\x8d\r\nsecond line  ");
    state->result = {SelectedTextStatus::Selected, text};
    waitUntil([&]() { return successes == 1; }, "successful capture hands off text");
    require(delivered == text, "preserve Unicode, whitespace and line breaks");
    state->initial = {SelectedTextStatus::Selected, QStringLiteral("next")};
    controller.capture();
    require(successes == 2 && delivered == QStringLiteral("next") && state->starts == 2,
            "a completed capture permits another explicit request");
}

void unusableCapturesHandOffEmptyText() {
    for (const auto status : {SelectedTextStatus::NoSelection, SelectedTextStatus::Unsupported,
                              SelectedTextStatus::Busy, SelectedTextStatus::TimedOut,
                              SelectedTextStatus::Failed, SelectedTextStatus::Selected}) {
        // Only a Selected capture carries text; the other statuses report empty payloads.
        const QString payload =
            status == SelectedTextStatus::Selected ? QStringLiteral(" \t\r\n") : QString();
        for (const bool immediate : {true, false}) {
            auto state = std::make_shared<CaptureState>();
            const SelectedTextCaptureResult result{status, payload};
            if (immediate) {
                state->initial = result;
            } else {
                state->result = result;
            }
            SelectedTextTranslationController controller(
                std::make_unique<FakeCaptureBackend>(state));
            int deliveries = 0;
            QString delivered;
            QObject::connect(&controller, &SelectedTextTranslationController::textReady,
                             &controller, [&](const QString& text) {
                                 ++deliveries;
                                 delivered = text;
                             });
            controller.capture();
            waitUntil([&]() { return deliveries == 1; },
                      "unusable capture hands off so the page can open and warn");
            require(delivered == payload,
                    "timeouts, failures and empty selections hand off without extra signals");
            state->initial = {SelectedTextStatus::Selected, QStringLiteral("recovered")};
            controller.capture();
            require(deliveries == 2 && delivered == QStringLiteral("recovered"),
                    "an unusable capture permits the next request");
        }
    }
}

void shutdownCancelsWithoutLateDelivery() {
    auto state = std::make_shared<CaptureState>();
    int deliveries = 0;
    {
        SelectedTextTranslationController controller(std::make_unique<FakeCaptureBackend>(state));
        QObject::connect(&controller, &SelectedTextTranslationController::textReady, &controller,
                         [&]() { ++deliveries; });
        controller.capture();
        controller.shutdown();
        controller.shutdown();
        state->result = {SelectedTextStatus::Selected, QStringLiteral("late")};
        controller.capture();
        bool ticked = false;
        QTimer::singleShot(60, &controller, [&]() { ticked = true; });
        waitUntil([&]() { return ticked; }, "event loop remains responsive after shutdown");
        require(state->cancellations == 1 && state->starts == 1 && state->polls == 0 &&
                    deliveries == 0,
                "shutdown cancels once, stops polling and suppresses all late work");
    }
    require(state->cancellations == 1, "destruction after shutdown remains idempotent");
    {
        SelectedTextTranslationController controller(std::make_unique<FakeCaptureBackend>(state));
        controller.capture();
    }
    require(state->cancellations == 2, "destruction cancels pending work without waiting");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    captureOnlyHandsOffCompletedText();
    unusableCapturesHandOffEmptyText();
    shutdownCancelsWithoutLateDelivery();
    return 0;
}
