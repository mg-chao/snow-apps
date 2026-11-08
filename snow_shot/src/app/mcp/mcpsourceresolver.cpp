// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/mcp/mcpsourceresolver.h"
#include "mcpasyncwork_p.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotpinnedrecognition.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/storage/capturehistoryrepository.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <mutex>

namespace snow_shot::app::mcp {
namespace {
QString sourceKey(quint64 owner, const QString& requestId) {
    return QString::number(owner) + u':' + requestId;
}

qint64 rasterBytes(QSize size) {
    const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
    return size.width() > 0 && size.height() > 0 && pixels <= 64000000 ? pixels * 8 : -1;
}
} // namespace

struct McpSourceResolver::Impl {
    Impl(McpSourceResolver& owner, Ports sourcePorts) : q(owner), ports(std::move(sourcePorts)) {
        workers.setMaxThreadCount(2);
    }

    bool cancelKey(const QString& key) {
        const auto canceled = requests.take(key);
        if (!canceled)
            return false;
        canceled->store(true);
        delete delays.take(key);
        if (captureKey == key) {
            captureKey.clear();
            const auto cancelCapture = ports.cancelCapture;
            if (cancelCapture)
                cancelCapture();
        }
        return true;
    }

    void stopAndDrain() {
        if (stopped)
            return;
        stopped = true;
        for (const auto& canceled : requests)
            canceled->store(true);
        requests.clear();
        qDeleteAll(delays);
        delays.clear();
        if (!captureKey.isEmpty()) {
            captureKey.clear();
            const auto cancelCapture = ports.cancelCapture;
            const QPointer<McpSourceResolver> guard(&q);
            if (cancelCapture)
                cancelCapture();
            if (!guard)
                return;
        }
        workers.waitForDone();
        // Finished signals may still be queued on the GUI thread. Retire their
        // receivers and occupied slots before releasing borrowed ports.
        qDeleteAll(watchers);
        watchers.clear();
    }

