// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotclipboardformularenderer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QTimer>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Operation> void measure(const QString& label, int count, Operation operation) {
    std::vector<qint64> durations;
    durations.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        QElapsedTimer timer;
        timer.start();
        operation();
        durations.push_back(timer.nsecsElapsed());
    }
    std::sort(durations.begin(), durations.end());
    std::cout << label.toStdString() << " samples=" << count
              << " median_ms=" << static_cast<double>(durations[durations.size() / 2]) / 1e6
              << " p95_ms=" << static_cast<double>(durations[durations.size() * 95 / 100]) / 1e6
              << '\n';
}

QImage convert(ScreenshotClipboardFormulaRenderer& renderer,
               ScreenshotClipboardFormulaRenderer::PreparedContent content) {
    QEventLoop loop;
    bool completed = false;
    std::optional<ScreenshotClipboardContent> result;
    renderer.convert(std::move(content), 2,
                     [&](std::optional<ScreenshotClipboardContent> rendered) {
                         result = std::move(rendered);
                         completed = true;
                         loop.quit();
                     });
    if (!completed) {
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    require(completed && result && result->isValid() && !result->isFormattedText(),
            "benchmark formula must render successfully");
    return std::move(result->image);
}

void comparePipelines(const QString& name, int terms, const QString& fontFamily) {
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.text = QStringLiteral("$") +
                    QStringLiteral("\\frac{a+b}{\\sqrt{x^2+1}}+").repeated(terms) +
                    QStringLiteral("z$");
    snapshot.textStyle.emplace();
    snapshot.textStyle->fontFamily = fontFamily;
    snapshot.devicePixelRatio = 2;
    ScreenshotClipboardFormulaRenderer renderer;
    QObject::connect(
        renderer.findChild<ScreenshotLatexRenderer*>(), &ScreenshotLatexRenderer::rendered,
        &renderer, [](quint64, const QImage&, ScreenshotLatexRenderer::Error error) {
            if (error != ScreenshotLatexRenderer::Error::None)
                std::cerr << "formula render error=" << static_cast<int>(error) << '\n';
        });
    const auto eager = [&] {
        auto content = ScreenshotClipboardContentReader::decode(snapshot);
        require(content.has_value(), "eager baseline must decode its text raster");
        return convert(renderer, std::move(*content));
    };
    const auto deferred = [&] {
        auto content = ScreenshotClipboardFormulaRenderer::prepare(snapshot);
        require(content &&
                    std::holds_alternative<ScreenshotClipboardFormulaRenderer::DeferredFormula>(
                        *content),
                "optimized path must defer its text raster");
        return convert(renderer, std::move(*content));
    };
    const QImage expected = eager();
    require(deferred() == expected, "optimized formula pixels must match the eager baseline");
    const auto text = ScreenshotClipboardContentReader::decode(snapshot);
    require(text.has_value(), "text raster byte count must be available");
    std::cout << name.toStdString() << " source_units=" << snapshot.text.size()
              << " avoided_text_raster_bytes=" << text->image.sizeInBytes()
              << " formula_width=" << expected.width() << " formula_height=" << expected.height()
              << " formula_raster_bytes=" << expected.sizeInBytes() << '\n';
    // Both paths use the same real decoder/renderer and empty per-request raster cache.
    // Timings include conversion and event delivery, excluding export-queue admission.
    measure(name + QStringLiteral("_eager"), 40, eager);
    measure(name + QStringLiteral("_deferred"), 40, deferred);
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        const int font = QFontDatabase::addApplicationFont(
            QStringLiteral(":/recording-test-fonts/SnowRecordingTestSans-Regular.ttf"));
        require(font >= 0, "benchmark needs a deterministic text font");
        const QString family = QFontDatabase::applicationFontFamilies(font).first();
        QGuiApplication::setFont(QFont(family));
        comparePipelines(QStringLiteral("short_formula"), 1, family);
        comparePipelines(QStringLiteral("long_formula"), 20, family);

        const QString padded =
            QLatin1Char(' ') + QString(16 * 1024 * 1024, QLatin1Char('x')) + QLatin1Char(' ');
        const QString whitespace(16 * 1024 * 1024, QLatin1Char(' '));
        qsizetype accepted = 0;
        for (const auto& input : {padded, whitespace}) {
            measure(input == padded ? QStringLiteral("padded_16M_chars")
                                    : QStringLiteral("whitespace_16M_chars"),
                    100, [&] {
                        if (auto source = ScreenshotClipboardFormulaRenderer::formulaSource(input))
                            accepted += source->size();
                    });
        }
        require(accepted == 0, "oversized detector inputs must be rejected");
        ScreenshotExportCoordinator::shared().shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
