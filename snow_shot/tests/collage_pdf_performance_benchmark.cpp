// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/collagepdfexport.h"
#include "snow_shot/presentation/components/collagepdfpagegrid.h"
#include "snow_shot/presentation/components/collagepdftemplatewidget.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/scroll_area.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryApplicationStorage final {
  public:
    explicit TemporaryApplicationStorage(QApplication& application) {
        require(m_directory.isValid() &&
                    storage::ApplicationStorage::instance()
                        .initialize({m_directory.path(), m_directory.path(), 60000})
                        .success,
                "initialize temporary application storage without touching user settings");
        snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    }
    ~TemporaryApplicationStorage() {
        ScreenshotExportCoordinator::shared().shutdown();
        storage::ApplicationStorage::instance().shutdown();
    }

  private:
    QTemporaryDir m_directory;
};

double milliseconds(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000;
}

void distribution(QJsonObject& result, const QString& name, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double fraction) {
        const auto index =
            static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(samples.size())) - 1);
        return samples[std::min(samples.size() - 1, index)];
    };
    result.insert(name + QStringLiteral("_p50_ms"), percentile(0.5));
    result.insert(name + QStringLiteral("_p95_ms"), percentile(0.95));
    result.insert(name + QStringLiteral("_samples"), static_cast<int>(samples.size()));
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_thread([this] {
              while (!m_stop.load(std::memory_order_relaxed)) {
                  const auto snapshot = snow::test_support::memorySnapshot();
                  m_resident.store(std::max(m_resident.load(), quint64(snapshot.residentBytes)));
                  m_footprint.store(std::max(m_footprint.load(), quint64(snapshot.footprintBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
          }) {}
    ~MemorySampler() {
        m_stop.store(true, std::memory_order_relaxed);
        m_thread.join();
    }
    void report(QJsonObject& result, const snow::test_support::MemorySnapshot& baseline) const {
        const auto after = snow::test_support::memorySnapshot();
        result.insert(QStringLiteral("baseline_resident_bytes"),
                      static_cast<double>(baseline.residentBytes));
        result.insert(QStringLiteral("baseline_footprint_bytes"),
                      static_cast<double>(baseline.footprintBytes));
        result.insert(QStringLiteral("peak_resident_delta_bytes"),
                      static_cast<double>(std::max({m_resident.load(), quint64(after.residentBytes),
                                                    quint64(baseline.residentBytes)}) -
                                          baseline.residentBytes));
        result.insert(
            QStringLiteral("peak_footprint_delta_bytes"),
            static_cast<double>(std::max({m_footprint.load(), quint64(after.footprintBytes),
                                          quint64(baseline.footprintBytes)}) -
                                baseline.footprintBytes));
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_resident{0};
    std::atomic<quint64> m_footprint{0};
    std::thread m_thread;
};

void render(QWidget& grid, QImage& frame) {
    const QSize size = grid.size();
    if (frame.size() != size)
        frame = QImage(size, QImage::Format_ARGB32_Premultiplied);
    grid.render(&frame);
}

void pump() {
    QApplication::processEvents();
    // processEvents() alone does not deliver DeferredDelete in this synchronous harness.
    // Retire recycled page cards just as the application's normal event loop does.
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QJsonObject gridScenario(int count) {
    ImageStitchingDocument document;
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    QImage source(24, 36, QImage::Format_RGBA8888);
    source.fill(Qt::blue);
    source.setColorSpace(QColorSpace::SRgb);
    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < count; ++index)
        require(document.append(source, QStringLiteral("Page %1").arg(index + 1)) != 0,
                "large PDF collection fixture is admitted");
    const double admissionMs = milliseconds(timer);
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    timer.restart();
    ImageStitchingPdfPageGrid grid(&document);
    grid.resize(1200, 780);
    grid.show();
    pump();
    QImage frame;
    render(grid, frame);
    const double openMs = milliseconds(timer);
    auto* scroll =
        grid.findChild<adqt::widgets::AdScrollArea*>(QStringLiteral("collagePdfPagesScroll"));
    require(scroll != nullptr && scroll->verticalScrollBar()->maximum() > 0,
            "large PDF collections produce a scrollable grid");
    int maximumCards = grid.property("materializedPageCount").toInt();
    qint64 maximumThumbnailBytes = grid.property("pageThumbnailBytes").toLongLong();
    std::vector<double> scrollSamples;
    std::vector<double> settingsSamples;
    std::vector<double> resizeSamples;
    for (int sample = 0; sample < 48; ++sample) {
        timer.restart();
        const qint64 maximum = scroll->verticalScrollBar()->maximum();
        scroll->verticalScrollBar()->setValue(static_cast<int>(maximum * (sample % 17) / 16));
        pump();
        render(grid, frame);
        scrollSamples.push_back(milliseconds(timer));
        maximumCards = std::max(maximumCards, grid.property("materializedPageCount").toInt());
        maximumThumbnailBytes =
            std::max(maximumThumbnailBytes, grid.property("pageThumbnailBytes").toLongLong());

        auto settings = document.pdfSettings();
        settings.orientation = sample % 2 == 0 ? ImageStitchingPdfOrientation::Portrait
                                               : ImageStitchingPdfOrientation::Landscape;
        settings.marginLeftMm = 10 + (sample % 7) * 0.25;
        timer.restart();
        document.setPdfSettings(settings);
        pump();
        render(grid, frame);
        settingsSamples.push_back(milliseconds(timer));
        maximumThumbnailBytes =
            std::max(maximumThumbnailBytes, grid.property("pageThumbnailBytes").toLongLong());

        timer.restart();
        grid.resize(sample % 2 == 0 ? 900 : 1200, 780);
        pump();
        render(grid, frame);
        resizeSamples.push_back(milliseconds(timer));
        maximumCards = std::max(maximumCards, grid.property("materializedPageCount").toInt());
        maximumThumbnailBytes =
            std::max(maximumThumbnailBytes, grid.property("pageThumbnailBytes").toLongLong());
    }
    require(maximumCards > 0 && maximumCards < 100,
            "materialized PDF cards stay bounded by the viewport, independently of page count");
    QJsonObject result{{QStringLiteral("scenario"), QStringLiteral("page_grid")},
                       {QStringLiteral("pages"), count},
                       {QStringLiteral("source_admission_ms"), admissionMs},
                       {QStringLiteral("open_and_first_paint_ms"), openMs},
                       {QStringLiteral("maximum_page_thumbnail_bytes"),
                        static_cast<double>(maximumThumbnailBytes)},
                       {QStringLiteral("maximum_materialized_pages"), maximumCards}};
    distribution(result, QStringLiteral("scroll_and_paint"), std::move(scrollSamples));
    distribution(result, QStringLiteral("paper_edit_and_paint"), std::move(settingsSamples));
    distribution(result, QStringLiteral("resize_and_paint"), std::move(resizeSamples));
    memory.report(result, baseline);
    return result;
}

QJsonObject layoutScenario() {
    ImageStitchingPdfSettings settings;
    std::vector<double> samples;
    qreal checksum = 0;
    for (int sample = 0; sample < 48; ++sample) {
        settings.orientation = sample % 2 == 0 ? ImageStitchingPdfOrientation::Portrait
                                               : ImageStitchingPdfOrientation::Landscape;
        settings.marginTopMm = 10 + (sample % 5) * 0.25;
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < 10000; ++index) {
            const auto layout = calculateImageStitchingPdfPageLayout({64, 32768 + index}, settings);
            require(layout.isValid(), "large layout measurements remain valid");
            checksum += layout.imageMillimeters.width();
        }
        samples.push_back(milliseconds(timer));
    }
    require(checksum > 0, "consume geometry to keep its measured calculations observable");
    QJsonObject result{{QStringLiteral("scenario"), QStringLiteral("page_layout")},
                       {QStringLiteral("pages_per_edit"), 10000},
                       {QStringLiteral("geometry_checksum"), checksum}};
    distribution(result, QStringLiteral("calculate_all_pages"), std::move(samples));
    return result;
}

QJsonObject templateScenario(int count) {
    ImageStitchingDocument document;
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    auto settings = document.pdfSettings();
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    settings.templateType = ImageStitchingPdfTemplateType::NineImages;
    document.setPdfSettings(settings);
    // Keep 10,000 retained preview charges below the document's 64 MiB preview limit.
    QImage source(40, 30, QImage::Format_RGBA8888);
    source.fill(Qt::blue);
    source.setColorSpace(QColorSpace::SRgb);
    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < count; ++index)
        require(document.append(source, QStringLiteral("Image %1").arg(index + 1)) != 0,
                "large template collection fixture is admitted");
    const double admissionMs = milliseconds(timer);
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    timer.restart();
    ImageStitchingPdfTemplateWidget widget(&document);
    widget.resize(1200, 780);
    widget.show();
    pump();
    QImage frame;
    render(widget, frame);
    const double openMs = milliseconds(timer);
    int maximumThumbnails = widget.property("materializedThumbnailCount").toInt();
    std::vector<double> selectionSamples;
    std::vector<double> placementSamples;
    std::vector<double> settingsSamples;
    for (int sample = 0; sample < 48; ++sample) {
        const qsizetype index = sample % 2 == 0 ? count - 1 : 0;
        const auto id = document.items().at(index).id;
        timer.restart();
        document.select(id);
        pump();
        render(widget, frame);
        selectionSamples.push_back(milliseconds(timer));
        const int capacity = imageStitchingPdfTemplateCapacity(document.pdfSettings().templateType);
        require(widget.canvas()->pageIndex() == index / capacity,
                "selecting a distant image resolves its template page directly");
        maximumThumbnails =
            std::max(maximumThumbnails, widget.property("materializedThumbnailCount").toInt());

        const auto page = calculateImageStitchingPdfComposedPage(
            document.items(), document.pdfSettings(), widget.canvas()->pageIndex());
        const auto& cell = page.cells.at(index % capacity);
        const qreal fillScale = cell.clipMillimeters.width() / source.width();
        ImageStitchingPdfPlacement placement;
        placement.centerOffsetMillimeters = QPointF(sample % 7 - 3, sample % 5 - 2);
        placement.millimetersPerPixel = fillScale * (.6 + (sample % 6) * .09);
        timer.restart();
        require(document.setPdfPlacement(id, placement), "template placement edit is accepted");
        pump();
        render(widget, frame);
        placementSamples.push_back(milliseconds(timer));

        settings = document.pdfSettings();
        settings.orientation = sample % 2 == 0 ? ImageStitchingPdfOrientation::Landscape
                                               : ImageStitchingPdfOrientation::Portrait;
        settings.templateType = sample % 3 == 0 ? ImageStitchingPdfTemplateType::FiveImages
                                                : ImageStitchingPdfTemplateType::NineImages;
        settings.paperSize =
            sample % 5 == 0 ? ImageStitchingPdfPaperSize::Letter : ImageStitchingPdfPaperSize::A4;
        settings.imageSpacingMm = 4 + (sample % 7) * .25;
        timer.restart();
        document.setPdfSettings(settings);
        pump();
        render(widget, frame);
        settingsSamples.push_back(milliseconds(timer));
        maximumThumbnails =
            std::max(maximumThumbnails, widget.property("materializedThumbnailCount").toInt());
    }
    require(maximumThumbnails > 0 && maximumThumbnails < 100,
            "template thumbnail widgets remain bounded by the viewport for large input counts");
    QJsonObject result{{QStringLiteral("scenario"), QStringLiteral("template_preview")},
                       {QStringLiteral("images"), count},
                       {QStringLiteral("source_admission_ms"), admissionMs},
                       {QStringLiteral("open_and_first_paint_ms"), openMs},
                       {QStringLiteral("maximum_materialized_thumbnails"), maximumThumbnails}};
    distribution(result, QStringLiteral("far_page_selection_and_paint"),
                 std::move(selectionSamples));
    distribution(result, QStringLiteral("placement_edit_and_paint"), std::move(placementSamples));
    distribution(result, QStringLiteral("layout_edit_and_paint"), std::move(settingsSamples));
    memory.report(result, baseline);
    return result;
}

QJsonObject denseTemplateExportScenario() {
    QTemporaryDir destination;
    require(destination.isValid(), "create temporary dense-template PDF output directory");
    ImageStitchingDocument document;
    auto settings = document.pdfSettings();
    settings.stitchingMethod = ImageStitchingPdfStitchingMethod::TemplateBased;
    settings.templateType = ImageStitchingPdfTemplateType::NineImages;
    document.setPdfSettings(settings);
    QImage source(1536, 1024, QImage::Format_RGBA8888);
    require(!source.isNull(), "allocate a dense source-resolution export fixture");
    source.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 17 + y) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x + y * 7) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 3 + y * 13) & 255);
            row[x * 4 + 3] = static_cast<uchar>(x % 11 == 0 ? 127 : 255);
        }
    }
    for (int index = 0; index < 18; ++index) {
        const auto id = document.append(source, QStringLiteral("Dense image %1").arg(index + 1));
        require(id != 0, "admit two full nine-image template pages");
        if (index % 3 == 1)
            require(document.rotate(id, true), "rotate dense template source");
        if (index % 3 == 2)
            require(document.flip(id, false), "flip dense template source");
        if (index % 2 == 0) {
            ImageStitchingPdfPlacement placement;
            placement.centerOffsetMillimeters = QPointF(4, -3);
            require(document.setPdfPlacement(id, placement), "offset dense template source");
        }
    }
    source = {};
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    const qint64 baselineReserved = imageStitchingResourceUsage().reservedBytes;
    qint64 peakReserved = baselineReserved;
    std::vector<double> samples;
    qint64 outputBytes = 0;
    for (int sample = 0; sample < 3; ++sample) {
        const auto path = destination.filePath(QStringLiteral("dense-%1.pdf").arg(sample));
        QElapsedTimer timer;
        timer.start();
        const auto exported = exportImageStitchingPdf(
            document.items(), settings, {path, false}, {},
            [&](const ImageStitchingPdfExportProgress&) {
                peakReserved = std::max(peakReserved, imageStitchingResourceUsage().reservedBytes);
            });
        samples.push_back(milliseconds(timer));
        require(exported.succeeded() && exported.totalCount == 2 &&
                    exported.completedPages.size() == 2 && exported.paths.size() == 1,
                "dense export counts composed pages rather than source images");
        outputBytes = QFileInfo(path).size();
        require(outputBytes > 0 && imageStitchingResourceUsage().reservedBytes == baselineReserved,
                "dense template export writes its PDF and releases scratch reservations");
    }
    QJsonObject result{{QStringLiteral("scenario"), QStringLiteral("dense_template_export")},
                       {QStringLiteral("images"), 18},
                       {QStringLiteral("pages"), 2},
                       {QStringLiteral("images_per_page"), 9},
                       {QStringLiteral("source_width"), 1536},
                       {QStringLiteral("source_height"), 1024},
                       {QStringLiteral("output_bytes"), static_cast<double>(outputBytes)},
                       {QStringLiteral("peak_reserved_delta_bytes"),
                        static_cast<double>(peakReserved - baselineReserved)}};
    distribution(result, QStringLiteral("export"), std::move(samples));
    memory.report(result, baseline);
    return result;
}

