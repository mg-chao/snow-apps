#include "snow_shot/presentation/selectedtexttranslationcontroller.h"

#include "snow_selected_text.h"
#include <QCoreApplication>

namespace snow_shot::presentation {
namespace {
// Existing platforms retain the established clipboard-only path and its shorter budget.
// macOS keeps the FFI defaults so Accessibility runs before guarded clipboard fallback.
#if !defined(Q_OS_MACOS)
constexpr uint32_t kClipboardCaptureTimeoutMs = 800;
#endif

SelectedTextStatus errorStatus(uint32_t kind) {
    switch (kind) {
    case SNOW_SELECTED_TEXT_ERROR_BUSY:
        return SelectedTextStatus::Busy;
    case SNOW_SELECTED_TEXT_ERROR_TIMED_OUT:
        return SelectedTextStatus::TimedOut;
    case SNOW_SELECTED_TEXT_ERROR_UNSUPPORTED_PLATFORM:
        return SelectedTextStatus::Unsupported;
    default:
        return SelectedTextStatus::Failed;
    }
}

class NativeSelectedTextCaptureBackend final : public SelectedTextCaptureBackend {
  public:
    ~NativeSelectedTextCaptureBackend() override {
        cancel();
    }

    SelectedTextCaptureResult start() override {
        auto activity = runtime::RuntimeActivityTracker::shared().acquire();
        if (!m_service) {
            SnowSelectedTextService* service = nullptr;
            const auto error = snow_selected_text_service_create(&service, nullptr);
            m_service.reset(service, snow_selected_text_service_destroy);
            if (error != 0) {
                return {errorStatus(error), {}};
            }
        }
        SnowSelectedTextOptions options{};
        if (!snow_selected_text_options_init(&options)) {
            return {SelectedTextStatus::Failed, {}};
        }
#if !defined(Q_OS_MACOS)
        options.strategy = SNOW_SELECTED_TEXT_STRATEGY_CLIPBOARD;
        options.timeout_ms = kClipboardCaptureTimeoutMs;
#endif
        SnowSelectedTextRequest* request = nullptr;
        const auto error = snow_selected_text_start(m_service.get(), &options, &request, nullptr);
        m_request.reset(request);
        if (error == 0)
            m_activity = std::move(activity);
        return {error == 0 ? SelectedTextStatus::Pending : errorStatus(error), {}};
    }

    SelectedTextCaptureResult poll() override {
        const auto status = snow_selected_text_request_poll(m_request.get());
        if (status == SNOW_SELECTED_TEXT_PENDING) {
            return {};
        }
        SnowSelectedTextResult* rawResult = nullptr;
        const auto outcome = snow_selected_text_request_result(m_request.get(), &rawResult);
        const std::unique_ptr<SnowSelectedTextResult, decltype(&snow_selected_text_result_destroy)>
            result(rawResult, snow_selected_text_result_destroy);
        m_request.reset();
        retainUntilNativeIdle();
        switch (outcome) {
        case SNOW_SELECTED_TEXT_SELECTED: {
            SnowSelectedTextBytes text{};
            if (snow_selected_text_result_text(result.get(), &text)) {
                return {SelectedTextStatus::Selected,
                        QString::fromUtf8(reinterpret_cast<const char*>(text.data),
                                          static_cast<qsizetype>(text.length))};
            }
            break;
        }
        case SNOW_SELECTED_TEXT_NO_SELECTION:
            return {SelectedTextStatus::NoSelection, {}};
        case SNOW_SELECTED_TEXT_UNSUPPORTED:
            return {SelectedTextStatus::Unsupported, {}};
        default:
            SnowSelectedTextError error{};
            if (snow_selected_text_result_error(result.get(), &error)) {
                return {errorStatus(error.kind), {}};
            }
            break;
        }
        return {SelectedTextStatus::Failed, {}};
    }

    void cancel() override {
        if (m_request) {
            snow_selected_text_request_cancel(m_request.get());
            m_request.reset();
        }
        retainUntilNativeIdle();
    }

  private:
    void retainUntilNativeIdle() {
        if (!m_activity)
            return;
        if (!snow_selected_text_service_busy(m_service.get())) {
            m_activity = {};
            return;
        }
        // Native cancellation publishes its result before clipboard restoration and
        // provider cleanup finish. Retain the service and reservation after this owner dies.
        auto* timer = new QTimer(QCoreApplication::instance());
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, timer,
                         [service = m_service, activity = std::move(m_activity), timer] {
                             static_cast<void>(activity);
                             if (!snow_selected_text_service_busy(service.get())) {
                                 timer->stop();
                                 timer->deleteLater();
                             }
                         });
        timer->start();
    }

    std::shared_ptr<SnowSelectedTextService> m_service;
    runtime::RuntimeActivityLease m_activity;
    std::unique_ptr<SnowSelectedTextRequest, decltype(&snow_selected_text_request_destroy)>
        m_request{nullptr, snow_selected_text_request_destroy};
};
} // namespace

SelectedTextTranslationController::SelectedTextTranslationController(QObject* parent)
    : SelectedTextTranslationController(std::make_unique<NativeSelectedTextCaptureBackend>(),
                                        parent) {}

SelectedTextTranslationController::SelectedTextTranslationController(
    std::unique_ptr<SelectedTextCaptureBackend> backend, QObject* parent)
    : QObject(parent), m_backend(std::move(backend)) {
    m_pollTimer.setInterval(20);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() { acceptResult(m_backend->poll()); });
}

SelectedTextTranslationController::~SelectedTextTranslationController() {
    shutdown();
}

void SelectedTextTranslationController::capture() {
    if (m_shutdown || m_pending) {
        return;
    }
    m_pending = true;
    m_activity = runtime::RuntimeActivityTracker::shared().acquire();
    m_pollTimer.start();
    acceptResult(m_backend->start());
}

void SelectedTextTranslationController::shutdown() {
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;
    cancel();
}

void SelectedTextTranslationController::cancel() {
    m_pending = false;
    m_pollTimer.stop();
    m_backend->cancel();
    m_activity = {};
}

void SelectedTextTranslationController::acceptResult(const SelectedTextCaptureResult& result) {
    if (m_shutdown || !m_pending || result.status == SelectedTextStatus::Pending) {
        return;
    }
    m_pollTimer.stop();
    m_pending = false;
    const auto activity = std::move(m_activity);
    // Every completed capture hands off, even with empty text; the destination opens the
    // translation page and warns there when nothing was retrieved.
    emit textReady(result.text);
}
} // namespace snow_shot::presentation
