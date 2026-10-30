// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_shot/presentation/screenshotrecognitionresults.h"
#include "snow_shot/presentation/screenshotimagesource.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotpinnedtextpreparation.h"
#include "snow_shot/storage/capturehistoryrepository.h"
#include "snow_shot/storage/pinnedwindowtypes.h"
#include <QObject>
#include <QHash>

namespace snow_shot::presentation {

struct ImageEditingSource {
    QImage image;
    ScreenshotImageSource layeredImage;
    QSize pixelSize;
    QRectF canvasRect;
    QByteArray canvasSession;
    QByteArray canvasHistory;
    SnowCanvasSmartEraseSnapshot smartErase;
    QList<SnowCanvasBaseImageSource> samplingSources;
    ScreenshotRecognitionResults recognition;
    QString originalFileName;
    QString error;
    bool flattened = false;
    // A pending pin appearance is composed before importing into a rectangular document.
    std::optional<ScreenshotResultStyle> resultStyle;
};

ImageEditingSource readImageEditingHistory(storage::CaptureHistoryRepository& repository,
                                           const storage::CaptureHistoryRecord& record);
ImageEditingSource readImageEditingPinned(storage::PinnedWindowRecord record,
                                          const ScreenshotPinnedTextPreparation& preparation);
// Restrict only the base raster; retain the annotation document and its canvas origin.
void restrictImageEditingSource(ImageEditingSource& source, const QRectF& visible);

[[nodiscard]] QImage renderImageEditingBaseImage(const ScreenshotImageSource& source,
                                                 const QRectF& canvasRect, const QSize& pixelSize);
[[nodiscard]] ScreenshotExportSource imageEditingCompositeSource(const ImageEditingSource& source);
[[nodiscard]] ScreenshotExportSource imageEditingExportSource(const ImageEditingSource& source);
[[nodiscard]] ImageEditingSource completeImageEditingComposite(ImageEditingSource source,
                                                               QImage image);

class ImageEditingDocument final : public QObject {
    Q_OBJECT
  public:
    enum class CropAxis { Rows, Columns };
    struct Crop {
        CropAxis axis = CropAxis::Rows;
        int start = 0;
        int length = 0;
        QSize beforeSize;
        QImage removed;
        QImage result;
        qint64 sourceKey = 0;
    };
    explicit ImageEditingDocument(QObject* parent = nullptr);
    bool load(const ImageEditingSource& source);
    SnowCanvasRuntime& runtime() {
        return m_runtime;
    }
    const QImage& image() const {
        return m_image;
    }
    QRectF bounds() const;
    QPointF imagePixelAt(QPointF canvasPoint) const;
    quint64 baseState() const {
        return m_runtime.hostHistoryState();
    }
    bool retainsBaseHistoryState(quint64 state) const {
        return state == 0 || state == baseState() || m_crops.contains(state);
    }
    QList<SnowCanvasBaseImageSource> baseImageSources() const;
    static Crop prepareCrop(const QImage& image, CropAxis axis, int first, int last,
                            const std::function<bool()>& cancelled = {});
    bool applyCrop(Crop crop);
    bool crop(CropAxis axis, int first, int last);
    struct HistoryTransition {
        bool redo = false;
        quint64 state = 0;
        quint64 nextState = 0;
        quint64 revision = 0;
        qint64 sourceKey = 0;
        QImage image;
        Crop crop;
    };
    HistoryTransition historyTransition(bool redo) const;
    static void prepareHistoryTransition(HistoryTransition& transition,
                                         const std::function<bool()>& cancelled = {});
    bool applyHistoryTransition(HistoryTransition transition);
    bool undo();
    bool redo();
    QByteArray checkpoint() const;
    bool dirty() const;
    void markExported(QByteArray checkpoint);
    ImageEditingSource snapshot() const;
    ScreenshotExportSource exportSource(QString* error = nullptr) const;

  signals:
    void changed();
    void baseImageChanged();

  private:
    struct CropEntry {
        quint64 before = 0;
        Crop crop;
    };
    bool travel(bool redo);
    void updateSources();
    void pruneCrops();
    SnowCanvasRuntime m_runtime;
    QImage m_image;
    QList<SnowCanvasBaseImageSource> m_samplingSources;
    QString m_originalFileName;
    QPointF m_origin;
    QSizeF m_unitsPerPixel;
    QHash<quint64, CropEntry> m_crops;
    quint64 m_serial = 0;
    QByteArray m_saved;
    mutable QByteArray m_checkpointContent;
    mutable QByteArray m_checkpoint;
    mutable quint64 m_checkpointBaseState = 0;
    struct ExportCache {
        QByteArray checkpoint;
        QByteArray smartEraseKey;
        ScreenshotExportSource source;
    };
    mutable ExportCache m_exportCache;
};
} // namespace snow_shot::presentation