QJsonObject longImageExportScenario() {
    QTemporaryDir destination;
    require(destination.isValid(), "create temporary PDF output directory");
    ImageStitchingDocument document;
    QImage source(64, 65536, QImage::Format_RGBA8888);
    require(!source.isNull(), "allocate a four-million-pixel long-image fixture");
    source.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 17 + y) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x + y * 7) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 3 + y * 13) & 255);
            row[x * 4 + 3] = static_cast<uchar>(y % 5 == 0 ? 127 : 255);
        }
    }
    const auto first = document.append(source, QStringLiteral("long-original"));
    const auto second = document.append(source, QStringLiteral("long-rotated"));
    const auto third = document.append(source, QStringLiteral("long-flipped"));
    require(first != 0 && second != 0 && third != 0 && document.rotate(second, true) &&
                document.flip(third, false),
            "prepare original, rotated, and flipped long-page sources");
    source = {};
    const auto baseline = snow::test_support::memorySnapshot();
    MemorySampler memory;
    const qint64 baselineReserved = imageStitchingResourceUsage().reservedBytes;
    qint64 peakReserved = baselineReserved;
    int callbacks = 0;
    std::vector<double> samples;
    qint64 outputBytes = 0;
    for (int sample = 0; sample < 3; ++sample) {
        const auto path = destination.filePath(QStringLiteral("long-%1.pdf").arg(sample));
        QElapsedTimer timer;
        timer.start();
        const auto result = exportImageStitchingPdf(
            document.items(), document.pdfSettings(), {path, false}, {},
            [&](const ImageStitchingPdfExportProgress&) {
                ++callbacks;
                peakReserved = std::max(peakReserved, imageStitchingResourceUsage().reservedBytes);
            });
        samples.push_back(milliseconds(timer));
        require(result.succeeded() && result.completedPages.size() == 3 &&
                    result.paths.size() == 1 && QFileInfo(path).size() > 0,
                "long-image export completes exactly one page per source using source resolution");
        outputBytes = QFileInfo(path).size();
        require(imageStitchingResourceUsage().reservedBytes == baselineReserved,
                "completed exports release all temporary image-resource reservations");
    }
    QJsonObject result{{QStringLiteral("scenario"), QStringLiteral("long_image_export")},
                       {QStringLiteral("pages"), 3},
                       {QStringLiteral("source_width"), 64},
                       {QStringLiteral("source_height"), 65536},
                       {QStringLiteral("output_bytes"), static_cast<double>(outputBytes)},
                       {QStringLiteral("progress_callbacks"), callbacks},
                       {QStringLiteral("peak_reserved_delta_bytes"),
                        static_cast<double>(peakReserved - baselineReserved)}};
    distribution(result, QStringLiteral("export"), std::move(samples));
    memory.report(result, baseline);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        TemporaryApplicationStorage temporaryStorage(application);
        QJsonArray scenarios;
        for (const int pages : {100, 1000, 10000})
            scenarios.append(gridScenario(pages));
        for (const int images : {100, 1000, 10000})
            scenarios.append(templateScenario(images));
        scenarios.append(layoutScenario());
        scenarios.append(longImageExportScenario());
        scenarios.append(denseTemplateExportScenario());
        const QJsonObject report{{QStringLiteral("benchmark"), QStringLiteral("collage_pdf")},
                                 {QStringLiteral("scenarios"), scenarios}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
