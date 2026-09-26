#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/app/mcp/mcpjobregistry.h"
#include "snow_shot/app/mcp/screenshotmcpselection.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "snow_shot/app/mcp/mcpstylepatch.h"
#include "snow_shot/app/mcp/mcpimageexportoptions.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snowimageqtcodec.h"
#include <QCryptographicHash>
#include <QApplication>
#include <QClipboard>
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QSaveFile>
#include <QDateTime>
#include <QImageReader>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QMutex>
#include <QScopeGuard>
#include <QUuid>
#include <QtMath>
#include <atomic>
#include <array>
#include <cmath>
#include <utility>

namespace snow_shot::app::mcp {
namespace {
constexpr qint64 kMaximumPixels = 64000000;
constexpr qint64 kMaximumSourceBytes = 512LL * 1024 * 1024;
constexpr qint64 kMaximumCacheBytes = 64LL * 1024 * 1024;
const QStringList kTools{QStringLiteral("snow_shot_document_open"),
                         QStringLiteral("snow_shot_document_list"),
                         QStringLiteral("snow_shot_document_state"),
                         QStringLiteral("snow_shot_document_clone"),
                         QStringLiteral("snow_shot_document_apply_annotations"),
                         QStringLiteral("snow_shot_document_set_selection"),
                         QStringLiteral("snow_shot_document_undo"),
                         QStringLiteral("snow_shot_document_redo"),
                         QStringLiteral("snow_shot_document_render"),
                         QStringLiteral("snow_shot_document_save"),
                         QStringLiteral("snow_shot_document_copy"),
                         QStringLiteral("snow_shot_document_pin"),
                         QStringLiteral("snow_shot_document_set_selection_style"),
                         QStringLiteral("snow_shot_document_close"),
                         QStringLiteral("snow_shot_document_recognize"),
                         QStringLiteral("snow_shot_document_present"),
                         QStringLiteral("snow_shot_document_set_tool"),
                         QStringLiteral("snow_shot_document_set_tool_style"),
                         QStringLiteral("snow_shot_document_edit_elements"),
                         QStringLiteral("snow_shot_document_draw_template"),
                         QStringLiteral("snow_shot_document_sample_color"),
                         QStringLiteral("snow_shot_document_recapture"),
                         QStringLiteral("snow_shot_document_recognition_state"),
                         QStringLiteral("snow_shot_document_edit_recognition"),
                         QStringLiteral("snow_shot_document_export_recognition"),
                         QStringLiteral("snow_shot_document_original_content"),
                         QStringLiteral("snow_shot_document_auto_filter"),
                         QStringLiteral("snow_shot_job_get"),
                         QStringLiteral("snow_shot_job_list"),
                         QStringLiteral("snow_shot_job_cancel"),
                         QStringLiteral("snow_shot_artifact_list"),
                         QStringLiteral("snow_shot_artifact_read"),
                         QStringLiteral("snow_shot_artifact_release")};
QString identifier() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
bool readOnly(const QString& method) {
    return method.endsWith(QStringLiteral("_list")) || method.endsWith(QStringLiteral("_state")) ||
           method.endsWith(QStringLiteral("_render")) ||
           method == QStringLiteral("snow_shot_artifact_read") ||
           method == QStringLiteral("snow_shot_job_get") ||
           method == QStringLiteral("snow_shot_job_cancel");
}
ScreenshotMcpResponse failure(const ScreenshotMcpRequest& request, const QString& code,
                              const QString& field = {}) {
    ScreenshotMcpResponse response;
    response.requestId = request.requestId;
    response.errorCode = code;
    response.errorMessage =
        QCoreApplication::translate("McpDocumentService", "Document request failed (%1).")
            .arg(code);
    if (!field.isEmpty())
        response.errorDetails.insert(QStringLiteral("field"), field);
    return response;
}
ScreenshotMcpResponse success(const ScreenshotMcpRequest& request, QJsonObject value) {
    ScreenshotMcpResponse response;
    response.requestId = request.requestId;
    response.ok = true;
    if (value.contains(QStringLiteral("revision")))
        response.revision =
            static_cast<quint64>(value.value(QStringLiteral("revision")).toInteger());
    response.result = std::move(value);
    return response;
}
QJsonArray rectangle(const QRectF& rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}
bool validSize(QSize size) {
    return size.width() > 0 && size.height() > 0 &&
           static_cast<qint64>(size.width()) * size.height() <= kMaximumPixels;
}
const QHash<QString, SnowCanvasTool> kCanvasTools{
    {QStringLiteral("select"), SnowCanvasTool::Select},
    {QStringLiteral("rectangle"), SnowCanvasTool::Shape},
    {QStringLiteral("arrow"), SnowCanvasTool::Arrow},
    {QStringLiteral("line"), SnowCanvasTool::Line},
    {QStringLiteral("freehand"), SnowCanvasTool::FreeDraw},
    {QStringLiteral("rectangle_highlight"), SnowCanvasTool::RectangleHighlight},
    {QStringLiteral("pen_highlight"), SnowCanvasTool::PenHighlight},
    {QStringLiteral("eraser"), SnowCanvasTool::Eraser},
    {QStringLiteral("rectangle_filter"), SnowCanvasTool::RectangleFilter},
    {QStringLiteral("pen_filter"), SnowCanvasTool::PenFilter},
    {QStringLiteral("text"), SnowCanvasTool::Text},
    {QStringLiteral("serial_number"), SnowCanvasTool::SerialNumber},
    {QStringLiteral("watermark"), SnowCanvasTool::Watermark},
    {QStringLiteral("spotlight"), SnowCanvasTool::Spotlight},
    {QStringLiteral("auto_filter"), SnowCanvasTool::AutoFilter}};
qint64 sourceAuxiliaryBytes(const McpDocumentService::Source& source) {
    qint64 bytes = 2LL * (source.originalContent.text.size() + source.originalContent.html.size() +
                          source.originalContent.localFilePath.size()) +
                   2LL * QJsonDocument(source.metadata).toJson(QJsonDocument::Compact).size();
    const auto textBytes = [](const std::shared_ptr<ScreenshotOcrPresentation>& text) {
        qint64 result = 0;
        if (text)
            for (const auto& line : text->lines)
                result += 4096 + 8LL * line.text.size() + 64LL * line.sourceLineQuads.size();
        return result;
    };
    if (source.recognitionResults.text)
        bytes += textBytes(source.recognitionResults.text->presentation);
    bytes += textBytes(source.recognitionResults.translatedText);
    if (source.recognitionResults.table)
        bytes += 2LL * source.recognitionResults.table->html.size();
    if (source.recognitionResults.qr)
        for (const auto& value : source.recognitionResults.qr->contents)
            bytes += 2LL * value.size();
    for (const auto& conversion : source.recognitionResults.conversions)
        bytes += 2LL * (conversion.source.size() + conversion.model.size() +
                        conversion.modelFingerprint.size());
    return bytes;
}
struct Document {
    quint64 owner = 0;
    QString id;
    quint64 revision = 1;
    QList<CanvasExportSource> sources;
    QRectF canvasBounds;
    qint64 sourceBytes = 0;
    qint64 historyCharge = 0;
    QJsonObject metadata;
    ScreenshotClipboardOriginalContent originalContent;
    ScreenshotRecognitionResults recognitionResults;
    QString tool = QStringLiteral("select");
    ScreenshotSelectionModel selection;
    SnowCanvasRuntime runtime{
        SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()}};
    QImage rendered;
    qreal renderedScale = 0;
    QJsonObject state() const {
        return {{QStringLiteral("document_id"), id},
                {QStringLiteral("revision"), static_cast<qint64>(revision)},
                {QStringLiteral("canvas_bounds"), rectangle(canvasBounds)},
                {QStringLiteral("selection"), selection.selectionRegion().toJson()},
                {QStringLiteral("selection_bounds"), rectangle(selection.normalizedSelection())},
                {QStringLiteral("can_undo"), runtime.canUndo()},
                {QStringLiteral("can_redo"), runtime.canRedo()},
                {QStringLiteral("selected_element_ids"), runtime.selectedElementIds()},
                {QStringLiteral("coordinate_space"), QStringLiteral("canvas_half_open")},
                {QStringLiteral("presentation"), QStringLiteral("background")},
                {QStringLiteral("tool"), tool},
                {QStringLiteral("has_original_content"), !originalContent.isEmpty()},
                {QStringLiteral("source"), metadata}};
    }
};
struct WorkerResult {
    ScreenshotMcpResponse response;
    QImage image;
    std::optional<McpDocumentService::Source> source;
};
// Admission is shared by all lanes. Only short accounting operations hold the lock;
// decoding, document mutation, and rendering always run without it.
struct DocumentAdmission {
    QMutex mutex;
    int count = 0;
    QHash<quint64, int> owners;
    qint64 sources = 0;
    qint64 history = 0;
    qint64 cache = 0;
    bool create(quint64 owner, qint64 bytes, qint64 charge) {
        QMutexLocker lock(&mutex);
        if (count >= 16 || owners.value(owner) >= 4 || sources + bytes > kMaximumSourceBytes ||
            charge > 64 * 1024 * 1024 || history + charge > 256LL * 1024 * 1024)
            return false;
        ++count;
        ++owners[owner];
        sources += bytes;
        history += charge;
        return true;
    }
    void remove(quint64 owner, qint64 bytes, qint64 charge) {
        QMutexLocker lock(&mutex);
        --count;
        if (--owners[owner] == 0)
            owners.remove(owner);
        sources -= bytes;
        history -= charge;
    }
    bool replace(qint64 previous, qint64 bytes) {
        QMutexLocker lock(&mutex);
        if (sources - previous + bytes > kMaximumSourceBytes)
            return false;
        sources += bytes - previous;
        return true;
    }
    bool reserveHistory(qint64 charge) {
        QMutexLocker lock(&mutex);
        if (history + charge > 256LL * 1024 * 1024)
            return false;
        history += charge;
        return true;
    }
    void releaseHistory(qint64 charge) {
        QMutexLocker lock(&mutex);
        history -= charge;
    }
    bool reserveCache(qint64 bytes) {
        QMutexLocker lock(&mutex);
        if (cache + bytes > kMaximumCacheBytes)
            return false;
        cache += bytes;
        return true;
    }
    void releaseCache(qint64 bytes) {
        QMutexLocker lock(&mutex);
        cache -= bytes;
    }
};
struct DocumentReservation {
    QMutex mutex;
    std::shared_ptr<DocumentAdmission> admission;
    quint64 owner = 0;
    qint64 bytes = 0;
    qint64 history = 0;
    bool ownsSlot = true;
    bool consumed = false;
    ~DocumentReservation() {
        if (!consumed) {
            if (ownsSlot)
                admission->remove(owner, bytes, history);
            else
                static_cast<void>(admission->replace(bytes, 0));
        }
    }
};
// Each runtime remains in one bounded lane for its complete lifetime.
class DocumentWorker final : public QObject {
  public:
    explicit DocumentWorker(std::shared_ptr<DocumentAdmission> limits,
                            std::function<void(const ScreenshotMcpRequest&)> decodeHook,
                            std::function<void(const QString&, const QString&)> observed)
        : admission(std::move(limits)), beforeDecode(std::move(decodeHook)),
          workObserved(std::move(observed)) {}
    std::shared_ptr<DocumentAdmission> admission;
    std::function<void(const ScreenshotMcpRequest&)> beforeDecode;
    std::function<void(const QString&, const QString&)> workObserved;
    QHash<QString, std::shared_ptr<Document>> documents;
    WorkerResult create(const ScreenshotMcpRequest& request, McpDocumentService::Source source,
                        std::shared_ptr<DocumentReservation> reservation = {}) {
        QMutexLocker reservationLock(reservation ? &reservation->mutex : nullptr);
        if (source.images.isEmpty() && !source.image.isNull()) {
            if (source.canvasBounds.isEmpty())
                source.canvasBounds = QRectF(QPointF(), source.image.size());
            source.images.append({std::move(source.image), source.canvasBounds});
        }
        qint64 bytes = sourceAuxiliaryBytes(source);
        for (const auto& image : source.images) {
            if (image.image.isNull() || !validSize(image.image.size()) ||
                image.canvasRect.isEmpty() || !source.canvasBounds.contains(image.canvasRect))
                return {failure(request, QStringLiteral("invalid_source")), {}};
            bytes += image.image.sizeInBytes();
        }
        const qint64 inputCharge =
            4 * (source.documentSession.size() + source.documentHistory.size());
        if (source.images.isEmpty() || !validSize(source.canvasBounds.size().toSize()))
            return {failure(request, QStringLiteral("invalid_source")), {}};
        if (reservation) {
            if (!admission->replace(reservation->bytes, bytes))
                return {failure(request, QStringLiteral("resource_limit")), {}};
            reservation->bytes = bytes;
            if (inputCharge > 64 * 1024 * 1024 || !admission->reserveHistory(inputCharge))
                return {failure(request, QStringLiteral("resource_limit")), {}};
            reservation->history = inputCharge;
        } else {
            if (!admission->create(request.connectionId, bytes, inputCharge))
                return {failure(request, QStringLiteral("resource_limit")), {}};
            reservation = std::make_shared<DocumentReservation>();
            reservation->admission = admission;
            reservation->owner = request.connectionId;
            reservation->bytes = bytes;
            reservation->history = inputCharge;
        }
        auto document = std::make_shared<Document>();
        if (!document->runtime.isValid())
            return {failure(request, QStringLiteral("unavailable")), {}};
        document->owner = request.connectionId;
        document->id = identifier();
        document->sources = std::move(source.images);
        document->canvasBounds = source.canvasBounds;
        document->sourceBytes = bytes;
        document->historyCharge = inputCharge;
        document->metadata = std::move(source.metadata);
        document->originalContent = std::move(source.originalContent);
        document->recognitionResults = std::move(source.recognitionResults);
        document->tool = std::move(source.tool);
        document->selection.setSelectionRect(document->canvasBounds);
        if ((!source.documentSession.isEmpty() &&
             !document->runtime.restoreDocumentSession(source.documentSession)) ||
            (!source.documentHistory.isEmpty() &&
             !document->runtime.restoreDocumentHistory(source.documentHistory)) ||
            (source.selection && !document->selection.applyParams(
                                     *source.selection, document->canvasBounds.toAlignedRect())))
            return {failure(request, QStringLiteral("invalid_source")), {}};
        documents.insert(document->id, document);
        reservation->consumed = true;
        return {success(request, document->state()), {}};
    }
    void invalidate(Document& document) {
        admission->releaseCache(document.rendered.sizeInBytes());
        document.rendered = {};
        document.renderedScale = 0;
    }
    void disconnected(quint64 owner) {
        for (auto it = documents.begin(); it != documents.end();) {
            if ((*it)->owner != owner) {
                ++it;
                continue;
            }
            admission->remove((*it)->owner, (*it)->sourceBytes, (*it)->historyCharge);
            invalidate(**it);
            it = documents.erase(it);
        }
    }
    void discard(quint64 owner, const QString& id) {
        const auto found = documents.find(id);
        if (found == documents.end() || (*found)->owner != owner)
            return;
        admission->remove((*found)->owner, (*found)->sourceBytes, (*found)->historyCharge);
        invalidate(**found);
        documents.erase(found);
    }
    WorkerResult replace(const ScreenshotMcpRequest& request, McpDocumentService::Source source,
                         std::shared_ptr<DocumentReservation> reservation = {}) {
        QMutexLocker reservationLock(reservation ? &reservation->mutex : nullptr);
        const auto found =
            documents.find(request.params.value(QStringLiteral("document_id")).toString());
        if (found == documents.end() || (*found)->owner != request.connectionId)
            return {failure(request, QStringLiteral("document_not_found")), {}};
        auto& document = **found;
        if (!request.expectedRevision || *request.expectedRevision != document.revision)
            return {failure(request, QStringLiteral("stale_revision")), {}};
        if (source.images.isEmpty() && !source.image.isNull()) {
            if (source.canvasBounds.isEmpty())
                source.canvasBounds = QRectF(QPointF(), source.image.size());
            source.images.append({std::move(source.image), source.canvasBounds});
        }
        source.recognitionResults = {};
        qint64 bytes = sourceAuxiliaryBytes(source);
        for (const auto& image : source.images) {
            if (image.image.isNull() || !validSize(image.image.size()) ||
                image.canvasRect.isEmpty() || !source.canvasBounds.contains(image.canvasRect))
                return {failure(request, QStringLiteral("invalid_source")), {}};
            bytes += image.image.sizeInBytes();
        }
        if (source.images.isEmpty() || !validSize(source.canvasBounds.size().toSize()))
            return {failure(request, QStringLiteral("resource_limit")), {}};
        if (source.canvasBounds != document.canvasBounds)
            return {failure(request, QStringLiteral("source_bounds_changed")), {}};
        if (!admission->replace(document.sourceBytes + (reservation ? reservation->bytes : 0),
                                bytes))
            return {failure(request, QStringLiteral("resource_limit")), {}};
        if (reservation)
            reservation->consumed = true;
        document.sourceBytes = bytes;
        document.sources = std::move(source.images);
        document.metadata = std::move(source.metadata);
        document.originalContent = std::move(source.originalContent);
        document.recognitionResults = {};
        ++document.revision;
        invalidate(document);
        return {success(request, document.state()), {}};
    }
    WorkerResult request(const ScreenshotMcpRequest& request,
                         std::shared_ptr<std::atomic_bool> canceled = {}) {
        const auto document =
            documents.value(request.params.value(QStringLiteral("document_id")).toString());
        if (!document || document->owner != request.connectionId)
            return execute(request, canceled);
        const auto& method = request.method;
        if (!request.expectedRevision || *request.expectedRevision != document->revision)
            return execute(request, canceled);
        qint64 charge = 0;
        const bool clone = method == QStringLiteral("snow_shot_document_clone");
        if (clone)
            charge = document->historyCharge;
        else if (method == QStringLiteral("snow_shot_document_apply_annotations") ||
                 method == QStringLiteral("snow_shot_document_set_tool_style") ||
                 method == QStringLiteral("snow_shot_document_edit_elements") ||
                 (method == QStringLiteral("snow_shot_document_draw_template") &&
                  request.params.value(QStringLiteral("action")) == QStringLiteral("insert"))) {
            // Conservative retained-history admission: charge payload and affected elements,
            // including edits with very small inputs that affect a large existing selection.
            charge =
                8 * QJsonDocument(request.params).toJson(QJsonDocument::Compact).size() +
                4096LL * (document->runtime.selectedElementIds().size() +
                          request.params.value(QStringLiteral("operations")).toArray().size() + 1);
        }
        if ((!clone && document->historyCharge + charge > 64 * 1024 * 1024) ||
            !admission->reserveHistory(charge))
            return {failure(request, QStringLiteral("resource_limit")), {}};
        auto releaseCharge = qScopeGuard([&] { admission->releaseHistory(charge); });
        const auto before = document->revision;
        auto result = execute(request, canceled);
        if (!result.response.ok)
            return result;
        if (clone) {
            const auto copy = documents.value(
                result.response.result.value(QStringLiteral("document_id")).toString());
            copy->historyCharge = charge;
            releaseCharge.dismiss();
        } else if (document->revision != before) {
            document->historyCharge += charge;
            releaseCharge.dismiss();
        }
        return result;
    }
    WorkerResult execute(const ScreenshotMcpRequest& request,
                         const std::shared_ptr<std::atomic_bool>& canceled) {
        const auto& params = request.params;
        const auto& method = request.method;
        if (method == QStringLiteral("snow_shot_document_list")) {
            QJsonArray list;
            for (const auto& document : std::as_const(documents))
                if (document->owner == request.connectionId)
                    list.append(document->state());
            return {success(request, {{QStringLiteral("documents"), list}}), {}};
        }
        if (method == QStringLiteral("snow_shot_document_open")) {
            const QString path = params.value(QStringLiteral("path")).toString();
            if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile() ||
                path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//")))
                return {
                    failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("path")),
                    {}};
            constexpr qint64 maximumEncodedBytes = 128LL * 1024 * 1024;
            if (QFileInfo(path).size() > maximumEncodedBytes ||
                !admission->create(request.connectionId, 0, 0))
                return {failure(request, QStringLiteral("resource_limit")), {}};
            auto reservation = std::make_shared<DocumentReservation>();
            reservation->admission = admission;
            reservation->owner = request.connectionId;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return {failure(request, QStringLiteral("invalid_source")), {}};
            const auto encodedSize = file.size();
            if (encodedSize < 0 || encodedSize > maximumEncodedBytes ||
                !admission->replace(0, encodedSize + 1))
                return {failure(request, QStringLiteral("resource_limit")), {}};
            reservation->bytes = encodedSize + 1;
            auto encoded = file.read(encodedSize + 1);
            if (encoded.size() != encodedSize)
                return {failure(request, QStringLiteral("invalid_source")), {}};
            McpDocumentService::Source source;
            {
            QBuffer buffer(&encoded);
            if (!buffer.open(QIODevice::ReadOnly))
                return {failure(request, QStringLiteral("invalid_source")), {}};
            QImageReader reader(&buffer);
            reader.setAutoTransform(true);
            auto size = reader.size();
            const bool native = !size.isValid();
            auto nativeFormat = snow::image::Format::unknown;
            if (native) {
                const auto format = ScreenshotImageFileService::formatForPath(path);
                if (format && *format != ScreenshotImageFileFormat::Pdf)
                    nativeFormat = ScreenshotImageFileService::snowImageFormat(*format);
                size = snow_shot::image_codec::inspectSize(encoded, nativeFormat);
            }
            if (!validSize(size))
                return {failure(request, QStringLiteral("invalid_source")), {}};
            // Qt may decode to 64-bit pixels; the native decoder may temporarily retain an
            // RGBA buffer plus its QImage copy. Reserve their conservative peak before decoding.
            const qint64 decodedBytes = static_cast<qint64>(size.width()) * size.height() * 8;
            if (!admission->replace(reservation->bytes, encoded.size() + decodedBytes))
                return {failure(request, QStringLiteral("resource_limit")), {}};
            reservation->bytes = encoded.size() + decodedBytes;
            if (beforeDecode)
                beforeDecode(request);
            if (canceled && canceled->load())
                return {failure(request, QStringLiteral("canceled")), {}};
            source.image = native ? snow_shot::image_codec::decode(encoded, nativeFormat, nullptr)
                                  : reader.read();
            }
            encoded.clear();
            source.metadata = {{QStringLiteral("kind"), QStringLiteral("file")},
                               {QStringLiteral("path"), QFileInfo(path).canonicalFilePath()}};
            return create(request, std::move(source), std::move(reservation));
        }
        const QString id = params.value(QStringLiteral("document_id")).toString();
        const auto found = documents.find(id);
        if (found == documents.end() || (*found)->owner != request.connectionId)
            return {failure(request, QStringLiteral("document_not_found")), {}};
        auto& document = **found;
        if (method == QStringLiteral("snow_shot_document_state"))
            return {success(request, document.state()), {}};
        if (!request.expectedRevision)
            return {failure(request, QStringLiteral("revision_required")), {}};
        if (*request.expectedRevision != document.revision) {
            auto response = failure(request, QStringLiteral("stale_revision"));
            response.errorDetails.insert(QStringLiteral("state"), document.state());
            return {response, {}};
        }
        if (method == QStringLiteral("snow_shot_document_present") ||
            method == QStringLiteral("snow_shot_document_pin")) {
            McpDocumentService::Source source;
            source.images = document.sources;
            source.canvasBounds = document.canvasBounds;
            source.documentSession = document.runtime.serializeDocumentSession();
            source.documentHistory = document.runtime.serializeDocumentHistory();
            source.selection = document.selection.params(document.canvasBounds.toAlignedRect());
            source.originalContent = document.originalContent;
            source.recognitionResults = document.recognitionResults;
            source.tool = document.tool;
            if (method == QStringLiteral("snow_shot_document_pin")) {
                SnowCanvasRuntime base;
                const auto selection = document.selection.pixelSelection();
                if (workObserved)
                    workObserved(document.id, QStringLiteral("render"));
                auto background = base.renderToImage(selection, selection.size(), document.sources);
                if (background.isNull())
                    return {failure(request, QStringLiteral("output_failed")), {}};
                return {success(request, document.state()), std::move(background),
                        std::move(source)};
            }
            return {success(request, document.state()), {}, std::move(source)};
        }
        if (method == QStringLiteral("snow_shot_document_recognition_state") ||
            method == QStringLiteral("snow_shot_document_edit_recognition") ||
            method == QStringLiteral("snow_shot_document_export_recognition"))
            return {success(request, document.state()), {}};
        if (method == QStringLiteral("snow_shot_document_original_content")) {
            if (document.originalContent.isEmpty())
                return {failure(request, QStringLiteral("original_content_unavailable")), {}};
            auto value = document.state();
            value.insert(QStringLiteral("text"), document.originalContent.text);
            value.insert(QStringLiteral("html"), document.originalContent.html);
            value.insert(QStringLiteral("local_file_path"), document.originalContent.localFilePath);
            return {success(request, std::move(value)), {}};
        }
        if (method == QStringLiteral("snow_shot_document_set_tool")) {
            const auto tool = params.value(QStringLiteral("tool")).toString();
            if (!kCanvasTools.contains(tool))
                return {
                    failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("tool")),
                    {}};
            if (tool != document.tool) {
                SnowCanvasRuntimeEditor editor(document.runtime);
                if (!editor.isValid() || !editor.setActiveTool(kCanvasTools.value(tool)))
                    return {failure(request, QStringLiteral("action_unavailable")), {}};
                document.tool = tool;
                ++document.revision;
            }
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_set_tool_style")) {
            const auto target = params.value(QStringLiteral("target")).toString();
            if (!kCanvasTools.contains(target))
                return {failure(request, QStringLiteral("invalid_parameters"),
                                QStringLiteral("target")),
                        {}};
            // Keep a recovery snapshot so validation/engine rejection cannot partially change
            // defaults.
            const auto session = document.runtime.serializeDocumentSession();
            const auto history = document.runtime.serializeDocumentHistory();
            bool ok;
            {
                SnowCanvasRuntimeEditor editor(document.runtime, kCanvasTools.value(target));
                ok =
                    editor.isValid() && mcpStylePatch(editor, editor, params) && editor.succeeded();
                if (ok)
                    ok = editor.setActiveTool(kCanvasTools.value(document.tool));
            }
            if (!ok) {
                static_cast<void>(document.runtime.restoreDocumentSession(session));
                static_cast<void>(document.runtime.restoreDocumentHistory(history));
                return {
                    failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("style")),
                    {}};
            }
            ++document.revision;
            invalidate(document);
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_draw_template")) {
            if (params.value(QStringLiteral("action")) == QStringLiteral("export")) {
                const auto payload = document.runtime.serializeSelectedDrawTemplate();
                if (payload.isEmpty())
                    return {failure(request, QStringLiteral("action_unavailable")), {}};
                auto value = document.state();
                value.insert(QStringLiteral("payload"), QString::fromUtf8(payload));
                return {success(request, std::move(value)), {}};
            }
            const auto payload = params.value(QStringLiteral("payload")).toString().toUtf8();
            if (params.value(QStringLiteral("action")) != QStringLiteral("insert") ||
                payload.isEmpty() || payload.size() > 1024 * 1024)
                return {failure(request, QStringLiteral("invalid_parameters")), {}};
            SnowCanvasRuntimeEditor editor(document.runtime);
            if (!editor.insertDrawTemplate(payload,
                                           document.selection.normalizedSelection().center()))
                return {failure(request, QStringLiteral("invalid_parameters")), {}};
            ++document.revision;
            invalidate(document);
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_edit_elements")) {
            SnowCanvasRuntimeEditor editor(document.runtime);
            const auto action = params.value(QStringLiteral("action")).toString();
            bool ok = false;
            if (action == QStringLiteral("select")) {
                const auto elementId = params.value(QStringLiteral("id")).toObject();
                const auto index = elementId.value(QStringLiteral("index")).toInteger(-1);
                const auto generation = elementId.value(QStringLiteral("generation")).toInteger(-1);
                ok = index >= 0 && generation >= 0 && index <= UINT32_MAX &&
                     generation <= UINT32_MAX &&
                     editor.select(static_cast<quint32>(index), static_cast<quint32>(generation));
            } else if (action == QStringLiteral("delete"))
                ok = editor.deleteSelected();
            else if (action == QStringLiteral("erase_path")) {
                const auto values = params.value(QStringLiteral("points")).toArray();
                QList<QPointF> points;
                if (values.isEmpty() || values.size() > 8192)
                    return {failure(request, QStringLiteral("invalid_parameters")), {}};
                for (const auto& value : values) {
                    const auto pair = value.toArray();
                    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble() ||
                        !std::isfinite(pair[0].toDouble()) || !std::isfinite(pair[1].toDouble()))
                        return {failure(request, QStringLiteral("invalid_parameters")), {}};
                    const QPointF point(pair[0].toDouble(), pair[1].toDouble());
                    if (!document.canvasBounds.contains(point))
                        return {failure(request, QStringLiteral("invalid_parameters")), {}};
                    points.append(point);
                }
                ok = editor.erasePath(points);
            } else if (action == QStringLiteral("delete_all"))
                ok = editor.deleteAllElements();
            else if (action == QStringLiteral("duplicate"))
                ok = editor.duplicateSelected();
            else if (action == QStringLiteral("create_serial_text"))
                ok = editor.createSerialNumberText();
            else if (action == QStringLiteral("adjust_serial_numbers"))
                ok = params.value(QStringLiteral("delta")).isDouble() &&
                     editor.adjustSelectedSerialNumbers(
                         params.value(QStringLiteral("delta")).toInteger());
            else if (action == QStringLiteral("opacity")) {
                const auto opacity = params.value(QStringLiteral("opacity")).toDouble(-1);
                ok = std::isfinite(opacity) && opacity >= 0 && opacity <= 1 &&
                     editor.setSelectedOpacity(opacity);
            } else if (action == QStringLiteral("order")) {
                const QStringList values{
                    QStringLiteral("send_to_back"), QStringLiteral("send_backward"),
                    QStringLiteral("bring_forward"), QStringLiteral("bring_to_front")};
                const int index = values.indexOf(params.value(QStringLiteral("order")).toString());
                ok = index >= 0 &&
                     editor.reorderSelected(static_cast<SnowCanvasSelectionOrder>(index));
            } else if (action == QStringLiteral("align")) {
                const QStringList values{QStringLiteral("left"),
                                         QStringLiteral("center_horizontally"),
                                         QStringLiteral("right"),
                                         QStringLiteral("top"),
                                         QStringLiteral("center_vertically"),
                                         QStringLiteral("bottom"),
                                         QStringLiteral("distribute_horizontally"),
                                         QStringLiteral("distribute_vertically")};
                const int index =
                    values.indexOf(params.value(QStringLiteral("alignment")).toString());
                ok = index >= 0 &&
                     editor.alignSelected(static_cast<SnowCanvasSelectionAlignment>(index));
            }
            if (!ok)
                return {failure(request, QStringLiteral("action_unavailable")), {}};
            ++document.revision;
            invalidate(document);
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_sample_color")) {
            const auto point = params.value(QStringLiteral("point")).toArray();
            if (point.size() != 2 || !point[0].isDouble() || !point[1].isDouble() ||
                !std::isfinite(point[0].toDouble()) || !std::isfinite(point[1].toDouble()))
                return {
                    failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("point")),
                    {}};
            const QPointF position(std::floor(point[0].toDouble()),
                                   std::floor(point[1].toDouble()));
            if (!document.canvasBounds.contains(QRectF(position, QSizeF(1, 1))))
                return {
                    failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("point")),
                    {}};
            if (workObserved)
                workObserved(document.id, QStringLiteral("render"));
            const auto image = document.runtime.renderToImage(QRectF(position, QSizeF(1, 1)),
                                                              QSize(1, 1), document.sources);
            if (image.isNull())
                return {failure(request, QStringLiteral("output_failed")), {}};
            const auto color = image.pixelColor(0, 0);
            return {success(request,
                            {{QStringLiteral("document_id"), id},
                             {QStringLiteral("revision"), static_cast<qint64>(document.revision)},
                             {QStringLiteral("rgba"),
                              QJsonArray{color.red(), color.green(), color.blue(), color.alpha()}},
                             {QStringLiteral("hex"), color.name(QColor::HexArgb)}}),
                    {}};
        }
        if (method == QStringLiteral("snow_shot_document_close")) {
            admission->remove(document.owner, document.sourceBytes, document.historyCharge);
            invalidate(document);
            documents.erase(found);
            return {success(request, {{QStringLiteral("document_id"), id},
                                      {QStringLiteral("closed"), true}}),
                    {}};
        }
        if (method == QStringLiteral("snow_shot_document_clone")) {
            const auto source = *found;
            McpDocumentService::Source input;
            input.images = source->sources;
            input.canvasBounds = source->canvasBounds;
            input.metadata = source->metadata;
            input.originalContent = source->originalContent;
            input.recognitionResults = source->recognitionResults;
            auto result = create(request, std::move(input));
            if (!result.response.ok)
                return result;
            const auto clone = documents.value(
                result.response.result.value(QStringLiteral("document_id")).toString());
            if (!clone->runtime.cloneDocumentSessionFrom(source->runtime)) {
                admission->remove(clone->owner, clone->sourceBytes, clone->historyCharge);
                documents.remove(clone->id);
                return {failure(request, QStringLiteral("output_failed")), {}};
            }
            clone->selection = source->selection;
            clone->originalContent = source->originalContent;
            clone->recognitionResults = source->recognitionResults;
            clone->tool = source->tool;
            return {success(request, clone->state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_apply_annotations")) {
            for (const auto& value : params.value(QStringLiteral("operations")).toArray()) {
                const auto operation = value.toObject();
                if (operation.contains(QStringLiteral("bounds"))) {
                    const auto bounds = operation.value(QStringLiteral("bounds")).toArray();
                    if (bounds.size() != 4)
                        return {failure(request, QStringLiteral("invalid_parameters"),
                                        QStringLiteral("operations.bounds")),
                                {}};
                    for (const auto& coordinate : bounds)
                        if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble()))
                            return {failure(request, QStringLiteral("invalid_parameters"),
                                            QStringLiteral("operations.bounds")),
                                    {}};
                    const QRectF rect(bounds[0].toDouble(), bounds[1].toDouble(),
                                      bounds[2].toDouble(), bounds[3].toDouble());
                    if (rect.isEmpty() || !document.canvasBounds.contains(rect))
                        return {failure(request, QStringLiteral("invalid_parameters"),
                                        QStringLiteral("operations.bounds")),
                                {}};
                }
                auto points = operation.value(QStringLiteral("points")).toArray();
                if (operation.contains(QStringLiteral("center")))
                    points.append(operation.value(QStringLiteral("center")));
                for (const auto& point : points) {
                    const auto pair = point.toArray();
                    if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble() ||
                        !std::isfinite(pair[0].toDouble()) || !std::isfinite(pair[1].toDouble()) ||
                        !document.canvasBounds.contains(
                            QPointF(pair[0].toDouble(), pair[1].toDouble())))
                        return {failure(request, QStringLiteral("invalid_parameters"),
                                        QStringLiteral("operations.points")),
                                {}};
                }
            }
            const QJsonObject batch{
                {QStringLiteral("version"), params.value(QStringLiteral("version")).toInt(1)},
                {QStringLiteral("label"),
                 params.value(QStringLiteral("label")).toString(QStringLiteral("MCP annotation"))},
                {QStringLiteral("operations"), params.value(QStringLiteral("operations"))}};
            const QByteArray bytes = QJsonDocument(batch).toJson(QJsonDocument::Compact);
            if (bytes.size() > 1024 * 1024)
                return {failure(request, QStringLiteral("invalid_parameters"),
                                QStringLiteral("operations")),
                        {}};
            const auto old = document.runtime.documentRevision();
            const auto transaction = document.runtime.applyAnnotationTransaction(bytes);
            if (transaction.isEmpty())
                return {failure(request, QStringLiteral("invalid_parameters"),
                                QStringLiteral("operations")),
                        {}};
            if (old != document.runtime.documentRevision()) {
                ++document.revision;
                invalidate(document);
            }
            auto state = document.state();
            state.insert(QStringLiteral("transaction"),
                         QJsonDocument::fromJson(transaction).object());
            return {success(request, state), {}};
        }
        if (method == QStringLiteral("snow_shot_document_set_selection")) {
            const auto before = document.selection.selectionRegion().toJson();
            QString field;
            if (!applySelection(document.selection, document.canvasBounds, params, &field))
                return {failure(request, QStringLiteral("invalid_parameters"), field), {}};
            if (before != document.selection.selectionRegion().toJson()) {
                ++document.revision;
                invalidate(document);
            }
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_set_selection_style")) {
            auto candidate = document.selection;
            for (const auto& key :
                 {QStringLiteral("corner_radius"), QStringLiteral("shadow_width")}) {
                if (!params.contains(key))
                    continue;
                const auto value = params.value(key);
                const double number = value.toDouble(-1);
                if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > 8192 ||
                    std::floor(number) != number)
                    return {failure(request, QStringLiteral("invalid_parameters"), key), {}};
                if (key == QStringLiteral("corner_radius"))
                    static_cast<void>(candidate.setCornerRadius(static_cast<int>(number)));
                else
                    static_cast<void>(candidate.setShadowWidth(static_cast<int>(number)));
            }
            if (params.contains(QStringLiteral("shadow_color"))) {
                const auto color = params.value(QStringLiteral("shadow_color")).toArray();
                if (color.size() != 4)
                    return {failure(request, QStringLiteral("invalid_parameters"),
                                    QStringLiteral("shadow_color")),
                            {}};
                for (const auto& value : color)
                    if (!value.isDouble() || value.toDouble(-1) < 0 || value.toDouble(256) > 255 ||
                        std::floor(value.toDouble()) != value.toDouble())
                        return {failure(request, QStringLiteral("invalid_parameters"),
                                        QStringLiteral("shadow_color")),
                                {}};
                candidate.setShadowColor(
                    QColor(color[0].toInt(), color[1].toInt(), color[2].toInt(), color[3].toInt()));
            }
            if (params.contains(QStringLiteral("aspect_ratio_locked"))) {
                if (!params.value(QStringLiteral("aspect_ratio_locked")).isBool())
                    return {failure(request, QStringLiteral("invalid_parameters"),
                                    QStringLiteral("aspect_ratio_locked")),
                            {}};
                static_cast<void>(candidate.setAspectRatioLockEnabled(
                    params.value(QStringLiteral("aspect_ratio_locked")).toBool(), 1));
            }
            if (candidate.params(document.canvasBounds.toAlignedRect()) !=
                document.selection.params(document.canvasBounds.toAlignedRect())) {
                document.selection = std::move(candidate);
                ++document.revision;
                invalidate(document);
            }
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_undo") ||
            method == QStringLiteral("snow_shot_document_redo")) {
            const bool changed = method.endsWith(QStringLiteral("_redo")) ? document.runtime.redo()
                                                                          : document.runtime.undo();
            if (changed) {
                ++document.revision;
                invalidate(document);
            }
            return {success(request, document.state()), {}};
        }
        if (method == QStringLiteral("snow_shot_document_auto_filter")) {
            const auto selection = document.selection.pixelSelection();
            if (workObserved)
                workObserved(document.id, QStringLiteral("render"));
            auto image =
                document.runtime.renderToImage(selection, selection.size(), document.sources);
            if (image.isNull())
                return {failure(request, QStringLiteral("output_failed")), {}};
            auto value = document.state();
            value.insert(QStringLiteral("canvas_rect"), rectangle(selection));
            return {success(request, std::move(value)), std::move(image)};
        }
        const double scale = params.value(QStringLiteral("scale")).toDouble(1);
        if (!std::isfinite(scale) || scale < 0.1 || scale > 4)
            return {failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("scale")),
                    {}};
        const QRect selection = document.selection.pixelSelection();
        const QSize size(std::max(1, qCeil(selection.width() * scale)),
                         std::max(1, qCeil(selection.height() * scale)));
        if (!validSize(size))
            return {failure(request, QStringLiteral("output_too_large")), {}};
        const bool cached = !document.rendered.isNull() && document.renderedScale == scale;
        if (!cached && workObserved)
            workObserved(document.id, QStringLiteral("render"));
        QImage image = cached ? document.rendered
                              : document.runtime.renderToImage(selection, size, document.sources);
        if (image.isNull())
            return {failure(request, QStringLiteral("output_failed")), {}};
        if (!cached) {
            auto style = document.selection.resultStyle();
            style.regionScale *= scale;
            style.cornerRadius = qRound(style.cornerRadius * scale);
            style.shadowWidth = qRound(style.shadowWidth * scale);
            image = ScreenshotResultCompositor::compose(image, style);
            if (image.isNull())
                return {failure(request, QStringLiteral("output_failed")), {}};
            invalidate(document);
            if (image.sizeInBytes() <= kMaximumCacheBytes) {
                bool retained = admission->reserveCache(image.sizeInBytes());
                if (!retained) {
                    for (auto& other : documents)
                        invalidate(*other);
                    retained = admission->reserveCache(image.sizeInBytes());
                }
                if (retained) {
                    document.rendered = image;
                    document.renderedScale = scale;
                }
            }
        }
        WorkerResult output{
            success(request,
                    {{QStringLiteral("document_id"), id},
                     {QStringLiteral("revision"), static_cast<qint64>(document.revision)},
                     {QStringLiteral("source_revision"), static_cast<qint64>(document.revision)},
                     {QStringLiteral("width"), image.width()},
                     {QStringLiteral("height"), image.height()},
                     {QStringLiteral("scale"), scale},
                     {QStringLiteral("cache_hit"), cached}}),
            image};
        if (method == QStringLiteral("snow_shot_document_recognize")) {
            McpDocumentService::Source source;
            source.originalContent = document.originalContent;
            source.recognitionResults = document.recognitionResults;
            output.source = std::move(source);
        }
        return output;
    }
    WorkerResult autoFilter(const ScreenshotMcpRequest& request,
                            SnowCanvasAutoFilterRecord record) {
        const auto id = request.params.value(QStringLiteral("document_id")).toString();
        const auto found = documents.find(id);
        if (found == documents.end() || (*found)->owner != request.connectionId)
            return {failure(request, QStringLiteral("document_not_found")), {}};
        auto& document = **found;
        if (!request.expectedRevision || *request.expectedRevision != document.revision)
            return {failure(request, QStringLiteral("stale_revision")), {}};
        const qint64 charge = 4096LL * (record.regions.size() + 1);
        if (document.historyCharge + charge > 64 * 1024 * 1024 ||
            !admission->reserveHistory(charge))
            return {failure(request, QStringLiteral("resource_limit")), {}};
        auto releaseCharge = qScopeGuard([&] { admission->releaseHistory(charge); });
        SnowCanvasRuntimeEditor editor(document.runtime, SnowCanvasTool::AutoFilter);
        if (!editor.setAutoFilterRegions(record))
            return {failure(request, QStringLiteral("output_failed")), {}};
        for (const auto& value : request.params.value(QStringLiteral("categories")).toArray()) {
            bool present = false;
            for (const auto& region : record.regions)
                present |= region.category == value.toString();
            if (present && !editor.fillAutoFilterCategory(value.toString()))
                return {failure(request, QStringLiteral("output_failed")), {}};
        }
        if (!editor.setActiveTool(kCanvasTools.value(document.tool)))
            return {failure(request, QStringLiteral("output_failed")), {}};
        ++document.revision;
        document.historyCharge += charge;
        releaseCharge.dismiss();
        invalidate(document);
        auto state = document.state();
        state.insert(QStringLiteral("regions"), record.regions.size());
        return {success(request, std::move(state)), {}};
    }
};
} // namespace

