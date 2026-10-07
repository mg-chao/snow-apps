#include "snow_canvas_runtime_session.h"
#include "snow_canvas_runtime_cleanup.h"

#include "snow_canvas_changed_viewports.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_fill_render.h"
#include "snow_canvas_text_render.h"
#include "snow_canvas_type_conversions.h"
#include "snow_canvas_watermark_renderer.h"

#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace snow_canvas_runtime {
namespace {

void clearDrawingCachesForCurrentThread() {
    // Export sessions have no canvas clients, and their threads outlive the document.
    snow_canvas_fill_render::resetHatchTextureCacheForCurrentThread();
    snow_canvas_renderer::resetWatermarkRenderCacheForCurrentThread();
    snow_canvas_text_render::resetLayoutCacheForCurrentThread();
}

bool toEngineRuntimeConfig(const SnowCanvasRuntimeConfig& config, SnowStyleDefaults& styleDefaults,
                           SnowRuntimeConfig& engineConfig) {
    engineConfig = SnowRuntimeConfig{};
    if (!config.styleDefaults.has_value()) {
        return true;
    }
    if (!snow_canvas_types::toEngineStyleDefaults(*config.styleDefaults, styleDefaults)) {
        return false;
    }
    engineConfig.style_defaults = &styleDefaults;
    return true;
}

std::uint64_t quickSelectionDisabledToolMask(const SnowCanvasRuntimeConfig& config) {
    std::uint64_t mask = 0;
    for (const SnowCanvasTool tool : config.quickSelectionDisabledTools) {
        mask |= std::uint64_t{1} << static_cast<unsigned>(snow_canvas_types::toEngineTool(tool));
    }
    return mask;
}

void applyQuickSelectionPolicy(SnowRuntime runtime, std::uint64_t toolMask) {
    // Runtime policy is shared by every viewport attached to this document session.
    ScopedChangedViewportList changedViewports;
    static_cast<void>(snow_runtime_set_quick_selection_disabled_tools_ex(
        runtime, toolMask, changedViewports.outParam()));
}

ScopedRuntimeHandle createRuntime(const SnowCanvasRuntimeConfig& config) {
    ScopedRuntimeHandle runtime;
    SnowStyleDefaults styleDefaults{};
    SnowRuntimeConfig engineConfig{};
    if (!toEngineRuntimeConfig(config, styleDefaults, engineConfig) ||
        snow_runtime_create_with_config(&engineConfig, runtime.outParam()) != SNOW_OK) {
        runtime.reset();
    }
    if (runtime.get() != nullptr) {
        applyQuickSelectionPolicy(runtime.get(), quickSelectionDisabledToolMask(config));
    }
    return runtime;
}

ScopedRuntimeHandle cloneRuntimeFrom(SnowRuntime source, const SnowCanvasRuntimeConfig& config) {
    ScopedRuntimeHandle runtime;
    if (source == nullptr) {
        return runtime;
    }
    SnowStyleDefaults styleDefaults{};
    SnowRuntimeConfig engineConfig{};
    if (!toEngineRuntimeConfig(config, styleDefaults, engineConfig) ||
        snow_runtime_clone_document_session_with_config(source, &engineConfig,
                                                        runtime.outParam()) != SNOW_OK) {
        runtime.reset();
    }
    if (runtime.get() != nullptr) {
        applyQuickSelectionPolicy(runtime.get(), quickSelectionDisabledToolMask(config));
    }
    return runtime;
}

ScopedRuntimeHandle runtimeFromSerializedSession(const QByteArray& payload,
                                                 const SnowCanvasRuntimeConfig& config) {
    ScopedRuntimeHandle runtime;
    SnowStyleDefaults styleDefaults{};
    SnowRuntimeConfig engineConfig{};
    if (payload.isEmpty() || !toEngineRuntimeConfig(config, styleDefaults, engineConfig)) {
        qWarning("Canvas session restoration failed: stage=input_validation bytes=%lld",
                 static_cast<long long>(payload.size()));
        return runtime;
    }
    const auto status = snow_runtime_create_from_document_session_with_config(
        reinterpret_cast<const std::uint8_t*>(payload.constData()),
        static_cast<std::size_t>(payload.size()), &engineConfig, runtime.outParam());
    if (status != SNOW_OK) {
        qWarning("Canvas session restoration failed: stage=deserialize status=%d bytes=%lld",
                 static_cast<int>(status), static_cast<long long>(payload.size()));
        runtime.reset();
    }
    if (runtime.get() != nullptr) {
        applyQuickSelectionPolicy(runtime.get(), quickSelectionDisabledToolMask(config));
    }
    return runtime;
}

ScopedRuntimeHandle runtimeFromSerializedHistory(const QByteArray& payload,
                                                 const SnowCanvasRuntimeConfig& config) {
    ScopedRuntimeHandle runtime;
    SnowStyleDefaults styleDefaults{};
    SnowRuntimeConfig engineConfig{};
    if (payload.isEmpty() || !toEngineRuntimeConfig(config, styleDefaults, engineConfig) ||
        snow_runtime_create_from_document_history_with_config(
            reinterpret_cast<const std::uint8_t*>(payload.constData()),
            static_cast<std::size_t>(payload.size()), &engineConfig,
            runtime.outParam()) != SNOW_OK) {
        runtime.reset();
    }
    if (runtime.get() != nullptr) {
        applyQuickSelectionPolicy(runtime.get(), quickSelectionDisabledToolMask(config));
    }
    return runtime;
}

void destroyNow(SnowRuntime runtime) noexcept {
    if (runtime != nullptr) {
        snow_runtime_destroy(runtime);
    }
}

class RuntimeCleanupQueue final {
  public:
    RuntimeCleanupQueue() : m_worker([this] { run(); }) {}