    void resolve(const ScreenshotMcpRequest& request, Completion completion, Budget sourceBudget) {
        if (stopped || suspended) {
            completion({}, QStringLiteral("busy"));
            return;
        }
        const auto params = request.params;
        const auto source = params.value(QStringLiteral("source")).toString();
        const auto key = sourceKey(request.connectionId, request.requestId);
        if (requests.contains(key)) {
            completion({}, QStringLiteral("busy"));
            return;
        }
        const auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto rejected = std::make_shared<std::atomic_bool>(false);
        struct BudgetState {
            std::mutex mutex;
            qint64 peak = 0;
        };
        const auto budgetState = std::make_shared<BudgetState>();
        const auto budget = [sourceBudget = std::move(sourceBudget), canceled, rejected,
                             budgetState](qint64 bytes) {
            // Native fallback and resize checks can overlap. Keep the shared
            // document reservation monotonic across every admission callback.
            const std::lock_guard lock(budgetState->mutex);
            if (canceled->load())
                return false;
            const qint64 total = std::max(budgetState->peak, bytes);
            if (bytes < 0 || !sourceBudget || !sourceBudget(total)) {
                rejected->store(true);
                return false;
            }
            budgetState->peak = total;
            return true;
        };
        requests.insert(key, canceled);
        const QPointer<McpSourceResolver> guard(&q);
        auto sourceDone = [this, guard, key, canceled, rejected,
                           completion = std::move(completion)](Source data, QString error) mutable {
            if (!guard || canceled->exchange(true))
                return;
            requests.remove(key);
            completion(std::move(data),
                       rejected->load() ? QStringLiteral("resource_limit") : std::move(error));
        };
        auto done = [sourceDone](QImage image, QJsonObject metadata, QString error) mutable {
            Source data;
            data.image = std::move(image);
            data.metadata = std::move(metadata);
            sourceDone(std::move(data), std::move(error));
        };
        if (source == u"capture") {
            const int delay = params.value(QStringLiteral("delay_seconds")).toInt(0);
            if (delay < 0 || delay > 10) {
                done({}, {}, QStringLiteral("invalid_parameters"));
                return;
            }
            auto* timer = new QTimer(&q);
            timer->setSingleShot(true);
            delays.insert(key, timer);
            QObject::connect(
                timer, &QTimer::timeout, &q,
                [this, guard, timer, key, params, canceled, budget,
                 done = std::move(done)]() mutable {
                    delays.remove(key);
                    timer->deleteLater();
                    if (!guard || canceled->load())
                        return;
                    if (stopped || suspended || !captureKey.isEmpty()) {
                        done({}, {}, QStringLiteral("busy"));
                        return;
                    }
                    const auto captureAllowed = ports.captureAllowed;
                    if (captureAllowed) {
                        const auto error = captureAllowed();
                        if (!guard)
                            return;
                        if (!error.isEmpty()) {
                            done({}, {}, error);
                            return;
                        }
                    }
                    if (!guard || canceled->load())
                        return;
                    if (stopped || suspended || !captureKey.isEmpty()) {
                        done({}, {}, QStringLiteral("busy"));
                        return;
                    }
                    const auto scale = params.value(QStringLiteral("scale")).toDouble(1.0);
                    if (!std::isfinite(scale) || scale < 0.1 || scale > 4.0) {
                        done({}, {}, QStringLiteral("invalid_parameters"));
                        return;
                    }
                    captureKey = key;
                    const auto capture = ports.capture;
                    const bool accepted =
                        capture &&
                        capture(
                            params,
                            [this, guard, key, canceled, done](QImage image, QJsonObject metadata,
                                                               QString error) mutable {
                                if (!guard)
                                    return;
                                // A canceled completion must never retire a newer capture.
                                if (captureKey == key && !canceled->load())
                                    captureKey.clear();
                                done(std::move(image), std::move(metadata), std::move(error));
                            },
                            budget);
                    if (!guard)
                        return;
                    if (!accepted) {
                        if (captureKey == key && !canceled->load())
                            captureKey.clear();
                        done({}, {}, QStringLiteral("busy"));
                    }
                });
            timer->start(delay * 1000);
            return;
        }
        if (source != u"clipboard" && source != u"history" && source != u"pinned" &&
            source != u"text" && source != u"html") {
            done({}, {}, QStringLiteral("unsupported_source"));
            return;
        }
        if (watchers.size() >= 2) {
            done({}, {}, QStringLiteral("queue_full"));
            return;
        }
        // The watcher owns the source slot from snapshot admission through worker completion.
        // Early exits retire it automatically; shutdown may also delete it during a snapshot.
        auto* watcher = new QFutureWatcher<Source>(&q);
        watchers.insert(watcher);
        const auto releaseSlot = [this, guard, watcher = QPointer(watcher)] {
            if (!guard || !watcher)
                return;
            watchers.remove(watcher.data());
            delete watcher.data();
        };
        auto pendingSlot = qScopeGuard(releaseSlot);
        std::optional<ScreenshotClipboardContentSnapshot> clipboard;
        std::optional<storage::PinnedWindowRecord> livePin;
        std::optional<ScreenshotRecognitionResults> liveRecognition;
        QString livePinTool;
        const auto clipboardPort = ports.clipboard;
        const auto livePinPort = ports.livePin;
        try {
            if (source == u"clipboard")
                clipboard = clipboardPort ? clipboardPort(budget)
                                          : ScreenshotClipboardContentReader::snapshot(
                                                QApplication::clipboard(), 1.0, budget);
            if (!guard || canceled->load())
                return;
            if (source == u"pinned" && livePinPort) {
                auto snapshot =
                    livePinPort(params.value(QStringLiteral("source_id")).toString(), budget);
                if (!guard || canceled->load())
                    return;
                if (snapshot) {
                    livePin = std::move(snapshot->record);
                    liveRecognition = std::move(snapshot->recognition);
                    livePinTool = std::move(snapshot->tool);
                }
            }
        } catch (...) {
            if (!guard || canceled->load())
                return;
            releaseSlot();
            done({}, {}, QStringLiteral("invalid_source"));
            return;
        }
        if (suspended) {
            releaseSlot();
            done({}, {}, QStringLiteral("busy"));
            return;
        }
        if (rejected->load()) {
            releaseSlot();
            done({}, {}, QStringLiteral("resource_limit"));
            return;
        }
        QObject::connect(watcher, &QFutureWatcher<Source>::finished, &q,
                         [this, watcher, sourceDone = std::move(sourceDone)]() mutable {
                             watchers.remove(watcher);
                             Source result;
                             try {
                                 result = watcher->result();
                             } catch (...) {
                                 // A failed worker still releases its lane and completes once.
                             }
                             watcher->deleteLater();
                             const auto error = result.image.isNull() && result.images.isEmpty()
                                                    ? QStringLiteral("invalid_source")
                                                    : QString();
                             sourceDone(std::move(result), error);
                         });
        try {
            const auto swatchStyle = capturePinnedColorSwatchStyle();
            const auto font = QApplication::font();
            watcher->setFuture(runMcpWork(&workers, [source, clipboard, livePin, liveRecognition,
                                                     livePinTool, params, canceled, budget,
                                                     swatchStyle, font, loadPin = ports.loadPin,
                                                     historyRepository = ports.history] {
                Source result;
                if (canceled->load())
                    return result;
                if (source == u"clipboard") {
                    if (!clipboard)
                        return result;
                    const auto content = ScreenshotClipboardContentReader::decode(
                        *clipboard, [canceled] { return canceled->load(); }, budget);
                    if (content) {
                        result.image = content->image;
                        result.originalContent = content->originalContent;
                    }
                    return result;
                }
                if (source == u"text" || source == u"html") {
                    ScreenshotClipboardOriginalContent original;
                    original.text = params.value(QStringLiteral("text")).toString();
                    if (source == u"html")
                        original.html = params.value(QStringLiteral("html")).toString();
                    result.originalContent = original;
                    const auto content = ScreenshotClipboardContentReader::renderOriginalText(
                        original, 1.0, {}, budget);
                    if (content)
                        result.image = content->image;
                    return result;
                }
                if (source == u"pinned") {
                    const auto record =
                        livePin ? livePin
                        : loadPin
                            ? loadPin(params.value(QStringLiteral("source_id")).toString(), budget)
                            : std::nullopt;
                    if (!record)
                        return result;
                    result.image = record->image;
                    result.originalContent = {record->originalHtml, record->originalText,
                                              record->originalFilePath, record->textStyle,
                                              record->textFormat};
                    result.recognitionResults = liveRecognition.value_or(
                        decodeScreenshotPinnedRecognition(record->recognitionResults, font));
                    if (record->sourceKind == storage::PinnedWindowSourceKind::ClipboardText) {
                        const auto content = ScreenshotClipboardContentReader::renderOriginalText(
                            result.originalContent, record->firstCreationTextDpi, {}, budget,
                            swatchStyle);
                        if (content)
                            result.image = content->image;
                    }
                    // A pinned record retains original pixels plus a separate
                    // transform. Its annotations already use the current canvas
                    // coordinates, so import the transformed background into the
                    // current content rectangle without flattening annotations.
                    const auto originalPixels = result.image.size();
                    const auto transformedPixels =
                        record->imageTransform.mapRect(QRectF(QPointF(), originalPixels))
                            .toAlignedRect()
                            .size();
                    const auto sourceBytes = rasterBytes(originalPixels);
                    const auto transformedBytes = rasterBytes(transformedPixels);
                    if (sourceBytes < 0 || transformedBytes < 0 ||
                        !budget(sourceBytes + transformedBytes + (livePin ? 64 * 1024 * 1024 : 0) +
                                8LL * (record->canvasSession.size() +
                                       record->recognitionResults.size() +
                                       record->originalHtml.size() + record->originalText.size())))
                        return Source{};
                    if (!record->imageTransform.isIdentity())
                        result.image = snowCanvasTransformImage(
                            result.image, record->imageTransform, Qt::SmoothTransformation);
                    const auto style = decodeScreenshotResultStyle(record->resultStyle);
                    if (result.image.isNull() || !style)
                        return Source{};
                    const auto contentBounds = !record->contentCanvasRect.isEmpty()
                                                   ? record->contentCanvasRect
                                                   : record->canvasSourceRect;
                    result.canvasBounds = record->surfaceCanvasRect;
                    if (result.canvasBounds.isEmpty())
                        result.canvasBounds = QRectF(QPointF(), result.image.size());
                    const auto selectionBounds =
                        contentBounds.isEmpty() ? result.canvasBounds : contentBounds;
                    result.recognitionResults = transformScreenshotPinnedRecognition(
                        std::move(result.recognitionResults), record->canvasSourceRect,
                        originalPixels, record->imageTransform, result.image.size(),
                        selectionBounds);
                    result.images.append({result.image, selectionBounds});
                    result.selection = ScreenshotSelectionParams{selectionBounds.toAlignedRect(),
                                                                 style->cornerRadius,
                                                                 style->shadowWidth,
                                                                 style->shadowColor,
                                                                 false,
                                                                 false,
                                                                 style->region};
                    result.selection->borderType = style->borderType;
                    result.selection->borderWidth = style->borderWidth;
                    result.selection->borderColor = style->borderColor;
                    result.documentSession = record->canvasSession;
                    if (!livePinTool.isEmpty())
                        result.tool = livePinTool;
                    return result;
                }
                if (!historyRepository)
                    return result;
                auto& history = *historyRepository;
                for (const auto& record : history.records()) {
                    if (record.id != params.value(QStringLiteral("source_id")).toString())
                        continue;
                    qint64 historyBytes =
                        record.canvasBytes >= 0 && record.canvasBytes <= 64 * 1024 * 1024
                            ? 8 * record.canvasBytes
                            : -1;
                    if (!budget(historyBytes))
                        return result;
                    const auto addImage = [&](QSize size, qint64 encodedBytes) {
                        const auto bytes = rasterBytes(size);
                        if (bytes < 0 || encodedBytes < 0 || encodedBytes > 256 * 1024 * 1024)
                            return budget(-1);
                        historyBytes += bytes + 2 * encodedBytes;
                        return budget(historyBytes);
                    };
                    for (const auto& display : record.displays) {
                        if (!addImage(display.imageSize, display.encodedBytes))
                            return result;
                    }
                    if (record.result &&
                        !addImage(record.result->imageSize, record.result->encodedBytes))
                        return result;
                    const auto payload = history.load(record);
                    if (!payload)
                        return result;
                    result.documentHistory = payload->canvasHistory;
                    result.canvasBounds = record.canvasBounds;
                    const auto& selected = record.selection;
                    result.selection = ScreenshotSelectionParams{
                        selected.rectangle,       selected.cornerRadius,
                        selected.shadowWidth,     selected.shadowColor,
                        selected.lockAspectRatio, selected.lockDragAspectRatio,
                        selected.region};
                    result.selection->borderType = selected.borderType;
                    result.selection->borderWidth = selected.borderWidth;
                    result.selection->borderColor = selected.borderColor;
                    for (qsizetype index = 0;
                         index < payload->displayImages.size() && index < record.displays.size();
                         ++index) {
                        const auto& image = payload->displayImages[index];
                        const auto& display = record.displays[index];
                        const auto bounds = display.sourceCanvasRect.value_or(
                            QRect(display.sourceCanvasOrigin.value_or(QPoint()), image.size()));
                        result.images.append({image, QRectF(bounds)});
                    }
                    if (result.images.isEmpty())
                        result.image = history.loadResultImage(record).value_or(QImage());
                    return result;
                }
                return result;
            }));
            pendingSlot.dismiss();
        } catch (...) {
            releaseSlot();
            done({}, {}, QStringLiteral("invalid_source"));
        }
    }

