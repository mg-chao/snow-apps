#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include "snow_canvas_export.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_runtime_session.h"
#include "snow_canvas_runtime_thread_affinity.h"
#include "snow_canvas_ffi_handles.h"

#include <memory>

struct SnowCanvasRuntime::Impl {
    Impl(SnowCanvasRuntime& owner, const SnowCanvasRuntimeConfig& config);
    ~Impl();

    bool isOwnerThread() const;
    bool isValid() const;
    bool reset();
    bool cloneDocumentSessionFrom(const Impl& source);
    QByteArray serializeDocumentSession() const;
    QByteArray serializeSelectedDrawTemplate() const;
    bool restoreDocumentSession(const QByteArray& payload);
    QByteArray serializeDocumentHistory() const;
    bool restoreDocumentHistory(const QByteArray& payload);
    bool restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload);
    bool clearDocumentPreservingViewports();
    bool setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools);
    void destroyAsync();
    QImage renderToImage(const QRectF& virtualSelectionRect, const QSize& outputSize,
                         const QList<CanvasExportSource>& sources);

    snow_canvas_smart_erase::Coordinator& smartErase() {
        return session.smartErase();
    }
    SnowRuntime handle() const;
    void registerClient(snow_canvas_runtime::Client* client);
    void unregisterClient(snow_canvas_runtime::Client* client);
    void syncChangedViewports(SnowChangedViewportList changedViewports);
    void syncChangedViewportIds(const std::vector<std::uint64_t>& changedViewportIds);
    std::function<void()> documentChanged;
    quint64 observedRevision = 0;
    void observeDocument() {
        const auto revision = snow_runtime_document_revision(handle());
        if (revision != observedRevision) {
            observedRevision = revision;
            if (documentChanged)
                documentChanged();
        }
    }

  private:
    bool hasThreadAccess(const char* operation) const;

    SnowCanvasRuntime& owner;
    snow_canvas_runtime::ThreadAffinity threadAffinity;
    snow_canvas_runtime::RuntimeSession session;
};

SnowCanvasRuntime::Impl::Impl(SnowCanvasRuntime& owner, const SnowCanvasRuntimeConfig& config)
    : owner(owner), session(config) {}

SnowCanvasRuntime::Impl::~Impl() {
    session.destroyForOwnerDestruction(
        owner, threadAffinity.hasDestructionAccess("~SnowCanvasRuntime")
                   ? snow_canvas_runtime::OwnerDestructionPolicy::DetachClients
                   : snow_canvas_runtime::OwnerDestructionPolicy::AbandonClientsAndDestroyAsync);
}

bool SnowCanvasRuntime::Impl::isOwnerThread() const {
    return threadAffinity.isOwnerThread();
}

bool SnowCanvasRuntime::Impl::isValid() const {
    if (!hasThreadAccess("isValid")) {
        return false;
    }
    return session.isValid();
}

bool SnowCanvasRuntime::Impl::reset() {
    if (!hasThreadAccess("reset")) {
        return false;
    }

    return session.reset();
}

bool SnowCanvasRuntime::Impl::cloneDocumentSessionFrom(const Impl& source) {
    if (!hasThreadAccess("cloneDocumentSessionFrom") ||
        !source.hasThreadAccess("cloneDocumentSessionFrom(source)")) {
        return false;
    }
    return session.cloneDocumentSessionFrom(source.session);
}

QByteArray SnowCanvasRuntime::Impl::serializeDocumentSession() const {
    if (!hasThreadAccess("serializeDocumentSession")) {
        return {};
    }
    return session.serializeDocumentSession();
}

QByteArray SnowCanvasRuntime::Impl::serializeSelectedDrawTemplate() const {
    if (!hasThreadAccess("serializeSelectedDrawTemplate")) {
        return {};
    }
    return session.serializeSelectedDrawTemplate();
}

bool SnowCanvasRuntime::Impl::restoreDocumentSession(const QByteArray& payload) {
    return hasThreadAccess("restoreDocumentSession") && session.restoreDocumentSession(payload);
}

QByteArray SnowCanvasRuntime::Impl::serializeDocumentHistory() const {
    if (!hasThreadAccess("serializeDocumentHistory")) {
        return {};
    }
    return session.serializeDocumentHistory();
}

bool SnowCanvasRuntime::Impl::restoreDocumentHistory(const QByteArray& payload) {
    return hasThreadAccess("restoreDocumentHistory") && session.restoreDocumentHistory(payload);
}

bool SnowCanvasRuntime::Impl::restoreDocumentHistoryPreservingEditorStyles(
    const QByteArray& payload) {
    return hasThreadAccess("restoreDocumentHistoryPreservingEditorStyles") &&
           session.restoreDocumentHistoryPreservingEditorStyles(payload);
}