    ~RuntimeCleanupQueue() {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_pendingChanged.notify_one();
        m_worker.join();
    }

    void enqueue(std::function<void()> cleanup) {
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_pending.push_back(std::move(cleanup));
        }
        m_pendingChanged.notify_one();
    }

    void waitUntilIdle() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle.wait(lock, [this] { return m_pending.empty() && !m_running; });
    }

  private:
    void run() noexcept {
        for (;;) {
            std::function<void()> cleanup;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_pendingChanged.wait(lock, [this] { return m_stopping || !m_pending.empty(); });
                if (m_pending.empty()) {
                    return;
                }
                cleanup = std::move(m_pending.front());
                m_pending.pop_front();
                m_running = true;
            }
            cleanup();
            cleanup = {};
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_running = false;
                if (m_pending.empty()) {
                    m_idle.notify_all();
                }
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_pendingChanged;
    std::condition_variable m_idle;
    std::deque<std::function<void()>> m_pending;
    bool m_stopping = false;
    bool m_running = false;
    std::thread m_worker;
};

RuntimeCleanupQueue& runtimeCleanupQueue() {
    static RuntimeCleanupQueue queue;
    return queue;
}

} // namespace

void enqueueRuntimeCleanup(std::function<void()> cleanup) {
    if (cleanup) {
        runtimeCleanupQueue().enqueue(std::move(cleanup));
    }
}

void waitForRuntimeCleanup() {
    runtimeCleanupQueue().waitUntilIdle();
}

RuntimeSession::RuntimeSession() : RuntimeSession(SnowCanvasRuntimeConfig{}) {}

RuntimeSession::RuntimeSession(const SnowCanvasRuntimeConfig& config)
    : m_config(config), m_runtime(createRuntime(m_config)),
      m_smartErase([this] { m_clients.smartEraseChanged(); }) {
    // Register process cleanup before any runtime owner can register its static destructor.
    // The worker then drains detached handles after every runtime owner has been destroyed.
    static_cast<void>(runtimeCleanupQueue());
}

RuntimeSession::~RuntimeSession() {
    destroyRuntimeAsync();
}

bool RuntimeSession::isValid() const {
    return m_runtime.get() != nullptr;
}

bool RuntimeSession::reset() {
    if (!replaceRuntime(createRuntime(m_config))) {
        return false;
    }
    clearDrawingCachesForCurrentThread();
    return true;
}

bool RuntimeSession::cloneDocumentSessionFrom(const RuntimeSession& source) {
    if (source.m_runtime.get() == nullptr) {
        return false;
    }

    return replaceRuntime(cloneRuntimeFrom(source.m_runtime.get(), m_config));
}

QByteArray RuntimeSession::serializeDocumentSession() const {
    if (m_runtime.get() == nullptr) {
        return {};
    }
    SnowSerializedBytes bytes = nullptr;
    if (snow_runtime_serialize_document_session_bytes(m_runtime.get(), &bytes) != SNOW_OK) {
        return {};
    }
    const std::unique_ptr<SnowSerializedBytesImpl, decltype(&snow_serialized_bytes_destroy)>
        snapshot(bytes, &snow_serialized_bytes_destroy);
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    if (snow_serialized_bytes_data(snapshot.get(), &data, &size) != SNOW_OK || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(size));
}