    McpSourceResolver& q;
    Ports ports;
    QHash<QString, std::shared_ptr<std::atomic_bool>> requests;
    QHash<QString, QTimer*> delays;
    QString captureKey;
    QThreadPool workers;
    QSet<QFutureWatcher<Source>*> watchers;
    bool stopped = false;
    bool suspended = false;
};

McpSourceResolver::McpSourceResolver(Ports ports, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(ports))) {}
McpSourceResolver::~McpSourceResolver() {
    m_impl->stopAndDrain();
}
void McpSourceResolver::resolve(const ScreenshotMcpRequest& request, Completion completion,
                                Budget budget) {
    m_impl->resolve(request, std::move(completion), std::move(budget));
}
bool McpSourceResolver::cancel(quint64 owner, const QString& requestId) {
    return m_impl->cancelKey(sourceKey(owner, requestId));
}
void McpSourceResolver::disconnected(quint64 owner) {
    const QPointer<McpSourceResolver> guard(this);
    const auto prefix = QString::number(owner) + u':';
    const auto keys = m_impl->requests.keys();
    for (const auto& key : keys) {
        if (key.startsWith(prefix)) {
            m_impl->cancelKey(key);
            if (!guard)
                return;
        }
    }
}
int McpSourceResolver::workerCount() const {
    return static_cast<int>(m_impl->watchers.size());
}
bool McpSourceResolver::hasPendingSources() const {
    return !m_impl->requests.isEmpty() || !m_impl->watchers.isEmpty();
}
void McpSourceResolver::setSuspended(bool suspended) {
    m_impl->suspended = suspended;
}
void McpSourceResolver::stopAndDrain() {
    m_impl->stopAndDrain();
}
} // namespace snow_shot::app::mcp
