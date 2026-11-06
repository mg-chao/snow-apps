// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"
#include "snow_shot/presentation/components/imagespliteditorwidget.h"
#include "snow_shot/presentation/imagesplitwindow.h"
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
#include "imagesplitimagepreparation_p.h"
#include "imagesplit_smart_reference.h"
#include "widgets/input_number.h"
#include "snow_shot/storage/applicationstorage.h"
#endif
#include "theme/theme_manager.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QColorSpace>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
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
double milliseconds(const QElapsedTimer& timer) {
    return double(timer.nsecsElapsed()) / 1'000'000;
}
double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const size_t index =
        std::min(values.size() - 1, size_t(std::ceil(double(values.size()) * fraction) - 1));
    return values.at(index);
}

void latencyReport(QJsonObject& report, const QString& prefix, const std::vector<double>& values) {
    report.insert(prefix + QStringLiteral("_samples"), int(values.size()));
    report.insert(prefix + QStringLiteral("_p50_ms"), percentile(values, .50));
    report.insert(prefix + QStringLiteral("_p95_ms"), percentile(values, .95));
    report.insert(prefix + QStringLiteral("_max_ms"),
                  values.empty() ? 0 : *std::max_element(values.begin(), values.end()));
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_peak(quint64(snow::test_support::memorySnapshot().residentBytes)),
          m_privatePeak(quint64(snow::test_support::memorySnapshot().footprintBytes)),
          m_thread([this] {
              while (!m_stop.load()) {
                  const auto snapshot = snow::test_support::memorySnapshot();
                  m_peak.store(std::max(m_peak.load(), quint64(snapshot.residentBytes)));
                  m_privatePeak.store(
                      std::max(m_privatePeak.load(), quint64(snapshot.footprintBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
          }) {}
    ~MemorySampler() {
        m_stop.store(true);
        m_thread.join();
    }
    quint64 peak() const {
        return m_peak.load();
    }
    quint64 privatePeak() const {
        return m_privatePeak.load();
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_peak;
    std::atomic<quint64> m_privatePeak;
    std::thread m_thread;
};

void mouse(QWidget* target, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, target->mapToGlobal(point.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void wheel(QWidget* target, QPointF point, int delta) {
    QWheelEvent event(point, target->mapToGlobal(point.toPoint()), {}, QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(target, &event);
}

class CanvasSampler final {
  public:
    explicit CanvasSampler(ImageSplitPreviewCanvas* canvas) : m_canvas(canvas) {}
    void render() {
        const qreal ratio = m_canvas->devicePixelRatioF();
        const QSize size(qCeil(m_canvas->width() * ratio), qCeil(m_canvas->height() * ratio));
        if (m_frame.size() != size || m_frame.devicePixelRatio() != ratio) {
            m_frame = QImage(size, QImage::Format_ARGB32_Premultiplied);
            require(!m_frame.isNull(), "canvas benchmark render target allocation succeeds");
            m_frame.setDevicePixelRatio(ratio);
        }
        m_canvas->render(&m_frame);
    }
    std::vector<double> samplePaint(int count) {
        std::vector<double> values;
        values.reserve(size_t(count));
        for (int index = 0; index < count; ++index) {
            QElapsedTimer timer;
            timer.start();
            render();
            values.push_back(milliseconds(timer));
        }
        return values;
    }
    std::vector<double> samplePan(int count) {
        std::vector<double> values;
        values.reserve(size_t(count));
        const QPointF center = QRectF(m_canvas->rect()).center();
        mouse(m_canvas, QEvent::MouseButtonPress, center, Qt::MiddleButton, Qt::MiddleButton);
        for (int index = 0; index < count; ++index) {
            QElapsedTimer timer;
            timer.start();
            const QPointF next = center + (index % 2 == 0 ? QPointF(12, 8) : QPointF{});
            mouse(m_canvas, QEvent::MouseMove, next, Qt::NoButton, Qt::MiddleButton);
            render();
            values.push_back(milliseconds(timer));
        }
        mouse(m_canvas, QEvent::MouseButtonRelease, center, Qt::MiddleButton, Qt::NoButton);
        return values;
    }
    std::vector<double> sampleZoom(int count) {
        std::vector<double> values;
        values.reserve(size_t(count));
        const QPointF center = QRectF(m_canvas->rect()).center();
        for (int index = 0; index < count; ++index) {
            QElapsedTimer timer;
            timer.start();
            wheel(m_canvas, center, index % 2 == 0 ? 120 : -120);
            render();
            values.push_back(milliseconds(timer));
        }
        return values;
    }

  private:
    ImageSplitPreviewCanvas* m_canvas;
    QImage m_frame;
};

class PaintCounter final : public QObject {
  public:
    int count = 0;
    qint64 area = 0;
    void reset() {
        count = 0;
        area = 0;
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            ++count;
            for (const auto& rect : static_cast<QPaintEvent*>(event)->region())
                area += qint64(rect.width()) * rect.height();
        }
        return false;
    }
};

ImageSplitSettings equalSplitSettings() {
    ImageSplitSettings settings;
    settings.type = ImageSplitType::Equal;
    return settings;
}

void hoverReport(QJsonObject& report, const QImage& image, const QImage& preview) {
    ImageSplitDocument document;
    document.setImage(image, preview, equalSplitSettings());
    ImageSplitEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    auto* canvas = editor.canvas();
    canvas->fitToCanvas();
    QCoreApplication::processEvents();
    PaintCounter counter;
    canvas->installEventFilter(&counter);
    const auto point = canvas->imageToViewport({image.width() * .25, image.height() * .25});
    mouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
    QCoreApplication::processEvents();
    counter.reset();
    std::vector<double> values;
    for (int index = 0; index < 128; ++index) {
        QElapsedTimer elapsed;
        elapsed.start();
        mouse(canvas, QEvent::MouseMove, point + QPointF(index % 2, index % 3), Qt::NoButton,
              Qt::NoButton);
        QCoreApplication::processEvents();
        values.push_back(milliseconds(elapsed));
    }
    latencyReport(report, QStringLiteral("unchanged_hover"), values);
    report.insert(QStringLiteral("unchanged_hover_paints"), counter.count);
    report.insert(QStringLiteral("unchanged_hover_damage_pixels"), double(counter.area));
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
    require(counter.count == 0, "unchanged hover must not repaint the canvas");
#endif
    canvas->removeEventFilter(&counter);
}

void historyReport(QJsonObject& report, int operations) {
    ImageSplitDocument document;
    document.setImage(QImage(operations * 4 + 4, 1, QImage::Format_RGBA8888), {},
                      equalSplitSettings());
    auto settings = document.settings();
    settings.count = document.image().width();
    require(document.setSettings(settings), "fragmented history fixture is valid");
    const auto original = document.state();
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    std::vector<double> values;
    qint64 retained = document.state().verticalLines.storageBytes();
    for (int index = 1; index <= operations; ++index) {
        const int coordinate = index * 4;
        const auto lines = document.state().verticalLines;
        const auto found = std::lower_bound(lines.cbegin(), lines.cend(), coordinate);
        require(found != lines.cend() && *found == coordinate, "spaced guide exists");
        QElapsedTimer elapsed;
        elapsed.start();
        require(document.removeLine(true, int(found - lines.cbegin())), "spaced deletion succeeds");
        values.push_back(milliseconds(elapsed));
        retained += document.state().verticalLines.storageBytes();
    }
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
    retained = document.historyStorageBytes();
    require(retained < qint64(operations) * 4096,
            "fragmented history retains shared paths within a linear-logarithmic budget");
#endif
    const QString prefix = QStringLiteral("history_%1").arg(operations);
    latencyReport(report, prefix + QStringLiteral("_delete"), values);
    report.insert(prefix + QStringLiteral("_retained_storage_bytes"), double(retained));
    report.insert(prefix + QStringLiteral("_peak_resident_delta_bytes"),
                  double(memory.peak() - baseline.residentBytes));
    report.insert(prefix + QStringLiteral("_peak_private_delta_bytes"),
                  double(memory.privatePeak() - baseline.footprintBytes));
    const auto edited = document.state();
    for (int index = 0; index < operations; ++index)
        document.undo();
    require(document.state() == original, "all fragmented edits undo exactly");
    for (int index = 0; index < operations; ++index)
        document.redo();
    require(document.state() == edited, "all fragmented edits redo exactly");
}

void previewReport(QJsonObject& report, const QImage& image,
                   const QString& prefix = QStringLiteral("preview")) {
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    QElapsedTimer elapsed;
    elapsed.start();
#ifdef SNOW_SHOT_IMAGE_SPLIT_BASELINE
    const auto preview = image.scaled({2048, 2048}, Qt::KeepAspectRatio, Qt::SmoothTransformation);
#else
    const auto preview = imageSplitPreview(image);
#endif
    const QSize expected =
        image.size().scaled(2048, 2048, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    require(!preview.isNull() && preview.size() == expected, "large preview is complete");
    report.insert(prefix + QStringLiteral("_prepare_ms"), milliseconds(elapsed));
    report.insert(prefix + QStringLiteral("_peak_resident_delta_bytes"),
                  double(memory.peak() - baseline.residentBytes));
    report.insert(prefix + QStringLiteral("_peak_private_delta_bytes"),
                  double(memory.privatePeak() - baseline.footprintBytes));
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
    require(memory.peak() - baseline.residentBytes < 64 * 1024 * 1024,
            "preview preparation does not allocate a full source raster");
#endif
}

void loadReport(QJsonObject& report, const QImage& image) {
    const QImage native = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    ImageSplitWindowServices services;
    services.readHistory = [native](const auto&) -> std::optional<QImage> { return native; };
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    QElapsedTimer elapsed;
    elapsed.start();
    ImageSplitWindow window(nullptr, std::move(services));
    bool complete = false;
    bool succeeded = false;
    QObject::connect(&window, &ImageSplitWindow::imageLoaded, [&](bool success) {
        succeeded = success;
        complete = true;
    });
    window.presentHistory({});
    while (!complete && elapsed.elapsed() < 30000) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(complete && succeeded, "native history source loads through the real window worker");
    report.insert(QStringLiteral("native_history_load_ms"), milliseconds(elapsed));
    report.insert(QStringLiteral("native_history_source_shared"),
                  window.document()->image().constBits() == native.constBits());
    report.insert(QStringLiteral("native_history_peak_resident_delta_bytes"),
                  double(memory.peak() - baseline.residentBytes));
    report.insert(QStringLiteral("native_history_peak_private_delta_bytes"),
                  double(memory.privatePeak() - baseline.footprintBytes));
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
    require(window.document()->image().constBits() == native.constBits(),
            "history loading shares immutable native pixels");
    require(memory.peak() - baseline.residentBytes < 96 * 1024 * 1024,
            "native loading avoids normalized and premultiplied full-size temporaries");
#endif
    window.shutdown();
}

void canvasReport(QJsonObject& report, const QImage& image, const QImage& preview) {
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    ImageSplitDocument document;
    document.setImage(image, preview, equalSplitSettings());
    ImageSplitEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    QApplication::processEvents(QEventLoop::AllEvents);
    auto* canvas = editor.canvas();
    canvas->fitToCanvas();
    CanvasSampler sampler(canvas);
    sampler.render();
    report.insert(QStringLiteral("canvas_width"), canvas->width());
    report.insert(QStringLiteral("canvas_height"), canvas->height());
    report.insert(QStringLiteral("canvas_device_pixel_ratio"), canvas->devicePixelRatioF());
    report.insert(QStringLiteral("canvas_preview_width"), preview.width());
    report.insert(QStringLiteral("canvas_preview_height"), preview.height());
    latencyReport(report, QStringLiteral("canvas_fit_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_fit_pan_and_paint"), sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_fit_zoom_and_paint"), sampler.sampleZoom(96));
    ImageSplitSettings settings;
    settings.type = ImageSplitType::Equal;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 1000;
    settings.columns = 1000;
    require(document.setSettings(settings), "dense canvas benchmark grid is accepted");
    canvas->fitToCanvas();
    sampler.render();
    const auto before = document.state();
    report.insert(QStringLiteral("canvas_dense_grid_outputs"),
                  double(document.layout().outputCount()));
    latencyReport(report, QStringLiteral("canvas_dense_fit_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_dense_fit_pan_and_paint"), sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_dense_fit_zoom_and_paint"),
                  sampler.sampleZoom(96));
    const QPointF center = QRectF(canvas->rect()).center();
    const qreal steps = std::log(1.0 / canvas->zoom()) / std::log(1.15);
    wheel(canvas, center, qRound(steps * 120));
    sampler.render();
    report.insert(QStringLiteral("canvas_detail_zoom"), canvas->zoom());
    latencyReport(report, QStringLiteral("canvas_dense_detail_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_dense_detail_pan_and_paint"),
                  sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_dense_detail_zoom_and_paint"),
                  sampler.sampleZoom(96));
    require(document.state() == before, "canvas pan, zoom and painting preserve all editing state");
    require(!document.canRedo(), "viewport interactions do not introduce redo operations");
    const auto after = snow::test_support::memorySnapshot();
    report.insert(QStringLiteral("canvas_baseline_resident_bytes"), double(baseline.residentBytes));
    report.insert(QStringLiteral("canvas_peak_sampled_resident_bytes"), double(memory.peak()));
    report.insert(QStringLiteral("canvas_peak_resident_delta_bytes"),
                  double(memory.peak() - quint64(baseline.residentBytes)));
    report.insert(QStringLiteral("canvas_after_resident_bytes"), double(after.residentBytes));
}

void extremeCanvasReport(QJsonObject& report, const QImage& source) {
    // Reinterpret the existing contiguous 64MP fixture as an elongated source. Its lifetime spans
    // this call, so the benchmark needs no second full-resolution allocation or preparation copy.
    require(source.bytesPerLine() == qsizetype(source.width()) * 4,
            "extreme canvas fixture has contiguous RGBA pixels");
    const int width = source.width() * source.height();
    const QImage image(source.constBits(), width, 1, source.sizeInBytes(), QImage::Format_RGBA8888);
    require(!image.isNull(), "extreme canvas fixture aliases the prepared source");
    const QImage preview = image.scaled({2048, 1}, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    require(!preview.isNull(), "extreme canvas preview is prepared before measurements");
    ImageSplitDocument document;
    document.setImage(image, preview, equalSplitSettings());
    ImageSplitEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    QApplication::processEvents(QEventLoop::AllEvents);
    ImageSplitSettings settings;
    settings.type = ImageSplitType::Equal;
    settings.count = width;
    QElapsedTimer configuration;
    configuration.start();
    require(document.setSettings(settings), "extreme canvas equal split accepts every pixel");
    report.insert(QStringLiteral("canvas_extreme_configuration_ms"), milliseconds(configuration));
    auto* canvas = editor.canvas();
    canvas->fitToCanvas();
    CanvasSampler sampler(canvas);
    sampler.render();
    const auto original = document.state();
    report.insert(QStringLiteral("canvas_extreme_source_width"), width);
    report.insert(QStringLiteral("canvas_extreme_source_height"), 1);
    report.insert(QStringLiteral("canvas_extreme_outputs"),
                  double(document.layout().outputCount()));
    report.insert(QStringLiteral("canvas_extreme_guide_storage_bytes"),
                  double(document.layout().verticalLines.storageBytes()));
    latencyReport(report, QStringLiteral("canvas_extreme_fit_paint"), sampler.samplePaint(96));
    latencyReport(report, QStringLiteral("canvas_extreme_fit_pan_and_paint"),
                  sampler.samplePan(96));
    latencyReport(report, QStringLiteral("canvas_extreme_fit_zoom_and_paint"),
                  sampler.sampleZoom(96));
    require(document.state() == original, "extreme viewport interactions preserve guide state");
    std::vector<double> edits;
    for (int sample = 0; sample < 96; ++sample) {
        QElapsedTimer timer;
        timer.start();
        require(document.removeLine(true, width / 2 - 1), "dense manual deletion succeeds");
        document.undo();
        require(document.state() == original, "dense undo restores compact geometry");
        edits.push_back(milliseconds(timer));
    }
    latencyReport(report, QStringLiteral("canvas_extreme_delete_and_undo"), edits);
}
QImage fixture(QSize size) {
    QImage image(size, QImage::Format_RGBA8888);
    require(!image.isNull(), "benchmark source allocation succeeds");
    for (int y = 0; y < size.height(); ++y) {
        auto* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[qsizetype(x) * 4] = uchar((x * 13 + y * 7) % 256);
            row[qsizetype(x) * 4 + 1] = uchar((x * 3 + y * 17) % 256);
            row[qsizetype(x) * 4 + 2] = uchar((x * 29 + y) % 256);
            row[qsizetype(x) * 4 + 3] = uchar(64 + (x + y) % 192);
        }
    }
    return image;
}

void geometryReport(QJsonObject& report) {
    ImageSplitSettings settings;
    settings.type = ImageSplitType::Equal;
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2000;
    settings.columns = 32000;
    std::vector<double> elapsed;
    qint64 checksum = 0;
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto layout = calculateImageSplitLayout({32000, 2000}, settings);
        checksum += layout.regionAt(63'999'999).x();
        elapsed.push_back(milliseconds(timer));
        require(layout.isValid() && layout.outputCount() == 64'000'000,
                "dense-grid geometry is valid");
    }
    report.insert(QStringLiteral("dense_grid_output_count"), 64'000'000);
    report.insert(QStringLiteral("dense_grid_geometry_p50_ms"), percentile(elapsed, .50));
    report.insert(QStringLiteral("dense_grid_geometry_p95_ms"), percentile(elapsed, .95));
    const auto dense = calculateImageSplitLayout({32000, 2000}, settings);
    report.insert(
        QStringLiteral("dense_grid_boundary_bytes"),
        double(dense.verticalLines.storageBytes() + dense.horizontalLines.storageBytes()));
    report.insert(QStringLiteral("dense_grid_boundary_runs"),
                  int(dense.verticalLines.runCount() + dense.horizontalLines.runCount()));
    elapsed.clear();
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        require(dense.isValid(), "dense-grid boundary validation succeeds");
        elapsed.push_back(milliseconds(timer));
    }
    report.insert(QStringLiteral("dense_grid_validation_p95_ms"), percentile(elapsed, .95));
    QElapsedTimer lookup;
    lookup.start();
    for (qint64 index = 0; index < 100'000; ++index)
        checksum += dense.regionAt(index * 619 % dense.outputCount()).x();
    report.insert(QStringLiteral("dense_grid_100000_region_lookups_ms"), milliseconds(lookup));
    settings.direction = ImageSplitDirection::Vertical;
    settings.count = 64'000'000;
    elapsed.clear();
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto extreme = calculateImageSplitLayout({64'000'000, 1}, settings);
        require(extreme.isValid() && extreme.outputCount() == 64'000'000 &&
                    extreme.regionAt(63'999'999) == QRect(63'999'999, 0, 1, 1),
                "extreme aspect geometry is valid and reaches its final pixel");
        elapsed.push_back(milliseconds(timer));
    }
    latencyReport(report, QStringLiteral("extreme_axis_geometry"), elapsed);
    const auto extreme = calculateImageSplitLayout({64'000'000, 1}, settings);
    report.insert(QStringLiteral("extreme_axis_boundary_bytes"),
                  double(extreme.verticalLines.storageBytes()));
    report.insert(QStringLiteral("extreme_axis_boundary_runs"),
                  int(extreme.verticalLines.runCount()));
    lookup.restart();
    for (qint64 index = 0; index < 100'000; ++index)
        checksum += extreme.regionAt(index * 619 % extreme.outputCount()).x();
    report.insert(QStringLiteral("extreme_axis_100000_region_lookups_ms"), milliseconds(lookup));
    report.insert(QStringLiteral("geometry_checksum"), double(checksum));
}

void exportReport(QJsonObject& report, const QImage& image, const QString& parent,
                  const QString& prefix, ImageSplitSettings settings) {
    const auto layout = calculateImageSplitLayout(image.size(), settings);
    require(layout.isValid(), "benchmark export layout is valid");
    const auto baseline = snow::test_support::memorySnapshot();
    std::atomic_bool stop{false};
    std::atomic<quint64> peak{quint64(baseline.residentBytes)};
    std::thread sampler([&] {
        while (!stop.load()) {
            peak.store(
                std::max(peak.load(), quint64(snow::test_support::memorySnapshot().residentBytes)));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer heartbeat;
    heartbeat.start();
    std::vector<double> gaps;
    QTimer timer;
    timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        gaps.push_back(milliseconds(heartbeat));
        heartbeat.restart();
    });
    timer.start();
    auto worker = std::async(std::launch::async,
                             [&] { return exportSplitImages(image, layout, parent, prefix); });
    while (worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto result = worker.get();
    timer.stop();
    stop.store(true);
    sampler.join();
    require(result.succeeded(), "benchmark batch export succeeds");
    report.insert(prefix + QStringLiteral("_export_ms"), milliseconds(elapsed));
    report.insert(prefix + QStringLiteral("_outputs"), double(result.savedCount));
    report.insert(prefix + QStringLiteral("_gui_heartbeat_samples"), int(gaps.size()));
    report.insert(prefix + QStringLiteral("_gui_heartbeat_p95_ms"), percentile(gaps, .95));
    report.insert(prefix + QStringLiteral("_gui_heartbeat_max_ms"),
                  gaps.empty() ? 0 : *std::max_element(gaps.begin(), gaps.end()));
    report.insert(prefix + QStringLiteral("_baseline_resident_bytes"),
                  double(baseline.residentBytes));
    report.insert(prefix + QStringLiteral("_peak_resident_bytes"), double(peak.load()));
    report.insert(prefix + QStringLiteral("_peak_resident_delta_bytes"),
                  double(peak.load() - quint64(baseline.residentBytes)));
}
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
void smartCase(QJsonObject& report, const QImage& image, ImageSplitSettings settings,
               const QString& prefix, int samples, bool verify = true) {
    QElapsedTimer firstTimer;
    firstTimer.start();
    const auto first = prepareImageSplitLayout(image, settings);
    report.insert(prefix + QStringLiteral("_first_ms"), milliseconds(firstTimer));
    require(first && first->layout.isValid(), "first Smart calculation succeeds");
    std::optional<ImageSplitLayout> reference;
    if (verify)
        reference = image_split_test::referenceSmart(image, settings);
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    std::vector<double> elapsed;
    for (int sample = 0; sample < samples; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto prepared = prepareImageSplitLayout(image, settings);
        elapsed.push_back(milliseconds(timer));
        require(prepared && prepared->layout.isValid(), "Smart benchmark returns valid geometry");
        if (reference)
            require(prepared->layout.verticalLines == reference->verticalLines &&
                        prepared->layout.horizontalLines == reference->horizontalLines,
                    "optimized Smart boundaries exactly match scalar reference");
    }
    latencyReport(report, prefix, elapsed);
    report.insert(prefix + QStringLiteral("_peak_resident_delta_bytes"),
                  double(memory.peak() - std::min(memory.peak(), quint64(baseline.residentBytes))));
}
void smartReport(QJsonObject& report) {
    const auto image = fixture({3840, 2160});
    const auto opaque = image.convertToFormat(QImage::Format_RGB32);
    ImageSplitSettings settings;
    for (const auto direction : {ImageSplitDirection::Vertical, ImageSplitDirection::Horizontal}) {
        settings.direction = direction;
        const auto axis = direction == ImageSplitDirection::Vertical ? QStringLiteral("vertical")
                                                                     : QStringLiteral("horizontal");
        for (const int count : {2, 8, 16}) {
            settings.count = count;
            smartCase(report, opaque, settings,
                      QStringLiteral("smart_4k_rgb_%1_%2").arg(axis).arg(count), 16);
            smartCase(report, image, settings,
                      QStringLiteral("smart_4k_rgba_%1_%2").arg(axis).arg(count), 16);
        }
        settings.count = 16;
        smartCase(report, image.convertToFormat(QImage::Format_RGBA64), settings,
                  QStringLiteral("smart_4k_rgba64_%1").arg(axis), 8);
    }
    ImageSplitDocument document;
    document.setImage(image, {}, settings);
    std::vector<double> cached;
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        require(document.cachedLayout(settings).has_value(), "Smart cache hit succeeds");
        cached.push_back(milliseconds(timer));
    }
    latencyReport(report, QStringLiteral("smart_cached_layout_lookup"), cached);
    cached.clear();
    auto equal = settings;
    equal.type = ImageSplitType::Equal;
    for (int sample = 0; sample < 100; ++sample) {
        QElapsedTimer timer;
        timer.start();
        require(document.setSettings(equal) && document.setSettings(settings),
                "cached mode roundtrip commits Equal and Smart without analysis");
        cached.push_back(milliseconds(timer));
    }
    latencyReport(report, QStringLiteral("smart_cached_mode_roundtrip"), cached);
    QImage large(8000, 8000, QImage::Format_RGB32);
    large.fill(QColor(32, 64, 96));
    for (int y = 0; y < large.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(large.scanLine(y));
        for (int x = 0; x < large.width(); ++x)
            if (y % 40 < 25 && x % 16 < 3)
                row[x] = qRgb(210, 220, 230);
    }
    for (const auto direction : {ImageSplitDirection::Vertical, ImageSplitDirection::Horizontal}) {
        settings.direction = direction;
        const auto axis = direction == ImageSplitDirection::Vertical ? QStringLiteral("vertical")
                                                                     : QStringLiteral("horizontal");
        for (int count : {16, 1024}) {
            settings.count = count;
            smartCase(report, large, settings,
                      QStringLiteral("smart_64mp_%1_%2").arg(axis).arg(count), 5);
        }
    }
    int checks = 0;
    QElapsedTimer cancelTimer;
    cancelTimer.start();
    require(!prepareImageSplitLayout(large, settings, [&] { return ++checks >= 8; }),
            "large Smart analysis supports cancellation inside a scan batch");
    report.insert(QStringLiteral("smart_cancel_ms"), milliseconds(cancelTimer));
    report.insert(QStringLiteral("smart_analysis_scratch_limit_bytes"),
                  double(imageSplitAnalysisMaximumWorkingBytes));
    QImage thin(1'000'000, 1, QImage::Format_RGB32);
    thin.fill(Qt::white);
    settings.direction = ImageSplitDirection::Vertical;
    settings.count = thin.width();
    smartCase(report, thin, settings, QStringLiteral("smart_million_fixed_guides"), 100, false);
    ImageSplitWindow window;
    window.presentImage(image);
    while (window.loading()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!window.document()->image().isNull(), "Smart GUI benchmark source loads");
    auto* count = window.editor()->findChild<adqt::widgets::AdInputNumber*>(
        QStringLiteral("imageSplitCount"));
    require(count, "Smart GUI benchmark finds count control");
    QElapsedTimer heartbeat;
    std::vector<double> gaps;
    QTimer pulse;
    pulse.setInterval(1);
    QObject::connect(&pulse, &QTimer::timeout, [&] {
        gaps.push_back(milliseconds(heartbeat));
        heartbeat.restart();
    });
    heartbeat.start();
    pulse.start();
    for (int sample = 0; sample < 8; ++sample) {
        const int next = 511 + sample % 2;
        count->setValue(next);
        while (window.editor()->calculating()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(window.document()->settings().count == next,
                "GUI applies the completed Smart count");
    }
    pulse.stop();
    latencyReport(report, QStringLiteral("smart_gui_heartbeat"), gaps);
    window.shutdown();
    geometryReport(report);
}
#endif

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    adqt::theme::ThemeManager::instance().applyTo(application);
#ifndef NDEBUG
    std::cerr << "Build this benchmark with the windows-msvc-performance Release preset.\n";
    return 1;
#else
    try {
#ifndef SNOW_SHOT_IMAGE_SPLIT_BASELINE
        if (application.arguments().contains(QStringLiteral("--smart-only"))) {
            QTemporaryDir storage;
            require(storage.isValid(), "Smart benchmark uses isolated storage");
            require(snow_shot::storage::ApplicationStorage::instance()
                        .initialize({storage.path(), storage.path(), 60000})
                        .success,
                    "Smart benchmark storage initializes");
            QJsonObject report;
            report.insert(QStringLiteral("build_type"), QStringLiteral("Release"));
            smartReport(report);
            snow_shot::storage::ApplicationStorage::instance().shutdown();
            std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
            return 0;
        }
#endif
        QTemporaryDir temporary;
        require(temporary.isValid(), "benchmark output directory exists");
        QJsonObject report;
        report.insert(QStringLiteral("build_type"), QStringLiteral("Release"));
#ifdef SNOW_SHOT_IMAGE_SPLIT_BASELINE
        report.insert(QStringLiteral("implementation"), QStringLiteral("captured_baseline"));
#else
        report.insert(QStringLiteral("implementation"), QStringLiteral("optimized"));
#endif
        geometryReport(report);
        historyReport(report, 1000);
        historyReport(report, 4000);
        const QImage image = fixture({8000, 8000});
        report.insert(QStringLiteral("source_pixels"),
                      double(qint64(image.width()) * image.height()));
        report.insert(QStringLiteral("source_bytes"), double(image.sizeInBytes()));
        previewReport(report, image);
        const QImage coprime(image.constBits(), image.width() - 1, image.height(),
                             image.bytesPerLine(), image.format());
        previewReport(report, coprime, QStringLiteral("preview_coprime"));
        loadReport(report, image);
        const auto preview =
            image.scaled({2048, 2048}, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        canvasReport(report, image, preview);
        hoverReport(report, image, preview);
        extremeCanvasReport(report, image);
        ImageSplitSettings settings;
        settings.type = ImageSplitType::Equal;
        exportReport(report, image, temporary.path(), QStringLiteral("large_image"), settings);
        settings.direction = ImageSplitDirection::Grid;
        settings.rows = 20;
        settings.columns = 20;
        exportReport(report, image, temporary.path(), QStringLiteral("many_regions"), settings);
        QImage profiled = image;
        profiled.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        settings.rows = 2;
        settings.columns = 2;
        exportReport(report, profiled, temporary.path(), QStringLiteral("profiled_regions"),
                     settings);
        const QImage opaque = image.convertToFormat(QImage::Format_RGB32);
        exportReport(report, opaque, temporary.path(), QStringLiteral("opaque_regions"), settings);
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