struct McpDocumentService::Impl {
    McpDocumentService& q;
    Ports ports;
    static constexpr int laneCount = 2;
    std::array<QThread, laneCount> workerThreads;
    std::array<DocumentWorker*, laneCount> workers{};
    std::shared_ptr<DocumentAdmission> admission = std::make_shared<DocumentAdmission>();
    struct Affinity {
        quint64 owner;
        int lane;
    };
    QHash<QString, Affinity> documentLanes;
    int nextLane = 0;
    QHash<quint64, std::shared_ptr<std::atomic_bool>> owners;
    struct ActiveRequest {
        ScreenshotMcpRequest request;
        std::shared_ptr<std::atomic_bool> canceled;
        ScreenshotMcpServer::Completion completion;
    };
    QHash<QString, ActiveRequest> activeRequests;
    QHash<QString, std::shared_ptr<ScreenshotExportArtifact>> artifacts;
    struct ArtifactCache {
        quint64 owner = 0;
        quint64 revision = 0;
        double scale = 1;
        ScreenshotCompressionLevel compression = ScreenshotCompressionLevel::Medium;
        qsizetype bytes = 0;
        std::shared_ptr<ScreenshotExportArtifact> artifact;
        QJsonObject metadata;
    };
    QHash<QString, ArtifactCache> artifactCache;
    qsizetype artifactCacheBytes = 0;
    qsizetype artifactCacheLimit() const {
        return std::clamp<qsizetype>(ports.artifactCacheBytes, 0, kMaximumCacheBytes);
    }
    void retainEncodedMetadata(const QString& documentId,
                               const std::shared_ptr<ScreenshotExportArtifact>& artifact,
                               const QJsonObject& metadata) {
        auto cached = artifactCache.find(documentId);
        if (cached == artifactCache.end() || cached->artifact != artifact)
            return;
        const bool firstEncoding = cached->metadata.isEmpty();
        cached->metadata = metadata;
        if (firstEncoding) {
            const auto size = metadata.value(QStringLiteral("byte_count")).toInteger();
            cached->bytes += size;
            artifactCacheBytes += size;
        }
        if (artifactCacheBytes > artifactCacheLimit())
            artifactCacheBytes -= artifactCache.take(documentId).bytes;
    }
    struct Blob {
        quint64 owner = 0;
        QByteArray bytes;
        QJsonObject descriptor;
        qint64 expires = 0;
        std::shared_ptr<QTemporaryDir> file;
        qint64 diskSize = 0;
    };
    QHash<QString, Blob> blobs;
    qsizetype blobBytes = 0;
    qint64 diskBytes = 0;
    qint64 pendingDiskBytes = 0;
    QHash<quint64, int> pendingFileCounts;
    int pendingFiles = 0;
    QHash<QString, ScreenshotClipboardCommitHandle> clipboard;
    QHash<QString, QPointer<ScreenshotRecognitionSessionController>> recognition;
    struct RetainedRecognition {
        quint64 owner = 0;
        quint64 sourceRevision = 0;
        quint64 revision = 1;
        QString job;
        QPointer<ScreenshotRecognitionSessionController> session;
        qint64 editBytes = 0;
    };
    QHash<QString, RetainedRecognition> retainedRecognition;
    QHash<QString, ScreenshotExportJobHandle> textExports;
    struct Replay {
        QByteArray fingerprint;
        std::optional<ScreenshotMcpResponse> response;
        QVector<ScreenshotMcpServer::Completion> waiters;
        qsizetype bytes = 0;
    };
    QHash<QString, Replay> replay;
    QQueue<QString> replayOrder;
    qsizetype replayBytes = 0;
    bool stopped = false;
    Impl(McpDocumentService& owner, Ports options) : q(owner), ports(std::move(options)) {
        if (!ports.jobs)
            ports.jobs = new McpJobRegistry(&q);
        for (int index = 0; index < laneCount; ++index) {
            auto* worker =
                new DocumentWorker(admission, ports.beforeWorkerDecode, ports.workObserved);
            workers[index] = worker;
            auto& thread = workerThreads[index];
            worker->moveToThread(&thread);
            QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
            thread.setObjectName(QStringLiteral("McpDocumentWorker%1").arg(index));
            thread.start();
        }
        auto* expiration = new QTimer(&q);
        expiration->setInterval(30000);
        QObject::connect(expiration, &QTimer::timeout, &q, [this] { pruneBlobs(); });
        expiration->start();
    }
    qint64 now() const {
        return ports.clock ? ports.clock() : QDateTime::currentMSecsSinceEpoch();
    }
    int laneFor(const ScreenshotMcpRequest& request) {
        if (request.method == QStringLiteral("snow_shot_document_open"))
            return nextLane++ % laneCount;
        return documentLanes
            .value(request.params.value(QStringLiteral("document_id")).toString(), Affinity{0, 0})
            .lane;
    }
    void pruneBlobs() {
        const auto timestamp = now();
        for (auto it = blobs.begin(); it != blobs.end();) {
            if (it->expires > timestamp) {
                ++it;
                continue;
            }
            const auto owner = it->owner;
            const auto id = it.key();
            blobBytes -= it->bytes.size();
            diskBytes -= it->diskSize;
            it = blobs.erase(it);
            emit q.artifactChanged(owner, id);
        }
    }
    void readBlob(const ScreenshotMcpRequest& request, ScreenshotMcpServer::Completion done) {
        pruneBlobs();
        if (request.method == QStringLiteral("snow_shot_artifact_list")) {
            QJsonArray values;
            for (const auto& blob : std::as_const(blobs))
                if (blob.owner == request.connectionId)
                    values.append(blob.descriptor);
            done(success(request, {{QStringLiteral("artifacts"), values}}));
            return;
        }
        const auto id = request.params.value(QStringLiteral("artifact_id")).toString();
        const auto found = blobs.find(id);
        if (found == blobs.end() || found->owner != request.connectionId) {
            done(failure(request, QStringLiteral("artifact_not_found")));
            return;
        }
        if (request.method == QStringLiteral("snow_shot_artifact_release")) {
            blobBytes -= found->bytes.size();
            diskBytes -= found->diskSize;
            blobs.erase(found);
            emit q.artifactChanged(request.connectionId, id);
            done(success(request, {{QStringLiteral("artifact_id"), id},
                                   {QStringLiteral("released"), true}}));
            return;
        }
        const auto offsetValue = request.params.value(QStringLiteral("offset"));
        const auto limitValue = request.params.value(QStringLiteral("max_bytes"));
        const auto offset = offsetValue.toInteger(0);
        const auto limit = limitValue.toInteger(65536);
        if ((!offsetValue.isUndefined() &&
             (!offsetValue.isDouble() || offsetValue.toDouble() != static_cast<double>(offset))) ||
            (!limitValue.isUndefined() &&
             (!limitValue.isDouble() || limitValue.toDouble() != static_cast<double>(limit))) ||
            offset < 0 || offset > (found->file ? found->diskSize : found->bytes.size()) ||
            limit < 1 || limit > 262144) {
            done(failure(request, QStringLiteral("invalid_parameters")));
            return;
        }
        const auto bytes = found->bytes;
        const auto file = found->file;
        const auto size = file ? found->diskSize : bytes.size();
        auto descriptor = found->descriptor;
        const auto canceled = requestToken(request);
        QPointer<McpDocumentService> guard(&q);
        auto* worker = workers[qHash(id) % laneCount];
        QMetaObject::invokeMethod(
            worker,
            [this, guard, canceled, request, id, bytes, file, size,
             descriptor = std::move(descriptor), offset, limit, done = std::move(done)]() mutable {
                if (canceled->load())
                    return;
                if (!descriptor.contains(QStringLiteral("sha256")))
                    descriptor.insert(
                        QStringLiteral("sha256"),
                        QString::fromLatin1(
                            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
                QByteArray chunk;
                bool readable = true;
                if (file) {
                    QFile input(file->filePath(QStringLiteral("content")));
                    readable = input.open(QIODevice::ReadOnly) && input.seek(offset);
                    if (readable) {
                        chunk = input.read(limit);
                        readable = chunk.size() == std::min<qint64>(limit, size - offset);
                    }
                } else
                    chunk = bytes.mid(offset, limit);
                auto value = descriptor;
                value.insert(QStringLiteral("offset"), offset);
                value.insert(QStringLiteral("next_offset"), offset + chunk.size());
                value.insert(QStringLiteral("eof"), offset + chunk.size() == size);
                if (!guard)
                    return;
                QMetaObject::invokeMethod(
                    guard,
                    [this, guard, canceled, request, id, descriptor = std::move(descriptor),
                     value = std::move(value), chunk, readable, done = std::move(done)]() mutable {
                        if (!guard || canceled->load())
                            return;
                        auto current = blobs.find(id);
                        if (current == blobs.end() || current->owner != request.connectionId) {
                            done(failure(request, QStringLiteral("artifact_not_found")));
                            return;
                        }
                        if (!readable) {
                            done(failure(request, QStringLiteral("output_failed")));
                            return;
                        }
                        current->descriptor = descriptor;
                        auto response = success(request, std::move(value));
                        response.attachment = chunk;
                        response.attachmentMime =
                            descriptor.value(QStringLiteral("mime_type")).toString();
                        done(std::move(response));
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    std::shared_ptr<std::atomic_bool> token(quint64 owner) {
        auto& value = owners[owner];
        if (!value)
            value = std::make_shared<std::atomic_bool>(false);
        return value;
    }
    std::shared_ptr<std::atomic_bool> requestToken(const ScreenshotMcpRequest& request) {
        const auto found = activeRequests.constFind(QString::number(request.connectionId) + u':' +
                                                    request.requestId);
        return found == activeRequests.cend() ? std::make_shared<std::atomic_bool>(true)
                                              : found->canceled;
    }
    void dispatch(ScreenshotMcpRequest request, ScreenshotMcpServer::Completion done,
                  std::optional<Source> source = std::nullopt,
                  std::shared_ptr<DocumentReservation> reservation = {}) {
        const auto canceled = token(request.connectionId);
        const auto requestCanceled = requestToken(request);
        QPointer<McpDocumentService> guard(&q);
        if (request.method == QStringLiteral("snow_shot_document_list")) {
            auto remaining = std::make_shared<int>(laneCount);
            auto values = std::make_shared<QJsonArray>();
            auto completion = std::make_shared<ScreenshotMcpServer::Completion>(std::move(done));
            for (auto* worker : workers)
                QMetaObject::invokeMethod(
                    worker,
                    [guard, worker, canceled, requestCanceled, request, remaining, values,
                     completion] {
                        if (!guard || canceled->load() || requestCanceled->load())
                            return;
                        auto result = worker->request(request);
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, canceled, requestCanceled, request, result = std::move(result),
                             remaining, values, completion]() mutable {
                                if (!guard || canceled->load() || requestCanceled->load())
                                    return;
                                for (const auto& value :
                                     result.response.result.value(QStringLiteral("documents"))
                                         .toArray())
                                    values->append(value);
                                if (--*remaining == 0)
                                    (*completion)(
                                        success(request, {{QStringLiteral("documents"), *values}}));
                            },
                            Qt::QueuedConnection);
                    },
                    Qt::QueuedConnection);
            return;
        }
        const int lane = laneFor(request);
        auto* worker = workers[lane];
        const auto beforeWork = ports.beforeWorkerRequest;
        QMetaObject::invokeMethod(
            worker,
            [this, guard, worker, lane, beforeWork, canceled, requestCanceled, request,
             done = std::move(done), source = std::move(source),
             reservation = std::move(reservation)]() mutable {
                if (beforeWork)
                    beforeWork(request);
                if (canceled->load() || requestCanceled->load())
                    return;
                auto result =
                    source ? (request.method == QStringLiteral("snow_shot_document_recapture")
                                  ? worker->replace(request, std::move(*source),
                                                    std::move(reservation))
                                  : worker->create(request, std::move(*source),
                                                   std::move(reservation)))
                           : worker->request(request, requestCanceled);
                const bool created = request.method == QStringLiteral("snow_shot_document_open") ||
                                     request.method == QStringLiteral("snow_shot_document_clone");
                if (canceled->load() || requestCanceled->load() || !guard) {
                    if (created && result.response.ok)
                        worker->discard(
                            request.connectionId,
                            result.response.result.value(QStringLiteral("document_id")).toString());
                    return;
                }
                QMetaObject::invokeMethod(
                    guard,
                    [this, guard, worker, lane, canceled, requestCanceled, request,
                     result = std::move(result), done = std::move(done)]() mutable {
                        if (!guard)
                            return;
                        if (canceled->load() || requestCanceled->load()) {
                            if (!stopped && result.response.ok &&
                                (request.method == QStringLiteral("snow_shot_document_open") ||
                                 request.method == QStringLiteral("snow_shot_document_clone"))) {
                                const auto createdId =
                                    result.response.result.value(QStringLiteral("document_id"))
                                        .toString();
                                QMetaObject::invokeMethod(
                                    worker,
                                    [target = worker, owner = request.connectionId, createdId] {
                                        target->discard(owner, createdId);
                                    },
                                    Qt::QueuedConnection);
                            }
                            return;
                        }
                        const auto documentId =
                            request.params.value(QStringLiteral("document_id")).toString();
                        if (result.response.ok) {
                            if (request.method == QStringLiteral("snow_shot_document_open") ||
                                request.method == QStringLiteral("snow_shot_document_clone"))
                                documentLanes.insert(
                                    result.response.result.value(QStringLiteral("document_id"))
                                        .toString(),
                                    Affinity{request.connectionId, lane});
                            else if (request.method == QStringLiteral("snow_shot_document_close"))
                                documentLanes.remove(documentId);
                        }
                        const auto documentRevision = static_cast<quint64>(
                            result.response.result.value(QStringLiteral("revision")).toInteger());
                        if (result.response.ok &&
                            (request.method ==
                                 QStringLiteral("snow_shot_document_recognition_state") ||
                             request.method ==
                                 QStringLiteral("snow_shot_document_edit_recognition") ||
                             request.method ==
                                 QStringLiteral("snow_shot_document_export_recognition"))) {
                            recognitionCommand(request, std::move(result.response),
                                               std::move(done));
                            return;
                        }
                        if (result.response.ok &&
                            request.method ==
                                QStringLiteral("snow_shot_document_original_content")) {
                            textOutput(request, std::move(result.response), std::move(done));
                            return;
                        }
                        if (result.response.ok && retainedRecognition.contains(documentId) &&
                            (retainedRecognition.value(documentId).sourceRevision !=
                                 documentRevision ||
                             request.method == QStringLiteral("snow_shot_document_close"))) {
                            auto entry = retainedRecognition.take(documentId);
                            ports.jobs->cancel(entry.owner, entry.job);
                            if (entry.session) {
                                entry.session->cancelWorkflow();
                                entry.session->deleteLater();
                            }
                        }
                        if (result.response.ok && result.source &&
                            request.method == QStringLiteral("snow_shot_document_pin")) {
                            if (!ports.pinDocument) {
                                done(failure(request, QStringLiteral("unavailable")));
                                return;
                            }
                            auto completion =
                                std::make_shared<ScreenshotMcpServer::Completion>(std::move(done));
                            if (const auto entry = retainedRecognition.value(documentId);
                                entry.session)
                                result.source->recognitionResults =
                                    entry.session->recognitionResultsSnapshot();
                            const auto response = result.response;
                            if (!ports.pinDocument(
                                    std::move(*result.source), std::move(result.image),
                                    [guard, requestCanceled, request, response,
                                     completion](bool ok) mutable {
                                        if (!guard || requestCanceled->load() || !*completion)
                                            return;
                                        auto publish = std::exchange(*completion, {});
                                        auto value = response;
                                        value.result.insert(QStringLiteral("pinned"), true);
                                        value.result.insert(QStringLiteral("ownership"),
                                                            QStringLiteral("user"));
                                        publish(
                                            ok ? value
                                               : failure(request, QStringLiteral("output_failed")));
                                    }) &&
                                *completion) {
                                auto publish = std::exchange(*completion, {});
                                publish(failure(request, QStringLiteral("busy")));
                            }
                            return;
                        }
                        if (result.response.ok && result.source &&
                            request.method == QStringLiteral("snow_shot_document_present")) {
                            if (!ports.present) {
                                done(failure(request, QStringLiteral("unavailable")));
                                return;
                            }
                            auto completion =
                                std::make_shared<ScreenshotMcpServer::Completion>(std::move(done));
                            const auto response = result.response;
                            if (const auto entry = retainedRecognition.value(documentId);
                                entry.session)
                                result.source->recognitionResults =
                                    entry.session->recognitionResultsSnapshot();
                            if (!ports.present(
                                    std::move(*result.source),
                                    [guard, requestCanceled, request, response,
                                     completion](bool ok) mutable {
                                        if (!guard || requestCanceled->load() || !*completion)
                                            return;
                                        auto deliver = std::exchange(*completion, {});
                                        auto value = response;
                                        value.result.insert(QStringLiteral("presented"), true);
                                        value.result.insert(QStringLiteral("ownership"),
                                                            QStringLiteral("user"));
                                        deliver(ok ? value
                                                   : failure(request, QStringLiteral(
                                                                          "presentation_failed")));
                                    }) &&
                                *completion) {
                                auto deliver = std::exchange(*completion, {});
                                deliver(failure(request, QStringLiteral("busy")));
                            }
                            return;
                        }
                        if (!result.response.ok || result.image.isNull()) {
                            if (result.response.ok &&
                                request.method == QStringLiteral("snow_shot_document_close")) {
                                const auto id =
                                    request.params.value(QStringLiteral("document_id")).toString();
                                artifactCacheBytes -= artifactCache.take(id).bytes;
                                for (const auto& value : ports.jobs->list(request.connectionId)) {
                                    const auto job = value.toObject();
                                    if (job.value(QStringLiteral("document_id")) == id)
                                        ports.jobs->cancel(
                                            request.connectionId,
                                            job.value(QStringLiteral("job_id")).toString());
                                }
                            }
                            done(std::move(result.response));
                            return;
                        }
                        if (request.method == QStringLiteral("snow_shot_document_auto_filter")) {
                            autoFilter(request, std::move(result), std::move(done));
                            return;
                        }
                        if (request.method == QStringLiteral("snow_shot_document_recognize")) {
                            recognize(request, std::move(result), std::move(done));
                            return;
                        }
                        output(request, std::move(result), std::move(done));
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }
    void autoFilter(const ScreenshotMcpRequest& request, WorkerResult result,
                    ScreenshotMcpServer::Completion done) {
        const QStringList allowed{QStringLiteral("text"),      QStringLiteral("text_in_box"),
                                  QStringLiteral("image"),     QStringLiteral("avatar"),
                                  QStringLiteral("icon"),      QStringLiteral("message_box"),
                                  QStringLiteral("text_block")};
        const auto categories = request.params.value(QStringLiteral("categories")).toArray();
        if (!request.params.value(QStringLiteral("categories")).isArray() || categories.isEmpty()) {
            done(failure(request, QStringLiteral("invalid_parameters")));
            return;
        }
        for (const auto& category : categories)
            if (!allowed.contains(category.toString())) {
                done(failure(request, QStringLiteral("invalid_parameters")));
                return;
            }
        if (!ports.autoFilter) {
            done(failure(request, QStringLiteral("provider_unavailable")));
            return;
        }
        const auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto ownerCanceled = token(request.connectionId);
        const auto id = ports.jobs->start(
            request.connectionId, QStringLiteral("document_auto_filter"),
            [canceled] { canceled->store(true); },
            {{QStringLiteral("document_id"), request.params.value(QStringLiteral("document_id"))},
             {QStringLiteral("source_revision"),
              static_cast<qint64>(request.expectedRevision.value_or(0))}});
        if (id.isEmpty()) {
            done(failure(request, QStringLiteral("queue_full")));
            return;
        }
        QPointer<McpDocumentService> guard(&q);
        const auto canvasRect =
            result.response.result.value(QStringLiteral("canvas_rect")).toArray();
        const QRectF bounds(canvasRect[0].toDouble(), canvasRect[1].toDouble(),
                            canvasRect[2].toDouble(), canvasRect[3].toDouble());
        const auto pixels = result.image.size();
        ports.autoFilter(std::move(result.image), [this, guard, canceled, ownerCanceled, request,
                                                   id, bounds, pixels](
                                                      QList<SnowCanvasAutoFilterRegion> regions,
                                                      QString error) mutable {
            if (!guard || canceled->load() || ownerCanceled->load())
                return;
            if (!error.isEmpty()) {
                ports.jobs->fail(id, QStringLiteral("recognition_failed"));
                return;
            }
            for (auto& region : regions)
                region.bounds =
                    QRectF(bounds.x() + region.bounds.x() * bounds.width() / pixels.width(),
                           bounds.y() + region.bounds.y() * bounds.height() / pixels.height(),
                           region.bounds.width() * bounds.width() / pixels.width(),
                           region.bounds.height() * bounds.height() / pixels.height())
                        .intersected(bounds);
            regions.removeIf([](const auto& region) { return region.bounds.isEmpty(); });
            // Once the engine transaction is queued it must complete atomically; a
            // cancel acknowledgment may not race a successful document mutation.
            if (!ports.jobs->setCancellation(id, {}))
                return;
            auto* worker = workers[laneFor(request)];
            QMetaObject::invokeMethod(
                worker,
                [this, guard, worker, canceled, ownerCanceled, request, id,
                 record = SnowCanvasAutoFilterRecord{bounds, std::move(regions)}]() mutable {
                    if (canceled->load() || ownerCanceled->load())
                        return;
                    auto value = worker->autoFilter(request, std::move(record));
                    if (!guard)
                        return;
                    QMetaObject::invokeMethod(
                        guard,
                        [this, guard, canceled, ownerCanceled, request, id,
                         value = std::move(value)]() mutable {
                            if (!guard || canceled->load() || ownerCanceled->load())
                                return;
                            if (value.response.ok) {
                                ports.jobs->complete(id, value.response.result);
                                emit q.changed(
                                    request.connectionId,
                                    request.params.value(QStringLiteral("document_id")).toString());
                            } else
                                ports.jobs->fail(id, value.response.errorCode);
                        },
                        Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
        });
        done(success(request, *ports.jobs->get(request.connectionId, id)));
    }
    void recognize(const ScreenshotMcpRequest& request, WorkerResult result,
                   ScreenshotMcpServer::Completion done) {
        const QString kind =
            request.params.value(QStringLiteral("kind")).toString(QStringLiteral("text"));
        const QStringList modes{QStringLiteral("text"), QStringLiteral("table"),
                                QStringLiteral("qr"), QStringLiteral("markdown"),
                                QStringLiteral("html")};
        const int index = modes.indexOf(kind);
        if (index < 0 || (index == 0 && !ports.recognition) ||
            (index == 2 && !ports.qrRecognition) || (index != 0 && index != 2 && !ports.api)) {
            done(failure(request,
                         index < 0 ? QStringLiteral("invalid_parameters")
                                   : QStringLiteral("provider_unavailable"),
                         QStringLiteral("kind")));
            return;
        }
        if (!screenshotOcrImageWithinPixelLimit(result.image.size()) && index == 0) {
            done(failure(request, QStringLiteral("output_too_large")));
            return;
        }
        ScreenshotRecognitionSessionActions actions;
        actions.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
        auto* session = new ScreenshotRecognitionSessionController(
            ports.recognition, ports.qrRecognition, ports.api, std::move(actions), &q);
        QPointer<ScreenshotRecognitionSessionController> guardedSession(session);
        const auto documentId = request.params.value(QStringLiteral("document_id")).toString();
        const auto sourceRevision = static_cast<quint64>(
            result.response.result.value(QStringLiteral("revision")).toInteger());
        auto prior = retainedRecognition.take(documentId);
        ScreenshotRecognitionResults cached;
        if (prior.session) {
            if (prior.sourceRevision == sourceRevision)
                cached = prior.session->recognitionResultsSnapshot();
            ports.jobs->cancel(prior.owner, prior.job);
            prior.session->cancelWorkflow();
            prior.session->deleteLater();
        } else if (result.source)
            cached = result.source->recognitionResults;
        const QString id = ports.jobs->start(
            request.connectionId, QStringLiteral("document_recognition"),
            [guardedSession] {
                if (guardedSession) {
                    guardedSession->cancelWorkflow();
                    guardedSession->deleteLater();
                }
            },
            {{QStringLiteral("document_id"), request.params.value(QStringLiteral("document_id"))},
             {QStringLiteral("source_revision"),
              result.response.result.value(QStringLiteral("revision"))}});
        if (id.isEmpty()) {
            delete session;
            done(failure(request, QStringLiteral("queue_full")));
            return;
        }
        recognition.insert(id, session);
        QObject::connect(session, &QObject::destroyed, &q, [this, id, documentId] {
            recognition.remove(id);
            if (retainedRecognition.value(documentId).job == id)
                retainedRecognition.remove(documentId);
        });
        retainedRecognition.insert(
            documentId, RetainedRecognition{request.connectionId, sourceRevision, 1, id, session});
        QPointer<McpDocumentService> guard(&q);
        const auto settle = [this, guard, session, id, documentId] {
            if (!guard || !recognition.contains(id) ||
                session->workflowState().value(QStringLiteral("busy")).toBool())
                return;
            const auto state = session->workflowState();
            auto value = session->workflowResult();
            const auto entry = retainedRecognition.value(documentId);
            const bool empty = value.isEmpty();
            value.insert(QStringLiteral("recognition_revision"),
                         static_cast<qint64>(entry.revision));
            value.insert(QStringLiteral("source_revision"),
                         static_cast<qint64>(entry.sourceRevision));
            if (!state.value(QStringLiteral("error")).toString().isEmpty() || empty)
                ports.jobs->fail(id, QStringLiteral("recognition_failed"));
            else
                ports.jobs->complete(id, value);
            recognition.remove(id);
            emit q.changed(entry.owner, documentId);
        };
        QObject::connect(session, &ScreenshotRecognitionSessionController::workflowStateChanged,
                         session, settle, Qt::QueuedConnection);
        QObject::connect(session,
                         &ScreenshotRecognitionSessionController::recognitionResultsChanged,
                         session, settle, Qt::QueuedConnection);
        session->setTarget({id, result.image, QRectF(QPointF(), result.image.size())});
        cached.key = id;
        if (!cached.isEmpty())
            session->seedRecognitionResults(std::move(cached));
        session->activate(static_cast<ScreenshotRecognitionSessionController::Mode>(index));
        QTimer::singleShot(0, session, settle);
        QTimer::singleShot(300000, session, [this, guardedSession, id] {
            if (!guardedSession || !guardedSession->busy())
                return;
            ports.jobs->fail(id, QStringLiteral("timeout"));
            guardedSession->cancelWorkflow();
            guardedSession->deleteLater();
            recognition.remove(id);
        });
        done(success(request, *ports.jobs->get(request.connectionId, id)));
    }
    void recognitionCommand(const ScreenshotMcpRequest& request, ScreenshotMcpResponse response,
                            ScreenshotMcpServer::Completion done) {
        const auto documentId = request.params.value(QStringLiteral("document_id")).toString();
        auto entry = retainedRecognition.find(documentId);
        if (entry == retainedRecognition.end() || !entry->session ||
            entry->owner != request.connectionId ||
            entry->sourceRevision !=
                static_cast<quint64>(
                    response.result.value(QStringLiteral("revision")).toInteger())) {
            done(failure(request, QStringLiteral("recognition_required")));
            return;
        }
        if (request.method == QStringLiteral("snow_shot_document_edit_recognition")) {
            if (request.params.value(QStringLiteral("expected_recognition_revision"))
                    .toInteger(-1) != static_cast<qint64>(entry->revision)) {
                auto value = failure(request, QStringLiteral("stale_revision"));
                value.errorDetails.insert(QStringLiteral("recognition_revision"),
                                          static_cast<qint64>(entry->revision));
                done(std::move(value));
                return;
            }
            if (entry->session->busy()) {
                done(failure(request, QStringLiteral("busy")));
                return;
            }
            const auto charge =
                4 * QJsonDocument(request.params).toJson(QJsonDocument::Compact).size() + 4096;
            if (entry->editBytes + charge > 16 * 1024 * 1024) {
                done(failure(request, QStringLiteral("resource_limit")));
                return;
            }
            if (!entry->session->editWorkflow(request.params)) {
                done(failure(request, QStringLiteral("action_unavailable")));
                return;
            }
            ++entry->revision;
            entry->editBytes += charge;
            emit q.changed(entry->owner, documentId);
        }
        auto value = entry->session->workflowResult();
        value.insert(QStringLiteral("workflow"), entry->session->workflowState());
        value.insert(QStringLiteral("recognition_revision"), static_cast<qint64>(entry->revision));
        value.insert(QStringLiteral("source_revision"), static_cast<qint64>(entry->sourceRevision));
        value.insert(QStringLiteral("document_id"), documentId);
        response.result = std::move(value);
        if (request.method == QStringLiteral("snow_shot_document_export_recognition"))
            textOutput(request, std::move(response), std::move(done));
        else
            done(std::move(response));
    }
    void textOutput(const ScreenshotMcpRequest& request, ScreenshotMcpResponse response,
                    ScreenshotMcpServer::Completion done) {
        const auto output =
            request.params.value(QStringLiteral("output")).toString(QStringLiteral("return"));
        const auto format =
            request.params.value(QStringLiteral("format")).toString(QStringLiteral("json"));
        QByteArray bytes;
        if (format == QStringLiteral("json"))
            bytes = QJsonDocument(response.result).toJson(QJsonDocument::Compact);
        else if (QStringList{QStringLiteral("text"), QStringLiteral("html"),
                             QStringLiteral("markdown")}
                     .contains(format) &&
                 response.result.contains(format))
            bytes = response.result.value(format).toString().toUtf8();
        else {
            done(failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("format")));
            return;
        }
        if (output == QStringLiteral("return")) {
            if (format != QStringLiteral("json")) {
                QJsonObject value{{QStringLiteral("format"), format},
                                  {format, response.result.value(format)}};
                for (const auto& key :
                     {QStringLiteral("document_id"), QStringLiteral("revision"),
                      QStringLiteral("source_revision"), QStringLiteral("recognition_revision")})
                    if (response.result.contains(key))
                        value.insert(key, response.result.value(key));
                response.result = std::move(value);
            }
            if (bytes.size() > 1024 * 1024) {
                const auto descriptor = q.storeArtifact(
                    request.connectionId, std::move(bytes),
                    format == QStringLiteral("json") ? QStringLiteral("application/json")
                                                     : QStringLiteral("text/plain;charset=utf-8"));
                if (descriptor.isEmpty()) {
                    done(failure(request, QStringLiteral("resource_limit")));
                    return;
                }
                response.result = {{QStringLiteral("artifact"), descriptor},
                                   {QStringLiteral("format"), format}};
            }
            done(std::move(response));
            return;
        }
        if (output == QStringLiteral("copy")) {
            auto* mime = new QMimeData;
            mime->setText(QString::fromUtf8(bytes));
            if (format == QStringLiteral("html"))
                mime->setHtml(QString::fromUtf8(bytes));
            QApplication::clipboard()->setMimeData(mime);
            done(success(request, {{QStringLiteral("copied"), true}}));
            return;
        }
        QString path;
        if (output != QStringLiteral("save") ||
            !ScreenshotMcpSession::validateOutputPath(
                request.params.value(QStringLiteral("path")).toString(), &path)) {
            done(failure(request, QStringLiteral("invalid_parameters"), QStringLiteral("path")));
            return;
        }
        const auto key = QString::number(request.connectionId) + u':' + request.requestId;
        const auto canceled = requestToken(request);
        auto deliver = std::make_shared<ScreenshotMcpServer::Completion>(std::move(done));
        auto job = ScreenshotExportCoordinator::shared().submit(
            &q, ScreenshotExportCoordinator::Priority::Foreground,
            [bytes = std::move(bytes), path](const ScreenshotExportCancellation& cancellation) {
                QSaveFile file(path);
                if (cancellation.isCancellationRequested() || !file.open(QIODevice::WriteOnly) ||
                    file.write(bytes) != bytes.size() || cancellation.isCancellationRequested() ||
                    !file.commit())
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               QStringLiteral("output_failed"));
                ScreenshotExportTaskResult result;
                result.savedPath = path;
                return result;
            },
            [this, key, canceled, request, deliver](ScreenshotExportTaskResult result) mutable {
                textExports.remove(key);
                if (!canceled->load() && *deliver) {
                    auto publish = std::exchange(*deliver, {});
                    publish(result.succeeded()
                                ? success(request, {{QStringLiteral("path"), result.savedPath}})
                                : failure(request, QStringLiteral("output_failed")));
                }
            });
        if (job.isValid())
            textExports.insert(key, std::move(job));
        else if (*deliver) {
            auto complete = std::exchange(*deliver, {});
            complete(failure(request, QStringLiteral("queue_full")));
        }
    }
    void output(const ScreenshotMcpRequest& request, WorkerResult result,
                ScreenshotMcpServer::Completion done) {
        const QString outputId = QString::number(request.connectionId) + u':' + request.requestId;
        const QString documentId = request.params.value(QStringLiteral("document_id")).toString();
        const auto revision = static_cast<quint64>(
            result.response.result.value(QStringLiteral("revision")).toInteger());
        const auto scale = result.response.result.value(QStringLiteral("scale")).toDouble(1);
        const bool pngSave =
            request.method == QStringLiteral("snow_shot_document_save") &&
            request.params.value(QStringLiteral("format")).toString(QStringLiteral("png")) ==
                QStringLiteral("png");
        const bool reusable =
            request.method == QStringLiteral("snow_shot_document_render") || pngSave;
        const auto compression = ScreenshotImageFileService::compressionLevelForKey(
            pngSave ? request.params.value(QStringLiteral("compression_level"))
                          .toString(QStringLiteral("medium"))
                    : snow_shot::storage::ScreenshotSettings().compressionLevel());
        auto cached = artifactCache.find(documentId);
        std::shared_ptr<ScreenshotExportArtifact> artifact;
        bool encodedCacheHit = false;
        if (reusable && cached != artifactCache.end() && cached->revision == revision &&
            cached->scale == scale && cached->compression == compression &&
            !cached->artifact->isCancelled()) {
            artifact = cached->artifact;
            // Metadata is installed only after the immutable PNG and its digest are ready.
            // A retained raster alone must not advertise a hit when encoding was evicted.
            encodedCacheHit = !cached->metadata.isEmpty();
        } else {
            if (reusable)
                artifactCacheBytes -= artifactCache.take(documentId).bytes;
            const auto bytes = result.image.sizeInBytes();
            ScreenshotExportArtifact::PngCachePolicy cachePolicy;
            if (ports.workObserved)
                cachePolicy.encodingStarted = [observed = ports.workObserved, documentId] {
                    observed(documentId, QStringLiteral("encode"));
                };
            artifact = std::make_shared<ScreenshotExportArtifact>(
                ScreenshotExportSource::fromImage(std::move(result.image)), compression,
                cachePolicy);
            if (reusable && bytes <= artifactCacheLimit()) {
                if (artifactCacheBytes + bytes > artifactCacheLimit()) {
                    artifactCache.clear();
                    artifactCacheBytes = 0;
                }
                artifactCache.insert(
                    documentId,
                    ArtifactCache{
                        request.connectionId, revision, scale, compression, bytes, artifact, {}});
                artifactCacheBytes += bytes;
            }
        }
        if (reusable)
            result.response.result.insert(
                QStringLiteral("cache_hit"),
                result.response.result.value(QStringLiteral("cache_hit")).toBool() &&
                    encodedCacheHit);
        artifacts.insert(outputId, artifact);
        const auto canceled = requestToken(request);
        const auto finish = [this, outputId, canceled,
                             done = std::move(done)](ScreenshotMcpResponse response) {
            artifacts.remove(outputId);
            clipboard.remove(outputId);
            if (!canceled->load())
                done(std::move(response));
        };
        if (request.method == QStringLiteral("snow_shot_document_pin")) {
            if (!ports.pin) {
                finish(failure(request, QStringLiteral("unavailable")));
                return;
            }
            if (!artifact->requestImage(&q, [this, request, canceled, response = result.response,
                                             finish](ScreenshotExportImageResult image) mutable {
                    if (canceled->load())
                        return;
                    if (!image.succeeded() ||
                        !ports.pin(std::move(image.image),
                                   [request, response, finish](bool pinned) mutable {
                                       if (!pinned) {
                                           finish(failure(request, QStringLiteral("pin_failed")));
                                           return;
                                       }
                                       response.result.insert(QStringLiteral("pinned"), true);
                                       finish(std::move(response));
                                   }))
                        finish(failure(request, QStringLiteral("pin_failed")));
                }))
                finish(failure(request, QStringLiteral("output_failed")));
            return;
        }
        if (request.method == QStringLiteral("snow_shot_document_copy")) {
            if (!artifact->requestClipboard(
                    &q, [this, request, outputId, canceled, response = result.response,
                         finish](ScreenshotExportClipboardResult prepared) mutable {
                        if (canceled->load())
                            return;
                        if (!prepared.succeeded()) {
                            finish(failure(request, QStringLiteral("output_failed")));
                            return;
                        }
                        const auto commit = ScreenshotClipboardService::commit(
                            QApplication::clipboard(), &q, std::move(prepared.payload),
                            [request, response,
                             finish](ScreenshotClipboardCommitResult committed) mutable {
                                if (!committed.succeeded()) {
                                    finish(failure(request, QStringLiteral("clipboard_failed")));
                                    return;
                                }
                                response.result.insert(QStringLiteral("copied"), true);
                                finish(std::move(response));
                            });
                        if (commit.isValid())
                            clipboard.insert(outputId, commit);
                        else
                            finish(failure(request, QStringLiteral("clipboard_failed")));
                    }))
                finish(failure(request, QStringLiteral("output_failed")));
            return;
        }
        if (request.method == QStringLiteral("snow_shot_document_save")) {
            const QString format =
                request.params.value(QStringLiteral("format")).toString(QStringLiteral("png"));
            const QStringList formats{QStringLiteral("png"),  QStringLiteral("jpeg"),
                                      QStringLiteral("webp"), QStringLiteral("jxl"),
                                      QStringLiteral("bmp"),  QStringLiteral("avif"),
                                      QStringLiteral("pdf")};
            QString path;
            ScreenshotImageEncodingOptions encoding;
            ScreenshotPdfOptions pdf;
            if (!formats.contains(format) || !imageExportOptions(request.params, encoding, pdf) ||
                !ScreenshotMcpSession::validateOutputPath(
                    request.params.value(QStringLiteral("path")).toString(), &path)) {
                finish(failure(request, QStringLiteral("invalid_parameters"),
                               QStringLiteral("path_or_format")));
                return;
            }
            if (!artifact->requestSaveToPath(
                    &q, path, ScreenshotImageFileService::formatForKey(format), encoding,
                    [this, request, format, documentId, artifact, response = result.response,
                     finish](ScreenshotExportTaskResult saved) mutable {
                        if (!saved.succeeded()) {
                            finish(failure(request, QStringLiteral("output_failed")));
                            return;
                        }
                        response.result.insert(QStringLiteral("path"), saved.savedPath);
                        response.result.insert(QStringLiteral("format"), format);
                        const auto encoded = artifactCache.constFind(documentId);
                        if (format == QStringLiteral("png") && encoded != artifactCache.cend() &&
                            encoded->artifact == artifact && !encoded->metadata.isEmpty()) {
                            for (auto it = encoded->metadata.begin(); it != encoded->metadata.end();
                                 ++it)
                                response.result.insert(it.key(), it.value());
                            finish(std::move(response));
                            return;
                        }
                        auto metadata = std::make_shared<QJsonObject>();
                        auto handle = ScreenshotExportCoordinator::shared().submit(
                            &q, ScreenshotExportCoordinator::Priority::Background,
                            [metadata, path = saved.savedPath](
                                const ScreenshotExportCancellation& cancellation) {
                                QFile file(path);
                                if (!file.open(QIODevice::ReadOnly))
                                    return ScreenshotExportTaskResult::failure(
                                        ScreenshotExportFailureStage::File,
                                        QStringLiteral("read_failed"));
                                QCryptographicHash hash(QCryptographicHash::Sha256);
                                while (!file.atEnd()) {
                                    if (cancellation.isCancellationRequested())
                                        return ScreenshotExportTaskResult::failure(
                                            ScreenshotExportFailureStage::Cancelled,
                                            QStringLiteral("canceled"));
                                    const auto bytes = file.read(1024 * 1024);
                                    if (bytes.isEmpty() && !file.atEnd())
                                        return ScreenshotExportTaskResult::failure(
                                            ScreenshotExportFailureStage::File,
                                            QStringLiteral("read_failed"));
                                    hash.addData(bytes);
                                }
                                *metadata = {{QStringLiteral("byte_count"), file.size()},
                                             {QStringLiteral("sha256"),
                                              QString::fromLatin1(hash.result().toHex())}};
                                return ScreenshotExportTaskResult{};
                            },
                            [this, request, format, documentId, artifact, metadata, response,
                             finish](ScreenshotExportTaskResult hashed) mutable {
                                if (!hashed.succeeded()) {
                                    finish(failure(request, QStringLiteral("output_failed")));
                                    return;
                                }
                                for (auto it = metadata->begin(); it != metadata->end(); ++it)
                                    response.result.insert(it.key(), it.value());
                                if (format == QStringLiteral("png")) {
                                    auto encodedMetadata = *metadata;
                                    encodedMetadata.insert(QStringLiteral("format"), format);
                                    retainEncodedMetadata(documentId, artifact, encodedMetadata);
                                }
                                finish(std::move(response));
                            });
                        if (!handle.isValid())
                            finish(failure(request, QStringLiteral("queue_full")));
                    },
                    pdf))
                finish(failure(request, QStringLiteral("output_failed")));
            return;
        }
        if (!artifact->requestCanonicalPng(&q, [this, request, artifact, documentId,
                                                response = result.response, finish](
                                                   ScreenshotExportEncodingResult encoded) mutable {
                if (!encoded.succeeded() ||
                    encoded.image.bytes().size() > 64 * 1024 * 1024 - 65536) {
                    finish(failure(request, QStringLiteral("output_too_large")));
                    return;
                }
                const auto png = encoded.image.bytes();
                const auto cached = artifactCache.constFind(documentId);
                if (cached != artifactCache.cend() && cached->artifact == artifact &&
                    !cached->metadata.isEmpty()) {
                    for (auto it = cached->metadata.begin(); it != cached->metadata.end(); ++it)
                        response.result.insert(it.key(), it.value());
                    response.attachment = png;
                    response.attachmentMime = QStringLiteral("image/png");
                    finish(std::move(response));
                    return;
                }
                auto digest = std::make_shared<QString>();
                auto handle = ScreenshotExportCoordinator::shared().submit(
                    &q, ScreenshotExportCoordinator::Priority::Background,
                    [png, digest](const ScreenshotExportCancellation& cancellation) {
                        if (cancellation.isCancellationRequested())
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Cancelled,
                                QStringLiteral("canceled"));
                        *digest = QString::fromLatin1(
                            QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
                        return ScreenshotExportTaskResult{};
                    },
                    [this, request, png, digest, artifact, documentId, response,
                     finish](ScreenshotExportTaskResult hashed) mutable {
                        if (!hashed.succeeded()) {
                            finish(failure(request, QStringLiteral("output_failed")));
                            return;
                        }
                        response.result.insert(QStringLiteral("format"), QStringLiteral("png"));
                        response.result.insert(QStringLiteral("byte_count"), png.size());
                        response.result.insert(QStringLiteral("sha256"), *digest);
                        retainEncodedMetadata(documentId, artifact,
                                              {{QStringLiteral("format"), QStringLiteral("png")},
                                               {QStringLiteral("byte_count"), png.size()},
                                               {QStringLiteral("sha256"), *digest}});
                        response.attachment = png;
                        response.attachmentMime = QStringLiteral("image/png");
                        finish(std::move(response));
                    });
                if (!handle.isValid())
                    finish(failure(request, QStringLiteral("queue_full")));
            }))
            finish(failure(request, QStringLiteral("output_failed")));
    }
    void execute(const ScreenshotMcpRequest& request, ScreenshotMcpServer::Completion done) {
        if (request.method == QStringLiteral("snow_shot_document_open") &&
            request.params.value(QStringLiteral("as_job"))
                .toBool(request.params.value(QStringLiteral("source")) ==
                            QStringLiteral("capture") &&
                        request.params.value(QStringLiteral("delay_seconds")).toDouble() > 0)) {
            const auto childId = QStringLiteral("document-open-") + identifier();
            QPointer<McpDocumentService> guard(&q);
            const auto id = ports.jobs->start(request.connectionId, QStringLiteral("document_open"),
                                              [guard, owner = request.connectionId, childId] {
                                                  if (guard)
                                                      guard->cancelRequest(owner, childId);
                                              });
            if (id.isEmpty()) {
                done(failure(request, QStringLiteral("queue_full")));
                return;
            }
            auto child = request;
            child.requestId = childId;
            child.idempotencyKey = childId;
            child.params.insert(QStringLiteral("as_job"), false);
            auto reply = success(request, *ports.jobs->get(request.connectionId, id));
            q.request(child, [this, guard, id](ScreenshotMcpResponse response) {
                if (!guard)
                    return;
                if (response.ok)
                    ports.jobs->complete(id, std::move(response.result));
                else
                    ports.jobs->fail(id, response.errorCode);
            });
            done(std::move(reply));
            return;
        }
        if (request.method.startsWith(QStringLiteral("snow_shot_artifact_"))) {
            readBlob(request, std::move(done));
            return;
        }
        if (request.method.startsWith(QStringLiteral("snow_shot_job_"))) {
            if (request.method == QStringLiteral("snow_shot_job_list")) {
                done(success(request,
                             {{QStringLiteral("jobs"), ports.jobs->list(request.connectionId)}}));
                return;
            }
            const auto id = request.params.value(QStringLiteral("job_id")).toString();
            auto value = ports.jobs->get(request.connectionId, id);
            if (!value) {
                done(failure(request, QStringLiteral("job_not_found")));
                return;
            }
            if (request.method == QStringLiteral("snow_shot_job_cancel")) {
                if (!ports.jobs->cancel(request.connectionId, id)) {
                    done(failure(request, QStringLiteral("not_cancelable")));
                    return;
                }
                value = ports.jobs->get(request.connectionId, id);
            }
            done(success(request, *value));
            return;
        }
        if (request.method == QStringLiteral("snow_shot_document_recapture") ||
            (request.method == QStringLiteral("snow_shot_document_open") &&
             request.params.value(QStringLiteral("source")).toString(QStringLiteral("file")) !=
                 QStringLiteral("file"))) {
            if (!ports.resolveSource) {
                done(failure(request, QStringLiteral("unsupported_source")));
                return;
            }
            QPointer<McpDocumentService> guard(&q);
            const auto canceled = token(request.connectionId);
            const auto requestCanceled = requestToken(request);
            const bool opening = request.method == QStringLiteral("snow_shot_document_open");
            if (opening && !admission->create(request.connectionId, 0, 0)) {
                done(failure(request, QStringLiteral("resource_limit")));
                return;
            }
            auto reservation = std::make_shared<DocumentReservation>();
            reservation->admission = admission;
            reservation->owner = request.connectionId;
            reservation->ownsSlot = opening;
            ports.resolveSource(
                request,
                [this, guard, canceled, requestCanceled, request, reservation,
                 done = std::move(done)](Source source, QString error) mutable {
                    if (!guard || canceled->load() || requestCanceled->load())
                        return;
                    if (!error.isEmpty() || (source.image.isNull() && source.images.isEmpty())) {
                        done(failure(request,
                                     error.isEmpty() ? QStringLiteral("invalid_source") : error));
                        return;
                    }
                    dispatch(request, std::move(done), std::move(source), std::move(reservation));
                },
                [reservation, canceled, requestCanceled](qint64 bytes) {
                    QMutexLocker lock(&reservation->mutex);
                    if (bytes < 0 || reservation->consumed || canceled->load() ||
                        requestCanceled->load() ||
                        !reservation->admission->replace(reservation->bytes, bytes))
                        return false;
                    reservation->bytes = bytes;
                    return true;
                });
            return;
        }
        dispatch(request, std::move(done));
    }
};

McpDocumentService::McpDocumentService(Ports ports, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(ports))) {}
McpDocumentService::~McpDocumentService() {
    shutdown();
}
bool McpDocumentService::handles(const QString& method) const {
    return kTools.contains(method);
}
QStringList McpDocumentService::capabilities() const {
    return kTools;
}
QJsonObject McpDocumentService::storeArtifact(quint64 owner, QByteArray bytes,
                                              const QString& mimeType) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    s.pruneBlobs();
    if (s.stopped || owner == 0 || bytes.size() > 64 * 1024 * 1024 || mimeType.isEmpty() ||
        mimeType.size() > 128 || mimeType.contains(u'\r') || mimeType.contains(u'\n') ||
        s.blobs.size() + s.pendingFiles >= 64 || s.blobBytes + bytes.size() > 256LL * 1024 * 1024)
        return {};
    int owned = s.pendingFileCounts.value(owner);
    for (const auto& blob : std::as_const(s.blobs))
        owned += blob.owner == owner ? 1 : 0;
    if (owned >= 16)
        return {};
    static_cast<void>(s.token(owner));
    const auto id = identifier();
    const auto now = QDateTime::fromMSecsSinceEpoch(s.now()).toUTC();
    QJsonObject descriptor{{QStringLiteral("artifact_id"), id},
                           {QStringLiteral("uri"), QStringLiteral("snow-shot://artifacts/") + id},
                           {QStringLiteral("mime_type"), mimeType},
                           {QStringLiteral("byte_count"), bytes.size()},
                           {QStringLiteral("created_at"), now.toString(Qt::ISODateWithMs)},
                           {QStringLiteral("ttl_ms"), 900000}};
    s.blobBytes += bytes.size();
    s.blobs.insert(
        id,
        Impl::Blob{owner, std::move(bytes), descriptor, now.toMSecsSinceEpoch() + 900000, {}, 0});
    emit artifactChanged(owner, id);
    return descriptor;
}
void McpDocumentService::storeFileArtifact(quint64 owner, const QString& sourcePath,
                                           const QString& mimeType,
                                           std::function<void(QJsonObject, QString)> completion) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    s.pruneBlobs();
    const QFileInfo source(sourcePath);
    const auto size = source.size();
    int owned = s.pendingFileCounts.value(owner);
    for (const auto& blob : std::as_const(s.blobs))
        owned += blob.owner == owner ? 1 : 0;
    if (s.stopped || owner == 0 || !source.isAbsolute() ||
        sourcePath.startsWith(QStringLiteral("\\\\")) ||
        sourcePath.startsWith(QStringLiteral("//")) || mimeType.isEmpty() ||
        mimeType.size() > 128 || mimeType.contains(u'\r') || mimeType.contains(u'\n')) {
        completion({},
                   s.stopped ? QStringLiteral("canceled") : QStringLiteral("invalid_parameters"));
        return;
    }
    if (!source.isFile() || size < 0) {
        completion({}, QStringLiteral("file_unavailable"));
        return;
    }
    if (size > 2LL * 1024 * 1024 * 1024 || owned >= 16 || s.blobs.size() + s.pendingFiles >= 64 ||
        s.diskBytes + s.pendingDiskBytes + size > 4LL * 1024 * 1024 * 1024) {
        completion({}, QStringLiteral("capacity_exceeded"));
        return;
    }
    const auto canceled = s.token(owner);
    const auto retained = std::make_shared<std::shared_ptr<QTemporaryDir>>();
    const auto hash = std::make_shared<QString>();
    ++s.pendingFiles;
    ++s.pendingFileCounts[owner];
    s.pendingDiskBytes += size;
    QPointer<McpDocumentService> guard(this);
    auto publish = [this, guard, canceled, retained, hash, owner, size, mimeType,
                    completion = std::move(completion)](ScreenshotExportTaskResult result) mutable {
        if (!guard)
            return;
        auto& state = *m_impl;
        --state.pendingFiles;
        if (--state.pendingFileCounts[owner] == 0)
            state.pendingFileCounts.remove(owner);
        state.pendingDiskBytes -= size;
        if (!result.succeeded() || canceled->load() || state.stopped || !*retained) {
            completion({}, canceled->load() || state.stopped ? QStringLiteral("canceled")
                           : !result.error.isEmpty()         ? result.error
                                                             : QStringLiteral("file_unavailable"));
            return;
        }
        const auto id = identifier();
        const auto now = QDateTime::fromMSecsSinceEpoch(state.now()).toUTC();
        QJsonObject descriptor{
            {QStringLiteral("artifact_id"), id},
            {QStringLiteral("uri"), QStringLiteral("snow-shot://artifacts/") + id},
            {QStringLiteral("mime_type"), mimeType},
            {QStringLiteral("byte_count"), size},
            {QStringLiteral("created_at"), now.toString(Qt::ISODateWithMs)},
            {QStringLiteral("ttl_ms"), 900000},
            {QStringLiteral("max_file_bytes"), 2LL * 1024 * 1024 * 1024},
            {QStringLiteral("global_disk_quota_bytes"), 4LL * 1024 * 1024 * 1024},
            {QStringLiteral("sha256"), *hash}};
        state.diskBytes += size;
        state.blobs.insert(
            id,
            Impl::Blob{owner, {}, descriptor, now.toMSecsSinceEpoch() + 900000, *retained, size});
        emit artifactChanged(owner, id);
        completion(std::move(descriptor), {});
    };
    const auto handle = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Background,
        [source, size, canceled, retained, hash](const ScreenshotExportCancellation& cancellation) {
            auto temporary = std::make_shared<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/snow-shot-mcp-artifact-XXXXXX"));
            QFile input(source.absoluteFilePath());
            QFile output(temporary->filePath(QStringLiteral("content")));
            if (!temporary->isValid() || !input.open(QIODevice::ReadOnly) || input.size() != size ||
                !output.open(QIODevice::WriteOnly))
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           QStringLiteral("file_unavailable"));
            QCryptographicHash digest(QCryptographicHash::Sha256);
            qint64 copied = 0;
            while (copied < size) {
                if (canceled->load() || cancellation.isCancellationRequested())
                    return ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Cancelled, QStringLiteral("canceled"));
                const auto bytes = input.read(std::min<qint64>(1024 * 1024, size - copied));
                if (bytes.isEmpty() || output.write(bytes) != bytes.size())
                    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                               QStringLiteral("copy_failed"));
                digest.addData(bytes);
                copied += bytes.size();
            }
            if (!output.flush() ||
                QFileInfo(source.absoluteFilePath()).lastModified() != source.lastModified())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                           QStringLiteral("source_changed"));
            output.close();
            *hash = QString::fromLatin1(digest.result().toHex());
            *retained = std::move(temporary);
            return ScreenshotExportTaskResult{};
        },
        publish);
    if (!handle.isValid())
        publish(ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File,
                                                    QStringLiteral("queue_full")));
}
void McpDocumentService::request(const ScreenshotMcpRequest& request,
                                 ScreenshotMcpServer::Completion done) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    const auto activeKey = QString::number(request.connectionId) + u':' + request.requestId;
    if (s.activeRequests.size() >= 64 || s.activeRequests.contains(activeKey)) {
        done(failure(request, QStringLiteral("queue_full")));
        return;
    }
    s.activeRequests.insert(
        activeKey,
        Impl::ActiveRequest{request, std::make_shared<std::atomic_bool>(false), std::move(done)});
    done = [this, activeKey](ScreenshotMcpResponse response) {
        auto active = m_impl->activeRequests.find(activeKey);
        if (active == m_impl->activeRequests.end())
            return;
        const auto request = active->request;
        auto completion = std::move(active->completion);
        m_impl->activeRequests.erase(active);
        if (response.ok && !readOnly(request.method) &&
            response.result.value(QStringLiteral("document_id")).isString())
            emit changed(request.connectionId,
                         response.result.value(QStringLiteral("document_id")).toString());
        completion(std::move(response));
    };
    if (s.stopped || !handles(request.method) || request.connectionId == 0) {
        done(failure(request,
                     s.stopped ? QStringLiteral("disabled") : QStringLiteral("method_not_found")));
        return;
    }
    if (readOnly(request.method)) {
        s.execute(request, std::move(done));
        return;
    }
    if (request.idempotencyKey.isEmpty() || request.idempotencyKey.size() > 128) {
        done(failure(request, QStringLiteral("invalid_parameters"),
                     QStringLiteral("idempotency_key")));
        return;
    }
    const auto fingerprint = QCryptographicHash::hash(
        QJsonDocument(QJsonObject{{QStringLiteral("method"), request.method},
                                  {QStringLiteral("params"), request.params},
                                  {QStringLiteral("expected_revision"),
                                   static_cast<qint64>(request.expectedRevision.value_or(0))}})
            .toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
    const auto key = QString::number(request.connectionId) + u':' + request.idempotencyKey;
    const auto found = s.replay.find(key);
    if (found != s.replay.end()) {
        if (found->fingerprint != fingerprint) {
            done(failure(request, QStringLiteral("idempotency_conflict")));
            return;
        }
        if (found->response) {
            auto response = *found->response;
            response.requestId = request.requestId;
            done(std::move(response));
        } else if (found->waiters.size() < 8) {
            found->waiters.append([done = std::move(done),
                                   id = request.requestId](ScreenshotMcpResponse response) mutable {
                response.requestId = id;
                done(std::move(response));
            });
        } else
            done(failure(request, QStringLiteral("queue_full")));
        return;
    }
    if (s.replay.size() - s.replayOrder.size() >= 32) {
        done(failure(request, QStringLiteral("queue_full")));
        return;
    }
    s.replay.insert(key, Impl::Replay{fingerprint, {}, {}, 0});
    s.execute(request, [this, key, done = std::move(done)](ScreenshotMcpResponse response) mutable {
        auto& s = *m_impl;
        auto entry = s.replay.find(key);
        if (entry == s.replay.end() || entry->response)
            return;
        auto waiters = std::move(entry->waiters);
        entry->response = response;
        entry->bytes = QJsonDocument(response.result).toJson(QJsonDocument::Compact).size() +
                       response.attachment.size() + 1024;
        s.replayBytes += entry->bytes;
        s.replayOrder.enqueue(key);
        while (!s.replayOrder.isEmpty() &&
               (s.replayOrder.size() > 64 || s.replayBytes > 32 * 1024 * 1024))
            s.replayBytes -= s.replay.take(s.replayOrder.dequeue()).bytes;
        done(response);
        for (auto& waiter : waiters)
            waiter(response);
    });
}
void McpDocumentService::disconnected(quint64 owner) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    if (const auto canceled = s.owners.take(owner))
        canceled->store(true);
    s.ports.jobs->disconnected(owner);
    const QString prefix = QString::number(owner) + u':';
    for (auto it = s.retainedRecognition.begin(); it != s.retainedRecognition.end();) {
        if (it->owner != owner) {
            ++it;
            continue;
        }
        const auto session = it->session;
        it = s.retainedRecognition.erase(it);
        if (session) {
            session->cancelWorkflow();
            session->deleteLater();
        }
    }
    for (auto it = s.textExports.begin(); it != s.textExports.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        it->cancel();
        it = s.textExports.erase(it);
    }
    for (auto it = s.blobs.begin(); it != s.blobs.end();) {
        if (it->owner != owner) {
            ++it;
            continue;
        }
        s.blobBytes -= it->bytes.size();
        s.diskBytes -= it->diskSize;
        it = s.blobs.erase(it);
    }
    for (auto it = s.artifactCache.begin(); it != s.artifactCache.end();) {
        if (it->owner != owner) {
            ++it;
            continue;
        }
        s.artifactCacheBytes -= it->bytes;
        it = s.artifactCache.erase(it);
    }
    for (auto it = s.activeRequests.begin(); it != s.activeRequests.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        it->canceled->store(true);
        it = s.activeRequests.erase(it);
    }
    for (auto it = s.clipboard.begin(); it != s.clipboard.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        const auto commit = *it;
        it = s.clipboard.erase(it);
        commit.cancel();
    }
    for (auto it = s.artifacts.begin(); it != s.artifacts.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        const auto artifact = *it;
        it = s.artifacts.erase(it);
        artifact->cancel();
    }
    for (auto it = s.replay.begin(); it != s.replay.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        s.replayBytes -= it->bytes;
        s.replayOrder.removeAll(it.key());
        it = s.replay.erase(it);
    }
    for (auto it = s.documentLanes.begin(); it != s.documentLanes.end();)
        if (it->owner == owner)
            it = s.documentLanes.erase(it);
        else
            ++it;
    if (!s.stopped)
        for (auto* worker : s.workers)
            QMetaObject::invokeMethod(
                worker, [worker, owner] { worker->disconnected(owner); }, Qt::QueuedConnection);
}
bool McpDocumentService::cancelRequest(quint64 owner, const QString& requestId) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    const QString key = QString::number(owner) + u':' + requestId;
    auto found = s.activeRequests.find(key);
    if (found == s.activeRequests.end())
        return false;
    auto active = std::move(*found);
    s.activeRequests.erase(found);
    active.canceled->store(true);
    if (s.ports.cancelSource)
        s.ports.cancelSource(owner, requestId);
    auto response = failure(active.request, QStringLiteral("canceled"));
    response.errorDetails.insert(QStringLiteral("outcome_may_have_completed"), true);
    const QString replayKey = QString::number(owner) + u':' + active.request.idempotencyKey;
    auto replay = s.replay.find(replayKey);
    QVector<ScreenshotMcpServer::Completion> waiters;
    if (replay != s.replay.end() && !replay->response) {
        waiters = std::move(replay->waiters);
        replay->response = response;
        replay->bytes = 1024;
        s.replayBytes += replay->bytes;
        s.replayOrder.enqueue(replayKey);
    }
    if (const auto artifact = s.artifacts.take(key)) {
        bool shared = false;
        for (const auto& cached : std::as_const(s.artifactCache))
            shared |= cached.artifact == artifact;
        for (const auto& activeArtifact : std::as_const(s.artifacts))
            shared |= activeArtifact == artifact;
        if (!shared)
            artifact->cancel();
    }
    s.clipboard.take(key).cancel();
    s.textExports.take(key).cancel();
    active.completion(response);
    for (auto& waiter : waiters)
        waiter(response);
    return true;
}
void McpDocumentService::shutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    auto& s = *m_impl;
    if (s.stopped)
        return;
    const auto owners = s.owners.keys();
    for (const auto owner : owners)
        disconnected(owner);
    s.stopped = true;
    for (auto& workerThread : s.workerThreads)
        workerThread.quit();
    for (auto& workerThread : s.workerThreads)
        workerThread.wait();
    s.workers.fill(nullptr);
}
} // namespace snow_shot::app::mcp