QByteArray RuntimeSession::serializeSelectedDrawTemplate() const {
    if (m_runtime.get() == nullptr) {
        return {};
    }
    std::size_t size = 0;
    if (snow_runtime_serialize_selected_draw_template(m_runtime.get(), nullptr, 0, &size) !=
            SNOW_OK ||
        size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
    std::size_t written = 0;
    if (snow_runtime_serialize_selected_draw_template(
            m_runtime.get(), reinterpret_cast<std::uint8_t*>(payload.data()), size, &written) !=
            SNOW_OK ||
        written != size) {
        return {};
    }
    return payload;
}

bool RuntimeSession::restoreDocumentSession(const QByteArray& payload) {
    return replaceRuntime(runtimeFromSerializedSession(payload, m_config));
}

QByteArray RuntimeSession::serializeDocumentHistory() const {
    if (m_runtime.get() == nullptr) {
        return {};
    }
    std::size_t size = 0;
    if (snow_runtime_serialize_document_history(m_runtime.get(), nullptr, 0, &size) != SNOW_OK ||
        size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
    std::size_t written = 0;
    if (snow_runtime_serialize_document_history(m_runtime.get(),
                                                reinterpret_cast<std::uint8_t*>(payload.data()),
                                                size, &written) != SNOW_OK ||
        written != size) {
        return {};
    }
    return payload;
}

bool RuntimeSession::restoreDocumentHistory(const QByteArray& payload) {
    return replaceRuntime(runtimeFromSerializedHistory(payload, m_config));
}

bool RuntimeSession::restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload) {
    if (m_runtime.get() == nullptr || payload.isEmpty()) {
        return false;
    }
    ScopedChangedViewportList changedViewports;
    if (snow_runtime_restore_document_history_preserving_editor_styles(
            m_runtime.get(), reinterpret_cast<const std::uint8_t*>(payload.constData()),
            static_cast<std::size_t>(payload.size()), changedViewports.outParam()) != SNOW_OK) {
        return false;
    }
    m_smartErase.reset();
    syncChangedViewports(changedViewports.get());
    return true;
}

bool RuntimeSession::clearDocumentPreservingViewports() {
    if (m_runtime.get() == nullptr || !m_clients.resetEditingState()) {
        return false;
    }

    ScopedChangedViewportList changedViewports;
    if (snow_runtime_clear_document_preserving_viewports(m_runtime.get(),
                                                         changedViewports.outParam()) != SNOW_OK) {
        return false;
    }

    m_smartErase.reset();
    m_clients.resetDocumentRetainedState();
    syncChangedViewports(changedViewports.get());
    clearDrawingCachesForCurrentThread();
    return true;
}

void RuntimeSession::clearRenderState() {
    m_smartErase.clearCache();
    m_clients.clearRenderState();
    clearDrawingCachesForCurrentThread();
}

bool RuntimeSession::setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools) {
    if (m_runtime.get() == nullptr) {
        return false;
    }

    SnowCanvasRuntimeConfig nextConfig = m_config;
    nextConfig.quickSelectionDisabledTools = tools;
    ScopedChangedViewportList changedViewports;
    if (snow_runtime_set_quick_selection_disabled_tools_ex(
            m_runtime.get(), quickSelectionDisabledToolMask(nextConfig),
            changedViewports.outParam()) != SNOW_OK) {
        return false;
    }
    m_config = std::move(nextConfig);
    syncChangedViewports(changedViewports.get());
    return true;
}

void RuntimeSession::destroyAsync(SnowCanvasRuntime& owner) {
    m_clients.detachAndReleaseClients(owner);
    destroyRuntimeAsync();
}

void RuntimeSession::destroyForOwnerDestruction(SnowCanvasRuntime& owner,
                                                OwnerDestructionPolicy policy) {
    if (policy == OwnerDestructionPolicy::DetachClients) {
        m_clients.detachAndReleaseClients(owner);
        return;
    }

    m_clients.abandonClientsWithoutDetach();
    destroyRuntimeAsync();
}

SnowRuntime RuntimeSession::handle() const {
    return m_runtime.get();
}

void RuntimeSession::registerClient(Client* client) {
    m_clients.registerClient(client);
}

void RuntimeSession::unregisterClient(Client* client) {
    m_clients.unregisterClient(client);
    m_smartErase.removeSources(client);
    m_smartErase.sync(m_runtime.get());
}

void RuntimeSession::syncChangedViewports(SnowChangedViewportList changedViewports) {
    if (changedViewports == nullptr) {
        return;
    }

    syncChangedViewportIds(snow_canvas_changed_viewports::idsFromList(changedViewports));
}

void RuntimeSession::syncChangedViewportIds(const std::vector<std::uint64_t>& changedViewportIds) {
    m_smartErase.sync(m_runtime.get());
    m_clients.syncChangedViewports(changedViewportIds);
}

bool RuntimeSession::replaceRuntime(ScopedRuntimeHandle replacement) {
    if (replacement.get() == nullptr) {
        return false;
    }

    m_smartErase.reset();
    const SnowRuntime runtime = replacement.get();
    const ClientRegistry::DetachedClients detachedClients = m_clients.detachForRuntimeReplacement();
    m_runtime.reset(replacement.release());
    m_clients.attachAfterRuntimeReplacement(detachedClients, runtime);
    m_smartErase.sync(runtime);
    return true;
}

void RuntimeSession::destroyRuntimeAsync() {
    const SnowRuntime runtime = m_runtime.release();
    if (runtime != nullptr) {
        try {
            enqueueRuntimeCleanup([runtime]() noexcept { destroyNow(runtime); });
        } catch (...) {
            // Allocation or worker creation failure must still release the detached handle.
            destroyNow(runtime);
        }
    }
    clearDrawingCachesForCurrentThread();
}

} // namespace snow_canvas_runtime
