#include "snow_shot/presentation/screenshotlatexrenderer.h"

#ifdef _MSC_VER
// Upstream inline templates contain intentional narrow character conversions. Keep
// their diagnostics local to these headers; the worker retains the app's strict checks.
#pragma warning(push)
#pragma warning(disable : 4242)
#endif
#include "core/formula.h"
#include "fonts/fonts.h"
#include "latex.h"
#include "platform/qt/graphic_qt.h"
#include "snow_preview.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <QCoreApplication>
#include <QMetaObject>
#include <QPainter>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

static void initializeLatexResources() {
    Q_INIT_RESOURCE(snow_shot_microtex);
}

namespace {

using Error = ScreenshotLatexRenderer::Error;

struct RasterKey final {
    QString source;
    QSize viewport;
    qreal dpr = 1;
    QRgb foreground = 0;

    bool operator==(const RasterKey&) const = default;
};

struct CachedRaster final {
    RasterKey key;
    QImage image;
};

struct PreviewState {
    ScreenshotLatexRenderer* receiver = nullptr; // Accessed only on the GUI thread.
    std::atomic<quint64> generation{0};
    std::shared_ptr<std::atomic_bool> cancellation;
    std::atomic<quint64> cacheEpoch{0};
    // The following fields belong exclusively to the serialized render worker.
    quint64 workerCacheEpoch = 0;
    std::deque<CachedRaster> cache;
};

struct RenderRequest final {
    std::shared_ptr<PreviewState> state;
    std::shared_ptr<std::atomic_bool> canceled;
    quint64 token = 0;
    QString source;
    QSize viewport;
    qreal dpr = 1;
    QColor foreground;
    bool clearOnly = false;
};

struct RenderResult final {
    QImage image;
    Error error = Error::None;
};

class FormulaScope final {
  public:
    explicit FormulaScope(bool& unusable) : m_unusable(unusable) {
        try {
            tex::SnowSessionState::reset();
        } catch (...) {
            m_unusable = true;
            throw;
        }
    }
    ~FormulaScope() noexcept {
        try {
            tex::SnowSessionState::reset();
        } catch (...) {
            // Registry restoration may allocate. Never throw while a parser exception
            // unwinds; stop accepting engine work if restoration cannot finish safely.
            m_unusable = true;
        }
    }

  private:
    bool& m_unusable;
};

RenderResult renderFormula(const RenderRequest& request, bool& initializationFailed) {
    constexpr int maximumDimension = 8192;
    constexpr qint64 maximumPixels = 16000000;
    if (request.source.size() > 32768 || !request.viewport.isValid() ||
        !std::isfinite(request.dpr) || request.dpr <= 0 || request.dpr > 8 ||
        request.viewport.width() > maximumDimension ||
        request.viewport.height() > maximumDimension) {
        return {{}, Error::LimitExceeded};
    }
    if (request.source.trimmed().isEmpty()) {
        return {};
    }
    const int padding = std::min({12, request.viewport.width() / 8, request.viewport.height() / 8});
    // Reset before installing the budget and after removing it. Cancellation must never
    // interrupt restoration of global registries or leave a half-restored next request.
    FormulaScope session(initializationFailed);
    tex::SnowPreviewBudget budget(*request.canceled);
    tex::Formula formula(false, request.source.toStdWString());
    tex::TeXRenderBuilder builder;
    const auto width = static_cast<float>(std::max(1, request.viewport.width() - padding * 2));
    const auto foreground = static_cast<tex::color>(request.foreground.rgba());
    std::unique_ptr<tex::TeXRender> render(
        builder.setStyle(tex::TexStyle::display)
            .setTextSize(24)
            .setWidth(tex::UnitType::pixel, width, tex::Alignment::left)
            .setIsMaxWidth(true)
            .setLineSpace(tex::UnitType::pixel, 8)
            .setForeground(foreground)
            .build(formula));
    tex::SnowPreviewBudget::checkpoint();
    const QSize logicalSize(std::max(1, render->getWidth() + padding * 2),
                            std::max(1, render->getHeight() + padding * 2));
    const double physicalWidth = std::ceil(static_cast<double>(logicalSize.width()) * request.dpr);
    const double physicalHeight =
        std::ceil(static_cast<double>(logicalSize.height()) * request.dpr);
    if (!std::isfinite(physicalWidth) || !std::isfinite(physicalHeight) ||
        physicalWidth > maximumDimension || physicalHeight > maximumDimension ||
        physicalWidth * physicalHeight > static_cast<double>(maximumPixels)) {
        return {{}, Error::LimitExceeded};
    }
    QImage image(QSize(static_cast<int>(physicalWidth), static_cast<int>(physicalHeight)),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {{}, Error::LimitExceeded};
    }
    image.setDevicePixelRatio(request.dpr);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        tex::Graphics2D_qt graphics(&painter);
        render->draw(graphics, padding, padding);
    }
    tex::SnowPreviewBudget::checkpoint();
    return {std::move(image), Error::None};
}

