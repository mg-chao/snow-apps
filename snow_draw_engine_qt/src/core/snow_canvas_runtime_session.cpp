#include "snow_canvas_runtime_session.h"

#include "snow_canvas_changed_viewports.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_type_conversions.h"

#include <future>
#include <limits>
#include <utility>

namespace snow_canvas_runtime {
namespace {

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

ScopedRuntimeHandle createRuntime(const SnowCanvasRuntimeConfig& config) {
    ScopedRuntimeHandle runtime;
    SnowStyleDefaults styleDefaults{};
    SnowRuntimeConfig engineConfig{};
    if (!toEngineRuntimeConfig(config, styleDefaults, engineConfig) ||
        snow_runtime_create_with_config(&engineConfig, runtime.outParam()) != SNOW_OK) {
        runtime.reset();
    }
    // Runtime policy is shared by every viewport attached to this document session.
    if (runtime.get() != nullptr) {
        static_cast<void>(snow_runtime_set_quick_selection_disabled_tools(
            runtime.get(), quickSelectionDisabledToolMask(config)));
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
        static_cast<void>(snow_runtime_set_quick_selection_disabled_tools(
            runtime.get(), quickSelectionDisabledToolMask(config)));
    }
    return runtime;
}

ScopedRuntimeHandle runtimeFromSerializedSession(const QByteArray& payload,
                                                 const SnowCanvasRuntimeConfig& config) {
    ScopedRuntimeHandle runtime;
    SnowStyleDefaults styleDefaults{};
    SnowRuntimeConfig engineConfig{};
    if (payload.isEmpty() || !toEngineRuntimeConfig(config, styleDefaults, engineConfig) ||
        snow_runtime_create_from_document_session_with_config(
            reinterpret_cast<const std::uint8_t*>(payload.constData()),
            static_cast<std::size_t>(payload.size()), &engineConfig,
            runtime.outParam()) != SNOW_OK) {
        runtime.reset();
    }
    if (runtime.get() != nullptr) {
        static_cast<void>(snow_runtime_set_quick_selection_disabled_tools(
            runtime.get(), quickSelectionDisabledToolMask(config)));
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
        static_cast<void>(snow_runtime_set_quick_selection_disabled_tools(
            runtime.get(), quickSelectionDisabledToolMask(config)));
    }
    return runtime;
}

void destroyNow(SnowRuntime runtime) noexcept {
    if (runtime != nullptr) {
        snow_runtime_destroy(runtime);
    }
}

std::future<void> startAsyncDestroy(SnowRuntime runtime) noexcept {
    if (runtime == nullptr) {
        return {};
    }

    try {
        return std::async(std::launch::async, [runtime]() noexcept { destroyNow(runtime); });
    } catch (...) {
        destroyNow(runtime);
        return {};
    }
}

} // namespace

RuntimeSession::RuntimeSession() : RuntimeSession(SnowCanvasRuntimeConfig{}) {}

RuntimeSession::RuntimeSession(const SnowCanvasRuntimeConfig& config)
    : m_config(config), m_runtime(createRuntime(m_config)) {}

RuntimeSession::~RuntimeSession() {
    m_runtime.reset();
    waitForPendingDestroy();
}

bool RuntimeSession::isValid() const {
    return m_runtime.get() != nullptr;
}

bool RuntimeSession::reset() {
    return replaceRuntime(createRuntime(m_config));
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
    std::size_t size = 0;
    if (snow_runtime_serialize_document_session(m_runtime.get(), nullptr, 0, &size) != SNOW_OK ||
        size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
    std::size_t written = 0;
    if (snow_runtime_serialize_document_session(m_runtime.get(),
                                                reinterpret_cast<std::uint8_t*>(payload.data()),
                                                size, &written) != SNOW_OK ||
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
    syncChangedViewports(changedViewports.get());
    return true;
}

bool RuntimeSession::clearDocumentPreservingViewports() {
    if (m_runtime.get() == nullptr) {
        return false;
    }

    ScopedChangedViewportList changedViewports;
    if (snow_runtime_clear_document_preserving_viewports(m_runtime.get(),
                                                         changedViewports.outParam()) != SNOW_OK) {
        return false;
    }

    syncChangedViewports(changedViewports.get());
    return true;
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
    waitForPendingDestroy();
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
}

void RuntimeSession::syncChangedViewports(SnowChangedViewportList changedViewports) {
    if (changedViewports == nullptr) {
        return;
    }

    syncChangedViewportIds(snow_canvas_changed_viewports::idsFromList(changedViewports));
}

void RuntimeSession::syncChangedViewportIds(const std::vector<std::uint64_t>& changedViewportIds) {
    m_clients.syncChangedViewports(changedViewportIds);
}

bool RuntimeSession::replaceRuntime(ScopedRuntimeHandle replacement) {
    waitForPendingDestroy();
    if (replacement.get() == nullptr) {
        return false;
    }

    const SnowRuntime runtime = replacement.get();
    const ClientRegistry::DetachedClients detachedClients = m_clients.detachForRuntimeReplacement();
    m_runtime.reset(replacement.release());
    m_clients.attachAfterRuntimeReplacement(detachedClients, runtime);
    return true;
}

void RuntimeSession::destroyRuntimeAsync() {
    waitForPendingDestroy();
    m_pendingDestroy = startAsyncDestroy(m_runtime.release());
}

void RuntimeSession::waitForPendingDestroy() {
    if (!m_pendingDestroy.valid()) {
        return;
    }

    m_pendingDestroy.wait();
    m_pendingDestroy = std::future<void>();
}

} // namespace snow_canvas_runtime
