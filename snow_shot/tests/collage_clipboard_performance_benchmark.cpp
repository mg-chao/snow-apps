// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snowimageqtcodec.h"
#include "../../test-support/memorysnapshot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
struct ScreenshotClipboardPayloadTestAccess {
    static QByteArray dibChecksum(const ScreenshotClipboardPayload& payload) {
        auto handle = static_cast<HGLOBAL>(payload.m_dibHandle);
        auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(handle));
        if (!header)
            return {};
        const auto bytes = qsizetype(sizeof(*header)) + header->biSizeImage;
        const auto checksum =
            QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(header), bytes),
                                     QCryptographicHash::Sha256);
        GlobalUnlock(handle);
        return checksum.toHex();
    }
};
#endif

namespace {
using namespace snow_shot::presentation;

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_thread([this] {
              while (!m_stop.load()) {
                  const auto memory = snow::test_support::memorySnapshot();
                  m_footprint.store(std::max(m_footprint.load(), quint64(memory.footprintBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
          }) {}
    ~MemorySampler() {
        stop();
    }
    void stop() {
        if (!m_thread.joinable())
            return;
        // Finish sampling and capture the retained result before checksum verification begins.
        m_stop.store(true);
        m_thread.join();
        const auto memory = snow::test_support::memorySnapshot();
        m_footprint.store(std::max(m_footprint.load(), quint64(memory.footprintBytes)));
    }
    quint64 footprint() const {
        return m_footprint.load();
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_footprint{0};
    std::thread m_thread;
};

QJsonObject measure(bool transformed, bool legacy) {
    ImageStitchingDocument document;
    for (int index = 0; index < 12; ++index) {
        const QSize size = index == 0 ? QSize(1024, 768) : QSize(1536, 1024);
        auto image = snowCanvasAllocateImage(size, QImage::Format_RGBA8888);
        require(!image.isNull(), "clipboard fixture allocation");
        for (int y = 0; y < image.height(); ++y) {
            auto* pixels = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                pixels[x * 4] = uchar((x * 13 + y * 7 + index * 11) % 256);
                pixels[x * 4 + 1] = uchar((x * 5 + y * 17 + index * 7) % 256);
                pixels[x * 4 + 2] = uchar((x * 23 + y * 3 + index * 13) % 256);
                pixels[x * 4 + 3] = uchar((x + y) % 3 == 0 ? 127 : 255);
            }
        }
        const auto id = document.append(std::move(image), QString::number(index));
        require(id != 0, "clipboard fixture admission");
        if (transformed) {
            if (index % 2 == 0)
                require(document.rotate(id, true), "clipboard fixture rotation");
            require(document.flip(id, true), "clipboard fixture reflection");
        }
    }
    auto settings = document.settings();
    settings.sizing = transformed ? ImageStitchingSizing::Uniform : ImageStitchingSizing::Original;
    settings.background = Qt::transparent;
    QVector<double> total;
    QVector<double> preparation;
    QVector<double> copy;
    qint64 readRows = 0;
    qint64 readCalls = 0;
    qint64 pngBytes = 0;
    QByteArray checksum;
    QByteArray dibChecksum;
    QSize output;
    const auto baseline = snow::test_support::memorySnapshot();
    quint64 peak = baseline.footprintBytes;
    for (int sample = 0; sample < 3; ++sample) {
        MemorySampler memory;
        QElapsedTimer timer;
        timer.start();
        QString error;
        auto rows = prepareImageStitchingRows(document.items(), settings, {}, &error);
        require(rows.isValid(), qPrintable(error));
        const double prepared = double(timer.nsecsElapsed()) / 1'000'000;
        readRows = 0;
        readCalls = 0;
        const auto original = rows.readRows;
        rows.readRows = [original, &readRows, &readCalls](int first, int count, qsizetype stride,
                                                          uchar* destination, qsizetype capacity) {
            readRows += count;
            ++readCalls;
            return original(first, count, stride, destination, capacity);
        };
        ScreenshotClipboardPayload payload;
        if (legacy) {
            // Baseline-compatible reproduction of the previous PNG-then-native preparation.
            const auto png = snow_shot::image_codec::encodePng(rows, 0);
            payload = ScreenshotClipboardService::prepareEncoded(rows, png);
        } else {
            payload = ScreenshotClipboardService::prepare(
                rows, ScreenshotImageEncodingOptions{100, ScreenshotCompressionLevel::Low});
        }
        require(payload.isValid(), "clipboard payload preparation");
        const double elapsed = double(timer.nsecsElapsed()) / 1'000'000;
        memory.stop();
        peak = std::max(peak, memory.footprint());
        output = rows.size;
        pngBytes = payload.pngBytes().size();
        preparation.append(prepared);
        copy.append(elapsed - prepared);
        total.append(elapsed);
        const auto encoded =
            QCryptographicHash::hash(payload.pngBytes(), QCryptographicHash::Sha256).toHex();
        require(checksum.isEmpty() || checksum == encoded,
                "clipboard PNG checksum changes across samples");
        checksum = encoded;
#if defined(Q_OS_WIN)
        const auto dib = ScreenshotClipboardPayloadTestAccess::dibChecksum(payload);
        require(!dib.isEmpty() && (dibChecksum.isEmpty() || dibChecksum == dib),
                "clipboard native bitmap checksum changes across samples");
        dibChecksum = dib;
        require(readRows == qint64(rows.size.height()) * (legacy ? 2 : 1),
                "clipboard benchmark must read exactly one fused or two legacy source passes");
#else
        require(readRows == rows.size.height(), "portable clipboard consumes one PNG source pass");
#endif
    }
    std::sort(total.begin(), total.end());
    std::sort(preparation.begin(), preparation.end());
    std::sort(copy.begin(), copy.end());
    return {{QStringLiteral("transformed_and_uniform_resized"), transformed},
            {QStringLiteral("legacy_two_pass"), legacy},
            {QStringLiteral("samples"), 3},
            {QStringLiteral("prepare_median_ms"), preparation[1]},
            {QStringLiteral("copy_median_ms"), copy[1]},
            {QStringLiteral("prepare_and_copy_median_ms"), total[1]},
            {QStringLiteral("output_width"), output.width()},
            {QStringLiteral("output_height"), output.height()},
            {QStringLiteral("source_rows_read"), double(readRows)},
            {QStringLiteral("source_read_calls"), double(readCalls)},
            {QStringLiteral("png_bytes"), double(pngBytes)},
            {QStringLiteral("png_sha256"), QString::fromLatin1(checksum)},
            {QStringLiteral("dib_sha256"), QString::fromLatin1(dibChecksum)},
            {QStringLiteral("peak_footprint_delta_bytes"), double(peak - baseline.footprintBytes)}};
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        QJsonArray scenarios;
        const auto arguments = application.arguments();
        const auto selected = arguments.indexOf(QStringLiteral("--case"));
        const auto mode = arguments.indexOf(QStringLiteral("--mode"));
        if (selected >= 0 && selected + 1 < arguments.size() && mode >= 0 &&
            mode + 1 < arguments.size()) {
            const auto result = measure(arguments[selected + 1] == QStringLiteral("transformed"),
                                        arguments[mode + 1] == QStringLiteral("legacy"));
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData();
            return 0;
        }
        for (const bool transformed : {false, true}) {
            QByteArray png;
            QByteArray dib;
            for (const bool legacy : {true, false}) {
                // Each measurement gets a fresh process so previous Qt/native allocator peaks
                // and retained source/encoder pages cannot bias the next case's memory baseline.
                QProcess child;
                child.start(
                    application.applicationFilePath(),
                    {QStringLiteral("--case"),
                     transformed ? QStringLiteral("transformed") : QStringLiteral("original"),
                     QStringLiteral("--mode"),
                     legacy ? QStringLiteral("legacy") : QStringLiteral("fused")});
                require(child.waitForStarted() && child.waitForFinished(120000) &&
                            child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                        qPrintable(QString::fromUtf8(child.readAllStandardError())));
                const auto result = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
                require(!result.isEmpty(), "clipboard child benchmark report");
                const auto encoded =
                    result.value(QStringLiteral("png_sha256")).toString().toLatin1();
                const auto native =
                    result.value(QStringLiteral("dib_sha256")).toString().toLatin1();
                require((png.isEmpty() || png == encoded) && (dib.isEmpty() || dib == native),
                        "legacy and fused clipboard paths must preserve exact PNG and DIB bytes");
                png = encoded;
                dib = native;
                scenarios.append(result);
            }
        }
        const auto report = QJsonDocument(scenarios).toJson(QJsonDocument::Indented);
        std::cout << report.constData();
        const auto option = arguments.indexOf(QStringLiteral("--output"));
        if (option >= 0 && option + 1 < arguments.size()) {
            QFile file(arguments[option + 1]);
            require(file.open(QIODevice::WriteOnly) && file.write(report) == report.size(),
                    "clipboard benchmark report write");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