bool SnowCanvasRuntime::Impl::clearDocumentPreservingViewports() {
    if (!hasThreadAccess("clearDocumentPreservingViewports")) {
        return false;
    }
    return session.clearDocumentPreservingViewports();
}

bool SnowCanvasRuntime::Impl::setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools) {
    return hasThreadAccess("setQuickSelectionDisabledTools") &&
           session.setQuickSelectionDisabledTools(tools);
}

void SnowCanvasRuntime::Impl::destroyAsync() {
    if (!hasThreadAccess("destroyAsync")) {
        return;
    }

    session.destroyAsync(owner);
}

SnowRuntime SnowCanvasRuntime::Impl::handle() const {
    if (!hasThreadAccess("handle")) {
        return nullptr;
    }
    return session.handle();
}

bool SnowCanvasRuntime::Impl::hasThreadAccess(const char* operation) const {
    return threadAffinity.hasAccess(operation);
}

void SnowCanvasRuntime::Impl::registerClient(snow_canvas_runtime::Client* client) {
    if (!hasThreadAccess("registerClient")) {
        return;
    }
    session.registerClient(client);
}

void SnowCanvasRuntime::Impl::unregisterClient(snow_canvas_runtime::Client* client) {
    if (!hasThreadAccess("unregisterClient")) {
        return;
    }
    session.unregisterClient(client);
}

void SnowCanvasRuntime::Impl::syncChangedViewports(SnowChangedViewportList changedViewports) {
    if (!hasThreadAccess("syncChangedViewports")) {
        return;
    }
    session.syncChangedViewports(changedViewports);
    observeDocument();
}

void SnowCanvasRuntime::Impl::syncChangedViewportIds(
    const std::vector<std::uint64_t>& changedViewportIds) {
    if (!hasThreadAccess("syncChangedViewportIds")) {
        return;
    }
    session.syncChangedViewportIds(changedViewportIds);
    observeDocument();
}

QImage SnowCanvasRuntime::Impl::renderToImage(const QRectF& virtualSelectionRect,
                                              const QSize& outputSize,
                                              const QList<CanvasExportSource>& sources) {
    if (!hasThreadAccess("renderToImage")) {
        return {};
    }
    return snow_canvas_export::renderToImage(session.handle(), virtualSelectionRect, outputSize,
                                             sources, session.smartErase().snapshot());
}

SnowCanvasRuntime::SnowCanvasRuntime() : SnowCanvasRuntime(SnowCanvasRuntimeConfig{}) {}

SnowCanvasRuntime::SnowCanvasRuntime(const SnowCanvasRuntimeConfig& config)
    : m_impl(std::make_unique<Impl>(*this, config)) {}

SnowCanvasRuntime::~SnowCanvasRuntime() = default;

bool SnowCanvasRuntime::isOwnerThread() const {
    return m_impl->isOwnerThread();
}

bool SnowCanvasRuntime::isValid() const {
    return m_impl->isValid();
}

bool SnowCanvasRuntime::reset() {
    return m_impl->reset();
}

bool SnowCanvasRuntime::cloneDocumentSessionFrom(const SnowCanvasRuntime& source) {
    return m_impl->cloneDocumentSessionFrom(*source.m_impl);
}

QByteArray SnowCanvasRuntime::serializeDocumentSession() const {
    return m_impl->serializeDocumentSession();
}

QByteArray SnowCanvasRuntime::serializeSelectedDrawTemplate() const {
    return m_impl->serializeSelectedDrawTemplate();
}

bool SnowCanvasRuntime::restoreDocumentSession(const QByteArray& payload) {
    return m_impl->restoreDocumentSession(payload);
}

QByteArray SnowCanvasRuntime::serializeDocumentHistory() const {
    return m_impl->serializeDocumentHistory();
}

bool SnowCanvasRuntime::restoreDocumentHistory(const QByteArray& payload) {
    return m_impl->restoreDocumentHistory(payload);
}

bool SnowCanvasRuntime::restoreDocumentHistoryPreservingEditorStyles(const QByteArray& payload) {
    return m_impl->restoreDocumentHistoryPreservingEditorStyles(payload);
}

bool SnowCanvasRuntime::clearDocumentPreservingViewports() {
    return m_impl->clearDocumentPreservingViewports();
}

bool SnowCanvasRuntime::setQuickSelectionDisabledTools(const QSet<SnowCanvasTool>& tools) {
    return m_impl->setQuickSelectionDisabledTools(tools);
}