class RenderWorker;
RenderWorker* sharedWorker = nullptr;
void stopSharedWorker();

class RenderWorker final : public QThread {
  public:
    explicit RenderWorker(QCoreApplication* application)
        : QThread(application), m_app(application) {
        QObject::connect(
            application, &QCoreApplication::aboutToQuit, this, [this]() { shutdown(); },
            Qt::DirectConnection);
        start(QThread::LowPriority);
    }

    ~RenderWorker() override {
        shutdown();
        if (sharedWorker == this) {
            sharedWorker = nullptr;
            qRemovePostRoutine(stopSharedWorker);
        }
    }

    void submit(RenderRequest request) {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            // At most one queued request per preview, even while another formula is parsing.
            std::erase_if(m_pending, [&](const RenderRequest& pending) {
                return pending.state == request.state;
            });
            m_pending.push_back(std::move(request));
        }
        m_ready.notify_one();
    }

    void forget(const std::shared_ptr<PreviewState>& state) {
        submit({state, {}, 0, {}, {}, 1, {}, true});
    }

    void shutdown() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            if (m_activeCancellation) {
                m_activeCancellation->store(true, std::memory_order_relaxed);
            }
            m_pending.clear();
        }
        m_ready.notify_one();
        wait();
    }

  protected:
    void run() override {
        bool initialized = false;
        bool initializationFailed = false;
        for (;;) {
            RenderRequest request;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock, [this]() { return m_stopping || !m_pending.empty(); });
                if (m_stopping) {
                    break;
                }
                request = std::move(m_pending.front());
                m_pending.pop_front();
                m_activeCancellation = request.canceled;
            }
            auto& state = *request.state;
            const auto cacheEpoch = state.cacheEpoch.load(std::memory_order_relaxed);
            if (cacheEpoch != state.workerCacheEpoch || request.clearOnly) {
                state.cache.clear();
                state.workerCacheEpoch = cacheEpoch;
            }
            if (request.clearOnly) {
                continue;
            }
            if (request.canceled->load(std::memory_order_relaxed)) {
                continue;
            }
            RenderResult result;
            const RasterKey key{request.source, request.viewport, request.dpr,
                                request.foreground.rgba()};
            try {
                if (!initialized && !initializationFailed && !request.source.trimmed().isEmpty()) {
                    initializeLatexResources();
                    tex::LaTeX::init(":/snow-shot/microtex");
                    // Load immutable alphabet metrics before installing a cancellable budget.
                    // Fonts themselves remain lazy. Aborted parses cannot partially add metrics.
                    tex::GreekRegistration greek;
                    tex::CyrillicRegistration cyrillic;
                    tex::DefaultTeXFont::addAlphabet(&greek);
                    tex::DefaultTeXFont::addAlphabet(&cyrillic);
                    initialized = true;
                }
                const auto cached =
                    std::find_if(state.cache.begin(), state.cache.end(),
                                 [&](const CachedRaster& entry) { return entry.key == key; });
                if (initializationFailed) {
                    state.cache.clear();
                    result.error = Error::InitializationFailed;
                } else if (cached != state.cache.end()) {
                    auto entry = std::move(*cached);
                    state.cache.erase(cached);
                    result.image = entry.image;
                    state.cache.push_back(std::move(entry));
                } else {
                    result = renderFormula(request, initializationFailed);
                    if (initializationFailed) {
                        state.cache.clear();
                        result = {{}, Error::InitializationFailed};
                    }
                    if (!request.canceled->load(std::memory_order_relaxed) &&
                        state.cacheEpoch.load(std::memory_order_relaxed) == cacheEpoch &&
                        result.error == Error::None && !result.image.isNull()) {
                        state.cache.push_back({key, result.image});
                        // Two completed inputs, with at most one full 16M-pixel allocation
                        // retained in aggregate. QImage copies share the cached storage.
                        constexpr qsizetype maximumCacheBytes = 64000000;
                        qsizetype bytes = 0;
                        for (const auto& entry : state.cache) {
                            bytes += entry.image.sizeInBytes();
                        }
                        while (state.cache.size() > 2 || bytes > maximumCacheBytes) {
                            bytes -= state.cache.front().image.sizeInBytes();
                            state.cache.pop_front();
                        }
                    }
                }
            } catch (const tex::SnowPreviewCanceled&) {
                continue;
            } catch (const tex::SnowPreviewLimit&) {
                result.error = Error::LimitExceeded;
            } catch (const tex::SnowPreviewResource&) {
                result.error = Error::InitializationFailed;
            } catch (const std::bad_alloc&) {
                initializationFailed = initializationFailed || !initialized;
                result.error = Error::LimitExceeded;
            } catch (const std::exception&) {
                initializationFailed = initializationFailed || !initialized;
                result.error = initialized ? Error::InvalidFormula : Error::InitializationFailed;
            }
            if (initializationFailed) {
                state.cache.clear();
                result = {{}, Error::InitializationFailed};
            }
            if (request.canceled->load(std::memory_order_relaxed)) {
                continue;
            }
            if ((result.error != Error::None || result.image.isNull()) && state.cache.size() > 1) {
                state.cache.pop_front(); // An invalid/empty current draft needs only the last valid
                                         // raster.
            }
            QMetaObject::invokeMethod(
                m_app,
                [state = request.state, token = request.token, result = std::move(result)]() {
                    if (state->receiver &&
                        state->generation.load(std::memory_order_relaxed) == token) {
                        emit state->receiver->rendered(token, result.image, result.error);
                    }
                },
                Qt::QueuedConnection);
        }
        if (initialized) {
            tex::LaTeX::release();
        }
    }

  private:
    QCoreApplication* m_app;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<RenderRequest> m_pending;
    std::shared_ptr<std::atomic_bool> m_activeCancellation;
    bool m_stopping = false;
};

