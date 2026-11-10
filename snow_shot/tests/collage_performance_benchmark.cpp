// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "theme/theme_manager.h"
#include "snowimageqtcodec.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_thread([this] {
              while (!m_stop.load()) {
                  const auto snapshot = snow::test_support::memorySnapshot();
                  m_resident.store(std::max(m_resident.load(), quint64(snapshot.residentBytes)));
                  m_footprint.store(std::max(m_footprint.load(), quint64(snapshot.footprintBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
          }) {}
    ~MemorySampler() {
        m_stop.store(true);
        m_thread.join();
    }
    quint64 resident() const {
        return m_resident.load();
    }
    quint64 footprint() const {
        return m_footprint.load();
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_resident{0};
    std::atomic<quint64> m_footprint{0};
    std::thread m_thread;
};

double elapsedMilliseconds(const QElapsedTimer& timer) {
    return double(timer.nsecsElapsed()) / 1000000;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(std::ceil(fraction * double(values.size())) - 1);
    return values[std::min(index, values.size() - 1)];
}

void latencyReport(QJsonObject& report, const QString& prefix, const std::vector<double>& values) {
    report.insert(prefix + QStringLiteral("_samples"), static_cast<int>(values.size()));
    report.insert(prefix + QStringLiteral("_p50_ms"), percentile(values, .50));
    report.insert(prefix + QStringLiteral("_p95_ms"), percentile(values, .95));
}

void memoryReport(QJsonObject& report, const QString& prefix,
                  const snow::test_support::MemorySnapshot& baseline,
                  const snow::test_support::MemorySnapshot& after, const MemorySampler& sampler) {
    const quint64 resident = std::max(
        {sampler.resident(), quint64(baseline.residentBytes), quint64(after.residentBytes)});
    const quint64 footprint = std::max(
        {sampler.footprint(), quint64(baseline.footprintBytes), quint64(after.footprintBytes)});
    report.insert(prefix + QStringLiteral("_baseline_resident_bytes"),
                  double(baseline.residentBytes));
    report.insert(prefix + QStringLiteral("_baseline_footprint_bytes"),
                  double(baseline.footprintBytes));
    report.insert(prefix + QStringLiteral("_peak_sampled_resident_bytes"), double(resident));
    report.insert(prefix + QStringLiteral("_peak_sampled_footprint_bytes"), double(footprint));
    report.insert(prefix + QStringLiteral("_peak_resident_delta_bytes"),
                  double(resident - baseline.residentBytes));
    report.insert(prefix + QStringLiteral("_peak_footprint_delta_bytes"),
                  double(footprint - baseline.footprintBytes));
}

void mouse(QWidget* widget, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, widget->mapToGlobal(point.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void wheel(QWidget* widget, QPointF point, int delta) {
    QWheelEvent event(point, widget->mapToGlobal(point.toPoint()), {}, QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(widget, &event);
}

class ViewportSampler final {
  public:
    explicit ViewportSampler(ImageStitchingPreviewCanvas* canvas) : m_canvas(canvas) {}

    void render() {
        const qreal ratio = m_canvas->devicePixelRatioF();
        const QSize size(qCeil(m_canvas->width() * ratio), qCeil(m_canvas->height() * ratio));
        if (m_frame.size() != size || m_frame.devicePixelRatio() != ratio) {
            m_frame = QImage(size, QImage::Format_ARGB32_Premultiplied);
            require(!m_frame.isNull(), "benchmark viewport render allocation failed");
            m_frame.setDevicePixelRatio(ratio);
        }
        m_canvas->render(&m_frame);
    }

    double interact() {
        QElapsedTimer timer;
        timer.start();
        const QPointF center(m_canvas->width() / 2.0, m_canvas->height() / 2.0);
        const int direction = m_sequence++ % 2 == 0 ? 1 : -1;
        wheel(m_canvas, center, direction * 120);
        mouse(m_canvas, QEvent::MouseButtonPress, center, Qt::MiddleButton, Qt::MiddleButton);
        // Opposite frames undo both the pointer zoom and its scaled pan translation.
        const qreal pan = direction > 0 ? 1.0 : -1.0 / 1.15;
        const QPointF next = center + QPointF(pan * 3, pan * 2);
        mouse(m_canvas, QEvent::MouseMove, next, Qt::NoButton, Qt::MiddleButton);
        mouse(m_canvas, QEvent::MouseButtonRelease, next, Qt::MiddleButton, Qt::NoButton);
        QApplication::processEvents(QEventLoop::AllEvents);
        render();
        return elapsedMilliseconds(timer);
    }

    std::vector<double> sample(int count) {
        std::vector<double> result;
        result.reserve(size_t(count));
        for (int index = 0; index < count; ++index)
            result.push_back(interact());
        return result;
    }

  private:
    ImageStitchingPreviewCanvas* m_canvas;
    QImage m_frame;
    int m_sequence = 0;
};

void drainPreviewWork() {
    QElapsedTimer timer;
    timer.start();
    while (ScreenshotExportCoordinator::shared().pendingJobCount() > 0 && timer.elapsed() < 15000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(ScreenshotExportCoordinator::shared().pendingJobCount() == 0,
            "benchmark preview workers did not retire");
    // Completion delivery and work captures retire independently from the pending counter.
    QApplication::processEvents(QEventLoop::AllEvents);
}

double runPhase(ImageStitchingEditorWidget& editor, ViewportSampler& viewport,
                ScreenshotExportCoordinator::Work work, std::vector<double>& guiSamples,
                QJsonObject* report = nullptr, const QString& prefix = {},
                QString* failure = nullptr) {
    struct Completion final {
        bool done = false;
        ScreenshotExportTaskResult result;
    };
    auto completion = std::make_shared<Completion>();
    const auto baseline = snow::test_support::memorySnapshot();
    auto sampler = report ? std::make_unique<MemorySampler>() : nullptr;
    QElapsedTimer timer;
    timer.start();
    const auto job = ScreenshotExportCoordinator::shared().submit(
        &editor, ScreenshotExportCoordinator::Priority::Foreground, std::move(work),
        [completion](ScreenshotExportTaskResult result) {
            completion->result = std::move(result);
            completion->done = true;
        });
    require(job.isValid(), "benchmark export worker submission failed");
    qint64 nextFrame = 0;
    while (!completion->done && timer.elapsed() < 600000) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (timer.elapsed() >= nextFrame) {
            guiSamples.push_back(viewport.interact());
            nextFrame = timer.elapsed() + 16;
        }
        QThread::msleep(1);
    }
    if (!completion->done) {
        job.cancel();
        throw std::runtime_error("benchmark export worker timed out");
    }
    if (report)
        memoryReport(*report, prefix, baseline, snow::test_support::memorySnapshot(), *sampler);
    if (!completion->result.succeeded()) {
        if (failure)
            *failure = completion->result.error;
        else
            throw std::runtime_error(completion->result.error.toStdString());
    }
    return elapsedMilliseconds(timer);
}

QString writeFixture(const QString& directory) {
    QImage image = snowCanvasAllocateImage({3840, 2160}, QImage::Format_RGB32);
    require(!image.isNull(), "fixture image allocation failed");
    for (int y = 0; y < image.height(); ++y) {
        auto* pixels = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            pixels[x] =
                qRgb((x * 7 + y * 13) % 256, (x * 17 + y * 3) % 256, (x * 11 + y * 19) % 256);
    }
    const QString path = directory + QStringLiteral("/fixture.png");
    require(
        ScreenshotImageFileService::write(image, path, ScreenshotImageFileFormat::Png).succeeded(),
        "fixture image write failed");
    return path;
}

QJsonObject scenario(int count, const QString& input, const QString& directory, bool transformed,
                     ImageStitchingDirection direction) {
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler sampler;
    QJsonObject result;
    result.insert(QStringLiteral("images"), count);
    result.insert(QStringLiteral("image_width"), 3840);
    result.insert(QStringLiteral("image_height"), 2160);
    result.insert(QStringLiteral("direction"), direction == ImageStitchingDirection::Vertical
                                                   ? QStringLiteral("vertical")
                                                   : QStringLiteral("horizontal"));
    result.insert(QStringLiteral("transformed"), transformed);

    {
        QElapsedTimer firstPreview;
        firstPreview.start();
        ImageStitchingDocument document;
        ImageStitchingEditorWidget editor(&document);
        editor.resize(1200, 700);
        editor.show();
        QApplication::processEvents(QEventLoop::AllEvents);
        ViewportSampler viewport(editor.canvas());
        viewport.render();
        result.insert(QStringLiteral("editor_initialization_ms"),
                      elapsedMilliseconds(firstPreview));
        result.insert(QStringLiteral("editor_width"), editor.width());
        result.insert(QStringLiteral("editor_height"), editor.height());
        result.insert(QStringLiteral("device_pixel_ratio"), editor.devicePixelRatioF());

        editor.setLoading(true);
        const auto loadingBaseline = snow::test_support::memorySnapshot();
        auto loadingSampler = std::make_unique<MemorySampler>();
        std::vector<double> loadingGui;
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < count; ++index) {
            struct Import final {
                QImage image;
                QImage preview;
                std::shared_ptr<ImageStitchingImportReservation> reservation;
            };
            auto imported = std::make_shared<Import>();
            static_cast<void>(runPhase(
                editor, viewport,
                [imported, input](const ScreenshotExportCancellation& cancellation) {
                    QString error;
                    imported->reservation = reserveImageStitchingImport(96 * 1024 * 1024, &error);
                    if (!imported->reservation)
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Source, error);
                    imported->image =
                        snow_shot::image_codec::decodeFile(input, snow::image::Format::png);
                    if (cancellation.isCancellationRequested())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled,
                            QStringLiteral("Benchmark cancelled"));
                    if (imported->image.isNull())
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Source,
                            QStringLiteral("Benchmark import failed"));
                    imported->preview =
                        snowCanvasScaleImage(imported->image, QSize(256, 256), Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation);
                    return ScreenshotExportTaskResult{};
                },
                loadingGui));
            const auto id = document.append(std::move(imported->image), QString::number(index),
                                            std::move(imported->preview));
            require(id != 0, "benchmark source resource admission failed");
            imported.reset();
            QApplication::processEvents(QEventLoop::AllEvents);
            editor.canvas()->fitToCanvas();
            viewport.render();
            if (index == 0) {
                require(editor.canvas()->imageViewportRect(id).intersects(editor.canvas()->rect()),
                        "benchmark first preview must be visible");
                result.insert(QStringLiteral("first_preview_ms"),
                              elapsedMilliseconds(firstPreview));
            }
        }
        editor.setLoading(false);
        result.insert(QStringLiteral("load_ms"), elapsedMilliseconds(timer));
        latencyReport(result, QStringLiteral("gui_during_loading"), loadingGui);
        memoryReport(result, QStringLiteral("load"), loadingBaseline,
                     snow::test_support::memorySnapshot(), *loadingSampler);
        loadingSampler.reset();
        auto settings = document.settings();
        settings.direction = direction;
        document.setSettings(settings);

        timer.restart();
        for (qsizetype index = 0; index < document.items().size(); ++index) {
            const quint64 id = document.items()[index].id;
            if (transformed) {
                if (index % 2 == 0)
                    document.rotate(id, true);
                document.flip(id, true);
            }
        }
        result.insert(QStringLiteral("initial_transforms_ms"), elapsedMilliseconds(timer));

        std::vector<double> editLatencies;
        const auto edit = [&](const std::function<void()>& action) {
            QElapsedTimer elapsed;
            elapsed.start();
            action();
            QApplication::processEvents(QEventLoop::AllEvents);
            viewport.render();
            editLatencies.push_back(elapsedMilliseconds(elapsed));
        };
        for (int index = 0; index < 16; ++index) {
            const quint64 first = document.items().front().id;
            const quint64 last = document.items().back().id;
            edit([&] { document.rotate(first, true); });
            edit([&] { document.rotate(first, false); });
            edit([&] { document.flip(first, true); });
            edit([&] { document.flip(first, true); });
            if (count > 1) {
                edit([&] { document.swapItems(first, last); });
                edit([&] { document.swapItems(first, last); });
            }
        }
        latencyReport(result, QStringLiteral("edit_and_repaint"), editLatencies);

        timer.restart();
        for (int iteration = 0; iteration < 1000; ++iteration) {
            require(document.layout().isValid(), "benchmark layout failed");
        }
        result.insert(QStringLiteral("1000_layouts_ms"), elapsedMilliseconds(timer));
        editor.canvas()->fitToCanvas();
        QApplication::processEvents(QEventLoop::AllEvents);
        viewport.render();
        latencyReport(result, QStringLiteral("viewport_fit"), viewport.sample(96));

        const quint64 first = document.items().front().id;
        editor.canvas()->centerImage(first);
        const auto firstRect = document.layout().rects.front();
        const qreal wanted = std::min(qreal(editor.canvas()->width() - 48) / firstRect.width(),
                                      qreal(editor.canvas()->height() - 48) / firstRect.height());
        const qreal steps = std::log(wanted / editor.canvas()->zoom()) / std::log(1.15);
        const QPointF center(editor.canvas()->width() / 2.0, editor.canvas()->height() / 2.0);
        wheel(editor.canvas(), center, qRound(steps * 120));
        QElapsedTimer detailWait;
        detailWait.start();
        while (editor.canvas()->property("detailTileCount").toInt() == 0 &&
               detailWait.elapsed() < 10000) {
            QApplication::processEvents(QEventLoop::AllEvents, 5);
            viewport.render();
            QThread::msleep(1);
        }
        require(editor.canvas()->property("detailTileCount").toInt() > 0,
                "benchmark zoomed viewport must obtain full-detail tiles");
        result.insert(QStringLiteral("first_detail_tile_ms"), elapsedMilliseconds(detailWait));
        latencyReport(result, QStringLiteral("viewport_detail"), viewport.sample(96));
        const auto steady = snow::test_support::memorySnapshot();
        result.insert(QStringLiteral("steady_resident_bytes"), double(steady.residentBytes));
        result.insert(QStringLiteral("steady_footprint_bytes"), double(steady.footprintBytes));

        editor.setExportBusy(true);
        auto rows = std::make_shared<ScreenshotImageRowSource>();
        std::vector<double> prepareGui;
        const double preparation = runPhase(
            editor, viewport,
            [snapshot = document.items(), settings,
             rows](const ScreenshotExportCancellation& cancellation) {
                QString error;
                *rows = prepareImageStitchingRows(
                    snapshot, settings,
                    [cancellation] { return cancellation.isCancellationRequested(); }, &error);
                return rows->isValid() ? ScreenshotExportTaskResult{}
                                       : ScreenshotExportTaskResult::failure(
                                             ScreenshotExportFailureStage::Render, error);
            },
            prepareGui, &result, QStringLiteral("prepare"));
        result.insert(QStringLiteral("prepare_ms"), preparation);
        latencyReport(result, QStringLiteral("gui_during_prepare"), prepareGui);
        result.insert(QStringLiteral("output_width"), rows->size.width());
        result.insert(QStringLiteral("output_height"), rows->size.height());

        auto checksum = std::make_shared<quint64>(0);
        std::vector<double> composeGui;
        const double composition = runPhase(
            editor, viewport,
            [rows, checksum](const ScreenshotExportCancellation& cancellation) {
                const qsizetype stride = qsizetype(rows->size.width()) * 4;
                const int batch = static_cast<int>(std::max<qsizetype>(
                    1, std::min<qsizetype>(64, qsizetype(16 * 1024 * 1024) / stride)));
                QByteArray buffer(stride * batch, Qt::Uninitialized);
                for (int firstRow = 0; firstRow < rows->size.height(); firstRow += batch) {
                    const int rowCount = std::min(batch, rows->size.height() - firstRow);
                    if (cancellation.isCancellationRequested() ||
                        !rows->readRows(firstRow, rowCount, stride,
                                        reinterpret_cast<uchar*>(buffer.data()), buffer.size()))
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Render,
                            QStringLiteral("Benchmark row production failed"));
                    *checksum = *checksum * 131 + static_cast<uchar>(buffer[0]) +
                                static_cast<uchar>(buffer[(rowCount - 1) * stride]);
                }
                return ScreenshotExportTaskResult{};
            },
            composeGui, &result, QStringLiteral("compose"));
        result.insert(QStringLiteral("compose_rows_ms"), composition);
        result.insert(QStringLiteral("row_checksum"), QString::number(*checksum));
        latencyReport(result, QStringLiteral("gui_during_compose"), composeGui);

        const QString output = directory + QStringLiteral("/output.png");
        auto saved = std::make_shared<ScreenshotImageFileSaveResult>();
        std::vector<double> encodeGui;
        const double encoding = runPhase(
            editor, viewport,
            [rows, output, saved](const ScreenshotExportCancellation& cancellation) {
                auto inputRows = *rows;
                inputRows.cancellationRequested = [cancellation] {
                    return cancellation.isCancellationRequested();
                };
                *saved = ScreenshotImageFileService::write(
                    inputRows, output, ScreenshotImageFileFormat::Png, {},
                    {100, ScreenshotCompressionLevel::Medium});
                return saved->succeeded() ? ScreenshotExportTaskResult{}
                                          : ScreenshotExportTaskResult::failure(
                                                ScreenshotExportFailureStage::File, saved->error);
            },
            encodeGui, &result, QStringLiteral("encode"));
        result.insert(QStringLiteral("save_png_ms"), encoding);
        result.insert(QStringLiteral("encoded_bytes"), double(saved->encodedByteCount));
        latencyReport(result, QStringLiteral("gui_during_encode"), encodeGui);
        require(QFile::remove(output), "benchmark output cleanup failed");

        const qint64 outputPixels = qint64(rows->size.width()) * rows->size.height();
        const auto clipboardEstimate = estimateImageStitchingClipboard(rows->size);
        result.insert(QStringLiteral("output_raw_rgba_bytes"), double(outputPixels * 4));
        result.insert(QStringLiteral("clipboard_capacity_reservation_bytes"),
                      double(clipboardEstimate.bytes));
        result.insert(QStringLiteral("clipboard_dib_pixel_bytes"),
                      double(clipboardEstimate.dibBytes));
        result.insert(QStringLiteral("clipboard_row_scratch_bytes"),
                      double(clipboardEstimate.rowScratchBytes));
        result.insert(QStringLiteral("clipboard_published"), false);
        result.insert(
            QStringLiteral("fixture_description"),
            QStringLiteral(
                "Opaque patterned RGB; independently decoded copies of one encoded fixture"));
        result.insert(QStringLiteral("png_compression_level"),
                      ScreenshotImageFileService::encodeOptions(
                          ScreenshotImageFileFormat::Png, {100, ScreenshotCompressionLevel::Medium})
                          .compression_level);
        if (transformed) {
            result.insert(QStringLiteral("clipboard_prepare_status"), QStringLiteral("skipped"));
            result.insert(
                QStringLiteral("clipboard_prepare_reason"),
                !clipboardEstimate.isValid()
                    ? clipboardEstimate.error
                    : QStringLiteral(
                          "Clipboard preparation is measured for unchanged fixtures only"));
        } else {
            struct ClipboardPreparation final {
                bool admitted = false;
                bool valid = false;
                qsizetype pngBytes = 0;
            };
            auto prepared = std::make_shared<ClipboardPreparation>();
            QString failure;
            std::vector<double> clipboardGui;
            const double clipboard = runPhase(
                editor, viewport,
                [rows, prepared](const ScreenshotExportCancellation& cancellation) {
                    QString error;
                    auto reservation = reserveImageStitchingClipboard(rows->size, &error);
                    if (!reservation)
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Clipboard, error);
                    prepared->admitted = true;
                    auto inputRows = *rows;
                    inputRows.cancellationRequested = [cancellation] {
                        return cancellation.isCancellationRequested();
                    };
                    auto payload = ScreenshotClipboardService::prepare(
                        inputRows, {100, ScreenshotCompressionLevel::Medium});
                    prepared->valid = payload.isValid();
                    prepared->pngBytes = payload.pngBytes().size();
                    // Native publication is deliberately excluded: releasing the payload on
                    // this worker frees its DIB and PNG handles without changing OS clipboard.
                    payload = {};
                    return prepared->valid
                               ? ScreenshotExportTaskResult{}
                               : ScreenshotExportTaskResult::failure(
                                     ScreenshotExportFailureStage::Clipboard,
                                     QStringLiteral("Native clipboard payload preparation failed"));
                },
                clipboardGui, &result, QStringLiteral("clipboard_prepare"), &failure);
            result.insert(QStringLiteral("clipboard_prepare_ms"), clipboard);
            result.insert(QStringLiteral("clipboard_png_bytes"), double(prepared->pngBytes));
            result.insert(QStringLiteral("clipboard_png_to_raw_ratio"),
                          double(prepared->pngBytes) / double(outputPixels * 4));
            result.insert(QStringLiteral("clipboard_prepare_status"),
                          prepared->valid      ? QStringLiteral("succeeded")
                          : prepared->admitted ? QStringLiteral("failed")
                                               : QStringLiteral("skipped"));
            if (!failure.isEmpty())
                result.insert(QStringLiteral("clipboard_prepare_reason"), failure);
            latencyReport(result, QStringLiteral("gui_during_clipboard_prepare"), clipboardGui);
        }
        result.insert(QStringLiteral("reserved_bytes"),
                      double(imageStitchingResourceUsage().reservedBytes));
    }
    drainPreviewWork();
    QElapsedTimer retirement;
    retirement.start();
    while (imageStitchingResourceUsage().reservedBytes != 0 && retirement.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    const auto after = snow::test_support::memorySnapshot();
    result.insert(QStringLiteral("peak_sampled_resident_bytes"), double(sampler.resident()));
    result.insert(QStringLiteral("peak_sampled_footprint_bytes"), double(sampler.footprint()));
    result.insert(QStringLiteral("baseline_resident_bytes"), double(baseline.residentBytes));
    result.insert(QStringLiteral("baseline_footprint_bytes"), double(baseline.footprintBytes));
    result.insert(QStringLiteral("after_close_resident_bytes"), double(after.residentBytes));
    result.insert(QStringLiteral("after_close_footprint_bytes"), double(after.footprintBytes));
    require(imageStitchingResourceUsage().reservedBytes == 0,
            "benchmark leaked collage reservations");
    return result;
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    adqt::theme::ThemeManager::instance().applyTo(application);
    const QStringList arguments = QCoreApplication::arguments();
    const auto option = [&](const QString& name) {
        const qsizetype index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : QString{};
    };
    QTemporaryDir directory;
    try {
        require(directory.isValid(), "benchmark temporary directory unavailable");
        const QString fixture = writeFixture(directory.path());
        QJsonArray results;
        const int requested = option(QStringLiteral("--images")).toInt();
        const QVector<int> counts = requested > 0 ? QVector<int>{requested} : QVector<int>{20, 100};
        for (int count : counts) {
            for (const auto direction :
                 {ImageStitchingDirection::Vertical, ImageStitchingDirection::Horizontal}) {
                for (bool transformed : {false, true}) {
                    auto result =
                        scenario(count, fixture, directory.path(), transformed, direction);
                    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
                              << '\n';
                    std::cout.flush();
                    results.append(result);
                }
            }
        }
        const QString output = option(QStringLiteral("--output"));
        if (!output.isEmpty()) {
            QFile file(output);
            require(file.open(QIODevice::WriteOnly), "benchmark report unavailable");
            const QByteArray report = QJsonDocument(results).toJson(QJsonDocument::Indented);
            require(file.write(report) == report.size(), "benchmark report write failed");
        }
    } catch (const std::exception& error) {
        ScreenshotExportCoordinator::shared().shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
    ScreenshotExportCoordinator::shared().shutdown();
    return 0;
}
