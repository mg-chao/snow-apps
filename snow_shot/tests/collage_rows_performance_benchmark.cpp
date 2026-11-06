// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagedocument.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/memorysnapshot.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace snow_shot::presentation;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QJsonObject measure(int count, QSize size, bool transformed, int batch) {
    ImageStitchingDocument document;
    for (int i = 0; i < count; ++i) {
        auto image = snowCanvasAllocateImage(size, QImage::Format_RGB32);
        require(!image.isNull(), "source allocation failed");
        image.fill(QColor((i * 13) % 256, (i * 29) % 256, (i * 47) % 256));
        const auto id = document.append(std::move(image), QString::number(i));
        require(id != 0, "source admission failed");
        if (transformed) {
            require(document.rotate(id, true), "rotation failed");
            require(document.flip(id, true), "reflection failed");
        }
    }
    auto settings = document.settings();
    settings.sizing = ImageStitchingSizing::Original;
    document.setSettings(settings);
    const auto baseline = imageStitchingResourceUsage().reservedBytes;
    QVector<double> preparation;
    QVector<double> composition;
    qint64 retained = 0;
    quint64 checksum = 0;
    for (int iteration = 0; iteration < 5; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        QString error;
        auto rows = prepareImageStitchingRows(document.items(), settings, {}, &error);
        require(rows.isValid(), qPrintable(error));
        preparation.append(double(timer.nsecsElapsed()) / 1'000'000.0);
        retained = std::max(retained, imageStitchingResourceUsage().reservedBytes - baseline);
        const qsizetype stride = qsizetype(rows.size.width()) * 4;
        QByteArray storage(stride * batch, Qt::Uninitialized);
        timer.restart();
        quint64 sample = 0;
        for (int first = 0; first < rows.size.height(); first += batch) {
            const int countRows = std::min(batch, rows.size.height() - first);
            require(rows.readRows(first, countRows, stride,
                                  reinterpret_cast<uchar*>(storage.data()), storage.size()),
                    "row production failed");
            sample = sample * 131 + static_cast<uchar>(storage[0]) +
                     static_cast<uchar>(storage[(countRows - 1) * stride]);
        }
        composition.append(double(timer.nsecsElapsed()) / 1'000'000.0);
        require(iteration == 0 || sample == checksum, "row checksum changed between samples");
        checksum = sample;
    }
    std::sort(preparation.begin(), preparation.end());
    std::sort(composition.begin(), composition.end());
    const auto memory = snow::test_support::memorySnapshot();
    return {{QStringLiteral("images"), count},
            {QStringLiteral("source_width"), size.width()},
            {QStringLiteral("source_height"), size.height()},
            {QStringLiteral("transformed"), transformed},
            {QStringLiteral("row_batch"), batch},
            {QStringLiteral("samples"), 5},
            {QStringLiteral("prepare_median_ms"), preparation[2]},
            {QStringLiteral("compose_median_ms"), composition[2]},
            {QStringLiteral("retained_export_reservation_bytes"), double(retained)},
            {QStringLiteral("resident_bytes"), double(memory.residentBytes)},
            {QStringLiteral("checksum"), QString::number(checksum)}};
}
} // namespace

int main(int argc, char** argv) {
#if defined(_DEBUG)
    std::cerr << "Run this benchmark in Release mode\n";
    return 2;
#endif
    QCoreApplication application(argc, argv);
    try {
        QJsonArray results;
        for (const int count : {1000, 10000})
            for (const int batch : {1, 64})
                results.append(measure(count, {32, 1}, false, batch));
        for (const int batch : {1, 64})
            results.append(measure(12, {2048, 1024}, true, batch));
        const auto report = QJsonDocument(results).toJson(QJsonDocument::Indented);
        std::cout << report.constData();
        const auto arguments = application.arguments();
        const auto option = arguments.indexOf(QStringLiteral("--output"));
        if (option >= 0 && option + 1 < arguments.size()) {
            QFile file(arguments[option + 1]);
            require(file.open(QIODevice::WriteOnly) && file.write(report) == report.size(),
                    "report write failed");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