QByteArray SnowCanvasRuntime::applyAnnotationTransaction(const QByteArray& payload) {
    if (!isOwnerThread() || payload.isEmpty() || payload.size() > 1024 * 1024)
        return {};
    SnowChangedViewportList changed = nullptr;
    uint8_t* json = nullptr;
    size_t size = 0;
    const auto result = snow_runtime_apply_annotation_json(
        m_impl->handle(), reinterpret_cast<const uint8_t*>(payload.constData()),
        static_cast<size_t>(payload.size()), &json, &size, &changed);
    const QByteArray response(result == SNOW_OK ? reinterpret_cast<const char*>(json) : nullptr,
                              result == SNOW_OK ? static_cast<qsizetype>(size) : 0);
    snow_annotation_result_destroy(json, size);
    if (result == SNOW_OK)
        m_impl->syncChangedViewports(changed);
    snow_changed_viewports_destroy(changed);
    return response;
}

bool SnowCanvasRuntime::undo() {
    if (!isOwnerThread() || !canUndo())
        return false;
    SnowChangedViewportList changed = nullptr;
    const bool ok = snow_runtime_undo_ex(m_impl->handle(), &changed) == SNOW_OK;
    if (ok)
        m_impl->syncChangedViewports(changed);
    snow_changed_viewports_destroy(changed);
    return ok;
}
bool SnowCanvasRuntime::redo() {
    if (!isOwnerThread() || !canRedo())
        return false;
    SnowChangedViewportList changed = nullptr;
    const bool ok = snow_runtime_redo_ex(m_impl->handle(), &changed) == SNOW_OK;
    if (ok)
        m_impl->syncChangedViewports(changed);
    snow_changed_viewports_destroy(changed);
    return ok;
}
bool SnowCanvasRuntime::canUndo() const {
    SnowHistoryState state{};
    return isOwnerThread() && snow_runtime_get_history_state(m_impl->handle(), &state) == SNOW_OK &&
           state.can_undo != 0;
}
bool SnowCanvasRuntime::canRedo() const {
    SnowHistoryState state{};
    return isOwnerThread() && snow_runtime_get_history_state(m_impl->handle(), &state) == SNOW_OK &&
           state.can_redo != 0;
}
quint64 SnowCanvasRuntime::documentRevision() const {
    return isOwnerThread() ? snow_runtime_document_revision(m_impl->handle()) : 0;
}
void SnowCanvasRuntime::setDocumentChangedHandler(std::function<void()> handler) {
    if (isOwnerThread()) {
        m_impl->observedRevision = documentRevision();
        m_impl->documentChanged = std::move(handler);
    }
}

void SnowCanvasRuntime::destroyAsync() {
    m_impl->destroyAsync();
}

QImage SnowCanvasRuntime::renderToImage(const QRectF& virtualSelectionRect, const QSize& outputSize,
                                        const QList<CanvasExportSource>& sources) {
    return m_impl->renderToImage(virtualSelectionRect, outputSize, sources);
}

void snow_canvas_runtime::Access::registerClient(SnowCanvasRuntime& runtime, Client& client) {
    runtime.m_impl->registerClient(&client);
}

SnowRuntime snow_canvas_runtime::Access::handle(const SnowCanvasRuntime& runtime) {
    return runtime.m_impl->handle();
}

void snow_canvas_runtime::Access::unregisterClient(SnowCanvasRuntime& runtime, Client& client) {
    runtime.m_impl->unregisterClient(&client);
}

void snow_canvas_runtime::Access::syncChangedViewports(SnowCanvasRuntime& runtime,
                                                       SnowChangedViewportList changedViewports) {
    runtime.m_impl->syncChangedViewports(changedViewports);
}

void snow_canvas_runtime::Access::syncChangedViewportIds(
    SnowCanvasRuntime& runtime, const std::vector<std::uint64_t>& changedViewportIds) {
    runtime.m_impl->syncChangedViewportIds(changedViewportIds);
}

snow_canvas_smart_erase::Coordinator&
snow_canvas_runtime::Access::smartErase(SnowCanvasRuntime& runtime) {
    return runtime.m_impl->smartErase();
}
void SnowCanvasRuntime::setBaseImageSources(const QList<SnowCanvasBaseImageSource>& sources) {
    if (!isOwnerThread())
        return;
    m_impl->smartErase().setSources(this, sources);
    m_impl->smartErase().sync(m_impl->handle());
}
SnowCanvasSmartEraseSnapshot SnowCanvasRuntime::smartEraseSnapshot() const {
    return isOwnerThread() ? m_impl->smartErase().snapshot() : SnowCanvasSmartEraseSnapshot{};
}
void SnowCanvasRuntime::restoreSmartEraseSnapshot(const SnowCanvasSmartEraseSnapshot& snapshot) {
    if (isOwnerThread())
        m_impl->smartErase().restoreSnapshot(snapshot);
}
