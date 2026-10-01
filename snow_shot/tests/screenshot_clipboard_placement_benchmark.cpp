#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snowimageqtcodec.h"
#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSysInfo>
#include <QScopeGuard>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("clipboard benchmark validation failed");
}
ScreenshotClipboardPlacement placement(QSize size) {
    ScreenshotClipboardPlacement value;
    value.placement = {QStringLiteral("display"), QStringLiteral("serial"), QPointF(20, 30), size};
    value.windowRect = QRect(QPoint(20, 30), size);
    value.rasterSize = size;
    value.displays = {{QStringLiteral("display"), QStringLiteral("serial"), QRect(0, 0, 8000, 5000),
                       QRect(0, 0, 8000, 5000), QRect(0, 0, 8000, 4950), 1}};
    return value;
}
struct Series {
    std::vector<double> samples;
    QJsonObject report() {
        std::sort(samples.begin(), samples.end());
        const auto middle = samples.size() / 2;
        const double median =
            samples.size() % 2 ? samples[middle] : (samples[middle - 1] + samples[middle]) / 2;
        return {{"median_ms", median}, {"p95_ms", samples[(samples.size() * 95 - 1) / 100]}};
    }
};
template <class Operation> double measure(Operation operation) {
    const auto start = Clock::now();
    operation();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
qsizetype placementStorage(const ScreenshotClipboardPlacement& value) {
    // Retained capacities, excluding allocator/Qt headers and native clipboard storage.
    const auto stringBytes = [](const QString& string) {
        return string.capacity() ? (string.capacity() + 1) * qsizetype(sizeof(QChar)) : 0;
    };
    qsizetype bytes = qsizetype(sizeof(value)) +
                      value.displays.capacity() * qsizetype(sizeof(ScreenshotClipboardDisplay)) +
                      stringBytes(value.placement.displayName) +
                      stringBytes(value.placement.displaySerial) + stringBytes(value.filePath);
    for (const auto& display : value.displays)
        bytes += stringBytes(display.name) + stringBytes(display.serial);
    return bytes;
}
QJsonObject scenario(QSize size, bool native) {
    QImage pixels(size, QImage::Format_RGBA8888);
    pixels.setColorSpace(QColorSpace::SRgb);
    pixels.fill(QColor(80, 120, 200, 255));
    const auto value = placement(size);
    auto initial = ScreenshotClipboardService::prepareImage(pixels);
    require(initial.isValid());
    const auto png = initial.pngBytes();
    const auto rows = snow_shot::image_codec::srgbRowSource(pixels);
    QMimeData ordinary, located;
    ordinary.setData(QStringLiteral("image/png"), png);
    located.setData(QStringLiteral("image/png"), png);
    setScreenshotClipboardPlacement(located, value);
    Series copy[2], pin[2];
    constexpr int warmups = 5, samples = 40;
    qsizetype metadataBytes = 0;
    qsizetype pinMetadataStorage = 0;
    // Alternate paired measurements to limit cache/order bias.
    for (int i = -warmups; i < samples; ++i) {
        for (int j = 0; j < 2; ++j) {
            const int mode = (i + warmups + j) % 2;
            const double copyMs = measure([&] {
                auto payload = ScreenshotClipboardService::prepareEncoded(
                    rows, png, mode ? std::optional(value) : std::nullopt);
                require(payload.isValid() && payload.pngBytes().constData() == png.constData());
                if (mode)
                    metadataBytes = payload.placementBytes().size();
                else
                    require(payload.placementBytes().isEmpty());
                if (native)
                    require(ScreenshotClipboardService::publish(QApplication::clipboard(),
                                                                std::move(payload)));
            });
            const double pinMs = measure([&] {
                auto snapshot =
                    native
                        ? ScreenshotClipboardContentReader::snapshot(QApplication::clipboard(), 1)
                        : ScreenshotClipboardContentReader::snapshotMimeData(
                              mode ? &located : &ordinary, 1, Qt::white);
                require(snapshot.has_value());
                auto decoded = ScreenshotClipboardContentReader::decode(std::move(*snapshot));
                require(decoded && decoded->image.size() == size);
                if (mode) {
                    require(decoded->placement.has_value());
                    pinMetadataStorage = placementStorage(*decoded->placement);
                    require(resolveScreenshotClipboardPlacement(*decoded->placement, value.displays,
                                                                true)
                                .windowRect == value.windowRect);
                } else
                    require(!decoded->placement);
            });
            if (i >= 0) {
                copy[mode].samples.push_back(copyMs);
                pin[mode].samples.push_back(pinMs);
            }
        }
    }
    const auto copyBase = copy[0].report(), copyWith = copy[1].report();
    const auto pinBase = pin[0].report(), pinWith = pin[1].report();
    const auto withinBudget = [](QJsonObject base, QJsonObject with) {
        for (const auto* statistic : {"median_ms", "p95_ms"}) {
            const double baseline = base[statistic].toDouble();
            if (with[statistic].toDouble() - baseline > std::max(1.0, baseline * 0.05))
                return false;
        }
        return true;
    };
    return {{"width", size.width()},
            {"height", size.height()},
            {"samples", samples},
            {"metadata_bytes", static_cast<qint64>(metadataBytes)},
            {"estimated_pin_metadata_storage_bytes", static_cast<qint64>(pinMetadataStorage)},
            {"canonical_png_shared", true},
            {"copy_baseline", copyBase},
            {"copy_with_placement", copyWith},
            {"pin_baseline", pinBase},
            {"pin_with_placement", pinWith},
            {"within_latency_budget",
             withinBudget(copyBase, copyWith) && withinBudget(pinBase, pinWith)}};
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        const bool native = args.contains(QStringLiteral("--native"));
        auto saved = std::make_unique<QMimeData>();
        if (native) {
            ensureScreenshotClipboardPlacementMimeSupport();
            const auto* mime = QApplication::clipboard()->mimeData();
            if (mime) {
                for (const auto& format : mime->formats())
                    saved->setData(format, mime->data(format));
                if (mime->hasImage())
                    saved->setImageData(mime->imageData());
            }
        }
        const auto restore = qScopeGuard([&] {
            if (!native)
                return;
            QApplication::clipboard()->setMimeData(saved.release());
            QCoreApplication::processEvents();
#if defined(Q_OS_WIN)
            using Flush = HRESULT(WINAPI*)();
            const auto module = GetModuleHandleW(L"ole32.dll");
            const auto flush =
                module ? reinterpret_cast<Flush>(GetProcAddress(module, "OleFlushClipboard"))
                       : nullptr;
            if (flush)
                static_cast<void>(flush());
#endif
        });
        QJsonArray scenarios;
        for (const auto size :
             {QSize(320, 200), QSize(1920, 1080), QSize(3840, 2160), QSize(7680, 4320)})
            scenarios.append(scenario(size, native));
        const auto report =
            QJsonDocument(QJsonObject{{"platform", QSysInfo::prettyProductName()},
                                      {"native_clipboard", native},
                                      {"memory_estimate_scope",
                                       "value and retained capacities; excludes allocator, Qt "
                                       "headers, native clipboard storage"},
                                      {"scenarios", scenarios}})
                .toJson();
        if (args.size() > 1 && args.last() != QStringLiteral("--native")) {
            QFile file(args.last());
            require(file.open(QIODevice::WriteOnly) && file.write(report) == report.size());
        } else
            std::cout << report.constData();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
