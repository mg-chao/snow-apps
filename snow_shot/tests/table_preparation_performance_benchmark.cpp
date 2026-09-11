#include "snow_shot/network/snowshotapiclient.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
#ifndef NDEBUG
    std::cerr << "Run with windows-msvc-performance Release.\n";
    return 1;
#else
    QImage image(2880, 1620, QImage::Format_RGB32);
    quint32 seed = 12345;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            seed = seed * 1664525U + 1013904223U;
            row[x] = 0xff000000U | (seed & 0x00ffffffU);
        }
    }
    for (int sample = 0; sample < 3; ++sample) {
        QElapsedTimer elapsed;
        elapsed.start();
        const QByteArray bytes =
            SnowShotApiClient::encodeWebp(SnowShotApiClient::prepareImage(image));
        const double blockingMs = static_cast<double>(elapsed.nsecsElapsed()) / 1e6;
        if (bytes.isEmpty()) {
            return 2;
        }
        SnowShotApiClient client(QStringLiteral("http://127.0.0.1:1"));
        QEventLoop loop;
        bool finished = false;
        double heartbeatMs = -1;
        elapsed.restart();
        const auto token = client.extractTable(image, &client, [&](SnowShotTableResult) {
            finished = true;
            loop.quit();
        });
        const double submitMs = static_cast<double>(elapsed.nsecsElapsed()) / 1e6;
        QTimer::singleShot(
            0, &loop, [&]() { heartbeatMs = static_cast<double>(elapsed.nsecsElapsed()) / 1e6; });
        QTimer::singleShot(40000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!token || !finished || heartbeatMs < 0) {
            return 3;
        }
        std::cout << "sample=" << sample << " size=2880x1620 baseline_blocking_ms=" << blockingMs
                  << " async_submit_ms=" << submitMs << " ui_heartbeat_ms=" << heartbeatMs
                  << " async_total_ms=" << static_cast<double>(elapsed.nsecsElapsed()) / 1e6
                  << '\n';
    }
    return 0;
#endif
}
