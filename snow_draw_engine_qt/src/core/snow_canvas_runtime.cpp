#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include "snow_canvas_export.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_runtime_session.h"
#include "snow_canvas_runtime_thread_affinity.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_viewport.h"
#include "snow_canvas_type_conversions.h"

#include <memory>
#include <QJsonDocument>
#include <limits>
#include <cmath>

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

QJsonArray SnowCanvasRuntime::selectedElementIds() const {
    if (!isOwnerThread() || !isValid())
        return {};
    std::size_t size = 0;
    if (snow_runtime_serialize_selected_element_ids(m_impl->handle(), nullptr, 0, &size) !=
            SNOW_OK ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return {};
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    std::size_t written = 0;
    if (snow_runtime_serialize_selected_element_ids(m_impl->handle(),
                                                    reinterpret_cast<std::uint8_t*>(bytes.data()),
                                                    size, &written) != SNOW_OK ||
        written != size)
        return {};
    return QJsonDocument::fromJson(bytes).array();
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

struct SnowCanvasRuntimeEditor::Impl {
    SnowCanvasRuntime& runtime;
    SnowCanvasViewport viewport;
    bool succeeded = true;
    Impl(SnowCanvasRuntime& value, SnowCanvasTool tool) : runtime(value) {
        if (!runtime.isOwnerThread() ||
            !viewport.create(snow_canvas_runtime::Access::handle(runtime),
                             snow_canvas_viewport::defaultEngineConfig())) {
            succeeded = false;
            return;
        }
        if (tool != SnowCanvasTool::Select)
            mutate([tool](SnowRuntime r, SnowViewport v, SnowChangedViewportList* changed) {
                return snow_viewport_set_active_tool_ex(r, v, snow_canvas_types::toEngineTool(tool),
                                                        changed);
            });
    }
    template <class F> bool mutate(F function) {
        if (!runtime.isOwnerThread() || !viewport.isValid())
            return succeeded = false;
        ScopedChangedViewportList changed;
        const bool ok = function(viewport.runtime(), viewport.get(), changed.outParam()) == SNOW_OK;
        if (ok)
            snow_canvas_runtime::Access::syncChangedViewports(runtime, changed.get());
        succeeded = succeeded && ok;
        return ok;
    }
};
SnowCanvasRuntimeEditor::SnowCanvasRuntimeEditor(SnowCanvasRuntime& runtime, SnowCanvasTool tool)
    : m_impl(std::make_unique<Impl>(runtime, tool)) {}
SnowCanvasRuntimeEditor::~SnowCanvasRuntimeEditor() = default;
bool SnowCanvasRuntimeEditor::isValid() const {
    return m_impl->runtime.isOwnerThread() && m_impl->viewport.isValid();
}
bool SnowCanvasRuntimeEditor::setActiveTool(SnowCanvasTool tool) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_active_tool_ex(r, v, snow_canvas_types::toEngineTool(tool),
                                                changed);
    });
}
bool SnowCanvasRuntimeEditor::succeeded() const {
    return m_impl->succeeded;
}
SnowCanvasStyleToolbarState SnowCanvasRuntimeEditor::canvasStyleToolbarState() const {
    SnowStyleToolbarState state{};
    if (isValid())
        static_cast<void>(snow_viewport_get_style_toolbar_state(m_impl->viewport.runtime(),
                                                                m_impl->viewport.get(), &state));
    return snow_canvas_types::toCanvasStyleToolbarState(state);
}
SnowCanvasWatermarkConfig SnowCanvasRuntimeEditor::canvasWatermarkConfig() const {
    SnowWatermarkConfig state{};
    if (isValid())
        static_cast<void>(snow_viewport_get_watermark_config(m_impl->viewport.runtime(),
                                                             m_impl->viewport.get(), &state));
    return snow_canvas_types::toCanvasWatermarkConfig(state);
}
SnowCanvasSpotlightConfig SnowCanvasRuntimeEditor::canvasSpotlightConfig() const {
    SnowSpotlightConfig state{};
    if (isValid())
        static_cast<void>(snow_viewport_get_spotlight_config(m_impl->viewport.runtime(),
                                                             m_impl->viewport.get(), &state));
    return snow_canvas_types::toCanvasSpotlightConfig(state);
}
bool SnowCanvasRuntimeEditor::setShapeStyleFromToolbar(const SnowCanvasShapeStyle& style,
                                                       quint32 properties,
                                                       SnowCanvasShapeKind kind) {
    const auto value = snow_canvas_types::toEngineShapeStyle(style);
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_shape_style_patch_ex(
            r, v, &value, properties, snow_canvas_types::toEngineShapeKind(kind), changed);
    });
}
bool SnowCanvasRuntimeEditor::setTextStyleFromToolbar(const SnowCanvasTextStyle& style) {
    const auto value = snow_canvas_types::toEngineTextStyle(style);
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_text_style_ex(r, v, &value, nullptr, 0, changed);
    });
}
bool SnowCanvasRuntimeEditor::setSerialNumberStyleFromToolbar(
    const SnowCanvasSerialNumberStyle& style) {
    const auto value = snow_canvas_types::toEngineSerialNumberStyle(style);
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_serial_number_style_ex(r, v, &value, changed);
    });
}
bool SnowCanvasRuntimeEditor::setFilterStyleFromToolbar(const SnowCanvasFilterStyle& style,
                                                        quint32 properties) {
    const SnowFilterStyle value{static_cast<SnowFilterType>(style.type), style.strength,
                                style.opacity, style.strokeWidth};
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_filter_style_ex(r, v, &value, properties, changed);
    });
}
bool SnowCanvasRuntimeEditor::setWatermarkConfigFromToolbar(
    const SnowCanvasWatermarkConfig& style) {
    const auto value = snow_canvas_types::toEngineWatermarkConfig(style);
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_watermark_config_ex(r, v, &value, changed);
    });
}
bool SnowCanvasRuntimeEditor::setSpotlightConfigFromToolbar(
    const SnowCanvasSpotlightConfig& style) {
    const auto value = snow_canvas_types::toEngineSpotlightConfig(style);
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_spotlight_config_ex(r, v, &value, changed);
    });
}
bool SnowCanvasRuntimeEditor::select(quint32 index, quint32 generation) {
    const SnowElementId id{index, generation};
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_select_element_ex(r, v, id, changed);
    });
}
bool SnowCanvasRuntimeEditor::deleteSelected() {
    return m_impl->mutate([](auto r, auto v, auto changed) {
        return snow_viewport_delete_selected_ex(r, v, changed);
    });
}
bool SnowCanvasRuntimeEditor::deleteAllElements() {
    return m_impl->mutate([](auto r, auto v, auto changed) {
        return snow_viewport_delete_all_elements_ex(r, v, changed);
    });
}
bool SnowCanvasRuntimeEditor::erasePath(const QList<QPointF>& points) {
    if (!isValid() || points.isEmpty() || points.size() > 8192)
        return false;
    for (const auto& point : points)
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            return false;
    const auto runtime = m_impl->viewport.runtime();
    const auto viewport = m_impl->viewport.get();
    SnowActiveTool previous{};
    if (snow_viewport_get_active_tool(runtime, viewport, &previous) != SNOW_OK ||
        snow_viewport_set_surface_size(runtime, viewport, 2, 2) != SNOW_OK ||
        snow_viewport_set_camera(runtime, viewport, 1, 1, 1) != SNOW_OK)
        return false;
    const auto setTool = [&](SnowActiveTool tool) {
        return m_impl->mutate([&](auto r, auto v, auto changed) {
            return snow_viewport_set_active_tool_ex(r, v, tool, changed);
        });
    };
    if (!setTool(snow_canvas_types::toEngineTool(SnowCanvasTool::Eraser)))
        return false;
    const auto eventAt = [](QPointF point, SnowPointerEventType type) {
        SnowInputEvent event{};
        event.kind = SNOW_INPUT_EVENT_POINTER;
        event.pointer.pointer_id = 1;
        event.pointer.event_type = type;
        event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
        event.pointer.position_x = point.x();
        event.pointer.position_y = point.y();
        event.pointer.button = SNOW_POINTER_BUTTON_PRIMARY;
        event.pointer.buttons = type == SNOW_POINTER_EVENT_UP ? 0 : 1;
        return event;
    };
    const auto send = [&](const SnowInputEvent& event) {
        SnowInteractionOutput output{};
        return m_impl->mutate([&](auto r, auto v, auto changed) {
            return snow_viewport_process_input_ex(r, v, &event, &output, changed);
        });
    };
    bool ok = send(eventAt(points.first(), SNOW_POINTER_EVENT_DOWN));
    if (ok && points.size() > 1) {
        std::vector<SnowInputEvent> moves;
        moves.reserve(static_cast<size_t>(points.size() - 1));
        for (qsizetype index = 1; index < points.size(); ++index)
            moves.push_back(eventAt(points[index], SNOW_POINTER_EVENT_MOVE));
        SnowInteractionOutput output{};
        ok = m_impl->mutate([&](auto r, auto v, auto changed) {
            return snow_viewport_process_pointer_move_batch_ex(
                r, v, moves.data(), static_cast<uint32_t>(moves.size()), &output, changed);
        });
    }
    if (ok)
        ok = send(eventAt(points.last(), SNOW_POINTER_EVENT_UP));
    // Tool changes discard uncommitted eraser previews if an input failed.
    const bool restored = setTool(previous);
    return ok && restored;
}
bool SnowCanvasRuntimeEditor::duplicateSelected(QPointF offset) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_duplicate_selected_ex(r, v, offset.x(), offset.y(), changed);
    });
}
bool SnowCanvasRuntimeEditor::reorderSelected(SnowCanvasSelectionOrder order) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_reorder_selected_ex(r, v, static_cast<uint32_t>(order), changed);
    });
}
bool SnowCanvasRuntimeEditor::alignSelected(SnowCanvasSelectionAlignment alignment) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_align_selected_ex(r, v, static_cast<uint32_t>(alignment), changed);
    });
}
bool SnowCanvasRuntimeEditor::setSelectedOpacity(double opacity) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_selected_opacity_ex(r, v, opacity, changed);
    });
}
bool SnowCanvasRuntimeEditor::adjustSelectedSerialNumbers(qint64 delta) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_adjust_selected_serial_numbers_ex(r, v, delta, changed);
    });
}
bool SnowCanvasRuntimeEditor::createSerialNumberText() {
    SnowElementId id{};
    uint8_t hasId = 0;
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_create_serial_number_text_ex(r, v, 0, 0, &id, &hasId, changed);
    }) && hasId != 0;
}
bool SnowCanvasRuntimeEditor::insertDrawTemplate(const QByteArray& payload, QPointF center) {
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_insert_draw_template_ex(
            r, v, reinterpret_cast<const uint8_t*>(payload.constData()),
            static_cast<size_t>(payload.size()), center.x(), center.y(), changed);
    });
}
bool SnowCanvasRuntimeEditor::setAutoFilterRegions(const SnowCanvasAutoFilterRecord& record) {
    const auto boundsOf = [](QRectF value) {
        return SnowAutoFilterBounds{value.left(), value.top(), value.right(), value.bottom()};
    };
    const auto bounds = boundsOf(record.sourceBounds);
    std::vector<SnowAutoFilterRegion> regions;
    for (const auto& region : record.regions) {
        const auto category = region.category.toUtf8();
        if (category.size() > 127)
            return false;
        SnowAutoFilterRegion value{};
        value.id = region.id;
        value.bounds = boundsOf(region.bounds);
        std::memcpy(value.category, category.constData(), static_cast<size_t>(category.size()));
        regions.push_back(value);
    }
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_set_auto_filter_regions(r, v, &bounds, regions.data(), regions.size(),
                                                     changed);
    });
}
bool SnowCanvasRuntimeEditor::fillAutoFilterCategory(const QString& category) {
    const auto bytes = category.toUtf8();
    return m_impl->mutate([&](auto r, auto v, auto changed) {
        return snow_viewport_fill_auto_filter_category(
            r, v, reinterpret_cast<const uint8_t*>(bytes.constData()),
            static_cast<size_t>(bytes.size()), changed);
    });
}
