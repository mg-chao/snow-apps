// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QThread>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

double milliseconds(const QElapsedTimer& timer) {
    return double(timer.nsecsElapsed()) / 1000000;
}

void latency(QJsonObject& report, const QString& name, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double fraction) {
        return samples[std::min(samples.size() - 1,
                                size_t(std::ceil(fraction * double(samples.size()))) - 1)];
    };
    report.insert(name + QStringLiteral("_p50_ms"), percentile(.5));
    report.insert(name + QStringLiteral("_p95_ms"), percentile(.95));
    report.insert(name + QStringLiteral("_samples"), int(samples.size()));
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_thread([this] {
              while (!m_stop.load()) {
                  const auto memory = snow::test_support::memorySnapshot();
                  m_resident.store(std::max(m_resident.load(), quint64(memory.residentBytes)));
                  m_footprint.store(std::max(m_footprint.load(), quint64(memory.footprintBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
          }) {}
    ~MemorySampler() {
        m_stop.store(true);
        m_thread.join();
    }
    void report(QJsonObject& result, const snow::test_support::MemorySnapshot& baseline) const {
        const auto after = snow::test_support::memorySnapshot();
        result.insert(QStringLiteral("baseline_resident_bytes"), double(baseline.residentBytes));
        result.insert(QStringLiteral("baseline_footprint_bytes"), double(baseline.footprintBytes));
        result.insert(QStringLiteral("peak_resident_delta_bytes"),
                      double(std::max({m_resident.load(), quint64(after.residentBytes),
                                       quint64(baseline.residentBytes)}) -
                             baseline.residentBytes));
        result.insert(QStringLiteral("peak_footprint_delta_bytes"),
                      double(std::max({m_footprint.load(), quint64(after.footprintBytes),
                                       quint64(baseline.footprintBytes)}) -
                             baseline.footprintBytes));
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_resident{0};
    std::atomic<quint64> m_footprint{0};
    std::thread m_thread;
};

void render(ImageStitchingPreviewCanvas* canvas, QImage& frame) {
    const qreal ratio = canvas->devicePixelRatioF();
    const QSize size(qCeil(canvas->width() * ratio), qCeil(canvas->height() * ratio));
    if (frame.size() != size || frame.devicePixelRatio() != ratio) {
        frame = QImage(size, QImage::Format_ARGB32_Premultiplied);
        frame.setDevicePixelRatio(ratio);
    }
    canvas->render(&frame);
}

void drain() {
    QElapsedTimer timer;
    timer.start();
    while (ScreenshotExportCoordinator::shared().pendingJobCount() != 0 &&
           timer.elapsed() < 30000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(ScreenshotExportCoordinator::shared().pendingJobCount() == 0,
            "collage detail workers must retire");
    QApplication::processEvents(QEventLoop::AllEvents);
}

QJsonObject tinyItems(int count) {
    ImageStitchingDocument document;
    QImage source(32, 32, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    source.setColorSpace(QColorSpace::SRgb);
    for (int index = 0; index < count; ++index)
        require(document.append(source, QString::number(index)) != 0, "tiny fixture admission");
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1200, 700);
    editor.show();
    QApplication::processEvents(QEventLoop::AllEvents);
    editor.canvas()->fitToCanvas();
    QImage frame;
    render(editor.canvas(), frame);
    QJsonObject result;
    result.insert(QStringLiteral("scenario"), QStringLiteral("tiny_items"));
    result.insert(QStringLiteral("images"), count);
    std::vector<double> repaint;
    std::vector<double> background;
    std::vector<double> lookup;
    for (int sample = 0; sample < 96; ++sample) {
        QElapsedTimer timer;
        timer.start();
        render(editor.canvas(), frame);
        repaint.push_back(milliseconds(timer));
        timer.restart();
        qreal sum = 0;
        for (const auto& item : document.items())
            sum += editor.canvas()->imageViewportRect(item.id).height();
        require(sum > 0, "all tiny viewport image bounds remain addressable");
        lookup.push_back(milliseconds(timer));
        auto settings = document.settings();
        settings.background = sample % 2 == 0 ? QColor(Qt::blue) : QColor(Qt::white);
        timer.restart();
        document.setSettings(settings);
        QApplication::processEvents(QEventLoop::AllEvents);
        render(editor.canvas(), frame);
        background.push_back(milliseconds(timer));
    }
    latency(result, QStringLiteral("repaint"), std::move(repaint));
    latency(result, QStringLiteral("background_edit_and_repaint"), std::move(background));
    latency(result, QStringLiteral("all_image_bounds"), std::move(lookup));
    drain();
    return result;
}

QJsonObject highDepthDetail() {
    ImageStitchingDocument document;
    QImage source(6000, 6000, QImage::Format_RGBA64_Premultiplied);
    require(!source.isNull(), "high-depth fixture allocation");
    source.fill(QColor(180, 70, 20, 128));
    source.setColorSpace(QColorSpace::DisplayP3);
    const auto key = source.cacheKey();
    require(document.append(source, QStringLiteral("large ICC detail")) != 0,
            "high-depth fixture admission");
    QJsonObject result;
    result.insert(QStringLiteral("scenario"), QStringLiteral("high_depth_icc_detail"));
    result.insert(QStringLiteral("source_bytes"), double(source.sizeInBytes()));
    {
        ImageStitchingEditorWidget editor(&document);
        editor.resize(1200, 700);
        editor.show();
        // Capture before delivering timers: constructing the complete editor can already take
        // longer than the detail debounce interval, especially on the first Qt/theme use.
        const auto baseline = snow::test_support::memorySnapshot();
        MemorySampler memory;
        QElapsedTimer timer;
        timer.start();
        QApplication::processEvents(QEventLoop::AllEvents);
        editor.canvas()->fitToCanvas();
        QImage frame;
        render(editor.canvas(), frame);
        while (editor.canvas()->property("detailTileCount").toInt() == 0 &&
               timer.elapsed() < 30000) {
            QApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(editor.canvas()->property("detailTileCount").toInt() != 0,
                "large high-depth detail must finish");
        result.insert(QStringLiteral("first_detail_ms"), milliseconds(timer));
        result.insert(QStringLiteral("preview_bytes"),
                      double(editor.canvas()->property("detailPreviewBytes").toLongLong()));
        result.insert(QStringLiteral("working_bytes"),
                      double(editor.canvas()->property("detailWorkingBytes").toLongLong()));
        drain();
        memory.report(result, baseline);
        require(source.cacheKey() == key && document.items().front().image.cacheKey() == key,
                "high-depth detail preserves original raster ownership");
    }
    drain();
    return result;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        QJsonArray scenarios;
        const auto arguments = application.arguments();
        if (!arguments.contains(QStringLiteral("--detail-only"))) {
            scenarios.append(tinyItems(100));
            scenarios.append(tinyItems(1000));
        }
        if (!arguments.contains(QStringLiteral("--tiny-only")))
            scenarios.append(highDepthDetail());
        ScreenshotExportCoordinator::shared().shutdown();
        std::cout << QJsonDocument(scenarios).toJson(QJsonDocument::Indented).constData();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ScreenshotExportCoordinator::shared().shutdown();
        return 1;
    }
}