void stopSharedWorker() {
    auto* worker = std::exchange(sharedWorker, nullptr);
    if (worker) {
        worker->shutdown();
        delete worker;
    }
}

RenderWorker* renderWorker() {
    if (!sharedWorker) {
        sharedWorker = new RenderWorker(QCoreApplication::instance());
        // QGuiApplication runs post routines before destroying its font backend, including
        // short-lived offscreen tests that never enter QCoreApplication::exec().
        qAddPostRoutine(stopSharedWorker);
    }
    return sharedWorker;
}

} // namespace

struct ScreenshotLatexRenderer::State final : PreviewState {};

ScreenshotLatexRenderer::ScreenshotLatexRenderer(QObject* parent)
    : QObject(parent), m_state(std::make_shared<State>()) {
    m_state->receiver = this;
}

ScreenshotLatexRenderer::~ScreenshotLatexRenderer() {
    clearCache();
    m_state->receiver = nullptr;
}

quint64 ScreenshotLatexRenderer::request(const QString& source, QSize viewportSize,
                                         qreal devicePixelRatio, QColor foreground) {
    cancel();
    const quint64 token = m_state->generation.load(std::memory_order_relaxed);
    m_state->cancellation = std::make_shared<std::atomic_bool>(false);
    renderWorker()->submit({m_state, m_state->cancellation, token, source, viewportSize,
                            devicePixelRatio, std::move(foreground)});
    return token;
}

void ScreenshotLatexRenderer::cancel() {
    if (m_state->cancellation) {
        m_state->cancellation->store(true, std::memory_order_relaxed);
    }
    m_state->generation.fetch_add(1, std::memory_order_relaxed);
}

void ScreenshotLatexRenderer::clearCache() {
    cancel();
    m_state->cacheEpoch.fetch_add(1, std::memory_order_relaxed);
    if (sharedWorker) {
        sharedWorker->forget(m_state);
    }
}
